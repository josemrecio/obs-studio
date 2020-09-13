// Copyright (C) 2020 by josemrecio

#include <obs-module.h>

struct opentok {
	char *server;
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
	struct opentok *service = data;

	bfree(service->server);
	bfree(service->session);
	bfree(service->token);
	bfree(service->output);

	service->server = bstrdup(obs_data_get_string(settings, "server"));
	service->session = bstrdup(obs_data_get_string(settings, "session"));
	service->token = bstrdup(obs_data_get_string(settings, "token"));
	service->output = bstrdup("opentok_output");
}

static void opentok_destroy(void *data)
{
	struct opentok *service = data;

	bfree(service->server);
	bfree(service->session);
	bfree(service->token);
	bfree(service->output);
	bfree(service);
}

static void *opentok_create(obs_data_t *settings, obs_service_t *service)
{
	struct opentok *data = bzalloc(sizeof(struct opentok));
	opentok_update(data, settings);

	UNUSED_PARAMETER(service);
	return data;
}

static obs_properties_t *opentok_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *ppts = obs_properties_create();
	obs_property_t *p;

	obs_properties_add_text(ppts, "server", "Server Name", OBS_TEXT_DEFAULT);
	obs_properties_add_text(ppts, "session", "OpenTok Session", OBS_TEXT_DEFAULT);
	obs_properties_add_text(ppts, "token", "OpenTok Token", OBS_TEXT_DEFAULT);

	p = obs_properties_get(ppts, "server");
	obs_property_set_visible(p, true);

	p = obs_properties_get(ppts, "session");
	obs_property_set_visible(p, true);

	p = obs_properties_get(ppts, "token");
	obs_property_set_visible(p, true);

	return ppts;
}

static const char *opentok_url(void *data)
{
	struct opentok *service = data;
	return service->server;
}

static const char *opentok_key(void *data)
{
	UNUSED_PARAMETER(data);
	return "";
}

static const char *opentok_session(void *data)
{
	struct opentok *service = data;
	return service->session;
}

static const char *opentok_token(void *data)
{
	struct opentok *service = data;
	return service->token;
}

static const char *opentok_get_output_type(void *data)
{
	struct opentok *service = data;
	return service->output;
}

struct obs_service_info opentok_service = {
	.id             = "opentok",
	.get_name       = opentok_name,
	.create         = opentok_create,
	.destroy        = opentok_destroy,
	.update         = opentok_update,
	.get_properties = opentok_properties,
	.get_url        = opentok_url,
	.get_key        = opentok_key,
	.get_session    = opentok_session,
	.get_token      = opentok_token,
	.get_output_type = opentok_get_output_type
};
