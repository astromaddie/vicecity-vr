/* sfvr_camera.h - signed third-person camera offset interpolation.
 *
 * For source camera E looking at target A, positive camera_closer moves E
 * toward A and negative values extend it away. camera_lower moves the remaining
 * vertical offset toward A when positive and away/higher when negative. Zero
 * for both reproduces E exactly. The canonical setting bounds are closer
 * [-1, 0.8] and lower [-1, 1].
 */
#ifndef SFVR_CAMERA_H
#define SFVR_CAMERA_H

#ifdef __cplusplus
extern "C" {
#endif

#define SFVR_CAMERA_CLOSER_MIN (-1.0f)
#define SFVR_CAMERA_CLOSER_MAX 0.8f
#define SFVR_CAMERA_LOWER_MIN (-1.0f)
#define SFVR_CAMERA_LOWER_MAX 1.0f

/* The input and output vectors are game-world XYZ. NULL input gives no output. */
void sfvr_camera_origin(const float source_camera[3], const float target[3],
	float camera_closer, float camera_lower, float out_origin[3]);

#ifdef __cplusplus
}
#endif

#endif
