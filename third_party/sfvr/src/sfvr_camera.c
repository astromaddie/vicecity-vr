#include "sfvr/sfvr_camera.h"

#include <math.h>
#include <string.h>

static float clamp(float value, float lo, float hi)
{
	return value < lo ? lo : (value > hi ? hi : value);
}

void sfvr_camera_origin(const float source_camera[3], const float target[3],
	float camera_closer, float camera_lower, float out_origin[3])
{
	if (!source_camera || !target || !out_origin)
		return;
	if (!isfinite(camera_closer))
		camera_closer = 0.0f;
	if (!isfinite(camera_lower))
		camera_lower = 0.0f;
	camera_closer = clamp(camera_closer, SFVR_CAMERA_CLOSER_MIN, SFVR_CAMERA_CLOSER_MAX);
	camera_lower = clamp(camera_lower, SFVR_CAMERA_LOWER_MIN, SFVR_CAMERA_LOWER_MAX);

	float p[3];
	for (int i = 0; i < 3; i++)
		p[i] = source_camera[i] + camera_closer * (target[i] - source_camera[i]);
	p[1] = target[1] + (1.0f - camera_closer) * (1.0f - camera_lower)
		* (source_camera[1] - target[1]);
	memcpy(out_origin, p, sizeof(p));
}
