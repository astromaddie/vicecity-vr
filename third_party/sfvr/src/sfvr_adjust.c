#include "sfvr/sfvr_adjust.h"

#include <math.h>
#include <string.h>

void sfvr_adjust_config_default(sfvr_adjust_config *config)
{
	if (!config)
		return;
	config->deadzone = SFVR_ADJUST_DEFAULT_DEADZONE;
	config->world_size_factor_per_s = SFVR_ADJUST_DEFAULT_WORLD_SIZE_FACTOR_PER_S;
	config->camera_units_per_s = SFVR_ADJUST_DEFAULT_CAMERA_UNITS_PER_S;
	config->max_dt_s = SFVR_ADJUST_DEFAULT_MAX_DT_S;
	config->world_scale_min = 0.01f;
	config->world_scale_max = 10000.0f;
	config->camera_closer_min = -1.0f;
	config->camera_closer_max = 0.8f;
	config->camera_lower_min = -1.0f;
	config->camera_lower_max = 1.0f;
}

void sfvr_adjust_init(sfvr_adjust_state *state)
{
	if (state)
		memset(state, 0, sizeof(*state));
}

void sfvr_adjust_focus_lost(sfvr_adjust_state *state)
{
	if (!state)
		return;
	state->last_display_s = 0.0;
	state->display_time_valid = 0;
	state->right_waiting_for_neutral = 1;
	state->modifier_was_active = 0;
	state->neutral_required_after_release = 1;
}

static float clamp(float value, float lo, float hi)
{
	return value < lo ? lo : (value > hi ? hi : value);
}

static float bounded_axis(float value)
{
	if (!isfinite(value))
		return 0.0f;
	return clamp(value, -1.0f, 1.0f);
}

static float deadzoned(float value, float deadzone)
{
	float magnitude = fabsf(value);
	if (magnitude <= deadzone)
		return 0.0f;
	return copysignf((magnitude - deadzone) / (1.0f - deadzone), value);
}

static float clamp_setting(float value, float lo, float hi)
{
	if (!isfinite(value))
		return lo;
	return clamp(value, lo, hi);
}

int sfvr_adjust_update(sfvr_adjust_state *state, const sfvr_adjust_config *config,
	float right_stick_xy[2], int left_grip, int eligible, int menu_active, int view_active,
	double predicted_display_s, float *world_scale, float *camera_closer, float *camera_lower)
{
	sfvr_adjust_config defaults;
	const sfvr_adjust_config *cfg = config;
	double dt = 0.0;
	float x, y;
	int modifier, just_released, changed = 0;

	if (!state || !right_stick_xy)
		return 0;
	if (!cfg) {
		sfvr_adjust_config_default(&defaults);
		cfg = &defaults;
	}

	if (isfinite(predicted_display_s)) {
		if (state->display_time_valid && predicted_display_s >= state->last_display_s)
			dt = predicted_display_s - state->last_display_s;
		state->last_display_s = predicted_display_s;
		state->display_time_valid = 1;
	}
	else {
		state->display_time_valid = 0;
	}
	if (!isfinite(cfg->max_dt_s) || cfg->max_dt_s < 0.0f)
		dt = 0.0;
	else if (dt > cfg->max_dt_s)
		dt = cfg->max_dt_s;

	x = bounded_axis(right_stick_xy[0]);
	y = bounded_axis(right_stick_xy[1]);
	float deadzone = isfinite(cfg->deadzone) ? clamp(cfg->deadzone, 0.0f, 0.99f) : SFVR_ADJUST_DEFAULT_DEADZONE;
	float x_adjust = deadzoned(x, deadzone);
	float y_adjust = deadzoned(y, deadzone);
	modifier = eligible && left_grip;
	just_released = state->modifier_was_active && !modifier;
	if (just_released) {
		state->right_waiting_for_neutral = 1;
		state->neutral_required_after_release = 1;
	}
	state->modifier_was_active = modifier;

	if (modifier && !state->neutral_required_after_release && !menu_active && !view_active && dt > 0.0) {
		if (world_scale && x_adjust != 0.0f) {
			float factor = cfg->world_size_factor_per_s;
			if (!isfinite(factor) || factor <= 1.0f)
				factor = SFVR_ADJUST_DEFAULT_WORLD_SIZE_FACTOR_PER_S;
			float before = clamp_setting(*world_scale, cfg->world_scale_min, cfg->world_scale_max);
			float after = before * expf(-x_adjust * logf(factor) * (float)dt);
			after = clamp_setting(after, cfg->world_scale_min, cfg->world_scale_max);
			changed |= after != *world_scale;
			*world_scale = after;
		}
		if (camera_closer && y_adjust != 0.0f) {
			float rate = isfinite(cfg->camera_units_per_s) && cfg->camera_units_per_s >= 0.0f
				? cfg->camera_units_per_s : SFVR_ADJUST_DEFAULT_CAMERA_UNITS_PER_S;
			float before = clamp_setting(*camera_closer, cfg->camera_closer_min, cfg->camera_closer_max);
			float after = before - y_adjust * rate * (float)dt;
			after = clamp_setting(after, cfg->camera_closer_min, cfg->camera_closer_max);
			changed |= after != *camera_closer;
			*camera_closer = after;
		}
		if (camera_lower && y_adjust != 0.0f) {
			float rate = isfinite(cfg->camera_units_per_s) && cfg->camera_units_per_s >= 0.0f
				? cfg->camera_units_per_s : SFVR_ADJUST_DEFAULT_CAMERA_UNITS_PER_S;
			float before = clamp_setting(*camera_lower, cfg->camera_lower_min, cfg->camera_lower_max);
			float after = before - y_adjust * rate * (float)dt;
			after = clamp_setting(after, cfg->camera_lower_min, cfg->camera_lower_max);
			changed |= after != *camera_lower;
			*camera_lower = after;
		}
	}

	if (modifier) {
		if (fabsf(x) > deadzone || fabsf(y) > deadzone)
			state->right_waiting_for_neutral = 1;
		right_stick_xy[0] = right_stick_xy[1] = 0.0f;
	}
	else if (state->right_waiting_for_neutral) {
		if (!just_released && fabsf(x) <= deadzone && fabsf(y) <= deadzone) {
			state->right_waiting_for_neutral = 0;
			state->neutral_required_after_release = 0;
		}
		else
			right_stick_xy[0] = right_stick_xy[1] = 0.0f;
	}
	return changed;
}
