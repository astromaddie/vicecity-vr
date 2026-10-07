#pragma once

#ifdef RW_VULKAN

#include <vulkan/vulkan.h>

namespace rw {
namespace vulkan {

enum {
	NUM_FRAME_CONTEXTS = 2,
	MAX_FRAMEBUFFERS_PER_CONTEXT = 8,
};

// Native raster attached to every rw::Raster on this platform.
struct VulkanRaster
{
	VkImage image;
	VkDeviceMemory memory;
	// What that allocation cost; getTextureMemoryUsed sums these.
	VkDeviceSize memoryBytes;
	VkImageView view;
	VkFormat format;
	VkImageLayout layout;

	// Staging allocation kept alive between rasterLock and rasterUnlock. The
	// game locks a mip level, writes into it and unlocks, so the upload cannot
	// be issued until unlock time.
	VkBuffer stagingBuffer;
	VkDeviceMemory stagingMemory;
	uint8 *stagingMapped;
	int32 lockedLevel;
	uint32 lockedFlags;

	int32 numLevels;
	// Vice City ships its world textures with a single level, so the far
	// half of every fence, sign and railing was being minified straight
	// off the full-size image. Set when the chain was allocated for a
	// texture that arrived without one and has to be filled in here.
	bool32 generateMips;
	bool32 hasAlpha;
	bool32 isCompressed;
	// Set when the source was a DXT/BC texture that this device cannot sample
	// natively and had to be decompressed on upload.
	bool32 wasTranscoded;

	// Descriptor state is per frame slot. Updating a descriptor while an
	// earlier submission still reads it is invalid Vulkan even when the image
	// itself is immutable.
	VkDescriptorSet descriptorSet[NUM_FRAME_CONTEXTS];
	uint32 samplerKey[NUM_FRAME_CONTEXTS];
};

// Mirrors the state the fixed-function RenderWare pipeline expects. Vulkan has
// no render-state machine, so the values are accumulated here and folded into
// a pipeline key when a draw is recorded.
struct RenderState
{
	uint32 vertexAlphaEnabled;
	uint32 srcBlend;
	uint32 dstBlend;
	uint32 zTestEnabled;
	uint32 zWriteEnabled;
	uint32 cullMode;
	uint32 alphaTestFunction;
	uint32 alphaTestRef;
	// The PS2 two-pass alpha rule. The game asks for it -- reVC sets it
	// from gPS2alphaTest every frame -- and this backend was the only one
	// dropping it on the floor; d3d9 and gl3 have carried it all along.
	uint32 gsAlphaTest;
	uint32 gsAlphaTestRef;
	// Mip level bias in half steps, for the draw only. High-frequency
	// leaf masks sparkle at distance; biasing them a level or two down
	// the chain is what a temporal filter would have done for free.
	uint32 mipLodBias;
	uint32 fogEnabled;
	RGBA fogColor;
	// Set through SetRenderStatePtr(TEXTURERASTER). Immediate mode reads it
	// back to pick the descriptor set, so it has to be stored, not dropped.
	Raster *texture;
	uint32 textureFilter;
	uint32 textureAddressU;
	uint32 textureAddressV;
};

// Kept at 32 bytes so the CPU layout exactly matches the fragment shader's
// vec4 + uvec4 push-constant block.
struct PostFxPushConstants
{
	float32 blurColour[4];
	uint32 mode[4];
	// x/y = native OpenXR output extent, z = active SGSR mode.
	uint32 upscale[4];
};

struct MotionUniformData
{
	float32 clipToPrevClip[2][16];
	// xy = scene size, z = history valid, w = temporal mode.
	float32 temporalParams[4];
	// xy = current SGSR2 Halton jitter in render pixels,
	// z = horizontal tan-half-FOV approximation, w = history reset.
	float32 sgsrParams[4];
};

// Everything the GPU may still be reading after a frame is submitted lives
// in the frame context that owns that submission.  Reusing another context
// lets the CPU record frame N+1 while the GPU executes frame N.
struct FrameContext
{
	VkCommandBuffer commandBuffer;
	VkFence fence;
	bool32 submissionPending;

	VkQueryPool timestampQueryPool;
	bool32 timestampPending;

	VkImage depthImage;
	VkDeviceMemory depthMemory;
	VkImageView depthView;

	VkImage sceneColourImage;
	VkDeviceMemory sceneColourMemory;
	VkImageView sceneColourView;
	VkImage sceneMsaaImage;
	VkDeviceMemory sceneMsaaMemory;
	VkImageView sceneMsaaView;
	VkFramebuffer sceneFramebuffer;
	VkDescriptorSet postFxDescriptor;
	bool32 sceneColourInitialised;
	// Reduced copy of the finished frame, for the reflection block to
	// sample instead of the full image. Built on the theory that the
	// scattered lookup was missing the texture cache; measuring says
	// otherwise -- a quarter-resolution copy moved the frame time by
	// nothing. It stays as a knob, not as a fix, and the size the
	// player picks includes reading the frame itself.
	VkImage sceneReflectionImage;
	VkDeviceMemory sceneReflectionMemory;
	VkImageView sceneReflectionView;
	bool32 sceneReflectionInitialised;
	bool32 reflectionHistoryValid;
	bool32 reflectionCoverageComplete;

	// V3 temporal history stores the already resolved (but not colour-filtered)
	// result. It is allocated only for SGSR2_RESOLVED_TEMPORAL_V3.
	VkImage resolvedHistoryImage;
	VkDeviceMemory resolvedHistoryMemory;
	VkImageView resolvedHistoryView;
	bool32 resolvedHistoryInitialised;

	// Official SGSR2 Convert output: motion.xy, depth clip and alpha mask.
	VkImage sgsrConvertImage;
	VkDeviceMemory sgsrConvertMemory;
	VkImageView sgsrConvertView;
	VkFramebuffer sgsrConvertFramebuffer;
	VkDescriptorSet sgsrConvertDescriptor;

	// SGSR2 foundation. Allocated only for MOTION DEBUG, so normal gameplay
	// pays neither the memory cost nor the depth writeback cost.
	VkImage motionImage;
	VkDeviceMemory motionMemory;
	VkImageView motionView;
	VkFramebuffer motionFramebuffer;
	VkDescriptorSet motionDescriptor;
	VkBuffer motionBuffer;
	VkDeviceMemory motionBufferMemory;
	void *motionMapped;

	VkImageView framebufferColourViews[MAX_FRAMEBUFFERS_PER_CONTEXT];
	VkFramebuffer framebuffers[MAX_FRAMEBUFFERS_PER_CONTEXT];
	uint32 numFramebuffers;
	VkFramebuffer framebuffer;
};

struct Globals
{
	VkInstance instance;
	VkPhysicalDevice physicalDevice;
	VkDevice device;
	VkQueue queue;
	uint32 queueFamilyIndex;

	VkPhysicalDeviceProperties deviceProperties;
	VkPhysicalDeviceMemoryProperties memoryProperties;
	VkPhysicalDeviceFeatures deviceFeatures;

	VkCommandPool commandPool;
	FrameContext frames[NUM_FRAME_CONTEXTS];
	uint32 activeFrame;
	uint32 nextFrame;
	uint32 lastSubmittedFrame;
	bool32 hasSubmittedFrame;
	// Alias used by draw code while a frame is being recorded.
	VkCommandBuffer frameCommands;
	// Optional two-timestamp query for the native Quest profiler.  The backend
	// reads a slot only after that slot's fence signals.
	uint32 frameTimestampValidBits;
	bool32 frameTimestampEnabled;
	bool32 frameTimestampAvailable;
	float32 frameGpuMilliseconds;

	VkRenderPass renderPass;
	VkRenderPass postFxRenderPass;
	VkFormat depthFormat;

	VkDescriptorSetLayout postFxDescriptorLayout;
	VkDescriptorPool postFxDescriptorPool;
	VkSampler postFxSampler;
	VkSampler motionDepthSampler;
	VkPipelineLayout postFxPipelineLayout;
	VkPipeline postFxPipeline;
	PostFxPushConstants postFxConstants;
	VkRenderPass motionRenderPass;
	VkFormat motionFormat;
	VkDescriptorSetLayout motionDescriptorLayout;
	VkDescriptorPool motionDescriptorPool;
	VkPipelineLayout motionPipelineLayout;
	VkPipeline motionPipeline;
	VkRenderPass sgsrConvertRenderPass;
	VkDescriptorSetLayout sgsrConvertDescriptorLayout;
	VkDescriptorPool sgsrConvertDescriptorPool;
	VkPipelineLayout sgsrConvertPipelineLayout;
	VkPipeline sgsrConvertPipeline;

	uint32 width;
	uint32 height;
	float32 renderScaleEffectivePercent;
	uint32 sceneWidth;
	uint32 sceneHeight;
	uint32 sgsrMode;
	VkSampleCountFlagBits sceneSamples;
	uint32 viewCount;
	VkFormat colourFormat;

	bool32 initialised;
	bool32 inFrame;
	bool32 supportsBC;
	bool32 fragmentStoresAndAtomicsEnabled;
	// Off unless the port asks for it: the game's own TXDs decide how
	// many levels a texture has, and building the rest costs memory.
	bool32 generateMipmaps;

	float32 stereoViewProjection[2][16];
	// Exact OpenXR matrices before temporal projection jitter. SGSR2 motion
	// reconstruction and dynamic-object velocity must use these; jitter is
	// supplied separately to the official upscaler.
	float32 stereoViewProjectionUnjittered[2][16];
	float32 previousStereoViewProjection[2][16];
	float32 previousStereoViewProjectionUnjittered[2][16];
	float32 previousWorldClip[2][16];
	bool32 temporalHistoryValid;
	uint64 motionFrameSerial;
	float32 sgsrJitter[2];
	float32 sgsrHorizontalTanHalfFov;
	float32 stereoCullPlanes[2][5][4];
	bool32 stereoCullPlanesValid;
	// Game world space to OpenXR play space, rebuilt from the camera in
	// beginUpdate and folded into each draw's model matrix. Captured when the
	// draw is recorded, so a frame that switches cameras stays correct.
	float32 worldToPlay[16];
	// The fold the submitted frame's world draws used, captured in endFrame;
	// the reflection reprojection needs it to move a probe from this frame's
	// play space into the one the sampled image was rendered in.
	float32 worldToPlaySubmitted[16];
	// And the eye matrices that frame rendered with, captured the same way.
	// scene.previousViewProj is NOT usable for this: it falls back to the
	// CURRENT matrices whenever temporalHistoryValid is off -- which with
	// SGSR disabled is always -- and a current-pose lookup into a one-frame-
	// old image makes every reflection stick to the head and snap back.
	float32 reflectionPrevViewProj[2][16];
	bool32 reflectionPrevValid;
	// Mid-eye pose in play space, updated once per frame by the app layer.
	float32 headPosition[3];
	float32 headPoseCorrection[3];
	float32 headPoseOffset[3];
	float32 worldScale;
	float32 headTranslation;
	float32 heightOffsetMetres;
	float32 standingHeadHeight;
	bool32 standingHeadHeightValid;
	bool32 seatedTracking;
	bool32 trackingComfortValid;
	float32 headYaw;
	float32 headQuat[4];
	// Active Im2D plane. It lives here rather than in the once-per-frame
	// scene block so a draw can move the interface mid-frame -- the wrist
	// minimap swaps it around the radar and puts it back.
	float32 im2dTransformActive[16];
	// Set once the app supplies its own world-sprite plane; until then the
	// sprites follow the HUD plane, as they always did.
	bool32 im2dSpriteOverride;
	// Wrist panels. Each draws into a private little target before the
	// frame's own pass opens; wristPanelOffscreen tells the Im2D path that
	// the model slot is carrying a straight screen-pixels-to-clip matrix for
	// one of them.
	void (*wristPanelRenderer[WRIST_PANEL_COUNT])(void);
	bool32 wristPanelWanted[WRIST_PANEL_COUNT];
	bool32 wristPanelOffscreen;
	float32 eyePosition[2][3];
	float32 eyeQuat[2][4];
	bool32 eyePosesValid;
	// Render pass clear colour; carries the timecycle sky. See beginFrame.
	float32 clearColour[3];
	// Linear fog planes, supplied by the game from its time cycle.
	float32 fogStart;
	float32 fogEnd;
	// This frame's dynamic lights, world space, straight from the game's
	// point light list. The play-space copy the shaders read is derived from
	// these against the current camera fold; see refreshScenePointLights.
	// The world-space originals also feed the per-draw light masks in
	// drawAtomicMeshes, together with the camera's world position.
	PointLight pointLights[MAX_POINT_LIGHTS];
	uint32 pointLightCount;
	// Zero disables the air-glow term and its per-draw mask work entirely.
	float32 pointLightGlowStrength;
	float32 cameraWorldPos[3];
	bool32 carReflections;
	// The reduced copy above: its size, and whether the colour format
	// can be blitted into it with a linear filter at all. Without the
	// blit the reflection stays on the full-resolution image.
	uint32 sceneReflectionDivisor;
	uint32 sceneReflectionWidth;
	uint32 sceneReflectionHeight;
	bool32 sceneReflectionBlit;
	// The last MatFX env-map raster a draw carried. Vice City's vehicles all
	// share the same few streak textures, so one global binding (set 3, once
	// per frame) serves them all; cleared when the raster is destroyed.
	Raster *envRaster;
	// The vehicle the player occupies; its atomics keep the panorama
	// reflection layer only. See setPlayerVehicle.
	float32 playerVehiclePos[3];
	float32 playerVehicleRadius;
	bool32 playerVehicleActive;
	// Player-tuned reflection strengths; see setCarReflectionParams.
	float32 carReflectionIntensity;
	float32 carReflectionSsr;
	float32 carReflectionSsrDistance;
	// Modern water state: the player switch, the per-draw water hint from
	// the game's render commands, ripple time and the glint sun.
	bool32 modernWater;
	bool32 im3dWaterDraw;
	float32 effectsTime;
	float32 waterSunDirWorld[3];
	float32 waterSunColour[3];
	// The seven WATER sliders in SceneData order; see setWaterParams.
	float32 waterParams[8];
#if defined(MIAMIVR_DEV_TOOLS) && MIAMIVR_DEV_TOOLS
	// Culling visualizer: the master switch, and the flat colour index the
	// next world draw is tinted with (1..4, 0 = untinted).
	bool32 debugVizActive;
	uint32 debugTint;
#endif

	// First person anchor: the player's head in the game world. anchorYaw is
	// the world yaw that play-space forward maps to. On foot it is latched at
	// activation (room-scale: the world holds still); in a vehicle it tracks
	// the vehicle heading (cockpit: the world turns with the car), with the
	// head yaw latched at entry riding on top.
	bool32 firstPersonActive;
	bool32 fpFollowHeading;
	bool32 fpUseFullBasis;
	float32 fpHeadWorld[3];
	float32 fpAnchorYaw;
	float32 fpLatchedHeadYaw;
	// Play-space head position latched together with fpLatchedHeadYaw. The
	// game anchor is placed on THIS position, never on the live one, so the
	// difference between them -- leaning, ducking, stepping in the room --
	// survives as positional (6DOF) tracking. Anchoring on the live position
	// cancels exactly against the per-eye poses and leaves rotation only.
	float32 fpLatchedHeadPos[3];
	// Play-space +X/+Y/+Z expressed in game world space. Used while a
	// vehicle is allowed to carry pitch/roll into the headset camera.
	float32 fpPlayX[3];
	float32 fpPlayY[3];
	float32 fpPlayZ[3];
};

extern Globals gvk;

inline bool32 carReflectionsActive(void)
{
	return gvk.carReflections && gvk.carReflectionIntensity > 0.0f;
}

inline bool32 sceneReflectionsActive(void)
{
	return (carReflectionsActive() && gvk.carReflectionSsr > 0.0f) ||
		(gvk.modernWater && gvk.waterParams[3] > 0.0f);
}

inline bool32 worldEffectsActive(void)
{
	return gvk.pointLightCount > 0 || carReflectionsActive() || gvk.modernWater;
}

inline bool32 reflectionCoverageActive(void)
{
	return gvk.fragmentStoresAndAtomicsEnabled && sceneReflectionsActive();
}
extern RenderState gstate;

// ---------------------------------------------------------------------------
// Shader variants and pipeline state
// ---------------------------------------------------------------------------

// One fixed interleaved layout for all world geometry, instead of deriving a
// vertex format per Geometry the way the GL backend does. Geometries without
// normals or prelighting get defaults filled in at instance time.
//
// This trades a little memory for one vertex-input description and therefore a
// far smaller pipeline cache. On a tiler, pipeline count and the associated
// shader patching cost matter more than 36 bytes per vertex.
struct WorldVertex
{
	float32 position[3];
	float32 normal[3];
	uint8 colour[4];
	float32 texCoord[2];
};

// Skinned geometry: the world vertex plus the four bone weights and indices
// RenderWare stores per vertex. Peds and anything else driven by an HAnim
// hierarchy comes through here.
struct SkinVertex
{
	float32 position[3];
	float32 normal[3];
	uint8 colour[4];
	float32 texCoord[2];
	float32 weights[4];
	uint8 boneIndices[4];
};

// RenderWare's own ceiling, and what the bone uniform block is sized for.
#define RW_MAX_BONES 64

enum ShaderVariant
{
	SHADER_WORLD,
	SHADER_IM2D,
	SHADER_IM3D,
	SHADER_SKIN,
	SHADER_COUNT
};

// Mirrors SceneData in rw_common.glsl. Written once per frame.
struct SceneData
{
	// Play space (OpenXR, metres, Y up) to clip. World geometry gets there
	// through the game camera transform folded into PushConstants::model.
	float32 viewProj[2][16];
	// Previous submitted-eye projection. Dynamic atomics use this together
	// with their cached root translation; static geometry falls back to the
	// exact depth reconstruction in the post pass.
	float32 previousViewProj[2][16];
	float32 fogColour[4];
	float32 fogParams[4];
	float32 ambient[4];
	// The frame's dynamic lights in play space; see refreshScenePointLights.
	// xyz = position, w = radius / rgb = colour / xyz = cone axis, w = cone.
	float32 lightPosRad[8][4];
	float32 lightColour[8][4];
	float32 lightDir[8][4];
	float32 lightCount[4];
	float32 im2dTransform[16];
	// x = Im2D plane distance, yzw = eye position in play space.
	float32 im2dParams[4];
	// Plane the world sprites (coronas, flares, the aim marker) land on: the
	// full head frame, roll included, because the game projected them through
	// a camera that carries the roll. The HUD plane in im2dTransform has the
	// roll taken out. See rw_im2d.vert.
	float32 im2dSpriteTransform[16];
	// The timecycle sky top the render pass clears to; the vehicle env
	// reflection mixes toward it away from the horizon's fog colour.
	float32 skyColour[4];
	// Current play space to the previous frame's clip, per eye: previous
	// view projection composed with the fold delta between the two frames.
	// See refreshSceneReprojection.
	float32 reflectionReproject[2][16];
	// 16x16 screen cells per eye that interface (Im2D) draws touched in the
	// frame the reflections sample: words 0-7 left eye, 8-15 right eye.
	uint32 im2dCoverage[16];
	// x = overall reflection intensity, y = SSR weight inside it, z = SSR
	// range in metres, w = game-time seconds for the water ripples.
	float32 reflectionParams[4];
	// xyz = direction toward the sun in play space, w = its colour as
	// RGBA8; the water glint reads it.
	float32 waterSun[4];
	// Play space back to game world. The water ripple field must anchor in
	// world coordinates -- play space moves with the camera -- and the near
	// water is atomic geometry whose vertex shader only ever sees play
	// space, so the fragment stage undoes the fold with this.
	float32 playToWorld[16];
	// The WATER page sliders as multipliers of the base recipe: waves,
	// speed, distortion, reflection / sheen, glint, sparks, unused.
	float32 waterParams1[4];
	float32 waterParams2[4];
#if defined(MIAMIVR_DEV_TOOLS) && MIAMIVR_DEV_TOOLS
	// Culling visualizer: x != 0 puts every world draw into flat debug
	// colour, its index carried per draw in surfaceProps.z. Zero in play.
	float32 vizParams[4];
#endif
};

// Mirrors PushConstants in rw_common.glsl. Exactly 128 bytes -- the guaranteed
// minimum -- so no device needs a fallback path and no byte is wasted.
//
// Lighting rides in the push constants rather than the scene block because it
// is resolved per atomic: world geometry carries its light baked into prelight
// (Geometry::LIGHT unset, ambient contribution zero), while peds and vehicles
// are lit dynamically. One block per frame cannot express that split.
struct PushConstants
{
	float32 model[16];
	float32 materialColour[4];
	float32 surfaceProps[4];
	// rgb = summed ambient from the world, already zeroed for unlit geometry.
	float32 ambientLight[4];
	// xyz = first directional's direction, rotated into play space to match
	// the normals; w = the light colour packed RGBA8 (unpackUnorm4x8).
	float32 lightDirColour[4];
};

// Stall checkpoints, provided by the application layer. Same binary, so this
// is just a forward declaration rather than a dependency.
} // namespace vulkan
} // namespace rw
namespace platform { void setCheckpoint(const char *label); }
namespace rw {
namespace vulkan {
#define VK_CHECKPOINT(label) platform::setCheckpoint(label)

// out = a*b, both column major. out may not alias a or b.
void multiplyMatrix(float32 out[16], const float32 a[16], const float32 b[16]);

// initSkin and makeSkinPipeline are declared in rwvk.h, next to the default
// pipeline, because skin.cpp needs them and only sees the public header.

// Geometry buffers that live for the lifetime of a model, staged into
// device-local memory.
void destroyStaticBuffer(VkBuffer &buffer, VkDeviceMemory &memory);
bool32 uploadStaticBuffer(const void *data, VkDeviceSize size,
                          VkBufferUsageFlags usage, VkBuffer *bufferOut,
                          VkDeviceMemory *memoryOut);

// The material walk shared by the object pipelines. boneOffset is nil for
// unskinned geometry; when set, descriptor set 2 is bound at that offset.
//
// Two passes, opaque then blended, mirroring the D3D12 backend's
// MeshSelection. A model's meshes arrive in file order, and drawing a blended
// mesh before the opaque mesh behind it leaves nothing to blend against: the
// triangle shows the background through the model.
void drawAtomicMeshes(Atomic *atomic, InstanceDataHeader *header, uint32 shader,
                      const uint32 *boneOffset);
void resetAtomicMotionHistory(void);

bool32 stateInit(void);
void stateShutdown(void);

// Builds (or returns a cached) pipeline for the given shader variant and
// topology, folding in the current render-state cache.
VkPipeline getPipeline(uint32 shader, VkPrimitiveTopology topology);

// Compiles anything getPipeline had to defer. Must be called outside a render
// pass; beginFrame does it before opening one.
void compilePendingPipelines(void);

// One line per frame while the counts change; silent once they settle.
void reportImStats(void);
VkPipelineLayout getPipelineLayout(void);

// Descriptor set 1 for a raster, or the 1x1 white fallback when raster is nil.
VkDescriptorSet getTextureDescriptor(Raster *raster);
VkDescriptorSet getSceneDescriptor(void);

// Descriptor set 2: the bone matrix block, a dynamic uniform buffer aliased
// over the per-frame allocator. One set exists for the whole run; each draw
// picks its own matrices with a dynamic offset. Only the skin pipeline binds
// it, but the layout is part of every pipeline so the two share a layout.
VkDescriptorSet getBoneDescriptor(void);
// Alignment a bone block must start at to be usable as a dynamic offset.
VkDeviceSize getBoneBlockAlignment(void);

SceneData *getSceneData(void);
void setStateFrame(uint32 frame);
void uploadSceneData(void);
// Writes and binds set 3 -- env streak texture + previous frame's scene
// colour -- for the reflection block; previousScene may be null before the
// first frame exists.
void bindEnvironmentDescriptor(VkCommandBuffer commandBuffer,
                               VkImageView previousScene);
// Rebuilds the play-space dynamic lights in the scene block from the
// world-space list the game pushed; call before uploadSceneData whenever
// worldToPlay may have changed.
void refreshScenePointLights(void);
// Rebuilds the reflection reprojection matrices; same rule as the lights.
void refreshSceneReprojection(void);
// Reads back and clears the interface coverage mask the Im2D fragments of
// this slot's previous run wrote; call after the slot's fence is waited.
void readAndResetCoverage(uint32 out[16]);

// Bump allocator over a per-frame host-visible buffer. Reset in beginFrame,
// which is safe because beginFrame has already waited on the previous frame's
// fence. Used for all immediate-mode geometry.
bool32 allocateDynamic(VkDeviceSize size, VkDeviceSize alignment,
                       VkBuffer *bufferOut, VkDeviceSize *offsetOut,
                       void **mappedOut);
void resetDynamic(void);

// Streaming frees GPU objects on the game thread.  With more than one frame
// submitted those destroys are retired behind the frame fence that last used
// them instead of invalidating commands already executing on the GPU.
void retireBuffer(VkBuffer buffer, VkDeviceMemory memory);
void retireImage(VkImageView view, VkImage image, VkDeviceMemory memory);
// Texture descriptor sets leak permanently without this: every destroyed
// raster must hand its sets back or the fixed-size pool exhausts and all
// later textures draw white.
void retireTextureDescriptorSets(VkDescriptorSet *sets, uint32 count);
// Implemented in vkstate.cpp, which owns the descriptor pool.
void freeTextureDescriptorSets(VkDescriptorSet *sets, uint32 count);

// Shared helpers used by both the device and the raster implementation.
bool32 findMemoryType(uint32 typeBits, VkMemoryPropertyFlags properties,
                      uint32 *indexOut);
bool32 createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                    VkMemoryPropertyFlags properties, VkBuffer *bufferOut,
                    VkDeviceMemory *memoryOut);
// Records and submits a one-shot command buffer. Live-frame uploads inherit
// the active frame fence; startup uploads retain a synchronous fallback.
VkCommandBuffer beginOneShot(void);
void endOneShot(VkCommandBuffer commandBuffer);
void transitionImageLayout(VkCommandBuffer commandBuffer, VkImage image,
                           VkImageAspectFlags aspect, uint32 levelCount,
                           VkImageLayout from, VkImageLayout to);

}
}

#endif
