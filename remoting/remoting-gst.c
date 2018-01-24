/*
 * Copyright © 2018 Renesas Electronics Corp.
 *
 * Based on vaapi-recorder by:
 * Copyright (c) 2012 Intel Corporation. All Rights Reserved.
 * Copyright © 2013 Intel Corporation
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

#include <gst/gst.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/gstvideometa.h>

#include "remoting-backend.h"
#include "shared/helpers.h"
#include "shared/timespec-util.h"

struct gst_backend {
	struct remoting_backend base;

	GMainContext *context;
	GstAllocator *allocator;
};

struct gst_output {
	struct remoting_backend_output base;

	int frame_count;

	int destroying;
	pthread_t worker_thread;
	pthread_mutex_t mutex;
	pthread_cond_t input_cond;

	struct {
		int valid;
		int prime_fd, stride, size;
	} input;

	GstElement *pipeline;
	GstAppSrc *src;
	GstClockTime timestamp;
	struct timespec last_frame_ts;
};

static inline struct gst_backend *
to_gst_backend(struct remoting_backend *base)
{
	return container_of(base, struct gst_backend, base);
}

static inline struct gst_output *
to_gst_output(struct remoting_backend_output *base)
{
	return container_of(base, struct gst_output, base);
}

static void
remoting_frame(struct gst_output *output)
{
	GstBuffer *buf;
	GstMemory *mem;
	gsize offset = 0;
	int64_t duration = 0;
	struct timespec current_frame_ts;
	struct gst_backend *b = to_gst_backend(output->base.backend);

	buf = gst_buffer_new();
	mem = gst_dmabuf_allocator_alloc(b->allocator,
					 output->input.prime_fd,
					 output->input.size);
	gst_buffer_append_memory(buf, mem);
	gst_buffer_add_video_meta_full(buf,
				       GST_VIDEO_FRAME_FLAG_NONE,
				       GST_VIDEO_FORMAT_RGBx,
				       output->base.width,
				       output->base.height,
				       1,
				       &offset,
				       &output->input.stride);

	weston_compositor_read_presentation_clock(b->base.compositor,
						  &current_frame_ts);
	if (output->timestamp == 0)
		duration = millihz_to_nsec(output->base.refresh);
	else
		duration = timespec_sub_to_nsec(&current_frame_ts,
						&output->last_frame_ts);

	GST_BUFFER_PTS(buf) = output->timestamp;
	GST_BUFFER_DURATION(buf) = duration;
	gst_app_src_push_buffer(output->src, buf);

	output->timestamp += duration;
	output->last_frame_ts = current_frame_ts;
}

static void *
worker_thread_function(void *data)
{
	struct gst_output *output = data;
	struct gst_backend *b = to_gst_backend(output->base.backend);

	pthread_mutex_lock(&output->mutex);

	while (!output->destroying) {
		if (!output->input.valid)
			pthread_cond_wait(&output->input_cond, &output->mutex);

		/* If the thread is awaken by destroy_worker_thread(),
		 * there might not be valid input */
		if (!output->input.valid)
			continue;

		g_main_context_iteration(b->context, FALSE);

		remoting_frame(output);
		output->input.valid = 0;
	}

	pthread_mutex_unlock(&output->mutex);

	return NULL;
}

static int
setup_worker_thread(struct gst_output *output)
{
	pthread_mutex_init(&output->mutex, NULL);
	pthread_cond_init(&output->input_cond, NULL);
	pthread_create(&output->worker_thread, NULL, worker_thread_function,
		       output);

	return 1;
}

static void
destroy_worker_thread(struct gst_output *output)
{
	pthread_mutex_lock(&output->mutex);

	/* Make sure the worker thread finishes */
	output->destroying = 1;
	pthread_cond_signal(&output->input_cond);

	pthread_mutex_unlock(&output->mutex);

	pthread_join(output->worker_thread, NULL);

	pthread_mutex_destroy(&output->mutex);
	pthread_cond_destroy(&output->input_cond);
}

static int
gst_backend_output_enable(struct remoting_backend_output *output)
{
	struct gst_output *gst_output = to_gst_output(output);
	char pipeline_str[1024];
	GstCaps *caps;

	if (setup_worker_thread(gst_output) < 0)
		return -1;

	snprintf(pipeline_str, 1023, "appsrc name=src ! videoconvert ! "
		 "video/x-raw,format=NV12 ! jpegenc ! rtpjpegpay ! "
		 "udpsink host=%s port=%d", output->host, output->port);
	weston_log("GST pipeline: %s\n", pipeline_str);

	gst_output->pipeline = gst_parse_launch(pipeline_str, NULL);
	if (!gst_output->pipeline) {
		weston_log("Could not create gstreamer pipeline\n");
		goto failed;
	}

	gst_output->src = (GstAppSrc*)
		gst_bin_get_by_name(GST_BIN(gst_output->pipeline), "src");
	caps = gst_caps_new_simple("video/x-raw",
				   "format", G_TYPE_STRING, "RGBx",
				   "width", G_TYPE_INT, output->width,
				   "height", G_TYPE_INT, output->height,
				   "framerate", GST_TYPE_FRACTION,
						output->refresh, 1,
				   NULL);
	if (!caps) {
		weston_log("Could not create gstreamer caps.\n");
		goto failed;
	}
	g_object_set(G_OBJECT(gst_output->src),
		     "caps", caps,
		     "stream-type", 0,
		     "format", GST_FORMAT_TIME,
		     "is-live", TRUE,
		     NULL);
	gst_caps_unref(caps);

	gst_element_set_state(gst_output->pipeline, GST_STATE_PLAYING);

	return 0;

failed:
	if (gst_output->pipeline) {
		gst_object_unref(GST_OBJECT(gst_output->pipeline));
		gst_output->pipeline = NULL;
	}
	destroy_worker_thread(gst_output);
	return -1;
}

static void
gst_backend_output_disable(struct remoting_backend_output *output)
{
	struct gst_output *gst_output = to_gst_output(output);

	weston_log("%s\n", __func__);
	if (gst_output->pipeline) {
		gst_element_set_state(gst_output->pipeline, GST_STATE_NULL);
		gst_object_unref(GST_OBJECT(gst_output->pipeline));
		gst_output->pipeline = NULL;

		destroy_worker_thread(gst_output);
	}
}

static void
gst_backend_output_destroy(struct remoting_backend_output *output)
{
	gst_backend_output_disable(output);
	free(output);
}

static int
gst_backend_output_frame(struct remoting_backend_output *output, int fd,
			  int size, int stride)
{
	struct gst_output *gst_output = to_gst_output(output);
	int ret = 0;

	pthread_mutex_lock(&gst_output->mutex);

	/* The mutex is never released while encoding,
	   never be reached if input. valid is true */
	assert(!gst_output->input.valid);

	gst_output->input.prime_fd = fd;
	gst_output->input.size = size;
	gst_output->input.stride = stride;
	gst_output->input.valid = 1;
	pthread_cond_signal(&gst_output->input_cond);

	pthread_mutex_unlock(&gst_output->mutex);
	return ret;
}

static struct remoting_backend_output *
gst_backend_create_output(struct remoting_backend *b)
{
	struct gst_output *output;

	output = zalloc(sizeof *output);
	if (!output)
		return NULL;

	output->base.backend = b;
	output->base.enable = gst_backend_output_enable;
	output->base.disable = gst_backend_output_disable;
	output->base.destroy = gst_backend_output_destroy;
	output->base.frame = gst_backend_output_frame;

	return (struct remoting_backend_output*)output;
}

static void
gst_backend_destroy(struct remoting_backend *b)
{
	struct gst_backend *gst_backend = to_gst_backend(b);
	gst_object_unref(gst_backend->allocator);
	g_main_context_unref(gst_backend->context);
	free(b);
	gst_deinit();
}

WL_EXPORT struct remoting_backend *
remoting_backend_init(struct weston_compositor *c)
{
	struct gst_backend *backend;
	GError *err = NULL;

	gst_init(NULL, NULL);
	if (!gst_init_check(NULL, NULL, &err)) {
		weston_log("GStreamer initialization error: %s\n",
			   err->message);
		g_error_free(err);
		return NULL;
	}

	backend = zalloc(sizeof *backend);
	if (!backend)
		goto failed;

	backend->context = g_main_context_new();
	if (!backend->context) {
		weston_log("Could not create context for gstreamer\n");
		goto failed;
	}

	backend->allocator = gst_dmabuf_allocator_new();
	if (!backend->allocator) {
		weston_log("Could not create dmabuf allocator for gstreamer\n");
		goto failed;
	}

	backend->base.compositor = c;
	backend->base.destroy = gst_backend_destroy;
	backend->base.create_output = gst_backend_create_output;

	return (struct remoting_backend *)backend;

failed:
	if (backend->context)
		g_main_context_unref(backend->context);
	if (backend)
		free(backend);
	gst_deinit();
	return NULL;
}
