// Shared uniform layout for the librw Vulkan backend.
//
// Set 0 is per-frame scene data, written once by the VR layer. Set 1 is the
// material texture. Everything that changes per draw travels in push constants,
// which keeps descriptor churn out of the world pass entirely: Vice City issues
// thousands of small draws per frame and a descriptor update per draw would
// dominate the CPU cost on this hardware.

#define RW_MAX_LIGHTS 8
// Must match RW_MAX_BONES in rwvkimpl.h: RenderWare's own ceiling.
#define RW_MAX_BONES 64

layout(constant_id = 1) const int RW_WORLD_EFFECTS = 0;

layout(set = 0, binding = 0) uniform SceneData {
	// Indexed by gl_ViewIndex. Both entries hold the same matrix in mono.
	// Play space (OpenXR, metres, Y up) to clip.
	//
	// World geometry does not reach this space by itself: RenderWare is Z up
	// with a mirrored X, and the viewer stands wherever the game camera says.
	// That transform is folded into push.model per draw rather than kept here,
	// because the game runs more than one camera per frame and every draw in a
	// frame reads this block at the value it holds when the frame is submitted.
	mat4 viewProj[2];
	mat4 previousViewProj[2];
	vec4 fogColour;
	// x = start, y = end, z = 1/(end-start), w = enabled
	vec4 fogParams;
	vec4 ambient;
	// The game's dynamic lights -- headlights, explosions, street lamps --
	// already moved to play space on the CPU. RenderWare's fixed function only
	// ever fed these to peds and vehicles as extra directionals; here the
	// fragment stage applies them to everything, so a headlight lands on the
	// road it is pointed at.
	// xyz = position, w = radius
	vec4 lightPosRad[RW_MAX_LIGHTS];
	vec4 lightColour[RW_MAX_LIGHTS];
	// xyz = cone axis, w > 0.5 marks a cone (car headlights)
	vec4 lightDir[RW_MAX_LIGHTS];
	// x = active dynamic light count, y = air glow strength (0 disables),
	// zw = scene pixel size for gl_FragCoord-based lookups
	vec4 lightCount;
	// Places the screen-space Im2D plane in the world, in front of the head.
	// Both eyes then project it through their own matrix, which is what makes
	// the HUD and menus converge; writing identical clip coordinates to both
	// eyes does not, because the two frustums are asymmetric.
	mat4 im2dTransform;
	// x = distance the Im2D plane sits at, yzw = eye position in play space.
	// Together they turn a screen vertex plus its own camera depth back into
	// a world point; see rw_im2d.vert.
	vec4 im2dParams;
	// Plane the world sprites land on (full head frame); im2dTransform above
	// carries the HUD, which may have its roll removed. See rw_im2d.vert.
	mat4 im2dSpriteTransform;
	// The timecycle sky top the render pass clears to; the vehicle env
	// reflection mixes toward it away from the horizon's fog colour.
	vec4 skyColour;
	// Current play space to the previous frame's clip, per eye, with the
	// camera-fold delta between the frames already composed in. Projecting
	// through previousViewProj alone leaves out that delta, and the
	// reflection then swims by one frame of camera motion and snaps back.
	mat4 reflectionReproject[2];
	// 16x16 screen cells per eye that interface (Im2D) draws touched in the
	// frame the reflections sample: words 0-7 left eye, 8-15 right eye. A
	// help box is head-locked, so its ghost would otherwise slide across a
	// car body with every head turn.
	uvec4 im2dCoverage[4];
	// x = overall reflection intensity, y = SSR weight inside it, z = how
	// far from the eye the SSR part reaches in metres before fading to the
	// panorama, w = game-time seconds for the water ripples. Player-tuned;
	// 1.0 / 1.0 / huge is the baseline.
	vec4 reflectionParams;
	// xyz = direction toward the sun in play space, w = its colour as
	// RGBA8; the modern-water glint reads it.
	vec4 waterSun;
	// Play space back to game world; the water ripple field anchors there.
	mat4 playToWorld;
	// The WATER page sliders as multipliers of the base recipe:
	// waterParams1 = wave steepness / scroll speed / mirror distortion /
	// reflection strength, waterParams2 = sky sheen / sun glint / light
	// sparks / unused.
	vec4 waterParams1;
	vec4 waterParams2;
	// x != 0 = culling visualizer active; the draw's flat colour index then
	// arrives in surfaceProps.z instead of the packed light mask.
#if defined(MIAMIVR_DEV_TOOLS) && MIAMIVR_DEV_TOOLS
	vec4 vizParams;
#endif
} scene;

layout(push_constant) uniform PushConstants {
	mat4 model;
	// Material colour modulated with the vertex colour.
	vec4 materialColour;
	// x = ambient, y = diffuse, z = the draw's dynamic-light state packed as
	// an exact integer -- bits 0-7 surface light mask, 8-15 glow mask, 16
	// real normals, 17-22 the MatFX env coefficient for the reflection,
	// 23 the player's own vehicle, which keeps the panorama layer only
	// (Im2D reuses the slot for its strict-depth select) -- w = alpha test
	// reference
	vec4 surfaceProps;
	// rgb = world ambient, zero for geometry lit purely by prelight.
	vec4 ambientLight;
	// xyz = directional light direction in play space, w = colour as RGBA8.
	vec4 lightDirColour;
} push;

// In MOTION DEBUG the otherwise constant affine W row carries the previous
// root-translation delta and a validity flag. Reconstruct the actual affine
// matrix before transforming vertices. This keeps PushConstants at the
// Vulkan-guaranteed 128 bytes instead of adding another per-draw buffer.
mat4 RwModelMatrix()
{
	mat4 model = push.model;
	model[0].w = 0.0;
	model[1].w = 0.0;
	model[2].w = 0.0;
	model[3].w = 1.0;
	return model;
}

vec4 RwRootScreenMotion()
{
	return vec4(push.model[0].w, push.model[1].w,
	            push.model[2].w, push.model[3].w);
}
