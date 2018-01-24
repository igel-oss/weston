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
	GType meta_api_type;
	const GstMetaInfo *meta_info;
};

struct remoted_gstpipe {
	int readfd;
	int writefd;
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
	GstClockTime last_frame_time;

	int retry_count;
};

typedef struct remoting_gst_meta {
	GstMeta base;
	struct weston_remoting *remoting;
	struct remoted_output *output;
} RemotingGstMeta;

/* message type for pipe */
#define PIPE_MSG_GST_BUS_SYNC		1
#define PIPE_MSG_GST_BUFFER_RELEASE	2

static gboolean
remoting_gst_meta_init(GstMeta *meta, gpointer params, GstBuffer *buffer)
{
	RemotingGstMeta *rmeta = (RemotingGstMeta *)meta;

	rmeta->remoting = NULL;
	rmeta->output = NULL;

	return true;
}

static void
remoting_gst_meta_free(GstMeta *meta, GstBuffer *buffer)
{
	RemotingGstMeta *rmeta = (RemotingGstMeta *)meta;
	struct remoted_gstpipe *pipe;
	char msg = PIPE_MSG_GST_BUFFER_RELEASE;
	ssize_t ret;

	if (!rmeta->output)
		return;

	pipe = &rmeta->output->gstpipe;

	ret = write(pipe->writefd, &msg, sizeof(msg));
	if (ret != sizeof(msg))
		weston_log("ERROR: failed to write, ret=%zd, errno=%d\n",
			   ret, errno);
}

static gboolean
remoting_gst_meta_transform(GstBuffer *transbuf, GstMeta *meta,
			    GstBuffer *buffer, GQuark type, gpointer data)
{
	RemotingGstMeta *rmeta = (RemotingGstMeta *)meta;
	RemotingGstMeta *tr_meta;
	struct weston_remoting *remoting = rmeta->remoting;

	tr_meta = (RemotingGstMeta *)
		gst_buffer_add_meta(transbuf, remoting->meta_info, NULL);
	tr_meta->remoting = remoting;

	return true;
}

static int
remoting_gst_init(struct weston_remoting *remoting)
{
	GError *err = NULL;
	const gchar *tags[] = { NULL };

	gst_init(NULL, NULL);
	if (!gst_init_check(NULL, NULL, &err)) {
		weston_log("GStreamer initialization error: %s\n",
			   err->message);
		g_error_free(err);
		return -1;
	}

	remoting->allocator = gst_dmabuf_allocator_new();
	remoting->meta_api_type =
		gst_meta_api_type_register("RemotingGstMetaAPI", tags);
	remoting->meta_info =
		gst_meta_register(remoting->meta_api_type, "RemotingGstMeta",
				  sizeof(RemotingGstMeta),
				  (GstMetaInitFunction)remoting_gst_meta_init,
				  (GstMetaFreeFunction)remoting_gst_meta_free,
				  remoting_gst_meta_transform);

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
	char msg = PIPE_MSG_GST_BUS_SYNC;
	ssize_t ret;

	ret = write(pipe->writefd, &msg, sizeof(msg));
	if (ret != sizeof(msg))
		weston_log("ERROR: failed to write, ret=%zd, errno=%d\n",
			   ret, errno);

	return GST_BUS_PASS;
}

static int
remoting_gst_pipeline_init(struct remoted_output *output)
{
	char pipeline_str[1024];
	GstCaps *caps;
	GError *err = NULL;
	GstStateChangeReturn ret;
	struct weston_mode *mode = output->output->current_mode;

	snprintf(pipeline_str, sizeof(pipeline_str),
		 "rtpbin name=rtpbin "
		 "appsrc name=src ! videoconvert ! video/x-raw,format=NV12 ! "
		 "jpegenc ! rtpjpegpay ! rtpbin.send_rtp_sink_0 "
		 "rtpbin.send_rtp_src_0 ! udpsink name=sink host=%s port=%d "
		 "rtpbin.send_rtcp_src_0 ! "
		 "udpsink host=%s port=%d sync=false async=false "
		 "udpsrc port=%d ! rtpbin.recv_rtcp_sink_0",
		 output->host, output->port, output->host, output->port + 1,
		 output->port + 2);
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
				   "width", G_TYPE_INT, mode->width,
				   "height", G_TYPE_INT, mode->height,
				   "framerate", GST_TYPE_FRACTION,
						mode->refresh, 1000,
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

	output->last_frame_time = 0;
	ret = gst_element_set_state(output->pipeline, GST_STATE_PLAYING);
	if (ret == GST_STATE_CHANGE_FAILURE) {
		weston_log("Couldn't set GST_STATE_PLAYING to pipeline\n");
		goto err;
	}

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
	if (output->bus)
		gst_object_unref(GST_OBJECT(output->bus));
	gst_object_unref(GST_OBJECT(output->pipeline));
	output->pipeline = NULL;
}

static void
remoting_output_destroy(struct weston_output *output);

static void
remoting_gst_restart(void *data)
{
	struct remoted_output *output = data;

	if (remoting_gst_pipeline_init(output) < 0) {
		weston_log("gst: Could not restart pipeline!!\n");
		remoting_output_destroy(output->output);
	}
}

static void
remoting_gst_schedule_restart(struct remoted_output *output)
{
	struct wl_event_loop *loop;
	struct weston_compositor *c = output->remoting->compositor;

	loop = wl_display_get_event_loop(c->wl_display);
	wl_event_loop_add_idle(loop, remoting_gst_restart, output);
}

static void
remoting_gst_bus_message_handler(struct remoted_output *output)
{
	GstMessage *message;
	GError *error;
	gchar *debug;

	/* get message from bus queue */
	message = gst_bus_pop(output->bus);
	if (!message)
		return;

	switch (GST_MESSAGE_TYPE(message)) {
	case GST_MESSAGE_STATE_CHANGED: {
		GstState new_state;
		gst_message_parse_state_changed(message, NULL, &new_state,
						NULL);
		if (!strcmp(GST_OBJECT_NAME(message->src), "sink") &&
		    new_state == GST_STATE_PLAYING)
			output->retry_count = 0;
		break;
	}
	case GST_MESSAGE_WARNING:
		gst_message_parse_warning(message, &error, &debug);
		weston_log("gst: Warning: %s: %s\n",
			   GST_OBJECT_NAME(message->src), error->message);
		break;
	case GST_MESSAGE_ERROR:
		gst_message_parse_error(message, &error, &debug);
		weston_log("gst: Error: %s: %s\n",
			   GST_OBJECT_NAME(message->src), error->message);
		if (output->retry_count < MAX_RETRY_COUNT) {
			output->retry_count++;
			remoting_gst_pipeline_deinit(output);
			remoting_gst_schedule_restart(output);
		} else {
			remoting_output_destroy(output->output);
		}
		break;
	default:
		break;
	}
}

static void
remoting_output_finish_frame(struct remoted_output *output)
{
	const struct weston_drm_virtual_output_api *api
		= output->remoting->virtual_output_api;

	api->finish_frame(output->output);
}

static int
remoting_gstpipe_handler(int fd, uint32_t mask, void *data)
{
	ssize_t ret;
	char msg;
	struct remoted_output *output = data;

	/* recieve message */
	ret = read(fd, &msg, sizeof(msg));
	if (ret != sizeof(msg)) {
		weston_log("ERROR: failed to read, ret=%zd, errno=%d\n",
			   ret, errno);
		remoting_output_destroy(output->output);
		return 0;
	}

	switch (msg) {
	case PIPE_MSG_GST_BUS_SYNC:
		remoting_gst_bus_message_handler(output);
		break;
	case PIPE_MSG_GST_BUFFER_RELEASE:
		remoting_output_finish_frame(output);
		break;
	default:
		weston_log("Recieved unknown message! msg=%d\n", msg);
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
remoting_output_finish_frame_handler(void *data)
{
	struct remoted_output *output = data;

	remoting_output_finish_frame(output);
}

static void
remoting_output_finish_frame_schedule(struct remoted_output *output)
{
	struct wl_event_loop *loop;
	struct weston_compositor *c = output->remoting->compositor;

	loop = wl_display_get_event_loop(c->wl_display);
	wl_event_loop_add_idle(loop, remoting_output_finish_frame_handler,
			       output);
}

static void
remoting_frame_notify(struct wl_listener *listener, void *data)
{
	struct remoted_output *output;
	const struct weston_drm_virtual_output_api *api;
	struct weston_remoting *remoting;
	struct weston_mode *mode;
	int fd, stride;
	GstBuffer *buf;
	GstMemory *mem;
	gsize offset = 0;
	struct timespec current_frame_ts;
	GstClockTime ts, current_frame_time;
	RemotingGstMeta *meta;

	output = container_of(listener, struct remoted_output, frame_listener);

	api = output->remoting->virtual_output_api;
	api->get_current_dmabuf(output->output, &fd, &stride);

	if (fd < 0) {
		remoting_output_finish_frame_schedule(output);
		return;
	}

	remoting = output->remoting;
	mode = output->output->current_mode;
	buf = gst_buffer_new();
	mem = gst_dmabuf_allocator_alloc(remoting->allocator, fd,
					 stride * mode->height);
	gst_buffer_append_memory(buf, mem);
	gst_buffer_add_video_meta_full(buf,
				       GST_VIDEO_FRAME_FLAG_NONE,
				       GST_VIDEO_FORMAT_BGRx,
				       mode->width,
				       mode->height,
				       1,
				       &offset,
				       &stride);

	meta = (RemotingGstMeta *)
		gst_buffer_add_meta(buf, remoting->meta_info, NULL);
	meta->remoting = remoting;
	meta->output = output;

	weston_compositor_read_presentation_clock(remoting->compositor,
						  &current_frame_ts);
	current_frame_time = GST_TIMESPEC_TO_TIME(current_frame_ts);
	if (output->last_frame_time == 0)
		ts = 0;
	else
		ts = current_frame_time - output->last_frame_time;

	if (GST_CLOCK_TIME_IS_VALID(ts))
		GST_BUFFER_PTS(buf) = ts;
	else
		GST_BUFFER_PTS(buf) = GST_CLOCK_TIME_NONE;
	GST_BUFFER_DURATION(buf) = GST_CLOCK_TIME_NONE;
	gst_app_src_push_buffer(output->appsrc, buf);

	output->last_frame_time = current_frame_time;
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
	struct weston_mode *mode, *next;

	wl_list_for_each_safe(mode, next, &output->mode_list, link) {
		wl_list_remove(&mode->link);
		free(mode);
	}

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
