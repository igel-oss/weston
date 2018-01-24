/*
 * Copyright © 2018 Renesas Electronics Corp.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors: IGEL Co., Ltd.
 */

#include "config.h"

#include <string.h>
#include <unistd.h>

#include "remoting-plugin.h"
#include "remoting-backend.h"
#include "compositor-drm.h"
#include "shared/helpers.h"

struct weston_remoting {
	struct weston_compositor *compositor;
	struct wl_list output_list;
	struct wl_listener destroy_listener;
	const struct weston_drm_virtual_output_api *virtual_output_api;
	struct remoting_backend *backend;
};

struct remoted_output {
	struct weston_output *output;
	void (*saved_destroy)(struct weston_output *output);
	int (*saved_enable)(struct weston_output *output);
	int (*saved_disable)(struct weston_output *output);

	struct weston_remoting *remoting;

	struct wl_listener frame_listener;
	struct wl_list link;

	struct remoting_backend_output *backend_output;
};

static void
remoting_output_destroy(struct weston_output *output);

static void
weston_remoting_destroy(struct wl_listener *l, void *data)
{
	struct weston_remoting *remoting =
		container_of(l, struct weston_remoting, destroy_listener);
	struct remoted_output *output, *next;

	wl_list_for_each_safe(output, next, &remoting->output_list, link)
		remoting_output_destroy(output->output);

	/* Finalize backend */
	remoting->backend->destroy(remoting->backend);

	wl_list_remove(&remoting->destroy_listener.link);
	free(remoting);
}

static struct weston_remoting *
weston_remoting_get(struct weston_compositor *compositor)
{
	struct wl_listener *listener;
	struct weston_remoting *remoting;

	listener = wl_signal_get(&compositor->destroy_signal,
				 weston_remoting_destroy);
	if (!listener)
		return NULL;

	remoting = wl_container_of(listener, remoting, destroy_listener);
	return remoting;
}

static void
remoting_frame_notify(struct wl_listener *listener, void *data)
{
	struct remoted_output *output;
	const struct weston_drm_virtual_output_api *api;
	int fd, size, stride;
	struct remoting_backend_output *backend_output;

	output = container_of(listener, struct remoted_output, frame_listener);

	api = output->remoting->virtual_output_api;
	api->get_current_dmabuf(output->output, &fd, &size, &stride);

	if (fd < 0)
		return;

	backend_output = output->backend_output;
	backend_output->frame(backend_output, fd, size, stride);
}

static struct remoted_output *
lookup_remoted_output(struct weston_output *output)
{
	struct weston_compositor *c = output->compositor;
	struct weston_remoting *remoting = weston_remoting_get(c);
	struct remoted_output *remoted_output;

	wl_list_for_each(remoted_output, &remoting->output_list, link) {
		if (remoted_output->output == output)
			return remoted_output;
	}

	weston_log("%s: %s: could not find output\n", __FILE__, __func__);
	return NULL;
}

static void
remoting_output_destroy(struct weston_output *output)
{
	struct remoted_output *remoted_output = lookup_remoted_output(output);
	struct remoting_backend_output *backend_output =
		remoted_output->backend_output;

	remoted_output->saved_destroy(output);

	if (backend_output) {
		if (backend_output->host)
			free(backend_output->host);
		backend_output->destroy(backend_output);
	}

	wl_list_remove(&remoted_output->link);
	free(remoted_output);
}

static int
remoting_output_enable(struct weston_output *output)
{
	struct remoted_output *remoted_output = lookup_remoted_output(output);
	struct remoting_backend_output *backend_output =
		remoted_output->backend_output;
	int ret;

	ret = remoted_output->saved_enable(output);
	if (ret < 0)
		return ret;

	ret = backend_output->enable(backend_output);
	if (ret < 0) {
		remoted_output->saved_disable(output);
		return -1;
	}

	remoted_output->frame_listener.notify = remoting_frame_notify;
	wl_signal_add(&output->frame_signal, &remoted_output->frame_listener);

	return 0;
}

static int
remoting_output_disable(struct weston_output *output)
{
	struct remoted_output *remoted_output = lookup_remoted_output(output);
	struct remoting_backend_output *backend_output =
		remoted_output->backend_output;

	if (backend_output)
		backend_output->disable(backend_output);

	return remoted_output->saved_disable(output);
}

static int
remoting_create_output(struct weston_compositor *c, char *name)
{
	struct weston_remoting *remoting = weston_remoting_get(c);
	struct remoted_output *output;
	const struct weston_drm_virtual_output_api *api;

	if (!name || !strlen(name))
		return -1;

	api = remoting->virtual_output_api;

	output = zalloc(sizeof *output);
	if (!output)
		return -1;

	output->backend_output =
		remoting->backend->create_output(remoting->backend);
	if (!output->backend_output)
		goto failed;

	output->output = api->virtual_create(c, name);
	if (!output->output) {
		weston_log("Can not create virtual output\n");
		goto failed;
	}

	output->saved_destroy = output->output->destroy;
	output->output->destroy = remoting_output_destroy;
	output->saved_enable = output->output->enable;
	output->output->enable = remoting_output_enable;
	output->saved_disable = output->output->disable;
	output->output->disable = remoting_output_disable;
	output->remoting = remoting;
	wl_list_insert(remoting->output_list.prev, &output->link);

	return 0;

failed:
	free(output);
	return -1;
}

static bool
remoting_is_remoted_output(struct weston_output *output)
{
	struct remoted_output *remoted_output = lookup_remoted_output(output);

	if (remoted_output)
		return true;

	return false;
}

static int
remoting_output_set_mode(struct weston_output *output, const char *modeline)
{
	struct remoted_output *remoted_output = lookup_remoted_output(output);
	struct weston_mode *mode;
	int n, width, height, refresh;

	if (!remoted_output) {
		weston_log("Output is not remoted.\n");
		return -1;
	}

	if (!modeline)
		return -1;

	n = sscanf(modeline, "%dx%d@%d", &width, &height, &refresh);
	if (n != 2 && n != 3)
		return -1;

	mode = zalloc(sizeof *mode);
	if (!mode)
		return -1;

	output->make = "Renesas";
	output->model = "Virtual Display";
	output->serial_number = "unknown";

	mode->flags = WL_OUTPUT_MODE_CURRENT;
	mode->width = width;
	mode->height = height;
	mode->refresh = (refresh ? refresh : 60) * 1000LL;

	wl_list_insert(output->mode_list.prev, &mode->link);

	output->current_mode = mode;

	remoted_output->backend_output->width = width;
	remoted_output->backend_output->height = height;
	remoted_output->backend_output->refresh = mode->refresh;

	return 0;
}

static void
remoting_output_set_gbm_format(struct weston_output *output,
			       const char *gbm_format)
{
	struct remoted_output *remoted_output = lookup_remoted_output(output);
	const struct weston_drm_virtual_output_api *api;

	if (!remoted_output)
		return;

	api = remoted_output->remoting->virtual_output_api;
	api->set_gbm_format(output, gbm_format);
}

static void
remoting_output_set_seat(struct weston_output *output, const char *seat)
{
	/* for now, nothing todo */
}

static void
remoting_set_host(struct weston_output *output, char *host)
{
	struct remoted_output *remoted_output = lookup_remoted_output(output);

	if (!remoted_output)
		return;

	if (remoted_output->backend_output->host)
		free(remoted_output->backend_output->host);
	remoted_output->backend_output->host = strdup(host);
}

static void
remoting_set_port(struct weston_output *output, int port)
{
	struct remoted_output *remoted_output = lookup_remoted_output(output);

	if (remoted_output)
		remoted_output->backend_output->port = port;
}

static const struct weston_remoting_api remoting_api = {
	remoting_create_output,
	remoting_is_remoted_output,
	remoting_output_set_mode,
	remoting_output_set_gbm_format,
	remoting_output_set_seat,
	remoting_set_host,
	remoting_set_port,
};

static int
backend_init(struct weston_remoting *r, struct weston_compositor *c)
{
	struct remoting_backend *(*init)(struct weston_compositor *c);

	init = weston_load_module("remoting-gst.so", "remoting_backend_init");
	if (!init)
		return -1;

	r->backend = init(c);
	if (!r->backend)
		return -1;

	return 0;
}

WL_EXPORT int
weston_module_init(struct weston_compositor *compositor)
{
	int ret;
	struct weston_remoting *remoting;
	const struct weston_drm_virtual_output_api *api =
		weston_drm_virtual_output_get_api(compositor);

	if (!api)
		return -1;

	remoting = zalloc(sizeof *remoting);
	if (!remoting)
		return -1;

	remoting->virtual_output_api = api;
	remoting->compositor = compositor;
	wl_list_init(&remoting->output_list);

	ret = weston_plugin_api_register(compositor, WESTON_REMOTING_API_NAME,
					 &remoting_api, sizeof(remoting_api));

	if (ret < 0) {
		weston_log("Failed to register remoting API.\n");
		goto failed;
	}

	/* Initialize backend */
	ret = backend_init(remoting, compositor);
	if (ret < 0) {
		weston_log("Failed to initialize remoting backend.\n");
		goto failed;
	}

	remoting->destroy_listener.notify = weston_remoting_destroy;
	wl_signal_add(&compositor->destroy_signal, &remoting->destroy_listener);
	return 0;

failed:
	free(remoting);
	return -1;
}
