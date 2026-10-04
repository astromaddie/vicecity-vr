#pragma once

#ifdef RW_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace rw {

#ifdef RW_VULKAN
// On Android the OpenXR runtime creates the Vulkan instance and device itself
// (xrCreateVulkanInstanceKHR / xrCreateVulkanDeviceKHR) so it can inject the
// extensions it needs to share swapchain images. The backend therefore adopts
// an existing device instead of opening one, which is the opposite of how the
// D3D12 backend is entered on Windows.
struct EngineOpenParams
{
	VkInstance instance;
	VkPhysicalDevice physicalDevice;
	VkDevice device;
	bool32 fragmentStoresAndAtomicsEnabled;
	uint32 queueFamilyIndex;
	VkQueue queue;

	// Colour target description owned by the OpenXR layer. viewCount is 2 for
	// the multiview stereo path and 1 for any mono pass.
	uint32 width;
	uint32 height;
	float32 renderScaleEffectivePercent;
	// Optional private scene target. Phase-one temporal diagnostics keep it at
	// native resolution; future SGSR2 modes may render it below the OpenXR
	// output. Zero preserves the original native-resolution path.
	uint32 sceneWidth;
	uint32 sceneHeight;
	uint32 sgsrMode;
	// Requested scene raster sample count. The backend validates it against
	// colour and depth limits and falls back to one sample if unsupported.
	uint32 sceneSampleCount;
	// How far the finished frame is reduced before the vehicle reflection
	// samples it: 2, 4 or 8. One reads the full image, which is what the
	// reflection did before the reduced copy existed. Anything else
	// falls back to 4.
	uint32 sceneReflectionDivisor;
	uint32 viewCount;
	VkFormat colourFormat;
};
#endif

namespace vulkan {

void registerPlatformPlugins(void);
void registerNativeRaster(void);
extern int32 nativeRasterOffset;

Raster *rasterCreate(Raster *raster);
uint8 *rasterLock(Raster *raster, int32 level, int32 lockMode);
void rasterUnlock(Raster *raster, int32 level);
int32 rasterNumLevels(Raster *raster);
bool32 rasterHasGeneratedMips(Raster *raster);
bool32 imageFindRasterFormat(Image *image, int32 type,
                             int32 *width, int32 *height,
                             int32 *depth, int32 *format);
bool32 rasterFromImage(Raster *raster, Image *image);
Image *rasterToImage(Raster *raster);
void setRasterHasAlpha(Raster *raster, bool32 hasAlpha);
bool32 rasterHasAlpha(Raster *raster);

// Vehicle alpha atomics can contain both opaque bodywork and transparent
// side-window meshes.  The game scopes this flag to its vehicle alpha list so
// the Vulkan object pipeline can keep depth writes for the body while leaving
// the glass out of the depth buffer.
void setVehicleAlphaPass(bool32 enabled);
// Marks the synchronous entity/clump submission as a moving game object.
// Used only by the optional temporal diagnostic path.
void setDynamicObjectPass(bool32 enabled);

// Compressed uploads. Adreno exposes ASTC and ETC2 natively but not BC/DXT,
// which is what every Vice City TXD actually contains, so the DXT path has to
// transcode on the way in. See vkraster.cpp.
bool32 allocateCompressed(Raster *raster, int32 dxt, int32 numLevels,
                          bool32 hasAlpha);
bool32 deviceSupportsBC(void);

// Bytes currently held by texture images.
size_t getTextureMemoryUsed(void);

void *destroyNativeData(void *object, int32 offset, int32 size);

// ---------------------------------------------------------------------------
// Object pipeline
// ---------------------------------------------------------------------------

struct InstanceData
{
	uint32 numIndex;
	uint32 minVert;
	int32 numVertices;
	Material *material;
	bool32 vertexAlpha;
	uint32 offset;		// byte offset into the index buffer
};

struct InstanceDataHeader : rw::InstanceDataHeader
{
	uint32 serialNumber;
	uint32 numMeshes;
	uint32 totalNumIndex;
	uint32 totalNumVertex;
	uint32 primType;	// VkPrimitiveTopology

	uint16 *indexBuffer;	// CPU copies, kept so a mesh change can reinstance
	uint8 *vertexBuffer;

#ifdef RW_VULKAN
	VkBuffer vbo;
	VkDeviceMemory vboMemory;
	VkBuffer ibo;
	VkDeviceMemory iboMemory;
#else
	void *vbo, *vboMemory, *ibo, *iboMemory;
#endif

	InstanceData *inst;
};

class ObjPipeline : public rw::ObjPipeline
{
public:
	void init(void);
	static ObjPipeline *create(void);

	void (*instanceCB)(Geometry *geo, InstanceDataHeader *header, bool32 reinstance);
	void (*uninstanceCB)(Geometry *geo, InstanceDataHeader *header);
	void (*renderCB)(Atomic *atomic, InstanceDataHeader *header);
};

void defaultInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance);
void defaultUninstanceCB(Geometry *geo, InstanceDataHeader *header);
void defaultRenderCB(Atomic *atomic, InstanceDataHeader *header);
void freeInstanceData(Geometry *geometry);
void *destroyNativeGeometryData(void *object, int32 offset, int32 size);

ObjPipeline *makeDefaultPipeline(void);

// Skinned geometry. Declared here, next to the default pipeline, so skin.cpp
// can reach it the same way it reaches every other platform's.
void initSkin(void);
ObjPipeline *makeSkinPipeline(void);

// Environment-mapped materials (vehicle bodies). Same arrangement for
// matfx.cpp.
void initMatFX(void);
ObjPipeline *makeMatFXPipeline(void);

// Immediate-mode vertex layouts. Kept backend-owned, matching how the D3D12
// backend defines its own rather than borrowing the D3D9 ABI.
struct Im3DVertex
{
	V3d position;
	uint32 color;
	float32 u, v;

	void setX(float32 value) { position.x = value; }
	void setY(float32 value) { position.y = value; }
	void setZ(float32 value) { position.z = value; }
	void setColor(uint8 r, uint8 g, uint8 b, uint8 a) {
		color = ((uint32)a << 24) | ((uint32)b << 16) |
		        ((uint32)g << 8) | (uint32)r;
	}
	void setU(float32 value) { u = value; }
	void setV(float32 value) { v = value; }

	float32 getX(void) { return position.x; }
	float32 getY(void) { return position.y; }
	float32 getZ(void) { return position.z; }
	RGBA getColor(void) {
		return makeRGBA(color & 0xFF, (color >> 8) & 0xFF,
		                (color >> 16) & 0xFF, (color >> 24) & 0xFF);
	}
	float32 getU(void) { return u; }
	float32 getV(void) { return v; }
};

struct Im2DVertex
{
	float32 x, y, z, w;
	uint32 color;
	float32 u, v;

	void setScreenX(float32 value) { x = value; }
	void setScreenY(float32 value) { y = value; }
	void setScreenZ(float32 value) { z = value; }
	void setCameraZ(float32 value) { w = value; }
	void setRecipCameraZ(float32 value) { w = 1.0f/value; }
	void setColor(uint8 r, uint8 g, uint8 b, uint8 a) {
		color = ((uint32)a << 24) | ((uint32)b << 16) |
		        ((uint32)g << 8) | (uint32)r;
	}
	void setU(float32 value, float32) { u = value; }
	void setV(float32 value, float32) { v = value; }

	float32 getScreenX(void) { return x; }
	float32 getScreenY(void) { return y; }
	float32 getScreenZ(void) { return z; }
	float32 getCameraZ(void) { return w; }
	float32 getRecipCameraZ(void) { return 1.0f/w; }
	RGBA getColor(void) {
		return makeRGBA(color & 0xFF, (color >> 8) & 0xFF,
		                (color >> 16) & 0xFF, (color >> 24) & 0xFF);
	}
	float32 getU(void) { return u; }
	float32 getV(void) { return v; }
};

// ---------------------------------------------------------------------------
// Frame interface driven by the OpenXR layer.
//
// The Vulkan backend does not own a swapchain: OpenXR hands it a colour image
// per frame. beginFrame binds that image and opens the multiview render pass;
// endFrame closes it and submits. Everything the game draws in between goes to
// both eyes in a single pass.
// ---------------------------------------------------------------------------
bool32 beginFrame(VkImage colourImage, VkImageView colourView);
void endFrame(void);

// Native profiler support.  GPU time is measured by two Vulkan timestamp
// queries around the complete multiview render pass and resolve.  The value
// returned is from the latest completed submission, never a CPU-frame-time
// estimate. Unsupported devices return false from getGpuFrameTimeMs.
void setGpuFrameTimingEnabled(bool32 enabled);
bool32 getGpuFrameTimeMs(float32 *milliseconds);

// True only when the most recent DEVICEOPEN failed while creating one of the
// full-resolution colour/depth targets or its framebuffer. This lets the
// Android shell safely distinguish a render-scale recovery case from an
// unrelated game-data or plugin initialisation failure.
bool32 didLastDeviceOpenFailForRenderTarget(void);

// Per-eye view-projection matrices for the multiview uniform block.
void setStereoViewProjection(const float32 left[16], const float32 right[16]);
// Horizontal OpenXR eye FOV used by SGSR2's depth-disocclusion tolerance.
// Kept separate because setStereoViewProjection receives projection*view,
// from which the projection scale cannot be recovered after head rotation.
void setSgsrHorizontalFovDegrees(float32 degrees);

// Places the Im2D screen plane in the world. Maps screen pixel coordinates to
// a quad in front of the head, so both eyes project it and it converges.
// planeDistance is where that plane sits and eye the viewer position, both in
// play space: they let a screen vertex carrying its own camera depth be put
// back at its true world position instead of flattened onto the plane.
void setIm2DTransform(const float32 transform[16], float32 planeDistance,
                      const float32 eye[3]);

// Overrides the plane world sprites (vertices carrying a real camera depth:
// coronas, flares, the aim marker) land on. The interface keeps the plane
// from setIm2DTransform. Lets the app roll the HUD out while the sprites keep
// the full head frame the game projected them through. Until it is called
// the sprites follow setIm2DTransform.
void setIm2DSpriteTransform(const float32 transform[16]);

// Scales immediate-mode UI vertices around the render-target centre. The game
// enables this only while drawing the immersive gameplay interface, keeping
// world-space sprites and theater/menu frames on their exact projection.
void setIm2DSafeAreaScale(float32 scale);

// Wrist panels.
//
// Interface that is not part of the head-locked plane: it renders into small
// private textures, before the frame's multiview pass opens, and the game
// hangs one textured quad each off the player's wrists. Being real geometry
// out there, they are depth tested against the arm and the scene.
//
// setWristPanelRenderer installs the callback that issues a panel's Im2D
// draws. It is invoked with that panel's target open and the model slot
// mapping screen pixels straight onto it, so the same code that draws the flat
// interface draws this one. The request is one-shot: the game re-arms it every
// frame the panel is visible, so a paused or menu frame stops rendering it.
enum {
	WRIST_PANEL_MAP = 0,
	WRIST_PANEL_STATUS,
	WRIST_PANEL_CLOCK,
	WRIST_PANEL_AMMO,
	WRIST_PANEL_COUNT
};
void setWristPanelRenderer(int32 panel, void (*renderer)(void));
void setWristPanelWanted(int32 panel, bool32 wanted);
// The finished panel. Null until its first offscreen render has completed.
Raster *getWristPanelRaster(int32 panel);
void setIm2DSafeAreaTransform(float32 scaleX, float32 scaleY,
                              float32 offsetX, float32 offsetY);

// Radar tiles use the original RenderWare depth-mask trick: four invisible
// corner fans write depth, then the map is accepted only inside the circle.
// The role is supplied per immediate-mode draw by the game renderer.

// Suspends the safe-area remap for the draws in between. The wrist minimap
// quad carries a transform of its own onto the arm, and scaling its screen
// coordinates around the panel centre would drag it off the wrist.
void setIm2DSafeAreaSuspended(bool32 suspended);
void setImmediate2DStrictDepth(bool32 enabled);
bool32 getImmediate2DStrictDepth(void);

// Mid-eye pose in play space this frame: position plus yaw about the vertical
// (0 = play forward -Z, positive turning left). The world transform pins the
// anchor to it, so turning in place pivots around the player's head instead
// of around the Guardian origin.
void setHeadPose(const float32 position[3], float32 yaw,
                 const float32 quat[4]);

// The current first person VIEW expressed in game world axes: where the
// player is actually looking, head orientation composed onto the anchor.
// Fails (returns 0) when first person is inactive. rwRight follows the
// RenderWare camera convention (it points to the viewer's LEFT), matching
// what BeginEye writes into the camera frame on the desktop.
bool32 getFirstPersonViewFrame(float32 rwRight[3], float32 rwUp[3],
                               float32 rwAt[3], float32 position[3]);

// Raw predicted OpenXR eye poses for the current immersive frame. They are
// converted through the same first-person anchor as the centre HMD frame so
// game-side systems can build independent left/right camera contexts.
void setFirstPersonEyePoses(const float32 positions[2][3],
                            const float32 orientations[2][4]);
void clearFirstPersonEyePoses(void);
bool32 getFirstPersonEyeViewFrame(uint32 eye, float32 rwRight[3],
                                  float32 rwUp[3], float32 rwAt[3],
                                  float32 position[3]);

// Converts an OpenXR pose expressed in the current play space into Vice
// City's world. Axes are the controller's local +X, +Y and forward -Z. This
// uses the exact same first-person anchor as world rendering, keeping native
// Quest hands rigidly registered with both eyes and the game world.
bool32 playPoseToFirstPersonWorld(const float32 playPosition[3],
                                  const float32 playQuaternion[4],
                                  float32 worldRight[3],
                                  float32 worldUp[3],
                                  float32 worldForward[3],
                                  float32 worldPosition[3]);

// Converts a direction from Vice City's world axes back into the current
// OpenXR play-space axes. Translation is deliberately ignored. Physical
// controls use this to freeze a reference frame that cannot be moved by the
// animated vehicle or first-person anchor later in the grab.
bool32 firstPersonWorldVectorToPlay(const float32 worldVector[3],
                                    float32 playVector[3]);

// The exact inverse of playPoseToFirstPersonWorld's position mapping: hands a
// world position the game produced back to the OpenXR-space layers.
bool32 firstPersonWorldPositionToPlay(const float32 worldPosition[3],
                                      float32 playPosition[3]);

// Tests a game-world bounding sphere against the exact asymmetric OpenXR
// projection of both eyes. Returns true when either eye can see any part of
// the sphere, and fails open while first-person/stereo state is unavailable.
// angularMarginTangent covers late head motion and imperfect legacy bounds
// without reverting to the old very wide centre-eye cone.
bool32 isFirstPersonWorldSphereVisibleInStereo(
    const float32 worldCentre[3], float32 radius,
    float32 angularMarginTangent);
// Tighter building test than a bounding sphere. The eight points are an
// oriented world-space box; visibility is the union of both physical eyes.
bool32 isFirstPersonWorldBoxVisibleInStereo(
    const float32 worldCorners[8][3], float32 angularMarginTangent);

// First person: anchor the view on the player's head in the game world
// instead of on the follow camera. headingYaw is the character's (or the
// vehicle's) facing in world (radians, atan2(fwd.y, fwd.x)). On activation
// the player's current physical facing is latched to it, so they start
// looking the way the character faces. With followHeading set the anchor
// tracks headingYaw every frame -- cockpit behaviour for vehicles -- and a
// change of followHeading re-latches, so entering a car starts looking down
// its nose. Passing active=false falls back to the camera anchor.
void setFirstPersonAnchor(const float32 headWorld[3], float32 headingYaw,
                          bool32 followHeading, bool32 active);

// Vehicle variant of setFirstPersonAnchor. right/up/forward are the
// vehicle's orthonormal world axes. It preserves pitch and roll while still
// applying the physical-yaw latch used by cockpit mode.
void setFirstPersonAnchorBasis(const float32 headWorld[3],
                               const float32 right[3],
                               const float32 up[3],
                               const float32 forward[3],
                               float32 headingYaw, bool32 active);

// View direction in game-world yaw while first person is active. Returns 0
// when inactive. Lets the pad layer make stick movement view-relative.
bool32 getFirstPersonViewYaw(float32 *yawOut);

// HMD yaw relative to the facing direction latched when first-person mode was
// entered or recentered. This is the stable basis used by head-relative
// locomotion; vehicle heading and world heading are deliberately excluded.
bool32 getFirstPersonLocalHeadYaw(float32 *yawOut);

// Sky colour for the frame; used as the render pass clear colour so the sky
// covers the full view rather than the 2D panel.
void setClearColour(uint8 red, uint8 green, uint8 blue);

// Distance fog for world, skinned and im3d geometry. start is where it
// begins and end is the far clip it reaches full strength at; the colour
// comes from the FOGCOLOR render state the game already sets each frame.
void setFogParams(float32 start, float32 end);

// Final-image colour treatment. Mode 0 is a plain resolve and mode 1 is Vice
// City's POSTFX_NORMAL filter. The filter runs once for both multiview layers
// after all world and 2D rendering has completed.
void setPostFx(uint32 mode, uint32 red, uint32 green, uint32 blue,
               float32 intensity);
void setFxaaEnabled(bool32 enabled);
void setSpatialAaMode(uint32 mode);
// Build a mip chain for textures whose TXD shipped a single level.
// Read when a raster is created, so it applies to what streams in next.
void setGenerateMipmaps(bool32 enabled);
// The PS2 two-pass alpha rule. On by default: without it masked
// geometry writes depth for colour it never draws.
void setPs2AlphaTestEnabled(bool32 enabled);
bool32 getPs2AlphaTestEnabled(void);
// The game's dynamic light list for this frame, world space, applied per
// pixel by the world, skin and vehicle pipelines. The fixed-function original
// only ever bent these into extra directionals for peds and vehicles; the
// backend keeps the same falloff and cone rules but lets the world receive
// them too. Push zero lights to switch the effect off; anything past
// MAX_POINT_LIGHTS is dropped, so hand over the nearest ones first.
// glowStrength additionally draws each light as a soft cloud where the view
// ray passes near it, so a source reads in the air and not only as a pool on
// surfaces; zero disables the term, 0.14 is the tuned baseline.
enum { MAX_POINT_LIGHTS = 8 };
struct PointLight
{
	float32 position[3];
	float32 radius;
	float32 colour[3];
	// A cone along direction, opened the way CPointLights' directional
	// lights are; direction is ignored for omni lights.
	bool32 spot;
	float32 direction[3];
};
void setPointLights(const PointLight *lights, uint32 count,
                    float32 glowStrength);
// Reflections on materials that carry a MatFX env map -- vehicle
// bodywork and chrome. The previous frame is reprojected onto the body,
// over a panorama of the streak art the DFF references where that
// lookup has no answer. The original drew these with a second textured
// pass this backend never implemented, so without it they have none.
void setCarReflectionsEnabled(bool32 enabled);
// How strong the reflection layer reads. intensity scales the whole env
// addition -- 1.0 is the tuned baseline, lower takes the glassy sheen off
// the paint. ssrStrength scales only the previous-frame lookup's weight
// within it: 1.0 is the baseline, 0 removes the live surroundings entirely
// and leaves every car on the panorama, the way the original's env pass
// behaved. ssrDistance keeps that live part within so many metres of the
// eye, fading to the panorama past it; pass something huge for no limit.
void setCarReflectionParams(float32 intensity, float32 ssrStrength,
                            float32 ssrDistance);
// Modern water: per-pixel ripples, Fresnel, the same previous-frame
// reflection the cars use, and a sun glint, on the water immediate-mode
// draws. The game marks those draws through setIm3DWater (driven by the
// port's render-command role) and the whole effect costs one integer test
// per pixel when disabled.
void setModernWaterEnabled(bool32 enabled);
void setIm3DWater(bool32 water);
// The WATER settings page, as multipliers of the shader's base recipe:
// wave steepness, scroll speed, how far the ripples distort the mirrored
// picture, the mirror's strength, the flat-sky fallback, and the sun and
// dynamic-light speculars. 1.0 everywhere is the baseline.
void setWaterParams(float32 waves, float32 speed, float32 distortion,
                    float32 reflection, float32 sheen, float32 glint,
                    float32 sparks);
// Culling visualizer. While active every world draw is replaced by a flat
// debug colour whose index (1 = visible, 2 = frustum-culled, 3 = occluder-
// culled, 4 = distance/LOD) the caller sets per entity with setDebugTint.
#if defined(MIAMIVR_DEV_TOOLS) && MIAMIVR_DEV_TOOLS
void setDebugVizActive(bool32 active);
void setDebugTint(uint32 index);
#else
inline void setDebugVizActive(bool32) {}
inline void setDebugTint(uint32) {}
#endif
// Game-time seconds for the ripple animation -- game time, so water freezes
// with the pause menu -- and the sun the glint reflects, world space with a
// 0-255 colour.
void setEffectsTime(float32 seconds);
void setWaterSun(const float32 directionWorld[3], float32 red, float32 green,
                 float32 blue);
// The vehicle the player currently occupies, world space, with its model
// bounding radius. Its atomics keep the panorama reflection layer only:
// from the driver's seat a screen-space lookup mostly finds the car itself,
// and the cockpit magnifies every artefact of it. The radius matters -- a
// fixed reach missed the bumper and door atomics of long vehicles, whose
// centres sit further out, and those parts kept flashing live reflections.
// Pass active = 0 on foot.
void setPlayerVehicle(const float32 position[3], float32 radius,
                      bool32 active);
// One line saying how many textures got a chain and what turned the rest
// away. Silent unless diagnostics are switched on: a player's logcat is
// not the place for it.
void reportGeneratedMips(void);
void setRenderDiagnostics(bool32 enabled);
// How far down the mip chain masked geometry is pushed, in half
// levels. Foliage sparkles because its mask is finer than a pixel;
// this trades that detail for stillness.
void setMaskedMipBias(uint32 halfLevels);
uint32 getMaskedMipBias(void);
bool32 renderDiagnosticsEnabled(void);
// Uploads whatever the mip worker has finished. Call once per frame
// from the thread that owns the command buffer.
void uploadFinishedMips(void);
// Joins the mip worker and drops anything still queued.
void stopGeneratedMips(void);

enum SgsrMode {
	SGSR_OFF = 0,
	// Diagnostic foundation for SGSR2. Displays per-eye camera motion
	// reconstructed from depth; it does not upscale or accumulate history.
	SGSR2_MOTION_DEBUG,
	// Native-resolution temporal stabilizer. Reprojects the preceding colour
	// frame with the same per-eye camera/object vectors used by the diagnostic
	// view. It is deliberately separate from resolution scaling and remains an
	// opt-in experiment until headset ghosting/disocclusion tests pass.
	SGSR2_TEMPORAL_STABILIZER,
	// Two complementary sub-pixel projection samples resolved across adjacent
	// raw frames. This remains isolated from the proven stabilizer and OFF.
	SGSR2_JITTERED_TAA_2X,
	// Stable resolved-history experiment. Unlike V2 it retains the temporally
	// resolved output, but deliberately applies no camera-projection jitter:
	// the four-phase version made the entire VR world visibly tremble.
	SGSR2_RESOLVED_TEMPORAL_V3,
	// Official Qualcomm SGSR2 two-fragment-pass temporal upscaler. The OpenXR
	// output remains at the requested scale while the 3D scene is rendered at
	// 80% linear resolution (125% output therefore costs roughly a 100% scene).
	SGSR2_OFFICIAL_QUALITY,
	SGSR_MODE_COUNT,
};

// Reports the active temporal startup configuration. Changing Sgsr2Mode in
// the INI requires a restart because private targets are allocated during
// DEVICEOPEN.
bool32 getSgsrStatus(uint32 *mode, uint32 *sceneWidth, uint32 *sceneHeight,
                     uint32 *outputWidth, uint32 *outputHeight);
uint32 getSceneSampleCount(void);

// Texture straight from DXT blocks (BC formats), no CPU decode. Returns nil
// when the device lacks BC support or the type is not DXT1/3/5; the caller
// falls back to software decoding.
Raster *rasterFromDXT(int32 width, int32 height, int32 dxt, bool32 hasAlpha,
                      const uint8 *blocks, uint32 size,
                      const uint32 *levelSizes, int32 txdLevels);

// Immediate mode, implemented in vkim.cpp and wired into the device table.
void im2DRenderLine(void *vertices, int32 numVertices, int32 vert1, int32 vert2);
void im2DRenderTriangle(void *vertices, int32 numVertices, int32 vert1,
                        int32 vert2, int32 vert3);
void im2DRenderPrimitive(PrimitiveType primType, void *vertices, int32 numVertices);
void im2DRenderIndexedPrimitive(PrimitiveType primType, void *vertices,
                                int32 numVertices, void *indices, int32 numIndices);
void im3DTransform(void *vertices, int32 numVertices, Matrix *world, uint32 flags);
void im3DRenderPrimitive(PrimitiveType primType);
void im3DRenderIndexedPrimitive(PrimitiveType primType, void *indices,
                                int32 numIndices);
void im3DEnd(void);

bool32 isInitialized(void);
const char *getAdapterName(void);
bool32 waitForGpu(void);

extern Device renderdevice;

}
}
