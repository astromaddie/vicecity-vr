#version 450
#extension GL_EXT_multiview : require

#include "rw_common.glsl"

// Immediate-mode 2D: HUD, fonts, menus. The game submits vertices already in
// screen space, as the fixed-function pipeline expected.
//
// They cannot simply be written out as clip coordinates. Doing that puts the
// same NDC position in both eyes, and because the Quest's per-eye frustums are
// asymmetric, one NDC position is a different direction in each eye -- the two
// images pull apart and never converge.
//
// Instead the screen plane is placed in the world in front of the head by
// scene.im2dTransform, and each eye projects it with its own matrix. The
// result is a stable panel at a comfortable distance rather than something the
// eyes cannot fuse.

// x, y screen; z screen depth; w CAMERA Z IN METRES -- Im2DVertex's
// setRecipCameraZ stores the reciprocal of what it is handed, so this field
// always ends up holding the real distance, never its reciprocal.
layout(location = 0) in vec4 inPosition;
layout(location = 1) in vec4 inColour;
layout(location = 2) in vec2 inTexCoord;

layout(location = 0) out vec4 fragColour;
layout(location = 1) out vec2 fragTexCoord;
layout(location = 2) flat out float fragWorldSprite;

void main()
{
	// im2dTransform places the screen plane at scene.im2dParams.x metres from
	// the eye. Every vertex carries its true camera-space depth as 1/w, so
	// scaling its offset from the eye along the same ray puts it where it
	// actually belongs: coronas and other world sprites land on their light
	// source instead of floating on the panel in front of the face, and sort
	// against the scene through the depth test.
	//
	// HUD and font vertices come from CSprite2d, whose depth is the near clip
	// plane; clamping to the panel distance leaves those exactly where they
	// already were.
	// The plane arrives per draw in the model slot, so a single draw can be
	// placed somewhere else -- the wrist minimap rides on that.
	//
	// World sprites take the sprite plane instead: the full head frame the
	// game projected them through, so they land on their source. The game
	// counter-rolls their corners about the centre, and the HUD plane has
	// the head roll taken out.
	float panel = scene.im2dParams.x;
	float depth = max(inPosition.w, panel);
	bool deep = inPosition.w > panel+0.01;
	mat4 spritePlane = deep && push.surfaceProps.x <= 1.5 ? scene.im2dSpriteTransform : push.model;
	vec4 plane = spritePlane * vec4(inPosition.xy, 0.0, 1.0);
	vec3 eye = scene.im2dParams.yzw;
	vec3 world = eye + (plane.xyz - eye) * (depth/panel);
	gl_Position = scene.viewProj[gl_ViewIndex] * vec4(world, 1.0);
	// Wrist minimap. That map renders into a small texture of its own, where
	// the model slot is a straight screen-pixels-to-clip matrix and neither
	// eye is involved: both multiview layers receive the same flat map, and
	// what the eyes converge on is the quad the game hangs off the wrist.
	if(push.surfaceProps.x > 1.5)
		gl_Position = vec4(plane.xy, 0.5, 1.0);
	// Radar-only depth masking must sit in front of the already-rendered
	// cockpit/world. Preserve the tiny RenderWare screen-Z difference between
	// the outside-circle mask and the textured tiles: strict LESS depends on
	// that bias to reject the tiles only where the mask was written.
	if(push.surfaceProps.z > 0.5)
		gl_Position.z = max(inPosition.z, 0.00001) * gl_Position.w;
	fragColour = inColour * push.materialColour;
	fragTexCoord = inTexCoord;
	// HUD/fonts arrive at the near plane and are clamped to the head-locked
	// panel. Coronas, headlights and particles retain their real camera depth.
	fragWorldSprite = deep ? 1.0 : 0.0;
}
