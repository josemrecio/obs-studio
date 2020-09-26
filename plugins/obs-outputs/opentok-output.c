// Copyright (C) 2020 by josemrecio

#include <stdio.h>
#include <obs-module.h>
#include <obs-avc.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <util/threading.h>
#include <inttypes.h>
#include <opentok.h>
#include "net-if.h"

// OpenTok options
//#define OPENTOK_IP_PROXY
#define OPENTOK_CONSOLE_LOGGING

// video and audio capturer
// OpenTok and OBS formats must be in synch!
// TODO: josemrecio - test with other formats with the goal of reducing memory copy
/*
ot:
OTC_VIDEO_FRAME_FORMAT_BGRA32
OTC_VIDEO_FRAME_FORMAT_RGBA32
OTC_VIDEO_FRAME_FORMAT_UYVY
OTC_VIDEO_FRAME_FORMAT_YUY2

obs:
VIDEO_FORMAT_BGRA
VIDEO_FORMAT_RGBA
VIDEO_FORMAT_UYVY
VIDEO_FORMAT_YUY2
*/
#define OPENTOK_CAPTURER_OBS_FRAME_FORMAT VIDEO_FORMAT_NV12
const enum otc_video_frame_format otc_video_format = OTC_VIDEO_FRAME_FORMAT_NV12;
/*
#define OPENTOK_CAPTURER_OBS_FRAME_FORMAT VIDEO_FORMAT_UYVY
const enum otc_video_frame_format otc_video_format = OTC_VIDEO_FRAME_FORMAT_UYVY;
*/
#define OPENTOK_CAPTURER_WIDTH 1280
#define OPENTOK_CAPTURER_HEIGHT 720
#define OPENTOK_AUDIO_SAMPLE_RATE 48000

#define do_log(level, format, ...)                \
		blog(level, "[opentok output: '%s'] " format, \
				obs_output_get_name(opentok_output->output), ##__VA_ARGS__)

#define error(format, ...) do_log(LOG_ERROR, format, ##__VA_ARGS__)
#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG, format, ##__VA_ARGS__)

// TODO: josemrecio - move to header file
#define OPT_DROP_THRESHOLD "drop_threshold_ms"
#define OPT_PFRAME_DROP_THRESHOLD "pframe_drop_threshold_ms"
#define OPT_MAX_SHUTDOWN_TIME_SEC "max_shutdown_time_sec"
#define OPT_BIND_IP "bind_ip"
#define OPT_NEWSOCKETLOOP_ENABLED "new_socket_loop_enabled"
#define OPT_LOWLATENCY_ENABLED "low_latency_mode_enabled"

#ifdef OPENTOK_CONSOLE_LOGGING
static void on_otc_log_message(const char* message)
{
	blog(LOG_DEBUG, "OpenTok - %s", message);
}
#endif // OPENTOK_CONSOLE_LOGGING

// custom audio device
struct audio_device {
	struct otc_audio_device_callbacks audio_device_callbacks;
	// TODO - josemrecio: do we need this thread?
	//otk_thread_t capturer_thread;
	//std::atomic<bool> capturer_thread_exit;
};

static otc_bool audio_device_destroy_capturer(const otc_audio_device *audio_device,
		void *user_data) {
	UNUSED_PARAMETER(audio_device);
	struct audio_device *device = user_data;
	if (device == NULL) {
		return OTC_FALSE;
	}

	return OTC_TRUE;
}

static otc_bool audio_device_start_capturer(const otc_audio_device *audio_device,
		void *user_data) {
	UNUSED_PARAMETER(audio_device);
	struct audio_device *device = user_data;
	if (device == NULL) {
		return OTC_FALSE;
	}

	return OTC_TRUE;
}

static otc_bool audio_device_get_capture_settings(const otc_audio_device *audio_device,
		void *user_data,
		struct otc_audio_device_settings *settings) {
	UNUSED_PARAMETER(audio_device);
	UNUSED_PARAMETER(user_data);
	if (settings == NULL) {
		return OTC_FALSE;
	}
	// FIXME: josemrecio - check settings
	settings->number_of_channels = 1;
	settings->sampling_rate = OPENTOK_AUDIO_SAMPLE_RATE;
	return OTC_TRUE;
}

// custom video capturer
struct custom_video_capturer {
	const otc_video_capturer *video_capturer;
	struct otc_video_capturer_callbacks video_capturer_callbacks;
	int width;
	int height;
	enum otc_video_frame_format video_format;
	// TODO - josemrecio: do we need this thread?
	//otk_thread_t capturer_thread;
	//std::atomic<bool> capturer_thread_exit;
};

// video capturer callbacks
static otc_bool get_video_capturer_capture_settings(const otc_video_capturer *capturer,
		void *user_data,
		struct otc_video_capturer_settings *settings) {
	UNUSED_PARAMETER(capturer);
	struct custom_video_capturer *video_capturer = user_data;
	if (video_capturer == NULL) {
		return OTC_FALSE;
	}

	// FIXME: josemrecio - is this the right format and settings?
	settings->format = video_capturer->video_format;
	settings->width = video_capturer->width;
	settings->height = video_capturer->height;
	settings->fps = 30;
	settings->mirror_on_local_render = OTC_FALSE;
	settings->expected_delay = 0;

	return OTC_TRUE;
}

static otc_bool video_capturer_init(const otc_video_capturer *capturer, void *user_data) {
	UNUSED_PARAMETER(capturer);
	struct custom_video_capturer *video_capturer = user_data;
	if (video_capturer == NULL) {
		return OTC_FALSE;
	}

	video_capturer->video_capturer = capturer;

	return OTC_TRUE;
}

static otc_bool video_capturer_destroy(const otc_video_capturer *capturer, void *user_data) {
	UNUSED_PARAMETER(capturer);
	struct custom_video_capturer *video_capturer = user_data;
	if (video_capturer == NULL) {
		return OTC_FALSE;
	}
	video_capturer->video_capturer = NULL;
	return OTC_TRUE;
}

static otc_bool video_capturer_start(const otc_video_capturer *capturer, void *user_data) {
	UNUSED_PARAMETER(capturer);
	struct custom_video_capturer *video_capturer = user_data;
	if (video_capturer == NULL) {
		return OTC_FALSE;
	}
	return OTC_TRUE;
}

struct opentok_output {
	obs_output_t *output;
	struct custom_video_capturer *custom_video_capturer;
	otc_publisher *publisher;
	volatile bool active;
	volatile bool stopping;
	uint64_t stop_ts;
	bool sent_headers;
	int64_t last_packet_ts;

	pthread_mutex_t mutex;

	bool got_first_video;
	int32_t start_dts_offset;
};

// publisher callbacks
static void on_publisher_stream_created(otc_publisher *publisher,
		void *user_data,
		const otc_stream *stream) {
	UNUSED_PARAMETER(publisher);
	UNUSED_PARAMETER(user_data);
	blog(LOG_DEBUG, "%s - stream: %s", __FUNCTION__, otc_stream_get_id(stream));
}

static void on_publisher_error(otc_publisher *publisher,
		void *user_data,
		const char* error_string,
		enum otc_publisher_error_code error_code) {
	UNUSED_PARAMETER(publisher);
	UNUSED_PARAMETER(user_data);
	blog(LOG_DEBUG, "%s - error: %d - %s", __FUNCTION__, error_code, error_string);
}

// session callbacks
static void on_session_connected(otc_session *session, void *user_data)
{
	struct opentok_output *opentok_output = user_data;
	blog(LOG_DEBUG, "%s - connection: %s", __FUNCTION__, otc_connection_get_id(otc_session_get_connection(session)));
	// FIXME: josemrecio - start publisher
	//g_is_connected = true;
	if ((session != NULL) && (opentok_output->publisher != NULL)) {
		otc_status status = otc_session_publish(session, opentok_output->publisher);
		if (status == OTC_SUCCESS) {
			//g_is_publishing = true;
			obs_output_begin_data_capture(opentok_output->output, 0);
			blog(LOG_DEBUG, "%s - Publication started - publisher %p", __FUNCTION__, opentok_output->publisher);
			return;
		}
		blog(LOG_ERROR, "%s - Error starting publication: %d", __FUNCTION__, status);
		return;
	}
	blog(LOG_ERROR, "%s - Publication could not start due to null data", __FUNCTION__);
}

static void on_session_connection_created(otc_session *session,
		void *user_data,
		const otc_connection *connection)
{
	UNUSED_PARAMETER(session);
	UNUSED_PARAMETER(user_data);
	blog(LOG_DEBUG, "%s - connection: %s", __FUNCTION__, otc_connection_get_id(connection));
}

static void on_session_connection_dropped(otc_session *session,
		void *user_data,
		const otc_connection *connection)
{
	UNUSED_PARAMETER(session);
	UNUSED_PARAMETER(user_data);
	blog(LOG_DEBUG, "%s - connection: %s", __FUNCTION__, otc_connection_get_id(connection));
}

static void on_session_disconnected(otc_session *session, void *user_data) {
	UNUSED_PARAMETER(user_data);
	blog(LOG_DEBUG, "%s - connection: %s", __FUNCTION__, otc_connection_get_id(otc_session_get_connection(session)));
}

static void on_session_error(otc_session *session,
		void *user_data,
		const char *error_string,
		enum otc_session_error_code error) {
	UNUSED_PARAMETER(session);
	UNUSED_PARAMETER(user_data);
	blog(LOG_DEBUG, "%s - error: %d - %s", __FUNCTION__, error, error_string);
}

static const char *opentok_output_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("OpenTokOutput");
}

static void *opentok_output_create(obs_data_t *settings, obs_output_t *output)
{
	UNUSED_PARAMETER(settings);
	struct opentok_output *opentok_output = NULL;
	// init OpenTok SDK
	if (otc_init(NULL) != OTC_SUCCESS) {
		blog(LOG_ERROR, "Could not init OpenTok library");
		return opentok_output;
	}
#ifdef OPENTOK_CONSOLE_LOGGING
	otc_log_set_logger_callback(on_otc_log_message);
	otc_log_enable(OTC_LOG_LEVEL_DEBUG); //OTC_LOG_LEVEL_ALL);
#endif // OPENTOK_CONSOLE_LOGGING
	opentok_output = bzalloc(sizeof(struct opentok_output));
	opentok_output->output = output;
	pthread_mutex_init(&opentok_output->mutex, NULL);

	return opentok_output;
}

static void opentok_output_destroy(void *data)
{
	blog(LOG_DEBUG, "%s", __FUNCTION__);
	struct opentok_output *opentok_output = data;

	pthread_mutex_destroy(&opentok_output->mutex);
	bfree(opentok_output);
}

static bool opentok_output_start(void *data)
{
	struct opentok_output *opentok_output = data;
	// KK
	struct video_scale_info video_to = {};
	video_to.format = OPENTOK_CAPTURER_OBS_FRAME_FORMAT;
	video_to.width = OPENTOK_CAPTURER_WIDTH;
	video_to.height = OPENTOK_CAPTURER_HEIGHT;
	obs_output_set_video_conversion(opentok_output->output, &video_to);
	struct audio_convert_info audio_to = {};
	audio_to.format = AUDIO_FORMAT_16BIT;
	audio_to.speakers = SPEAKERS_MONO;
	audio_to.samples_per_sec = OPENTOK_AUDIO_SAMPLE_RATE;
	obs_output_set_audio_conversion(opentok_output->output, &audio_to);
	// KK
	obs_service_t *service = obs_output_get_service(opentok_output->output);
	{
		struct audio_device *audio_device = bzalloc(sizeof(struct audio_device));
		audio_device->audio_device_callbacks.user_data = audio_device;
		audio_device->audio_device_callbacks.destroy_capturer = audio_device_destroy_capturer;
		audio_device->audio_device_callbacks.start_capturer = audio_device_start_capturer;
		audio_device->audio_device_callbacks.get_capture_settings = audio_device_get_capture_settings;
		otc_set_audio_device(&(audio_device->audio_device_callbacks));
	}
	// OpenTok video capturer and publisher
	otc_publisher* publisher = NULL;
	{
		struct custom_video_capturer *video_capturer = bzalloc(sizeof(struct custom_video_capturer));
		video_capturer->video_capturer_callbacks.user_data = video_capturer;
		video_capturer->video_capturer_callbacks.init = video_capturer_init;
		video_capturer->video_capturer_callbacks.destroy = video_capturer_destroy;
		video_capturer->video_capturer_callbacks.start = video_capturer_start;
		video_capturer->video_capturer_callbacks.get_capture_settings = get_video_capturer_capture_settings;
		video_capturer->width = OPENTOK_CAPTURER_WIDTH;
		video_capturer->height = OPENTOK_CAPTURER_HEIGHT;
		video_capturer->video_format = otc_video_format;
		opentok_output->custom_video_capturer = video_capturer;

		struct otc_publisher_callbacks publisher_callbacks = {0};
		publisher_callbacks.user_data = NULL; // opentok_output;
		publisher_callbacks.on_stream_created = on_publisher_stream_created;
		//publisher_callbacks.on_render_frame = on_publisher_render_frame;
		//publisher_callbacks.on_stream_destroyed = on_publisher_stream_destroyed;
		publisher_callbacks.on_error = on_publisher_error;

		publisher = otc_publisher_new("OBS-studio",
				&(video_capturer->video_capturer_callbacks),
				&publisher_callbacks);
	}
	if (publisher == NULL) {
		error("Could not create OpenTok publisher successfully");
		return false;
	}
	opentok_output->publisher = publisher;
	// OpenTok session
	otc_session *session = NULL;
	{
		struct otc_session_callbacks session_callbacks = {0};
		//session_callbacks.user_data = &renderer_manager;
		session_callbacks.on_connected = on_session_connected;
		session_callbacks.on_connection_created = on_session_connection_created;
		session_callbacks.on_connection_dropped = on_session_connection_dropped;
		//session_callbacks.on_stream_received = on_session_stream_received;
		//session_callbacks.on_stream_dropped = on_session_stream_dropped;
		session_callbacks.on_disconnected = on_session_disconnected;
		session_callbacks.on_error = on_session_error;
		session_callbacks.user_data = opentok_output;
		debug("KK - service name: %s - id: %s", obs_service_get_name(service), obs_service_get_id(service));
		// TODO: josemrecio - add API KEY setting
		const char* api_key = obs_service_get_api_key(service);
		const char* session_id = obs_service_get_session(service);
		debug("KK - api_key: %s", api_key);
		debug("KK - sessionId: %s", session_id);
#ifdef OPENTOK_IP_PROXY
		// IP Proxy
		otc_session_settings *session_settings = otc_session_settings_new();
		otc_session_settings_set_proxy_url(session_settings, "https://test.ch3m4.com:8888");
		session = otc_session_new_with_settings(api_key, session_id, &session_callbacks, session_settings);
		otc_session_settings_delete(session_settings);
#else
		session = otc_session_new(api_key, session_id, &session_callbacks);
#endif
	}
	// FIXME: call otc_session_delete(session) on stop
	if (session == NULL) {
		error("Could not create OpenTok session successfully");
		return false;
	}
	const char* token = obs_service_get_token(service);
	debug("KK - token: %s", token);
	otc_session_connect(session, token);

//	if (!obs_output_can_begin_data_capture(opentok_output->output, 0))
//		return false;
//
//	opentok_output->got_first_video = false;
//	opentok_output->sent_headers = false;
//	os_atomic_set_bool(&opentok_output->stopping, false);
//
//	/* write headers and start capture */
//	os_atomic_set_bool(&opentok_output->active, true);
//	obs_output_begin_data_capture(opentok_output->output, 0);

	return true;
}

static void opentok_output_stop(void *data, uint64_t ts)
{
	struct opentok_output *opentok_output = data;
	UNUSED_PARAMETER(ts);
	blog(LOG_DEBUG, "%s", __FUNCTION__);
	// FIXME: josemrecio - call otc_session_delete(session) on stop? or on destroy?
	// FIXME: josemrecio - free memory allocated at start: audio and video capturers? anything else?
	// FIXME: josemrecio - call otc_publisher_delete(publisher) on stop? or on destroy?
	opentok_output->custom_video_capturer = NULL;
	/*
	struct opentok_output *opentok_output = data;
	opentok_output->stop_ts = ts / 1000;
	os_atomic_set_bool(&opentok_output->stopping, true);
	 */
}

static inline int generate_random_integer() {
  //srand(time(nullptr));
  return rand();
}

static void opentok_receive_video(void *data, struct video_data *video_data)
{
	UNUSED_PARAMETER(video_data);
	struct opentok_output *opentok_output = data;
	struct custom_video_capturer *video_capturer = opentok_output->custom_video_capturer;
	if (video_capturer == NULL) {
		return;
	}
//	uint8_t *buffer = malloc(sizeof(uint8_t) * video_capturer->width * video_capturer->height * 4);
//	memset(buffer, generate_random_integer() & 0xFF, video_capturer->width * video_capturer->height * 4);
//	otc_video_frame *otc_frame = otc_video_frame_new(OTC_VIDEO_FRAME_FORMAT_ARGB32, video_capturer->width, video_capturer->height, buffer);
//	otc_video_capturer_provide_frame(video_capturer->video_capturer, 0, otc_frame);
//	if (otc_frame != NULL) {
//		otc_video_frame_delete(otc_frame);
//	}
	//blog(LOG_DEBUG, "%s", __FUNCTION__);
	//otc_video_frame_new_NV12_wrapper(width, height, y_plane, y_stride, uv_plane, uv_stride)
	otc_video_frame *otc_frame = otc_video_frame_new_from_planes(
			video_capturer->video_format,
			video_capturer->width,
			video_capturer->height,
			(const uint8_t **)video_data->data,
			video_data->linesize);
	    //const uint8_t **planes, int *strides)
//	otc_video_frame *otc_frame = otc_video_frame_new(
//			video_capturer->video_format,
//			video_capturer->width,
//			video_capturer->height,
//			(const uint8_t *)video_data->data);
    otc_video_capturer_provide_frame(video_capturer->video_capturer, 0, otc_frame);
    if (otc_frame != NULL) {
      otc_video_frame_delete(otc_frame);
    }
}

static void opentok_receive_audio(void *data, struct audio_data *audio_data)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(audio_data);
	/*
	struct opentok_output *opentok_output = data;
	opentok_output->onAudioFrame(frame);
	*/
/*
  int16_t samples[480];
  static double time = 0;

  while (device->capturer_thread_exit.load() == false) {
    for (int i = 0; i < 480; i++) {
      double val = (INT16_MAX  * 0.75) * cos(2.0 * M_PI * 4.0 * time / 10.0 );
      samples[i] = (int16_t)val;
      time += 10.0 / 480.0;
    }
    otc_audio_device_write_capture_data(samples, 480);
    usleep(10 * 1000);
  }
*/
	otc_audio_device_write_capture_data(audio_data->data, audio_data->frames);
}

// TODO: josemrecio - these are taken from rtmp, check whether they make sense
static void opentok_output_defaults(obs_data_t *defaults)
{
	obs_data_set_default_int(defaults, OPT_DROP_THRESHOLD, 700);
	obs_data_set_default_int(defaults, OPT_PFRAME_DROP_THRESHOLD, 900);
	obs_data_set_default_int(defaults, OPT_MAX_SHUTDOWN_TIME_SEC, 30);
	obs_data_set_default_string(defaults, OPT_BIND_IP, "default");
	obs_data_set_default_bool(defaults, OPT_NEWSOCKETLOOP_ENABLED, false);
	obs_data_set_default_bool(defaults, OPT_LOWLATENCY_ENABLED, false);
}

// TODO: josemrecio - complete with more properties, bind_ip, low latency mode and threshold left as examples
static obs_properties_t *opentok_output_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *props = obs_properties_create();
	struct netif_saddr_data addrs = {0};
	obs_property_t *p;

	obs_properties_add_text(props, "api_key",
			obs_module_text("OpenTokOutput.APIKey"),
			OBS_TEXT_DEFAULT);

	obs_properties_add_text(props, "session",
			obs_module_text("OpenTokOutput.Session"),
			OBS_TEXT_DEFAULT);

	obs_properties_add_text(props, "token",
			obs_module_text("OpenTokOutput.Token"),
			OBS_TEXT_DEFAULT);

	obs_properties_add_int(props, OPT_DROP_THRESHOLD,
			obs_module_text("OpenTokOutput.DropThreshold"), 200,
			10000, 100);

	p = obs_properties_add_list(props, OPT_BIND_IP,
			obs_module_text("OpenTokOutput.BindIP"),
			OBS_COMBO_TYPE_LIST,
			OBS_COMBO_FORMAT_STRING);

	obs_property_list_add_string(p, obs_module_text("Default"), "default");

	netif_get_addrs(&addrs);
	for (size_t i = 0; i < addrs.addrs.num; i++) {
		struct netif_saddr_item item = addrs.addrs.array[i];
		obs_property_list_add_string(p, item.name, item.addr);
	}
	netif_saddr_data_free(&addrs);

	obs_properties_add_bool(props, OPT_LOWLATENCY_ENABLED,
			obs_module_text("OpenTokOutput.LowLatencyMode"));

	return props;
}

static uint64_t opentok_output_total_bytes_sent(void *data)
{
	UNUSED_PARAMETER(data);
	/*
	struct opentok_stream *stream = data;
	return stream->total_bytes_sent;
	 */
	return 0;
}

static float opentok_output_congestion(void *data)
{
	UNUSED_PARAMETER(data);
	/*
	struct opentok_stream *stream = data;
	if (stream->new_socket_loop)
		return (float)stream->write_buf_len /
		       (float)stream->write_buf_size;
	else
		return stream->min_priority > 0 ? 1.0f : stream->congestion;
	 */
	// TODO: josemrecio - no congestion, take it from getStats()
	return 0;
}

static int opentok_output_connect_time(void *data)
{
	UNUSED_PARAMETER(data);
	/*
	struct opentok_stream *stream = data;
	return stream->rtmp.connect_time_ms;
	 */
	// TODO - josemrecio: take it from ???
	return 0;
}

static int opentok_output_dropped_frames(void *data)
{
	UNUSED_PARAMETER(data);
	/*
	struct opentok_stream *stream = data;
	//return stream->dropped_frames;
	 */
	// TODO - josemrecio: take it from getStats()
	return 0;
}

struct obs_output_info opentok_output_info = {
		.id = "opentok_output",
		// FIXME: josemrecio - add OBS_OUTPUT_ENCODED?
		.flags = OBS_OUTPUT_AV | OBS_OUTPUT_SERVICE,
		.encoded_video_codecs = "vp8",
		.encoded_audio_codecs = "opus",
		.get_name = opentok_output_getname,
		.create = opentok_output_create,
		.destroy = opentok_output_destroy,
		.start = opentok_output_start,
		.stop = opentok_output_stop,
		.raw_video = opentok_receive_video,
		.raw_audio = opentok_receive_audio,
		.get_defaults = opentok_output_defaults,
		.get_properties = opentok_output_properties,
		.get_total_bytes = opentok_output_total_bytes_sent,
		.get_congestion = opentok_output_congestion,
		.get_connect_time_ms = opentok_output_connect_time,
		.get_dropped_frames = opentok_output_dropped_frames,
};
