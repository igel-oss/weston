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

#ifndef REMOTING_BACKEND_H
#define REMOTING_BACKEND_H

#include "compositor.h"

struct remoting_backend {
	struct weston_compositor *compositor;

	void (*destroy)(struct remoting_backend *b);
	struct remoting_backend_output *
		(*create_output)(struct remoting_backend *b);
};

struct remoting_backend_output {
	int width;
	int height;
	int refresh;
	char *host;
	int port;

	struct remoting_backend *backend;

	int (*enable)(struct remoting_backend_output *output);
	void (*disable)(struct remoting_backend_output *output);
	void (*destroy)(struct remoting_backend_output *output);
	int (*frame)(struct remoting_backend_output *output, int fd, int size,
		     int stride);
};

struct remoting_backend *
remoting_backend_init(struct weston_compositor *c);

#endif
