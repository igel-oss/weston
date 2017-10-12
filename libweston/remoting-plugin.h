#ifndef REMOTING_PLUGIN_H
#define REMOTING_PLUGIN_H

#include "compositor.h"
#include "plugin-registry.h"

#define WESTON_REMOTING_API_NAME	"weston_remoting_api_v1"

struct weston_remoting_api {
	/** Create remoted outputs
	 *
	 * Returns 0 on success, -1 on failure.
	 */
	int (*create_outputs)(struct weston_compositor *c,
			      int num_of_outputs);

	/** Check if output is remoted */
	bool (*is_remoted_output)(struct weston_output *output);

	/** Set mode */
	int (*set_mode)(struct weston_output *output, const char *modeline);

	/** Set gbm format */
	void (*set_gbm_format)(struct weston_output *output,
			       const char *gbm_format);

	/** Set seat */
	void (*set_seat)(struct weston_output *output, const char *seat);

	/** Set the destination Host(IP Address) */
	void (*set_host)(struct weston_output *output, char *ip);

	/** Set the port number */
	void (*set_port)(struct weston_output *output, int port);

	/** Set the bitrate */
	void (*set_bitrate)(struct weston_output *output, int bitrate);
};

static inline const struct weston_remoting_api *
weston_remoting_get_api(struct weston_compositor *compositor)
{
	const void *api;
	api = weston_plugin_api_get(compositor, WESTON_REMOTING_API_NAME,
				    sizeof(struct weston_remoting_api));

	return (const struct weston_remoting_api *)api;
}

/** Initialize remoting plugin
 *
 * Returns 0 on success, -1 on failure
 */
int weston_module_init(struct weston_compositor *compositor);


#endif /* REMOTING_PLUGIN_H */
