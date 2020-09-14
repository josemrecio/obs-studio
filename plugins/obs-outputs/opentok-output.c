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

#define do_log(level, format, ...)                \
	blog(level, "[opentok output: '%s'] " format, \
	     obs_output_get_name(stream->output), ##__VA_ARGS__)

#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)

// TODO: josemrecio - move to header file
#define OPT_DROP_THRESHOLD "drop_threshold_ms"
#define OPT_PFRAME_DROP_THRESHOLD "pframe_drop_threshold_ms"
#define OPT_MAX_SHUTDOWN_TIME_SEC "max_shutdown_time_sec"
#define OPT_BIND_IP "bind_ip"
#define OPT_NEWSOCKETLOOP_ENABLED "new_socket_loop_enabled"
#define OPT_LOWLATENCY_ENABLED "low_latency_mode_enabled"

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
	struct opentok_output *stream = bzalloc(sizeof(struct opentok_output));
	stream->output = output;
	pthread_mutex_init(&stream->mutex, NULL);

	UNUSED_PARAMETER(settings);
	return stream;
}

static void opentok_output_destroy(void *data)
{
	struct opentok_output *stream = data;

	pthread_mutex_destroy(&stream->mutex);
	bfree(stream);
}

static bool opentok_output_start(void *data)
{
	struct opentok_output *stream = data;
	obs_data_t *settings;
	const char *path;

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
	struct opentok_output *stream = data;
	stream->stop_ts = ts / 1000;
	os_atomic_set_bool(&stream->stopping, true);
}

static void opentok_receive_video(void *data, struct video_data *frame)
{
	struct opentok_output *stream = data;
	//stream->onVideoFrame(frame);
}

static void opentok_receive_audio(void *data, struct audio_data *frame)
{
	struct opentok_output *stream = data;
	//stream->onAudioFrame(frame);
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

	obs_properties_add_text(props, "session",
				obs_module_text("OpenTokOutput.Session"),
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
	struct opentok_stream *stream = data;
	//return stream->total_bytes_sent;
	return 0;
}

static float opentok_output_congestion(void *data)
{
	struct opentok_stream *stream = data;
/*
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
	struct opentok_stream *stream = data;
	//return stream->rtmp.connect_time_ms;
	// TODO - josemrecio: take it from ???
	return 0;
}

static int opentok_output_dropped_frames(void *data)
{
	struct opentok_stream *stream = data;
	// TODO - josemrecio: take it from getStats()
	//return stream->dropped_frames;
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
