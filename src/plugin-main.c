#include "obs.h"
#include "obs-module.h"
#include "plugin-support.h"
#include <util/dstr.h>

/*
 * Sends the scene or source this filter sits on to a DeckLink device, through a private canvas
 * and OBS's own decklink_output. The filter draws nothing itself.
 *
 * Feather Creative fork (feathercreative/obs-decklink-output-filter), based on cg2121's
 * canvas rewrite. Changes from upstream main:
 * - Auto-start waits until the filter is attached to its parent (filter_add), not in create,
 *   when the parent isn't known yet.
 * - The filter is stopped when it is removed from its parent (filter_remove), before destroy.
 * - Stop releases in order: output first, then the canvas it reads from.
 * - Stop undoes the showing count that start added on the parent.
 * - No pointer to the properties button is kept. The properties object (and its button) is
 *   freed when the properties window closes, so updating it later from stop() was a
 *   use-after-free. The button label comes from get_properties, refreshed after a click.
 * - Each filter's output gets its own name, so several filters can run side by side.
 */

struct decklink_output_filter_context {
	obs_output_t *output;
	obs_source_t *source;
	obs_source_t *parent; /* set by filter_add, cleared by filter_remove; not referenced */
	obs_canvas_t *canvas;

	bool active;
	bool showing;
};

static const char *decklink_output_filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("DecklinkOutput");
}

static void decklink_output_filter_stop(struct decklink_output_filter_context *filter)
{
	if (!filter->active)
		return;

	filter->active = false;

	if (filter->output) {
		obs_output_stop(filter->output);
		obs_output_release(filter->output);
		filter->output = NULL;
	}

	if (filter->canvas) {
		obs_canvas_set_channel(filter->canvas, 0, NULL);
		obs_canvas_release(filter->canvas);
		filter->canvas = NULL;
	}

	if (filter->showing && filter->parent)
		obs_source_dec_showing(filter->parent);
	filter->showing = false;

	obs_log(LOG_INFO, "Filter stopped");
}

static void decklink_output_filter_start(struct decklink_output_filter_context *filter, obs_data_t *settings)
{
	if (filter->active)
		return;

	if (!obs_source_enabled(filter->source)) {
		obs_log(LOG_INFO, "Filter not enabled; not starting");
		return;
	}

	if (!filter->parent) {
		obs_log(LOG_INFO, "Filter not attached to a source yet; not starting");
		return;
	}

	const char *parent_name = obs_source_get_name(filter->parent);

	struct dstr name = {0};
	dstr_printf(&name, "decklink_filter_output (%s)", parent_name);
	filter->output = obs_output_create("decklink_output", name.array, settings, NULL);
	dstr_free(&name);
	if (!filter->output) {
		obs_log(LOG_ERROR, "Could not create the DeckLink output");
		return;
	}

	struct obs_video_info ovi;
	obs_get_video_info(&ovi);
	/* The canvas needs a name: obs-websocket (OBS 32) reads it when the canvas is destroyed
	 * and crashed on the unnamed one (2026-10-23). */
	struct dstr canvas_name = {0};
	dstr_printf(&canvas_name, "DeckLink filter (%s)", parent_name);
	filter->canvas = obs_canvas_create_private(canvas_name.array, &ovi, DEVICE);
	dstr_free(&canvas_name);
	if (!filter->canvas) {
		obs_log(LOG_ERROR, "Could not create a canvas");
		obs_output_release(filter->output);
		filter->output = NULL;
		return;
	}

	obs_canvas_set_channel(filter->canvas, 0, filter->parent);
	obs_output_set_media(filter->output, obs_canvas_get_video(filter->canvas), obs_get_audio());

	obs_source_inc_showing(filter->parent);
	filter->showing = true;
	filter->active = true;

	if (!obs_output_start(filter->output)) {
		obs_log(LOG_ERROR, "Filter failed to start (is the device in use?)");
		decklink_output_filter_stop(filter);
		return;
	}

	obs_log(LOG_INFO, "Filter started on '%s'", obs_source_get_name(filter->parent));
}

static bool auto_start_enabled(struct decklink_output_filter_context *filter)
{
	obs_data_t *settings = obs_source_get_settings(filter->source);
	bool auto_start = obs_data_get_bool(settings, "auto_start");
	obs_data_release(settings);
	return auto_start;
}

static void start_with_current_settings(struct decklink_output_filter_context *filter)
{
	obs_data_t *settings = obs_source_get_settings(filter->source);
	decklink_output_filter_start(filter, settings);
	obs_data_release(settings);
}

static void decklink_output_filter_update(void *data, obs_data_t *settings)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(settings);
}

static void set_filter_enabled(void *data, calldata_t *calldata)
{
	struct decklink_output_filter_context *filter = data;

	bool enable = calldata_bool(calldata, "enabled");

	if (enable && auto_start_enabled(filter))
		start_with_current_settings(filter);
	else if (!enable)
		decklink_output_filter_stop(filter);
}

static void *decklink_output_filter_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);

	struct decklink_output_filter_context *filter = bzalloc(sizeof(struct decklink_output_filter_context));
	filter->source = source;

	signal_handler_t *sh = obs_source_get_signal_handler(filter->source);
	signal_handler_connect(sh, "enable", set_filter_enabled, filter);

	return filter;
}

static void decklink_output_filter_add(void *data, obs_source_t *parent)
{
	struct decklink_output_filter_context *filter = data;
	filter->parent = parent;

	if (auto_start_enabled(filter))
		start_with_current_settings(filter);
}

static void decklink_output_filter_remove(void *data, obs_source_t *parent)
{
	UNUSED_PARAMETER(parent);

	struct decklink_output_filter_context *filter = data;
	decklink_output_filter_stop(filter);
	filter->parent = NULL;
}

static void decklink_output_filter_destroy(void *data)
{
	struct decklink_output_filter_context *filter = data;

	decklink_output_filter_stop(filter);

	signal_handler_t *sh = obs_source_get_signal_handler(filter->source);
	signal_handler_disconnect(sh, "enable", set_filter_enabled, filter);

	bfree(filter);
}

static void decklink_output_filter_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);

	struct decklink_output_filter_context *filter = data;
	obs_source_skip_video_filter(filter->source);
}

static bool button_cb(obs_properties_t *properties, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(properties);
	UNUSED_PARAMETER(property);

	struct decklink_output_filter_context *filter = data;

	if (filter->active)
		decklink_output_filter_stop(filter);
	else
		start_with_current_settings(filter);

	/* true: OBS rebuilds the properties, so the button label follows the new state */
	return true;
}

static obs_properties_t *decklink_output_filter_properties(void *data)
{
	struct decklink_output_filter_context *filter = data;

	obs_properties_t *props = obs_get_output_properties("decklink_output");
	bool active = filter && filter->active;
	obs_properties_add_button2(props, "Button", active ? obs_module_text("Stop") : obs_module_text("Start"),
				   button_cb, filter);

	return props;
}

struct obs_source_info decklink_output_filter = {.id = "decklink_output_filter",
						 .type = OBS_SOURCE_TYPE_FILTER,
						 .output_flags = OBS_SOURCE_VIDEO,
						 .get_name = decklink_output_filter_get_name,
						 .create = decklink_output_filter_create,
						 .destroy = decklink_output_filter_destroy,
						 .update = decklink_output_filter_update,
						 .video_render = decklink_output_filter_render,
						 .filter_add = decklink_output_filter_add,
						 .filter_remove = decklink_output_filter_remove,
						 .get_properties = decklink_output_filter_properties};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("decklink-output-filter", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "Decklink Output Filter";
}

bool obs_module_load(void)
{
	return true;
}

void obs_module_post_load(void)
{
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	obs_register_source(&decklink_output_filter);
}
