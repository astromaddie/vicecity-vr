#pragma once

struct android_app;

#include <vulkan/vulkan.h>

// OpenXR session bound to a Vulkan device, targeting a single two-layer
// swapchain rendered with VK_KHR_multiview.
//
// This is the platform floor the ported VR layer sits on. The desktop build
// binds OpenXR to D3D12 and resolves a double-wide target; here the equivalent
// role is played by an array target plus multiview, which is the arrangement
// Quest's runtime and tiler are built around.
namespace xrvk {

// Creates the loader, instance, system, Vulkan device and session. Safe to
// call once the activity exists; does not require a window surface.
bool create(android_app *app);
void destroy(void);

// Drains the OpenXR event queue and tracks session state. Returns false when
// the runtime has asked the application to exit.
bool pollEvents(void);

// Fires a vibration burst on one controller (0 = left, 1 = right).
// Amplitude 0..1, frequency in Hz, duration in milliseconds. Safe to call
// any time; quietly does nothing before the session is focused.
void triggerHaptic(int hand, float amplitude, float frequencyHz, float durationMs);

// Sets the preferred display refresh rate (Hz). The session requests the
// closest rate the runtime offers and keeps retrying until confirmed. Safe
// to call any time.
void setPreferredDisplayRefreshRate(float hz);

// OpenXR performance hint for the standalone runtime. Auto leaves a fresh
// session entirely under runtime control; the two explicit modes provide
// performance-policy hints for profiling and demanding traffic scenes.
enum PerformanceMode
{
	PERFORMANCE_MODE_AUTO = 0,
	PERFORMANCE_MODE_SUSTAINED_HIGH = 1,
	PERFORMANCE_MODE_BOOST = 2,
};
void setPerformanceMode(int mode);
int getPerformanceMode(void);
// Independent GPU clock-policy hint. BOOST is persistent for the session;
// the Quest runtime remains free to thermally throttle it.
void setGpuPerformanceMode(int mode);
int getGpuPerformanceMode(void);
int getActiveGpuPerformanceMode(void);
bool isPerformanceModeSupported(void);

bool isPerformanceBoostBlocked(void);
// Effective hint in this session, or -1 when no complete hint was applied.
// This can differ from getPerformanceMode(): OpenXR has no "clear hint" call,
// so returning to Auto after an explicit mode needs a session restart to
// restore the runtime's true default policy.
int getActivePerformanceMode(void);

// True once the session reached a state in which frames must be submitted.
bool shouldRender(void);

// True only inside the current immersive game frame with a focused session
// and freshly tracked position AND orientation. Rendering may deliberately
// continue with a stale position; that pose must not become a new seat origin.
bool hasTrackedGameplayHeadPose(void);

// One xrWaitFrame / xrBeginFrame / xrEndFrame cycle.
void renderFrame(void);

// The Vulkan objects the runtime created, which librw's backend adopts.
struct GraphicsContext
{
	VkInstance instance;
	VkPhysicalDevice physicalDevice;
	VkDevice device;
	bool fragmentStoresAndAtomicsEnabled;
	VkQueue queue;
	unsigned int queueFamilyIndex;
	unsigned int width;
	unsigned int height;
	float renderScaleEffectivePercent;
	unsigned int viewCount;
	VkFormat colourFormat;
};

// Valid only after create() succeeded.
bool getContext(GraphicsContext *out);

// The menu value is only a request. OpenXR may clamp it to the runtime's
// maximum image extent or reject the large swapchain and make startup select
// a lower preset. Keep all three values visible so UI and profiler output do
// not claim 150% while the submitted image is actually smaller.
enum RenderScaleFallbackReason
{
	RENDER_SCALE_FALLBACK_NONE = 0,
	RENDER_SCALE_FALLBACK_RUNTIME_LIMIT = 1,
	RENDER_SCALE_FALLBACK_SWAPCHAIN_ALLOCATION = 2,
	RENDER_SCALE_FALLBACK_GAME_RENDERER_ALLOCATION = 3,
};

struct RenderScaleStatus
{
	int requestedPercent;
	int selectedPresetPercent;
	float effectivePercent;
	unsigned int recommendedWidth;
	unsigned int recommendedHeight;
	unsigned int actualWidth;
	unsigned int actualHeight;
	unsigned int runtimeMaxWidth;
	unsigned int runtimeMaxHeight;
	int fallbackReason;
	// Survives the recovery launch after a high-resolution game-renderer
	// allocation failed and RenderScalePercent had to be reset to 100.
	int previousFallbackRequestedPercent;
	int previousFallbackPercent;
	int previousFallbackReason;
};

bool getRenderScaleStatus(RenderScaleStatus *out);
const char *getRenderScaleFallbackReasonName(int reason);
// Called only after librw has successfully allocated its private scene/depth
// targets. A high-resolution swapchain alone is not proof that the complete
// render configuration started successfully.
void confirmRenderScaleRendererReady(void);

// Snapshot of the tracked controllers, refreshed once per frame while the
// session holds focus. Axes are -1..1, triggers and grips 0..1.
struct TrackedPose
{
	float position[3];
	float orientation[4];
	bool valid;
};

struct ControllerInput
{
	float leftStickX, leftStickY;
	float rightStickX, rightStickY;
	float leftTrigger, rightTrigger;
	float leftGrip, rightGrip;
	bool a, b, x, y;
	bool menu;
	bool leftStickClick, rightStickClick;
	TrackedPose gripPose[2];
	TrackedPose aimPose[2];
#if MIAMIVR_STEAM_FRAME
	// Steam Frame controller: left View, the left D-pad, and a system
	// recentre (XrEventDataReferenceSpaceChangePending) latched until read.
	bool view;
	bool dpadUp, dpadDown, dpadLeft, dpadRight;
	bool leftBumper, rightBumper;
	bool systemRecentre;
#endif
};

void getInput(ControllerInput *out);

// Draws the contents of one frame, called between swapchain image acquire and
// release with the image already waited on. viewProj holds the per-eye
// view-projection matrices, column-major, indexed by view.
//
// Keeping this a callback is what stops the OpenXR layer from having to know
// about librw or the game at all; when none is installed the bring-up scene is
// drawn instead.
// im2dWorld maps screen pixel coordinates onto a quad placed in front of the
// head, so 2D content can be projected per eye instead of written straight to
// clip space.
// headPos is the mid-eye position in play space this frame; headYaw is the
// head's rotation about the vertical (radians, 0 = play-space forward -Z,
// positive turning left). The renderer anchors the game world to the head,
// so turning in place pivots around the player's own head rather than around
// wherever the Guardian origin happens to be in the room.
// headQuat is the full head orientation (x,y,z,w) and eyeFovDeg the
// horizontal field of view of an eye in degrees; both feed the game's own
// camera and sprite mathematics the way the desktop VR layer does.
// im2dDistance is where that plane sits, needed to turn a vertex's own depth
// back into a world position.
typedef void (*FrameRenderer)(VkImage image, VkImageView view,
                              const float viewProj[2][16],
                              const float im2dWorld[16], float im2dDistance,
                              const float headPos[3], float headYaw,
                              const float headQuat[4], float eyeFovDeg,
                              const float eyePos[2][3],
                              const float eyeQuat[2][4]);
void setFrameRenderer(FrameRenderer renderer);

// Selects how the frame just rendered is submitted. Immersive gameplay uses
// the stereo projection layer; frontend and cinematic frames use array layer
// zero as one shared image on a world-locked cinema quad.
void setTheaterMode(bool enabled);

// The theater screen's size in metres; it hangs 2 m in front of the head.
constexpr float kTheaterScreenWidth = 2.56f;
constexpr float kTheaterScreenHeight = 1.44f;

#if MIAMIVR_STEAM_FRAME
// 3D cinema: show each eye its own array layer on the theater screen instead
// of layer 0 to both. Takes effect with the next submitted frame.
void setTheaterStereo(bool enabled);

// Drops the theater screen's latched placement, so it is set again in front
// of the head on the next theater frame. A recentre of a screen the player
// sits at an angle to would otherwise leave it where it was.
void resetTheaterAnchor(void);
#endif

// Predicted display time of the frame currently being rendered, in
// nanoseconds. Zero before the first frame. Vsync-quantised, which makes it
// the right clock to drive game time from: wall-clock deltas jitter with
// scheduling while frames present on exact display intervals, and the
// difference reads as the world stuttering.
long long getPredictedDisplayTimeNs(void);

// RGBA pixels for the head-locked UI overlay quad, or nullptr to hide it.
// Compact diagnostics/settings use 512x128; full menus use 1024x768.
void setDebugOverlay(const unsigned char *rgba, int width, int height);

// Meta runtime app timings. When XR_META_performance_metrics is exposed these
// are the runtime's own CPU/GPU frame times, sampled after xrEndFrame. The
// Vulkan backend also keeps an independent timestamp pair for validation and
// as a fallback on runtimes without the extension.
struct PerformanceMetrics
{
	float appCpuFrameMs;
	float appGpuFrameMs;
	float displayRefreshRateHz;
	bool appCpuFrameValid;
	bool appGpuFrameValid;
};
void setPerformanceMetricsEnabled(bool enabled);
bool getPerformanceMetrics(PerformanceMetrics *out);

} // namespace xrvk
