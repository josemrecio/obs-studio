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
//#define OPENTOK_CONSOLE_LOGGING

#define do_log(level, format, ...)                \
		blog(level, "[opentok output: '%s'] " format, \
				obs_output_get_name(stream->output), ##__VA_ARGS__)

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

static void on_otc_log_message(const char* message)
{
	blog(LOG_DEBUG, "OpenTok - %s", message);
}

static void on_session_connected(otc_session *session, void *user_data)
{
	UNUSED_PARAMETER(user_data);
	blog(LOG_DEBUG, "%s - connection: %s", __FUNCTION__, otc_connection_get_id(otc_session_get_connection(session)));
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

static void on_publisher_stream_created(otc_publisher *publisher,
		void *user_data,
		const otc_stream *stream) {
	UNUSED_PARAMETER(publisher);
	UNUSED_PARAMETER(user_data);
	blog(LOG_DEBUG, "%s - stream: %s", __FUNCTION__, otc_stream_get_id(stream));
}

struct opentok_output {
	obs_output_t *output;
	volatile bool active;
	volatile bool stopping;
	uint64_t stop_ts;
	bool sent_headers;
	int64_t last_packet_ts;

	pthread_mutex_t mutex;

	bool got_first_video;
	int32_t start_dts_offset;
};

static const char *opentok_output_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("OpenTokOutput");
}

static void *opentok_output_create(obs_data_t *settings, obs_output_t *output)
{
	UNUSED_PARAMETER(settings);
	struct opentok_output *stream = NULL;
	// init OpenTok SDK
	if (otc_init(NULL) != OTC_SUCCESS) {
		blog(LOG_ERROR, "Could not init OpenTok library");
		return stream;
	}
#ifdef OPENTOK_CONSOLE_LOGGING
	otc_log_set_logger_callback(on_otc_log_message);
	otc_log_enable(OTC_LOG_LEVEL_ALL);
#endif
	stream = bzalloc(sizeof(struct opentok_output));
	stream->output = output;
	pthread_mutex_init(&stream->mutex, NULL);

	return stream;
}

static void opentok_output_destroy(void *data)
{
	blog(LOG_DEBUG, "%s", __FUNCTION__);
	struct opentok_output *stream = data;

	pthread_mutex_destroy(&stream->mutex);
	bfree(stream);
}

static bool opentok_output_start(void *data)
{
	struct opentok_output *stream = data;
	// OpenTok session
	struct otc_session_callbacks session_callbacks = {0};
	//session_callbacks.user_data = &renderer_manager;
	session_callbacks.on_connected = on_session_connected;
	session_callbacks.on_connection_created = on_session_connection_created;
	session_callbacks.on_connection_dropped = on_session_connection_dropped;
	//session_callbacks.on_stream_received = on_session_stream_received;
	//session_callbacks.on_stream_dropped = on_session_stream_dropped;
	//session_callbacks.on_disconnected = on_session_disconnected;
	//session_callbacks.on_error = on_session_error;
	obs_service_t *service = obs_output_get_service(stream->output);
	debug("KK - service name: %s - id: %s", obs_service_get_name(service), obs_service_get_id(service));
	// TODO: josemrecio - add API KEY setting
	const char* api_key = obs_service_get_api_key(service);
	const char* session_id = obs_service_get_session(service);
	const char* token = obs_service_get_token(service);
	debug("KK - api_key: %s", api_key);
	debug("KK - sessionId: %s", session_id);
	debug("KK - token: %s", token);
	otc_session *session = NULL;
#ifdef OPENTOK_IP_PROXY
	// IP Proxy
	otc_session_settings *session_settings = otc_session_settings_new();
	otc_session_settings_set_proxy_url(session_settings, "https://test.ch3m4.com:8888");
	session = otc_session_new_with_settings(api_key, session_id, &session_callbacks, session_settings);
	otc_session_settings_delete(session_settings);
#else
	session = otc_session_new(api_key, session_id, &session_callbacks);
#endif
	// FIXME: call otc_session_delete(session) on stop
	if (session == NULL) {
		error("Could not create OpenTok session successfully");
		return false;
	}
	otc_session_connect(session, token);

	if (!obs_output_can_begin_data_capture(stream->output, 0))
		return false;
	// FIXME: josemrecio - this is not correct for this kind of output
	if (!obs_output_initialize_encoders(stream->output, 0))
		return false;

	stream->got_first_video = false;
	stream->sent_headers = false;
	os_atomic_set_bool(&stream->stopping, false);

	/* write headers and start capture */
	os_atomic_set_bool(&stream->active, true);
	obs_output_begin_data_capture(stream->output, 0);

	return true;
}

static void opentok_output_stop(void *data, uint64_t ts)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(ts);
	blog(LOG_DEBUG, "%s", __FUNCTION__);
	// FIXME: josemrecio - call otc_session_delete(session) on stop? or on destroy?
/*
	struct opentok_output *stream = data;
	stream->stop_ts = ts / 1000;
	os_atomic_set_bool(&stream->stopping, true);
*/
}

static void opentok_receive_video(void *data, struct video_data *frame)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(frame);
/*
	struct opentok_output *stream = data;
	stream->onVideoFrame(frame);
*/
}

static void opentok_receive_audio(void *data, struct audio_data *frame)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(frame);
/*
	struct opentok_output *stream = data;
	stream->onAudioFrame(frame);
*/
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
