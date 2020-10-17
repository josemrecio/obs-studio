// Copyright (C) 2020 by josemrecio

#include <obs-module.h>

struct opentok_service_data {
	char *api_key;
	char *session;
	char *token;
	char *output;
};

static const char *opentok_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "OpenTok - Vonage Video API";
}

static void opentok_update(void *data, obs_data_t *settings)
{
	struct opentok_service_data *service_data = data;

	bfree(service_data->api_key);
	bfree(service_data->session);
	bfree(service_data->token);
	bfree(service_data->output);

	service_data->api_key = bstrdup(obs_data_get_string(settings, "api_key"));
	service_data->session = bstrdup(obs_data_get_string(settings, "session"));
	service_data->token = bstrdup(obs_data_get_string(settings, "token"));
	service_data->output = bstrdup("opentok_output");
}

static void opentok_destroy(void *data)
{
	struct opentok_service_data *service_data = data;

	bfree(service_data->api_key);
	bfree(service_data->session);
	bfree(service_data->token);
	bfree(service_data->output);
	bfree(service_data);
}

static void *opentok_create(obs_data_t *settings, obs_service_t *service)
{
	struct opentok_service_data *service_data = bzalloc(sizeof(struct opentok_service_data));
	opentok_update(service_data, settings);

	UNUSED_PARAMETER(service);
	return service_data;
}

static obs_properties_t *opentok_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *ppts = obs_properties_create();
	obs_property_t *p;

	obs_properties_add_text(ppts, "api_key", "OpenTok API key", OBS_TEXT_DEFAULT);
	obs_properties_add_text(ppts, "session", "OpenTok Session", OBS_TEXT_DEFAULT);
	obs_properties_add_text(ppts, "token", "OpenTok Token", OBS_TEXT_DEFAULT);

	p = obs_properties_get(ppts, "api_key");
	obs_property_set_visible(p, true);

	p = obs_properties_get(ppts, "session");
	obs_property_set_visible(p, true);

	p = obs_properties_get(ppts, "token");
	obs_property_set_visible(p, true);

	return ppts;
}

static const char *opentok_api_key(void *data)
{
	struct opentok_service_data *service_data = data;
	return service_data->api_key;
}

static const char *opentok_session(void *data)
{
	struct opentok_service_data *service_data = data;
	return service_data->session;
}

static const char *opentok_token(void *data)
{
	struct opentok_service_data *service_data = data;
	return service_data->token;
}

static const char *opentok_get_output_type(void *data)
{
	struct opentok_service_data *service_data = data;
	return service_data->output;
}

struct obs_service_info opentok_service = {
	.id             = "opentok",
	.get_name       = opentok_name,
	.create         = opentok_create,
	.destroy        = opentok_destroy,
	.update         = opentok_update,
	.get_properties = opentok_properties,
	.get_api_key    = opentok_api_key,
	.get_session    = opentok_session,
	.get_token      = opentok_token,
	.get_output_type = opentok_get_output_type
};
