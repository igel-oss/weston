/*
 * Copyright © 2018 Renesas Electronics Corp.
 *
 * Based on vaapi-recorder by:
 *   Copyright (c) 2012 Intel Corporation. All Rights Reserved.
 *   Copyright © 2013 Intel Corporation
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

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>

#include <gst/gst.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/gstvideometa.h>

#include "remoting-plugin.h"
#include "compositor-drm.h"
#include "shared/helpers.h"
#include "shared/timespec-util.h"

#define MAX_RETRY_COUNT	3

struct weston_remoting {
	struct weston_compositor *compositor;
	struct wl_list output_list;
	struct wl_listener destroy_listener;
	const struct weston_drm_virtual_output_api *virtual_output_api;

	GstAllocator *allocator;
};

struct remoted_gstpipe {
	int readfd;
	int writefd;
	GstMessage *message;
	struct wl_event_source *source;
};

struct remoted_output {
	struct weston_output *output;
	void (*saved_destroy)(struct weston_output *output);
	int (*saved_enable)(struct weston_output *output);
	int (*saved_disable)(struct weston_output *output);

	char *host;
	int port;

	struct weston_remoting *remoting;

	struct wl_listener frame_listener;
	struct wl_list link;

	GstElement *pipeline;
	GstAppSrc *appsrc;
	GstBus *bus;
	struct remoted_gstpipe gstpipe;
	GstClockTime timestamp;
	struct timespec last_frame_ts;

	bool restart;
	int retry_count;
};

static int
remoting_gst_init(struct weston_remoting *remoting)
{
	GError *err = NULL;

	gst_init(NULL, NULL);
	if (!gst_init_check(NULL, NULL, &err)) {
		weston_log("GStreamer initialization error: %s\n",
			   err->message);
		g_error_free(err);
		return -1;
	}

	remoting->allocator = gst_dmabuf_allocator_new();

	return 0;
}

static void
remoting_gst_deinit(struct weston_remoting *remoting)
{
	gst_object_unref(remoting->allocator);
	gst_deinit();
}

static GstBusSyncReply
remoting_gst_bus_sync_handler(GstBus *bus, GstMessage *message,
			      gpointer user_data)
{
	struct remoted_gstpipe *pipe = user_data;
	char dummy_msg = 1;
	ssize_t ret;

	ret = write(pipe->writefd, &dummy_msg, sizeof(dummy_msg));
	if (ret != sizeof(dummy_msg))
		weston_log("ERROR: failed to write, ret=%zd, errno=%d\n",
			   ret, errno);

	return GST_BUS_PASS;
}

static int
remoting_gst_pipeline_init(struct remoted_output *output)
{
	char pipeline_str[1024];
	GstCaps *caps;
	GError *err;
	struct weston_mode *mode = output->output->current_mode;

	snprintf(pipeline_str, sizeof(pipeline_str),
		 "appsrc name=src ! videoconvert ! video/x-raw,format=NV12 ! "
		 "jpegenc ! rtpjpegpay ! udpsink host=%s port=%d",
		 output->host, output->port);
	weston_log("GST pipeline: %s\n", pipeline_str);

	output->pipeline = gst_parse_launch(pipeline_str, &err);
	if (!output->pipeline) {
		weston_log("Could not create gstreamer pipeline. Error: %s\n",
			   err->message);
		g_error_free(err);
		return -1;
	}

	output->appsrc = (GstAppSrc*)
		gst_bin_get_by_name(GST_BIN(output->pipeline), "src");
	if (!output->appsrc) {
		weston_log("Could not get appsrc from gstreamer pipeline\n");
		goto err;
	}

	caps = gst_caps_new_simple("video/x-raw",
				   "format", G_TYPE_STRING, "BGRx",
				   "width", G_TYPE_INT, output->output->width,
				   "height", G_TYPE_INT, output->output->height,
				   "framerate", GST_TYPE_FRACTION,
						mode->refresh, 1,
				   NULL);
	if (!caps) {
		weston_log("Could not create gstreamer caps.\n");
		goto err;
	}
	g_object_set(G_OBJECT(output->appsrc),
		     "caps", caps,
		     "stream-type", 0,
		     "format", GST_FORMAT_TIME,
		     "is-live", TRUE,
		     NULL);
	gst_caps_unref(caps);

	output->bus = gst_pipeline_get_bus(GST_PIPELINE(output->pipeline));
	if (!output->bus) {
		weston_log("Could not get bus from gstreamer pipeline\n");
		goto err;
	}
	gst_bus_set_sync_handler(output->bus, remoting_gst_bus_sync_handler,
				 &output->gstpipe, NULL);

	gst_element_set_state(output->pipeline, GST_STATE_PLAYING);

	return 0;

err:
	gst_object_unref(GST_OBJECT(output->pipeline));
	output->pipeline = NULL;
	return -1;
}

static void
remoting_gst_pipeline_deinit(struct remoted_output *output)
{
	if (!output->pipeline)
		return;

	gst_element_set_state(output->pipeline, GST_STATE_NULL);
	gst_object_unref(GST_OBJECT(output->pipeline));
	output->pipeline = NULL;
}

static void remoting_gst_restart(void *data);

static void
remoting_gst_schedule_restart(struct remoted_output *output)
{
	struct wl_event_loop *loop;
	struct weston_compositor *c = output->remoting->compositor;

	loop = wl_display_get_event_loop(c->wl_display);
	wl_event_loop_add_idle(loop, remoting_gst_restart, output);
}

static void
remoting_output_destroy(struct weston_output *output);

static void
remoting_gst_restart(void *data)
{
	struct remoted_output *output = data;
	GstStateChangeReturn ret;

	ret = gst_element_set_state(output->pipeline, GST_STATE_PLAYING);
	switch (ret) {
	case GST_STATE_CHANGE_FAILURE:
		if (output->retry_count < MAX_RETRY_COUNT) {
			output->retry_count++;
			remoting_gst_schedule_restart(output);
		} else {
			remoting_output_destroy(output->output);
		}
		break;
	case GST_STATE_CHANGE_ASYNC:
		output->restart = true;
		break;
	default:
		break;
	}
}


static int
remoting_gstpipe_handler(int fd, uint32_t mask, void *data)
{
	ssize_t ret;
	char dummy_msg;
	GstMessage *message;
	GError *error;
	gchar *debug;
	struct remoted_output *output = data;

	/* recieve dummy message */
	ret = read(fd, &dummy_msg, sizeof(dummy_msg));
	if (ret != sizeof(dummy_msg))
		weston_log("ERROR: failed to read, ret=%zd, errno=%d\n",
			   ret, errno);

	/* get message of from bus queue */
	message = gst_bus_pop(output->bus);
	if (!message)
		return 1;

	switch (GST_MESSAGE_TYPE(message)) {
	case GST_MESSAGE_ASYNC_DONE:
		output->restart = false;
		break;
	case GST_MESSAGE_WARNING:
		gst_message_parse_warning(message, &error, &debug);
		weston_log("gst: Warning: %s: %s\n",
			   GST_OBJECT_NAME(message->src), error->message);
		break;
	case GST_MESSAGE_ERROR:
		gst_message_parse_error(message, &error, &debug);
		weston_log("gst: Error: %s: %s\n",
			   GST_OBJECT_NAME(message->src), error->message);
		if (output->restart) {
			remoting_output_destroy(output->output);
		} else {
			gst_element_set_state(output->pipeline, GST_STATE_NULL);
			output->retry_count = 0;
			remoting_gst_schedule_restart(output);
		}
		break;
	default:
		break;
	}

	return 1;
}

static int
remoting_gstpipe_init(struct weston_compositor *c,
		      struct remoted_output *output)
{
	struct wl_event_loop *loop;
	int fd[2];

	if (pipe2(fd, O_CLOEXEC) == -1)
		return -1;

	output->gstpipe.readfd = fd[0];
	output->gstpipe.writefd = fd[1];
	loop = wl_display_get_event_loop(c->wl_display);
	output->gstpipe.source =
		wl_event_loop_add_fd(loop, output->gstpipe.readfd,
				     WL_EVENT_READABLE,
				     remoting_gstpipe_handler, output);
	if (!output->gstpipe.source) {
		close(fd[0]);
		close(fd[1]);
		return -1;
	}

	return 0;
}

static void
remoting_gstpipe_release(struct remoted_gstpipe *pipe)
{
	wl_event_source_remove(pipe->source);
	close(pipe->readfd);
	close(pipe->writefd);
}

static void
weston_remoting_destroy(struct wl_listener *l, void *data)
{
	struct weston_remoting *remoting =
		container_of(l, struct weston_remoting, destroy_listener);
	struct remoted_output *output, *next;

	wl_list_for_each_safe(output, next, &remoting->output_list, link)
		remoting_output_destroy(output->output);

	/* Finalize gstreamer */
	remoting_gst_deinit(remoting);

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
	int fd, stride;
	GstBuffer *buf;
	GstMemory *mem;
	gsize offset = 0;
	int64_t duration = 0;
	struct timespec current_frame_ts;

	output = container_of(listener, struct remoted_output, frame_listener);

	api = output->remoting->virtual_output_api;
	api->get_current_dmabuf(output->output, &fd, &stride);

	if (fd < 0)
		return;

	buf = gst_buffer_new();
	mem = gst_dmabuf_allocator_alloc(output->remoting->allocator,
					 fd, stride * output->output->height);
	gst_buffer_append_memory(buf, mem);
	gst_buffer_add_video_meta_full(buf,
				       GST_VIDEO_FRAME_FLAG_NONE,
				       GST_VIDEO_FORMAT_BGRx,
				       output->output->width,
				       output->output->height,
				       1,
				       &offset,
				       &stride);

	weston_compositor_read_presentation_clock(output->remoting->compositor,
						  &current_frame_ts);
	if (output->timestamp == 0) {
		duration =
			millihz_to_nsec(output->output->current_mode->refresh);
	} else {
		duration = timespec_sub_to_nsec(&current_frame_ts,
						&output->last_frame_ts);
	}
	GST_BUFFER_PTS(buf) = output->timestamp;
	GST_BUFFER_DURATION(buf) = duration;
	gst_app_src_push_buffer(output->appsrc, buf);

	output->timestamp += duration;
	output->last_frame_ts = current_frame_ts;
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

	remoted_output->saved_destroy(output);

	remoting_gst_pipeline_deinit(remoted_output);
	remoting_gstpipe_release(&remoted_output->gstpipe);

	if (remoted_output->host)
		free(remoted_output->host);

	wl_list_remove(&remoted_output->link);
	free(remoted_output);
}

static int
remoting_output_enable(struct weston_output *output)
{
	struct remoted_output *remoted_output = lookup_remoted_output(output);
	int ret;

	ret = remoted_output->saved_enable(output);
	if (ret < 0)
		return ret;

	ret = remoting_gst_pipeline_init(remoted_output);
	if (ret < 0) {
		remoted_output->saved_disable(output);
		return ret;
	}

	remoted_output->frame_listener.notify = remoting_frame_notify;
	wl_signal_add(&output->frame_signal, &remoted_output->frame_listener);

	return 0;
}

static int
remoting_output_disable(struct weston_output *output)
{
	struct remoted_output *remoted_output = lookup_remoted_output(output);

	remoting_gst_pipeline_deinit(remoted_output);

	wl_list_remove(&remoted_output->frame_listener.link);

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

	if (remoting_gstpipe_init(c, output) < 0) {
		weston_log("Can not create pipe for gstreamer\n");
		goto err;
	}

	output->output = api->virtual_create(c, name);
	if (!output->output) {
		weston_log("Can not create virtual output\n");
		goto err;
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

err:
	if (output->gstpipe.source)
		remoting_gstpipe_release(&output->gstpipe);
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
	struct weston_mode *mode;
	int n, width, height, refresh;

	if (!remoting_is_remoted_output(output)) {
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

	if (remoted_output->host)
		free(remoted_output->host);
	remoted_output->host = strdup(host);
}

static void
remoting_set_port(struct weston_output *output, int port)
{
	struct remoted_output *remoted_output = lookup_remoted_output(output);

	if (remoted_output)
		remoted_output->port = port;
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

	/* Initialize gstreamer */
	ret = remoting_gst_init(remoting);
	if (ret < 0) {
		weston_log("Failed to initialize gstreamer.\n");
		goto failed;
	}

	remoting->destroy_listener.notify = weston_remoting_destroy;
	wl_signal_add(&compositor->destroy_signal, &remoting->destroy_listener);
	return 0;

failed:
	free(remoting);
	return -1;
}
