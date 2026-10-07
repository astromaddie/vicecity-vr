/* sfvr_adjust.h - live world-scale and camera adjustment from left grip + right stick.
 *
 * Input axes are in OpenXR convention: x right, y up, each in [-1, 1]. World
 * scale is game units per metre (inverse apparent size): right grows the world
 * and therefore reduces this value; left shrinks the world and increases it.
 * Up moves the camera farther/higher by reducing both signed offsets; down moves
 * it closer/lower.
 *
 * Call once per focused XR frame with predictedDisplayTime in seconds. The
 * modifier is active only in an eligible gameplay or settings-preview context.
 * Menu and View take priority: no settings change is applied, though the
 * modifier stick remains consumed. After modifier release or focus loss, the
 * right stick stays muted until a later neutral sample, preventing stale input
 * from turning or selecting gameplay actions.
 */
#ifndef SFVR_ADJUST_H
#define SFVR_ADJUST_H

#ifdef __cplusplus
extern "C" {
#endif

#define SFVR_ADJUST_DEFAULT_DEADZONE 0.20f
#define SFVR_ADJUST_DEFAULT_WORLD_SIZE_FACTOR_PER_S 1.5f
#define SFVR_ADJUST_DEFAULT_CAMERA_UNITS_PER_S 0.25f
#define SFVR_ADJUST_DEFAULT_MAX_DT_S (1.0f / 30.0f)

typedef struct sfvr_adjust_config {
	float deadzone;
	float world_size_factor_per_s;
	float camera_units_per_s;
	float max_dt_s;
	float world_scale_min;
	float world_scale_max;
	float camera_closer_min;
	float camera_closer_max;
	float camera_lower_min;
	float camera_lower_max;
} sfvr_adjust_config;

typedef struct sfvr_adjust_state {
	double last_display_s;
	int display_time_valid;
	int right_waiting_for_neutral;
	int modifier_was_active;
	int neutral_required_after_release;
} sfvr_adjust_state;

/* Standard rates plus conservative generic ranges. Ports set world_scale bounds
 * to their own units; SM64 uses 20..400. The shared camera bounds are signed. */
void sfvr_adjust_config_default(sfvr_adjust_config *config);
void sfvr_adjust_init(sfvr_adjust_state *state);
/* Clear elapsed time and require a centered right stick after focus returns. */
void sfvr_adjust_focus_lost(sfvr_adjust_state *state);

/* Apply one frame of the modifier. right_stick_xy is consumed in place when
 * left grip is active in an eligible context, or while rearming after release.
 * NULL setting pointers disable that axis's adjustment. Returns nonzero if any
 * setting changed. */
int sfvr_adjust_update(sfvr_adjust_state *state, const sfvr_adjust_config *config,
	float right_stick_xy[2], int left_grip, int eligible, int menu_active, int view_active,
	double predicted_display_s, float *world_scale, float *camera_closer, float *camera_lower);

#ifdef __cplusplus
}
#endif

#endif
