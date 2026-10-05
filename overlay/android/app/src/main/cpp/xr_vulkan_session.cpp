#include "xr_vulkan_session.h"
#include "platform_android.h"
// For the game camera's view window, which the Im2D plane must match.
#include "android.h"
#if MIAMIVR_STEAM_FRAME
#include "frame_diag.h"
#include "sfvr/sfvr.h"
#endif

#include <android_native_app_glue.h>
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <vector>

static const uint32_t kBringupVertSpv[] =
#include "bringup_vert.h"
;
static const uint32_t kBringupFragSpv[] =
#include "bringup_frag.h"
;

#define XR_CHECK(expr, what) \
	do { \
		XrResult xrCheckResult = (expr); \
		if(XR_FAILED(xrCheckResult)){ \
			LOGE("%s failed: %d", what, (int)xrCheckResult); \
			return false; \
		} \
	} while(0)

#define VK_CHECK(expr, what) \
	do { \
		VkResult vkCheckResult = (expr); \
		if(vkCheckResult != VK_SUCCESS){ \
			LOGE("%s failed: %d", what, (int)vkCheckResult); \
			return false; \
		} \
	} while(0)

namespace xrvk {
namespace {

#if MIAMIVR_STEAM_FRAME
// Standard section 9: 120 Hz is the Frame target. SteamVR's per-app preferred
// rate decides what it really runs at; see requestPreferredDisplayRefreshRate.
static constexpr float kTargetDisplayRefreshRateHz = 120.0f;
#else
static constexpr float kTargetDisplayRefreshRateHz = 72.0f;
#endif

// ---------------------------------------------------------------------------
// Minimal column-major 4x4 maths. Column-major is what GLSL consumes directly,
// so the uniform block needs no transpose on upload.
// ---------------------------------------------------------------------------

struct Mat4 { float m[16]; };

Mat4
identity(void)
{
	Mat4 r = {};
	r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
	return r;
}

Mat4
multiply(const Mat4 &a, const Mat4 &b)
{
	Mat4 r;
	for(int col = 0; col < 4; col++)
		for(int row = 0; row < 4; row++){
			float sum = 0.0f;
			for(int k = 0; k < 4; k++)
				sum += a.m[k*4 + row] * b.m[col*4 + k];
			r.m[col*4 + row] = sum;
		}
	return r;
}

// Asymmetric off-axis projection for a Vulkan clip volume: depth 0..1 and a
// downward Y axis. The four OpenXR angles are signed half-angles from the
// view direction, so tangents cannot be assumed symmetric on Quest optics.
Mat4
projectionFromFov(const XrFovf &fov, float nearZ, float farZ)
{
	const float tanLeft = tanf(fov.angleLeft);
	const float tanRight = tanf(fov.angleRight);
	const float tanUp = tanf(fov.angleUp);
	const float tanDown = tanf(fov.angleDown);

	const float tanWidth = tanRight - tanLeft;
	const float tanHeight = tanDown - tanUp;	// flipped for Vulkan

	Mat4 r = {};
	r.m[0]  = 2.0f / tanWidth;
	r.m[5]  = 2.0f / tanHeight;
	r.m[8]  = (tanRight + tanLeft) / tanWidth;
	r.m[9]  = (tanUp + tanDown) / tanHeight;
	r.m[10] = -farZ / (farZ - nearZ);
	r.m[11] = -1.0f;
	r.m[14] = -(farZ * nearZ) / (farZ - nearZ);
	return r;
}

// Rigid inverse of the view pose: transpose the rotation, then rotate the
// negated translation by it.
Mat4
viewFromPose(const XrPosef &pose)
{
	const XrQuaternionf &q = pose.orientation;
	const float x = q.x, y = q.y, z = q.z, w = q.w;

	const float r00 = 1.0f - 2.0f*(y*y + z*z);
	const float r01 = 2.0f*(x*y - z*w);
	const float r02 = 2.0f*(x*z + y*w);
	const float r10 = 2.0f*(x*y + z*w);
	const float r11 = 1.0f - 2.0f*(x*x + z*z);
	const float r12 = 2.0f*(y*z - x*w);
	const float r20 = 2.0f*(x*z - y*w);
	const float r21 = 2.0f*(y*z + x*w);
	const float r22 = 1.0f - 2.0f*(x*x + y*y);

	const float px = pose.position.x, py = pose.position.y, pz = pose.position.z;

	Mat4 r = {};
	// Transposed rotation, written column-major.
	r.m[0] = r00; r.m[1] = r01; r.m[2] = r02;
	r.m[4] = r10; r.m[5] = r11; r.m[6] = r12;
	r.m[8] = r20; r.m[9] = r21; r.m[10] = r22;
	r.m[12] = -(r00*px + r10*py + r20*pz);
	r.m[13] = -(r01*px + r11*py + r21*pz);
	r.m[14] = -(r02*px + r12*py + r22*pz);
	r.m[15] = 1.0f;
	return r;
}

// Rigid head transform from a pose: rotation, then translation.
Mat4
worldFromPose(const XrPosef &pose)
{
	const XrQuaternionf &q = pose.orientation;
	const float x = q.x, y = q.y, z = q.z, w = q.w;

	Mat4 r = {};
	r.m[0] = 1.0f - 2.0f*(y*y + z*z);
	r.m[1] = 2.0f*(x*y + z*w);
	r.m[2] = 2.0f*(x*z - y*w);
	r.m[4] = 2.0f*(x*y - z*w);
	r.m[5] = 1.0f - 2.0f*(x*x + z*z);
	r.m[6] = 2.0f*(y*z + x*w);
	r.m[8] = 2.0f*(x*z + y*w);
	r.m[9] = 2.0f*(y*z - x*w);
	r.m[10] = 1.0f - 2.0f*(x*x + y*y);
	r.m[12] = pose.position.x;
	r.m[13] = pose.position.y;
	r.m[14] = pose.position.z;
	r.m[15] = 1.0f;
	return r;
}

// The head orientation with its roll taken out: yaw about +Y, then pitch about
// the resulting right axis. Looking straight up or down leaves yaw undefined,
// so the last good one is held through that.
XrQuaternionf
rollFreeOrientation(const XrQuaternionf &q, float *yawHold)
{
	const float fx = -(2.0f*(q.x*q.z + q.y*q.w));
	const float fy = -(2.0f*(q.y*q.z - q.x*q.w));
	const float fz = -(1.0f - 2.0f*(q.x*q.x + q.y*q.y));
	if(fx*fx + fz*fz > 0.0025f)
		*yawHold = atan2f(-fx, -fz);
	const float pitch = asinf(fy < -1.0f ? -1.0f : (fy > 1.0f ? 1.0f : fy));
	const float cy = cosf(*yawHold*0.5f), sy = sinf(*yawHold*0.5f);
	const float cx = cosf(pitch*0.5f), sx = sinf(pitch*0.5f);
	XrQuaternionf out;
	out.x = cy*sx;
	out.y = sy*cx;
	out.z = -sy*sx;
	out.w = cy*cx;
	return out;
}

// Maps screen pixel coordinates onto a quad of the given size, centred a fixed
// distance ahead of the head. Screen Y grows downward, hence the negative Y
// scale; the plane sits at -Z because that is forward in OpenXR view space.
Mat4
im2dPlane(const XrPosef &headPose, float screenWidth, float screenHeight,
          float distance, float planeWidth, float planeHeight)
{
	Mat4 plane = {};
	plane.m[0] = screenWidth > 0.0f ? planeWidth / screenWidth : 0.0f;
	plane.m[5] = screenHeight > 0.0f ? -planeHeight / screenHeight : 0.0f;
	plane.m[10] = 1.0f;
	plane.m[12] = -planeWidth * 0.5f;
	plane.m[13] = planeHeight * 0.5f;
	plane.m[14] = -distance;
	plane.m[15] = 1.0f;

	return multiply(worldFromPose(headPose), plane);
}

Mat4
translationScale(float tx, float ty, float tz, float sx, float sy, float sz)
{
	Mat4 r = identity();
	r.m[0] = sx; r.m[5] = sy; r.m[10] = sz;
	r.m[12] = tx; r.m[13] = ty; r.m[14] = tz;
	return r;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

struct Vertex { float position[3]; float normal[3]; };

const Vertex kCubeVertices[] = {
	// +X
	{{ 0.5f,-0.5f,-0.5f},{ 1, 0, 0}}, {{ 0.5f, 0.5f,-0.5f},{ 1, 0, 0}},
	{{ 0.5f, 0.5f, 0.5f},{ 1, 0, 0}}, {{ 0.5f,-0.5f, 0.5f},{ 1, 0, 0}},
	// -X
	{{-0.5f,-0.5f, 0.5f},{-1, 0, 0}}, {{-0.5f, 0.5f, 0.5f},{-1, 0, 0}},
	{{-0.5f, 0.5f,-0.5f},{-1, 0, 0}}, {{-0.5f,-0.5f,-0.5f},{-1, 0, 0}},
	// +Y
	{{-0.5f, 0.5f,-0.5f},{ 0, 1, 0}}, {{-0.5f, 0.5f, 0.5f},{ 0, 1, 0}},
	{{ 0.5f, 0.5f, 0.5f},{ 0, 1, 0}}, {{ 0.5f, 0.5f,-0.5f},{ 0, 1, 0}},
	// -Y
	{{-0.5f,-0.5f, 0.5f},{ 0,-1, 0}}, {{-0.5f,-0.5f,-0.5f},{ 0,-1, 0}},
	{{ 0.5f,-0.5f,-0.5f},{ 0,-1, 0}}, {{ 0.5f,-0.5f, 0.5f},{ 0,-1, 0}},
	// +Z
	{{-0.5f,-0.5f, 0.5f},{ 0, 0, 1}}, {{ 0.5f,-0.5f, 0.5f},{ 0, 0, 1}},
	{{ 0.5f, 0.5f, 0.5f},{ 0, 0, 1}}, {{-0.5f, 0.5f, 0.5f},{ 0, 0, 1}},
	// -Z
	{{ 0.5f,-0.5f,-0.5f},{ 0, 0,-1}}, {{-0.5f,-0.5f,-0.5f},{ 0, 0,-1}},
	{{-0.5f, 0.5f,-0.5f},{ 0, 0,-1}}, {{ 0.5f, 0.5f,-0.5f},{ 0, 0,-1}},
};

const uint16_t kCubeIndices[] = {
	 0, 1, 2,  0, 2, 3,      4, 5, 6,  4, 6, 7,
	 8, 9,10,  8,10,11,     12,13,14, 12,14,15,
	16,17,18, 16,18,19,     20,21,22, 20,22,23,
};

struct PushConstants { Mat4 model; float tint[4]; };

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct SwapchainImage
{
	VkImage image;
	VkImageView view;
	VkFramebuffer framebuffer;
	VkCommandBuffer commandBuffer;
	VkFence fence;
	VkBuffer uniformBuffer;
	VkDeviceMemory uniformMemory;
	void *uniformMapped;
	VkDescriptorSet descriptorSet;
};

struct State
{
	XrInstance instance = XR_NULL_HANDLE;
	XrSystemId systemId = XR_NULL_SYSTEM_ID;
	XrSession session = XR_NULL_HANDLE;
	XrSpace space = XR_NULL_HANDLE;
	XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
	bool running = false;
	bool exitRequested = false;

	XrViewConfigurationView viewConfigs[2];
	// STAGE is preferred because a standing game wants a floor-relative origin,
	// but it depends on a configured play area and stops reporting a valid
	// position without one. LOCAL always tracks, so it is kept as a fallback.
	XrSpace localSpace = XR_NULL_HANDLE;
	XrSpace viewSpace = XR_NULL_HANDLE;

	// Head-locked UI overlay quad. The swapchain is allocated for the largest
	// menu; a smaller imageRect carries the compact diagnostic/settings strip.
	XrSwapchain debugSwapchain = XR_NULL_HANDLE;
	std::vector<VkImage> debugImages;
	int debugWidth = 0, debugHeight = 0;
	int debugContentWidth = 0, debugContentHeight = 0;
	VkBuffer debugStaging = VK_NULL_HANDLE;
	VkDeviceMemory debugStagingMemory = VK_NULL_HANDLE;
	void *debugStagingMapped = nullptr;
	VkCommandBuffer debugCommand = VK_NULL_HANDLE;
	VkFence debugFence = VK_NULL_HANDLE;
	bool debugSubmissionPending = false;
	bool debugVisible = false;
	XrTime lastCompactDebugUploadTime = 0;
	XrSwapchain swapchain = XR_NULL_HANDLE;
	int64_t swapchainFormat = 0;
	// Format of the views and render pass that write the eye images. The
	// same as swapchainFormat, except on the Frame with an sRGB swapchain
	// (see createSwapchain).
	int64_t swapchainViewFormat = 0;
	uint32_t renderWidth = 0;
	uint32_t renderHeight = 0;
	int renderScaleRequestedPercent = 100;
	int renderScaleSelectedPresetPercent = 100;
	float renderScaleEffectivePercent = 100.0f;
	uint32_t renderScaleRecommendedWidth = 0;
	uint32_t renderScaleRecommendedHeight = 0;
	uint32_t renderScaleRuntimeMaxWidth = 0;
	uint32_t renderScaleRuntimeMaxHeight = 0;
	int renderScaleFallbackReason = RENDER_SCALE_FALLBACK_NONE;
	int previousRenderScaleFallbackRequestedPercent = 0;
	int previousRenderScaleFallbackPercent = 0;
	int previousRenderScaleFallbackReason = RENDER_SCALE_FALLBACK_NONE;

	VkInstance vkInstance = VK_NULL_HANDLE;
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	bool fragmentStoresAndAtomicsEnabled = false;
	uint32_t queueFamily = 0;
	VkQueue queue = VK_NULL_HANDLE;

	VkCommandPool commandPool = VK_NULL_HANDLE;
	VkRenderPass renderPass = VK_NULL_HANDLE;
	VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool descriptorPool = VK_NULL_HANDLE;

	VkImage depthImage = VK_NULL_HANDLE;
	VkDeviceMemory depthMemory = VK_NULL_HANDLE;
	VkImageView depthView = VK_NULL_HANDLE;
	VkFormat depthFormat = VK_FORMAT_D24_UNORM_S8_UINT;

	VkBuffer vertexBuffer = VK_NULL_HANDLE;
	VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
	VkBuffer indexBuffer = VK_NULL_HANDLE;
	VkDeviceMemory indexMemory = VK_NULL_HANDLE;

	std::vector<SwapchainImage> images;

	XrTime predictedDisplayTime = 0;
	long long lastPredictedDisplayTimeNs = 0;
	bool hasRefreshRateExt = false;
	PFN_xrEnumerateDisplayRefreshRatesFB enumerateRefreshRates = nullptr;
	PFN_xrGetDisplayRefreshRateFB getRefreshRate = nullptr;
	PFN_xrRequestDisplayRefreshRateFB requestRefreshRate = nullptr;
	bool refreshRateRequestAttempted = false;
	// Keep application layers hidden while the headset panel changes refresh
	// rate. Quest otherwise presents several menu frames at the old frequency.
	bool refreshRateReadyForFrames = false;
	int refreshRateRetryCount = 0;
	XrTime nextRefreshRateRetryTime = 0;
	float currentRefreshRateHz = kTargetDisplayRefreshRateHz;
	// Runtime-configurable target (vr_settings "RefreshRate"); the constant
	// above remains the safe default.
	float targetRefreshRateHz = kTargetDisplayRefreshRateHz;
	// What the last request actually asked for (the rate the display has to
	// reach before layers are released), 0 until one was made. It differs from
	// the target when the runtime offers no rate equal to it.
	float requestedRefreshRateHz = 0.0f;
	// The compositor's real rate, from predictedDisplayPeriod (0 until known).
	float measuredRefreshHz = 0.0f;
#if MIAMIVR_STEAM_FRAME
	// The app cannot change the Frame's refresh rate (SteamVR offers only the
	// title's per-app rate), so at most one request is made per session, and
	// the slow-frame check below only advises.
	bool refreshRequestedThisSession = false;
	bool refreshWarned = false;
	int refreshLowWindows = 0;
	double refreshLowFps = 0.0;
#endif
	float elapsed = 0.0f;

	// XR_EXT_performance_settings supplies hints rather than hard clock locks.
	// Keep the requested menu value separate from the effective hint because
	// the extension deliberately has no API for clearing a hint in-place.
	bool hasPerformanceSettingsExt = false;
	PFN_xrPerfSettingsSetPerformanceLevelEXT setPerformanceLevel = nullptr;
	int requestedPerformanceMode = PERFORMANCE_MODE_SUSTAINED_HIGH;
	int activePerformanceMode = -1;
	// The headset-validated 125% profile holds 72 Hz with sustained CPU/GPU
	// hints. Keep both domains on the thermally sustainable public OpenXR level;
	// Boost remains an explicit short test rather than a shipping default.
	int requestedGpuPerformanceMode = PERFORMANCE_MODE_SUSTAINED_HIGH;
	int activeGpuPerformanceMode = -1;
	bool gpuPerformanceHintSucceededThisSession = false;
	bool cpuPerformanceHintSucceededThisSession = false;
	bool cpuBoostActive = false;
	bool boostThermallyBlocked = false;
	int boostReturnMode = PERFORMANCE_MODE_AUTO;
	XrTime boostExpiryDisplayTime = 0;
	long long boostExpiryMonotonicNs = 0;
	bool performanceAutoLogged = false;

	// This port runs game logic and rendering on the same dedicated thread.
	// XR_KHR_android_thread_settings lets the runtime identify that time-critical
	// CPU thread without the application forcing a Linux scheduling policy.
	bool hasAndroidThreadSettingsExt = false;
	PFN_xrSetAndroidApplicationThreadKHR setAndroidApplicationThread = nullptr;
	uint32_t applicationMainThreadId = 0;
	bool applicationMainThreadRegistered = false;

	FrameRenderer frameRenderer = nullptr;
	bool headPoseTrackedForFrame = false;
	bool theaterMode = true;
#if MIAMIVR_STEAM_FRAME
	// 3D cinema: the theater screen shows each eye its own array layer.
	bool theaterStereo = false;
#endif
	bool theaterAnchorValid = false;
	XrSpace theaterSpace = XR_NULL_HANDLE;
	XrPosef theaterPose = {};
	// Yaw the roll-free HUD plane last had a defined heading at; see
	// rollFreeOrientation.
	float hudYawHold = 0.0f;
	// The VR menu is a world-locked panel: where the head was (position and
	// yaw only) when it opened or the player recentred.
	bool menuAnchorValid = false;
	XrSpace menuSpace = XR_NULL_HANDLE;
	float menuYaw = 0.0f;
	XrVector3f menuHeadPos = {};
#if MIAMIVR_STEAM_FRAME
	// Menu laser pointer: a beam strip and a cursor dot, each its own
	// alpha-blended quad layer submitted after every other layer, so they
	// composite over the menu, the cinema screen and the world. One small
	// swapchain holds the artwork, uploaded from CPU pixels each frame.
	XrSwapchain pointerSwapchain = XR_NULL_HANDLE;
	std::vector<VkImage> pointerImages;
	VkBuffer pointerStaging = VK_NULL_HANDLE;
	VkDeviceMemory pointerStagingMemory = VK_NULL_HANDLE;
	void *pointerStagingMapped = nullptr;
	VkCommandBuffer pointerCommand = VK_NULL_HANDLE;
	VkFence pointerFence = VK_NULL_HANDLE;
	bool pointerSubmissionPending = false;
	// The game says the frontend is what the theater screen shows.
	bool pointerFrontendUp = false;
	// Trigger state with hysteresis (down at 0.6, up below 0.45).
	bool pointerDown = false;
	// What updatePointer worked out this frame; the layers are built from it.
	bool ptrActive = false;
	bool ptrHit = false;
	bool ptrHeadValid = false;
	XrVector3f ptrOrigin = {}, ptrDir = {}, ptrHitPos = {}, ptrHead = {};
	// Orientation of the surface the ray hit (menu or theater pose); the dot
	// lies flat on it.
	XrQuaternionf ptrHitOrientation = { 0.0f, 0.0f, 0.0f, 1.0f };
	// What the last frame submitted, for the dump's flat composite.
	struct DiagLayers {
		bool valid = false;
		XrPosef head = {};
		bool theater = false;
		XrPosef theaterPose = {};
		XrExtent2Df theaterSize = {};
		bool menu = false;
		XrPosef menuPose = {};
		XrExtent2Df menuSize = {};
		int pointerCount = 0;
		XrPosef pointerPose[2] = {};
		XrExtent2Df pointerSize[2] = {};
		int pointerRect[2][4] = {};
	} diagLayers;
#endif

	// XR_META_performance_metrics is the authoritative Quest-side app timing
	// source. Vulkan timestamps remain useful because they isolate this
	// backend's command buffer and validate the runtime GPU number.
	bool hasPerformanceMetricsExt = false;
	bool performanceMetricsEnabled = false;
	PFN_xrEnumeratePerformanceMetricsCounterPathsMETA
		enumeratePerformanceCounterPaths = nullptr;
	PFN_xrSetPerformanceMetricsStateMETA
		setPerformanceMetricsState = nullptr;
	PFN_xrQueryPerformanceMetricsCounterMETA
		queryPerformanceCounter = nullptr;
	XrPath appCpuFrameTimePath = XR_NULL_PATH;
	XrPath appGpuFrameTimePath = XR_NULL_PATH;
	float appCpuFrameTimeMs = 0.0f;
	float appGpuFrameTimeMs = 0.0f;
	bool appCpuFrameTimeValid = false;
	bool appGpuFrameTimeValid = false;

	// Input
	XrActionSet actionSet = XR_NULL_HANDLE;
	XrPath handPath[2] = { XR_NULL_PATH, XR_NULL_PATH };
	XrAction gripPoseAction = XR_NULL_HANDLE;
	XrAction aimPoseAction = XR_NULL_HANDLE;
	XrSpace gripSpace[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
	XrSpace aimSpace[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
	XrAction thumbstickAction = XR_NULL_HANDLE;
	XrAction hapticAction = XR_NULL_HANDLE;
	XrAction triggerAction = XR_NULL_HANDLE;
	XrAction squeezeAction = XR_NULL_HANDLE;
	XrAction thumbClickAction = XR_NULL_HANDLE;
	XrAction aAction = XR_NULL_HANDLE;
	XrAction bAction = XR_NULL_HANDLE;
	XrAction xAction = XR_NULL_HANDLE;
	XrAction yAction = XR_NULL_HANDLE;
	XrAction menuAction = XR_NULL_HANDLE;
#if MIAMIVR_STEAM_FRAME
	// Steam Frame controller: left View and the left D-pad.
	bool hasFrameControllerExt = false;
	XrAction viewAction = XR_NULL_HANDLE;
	XrAction dpadAction[4] = { XR_NULL_HANDLE, XR_NULL_HANDLE,
	                           XR_NULL_HANDLE, XR_NULL_HANDLE };  // up down left right
	// The shoulder bumpers, one per hand; the Frame profile only.
	XrAction bumperAction = XR_NULL_HANDLE;
#endif
	ControllerInput input = {};
#if MIAMIVR_STEAM_FRAME
	// Unattended-test diagnostics (vr-diag.ini); see frame_diag.h.
	// The swapchain was created with TRANSFER_SRC, which dump_frame needs.
	bool swapchainTransferSrc = false;
	// What this frame's runtime said and which head it drew with, for the
	// dump's state file.
	bool diagShouldRender = false;
	bool diagSyntheticHead = false;
	// Last fov the runtime reported plausibly, reused by the synthetic head.
	XrFovf diagLastValidFov[2] = {};
	bool diagLastValidFovSet = false;
#endif
};

State g;

// ---------------------------------------------------------------------------
// Vulkan helpers
// ---------------------------------------------------------------------------

bool
findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties, uint32_t *out)
{
	VkPhysicalDeviceMemoryProperties memProps;
	vkGetPhysicalDeviceMemoryProperties(g.physicalDevice, &memProps);
	for(uint32_t i = 0; i < memProps.memoryTypeCount; i++){
		if((typeBits & (1u << i)) == 0)
			continue;
		if((memProps.memoryTypes[i].propertyFlags & properties) == properties){
			*out = i;
			return true;
		}
	}
	LOGE("no Vulkan memory type for bits %u properties %u", typeBits, properties);
	return false;
}

bool
createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
             VkMemoryPropertyFlags properties, VkBuffer *bufferOut,
             VkDeviceMemory *memoryOut)
{
	VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bufferInfo.size = size;
	bufferInfo.usage = usage;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VK_CHECK(vkCreateBuffer(g.device, &bufferInfo, nullptr, bufferOut), "vkCreateBuffer");

	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(g.device, *bufferOut, &requirements);

	uint32_t typeIndex = 0;
	if(!findMemoryType(requirements.memoryTypeBits, properties, &typeIndex))
		return false;

	VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	allocInfo.allocationSize = requirements.size;
	allocInfo.memoryTypeIndex = typeIndex;
	VK_CHECK(vkAllocateMemory(g.device, &allocInfo, nullptr, memoryOut), "vkAllocateMemory");
	VK_CHECK(vkBindBufferMemory(g.device, *bufferOut, *memoryOut, 0), "vkBindBufferMemory");
	return true;
}

bool
uploadThroughHostBuffer(const void *data, VkDeviceSize size,
                        VkBufferUsageFlags usage, VkBuffer *bufferOut,
                        VkDeviceMemory *memoryOut)
{
	// The bring-up geometry is a few kilobytes and is written once. A
	// host-visible buffer avoids a staging copy and a transfer submission
	// before the device is even known to work.
	if(!createBuffer(size, usage,
	                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                 bufferOut, memoryOut))
		return false;
	void *mapped = nullptr;
	VK_CHECK(vkMapMemory(g.device, *memoryOut, 0, size, 0, &mapped), "vkMapMemory");
	memcpy(mapped, data, (size_t)size);
	vkUnmapMemory(g.device, *memoryOut);
	return true;
}

VkShaderModule
createShaderModule(const uint32_t *code, size_t byteSize)
{
	VkShaderModuleCreateInfo info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	info.codeSize = byteSize;
	info.pCode = code;
	VkShaderModule module = VK_NULL_HANDLE;
	if(vkCreateShaderModule(g.device, &info, nullptr, &module) != VK_SUCCESS){
		LOGE("vkCreateShaderModule failed");
		return VK_NULL_HANDLE;
	}
	return module;
}

// ---------------------------------------------------------------------------
// OpenXR setup
// ---------------------------------------------------------------------------

bool
initialiseLoader(android_app *app)
{
	PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR = nullptr;
	XrResult result = xrGetInstanceProcAddr(
		XR_NULL_HANDLE, "xrInitializeLoaderKHR",
		(PFN_xrVoidFunction *)&xrInitializeLoaderKHR);
	if(XR_FAILED(result) || xrInitializeLoaderKHR == nullptr){
		LOGE("xrInitializeLoaderKHR unavailable: %d", (int)result);
		return false;
	}

	XrLoaderInitInfoAndroidKHR loaderInfo = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
	loaderInfo.applicationVM = app->activity->vm;
	loaderInfo.applicationContext = app->activity->clazz;
	XR_CHECK(xrInitializeLoaderKHR((const XrLoaderInitInfoBaseHeaderKHR *)&loaderInfo),
	         "xrInitializeLoaderKHR");
	return true;
}

bool
applyAndroidThreadSettings(const char *reason)
{
	if(!g.hasAndroidThreadSettingsExt ||
	   g.setAndroidApplicationThread == nullptr ||
	   g.session == XR_NULL_HANDLE || g.applicationMainThreadId == 0)
		return false;

	const XrResult result = g.setAndroidApplicationThread(
		g.session, XR_ANDROID_THREAD_TYPE_APPLICATION_MAIN_KHR,
		g.applicationMainThreadId);
	g.applicationMainThreadRegistered = XR_SUCCEEDED(result);
	if(g.applicationMainThreadRegistered)
		LOGI("XR_KHR_android_thread_settings application-main tid=%u (%s): result=%d",
		     g.applicationMainThreadId, reason, (int)result);
	else
		LOGW("xrSetAndroidApplicationThreadKHR application-main tid=%u (%s) failed: %d",
		     g.applicationMainThreadId, reason, (int)result);
	return g.applicationMainThreadRegistered;
}

#if MIAMIVR_STEAM_FRAME
// Valve's controller extension; newer than the loader's headers.
static const char kFrameControllerExtension[] =
	"XR_VALVE_frame_controller_interaction";
#endif

bool
createInstance(android_app *app)
{
	uint32_t extensionCount = 0;
	xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr);
	std::vector<XrExtensionProperties> available(
		extensionCount, { XR_TYPE_EXTENSION_PROPERTIES });
	xrEnumerateInstanceExtensionProperties(nullptr, extensionCount,
	                                       &extensionCount, available.data());

	auto has = [&](const char *name){
		for(const XrExtensionProperties &e : available)
			if(strcmp(e.extensionName, name) == 0)
				return true;
		return false;
	};

	std::vector<const char *> enabled;
	if(!has(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)){
		LOGE("runtime does not expose %s", XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
		return false;
	}
	enabled.push_back(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
	if(has(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME))
		enabled.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
	// Standalone Quest applications choose their own display frequency. The
	// Quest Link setting on the PC does not apply to this native process.
	g.hasRefreshRateExt = has(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
	if(g.hasRefreshRateExt)
		enabled.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
	g.hasPerformanceMetricsExt =
		has(XR_META_PERFORMANCE_METRICS_EXTENSION_NAME);
	if(g.hasPerformanceMetricsExt)
		enabled.push_back(XR_META_PERFORMANCE_METRICS_EXTENSION_NAME);
	g.hasPerformanceSettingsExt =
		has(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
	if(g.hasPerformanceSettingsExt)
		enabled.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
	g.hasAndroidThreadSettingsExt =
		has(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME);
	if(g.hasAndroidThreadSettingsExt)
		enabled.push_back(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME);
#if MIAMIVR_STEAM_FRAME
	g.hasFrameControllerExt = has(kFrameControllerExtension);
	if(g.hasFrameControllerExt)
		enabled.push_back(kFrameControllerExtension);
	LOGI("[vr] %s %s", kFrameControllerExtension,
	     g.hasFrameControllerExt ? "enabled" : "not advertised");
#endif
	XrInstanceCreateInfoAndroidKHR androidInfo = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
	androidInfo.applicationVM = app->activity->vm;
	androidInfo.applicationActivity = app->activity->clazz;

	XrInstanceCreateInfo info = { XR_TYPE_INSTANCE_CREATE_INFO };
	info.next = &androidInfo;
	info.enabledExtensionCount = (uint32_t)enabled.size();
	info.enabledExtensionNames = enabled.data();
	strcpy(info.applicationInfo.applicationName, "Vice City VR");
	info.applicationInfo.applicationVersion = 1;
	strcpy(info.applicationInfo.engineName, "reVC");
	info.applicationInfo.engineVersion = 1;
#if MIAMIVR_STEAM_FRAME
	// SteamVR on the Frame: request 1.0, as every port proven there does. No
	// OpenXR 1.1 core function is used, and 1.1 is tried if 1.0 is refused.
	info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	XrResult instanceResult = xrCreateInstance(&info, &g.instance);
	if(instanceResult == XR_ERROR_API_VERSION_UNSUPPORTED){
		LOGW("[vr] OpenXR 1.0 refused; retrying with 1.1");
		info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
		instanceResult = xrCreateInstance(&info, &g.instance);
	}
	XR_CHECK(instanceResult, "xrCreateInstance");
#else
	info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

	XR_CHECK(xrCreateInstance(&info, &g.instance), "xrCreateInstance");
#endif
	if(g.hasRefreshRateExt){
		const XrResult enumerateResult = xrGetInstanceProcAddr(
			g.instance, "xrEnumerateDisplayRefreshRatesFB",
			(PFN_xrVoidFunction *)&g.enumerateRefreshRates);
		const XrResult getResult = xrGetInstanceProcAddr(
			g.instance, "xrGetDisplayRefreshRateFB",
			(PFN_xrVoidFunction *)&g.getRefreshRate);
		const XrResult requestResult = xrGetInstanceProcAddr(
			g.instance, "xrRequestDisplayRefreshRateFB",
			(PFN_xrVoidFunction *)&g.requestRefreshRate);
		if(XR_FAILED(enumerateResult) || XR_FAILED(getResult) ||
		   XR_FAILED(requestResult) || g.enumerateRefreshRates == nullptr ||
		   g.getRefreshRate == nullptr || g.requestRefreshRate == nullptr){
			LOGE("XR_FB_display_refresh_rate functions unavailable: %d/%d/%d",
			     (int)enumerateResult, (int)getResult, (int)requestResult);
			g.hasRefreshRateExt = false;
		}
	}
	g.refreshRateReadyForFrames = !g.hasRefreshRateExt;
	if(g.hasPerformanceSettingsExt){
		const XrResult result = xrGetInstanceProcAddr(
			g.instance, "xrPerfSettingsSetPerformanceLevelEXT",
			(PFN_xrVoidFunction *)&g.setPerformanceLevel);
		if(XR_FAILED(result) || g.setPerformanceLevel == nullptr){
			LOGE("XR_EXT_performance_settings entry point unavailable: %d",
			     (int)result);
			g.hasPerformanceSettingsExt = false;
			g.setPerformanceLevel = nullptr;
		}else
			LOGI("XR_EXT_performance_settings supported");
	}else
		LOGI("XR_EXT_performance_settings not supported; runtime controls clocks");
	if(g.hasAndroidThreadSettingsExt){
		const XrResult result = xrGetInstanceProcAddr(
			g.instance, "xrSetAndroidApplicationThreadKHR",
			(PFN_xrVoidFunction *)&g.setAndroidApplicationThread);
		if(XR_FAILED(result) || g.setAndroidApplicationThread == nullptr){
			LOGW("XR_KHR_android_thread_settings entry point unavailable: %d",
			     (int)result);
			g.hasAndroidThreadSettingsExt = false;
			g.setAndroidApplicationThread = nullptr;
		}else
			LOGI("XR_KHR_android_thread_settings supported");
	}else
		LOGI("XR_KHR_android_thread_settings not supported");
	if(g.hasPerformanceMetricsExt){
		const XrResult enumerateResult = xrGetInstanceProcAddr(
			g.instance,
			"xrEnumeratePerformanceMetricsCounterPathsMETA",
			(PFN_xrVoidFunction *)&g.enumeratePerformanceCounterPaths);
		const XrResult setResult = xrGetInstanceProcAddr(
			g.instance, "xrSetPerformanceMetricsStateMETA",
			(PFN_xrVoidFunction *)&g.setPerformanceMetricsState);
		const XrResult queryResult = xrGetInstanceProcAddr(
			g.instance, "xrQueryPerformanceMetricsCounterMETA",
			(PFN_xrVoidFunction *)&g.queryPerformanceCounter);
		if(XR_FAILED(enumerateResult) || XR_FAILED(setResult) ||
		   XR_FAILED(queryResult) ||
		   g.enumeratePerformanceCounterPaths == nullptr ||
		   g.setPerformanceMetricsState == nullptr ||
		   g.queryPerformanceCounter == nullptr){
			LOGE("XR_META_performance_metrics functions unavailable: "
			     "%d/%d/%d", (int)enumerateResult, (int)setResult,
			     (int)queryResult);
			g.hasPerformanceMetricsExt = false;
		}else{
			uint32_t count = 0;
			if(XR_SUCCEEDED(g.enumeratePerformanceCounterPaths(
				g.instance, 0, &count, nullptr)) && count > 0){
				std::vector<XrPath> paths(count);
				if(XR_SUCCEEDED(g.enumeratePerformanceCounterPaths(
					g.instance, count, &count, paths.data()))){
					XrPath cpuCandidate = XR_NULL_PATH;
					XrPath gpuCandidate = XR_NULL_PATH;
					xrStringToPath(g.instance,
						"/perfmetrics_meta/app/cpu_frametime",
						&cpuCandidate);
					xrStringToPath(g.instance,
						"/perfmetrics_meta/app/gpu_frametime",
						&gpuCandidate);
					for(XrPath path : paths){
						if(path == cpuCandidate)
							g.appCpuFrameTimePath = path;
						if(path == gpuCandidate)
							g.appGpuFrameTimePath = path;
					}
				}
			}
			LOGI("XR_META performance metrics: CPU=%s GPU=%s",
			     g.appCpuFrameTimePath != XR_NULL_PATH ?
			     	"available" : "missing",
			     g.appGpuFrameTimePath != XR_NULL_PATH ?
			     	"available" : "missing");
		}
	}

	XrInstanceProperties properties = { XR_TYPE_INSTANCE_PROPERTIES };
	if(XR_SUCCEEDED(xrGetInstanceProperties(g.instance, &properties)))
		LOGI("OpenXR runtime: %s", properties.runtimeName);
	return true;
}

bool
createVulkan(void)
{
	XrSystemGetInfo systemInfo = { XR_TYPE_SYSTEM_GET_INFO };
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XR_CHECK(xrGetSystem(g.instance, &systemInfo, &g.systemId), "xrGetSystem");

	PFN_xrGetVulkanGraphicsRequirements2KHR getRequirements = nullptr;
	PFN_xrCreateVulkanInstanceKHR createVulkanInstance = nullptr;
	PFN_xrGetVulkanGraphicsDevice2KHR getGraphicsDevice = nullptr;
	PFN_xrCreateVulkanDeviceKHR createVulkanDevice = nullptr;
	xrGetInstanceProcAddr(g.instance, "xrGetVulkanGraphicsRequirements2KHR",
	                      (PFN_xrVoidFunction *)&getRequirements);
	xrGetInstanceProcAddr(g.instance, "xrCreateVulkanInstanceKHR",
	                      (PFN_xrVoidFunction *)&createVulkanInstance);
	xrGetInstanceProcAddr(g.instance, "xrGetVulkanGraphicsDevice2KHR",
	                      (PFN_xrVoidFunction *)&getGraphicsDevice);
	xrGetInstanceProcAddr(g.instance, "xrCreateVulkanDeviceKHR",
	                      (PFN_xrVoidFunction *)&createVulkanDevice);
	if(getRequirements == nullptr || createVulkanInstance == nullptr ||
	   getGraphicsDevice == nullptr || createVulkanDevice == nullptr){
		LOGE("XR_KHR_vulkan_enable2 entry points missing");
		return false;
	}

	XrGraphicsRequirementsVulkan2KHR requirements = {
		XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };
	XR_CHECK(getRequirements(g.instance, g.systemId, &requirements),
	         "xrGetVulkanGraphicsRequirements2KHR");

	// The runtime creates the VkInstance on our behalf so it can inject the
	// extensions it needs for swapchain sharing.
	VkApplicationInfo appInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	appInfo.pApplicationName = "Vice City VR";
	appInfo.applicationVersion = 1;
	appInfo.pEngineName = "reVC";
	appInfo.engineVersion = 1;
	appInfo.apiVersion = VK_API_VERSION_1_1;

	VkInstanceCreateInfo vkInstanceInfo = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	vkInstanceInfo.pApplicationInfo = &appInfo;

	XrVulkanInstanceCreateInfoKHR xrVkInstanceInfo = {
		XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR };
	xrVkInstanceInfo.systemId = g.systemId;
	xrVkInstanceInfo.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
	xrVkInstanceInfo.vulkanCreateInfo = &vkInstanceInfo;

	VkResult vkResult = VK_SUCCESS;
	XR_CHECK(createVulkanInstance(g.instance, &xrVkInstanceInfo, &g.vkInstance, &vkResult),
	         "xrCreateVulkanInstanceKHR");
	VK_CHECK(vkResult, "vkCreateInstance (via OpenXR)");

	XrVulkanGraphicsDeviceGetInfoKHR deviceGetInfo = {
		XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR };
	deviceGetInfo.systemId = g.systemId;
	deviceGetInfo.vulkanInstance = g.vkInstance;
	XR_CHECK(getGraphicsDevice(g.instance, &deviceGetInfo, &g.physicalDevice),
	         "xrGetVulkanGraphicsDevice2KHR");

	VkPhysicalDeviceProperties physicalProperties;
	vkGetPhysicalDeviceProperties(g.physicalDevice, &physicalProperties);
	LOGI("Vulkan device: %s (API %u.%u.%u)", physicalProperties.deviceName,
	     VK_VERSION_MAJOR(physicalProperties.apiVersion),
	     VK_VERSION_MINOR(physicalProperties.apiVersion),
	     VK_VERSION_PATCH(physicalProperties.apiVersion));

	uint32_t familyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(g.physicalDevice, &familyCount, nullptr);
	std::vector<VkQueueFamilyProperties> families(familyCount);
	vkGetPhysicalDeviceQueueFamilyProperties(g.physicalDevice, &familyCount, families.data());
	bool foundFamily = false;
	for(uint32_t i = 0; i < familyCount; i++)
		if(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT){
			g.queueFamily = i;
			foundFamily = true;
			break;
		}
	if(!foundFamily){
		LOGE("no graphics queue family");
		return false;
	}

	const float priority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	queueInfo.queueFamilyIndex = g.queueFamily;
	queueInfo.queueCount = 1;
	queueInfo.pQueuePriorities = &priority;

	// Multiview is the load-bearing feature of this backend, so it is requested
	// explicitly rather than relying on it being enabled by default.
	VkPhysicalDeviceMultiviewFeatures multiviewFeatures = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES };
	multiviewFeatures.multiview = VK_TRUE;

	VkPhysicalDeviceFeatures deviceFeatures = {};
	VkPhysicalDeviceFeatures availableFeatures = {};
	vkGetPhysicalDeviceFeatures(g.physicalDevice, &availableFeatures);
	deviceFeatures.fragmentStoresAndAtomics =
		availableFeatures.fragmentStoresAndAtomics;
	g.fragmentStoresAndAtomicsEnabled =
		deviceFeatures.fragmentStoresAndAtomics != VK_FALSE;
	deviceFeatures.samplerAnisotropy = VK_TRUE;
	deviceFeatures.textureCompressionASTC_LDR = VK_TRUE;

	VkDeviceCreateInfo deviceInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	deviceInfo.pNext = &multiviewFeatures;
	deviceInfo.queueCreateInfoCount = 1;
	deviceInfo.pQueueCreateInfos = &queueInfo;
	deviceInfo.pEnabledFeatures = &deviceFeatures;

	XrVulkanDeviceCreateInfoKHR xrDeviceInfo = { XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR };
	xrDeviceInfo.systemId = g.systemId;
	xrDeviceInfo.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
	xrDeviceInfo.vulkanPhysicalDevice = g.physicalDevice;
	xrDeviceInfo.vulkanCreateInfo = &deviceInfo;

	XR_CHECK(createVulkanDevice(g.instance, &xrDeviceInfo, &g.device, &vkResult),
	         "xrCreateVulkanDeviceKHR");
	VK_CHECK(vkResult, "vkCreateDevice (via OpenXR)");

	vkGetDeviceQueue(g.device, g.queueFamily, 0, &g.queue);
	return true;
}

bool
createSession(void)
{
	XrGraphicsBindingVulkan2KHR binding = { XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR };
	binding.instance = g.vkInstance;
	binding.physicalDevice = g.physicalDevice;
	binding.device = g.device;
	binding.queueFamilyIndex = g.queueFamily;
	binding.queueIndex = 0;

	XrSessionCreateInfo info = { XR_TYPE_SESSION_CREATE_INFO };
	info.next = &binding;
	info.systemId = g.systemId;
	XR_CHECK(xrCreateSession(g.instance, &info, &g.session), "xrCreateSession");
	// The specification only requires a valid session. Register immediately so
	// the runtime knows which CPU thread is critical while game resources are
	// initialised, then reassert at READY/FOCUSED for runtimes which reset
	// scheduling attributes across lifecycle transitions.
	applyAndroidThreadSettings("session created");
	// Thermal blocks and effective hints belong to one OpenXR session. A new
	// session starts from runtime policy again while retaining the requested
	// menu mode so READY can apply it.
	g.activePerformanceMode = -1;
	g.activeGpuPerformanceMode = -1;
	g.cpuPerformanceHintSucceededThisSession = false;
	g.gpuPerformanceHintSucceededThisSession = false;
	g.cpuBoostActive = false;
	g.boostThermallyBlocked = false;
	g.boostExpiryDisplayTime = 0;
	g.boostExpiryMonotonicNs = 0;
	g.performanceAutoLogged = false;

	// STAGE gives a floor-relative origin, which is what a standing game wants.
	// Not every runtime configuration offers it, so LOCAL remains the fallback.
	uint32_t spaceCount = 0;
	xrEnumerateReferenceSpaces(g.session, 0, &spaceCount, nullptr);
	std::vector<XrReferenceSpaceType> spaces(spaceCount);
	xrEnumerateReferenceSpaces(g.session, spaceCount, &spaceCount, spaces.data());
	XrReferenceSpaceType chosen = XR_REFERENCE_SPACE_TYPE_LOCAL;
	for(XrReferenceSpaceType type : spaces)
		if(type == XR_REFERENCE_SPACE_TYPE_STAGE){
			chosen = XR_REFERENCE_SPACE_TYPE_STAGE;
			break;
		}

	XrReferenceSpaceCreateInfo spaceInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	spaceInfo.referenceSpaceType = chosen;
	spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
	XR_CHECK(xrCreateReferenceSpace(g.session, &spaceInfo, &g.space),
	         "xrCreateReferenceSpace");

	// Always create LOCAL as well. If STAGE turns out not to be tracking --
	// which happens without a configured play area -- locating views in it
	// yields no valid position, and rendering would be skipped entirely.
	XrReferenceSpaceCreateInfo localInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	localInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	localInfo.poseInReferenceSpace.orientation.w = 1.0f;
	if(XR_FAILED(xrCreateReferenceSpace(g.session, &localInfo, &g.localSpace)))
		g.localSpace = XR_NULL_HANDLE;

	// VIEW space carries the head-locked quad layers (the desktop's debug
	// overlay and VR menu attach to gViewSpace the same way).
	XrReferenceSpaceCreateInfo viewInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	viewInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	viewInfo.poseInReferenceSpace.orientation.w = 1.0f;
	if(XR_FAILED(xrCreateReferenceSpace(g.session, &viewInfo, &g.viewSpace)))
		g.viewSpace = XR_NULL_HANDLE;

	LOGI("reference space: %s (local fallback %s)",
	     chosen == XR_REFERENCE_SPACE_TYPE_STAGE ? "STAGE" : "LOCAL",
	     g.localSpace != XR_NULL_HANDLE ? "available" : "unavailable");
	return true;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

XrAction
makeAction(const char *name, const char *localised, XrActionType type,
           bool perHand)
{
	XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
	info.actionType = type;
	strcpy(info.actionName, name);
	strcpy(info.localizedActionName, localised);
	if(perHand){
		info.countSubactionPaths = 2;
		info.subactionPaths = g.handPath;
	}
	XrAction action = XR_NULL_HANDLE;
	if(XR_FAILED(xrCreateAction(g.actionSet, &info, &action)))
		LOGE("xrCreateAction failed for %s", name);
	return action;
}

bool
createActions(void)
{
	XrActionSetCreateInfo setInfo = { XR_TYPE_ACTION_SET_CREATE_INFO };
	strcpy(setInfo.actionSetName, "gameplay");
	strcpy(setInfo.localizedActionSetName, "Gameplay");
	XR_CHECK(xrCreateActionSet(g.instance, &setInfo, &g.actionSet),
	         "xrCreateActionSet");

	xrStringToPath(g.instance, "/user/hand/left", &g.handPath[0]);
	xrStringToPath(g.instance, "/user/hand/right", &g.handPath[1]);

	g.thumbstickAction = makeAction("thumbstick", "Thumbstick",
	                                XR_ACTION_TYPE_VECTOR2F_INPUT, true);
	g.gripPoseAction = makeAction("grippose", "Grip Pose",
	                              XR_ACTION_TYPE_POSE_INPUT, true);
	g.aimPoseAction = makeAction("aimpose", "Aim Pose",
	                             XR_ACTION_TYPE_POSE_INPUT, true);
	g.triggerAction = makeAction("trigger", "Trigger",
	                             XR_ACTION_TYPE_FLOAT_INPUT, true);
	g.squeezeAction = makeAction("squeeze", "Grip",
	                             XR_ACTION_TYPE_FLOAT_INPUT, true);
	g.thumbClickAction = makeAction("thumbclick", "Thumbstick Click",
	                                XR_ACTION_TYPE_BOOLEAN_INPUT, true);
	g.aAction = makeAction("abutton", "A", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
	g.bAction = makeAction("bbutton", "B", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
	g.xAction = makeAction("xbutton", "X", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
	g.yAction = makeAction("ybutton", "Y", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
	g.menuAction = makeAction("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
	g.hapticAction = makeAction("haptic", "Haptic",
	                            XR_ACTION_TYPE_VIBRATION_OUTPUT, true);
#if MIAMIVR_STEAM_FRAME
	g.viewAction = makeAction("view", "View", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
	g.dpadAction[0] = makeAction("dpadup", "D-pad Up",
	                             XR_ACTION_TYPE_BOOLEAN_INPUT, false);
	g.dpadAction[1] = makeAction("dpaddown", "D-pad Down",
	                             XR_ACTION_TYPE_BOOLEAN_INPUT, false);
	g.dpadAction[2] = makeAction("dpadleft", "D-pad Left",
	                             XR_ACTION_TYPE_BOOLEAN_INPUT, false);
	g.dpadAction[3] = makeAction("dpadright", "D-pad Right",
	                             XR_ACTION_TYPE_BOOLEAN_INPUT, false);
	g.bumperAction = makeAction("bumper", "Bumper",
	                            XR_ACTION_TYPE_BOOLEAN_INPUT, true);
#endif

	struct Binding { XrAction action; const char *path; };
	const Binding bindings[] = {
		{ g.gripPoseAction,   "/user/hand/left/input/grip/pose" },
		{ g.gripPoseAction,   "/user/hand/right/input/grip/pose" },
		{ g.aimPoseAction,    "/user/hand/left/input/aim/pose" },
		{ g.aimPoseAction,    "/user/hand/right/input/aim/pose" },
		{ g.hapticAction,     "/user/hand/left/output/haptic" },
		{ g.hapticAction,     "/user/hand/right/output/haptic" },
		{ g.thumbstickAction, "/user/hand/left/input/thumbstick" },
		{ g.thumbstickAction, "/user/hand/right/input/thumbstick" },
		{ g.triggerAction,    "/user/hand/left/input/trigger/value" },
		{ g.triggerAction,    "/user/hand/right/input/trigger/value" },
		{ g.squeezeAction,    "/user/hand/left/input/squeeze/value" },
		{ g.squeezeAction,    "/user/hand/right/input/squeeze/value" },
		{ g.thumbClickAction, "/user/hand/left/input/thumbstick/click" },
		{ g.thumbClickAction, "/user/hand/right/input/thumbstick/click" },
		{ g.aAction,          "/user/hand/right/input/a/click" },
		{ g.bAction,          "/user/hand/right/input/b/click" },
		{ g.xAction,          "/user/hand/left/input/x/click" },
		{ g.yAction,          "/user/hand/left/input/y/click" },
		// Only the left controller exposes a menu button; the right one's
		// system button belongs to the runtime.
		{ g.menuAction,       "/user/hand/left/input/menu/click" },
	};

#if MIAMIVR_STEAM_FRAME
	// Steam Frame first, then Index, then the Quest Touch map above as the
	// last fallback. Each profile is suggested on its own, so one the runtime
	// refuses cannot take the others down with it. The Frame is a split
	// gamepad: A B X Y and Menu on the right, View and the D-pad on the left.
	// X and Y keep their names (the game's prompts and the CONTROLS page say
	// X and Y); only their hand changes from the Quest layout.
	const Binding frameBindings[] = {
		{ g.gripPoseAction,   "/user/hand/left/input/grip/pose" },
		{ g.gripPoseAction,   "/user/hand/right/input/grip/pose" },
		{ g.aimPoseAction,    "/user/hand/left/input/aim/pose" },
		{ g.aimPoseAction,    "/user/hand/right/input/aim/pose" },
		{ g.hapticAction,     "/user/hand/left/output/haptic" },
		{ g.hapticAction,     "/user/hand/right/output/haptic" },
		{ g.thumbstickAction, "/user/hand/left/input/thumbstick" },
		{ g.thumbstickAction, "/user/hand/right/input/thumbstick" },
		{ g.triggerAction,    "/user/hand/left/input/trigger/value" },
		{ g.triggerAction,    "/user/hand/right/input/trigger/value" },
		{ g.squeezeAction,    "/user/hand/left/input/squeeze/value" },
		{ g.squeezeAction,    "/user/hand/right/input/squeeze/value" },
		{ g.thumbClickAction, "/user/hand/left/input/thumbstick/click" },
		{ g.thumbClickAction, "/user/hand/right/input/thumbstick/click" },
		{ g.aAction,          "/user/hand/right/input/a/click" },
		{ g.bAction,          "/user/hand/right/input/b/click" },
		{ g.xAction,          "/user/hand/right/input/x/click" },
		{ g.yAction,          "/user/hand/right/input/y/click" },
		{ g.menuAction,       "/user/hand/right/input/menu/click" },
		{ g.viewAction,       "/user/hand/left/input/view/click" },
		{ g.dpadAction[0],    "/user/hand/left/input/dpad_up/click" },
		{ g.dpadAction[1],    "/user/hand/left/input/dpad_down/click" },
		{ g.dpadAction[2],    "/user/hand/left/input/dpad_left/click" },
		{ g.dpadAction[3],    "/user/hand/left/input/dpad_right/click" },
		{ g.bumperAction,     "/user/hand/left/input/bumper/click" },
		{ g.bumperAction,     "/user/hand/right/input/bumper/click" },
	};
	// Index has A and B on each hand and no Menu or View: left B is Menu
	// (Start), left A is View (back, hold to recentre). X and Y are not
	// reachable on Index except through the CONTROLS remapping page.
	const Binding indexBindings[] = {
		{ g.gripPoseAction,   "/user/hand/left/input/grip/pose" },
		{ g.gripPoseAction,   "/user/hand/right/input/grip/pose" },
		{ g.aimPoseAction,    "/user/hand/left/input/aim/pose" },
		{ g.aimPoseAction,    "/user/hand/right/input/aim/pose" },
		{ g.hapticAction,     "/user/hand/left/output/haptic" },
		{ g.hapticAction,     "/user/hand/right/output/haptic" },
		{ g.thumbstickAction, "/user/hand/left/input/thumbstick" },
		{ g.thumbstickAction, "/user/hand/right/input/thumbstick" },
		{ g.triggerAction,    "/user/hand/left/input/trigger/value" },
		{ g.triggerAction,    "/user/hand/right/input/trigger/value" },
		{ g.squeezeAction,    "/user/hand/left/input/squeeze/value" },
		{ g.squeezeAction,    "/user/hand/right/input/squeeze/value" },
		{ g.thumbClickAction, "/user/hand/left/input/thumbstick/click" },
		{ g.thumbClickAction, "/user/hand/right/input/thumbstick/click" },
		{ g.aAction,          "/user/hand/right/input/a/click" },
		{ g.bAction,          "/user/hand/right/input/b/click" },
		{ g.menuAction,       "/user/hand/left/input/b/click" },
		{ g.viewAction,       "/user/hand/left/input/a/click" },
	};
	struct Profile { const char *path; const Binding *bindings; size_t count; };
	const Profile profiles[] = {
		{ "/interaction_profiles/valve/frame_controller_valve",
		  frameBindings, sizeof(frameBindings)/sizeof(frameBindings[0]) },
		{ "/interaction_profiles/valve/index_controller",
		  indexBindings, sizeof(indexBindings)/sizeof(indexBindings[0]) },
		{ "/interaction_profiles/oculus/touch_controller",
		  bindings, sizeof(bindings)/sizeof(bindings[0]) },
	};
	size_t suggestedProfiles = 0;
	std::vector<XrActionSuggestedBinding> suggested;
	for(const Profile &p : profiles){
		if(p.bindings == frameBindings && !g.hasFrameControllerExt){
			LOGW("[vr] %s not advertised: no Steam Frame profile suggested",
			     kFrameControllerExtension);
			continue;
		}
		suggested.clear();
		for(size_t i = 0; i < p.count; i++){
			XrPath path = XR_NULL_PATH;
			if(XR_FAILED(xrStringToPath(g.instance, p.bindings[i].path, &path)))
				continue;
			suggested.push_back({ p.bindings[i].action, path });
		}
		XrPath profile = XR_NULL_PATH;
		xrStringToPath(g.instance, p.path, &profile);
		XrInteractionProfileSuggestedBinding suggestion = {
			XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
		suggestion.interactionProfile = profile;
		suggestion.countSuggestedBindings = (uint32_t)suggested.size();
		suggestion.suggestedBindings = suggested.data();
		const XrResult result =
			xrSuggestInteractionProfileBindings(g.instance, &suggestion);
		LOGI("[vr] xrSuggestInteractionProfileBindings %s: %zu bindings, result %d",
		     p.path, suggested.size(), (int)result);
		if(XR_SUCCEEDED(result))
			suggestedProfiles++;
	}
	if(suggestedProfiles == 0){
		LOGE("[vr] no interaction profile accepted; the game gets no input");
		return false;
	}
#else
	std::vector<XrActionSuggestedBinding> suggested;
	for(const Binding &binding : bindings){
		XrPath path = XR_NULL_PATH;
		if(XR_FAILED(xrStringToPath(g.instance, binding.path, &path)))
			continue;
		suggested.push_back({ binding.action, path });
	}

	XrPath profile = XR_NULL_PATH;
	xrStringToPath(g.instance, "/interaction_profiles/oculus/touch_controller",
	               &profile);
	XrInteractionProfileSuggestedBinding suggestion = {
		XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	suggestion.interactionProfile = profile;
	suggestion.countSuggestedBindings = (uint32_t)suggested.size();
	suggestion.suggestedBindings = suggested.data();
	XR_CHECK(xrSuggestInteractionProfileBindings(g.instance, &suggestion),
	         "xrSuggestInteractionProfileBindings");
#endif

	XrSessionActionSetsAttachInfo attachInfo = {
		XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	attachInfo.countActionSets = 1;
	attachInfo.actionSets = &g.actionSet;
	XR_CHECK(xrAttachSessionActionSets(g.session, &attachInfo),
	         "xrAttachSessionActionSets");

	for(int hand = 0; hand < 2; hand++){
		XrActionSpaceCreateInfo spaceInfo = {
			XR_TYPE_ACTION_SPACE_CREATE_INFO };
		spaceInfo.subactionPath = g.handPath[hand];
		spaceInfo.poseInActionSpace.orientation.w = 1.0f;

		spaceInfo.action = g.gripPoseAction;
		XR_CHECK(xrCreateActionSpace(g.session, &spaceInfo,
		                            &g.gripSpace[hand]),
		         "xrCreateActionSpace(grip)");
		spaceInfo.action = g.aimPoseAction;
		XR_CHECK(xrCreateActionSpace(g.session, &spaceInfo,
		                            &g.aimSpace[hand]),
		         "xrCreateActionSpace(aim)");
	}

	LOGI("input: %zu bindings attached", suggested.size());
	return true;
}

float
readFloat(XrAction action, int hand)
{
	XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
	info.action = action;
	info.subactionPath = g.handPath[hand];
	XrActionStateFloat state = { XR_TYPE_ACTION_STATE_FLOAT };
	if(XR_FAILED(xrGetActionStateFloat(g.session, &info, &state)) || !state.isActive)
		return 0.0f;
	return state.currentState;
}

bool
readBool(XrAction action, int hand)
{
	XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
	info.action = action;
	info.subactionPath = hand >= 0 ? g.handPath[hand] : XR_NULL_PATH;
	XrActionStateBoolean state = { XR_TYPE_ACTION_STATE_BOOLEAN };
	if(XR_FAILED(xrGetActionStateBoolean(g.session, &info, &state)) || !state.isActive)
		return false;
	return state.currentState == XR_TRUE;
}

void
readStick(int hand, float *outX, float *outY)
{
	XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
	info.action = g.thumbstickAction;
	info.subactionPath = g.handPath[hand];
	XrActionStateVector2f state = { XR_TYPE_ACTION_STATE_VECTOR2F };
	if(XR_FAILED(xrGetActionStateVector2f(g.session, &info, &state)) || !state.isActive){
		*outX = *outY = 0.0f;
		return;
	}
	*outX = state.currentState.x;
	*outY = state.currentState.y;
}

void
syncInput(void)
{
	if(g.actionSet == XR_NULL_HANDLE || g.sessionState != XR_SESSION_STATE_FOCUSED){
#if MIAMIVR_STEAM_FRAME
		// Focus lost: release the system-layer inputs rather than leave them
		// held, so a View hold cannot complete while the game is paused.
		g.input.view = false;
		g.input.dpadUp = g.input.dpadDown = false;
		g.input.dpadLeft = g.input.dpadRight = false;
		g.input.leftBumper = g.input.rightBumper = false;
#endif
		return;
	}

	XrActiveActionSet active = { g.actionSet, XR_NULL_PATH };
	XrActionsSyncInfo syncInfo = { XR_TYPE_ACTIONS_SYNC_INFO };
	syncInfo.countActiveActionSets = 1;
	syncInfo.activeActionSets = &active;
	if(XR_FAILED(xrSyncActions(g.session, &syncInfo)))
		return;

	readStick(0, &g.input.leftStickX, &g.input.leftStickY);
	readStick(1, &g.input.rightStickX, &g.input.rightStickY);
	g.input.leftTrigger = readFloat(g.triggerAction, 0);
	g.input.rightTrigger = readFloat(g.triggerAction, 1);
	g.input.leftGrip = readFloat(g.squeezeAction, 0);
	g.input.rightGrip = readFloat(g.squeezeAction, 1);
	g.input.leftStickClick = readBool(g.thumbClickAction, 0);
	g.input.rightStickClick = readBool(g.thumbClickAction, 1);
	g.input.a = readBool(g.aAction, -1);
	g.input.b = readBool(g.bAction, -1);
	g.input.x = readBool(g.xAction, -1);
	g.input.y = readBool(g.yAction, -1);
	g.input.menu = readBool(g.menuAction, -1);
#if MIAMIVR_STEAM_FRAME
	g.input.view = readBool(g.viewAction, -1);
	g.input.dpadUp = readBool(g.dpadAction[0], -1);
	g.input.dpadDown = readBool(g.dpadAction[1], -1);
	g.input.dpadLeft = readBool(g.dpadAction[2], -1);
	g.input.dpadRight = readBool(g.dpadAction[3], -1);
	g.input.leftBumper = readBool(g.bumperAction, 0);
	g.input.rightBumper = readBool(g.bumperAction, 1);
#endif
}

void
locateTrackedPose(XrSpace actionSpace, XrSpace baseSpace, XrTime time,
                  TrackedPose *out)
{
	out->valid = false;
	if(actionSpace == XR_NULL_HANDLE || baseSpace == XR_NULL_HANDLE)
		return;

	XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
	if(XR_FAILED(xrLocateSpace(actionSpace, baseSpace, time, &location)))
		return;
	const XrSpaceLocationFlags required =
		XR_SPACE_LOCATION_POSITION_VALID_BIT |
		XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	if((location.locationFlags & required) != required)
		return;

	out->position[0] = location.pose.position.x;
	out->position[1] = location.pose.position.y;
	out->position[2] = location.pose.position.z;
	out->orientation[0] = location.pose.orientation.x;
	out->orientation[1] = location.pose.orientation.y;
	out->orientation[2] = location.pose.orientation.z;
	out->orientation[3] = location.pose.orientation.w;
	out->valid = true;
}

bool
isPoseActionActive(XrAction action, int hand)
{
	if(action == XR_NULL_HANDLE || g.session == XR_NULL_HANDLE)
		return false;

	XrActionStateGetInfo getInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
	getInfo.action = action;
	getInfo.subactionPath = g.handPath[hand];
	XrActionStatePose state = { XR_TYPE_ACTION_STATE_POSE };
	return XR_SUCCEEDED(xrGetActionStatePose(g.session, &getInfo, &state)) &&
	       state.isActive == XR_TRUE;
}

void
locateControllerPoses(XrSpace baseSpace, XrTime time)
{
	for(int hand = 0; hand < 2; hand++){
		if(isPoseActionActive(g.gripPoseAction, hand))
			locateTrackedPose(g.gripSpace[hand], baseSpace, time,
			                 &g.input.gripPose[hand]);
		else
			g.input.gripPose[hand].valid = false;

		if(isPoseActionActive(g.aimPoseAction, hand))
			locateTrackedPose(g.aimSpace[hand], baseSpace, time,
			                 &g.input.aimPose[hand]);
		else
			g.input.aimPose[hand].valid = false;
	}
}

bool
createSwapchain(void)
{
	uint32_t viewCount = 0;
	XR_CHECK(xrEnumerateViewConfigurationViews(
	             g.instance, g.systemId,
	             XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr),
	         "xrEnumerateViewConfigurationViews");
	if(viewCount != 2){
		LOGE("expected a stereo view configuration, got %u views", viewCount);
		return false;
	}
	for(uint32_t i = 0; i < 2; i++)
		g.viewConfigs[i] = { XR_TYPE_VIEW_CONFIGURATION_VIEW };
	XR_CHECK(xrEnumerateViewConfigurationViews(
	             g.instance, g.systemId,
	             XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &viewCount, g.viewConfigs),
	         "xrEnumerateViewConfigurationViews");

	char settingsPath[512];
	snprintf(settingsPath, sizeof(settingsPath), "%s/vr_settings.ini",
		platform::gameDataRoot());
	// Apply the tested Quest baseline once when upgrading an older settings
	// file. The version marker is written last, so a partial write retries on
	// the next launch. After migration every menu option remains user-editable.
	const int baselineVersion = (int)GetPrivateProfileIntA(
		"VR", "QuestBaselineProfileVersion", 0, settingsPath);
	if(baselineVersion < 1){
		struct BaselineSetting { const char *key; const char *value; };
		static const BaselineSetting settings[] = {
			{ "RenderScalePercent", "100" },
			{ "Sgsr2Mode", "0" },
			{ "CpuPerformanceMode", "1" },
			{ "GpuPerformanceMode", "1" },
			{ "OcclusionCulling", "1" },
			{ "OcclusionCullingModeV2", "2" },
			{ "PhysicsDirectorMode", "3" },
			{ "PhysicsDirectorPreset", "0" },
			{ "VehicleVisualBudgetMode", "0" },
			{ "ModelSet", "1" },
			{ "ModelSetWorld", "1" },
			{ "ModelSetVegetation", "0" },
			{ "ModelSetVehicles", "0" },
			{ "ModelSetPeds", "0" },
			{ "ModelSetWeapons", "1" }
		};
		bool migrated = true;
		for(size_t i = 0; i < sizeof(settings)/sizeof(settings[0]); i++)
			migrated = WritePrivateProfileStringA("VR", settings[i].key,
				settings[i].value, settingsPath) && migrated;
		if(migrated)
			migrated = WritePrivateProfileStringA("VR",
				"QuestBaselineProfileVersion", "1", settingsPath);
		LOGI("Quest baseline profile migration: version=%d result=%s path=%s",
		     baselineVersion, migrated ? "applied" : "FAILED", settingsPath);
	}
	// 0.5.4 moves the baseline down to native resolution. The old 125%
	// left nothing for the effects added since, and a settings file
	// written by an earlier build still carries it. Only the scale is
	// rewritten; the rest of the page stays as the player set it.
	if(baselineVersion < 2){
		const bool rescaled =
			WritePrivateProfileStringA("VR", "RenderScalePercent",
				"100", settingsPath) &&
			WritePrivateProfileStringA("VR",
				"QuestBaselineProfileVersion", "2", settingsPath);
		LOGI("Quest render scale baseline reset to 100%%: result=%s",
		     rescaled ? "applied" : "FAILED");
	}
#if MIAMIVR_STEAM_FRAME
	// Standard section 9: the Frame renders each eye at about the panel's own
	// 2160x2160. What the runtime recommends depends on SteamVR's per-app
	// resolution override (1728 by default, 2160 when the player sets 2160), so
	// the default is not a fixed scale but AUTO: RenderScalePercent 0 means
	// "scale the recommended size to about 2160 per eye", worked out here at
	// session start. The eye swapchain is created at that size and the scene
	// renders straight into it (nothing is rendered larger and blitted down).
	// A percentage the player picks in the menu is kept as it is. Settings
	// files from earlier Frame builds carry 100% (Quest baseline) or 125% (the
	// first Frame default) and 72 Hz; move them to AUTO and 120 Hz once.
	const int frameDisplayVersion = (int)GetPrivateProfileIntA(
		"VR", "FrameDisplayProfileVersion", 0, settingsPath);
	if(frameDisplayVersion < 2){
		bool moved =
			WritePrivateProfileStringA("VR", "RenderScalePercent",
				"0", settingsPath);
		if(frameDisplayVersion < 1)
			moved = WritePrivateProfileStringA("VR", "RefreshRate",
				"120", settingsPath) && moved;
		moved = WritePrivateProfileStringA("VR", "FrameDisplayProfileVersion",
			"2", settingsPath) && moved;
		LOGI("[vr] display defaults moved to AUTO render scale%s: %s",
		     frameDisplayVersion < 1 ? " and 120 Hz" : "",
		     moved ? "applied" : "FAILED");
	}
	int renderScalePercent = (int)GetPrivateProfileIntA(
		"VR", "RenderScalePercent", 0, settingsPath);
	if(renderScalePercent <= 0){
		const uint32_t rec = g.viewConfigs[0].recommendedImageRectWidth;
		const int wanted = rec > 0 ?
			(int)(((uint64_t)2160*100 + rec/2)/rec) : 100;
		renderScalePercent = wanted < 100 ? 100 : wanted > 175 ? 175 : wanted;
		LOGI("[vr] render scale AUTO: recommended %ux%u, target 2160 per eye, "
		     "scale %d%% (about %ux%u)",
		     rec, g.viewConfigs[0].recommendedImageRectHeight,
		     renderScalePercent,
		     (unsigned)(rec*(uint64_t)renderScalePercent/100),
		     (unsigned)(g.viewConfigs[0].recommendedImageRectHeight*
		                (uint64_t)renderScalePercent/100));
	}
#else
	int renderScalePercent = (int)GetPrivateProfileIntA(
		"VR", "RenderScalePercent", 100, settingsPath);
#endif
	if(renderScalePercent < 100)
		renderScalePercent = 100;
	else if(renderScalePercent > 175)
		renderScalePercent = 175;
	const uint32_t recommendedWidth =
		g.viewConfigs[0].recommendedImageRectWidth;
	const uint32_t recommendedHeight =
		g.viewConfigs[0].recommendedImageRectHeight;
	const uint32_t maxWidth = g.viewConfigs[0].maxImageRectWidth;
	const uint32_t maxHeight = g.viewConfigs[0].maxImageRectHeight;
	g.renderScaleRequestedPercent = renderScalePercent;
	g.renderScaleRecommendedWidth = recommendedWidth;
	g.renderScaleRecommendedHeight = recommendedHeight;
	g.renderScaleRuntimeMaxWidth = maxWidth;
	g.renderScaleRuntimeMaxHeight = maxHeight;
	g.renderScaleSelectedPresetPercent = 0;
	g.renderScaleEffectivePercent = 0.0f;
	g.renderScaleFallbackReason = RENDER_SCALE_FALLBACK_NONE;
	g.previousRenderScaleFallbackRequestedPercent =
		(int)GetPrivateProfileIntA("VR", "RenderScaleLastFallbackRequest",
			0, settingsPath);
	g.previousRenderScaleFallbackPercent =
		(int)GetPrivateProfileIntA("VR", "RenderScaleLastFallbackPercent",
			0, settingsPath);
	g.previousRenderScaleFallbackReason =
		(int)GetPrivateProfileIntA("VR", "RenderScaleLastFallbackReason",
			RENDER_SCALE_FALLBACK_NONE, settingsPath);
	if(recommendedWidth == 0 || recommendedHeight == 0 ||
	   maxWidth < recommendedWidth || maxHeight < recommendedHeight){
		LOGE("invalid OpenXR render extents: recommended %ux%u, max %ux%u",
		     recommendedWidth, recommendedHeight, maxWidth, maxHeight);
		return false;
	}
	LOGI("render scale requested: %d%% (recommended %ux%u, runtime max %ux%u)",
		renderScalePercent, recommendedWidth, recommendedHeight,
		maxWidth, maxHeight);

	uint32_t formatCount = 0;
	xrEnumerateSwapchainFormats(g.session, 0, &formatCount, nullptr);
	std::vector<int64_t> formats(formatCount);
	xrEnumerateSwapchainFormats(g.session, formatCount, &formatCount, formats.data());

	// UNORM before SRGB on purpose. The game writes colours that are already
	// sRGB-encoded, so an sRGB attachment would encode them a second time and
	// wash the whole image out. With UNORM the bytes reach the compositor
	// exactly as the game produced them.
	const int64_t preferred[] = {
		VK_FORMAT_R8G8B8A8_UNORM,
		VK_FORMAT_B8G8R8A8_UNORM,
		VK_FORMAT_R8G8B8A8_SRGB,
		VK_FORMAT_B8G8R8A8_SRGB,
	};
	g.swapchainFormat = 0;
	for(int64_t want : preferred){
		for(int64_t have : formats)
			if(have == want){
				g.swapchainFormat = want;
				break;
			}
		if(g.swapchainFormat != 0)
			break;
	}
	if(g.swapchainFormat == 0){
		LOGE("no supported swapchain format");
		return false;
	}
	g.swapchainViewFormat = g.swapchainFormat;
#if MIAMIVR_STEAM_FRAME
	// SteamVR on the Frame offers sRGB swapchains only. The post-FX resolve
	// writes colour that is already gamma-encoded (the game's colours are
	// sRGB values, and nothing here linearises them), so writing it through
	// an sRGB view would encode it a second time and wash the picture out.
	// Render through the UNORM view of the same bytes instead; the compositor
	// still decodes the image as sRGB, which is what the bytes are.
	bool mutableSrgb = false;
	if(g.swapchainFormat == VK_FORMAT_R8G8B8A8_SRGB){
		g.swapchainViewFormat = VK_FORMAT_R8G8B8A8_UNORM;
		mutableSrgb = true;
	}else if(g.swapchainFormat == VK_FORMAT_B8G8R8A8_SRGB){
		g.swapchainViewFormat = VK_FORMAT_B8G8R8A8_UNORM;
		mutableSrgb = true;
	}
	LOGI("[vr] eye swapchain format %lld, rendered through format %lld%s",
	     (long long)g.swapchainFormat, (long long)g.swapchainViewFormat,
	     mutableSrgb ? " (sRGB image, UNORM view)" : "");
#endif

	// One swapchain with two array layers, not two swapchains. Multiview reads
	// gl_ViewIndex to pick the layer, so both eyes come out of a single pass.
	XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
	                  XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
#if MIAMIVR_STEAM_FRAME
	// dump_frame reads the image back with vkCmdCopyImageToBuffer, which needs
	// TRANSFER_SRC. Ask for it only when a dump was requested so normal runs
	// keep the runtime's cheapest allocation.
	if(frame_diag::config().dumpFrame > 0)
		info.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
	// The UNORM view needs an image created mutable-format.
	if(mutableSrgb)
		info.usageFlags |= XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
#endif
	info.format = g.swapchainFormat;
	info.sampleCount = 1;
	info.faceCount = 1;
	info.arraySize = 2;
	info.mipCount = 1;

	// Keep the two axes on one common scale. Independently clamping width and
	// height can distort the projection whenever only one runtime maximum is
	// reached. If the runtime cannot allocate the requested swapchain, retain
	// the persisted request and retry the nearest lower menu presets for this
	// launch only.
	int scaleCandidates[4];
	int scaleCandidateCount = 0;
	scaleCandidates[scaleCandidateCount++] = renderScalePercent;
	static const int lowerScalePresets[] = { 150, 125, 100 };
	for(int preset : lowerScalePresets)
		if(preset < renderScalePercent)
			scaleCandidates[scaleCandidateCount++] = preset;

	uint32_t previousWidth = 0;
	uint32_t previousHeight = 0;
	int selectedScalePercent = 0;
	double selectedEffectiveScalePercent = 0.0;
	for(int candidateIndex = 0;
	    candidateIndex < scaleCandidateCount;
	    candidateIndex++){
		const int candidateScalePercent = scaleCandidates[candidateIndex];
		double uniformScale = (double)candidateScalePercent/100.0;
		const double maxWidthScale = (double)maxWidth/recommendedWidth;
		const double maxHeightScale = (double)maxHeight/recommendedHeight;
		if(uniformScale > maxWidthScale)
			uniformScale = maxWidthScale;
		if(uniformScale > maxHeightScale)
			uniformScale = maxHeightScale;

		uint32_t candidateWidth =
			(uint32_t)(recommendedWidth*uniformScale+0.5);
		uint32_t candidateHeight =
			(uint32_t)(recommendedHeight*uniformScale+0.5);
		// Rounding should already respect the common maximum scale. Keep the
		// final values defensive without deriving a second per-axis scale.
		if(candidateWidth > maxWidth)
			candidateWidth = maxWidth;
		if(candidateHeight > maxHeight)
			candidateHeight = maxHeight;
		const double widthScale =
			(double)candidateWidth/recommendedWidth;
		const double heightScale =
			(double)candidateHeight/recommendedHeight;
		const double candidateEffectiveScalePercent =
			(widthScale < heightScale ? widthScale : heightScale)*100.0;

		if(candidateWidth == previousWidth &&
		   candidateHeight == previousHeight)
			continue;
		previousWidth = candidateWidth;
		previousHeight = candidateHeight;

		info.width = candidateWidth;
		info.height = candidateHeight;
		XrSwapchain candidateSwapchain = XR_NULL_HANDLE;
		XrResult createResult =
			xrCreateSwapchain(g.session, &info, &candidateSwapchain);
#if MIAMIVR_STEAM_FRAME
		if(XR_FAILED(createResult) &&
		   (info.usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT) != 0){
			// A runtime that refuses TRANSFER_SRC must not cost the render
			// scale: drop the dump rather than stepping down a preset.
			LOGW("[vr-dump] swapchain with TRANSFER_SRC failed (%d); "
			     "dump_frame disabled", (int)createResult);
			info.usageFlags &= ~XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
			createResult =
				xrCreateSwapchain(g.session, &info, &candidateSwapchain);
		}
#endif
		if(XR_SUCCEEDED(createResult)){
#if MIAMIVR_STEAM_FRAME
			g.swapchainTransferSrc =
				(info.usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT) != 0;
#endif
			g.swapchain = candidateSwapchain;
			g.renderWidth = candidateWidth;
			g.renderHeight = candidateHeight;
			selectedScalePercent = candidateScalePercent;
			selectedEffectiveScalePercent =
				candidateEffectiveScalePercent;
			break;
		}
		LOGW("xrCreateSwapchain %ux%u (request %d%%, candidate %d%%, "
		     "effective %.2f%%) failed: %d; trying a lower preset",
			candidateWidth, candidateHeight, renderScalePercent,
			candidateScalePercent, candidateEffectiveScalePercent,
			(int)createResult);
	}
	if(g.swapchain == XR_NULL_HANDLE){
		LOGE("xrCreateSwapchain failed for requested %d%% and all lower presets",
		     renderScalePercent);
		return false;
	}
	g.renderScaleSelectedPresetPercent = selectedScalePercent;
	g.renderScaleEffectivePercent = (float)selectedEffectiveScalePercent;
	if(selectedScalePercent < renderScalePercent)
		g.renderScaleFallbackReason =
			RENDER_SCALE_FALLBACK_SWAPCHAIN_ALLOCATION;
	else if(selectedEffectiveScalePercent+0.5 <
	        (double)renderScalePercent)
		g.renderScaleFallbackReason =
			RENDER_SCALE_FALLBACK_RUNTIME_LIMIT;
	const bool renderScaleFallback =
		g.renderScaleFallbackReason != RENDER_SCALE_FALLBACK_NONE;
	// Keep the user's request intact. The menu exposes REQUEST and ACTIVE
	// separately and highlights this fallback in red. Retrying this OpenXR
	// allocation next launch is safe because it already recovered in-process.
	// Only a later librw allocation failure writes emergency 100% for recovery.
	if(g.renderScaleFallbackReason ==
	   RENDER_SCALE_FALLBACK_SWAPCHAIN_ALLOCATION){
		char requestText[16], actualText[16], reasonText[16];
		snprintf(requestText, sizeof(requestText), "%d", renderScalePercent);
		snprintf(actualText, sizeof(actualText), "%d", selectedScalePercent);
		snprintf(reasonText, sizeof(reasonText), "%d",
			g.renderScaleFallbackReason);
		const bool saved =
			WritePrivateProfileStringA("VR", "RenderScaleLastFallbackRequest",
				requestText, settingsPath) &&
			WritePrivateProfileStringA("VR", "RenderScaleLastFallbackPercent",
				actualText, settingsPath) &&
			WritePrivateProfileStringA("VR", "RenderScaleLastFallbackReason",
				reasonText, settingsPath);
		g.previousRenderScaleFallbackRequestedPercent = renderScalePercent;
		g.previousRenderScaleFallbackPercent = selectedScalePercent;
		g.previousRenderScaleFallbackReason = g.renderScaleFallbackReason;
		if(!saved)
			LOGE("could not persist render-scale fallback status %d%% -> %d%%",
			     renderScalePercent, selectedScalePercent);
		else
			LOGW("render-scale fallback recorded without changing request: %d%% -> %d%%",
			     renderScalePercent, selectedScalePercent);
	}
	LOGI("per-eye render target: %ux%u (requested %d%%, selected %d%%, "
	     "effective %.2f%%, fallback %s)",
		g.renderWidth, g.renderHeight, renderScalePercent,
		selectedScalePercent, selectedEffectiveScalePercent,
		getRenderScaleFallbackReasonName(g.renderScaleFallbackReason));
	if(renderScaleFallback)
		LOGW("render scale fallback active: request=%d%% actual=%.2f%% "
		     "reason=%s",
			renderScalePercent, selectedEffectiveScalePercent,
			getRenderScaleFallbackReasonName(g.renderScaleFallbackReason));

	uint32_t imageCount = 0;
	xrEnumerateSwapchainImages(g.swapchain, 0, &imageCount, nullptr);
	std::vector<XrSwapchainImageVulkan2KHR> xrImages(
		imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR });
	XR_CHECK(xrEnumerateSwapchainImages(
	             g.swapchain, imageCount, &imageCount,
	             (XrSwapchainImageBaseHeader *)xrImages.data()),
	         "xrEnumerateSwapchainImages");

	g.images.resize(imageCount);
	for(uint32_t i = 0; i < imageCount; i++)
		g.images[i].image = xrImages[i].image;
	LOGI("swapchain: %u images, format %lld", imageCount, (long long)g.swapchainFormat);

	// UI overlay swapchain. It accommodates both the compact 512x128 debug
	// strip and the desktop-sized 1024x768 menu without recreating an OpenXR
	// swapchain while the menu is being opened.
	{
		g.debugWidth = 1024;
		g.debugHeight = 768;
		XrSwapchainCreateInfo dinfo = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		dinfo.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
		                   XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
		dinfo.format = g.swapchainFormat;
		dinfo.sampleCount = 1;
		dinfo.width = (uint32_t)g.debugWidth;
		dinfo.height = (uint32_t)g.debugHeight;
		dinfo.faceCount = 1;
		dinfo.arraySize = 1;
		dinfo.mipCount = 1;
		if(XR_SUCCEEDED(xrCreateSwapchain(g.session, &dinfo, &g.debugSwapchain))){
			uint32_t count = 0;
			xrEnumerateSwapchainImages(g.debugSwapchain, 0, &count, nullptr);
			std::vector<XrSwapchainImageVulkan2KHR> imgs(
				count, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR });
			if(XR_SUCCEEDED(xrEnumerateSwapchainImages(
			       g.debugSwapchain, count, &count,
			       (XrSwapchainImageBaseHeader *)imgs.data()))){
				g.debugImages.resize(count);
				for(uint32_t i = 0; i < count; i++)
					g.debugImages[i] = imgs[i].image;
			}
		}else{
			LOGE("debug overlay swapchain unavailable");
			g.debugSwapchain = XR_NULL_HANDLE;
		}
	}
#if MIAMIVR_STEAM_FRAME
	// Laser pointer artwork: 192 x 64, see buildPointerAtlas. Without it the
	// pointer is simply absent and the menus work from the pad.
	{
		XrSwapchainCreateInfo pinfo = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		pinfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
		                   XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
		                   XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
		pinfo.format = g.swapchainFormat;
		pinfo.sampleCount = 1;
		pinfo.width = 192;
		pinfo.height = 64;
		pinfo.faceCount = 1;
		pinfo.arraySize = 1;
		pinfo.mipCount = 1;
		if(XR_SUCCEEDED(xrCreateSwapchain(g.session, &pinfo, &g.pointerSwapchain))){
			uint32_t count = 0;
			xrEnumerateSwapchainImages(g.pointerSwapchain, 0, &count, nullptr);
			std::vector<XrSwapchainImageVulkan2KHR> imgs(
				count, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR });
			if(count > 0 && XR_SUCCEEDED(xrEnumerateSwapchainImages(
			       g.pointerSwapchain, count, &count,
			       (XrSwapchainImageBaseHeader *)imgs.data()))){
				g.pointerImages.resize(count);
				for(uint32_t i = 0; i < count; i++)
					g.pointerImages[i] = imgs[i].image;
				LOGI("[ui.pointer] swapchain 192x64, %u images", count);
			}else{
				LOGE("[ui.pointer] swapchain images unavailable; no pointer");
				xrDestroySwapchain(g.pointerSwapchain);
				g.pointerSwapchain = XR_NULL_HANDLE;
			}
		}else{
			LOGE("[ui.pointer] swapchain create failed; no pointer");
			g.pointerSwapchain = XR_NULL_HANDLE;
		}
	}
#endif
	return true;
}

bool
createDepthBuffer(void)
{
	const VkFormat candidates[] = {
		VK_FORMAT_D24_UNORM_S8_UINT,
		VK_FORMAT_D32_SFLOAT_S8_UINT,
		VK_FORMAT_D32_SFLOAT,
		VK_FORMAT_D16_UNORM,
	};
	bool chosen = false;
	for(VkFormat format : candidates){
		VkFormatProperties properties;
		vkGetPhysicalDeviceFormatProperties(g.physicalDevice, format, &properties);
		if(properties.optimalTilingFeatures &
		   VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT){
			g.depthFormat = format;
			chosen = true;
			break;
		}
	}
	if(!chosen){
		LOGE("no usable depth format");
		return false;
	}

	VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = g.depthFormat;
	imageInfo.extent = { g.renderWidth, g.renderHeight, 1 };
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 2;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VK_CHECK(vkCreateImage(g.device, &imageInfo, nullptr, &g.depthImage), "vkCreateImage(depth)");

	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(g.device, g.depthImage, &requirements);
	uint32_t typeIndex = 0;
	if(!findMemoryType(requirements.memoryTypeBits,
	                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &typeIndex))
		return false;

	VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	allocInfo.allocationSize = requirements.size;
	allocInfo.memoryTypeIndex = typeIndex;
	VK_CHECK(vkAllocateMemory(g.device, &allocInfo, nullptr, &g.depthMemory),
	         "vkAllocateMemory(depth)");
	VK_CHECK(vkBindImageMemory(g.device, g.depthImage, g.depthMemory, 0),
	         "vkBindImageMemory(depth)");

	VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	viewInfo.image = g.depthImage;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	viewInfo.format = g.depthFormat;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = 2;
	VK_CHECK(vkCreateImageView(g.device, &viewInfo, nullptr, &g.depthView),
	         "vkCreateImageView(depth)");
	return true;
}

bool
createRenderPass(void)
{
	VkAttachmentDescription attachments[2] = {};
	attachments[0].format = (VkFormat)g.swapchainViewFormat;
	attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	attachments[1].format = g.depthFormat;
	attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	// Depth is never read after the pass, and discarding it saves the tiler a
	// full writeback of a two-layer depth surface every frame.
	attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkAttachmentReference colourRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference depthRef = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

	VkSubpassDescription subpass = {};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colourRef;
	subpass.pDepthStencilAttachment = &depthRef;

	VkSubpassDependency dependency = {};
	dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	dependency.dstSubpass = 0;
	dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
	                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
	                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	dependency.srcAccessMask = 0;
	dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
	                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

	// Both views in one subpass. The correlation mask tells the driver the two
	// views are spatially coherent, which is what lets the tiler share work.
	const uint32_t viewMask = 0x3;
	const uint32_t correlationMask = 0x3;
	VkRenderPassMultiviewCreateInfo multiview = {
		VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO };
	multiview.subpassCount = 1;
	multiview.pViewMasks = &viewMask;
	multiview.correlationMaskCount = 1;
	multiview.pCorrelationMasks = &correlationMask;

	VkRenderPassCreateInfo info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
	info.pNext = &multiview;
	info.attachmentCount = 2;
	info.pAttachments = attachments;
	info.subpassCount = 1;
	info.pSubpasses = &subpass;
	info.dependencyCount = 1;
	info.pDependencies = &dependency;
	VK_CHECK(vkCreateRenderPass(g.device, &info, nullptr, &g.renderPass), "vkCreateRenderPass");
	return true;
}

bool
createPipeline(void)
{
	VkDescriptorSetLayoutBinding uboBinding = {};
	uboBinding.binding = 0;
	uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	uboBinding.descriptorCount = 1;
	uboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

	VkDescriptorSetLayoutCreateInfo layoutInfo = {
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	layoutInfo.bindingCount = 1;
	layoutInfo.pBindings = &uboBinding;
	VK_CHECK(vkCreateDescriptorSetLayout(g.device, &layoutInfo, nullptr,
	                                     &g.descriptorSetLayout),
	         "vkCreateDescriptorSetLayout");

	VkPushConstantRange pushRange = {};
	pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	pushRange.offset = 0;
	pushRange.size = sizeof(PushConstants);

	VkPipelineLayoutCreateInfo pipelineLayoutInfo = {
		VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pSetLayouts = &g.descriptorSetLayout;
	pipelineLayoutInfo.pushConstantRangeCount = 1;
	pipelineLayoutInfo.pPushConstantRanges = &pushRange;
	VK_CHECK(vkCreatePipelineLayout(g.device, &pipelineLayoutInfo, nullptr,
	                                &g.pipelineLayout),
	         "vkCreatePipelineLayout");

	VkShaderModule vertexModule = createShaderModule(kBringupVertSpv, sizeof(kBringupVertSpv));
	VkShaderModule fragmentModule = createShaderModule(kBringupFragSpv, sizeof(kBringupFragSpv));
	if(vertexModule == VK_NULL_HANDLE || fragmentModule == VK_NULL_HANDLE)
		return false;

	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertexModule;
	stages[0].pName = "main";
	stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragmentModule;
	stages[1].pName = "main";

	VkVertexInputBindingDescription binding = { 0, sizeof(Vertex),
	                                            VK_VERTEX_INPUT_RATE_VERTEX };
	VkVertexInputAttributeDescription attributes[2] = {
		{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, (uint32_t)offsetof(Vertex, position) },
		{ 1, 0, VK_FORMAT_R32G32B32_SFLOAT, (uint32_t)offsetof(Vertex, normal) },
	};

	VkPipelineVertexInputStateCreateInfo vertexInput = {
		VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	vertexInput.vertexBindingDescriptionCount = 1;
	vertexInput.pVertexBindingDescriptions = &binding;
	vertexInput.vertexAttributeDescriptionCount = 2;
	vertexInput.pVertexAttributeDescriptions = attributes;

	VkPipelineInputAssemblyStateCreateInfo inputAssembly = {
		VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo viewportState = {
		VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rasteriser = {
		VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	rasteriser.polygonMode = VK_POLYGON_MODE_FILL;
	rasteriser.cullMode = VK_CULL_MODE_BACK_BIT;
	// Vulkan's flipped Y makes the winding of the source geometry come out
	// clockwise on screen.
	rasteriser.frontFace = VK_FRONT_FACE_CLOCKWISE;
	rasteriser.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisample = {
		VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo depthStencil = {
		VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	depthStencil.depthTestEnable = VK_TRUE;
	depthStencil.depthWriteEnable = VK_TRUE;
	depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

	VkPipelineColorBlendAttachmentState blendAttachment = {};
	blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

	VkPipelineColorBlendStateCreateInfo colourBlend = {
		VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	colourBlend.attachmentCount = 1;
	colourBlend.pAttachments = &blendAttachment;

	const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT,
	                                         VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamicState = {
		VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dynamicState.dynamicStateCount = 2;
	dynamicState.pDynamicStates = dynamicStates;

	VkGraphicsPipelineCreateInfo info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	info.stageCount = 2;
	info.pStages = stages;
	info.pVertexInputState = &vertexInput;
	info.pInputAssemblyState = &inputAssembly;
	info.pViewportState = &viewportState;
	info.pRasterizationState = &rasteriser;
	info.pMultisampleState = &multisample;
	info.pDepthStencilState = &depthStencil;
	info.pColorBlendState = &colourBlend;
	info.pDynamicState = &dynamicState;
	info.layout = g.pipelineLayout;
	info.renderPass = g.renderPass;
	info.subpass = 0;

	VkResult result = vkCreateGraphicsPipelines(g.device, VK_NULL_HANDLE, 1, &info,
	                                            nullptr, &g.pipeline);
	vkDestroyShaderModule(g.device, vertexModule, nullptr);
	vkDestroyShaderModule(g.device, fragmentModule, nullptr);
	VK_CHECK(result, "vkCreateGraphicsPipelines");
	return true;
}

bool
createSwapchainImageViews(void)
{
	VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	poolInfo.queueFamilyIndex = g.queueFamily;
	VK_CHECK(vkCreateCommandPool(g.device, &poolInfo, nullptr, &g.commandPool),
	         "vkCreateCommandPool");

	for(SwapchainImage &image : g.images){
		VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		viewInfo.image = image.image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
		viewInfo.format = (VkFormat)g.swapchainViewFormat;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.layerCount = 2;
		VK_CHECK(vkCreateImageView(g.device, &viewInfo, nullptr, &image.view),
		         "vkCreateImageView(colour)");
	}
	return true;
}

bool
createPerImageBringupResources(void)
{
	const uint32_t imageCount = (uint32_t)g.images.size();

	VkDescriptorPoolSize poolSize = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, imageCount };
	VkDescriptorPoolCreateInfo descriptorPoolInfo = {
		VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	descriptorPoolInfo.maxSets = imageCount;
	descriptorPoolInfo.poolSizeCount = 1;
	descriptorPoolInfo.pPoolSizes = &poolSize;
	VK_CHECK(vkCreateDescriptorPool(g.device, &descriptorPoolInfo, nullptr,
	                                &g.descriptorPool),
	         "vkCreateDescriptorPool");

	for(uint32_t i = 0; i < imageCount; i++){
		SwapchainImage &image = g.images[i];

		const VkImageView attachments[2] = { image.view, g.depthView };
		VkFramebufferCreateInfo framebufferInfo = {
			VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
		framebufferInfo.renderPass = g.renderPass;
		framebufferInfo.attachmentCount = 2;
		framebufferInfo.pAttachments = attachments;
		framebufferInfo.width = g.renderWidth;
		framebufferInfo.height = g.renderHeight;
		// Multiview drives the layer count through the view mask, so the
		// framebuffer itself is declared single-layer.
		framebufferInfo.layers = 1;
		VK_CHECK(vkCreateFramebuffer(g.device, &framebufferInfo, nullptr,
		                             &image.framebuffer),
		         "vkCreateFramebuffer");

		VkCommandBufferAllocateInfo commandInfo = {
			VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		commandInfo.commandPool = g.commandPool;
		commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		commandInfo.commandBufferCount = 1;
		VK_CHECK(vkAllocateCommandBuffers(g.device, &commandInfo, &image.commandBuffer),
		         "vkAllocateCommandBuffers");

		VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		VK_CHECK(vkCreateFence(g.device, &fenceInfo, nullptr, &image.fence),
		         "vkCreateFence");

		if(!createBuffer(sizeof(Mat4) * 2, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		                 &image.uniformBuffer, &image.uniformMemory))
			return false;
		VK_CHECK(vkMapMemory(g.device, image.uniformMemory, 0, sizeof(Mat4) * 2, 0,
		                     &image.uniformMapped),
		         "vkMapMemory(uniform)");

		VkDescriptorSetAllocateInfo setInfo = {
			VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		setInfo.descriptorPool = g.descriptorPool;
		setInfo.descriptorSetCount = 1;
		setInfo.pSetLayouts = &g.descriptorSetLayout;
		VK_CHECK(vkAllocateDescriptorSets(g.device, &setInfo, &image.descriptorSet),
		         "vkAllocateDescriptorSets");

		VkDescriptorBufferInfo bufferInfo = { image.uniformBuffer, 0, sizeof(Mat4) * 2 };
		VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
		write.dstSet = image.descriptorSet;
		write.dstBinding = 0;
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		write.pBufferInfo = &bufferInfo;
		vkUpdateDescriptorSets(g.device, 1, &write, 0, nullptr);
	}
	return true;
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

void
drawScene(VkCommandBuffer commandBuffer)
{
	struct Instance { float x, y, z, sx, sy, sz, r, gr, b; };
	// A floor slab, a marker directly ahead, and a ring the head can be turned
	// through. Enough to read tracking, stereo separation and depth by eye.
	static const Instance instances[] = {
		{  0.0f, -0.05f,  0.0f, 8.0f, 0.1f, 8.0f, 0.16f, 0.17f, 0.22f },
		{  0.0f,  1.2f,  -2.0f, 0.3f, 0.3f, 0.3f, 0.95f, 0.35f, 0.55f },
		{  2.0f,  0.5f,   0.0f, 0.4f, 1.0f, 0.4f, 0.25f, 0.70f, 0.95f },
		{ -2.0f,  0.5f,   0.0f, 0.4f, 1.0f, 0.4f, 0.25f, 0.70f, 0.95f },
		{  0.0f,  0.5f,   2.0f, 0.4f, 1.0f, 0.4f, 0.95f, 0.75f, 0.25f },
		{  1.4f,  0.5f,  -1.4f, 0.4f, 1.0f, 0.4f, 0.40f, 0.90f, 0.45f },
		{ -1.4f,  0.5f,  -1.4f, 0.4f, 1.0f, 0.4f, 0.40f, 0.90f, 0.45f },
		{  1.4f,  0.5f,   1.4f, 0.4f, 1.0f, 0.4f, 0.85f, 0.85f, 0.35f },
		{ -1.4f,  0.5f,   1.4f, 0.4f, 1.0f, 0.4f, 0.85f, 0.85f, 0.35f },
	};

	for(const Instance &instance : instances){
		PushConstants push;
		push.model = translationScale(instance.x, instance.y, instance.z,
		                              instance.sx, instance.sy, instance.sz);
		push.tint[0] = instance.r;
		push.tint[1] = instance.gr;
		push.tint[2] = instance.b;
		push.tint[3] = 1.0f;
		vkCmdPushConstants(commandBuffer, g.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
		                   0, sizeof(push), &push);
		vkCmdDrawIndexed(commandBuffer,
		                 (uint32_t)(sizeof(kCubeIndices)/sizeof(kCubeIndices[0])),
		                 1, 0, 0, 0);
	}
}

#if MIAMIVR_STEAM_FRAME
// ---------------------------------------------------------------------------
// Menu laser pointer (standard.md section 3). A ray from the right controller's
// aim pose is tested against the theater screen (while the frontend is on it)
// and the VR menu panel, using the quads exactly as this frame submits them,
// and the nearest hit wins. The beam and the dot are two alpha-blended quad
// layers submitted last, so SteamVR composites them over everything else.
// ---------------------------------------------------------------------------
static inline XrVector3f ptAdd(XrVector3f a, XrVector3f b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
static inline XrVector3f ptSub(XrVector3f a, XrVector3f b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static inline XrVector3f ptScale(XrVector3f a, float k) { return { a.x*k, a.y*k, a.z*k }; }
static inline float ptDot(XrVector3f a, XrVector3f b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline XrVector3f ptCross(XrVector3f a, XrVector3f b)
{
	return { a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
}
static inline XrVector3f ptNorm(XrVector3f a, XrVector3f fallback)
{
	const float l = sqrtf(ptDot(a, a));
	return (isfinite(l) && l > 1e-6f) ? ptScale(a, 1.0f/l) : fallback;
}
static inline XrVector3f ptRotate(const XrQuaternionf &q, XrVector3f v)
{
	const XrVector3f u = { q.x, q.y, q.z };
	const XrVector3f t2 = ptScale(ptCross(u, v), 2.0f);
	return ptAdd(ptAdd(v, ptScale(t2, q.w)), ptCross(u, t2));
}
static inline XrQuaternionf ptConj(const XrQuaternionf &q) { return { -q.x, -q.y, -q.z, q.w }; }

// Rotation with the given axes as columns, as a quaternion.
static XrQuaternionf
ptFromAxes(XrVector3f x, XrVector3f y, XrVector3f z)
{
	const float m00 = x.x, m10 = x.y, m20 = x.z;
	const float m01 = y.x, m11 = y.y, m21 = y.z;
	const float m02 = z.x, m12 = z.y, m22 = z.z;
	XrQuaternionf q = { 0.0f, 0.0f, 0.0f, 1.0f };
	const float trace = m00 + m11 + m22;
	if(trace > 0.0f){
		const float k = 0.5f/sqrtf(trace + 1.0f);
		q = { (m21 - m12)*k, (m02 - m20)*k, (m10 - m01)*k, 0.25f/k };
	}else if(m00 > m11 && m00 > m22){
		const float k = 2.0f*sqrtf(1.0f + m00 - m11 - m22);
		q = { 0.25f*k, (m01 + m10)/k, (m02 + m20)/k, (m21 - m12)/k };
	}else if(m11 > m22){
		const float k = 2.0f*sqrtf(1.0f + m11 - m00 - m22);
		q = { (m01 + m10)/k, 0.25f*k, (m12 + m21)/k, (m02 - m20)/k };
	}else{
		const float k = 2.0f*sqrtf(1.0f + m22 - m00 - m11);
		q = { (m02 + m20)/k, (m12 + m21)/k, 0.25f*k, (m10 - m01)/k };
	}
	const float n = sqrtf(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
	if(n > 1e-6f)
		q = { q.x/n, q.y/n, q.z/n, q.w/n };
	return q;
}

// Ray against a quad (+Z towards the viewer, X right, Y up). u from the left,
// v from the top, both 0..1 on a hit.
static bool
ptRayQuad(const XrPosef &pose, const XrExtent2Df &size, XrVector3f o, XrVector3f d,
          float *tOut, float *uOut, float *vOut)
{
	if(size.width <= 0.0f || size.height <= 0.0f)
		return false;
	const XrQuaternionf inv = ptConj(pose.orientation);
	const XrVector3f lo = ptRotate(inv, ptSub(o, pose.position));
	const XrVector3f ld = ptRotate(inv, d);
	if(ld.z >= -1e-4f)
		return false;                      // pointing away from the face
	const float t = -lo.z/ld.z;
	if(!(t > 0.0f))
		return false;
	const float hx = lo.x + t*ld.x;
	const float hy = lo.y + t*ld.y;
	if(fabsf(hx) > size.width*0.5f || fabsf(hy) > size.height*0.5f)
		return false;
	*tOut = t;
	*uOut = hx/size.width + 0.5f;
	*vOut = 0.5f - hy/size.height;
	return true;
}

// Pointer artwork, 192 x 64, straight alpha, row 0 at the top:
//   x   0..63   cursor dot, idle (cyan)
//   x  64..127  cursor dot, trigger down (amber)
//   x 128..191  beam strip, rows 28..35 (alpha ramps up towards the hit point)
static const int kPointerTexW = 192, kPointerTexH = 64;
static const int kPointerBeamRow = 28, kPointerBeamRows = 8;

static float
ptSmooth(float a, float b, float x)
{
	float k = (x - a)/(b - a);
	k = k < 0.0f ? 0.0f : k > 1.0f ? 1.0f : k;
	return k*k*(3.0f - 2.0f*k);
}

// RGBA byte order; uploadPointerAtlas swaps for BGRA swapchains.
static void
buildPointerAtlas(uint8_t *px)
{
	memset(px, 0, (size_t)kPointerTexW*kPointerTexH*4);
	auto put = [&](int x, int y, float r, float gr, float b, float a){
		auto byte = [](float v){
			return (uint8_t)((v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v)*255.0f + 0.5f);
		};
		uint8_t *d = px + ((size_t)y*kPointerTexW + x)*4;
		d[0] = byte(r); d[1] = byte(gr); d[2] = byte(b); d[3] = byte(a);
	};
	for(int cell = 0; cell < 2; cell++){
		const float cr = cell == 0 ? 0.62f : 1.00f;
		const float cg = cell == 0 ? 0.93f : 0.78f;
		const float cb = cell == 0 ? 1.00f : 0.35f;
		for(int y = 0; y < 64; y++){
			for(int x = 0; x < 64; x++){
				const float dx = (x + 0.5f - 32.0f)/32.0f;
				const float dy = (y + 0.5f - 32.0f)/32.0f;
				const float r = sqrtf(dx*dx + dy*dy);
				// A dark halo for contrast on bright panels, a bright disc on top.
				const float halo = 0.80f*(1.0f - ptSmooth(0.56f, 0.82f, r));
				const float disc = 1.0f - ptSmooth(0.36f, 0.44f, r);
				const float a = disc + halo*(1.0f - disc);
				if(a <= 0.0f)
					continue;
				const float k = disc/a;
				put(cell*64 + x, y, cr*k + 0.03f*(1.0f - k),
				    cg*k + 0.06f*(1.0f - k), cb*k + 0.12f*(1.0f - k), a);
			}
		}
	}
	for(int row = 0; row < kPointerBeamRows; row++){
		const float dy = fabsf((row + 0.5f) - kPointerBeamRows*0.5f)/(kPointerBeamRows*0.5f);
		const float edge = 1.0f - ptSmooth(0.25f, 1.0f, dy);
		for(int x = 0; x < 64; x++){
			const float ramp = 0.30f + 0.65f*((float)x/63.0f);
			put(128 + x, kPointerBeamRow + row, 0.70f, 0.95f, 1.0f, ramp*edge);
		}
	}
}

static const uint8_t *
pointerAtlas(void)
{
	static std::vector<uint8_t> atlas;
	if(atlas.empty()){
		atlas.resize((size_t)kPointerTexW*kPointerTexH*4);
		buildPointerAtlas(atlas.data());
	}
	return atlas.data();
}

// Where the right hand's ray starts and points. With vr-diag.ini
// diag_pointer set and nobody holding a controller, a synthetic hand a little
// below and to the right of the head aims at a chosen point on a surface.
static bool
pointerAim(const XrPosef &head, bool theaterSurface, const XrPosef &theaterPose,
           bool menuSurface, const XrPosef &menuPose, const XrExtent2Df &menuSize,
           XrVector3f *origin, XrVector3f *dir)
{
	const frame_diag::Config &cfg = frame_diag::config();
	if(cfg.pointerTarget != 0 &&
	   ((cfg.pointerTarget == 1 && theaterSurface) ||
	    (cfg.pointerTarget == 2 && menuSurface))){
		const XrPosef &pose = cfg.pointerTarget == 1 ? theaterPose : menuPose;
		const XrExtent2Df size = cfg.pointerTarget == 1 ?
			XrExtent2Df{ kTheaterScreenWidth, kTheaterScreenHeight } : menuSize;
		const XrVector3f local = { (cfg.pointerU - 0.5f)*size.width,
		                           (0.5f - cfg.pointerV)*size.height, 0.0f };
		const XrVector3f target = ptAdd(pose.position, ptRotate(pose.orientation, local));
		const XrVector3f hand = ptAdd(head.position,
			ptRotate(head.orientation, { 0.20f, -0.30f, -0.15f }));
		*origin = hand;
		*dir = ptNorm(ptSub(target, hand), { 0.0f, 0.0f, -1.0f });
		return true;
	}
	const TrackedPose &aim = g.input.aimPose[1];
	if(!aim.valid)
		return false;
	const XrQuaternionf q = { aim.orientation[0], aim.orientation[1],
	                          aim.orientation[2], aim.orientation[3] };
	*origin = { aim.position[0], aim.position[1], aim.position[2] };
	*dir = ptNorm(ptRotate(q, { 0.0f, 0.0f, -1.0f }), { 0.0f, 0.0f, -1.0f });
	return true;
}

// Works out the pointer for this frame from the layers just built. The result
// reaches the game one frame later, through getInput.
static void
updatePointer(const XrView views[2], bool theaterSurface, const XrPosef &theaterPose,
              bool menuSurface, const XrPosef &menuPose, const XrExtent2Df &menuSize)
{
	ControllerInput &in = g.input;
	in.pointerPressed = false;
	in.pointerFrontend = false;
	in.pointerMenu = false;
	g.ptrActive = false;
	g.ptrHit = false;

	// Trigger with hysteresis. Kept current even while nothing is a target, so
	// holding it as a menu opens does not read as a fresh click.
	const float trigger = in.rightTrigger;
	g.pointerDown = g.pointerDown ? trigger > 0.45f : trigger > 0.6f;

	const bool frontendTarget = theaterSurface && g.pointerFrontendUp;
	if(!frontendTarget && !menuSurface)
		return;

	XrPosef head = views[0].pose;
	head.position.x = (views[0].pose.position.x + views[1].pose.position.x)*0.5f;
	head.position.y = (views[0].pose.position.y + views[1].pose.position.y)*0.5f;
	head.position.z = (views[0].pose.position.z + views[1].pose.position.z)*0.5f;

	XrVector3f origin, dir;
	if(!pointerAim(head, frontendTarget, theaterPose, menuSurface, menuPose, menuSize,
	               &origin, &dir))
		return;

	const XrExtent2Df theaterSize = { kTheaterScreenWidth, kTheaterScreenHeight };
	float tT = 0, uT = 0, vT = 0, tM = 0, uM = 0, vM = 0;
	const bool hitTheater = frontendTarget &&
		ptRayQuad(theaterPose, theaterSize, origin, dir, &tT, &uT, &vT);
	const bool hitMenu = menuSurface &&
		ptRayQuad(menuPose, menuSize, origin, dir, &tM, &uM, &vM);

	g.ptrActive = true;
	g.ptrOrigin = origin;
	g.ptrDir = dir;
	g.ptrHead = head.position;
	g.ptrHeadValid = true;
	in.pointerPressed = g.pointerDown;

	const char *target = "none";
	if(hitMenu && (!hitTheater || tM <= tT)){
		g.ptrHit = true;
		g.ptrHitPos = ptAdd(origin, ptScale(dir, tM));
		g.ptrHitOrientation = menuPose.orientation;
		in.pointerMenu = true;
		in.menuU = uM;
		in.menuV = vM;
		target = "vrmenu";
	}else if(hitTheater){
		g.ptrHit = true;
		g.ptrHitPos = ptAdd(origin, ptScale(dir, tT));
		g.ptrHitOrientation = theaterPose.orientation;
		in.pointerFrontend = true;
		in.frontendU = uT;
		in.frontendV = vT;
		target = "frontend";
	}
	static const char *lastTarget = "";
	if(strcmp(lastTarget, target) != 0){
		lastTarget = target;
		LOGI("[ui.pointer] target=%s", target);
	}
}

// Builds the beam and dot layers; returns how many. Each frame the artwork is
// uploaded again (48 KB, only while a menu is up), so nothing depends on a
// swapchain image keeping its contents between acquisitions.
static int
presentPointerLayers(XrSpace space, XrCompositionLayerQuad quads[2])
{
	if(!g.ptrActive || g.pointerSwapchain == XR_NULL_HANDLE ||
	   g.commandPool == VK_NULL_HANDLE)
		return 0;

	const VkDeviceSize bytes = (VkDeviceSize)kPointerTexW*kPointerTexH*4;
	if(g.pointerStaging == VK_NULL_HANDLE){
		if(!createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		                 &g.pointerStaging, &g.pointerStagingMemory))
			return 0;
		if(vkMapMemory(g.device, g.pointerStagingMemory, 0, bytes, 0,
		               &g.pointerStagingMapped) != VK_SUCCESS)
			return 0;
		VkCommandBufferAllocateInfo commandInfo = {};
		commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		commandInfo.commandPool = g.commandPool;
		commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		commandInfo.commandBufferCount = 1;
		vkAllocateCommandBuffers(g.device, &commandInfo, &g.pointerCommand);
		VkFenceCreateInfo fenceInfo = {};
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		vkCreateFence(g.device, &fenceInfo, nullptr, &g.pointerFence);
	}
	if(g.pointerStagingMapped == nullptr || g.pointerCommand == VK_NULL_HANDLE ||
	   g.pointerFence == VK_NULL_HANDLE)
		return 0;
	if(g.pointerSubmissionPending){
		// A tiny copy: it is done long before this frame comes round again.
		if(vkWaitForFences(g.device, 1, &g.pointerFence, VK_TRUE, 4000000) != VK_SUCCESS)
			return 0;
		g.pointerSubmissionPending = false;
	}

	const bool bgra = (VkFormat)g.swapchainFormat == VK_FORMAT_B8G8R8A8_UNORM ||
	                  (VkFormat)g.swapchainFormat == VK_FORMAT_B8G8R8A8_SRGB;
	const uint8_t *atlas = pointerAtlas();
	uint8_t *dst = (uint8_t *)g.pointerStagingMapped;
	memcpy(dst, atlas, (size_t)bytes);
	if(bgra)
		for(size_t i = 0; i < (size_t)bytes; i += 4){
			const uint8_t t = dst[i];
			dst[i] = dst[i + 2];
			dst[i + 2] = t;
		}

	uint32_t imageIndex = 0;
	XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	if(XR_FAILED(xrAcquireSwapchainImage(g.pointerSwapchain, &acquireInfo, &imageIndex)))
		return 0;
	XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	waitInfo.timeout = XR_INFINITE_DURATION;
	XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	if(XR_FAILED(xrWaitSwapchainImage(g.pointerSwapchain, &waitInfo)) ||
	   imageIndex >= g.pointerImages.size()){
		xrReleaseSwapchainImage(g.pointerSwapchain, &releaseInfo);
		return 0;
	}

	VkCommandBufferBeginInfo begin = {};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkResetCommandBuffer(g.pointerCommand, 0);
	vkBeginCommandBuffer(g.pointerCommand, &begin);
	VkImageMemoryBarrier barrier = {};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = g.pointerImages[imageIndex];
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(g.pointerCommand, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
	                     1, &barrier);
	VkBufferImageCopy region = {};
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	region.imageExtent = { (uint32_t)kPointerTexW, (uint32_t)kPointerTexH, 1 };
	vkCmdCopyBufferToImage(g.pointerCommand, g.pointerStaging, g.pointerImages[imageIndex],
	                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	// OpenXR wants a colour image released in COLOR_ATTACHMENT_OPTIMAL.
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
	                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	vkCmdPipelineBarrier(g.pointerCommand, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr,
	                     0, nullptr, 1, &barrier);
	vkEndCommandBuffer(g.pointerCommand);
	VkSubmitInfo submit = {};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &g.pointerCommand;
	vkResetFences(g.device, 1, &g.pointerFence);
	g.pointerSubmissionPending =
		vkQueueSubmit(g.queue, 1, &submit, g.pointerFence) == VK_SUCCESS;
	xrReleaseSwapchainImage(g.pointerSwapchain, &releaseInfo);
	if(!g.pointerSubmissionPending)
		return 0;

	const XrVector3f up = { 0.0f, 1.0f, 0.0f };
	const XrVector3f o = g.ptrOrigin, d = g.ptrDir;
	const float missLength = 1.5f;

	// SteamVR depth-composites quad layers that intersect, so anything that
	// touches the panel's plane gets half hidden and z-fights there (the dot
	// clipped to a half circle, worn 2026-10-05). The dot therefore lies on the
	// surface, lifted a little off it towards the viewer, and the beam stops
	// short of the plane.
	XrVector3f surfaceX = { 1.0f, 0.0f, 0.0f }, surfaceY = up;
	XrVector3f surfaceZ = { 0.0f, 0.0f, 1.0f };
	if(g.ptrHit){
		surfaceX = ptRotate(g.ptrHitOrientation, { 1.0f, 0.0f, 0.0f });
		surfaceY = ptRotate(g.ptrHitOrientation, { 0.0f, 1.0f, 0.0f });
		surfaceZ = ptRotate(g.ptrHitOrientation, { 0.0f, 0.0f, 1.0f });
		const XrVector3f toViewer = g.ptrHeadValid ?
			ptSub(g.ptrHead, g.ptrHitPos) : ptScale(d, -1.0f);
		if(ptDot(surfaceZ, toViewer) < 0.0f){
			// Pointing away from the head: lift towards the head's side and
			// turn the dot half a turn about its Y so its face looks that way.
			surfaceX = ptScale(surfaceX, -1.0f);
			surfaceZ = ptScale(surfaceZ, -1.0f);
		}
	}
	XrVector3f end = g.ptrHit ? g.ptrHitPos : ptAdd(o, ptScale(d, missLength));
	if(g.ptrHit){
		const XrVector3f full3 = ptSub(end, o);
		const float full = sqrtf(ptDot(full3, full3));
		float back = 0.02f/fmaxf(fabsf(ptDot(d, surfaceZ)), 0.25f);
		if(full - back < 0.03f)
			back = fmaxf(full - 0.03f, 0.0f);
		end = ptSub(end, ptScale(d, back));
	}
	const XrVector3f span = ptSub(end, o);
	const float length = sqrtf(ptDot(span, span));
	int count = 0;

	auto baseQuad = [&](XrCompositionLayerQuad &q, int rx, int ry, int rw, int rh){
		q = { XR_TYPE_COMPOSITION_LAYER_QUAD };
		q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
		               XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
		q.space = space;
		q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		q.subImage.swapchain = g.pointerSwapchain;
		q.subImage.imageArrayIndex = 0;
		q.subImage.imageRect.offset = { rx, ry };
		q.subImage.imageRect.extent = { rw, rh };
	};

	if(length > 0.03f){
		// A strip along the ray, turned about it so its face looks at the head.
		const XrVector3f mid = ptScale(ptAdd(o, end), 0.5f);
		const XrVector3f x = ptNorm(span, d);
		const XrVector3f toHead = g.ptrHeadValid ? ptSub(g.ptrHead, mid) : up;
		XrVector3f n = ptSub(toHead, ptScale(x, ptDot(toHead, x)));
		if(ptDot(n, n) < 1e-8f)
			n = ptCross(x, fabsf(x.y) < 0.9f ? up : XrVector3f{ 1.0f, 0.0f, 0.0f });
		n = ptNorm(n, up);
		const XrVector3f y = ptCross(n, x);
		const XrVector3f z = ptCross(x, y);
		baseQuad(quads[count], 128, kPointerBeamRow, 64, kPointerBeamRows);
		quads[count].pose.orientation = ptFromAxes(x, y, z);
		quads[count].pose.position = mid;
		quads[count].size = { length, 0.014f };
		++count;
	}
	if(g.ptrHit){
		// Flat on the surface it hit: panel and screen are yaw-anchored, so the
		// dot never rolls with the head. Size follows the distance to the head.
		float dist = length;
		if(g.ptrHeadValid){
			const XrVector3f toHead = ptSub(g.ptrHead, g.ptrHitPos);
			dist = sqrtf(ptDot(toHead, toHead));
		}
		float size = dist*0.05f;
		size = size < 0.04f ? 0.04f : size > 0.14f ? 0.14f : size;
		baseQuad(quads[count], g.pointerDown ? 64 : 0, 0, 64, 64);
		quads[count].pose.orientation =
			ptFromAxes(surfaceX, surfaceY, surfaceZ);
		quads[count].pose.position =
			ptAdd(g.ptrHitPos, ptScale(surfaceZ, 0.012f));
		quads[count].size = { size, size };
		++count;
	}
	static bool logged = false;
	if(!logged){
		logged = true;
		LOGI("[ui.pointer] layers active (beam + dot, submitted last)");
	}
	return count;
}

// vr-diag.ini dump_frame: a flat picture of what the last frame submitted, seen
// from the head, with the layers laid down in submission order (screen, panel,
// beam, dot) the way the compositor stacks them. It checks the geometry, the
// blend and that the pointer ends up over the panel; it does not stand in for
// the runtime's own compositor.
static void
diagDumpComposite(const char *directory, const uint8_t *eye0, uint32_t eyeW,
                  uint32_t eyeH, bool eyeBgra)
{
	const State::DiagLayers &L = g.diagLayers;
	if(!L.valid){
		LOGW("[vr-dump] no submitted layers to composite yet");
		return;
	}
	const int W = 1280, H = 720;
	const float tanX = 1.2f, tanY = tanX*(float)H/(float)W;
	std::vector<uint8_t> out((size_t)W*H*4);
	const uint8_t *atlas = pointerAtlas();
	const uint8_t *panel = (const uint8_t *)g.debugStagingMapped;
	const bool havePanel = L.menu && panel != nullptr && g.debugContentWidth > 0 &&
	                       g.debugContentHeight > 0;
	for(int py = 0; py < H; py++){
		for(int px = 0; px < W; px++){
			const float sx = ((px + 0.5f)/W*2.0f - 1.0f)*tanX;
			const float sy = (1.0f - (py + 0.5f)/H*2.0f)*tanY;
			const XrVector3f d = ptNorm(ptRotate(L.head.orientation, { sx, sy, -1.0f }),
			                            { 0.0f, 0.0f, -1.0f });
			float r = 22, gr = 24, b = 32;
			float t, u, v;
			if(L.theater && eye0 != nullptr &&
			   ptRayQuad(L.theaterPose, L.theaterSize, L.head.position, d, &t, &u, &v)){
				const uint8_t *s = eye0 + ((size_t)(v*(eyeH - 1))*eyeW + (size_t)(u*(eyeW - 1)))*4;
				r = s[eyeBgra ? 2 : 0]; gr = s[1]; b = s[eyeBgra ? 0 : 2];
			}
			if(havePanel &&
			   ptRayQuad(L.menuPose, L.menuSize, L.head.position, d, &t, &u, &v)){
				const int w = g.debugContentWidth, h = g.debugContentHeight;
				const uint8_t *s = panel + ((size_t)(v*(h - 1))*w + (size_t)(u*(w - 1)))*4;
				const float a = s[3]/255.0f;      // premultiplied, as submitted
				r = s[0] + r*(1.0f - a); gr = s[1] + gr*(1.0f - a); b = s[2] + b*(1.0f - a);
			}
			for(int i = 0; i < L.pointerCount; i++){
				if(!ptRayQuad(L.pointerPose[i], L.pointerSize[i], L.head.position, d, &t, &u, &v))
					continue;
				const int tx = L.pointerRect[i][0] + (int)(u*(L.pointerRect[i][2] - 1));
				const int ty = L.pointerRect[i][1] + (int)(v*(L.pointerRect[i][3] - 1));
				const uint8_t *s = atlas + ((size_t)ty*kPointerTexW + tx)*4;
				const float a = s[3]/255.0f;      // straight alpha
				r = s[0]*a + r*(1.0f - a); gr = s[1]*a + gr*(1.0f - a); b = s[2]*a + b*(1.0f - a);
			}
			uint8_t *o = &out[((size_t)py*W + px)*4];
			o[0] = (uint8_t)(r > 255 ? 255 : r); o[1] = (uint8_t)(gr > 255 ? 255 : gr);
			o[2] = (uint8_t)(b > 255 ? 255 : b); o[3] = 255;
		}
	}
	char path[600];
	snprintf(path, sizeof(path), "%s/vr-pointer.bmp", directory);
	if(frame_diag::writeBmp24(path, out.data(), W, H, false))
		LOGI("[vr-dump] wrote %s %dx%d (flat composite: screen=%d panel=%d pointer layers=%d)",
		     path, W, H, L.theater ? 1 : 0, havePanel ? 1 : 0, L.pointerCount);
}
#endif

#if MIAMIVR_STEAM_FRAME
// ---------------------------------------------------------------------------
// Steam Frame unattended-test diagnostics (standard.md section 8): synthetic
// head, swapchain dump, [vr-perf]. The vr-diag.ini side is in frame_diag.
// ---------------------------------------------------------------------------

long long
diagNowNs(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long long)now.tv_sec*1000000000LL + now.tv_nsec;
}

const char *
diagSessionStateName(XrSessionState state)
{
	switch(state){
	case XR_SESSION_STATE_IDLE: return "IDLE";
	case XR_SESSION_STATE_READY: return "READY";
	case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
	case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
	case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
	case XR_SESSION_STATE_STOPPING: return "STOPPING";
	case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
	case XR_SESSION_STATE_EXITING: return "EXITING";
	default: return "UNKNOWN";
	}
}

// [vr-perf] is always on in the Frame build rather than gated by vr-diag.ini
// `timing=1`: it is one log line per 10 s and one clock read per frame, and the
// standard wants it available on every run without touching the config. The
// timing key is still recognised (and logged) so a shared ini keeps working.
//
// Stages are chosen not to overlap, so the line's cpu= is the whole renderFrame
// CPU time after xrWaitFrame returns: "xr" is that total minus the game
// renderer callback, "game" is the callback (sim and draw submission, since the
// game steps inside it). xrWaitFrame is left out on purpose: it is the pacing
// block, not work. eye= is not measured (zeros) and gpu= is passed as unknown.
struct PerfState
{
	sfvr_perf perf;
	bool initialised = false;
	// Callback time of the frame in progress, filled in by renderLayer.
	float gameMs = 0.0f;
	long long lastDisplayTime = 0;
	// Smallest predicted-display-time step seen, the period when the runtime
	// reports none.
	long long smallestStep = 0;
	// The refresh-rate fallback only trusts a 10 s window the player wore from
	// start to end at a rate that did not change meanwhile.
	bool windowTainted = false;
};
PerfState gPerf;

float effectiveRefreshHz(void);

void
perfTaintWindow(void)
{
	gPerf.windowTainted = true;
}

// Standard section 9: after two consecutive 10 s windows below 90% of the
// display rate while worn (session FOCUSED), say once per session that the rate
// is not being held and which per-app rate to set in SteamVR. Nothing is
// requested: the app cannot change the rate. A window only counts if the
// headset was worn and rendering all through it and the rate did not change.
void
perfRefreshWindowEnded(unsigned long frames, double elapsedS)
{
	const bool tainted = gPerf.windowTainted;
	gPerf.windowTainted = false;
	if(g.refreshWarned)
		return;
	const float rate = effectiveRefreshHz();
	if(tainted || rate < 60.0f || elapsedS <= 0.0){
		g.refreshLowWindows = 0;
		return;
	}
	const double fps = (double)frames/elapsedS;
	if(fps >= 0.9*(double)rate){
		g.refreshLowWindows = 0;
		return;
	}
	if(++g.refreshLowWindows < 2){
		g.refreshLowFps = fps;
		return;
	}
	// The highest standard rate the measured frame rate would hold.
	static const int standard[] = { 120, 90, 72 };
	double lowest = fps < g.refreshLowFps ? fps : g.refreshLowFps;
	int recommended = 72;
	for(int hz : standard)
		if(lowest >= 0.9*hz){
			recommended = hz;
			break;
		}
	g.refreshWarned = true;
	LOGI("[vr] refresh: not holding %.0f Hz (fps %.1f, %.1f); set ViceCityVR to "
	     "%d Hz in SteamVR per-app settings", rate, g.refreshLowFps, fps,
	     recommended);
}

double
diagNowSeconds(void)
{
	return (double)diagNowNs()*1e-9;
}

void
perfPoll(void)
{
	if(!gPerf.initialised){
		static const char *const stages[] = { "xr", "game" };
		sfvr_perf_init(&gPerf.perf, SFVR_PERF_DEFAULT_WINDOW_S, stages, 2);
		gPerf.initialised = true;
	}
	char line[256];
	const double now = diagNowSeconds();
	// Only whole windows under a worn, rendering session count towards the
	// refresh advice.
	if(g.sessionState != XR_SESSION_STATE_FOCUSED || !g.diagShouldRender)
		gPerf.windowTainted = true;
	const unsigned long frames = gPerf.perf.frames;
	const double startS = gPerf.perf.start_s;
	if(sfvr_perf_poll(&gPerf.perf, now, line, sizeof(line))){
		LOGI("%s", line);
		perfRefreshWindowEnded(frames, now - startS);
	}
}

void
perfAddFrame(long long frameStartNs, XrTime displayTime, XrDuration period)
{
	const long long step = gPerf.lastDisplayTime != 0 ?
		(long long)displayTime - gPerf.lastDisplayTime : 0;
	if(step > 0 && (gPerf.smallestStep == 0 || step < gPerf.smallestStep))
		gPerf.smallestStep = step;
	const long long reference = period > 0 ? (long long)period :
		gPerf.smallestStep;
	// A frame whose display time jumped by more than 1.5 periods means at
	// least one display frame went by without one of ours.
	const bool missed = step > 0 && reference > 0 &&
		(double)step > 1.5*(double)reference;
	gPerf.lastDisplayTime = (long long)displayTime;

	const float totalMs = (float)((diagNowNs() - frameStartNs)*1e-6);
	const float stageMs[2] = { totalMs > gPerf.gameMs ?
		totalMs - gPerf.gameMs : 0.0f, gPerf.gameMs };
	sfvr_perf_add_frame(&gPerf.perf, diagNowSeconds(), missed ? 1 : 0, stageMs,
	                    nullptr, -1.0f);
}

bool
diagFovPlausible(const XrFovf &fov)
{
	const float limit = 1.57f;
	return fov.angleRight > fov.angleLeft && fov.angleUp > fov.angleDown &&
	       fov.angleLeft > -limit && fov.angleRight < limit &&
	       fov.angleDown > -limit && fov.angleUp < limit;
}

void
diagCacheFov(const XrView views[2])
{
	if(!diagFovPlausible(views[0].fov) || !diagFovPlausible(views[1].fov))
		return;
	g.diagLastValidFov[0] = views[0].fov;
	g.diagLastValidFov[1] = views[1].fov;
	g.diagLastValidFovSet = true;
}

// Both eyes 1.6 m up in the reference space, a head-width apart, looking along
// the yaw direction. Yaw is about +Y and positive turns left, the OpenXR
// convention (a right-handed rotation). The eye offset rotates with the head so
// the baseline stays across the view direction; at yaw 0 the eyes sit at
// x = -0.032 and +0.032.
void
diagMakeSyntheticViews(XrView views[2], float yawDegrees)
{
	const float yaw = yawDegrees*(3.14159265f/180.0f);
	const float half = yaw*0.5f;
	const float rightX = cosf(yaw);
	const float rightZ = -sinf(yaw);
	for(int eye = 0; eye < 2; eye++){
		const float side = eye == 0 ? -0.032f : 0.032f;
		views[eye].pose = {};
		views[eye].pose.orientation.y = sinf(half);
		views[eye].pose.orientation.w = cosf(half);
		views[eye].pose.position.x = side*rightX;
		views[eye].pose.position.y = 1.6f;
		views[eye].pose.position.z = side*rightZ;
		if(g.diagLastValidFovSet){
			views[eye].fov = g.diagLastValidFov[eye];
		}else{
			const float quarterPi = 0.78539816f;
			views[eye].fov.angleLeft = -quarterPi;
			views[eye].fov.angleRight = quarterPi;
			views[eye].fov.angleUp = quarterPi;
			views[eye].fov.angleDown = -quarterPi;
		}
	}
}

// Reads the two array layers of the swapchain image back and writes
// vr-eye0.bmp, vr-eye1.bmp, vr-hud.bmp (when the overlay is up) and
// vr-state.txt under <root>/vr-dumps.
//
// Layout: librw's resolve pass (postFxRenderPass, destinations[0] in
// vkdevice.cpp) writes the swapchain image with initialLayout UNDEFINED and
// finalLayout COLOR_ATTACHMENT_OPTIMAL, and nothing after it changes that;
// OpenXR also requires COLOR_ATTACHMENT_OPTIMAL at xrReleaseSwapchainImage. So
// this barriers COLOR_ATTACHMENT_OPTIMAL -> TRANSFER_SRC_OPTIMAL, copies, and
// barriers back. librw submitted its work on g.queue, the same queue used
// here, so queue order plus a full-pipeline barrier covers its writes;
// vkQueueWaitIdle then makes the result CPU-readable. It stalls once.
void
diagDumpSwapchain(uint32_t imageIndex, int frameNumber)
{
	if(!g.swapchainTransferSrc){
		LOGW("[vr-dump] swapchain has no TRANSFER_SRC usage; nothing dumped");
		return;
	}
	bool bgra = false;
	switch((VkFormat)g.swapchainFormat){
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_R8G8B8A8_SRGB:
		break;
	case VK_FORMAT_B8G8R8A8_UNORM:
	case VK_FORMAT_B8G8R8A8_SRGB:
		bgra = true;
		break;
	default:
		LOGE("[vr-dump] swapchain format %lld is not 8-bit RGBA/BGRA; "
		     "nothing dumped", (long long)g.swapchainFormat);
		return;
	}
	char directory[512];
	if(!frame_diag::dumpDirectory(directory, sizeof(directory)))
		return;

	const uint32_t width = g.renderWidth;
	const uint32_t height = g.renderHeight;
	const VkDeviceSize layerBytes = (VkDeviceSize)width*height*4;
	const VkImage image = g.images[imageIndex].image;

	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkCommandBuffer commands = VK_NULL_HANDLE;
	const uint8_t *mapped = nullptr;
	do{
		if(!createBuffer(layerBytes*2, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		                 &buffer, &memory))
			break;
		VkCommandBufferAllocateInfo allocInfo = {};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.commandPool = g.commandPool;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = 1;
		if(vkAllocateCommandBuffers(g.device, &allocInfo, &commands) !=
		   VK_SUCCESS){
			commands = VK_NULL_HANDLE;
			break;
		}
		VkCommandBufferBeginInfo begin = {};
		begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(commands, &begin);

		VkImageMemoryBarrier barrier = {};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 2;
		barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
		                     0, nullptr, 1, &barrier);

		VkBufferImageCopy regions[2] = {};
		for(uint32_t layer = 0; layer < 2; layer++){
			regions[layer].bufferOffset = layerBytes*layer;
			regions[layer].imageSubresource.aspectMask =
				VK_IMAGE_ASPECT_COLOR_BIT;
			regions[layer].imageSubresource.baseArrayLayer = layer;
			regions[layer].imageSubresource.layerCount = 1;
			regions[layer].imageExtent = { width, height, 1 };
		}
		vkCmdCopyImageToBuffer(commands, image,
		                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer,
		                       2, regions);

		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
		                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		                     0, 0, nullptr, 0, nullptr, 1, &barrier);
		vkEndCommandBuffer(commands);

		VkSubmitInfo submit = {};
		submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &commands;
		if(vkQueueSubmit(g.queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS)
			break;
		vkQueueWaitIdle(g.queue);

		void *pointer = nullptr;
		if(vkMapMemory(g.device, memory, 0, layerBytes*2, 0, &pointer) !=
		   VK_SUCCESS)
			break;
		mapped = (const uint8_t *)pointer;
	}while(0);

	if(mapped == nullptr)
		LOGE("[vr-dump] swapchain readback failed; nothing dumped");
	else{
		for(int eye = 0; eye < 2; eye++){
			char path[600];
			snprintf(path, sizeof(path), "%s/vr-eye%d.bmp", directory, eye);
			if(frame_diag::writeBmp24(path, mapped + layerBytes*eye,
			                          (int)width, (int)height, bgra))
				LOGI("[vr-dump] wrote %s %ux%u format=%lld", path, width,
				     height, (long long)g.swapchainFormat);
		}
		diagDumpComposite(directory, mapped, width, height, bgra);
		vkUnmapMemory(g.device, memory);
	}
	if(commands != VK_NULL_HANDLE)
		vkFreeCommandBuffers(g.device, g.commandPool, 1, &commands);
	if(buffer != VK_NULL_HANDLE)
		vkDestroyBuffer(g.device, buffer, nullptr);
	if(memory != VK_NULL_HANDLE)
		vkFreeMemory(g.device, memory, nullptr);

	// The overlay quad is a CPU upload (RGBA, tightly packed) that is kept in
	// the staging buffer, so it is written from there with no GPU readback.
	// Always RGBA regardless of the swapchain format: that is the byte order
	// the game hands over.
	bool hudDumped = false;
	if(g.debugVisible && g.debugStagingMapped != nullptr &&
	   g.debugContentWidth > 0 && g.debugContentHeight > 0){
		char path[600];
		snprintf(path, sizeof(path), "%s/vr-hud.bmp", directory);
		if(frame_diag::writeBmp24(path, (const uint8_t *)g.debugStagingMapped,
		                          g.debugContentWidth, g.debugContentHeight,
		                          false)){
			hudDumped = true;
			LOGI("[vr-dump] wrote %s %dx%d format=%d", path,
			     g.debugContentWidth, g.debugContentHeight,
			     (int)VK_FORMAT_R8G8B8A8_UNORM);
		}
	}

	char state[2048];
	snprintf(state, sizeof(state),
		"frame=%d\n"
		"predicted_display_time_ns=%lld\n"
		"session_state=%s (%d)\n"
		"should_render=%d\n"
		"force_render=%d\n"
		"synthetic_head=%s\n"
		"diag_yaw=%g\n"
		"swapchain_format=%lld (%s)\n"
		"swapchain_size=%ux%u x2 layers\n"
		"image_index=%u\n"
		"render_scale_requested=%d%% selected=%d%% effective=%.2f%%\n"
		"theater_mode=%s\n"
		"theater_note=%s\n"
		"bmp_note=bytes copied as stored (no sRGB decode); rows bottom-up\n"
		"hud=%s\n"
		"pointer_note=vr-pointer.bmp is a flat composite of the layers the previous "
		"frame submitted (screen, panel, beam, dot, in that order) seen from the head\n",
		frameNumber, (long long)g.predictedDisplayTime,
		diagSessionStateName(g.sessionState), (int)g.sessionState,
		g.diagShouldRender ? 1 : 0,
		frame_diag::config().forceRender ? 1 : 0,
		g.diagSyntheticHead ? "yes" : "no",
		(double)frame_diag::config().yawDeg,
		(long long)g.swapchainFormat, bgra ? "BGRA8" : "RGBA8",
		width, height, imageIndex,
		g.renderScaleRequestedPercent, g.renderScaleSelectedPresetPercent,
		(double)g.renderScaleEffectivePercent,
		g.theaterMode ? "on" : "off",
		g.theaterMode ?
			"menu/cutscene frame: only array layer 0 is submitted (as the "
			"theater quad); both layers were still dumped" :
			"immersive frame: layer 0 = left eye, layer 1 = right eye",
		hudDumped ? "vr-hud.bmp is the last RGBA overlay upload (CPU staging, "
		            "alpha dropped)" : "overlay not visible; no vr-hud.bmp");
	char path[600];
	snprintf(path, sizeof(path), "%s/vr-state.txt", directory);
	if(frame_diag::writeTextFile(path, state))
		LOGI("[vr-dump] wrote %s", path);
}

// Counts frames the game renderer ran for and dumps once on the N-th.
void
diagAfterGameRender(uint32_t imageIndex)
{
	static int renderedFrames = 0;
	static bool dumped = false;
	renderedFrames++;
	const int target = frame_diag::config().dumpFrame;
	if(target > 0 && !dumped && renderedFrames >= target){
		dumped = true;
		diagDumpSwapchain(imageIndex, renderedFrames);
	}
}
#endif

// Returns false when nothing was rendered. The caller must not submit a
// projection layer in that case: claiming a layer whose swapchain image was
// never acquired makes the compositor drop the frame, and Horizon OS replaces
// the app with its "still running" panel.
bool
renderLayer(const XrView views[2])
{
	uint32_t imageIndex = 0;
	XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	const XrResult acquired = xrAcquireSwapchainImage(g.swapchain, &acquireInfo,
	                                                  &imageIndex);
	if(XR_FAILED(acquired)){
		LOGE("xrAcquireSwapchainImage failed: %d", (int)acquired);
		return false;
	}

	XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	waitInfo.timeout = XR_INFINITE_DURATION;
	if(XR_FAILED(xrWaitSwapchainImage(g.swapchain, &waitInfo))){
		XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(g.swapchain, &releaseInfo);
		LOGE("xrWaitSwapchainImage failed");
		return false;
	}

	SwapchainImage &image = g.images[imageIndex];

	Mat4 viewProj[2];
	XrFovf renderFov[2] = { views[0].fov, views[1].fov };
	const float scopeZoom = androidgame::VrScopeZoomFactor();
	for(int eye = 0; eye < 2; eye++){
		if(scopeZoom > 1.0f){
			renderFov[eye].angleLeft =
				atanf(tanf(renderFov[eye].angleLeft)/scopeZoom);
			renderFov[eye].angleRight =
				atanf(tanf(renderFov[eye].angleRight)/scopeZoom);
			renderFov[eye].angleUp =
				atanf(tanf(renderFov[eye].angleUp)/scopeZoom);
			renderFov[eye].angleDown =
				atanf(tanf(renderFov[eye].angleDown)/scopeZoom);
		}
		const Mat4 projection = projectionFromFov(
			renderFov[eye], 0.05f, 1000.0f);
		const Mat4 viewMatrix = viewFromPose(views[eye].pose);
		viewProj[eye] = multiply(projection, viewMatrix);
	}

	// With a renderer installed the game owns the frame: it records and submits
	// through librw's Vulkan backend, so none of the bring-up scene's command
	// buffer, fence or descriptor state is touched.
	if(g.frameRenderer != nullptr){
		float matrices[2][16];
		memcpy(matrices[0], viewProj[0].m, sizeof(matrices[0]));
		memcpy(matrices[1], viewProj[1].m, sizeof(matrices[1]));

		// Head pose: the midpoint between the eyes, with the left eye's
		// orientation. Anchoring 2D content to one eye instead would make the
		// panel swing as the head rolls.
		XrPosef head = views[0].pose;
		head.position.x = (views[0].pose.position.x + views[1].pose.position.x) * 0.5f;
		head.position.y = (views[0].pose.position.y + views[1].pose.position.y) * 0.5f;
		head.position.z = (views[0].pose.position.z + views[1].pose.position.z) * 0.5f;

		// The plane has to subtend exactly what the game projected with, or a
		// screen coordinate maps to the wrong direction and world sprites
		// drift as the player moves. viewWindow holds the tangents of the
		// half angles, so the full width at distance d is 2*d*viewWindow.x.
		float viewWindowX = 0.6f, viewWindowY = 0.6f;
		androidgame::GetIm2DViewWindow(&viewWindowX, &viewWindowY);
		const float im2dDistance = 2.0f;
		const Mat4 im2dSpriteWorld = im2dPlane(head, (float)g.renderWidth,
		                                       (float)g.renderHeight,
		                                       im2dDistance,
		                                       2.0f*im2dDistance*viewWindowX,
		                                       2.0f*im2dDistance*viewWindowY);
#if MIAMIVR_STEAM_FRAME
		// The HUD, crosshair and sights follow yaw and pitch but never roll.
		XrPosef hudHead = head;
		hudHead.orientation = rollFreeOrientation(head.orientation,
		                                          &g.hudYawHold);
		const Mat4 im2dWorld = im2dPlane(hudHead, (float)g.renderWidth,
		                                 (float)g.renderHeight, im2dDistance,
		                                 2.0f*im2dDistance*viewWindowX,
		                                 2.0f*im2dDistance*viewWindowY);
		static bool reportedRollFree = false;
		if(!reportedRollFree){
			reportedRollFree = true;
			LOGI("[vr] roll-free billboards/HUD");
		}
#else
		const Mat4 im2dWorld = im2dSpriteWorld;
#endif
		const float headPos[3] = {
			head.position.x, head.position.y, head.position.z
		};
		// Head yaw: where the head's forward (-Z rotated by the orientation)
		// lands in the horizontal plane. atan2(-fx, -fz) is 0 facing -Z and
		// grows turning left, matching a right-handed rotation about +Y.
		const XrQuaternionf &hq = head.orientation;
		const float fx = -(2.0f*(hq.x*hq.z + hq.y*hq.w));
		const float fz = -(1.0f - 2.0f*(hq.x*hq.x + hq.y*hq.y));
		const float headYaw = atan2f(-fx, -fz);
		const float headQuat[4] = { hq.x, hq.y, hq.z, hq.w };
		// The eye fov drives the game's sprite mathematics, mirroring the
		// desktop BeginEye handing the game an eye fov: coronas and other
		// screen-space effects size themselves from it.
		const float eyeFovDeg =
			(renderFov[0].angleRight-renderFov[0].angleLeft)*57.29578f;
		float eyePos[2][3];
		float eyeQuat[2][4];
		for(int eye = 0; eye < 2; eye++){
			eyePos[eye][0] = views[eye].pose.position.x;
			eyePos[eye][1] = views[eye].pose.position.y;
			eyePos[eye][2] = views[eye].pose.position.z;
			eyeQuat[eye][0] = views[eye].pose.orientation.x;
			eyeQuat[eye][1] = views[eye].pose.orientation.y;
			eyeQuat[eye][2] = views[eye].pose.orientation.z;
			eyeQuat[eye][3] = views[eye].pose.orientation.w;
		}
#if MIAMIVR_STEAM_FRAME
		const long long gameStartNs = diagNowNs();
#endif
		g.frameRenderer(image.image, image.view, matrices, im2dWorld.m,
		                im2dSpriteWorld.m, im2dDistance, headPos, headYaw, headQuat, eyeFovDeg,
		                eyePos, eyeQuat);
#if MIAMIVR_STEAM_FRAME
		gPerf.gameMs = (float)((diagNowNs() - gameStartNs)*1e-6);
		// The image must be read back before it is released to the compositor.
		diagAfterGameRender(imageIndex);
#endif

		XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(g.swapchain, &releaseInfo);
		return true;
	}

#ifndef MIAMIVR_BRINGUP
	// Release builds deliberately do not allocate the standalone bring-up
	// depth/pipeline resources. android_main installs the librw callback before
	// the first frame; fail closed if that contract is ever broken instead of
	// dereferencing null Vulkan handles.
	XrSwapchainImageReleaseInfo releaseInfo = {
		XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	xrReleaseSwapchainImage(g.swapchain, &releaseInfo);
	LOGE("release frame reached without an installed game renderer");
	return false;
#else
	vkWaitForFences(g.device, 1, &image.fence, VK_TRUE, UINT64_MAX);
	vkResetFences(g.device, 1, &image.fence);
	memcpy(image.uniformMapped, viewProj, sizeof(viewProj));

	VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkResetCommandBuffer(image.commandBuffer, 0);
	vkBeginCommandBuffer(image.commandBuffer, &beginInfo);

	VkClearValue clears[2] = {};
	clears[0].color = { { 0.05f, 0.06f, 0.10f, 1.0f } };
	clears[1].depthStencil = { 1.0f, 0 };

	VkRenderPassBeginInfo passInfo = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
	passInfo.renderPass = g.renderPass;
	passInfo.framebuffer = image.framebuffer;
	passInfo.renderArea.extent = { g.renderWidth, g.renderHeight };
	passInfo.clearValueCount = 2;
	passInfo.pClearValues = clears;
	vkCmdBeginRenderPass(image.commandBuffer, &passInfo, VK_SUBPASS_CONTENTS_INLINE);

	VkViewport viewport = { 0.0f, 0.0f, (float)g.renderWidth, (float)g.renderHeight,
	                        0.0f, 1.0f };
	VkRect2D scissor = { { 0, 0 }, { g.renderWidth, g.renderHeight } };
	vkCmdSetViewport(image.commandBuffer, 0, 1, &viewport);
	vkCmdSetScissor(image.commandBuffer, 0, 1, &scissor);

	vkCmdBindPipeline(image.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipeline);
	vkCmdBindDescriptorSets(image.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                        g.pipelineLayout, 0, 1, &image.descriptorSet, 0, nullptr);
	const VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(image.commandBuffer, 0, 1, &g.vertexBuffer, &offset);
	vkCmdBindIndexBuffer(image.commandBuffer, g.indexBuffer, 0, VK_INDEX_TYPE_UINT16);

	drawScene(image.commandBuffer);

	vkCmdEndRenderPass(image.commandBuffer);
	vkEndCommandBuffer(image.commandBuffer);

	VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &image.commandBuffer;
	vkQueueSubmit(g.queue, 1, &submit, image.fence);

	XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	xrReleaseSwapchainImage(g.swapchain, &releaseInfo);
	return true;
#endif
}

// The rate the display has to reach: what the last request asked for, else the
// target.
float
refreshGoalHz(void)
{
	return g.requestedRefreshRateHz > 0.0f ? g.requestedRefreshRateHz :
	       g.targetRefreshRateHz;
}

// The rate the display is really running. On the Frame the compositor's frame
// period is the truth: the FB extension can list and report a different rate
// than the per-app one SteamVR applies (lesson 79).
float
effectiveRefreshHz(void)
{
#if MIAMIVR_STEAM_FRAME
	if(g.measuredRefreshHz > 0.0f)
		return g.measuredRefreshHz;
#endif
	return g.currentRefreshRateHz;
}

bool
refreshAtGoal(float reported)
{
	return fabsf(reported - refreshGoalHz()) < 0.5f;
}

#if MIAMIVR_STEAM_FRAME
// The Frame's one refresh request per session (standard section 9). SteamVR
// offers only the title's per-app rate, so this asks only for an offered rate,
// the highest not above the RefreshRate setting, and never for less than the
// compositor already runs. No retries, no holding frames, no fallback.
bool
requestFrameRefreshRate(void)
{
	if(g.refreshRequestedThisSession)
		return false;
	g.refreshRequestedThisSession = true;
	if(!g.hasRefreshRateExt || g.enumerateRefreshRates == nullptr ||
	   g.getRefreshRate == nullptr || g.requestRefreshRate == nullptr){
		LOGI("[vr] refresh: setting %.0f, no XR_FB_display_refresh_rate, "
		     "requested none (compositor at %.0f Hz)",
		     g.targetRefreshRateHz, effectiveRefreshHz());
		return false;
	}
	uint32_t count = 0;
	std::vector<float> rates;
	if(XR_SUCCEEDED(g.enumerateRefreshRates(g.session, 0, &count, nullptr)) &&
	   count > 0){
		rates.resize(count);
		if(XR_FAILED(g.enumerateRefreshRates(g.session, count, &count,
		                                     rates.data())))
			rates.clear();
	}
	char offered[160];
	size_t used = 0;
	offered[0] = '\0';
	float best = 0.0f;
	for(size_t i = 0; i < rates.size(); i++){
		if(rates[i] <= g.targetRefreshRateHz + 0.5f && rates[i] > best)
			best = rates[i];
		if(used < sizeof(offered) - 1)
			used += (size_t)snprintf(offered + used, sizeof(offered) - used,
			                         "%s%.0f", i ? ", " : "", rates[i]);
	}
	if(used >= sizeof(offered))
		offered[sizeof(offered) - 1] = '\0';
	const float running = effectiveRefreshHz();
	if(best <= 0.0f){
		LOGI("[vr] refresh: setting %.0f, offered {%s}, requested none "
		     "(nothing offered at or below the setting; compositor at %.0f Hz)",
		     g.targetRefreshRateHz, offered, running);
		return false;
	}
	if(g.measuredRefreshHz > 0.0f && g.measuredRefreshHz >= best - 2.0f){
		LOGI("[vr] refresh: setting %.0f, offered {%s}, requested none "
		     "(compositor already at %.0f Hz)",
		     g.targetRefreshRateHz, offered, g.measuredRefreshHz);
		return false;
	}
	LOGI("[vr] refresh: setting %.0f, offered {%s}, requested %.0f",
	     g.targetRefreshRateHz, offered, best);
	perfTaintWindow();
	const XrResult result = g.requestRefreshRate(g.session, best);
	g.requestedRefreshRateHz = best;
	float after = 0.0f;
	if(XR_SUCCEEDED(g.getRefreshRate(g.session, &after)) && after > 0.0f)
		g.currentRefreshRateHz = after;
	LOGI("[vr] refresh: request result %d, runtime reports %.1f Hz", (int)result,
	     g.currentRefreshRateHz);
	return XR_SUCCEEDED(result);
}
#endif

bool
requestPreferredDisplayRefreshRate(void)
{
#if MIAMIVR_STEAM_FRAME
	return requestFrameRefreshRate();
#endif
	if(!g.hasRefreshRateExt || g.enumerateRefreshRates == nullptr ||
	   g.getRefreshRate == nullptr || g.requestRefreshRate == nullptr)
		return false;

	uint32_t count = 0;
	XrResult result =
		g.enumerateRefreshRates(g.session, 0, &count, nullptr);
	if(XR_FAILED(result) || count == 0){
		// Some Horizon OS builds return an empty rate list. Request the
		// target directly and let the runtime validate it; unsupported
		// values fail cleanly with DISPLAY_REFRESH_RATE_UNSUPPORTED.
		g.requestedRefreshRateHz = g.targetRefreshRateHz;
		result = g.requestRefreshRate(g.session, g.targetRefreshRateHz);
		float actual = 0.0f;
		if(XR_SUCCEEDED(g.getRefreshRate(g.session, &actual)) &&
		   actual > 0.0f)
			g.currentRefreshRateHz = actual;
		LOGI("display refresh direct request %.1f Hz: result %d, "
		     "now %.1f Hz", g.targetRefreshRateHz, (int)result,
		     g.currentRefreshRateHz);
		return XR_SUCCEEDED(result);
	}

	std::vector<float> rates(count);
	result = g.enumerateRefreshRates(g.session, count, &count, rates.data());
	if(XR_FAILED(result)){
		LOGE("display refresh list failed: %d", (int)result);
		return false;
	}

	float target = rates[0];
	float targetDistance = fabsf(target - g.targetRefreshRateHz);
	for(uint32_t i = 0; i < count; i++){
		LOGI("display refresh offered[%u] = %.1f Hz", i, rates[i]);
		const float distance =
			fabsf(rates[i] - g.targetRefreshRateHz);
		if(distance < targetDistance){
			target = rates[i];
			targetDistance = distance;
		}
	}
	g.requestedRefreshRateHz = target;

	float before = 0.0f;
	const XrResult beforeResult = g.getRefreshRate(g.session, &before);
	result = g.requestRefreshRate(g.session, target);
	float after = 0.0f;
	const XrResult afterResult = g.getRefreshRate(g.session, &after);
	if(XR_SUCCEEDED(afterResult) && after > 0.0f)
		g.currentRefreshRateHz = after;
	else if(XR_SUCCEEDED(beforeResult) && before > 0.0f)
		g.currentRefreshRateHz = before;
	LOGI("display refresh request: current %.1f (%d), target %.1f, "
	     "result %d, immediate %.1f (%d)",
	     before, (int)beforeResult, target, (int)result,
	     after, (int)afterResult);
	return XR_SUCCEEDED(result);
}

const char *
performanceModeName(int mode)
{
	switch(mode){
	case PERFORMANCE_MODE_AUTO: return "Auto";
	case PERFORMANCE_MODE_SUSTAINED_HIGH: return "SustainedHigh";
	case PERFORMANCE_MODE_BOOST: return "Boost";
	default: return "Unknown";
	}
}

const char *
performanceDomainName(XrPerfSettingsDomainEXT domain)
{
	switch(domain){
	case XR_PERF_SETTINGS_DOMAIN_CPU_EXT: return "CPU";
	case XR_PERF_SETTINGS_DOMAIN_GPU_EXT: return "GPU";
	default: return "unknown";
	}
}

const char *
performanceSubDomainName(XrPerfSettingsSubDomainEXT subDomain)
{
	switch(subDomain){
	case XR_PERF_SETTINGS_SUB_DOMAIN_COMPOSITING_EXT: return "compositing";
	case XR_PERF_SETTINGS_SUB_DOMAIN_RENDERING_EXT: return "rendering";
	case XR_PERF_SETTINGS_SUB_DOMAIN_THERMAL_EXT: return "thermal";
	default: return "unknown";
	}
}

static constexpr long long kPerformanceBoostDurationNs = 25LL * 1000000000LL;
static constexpr long long kPerformanceStepDownRetryNs = 1LL * 1000000000LL;

long long
monotonicTimeNs(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long long)now.tv_sec * 1000000000LL + now.tv_nsec;
}

bool
setCpuPerformanceLevel(XrPerfSettingsLevelEXT level, const char *reason)
{
	if(!g.hasPerformanceSettingsExt || g.setPerformanceLevel == nullptr ||
	   g.session == XR_NULL_HANDLE || !g.running)
		return false;
	const XrResult result = g.setPerformanceLevel(
		g.session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, level);
	LOGI("OpenXR CPU performance %s: level=%d result=%d",
	     reason, (int)level, (int)result);
	if(XR_SUCCEEDED(result))
		g.cpuPerformanceHintSucceededThisSession = true;
	return XR_SUCCEEDED(result);
}

bool
setGpuPerformanceLevel(XrPerfSettingsLevelEXT level, const char *reason)
{
	if(!g.hasPerformanceSettingsExt || g.setPerformanceLevel == nullptr ||
	   g.session == XR_NULL_HANDLE || !g.running)
		return false;
	const XrResult result = g.setPerformanceLevel(
		g.session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, level);
	LOGI("OpenXR GPU performance %s: level=%d result=%d",
	     reason, (int)level, (int)result);
	if(XR_SUCCEEDED(result))
		g.gpuPerformanceHintSucceededThisSession = true;
	return XR_SUCCEEDED(result);
}

bool
applyGpuPerformanceMode(bool force)
{
	if(!g.hasPerformanceSettingsExt || g.setPerformanceLevel == nullptr ||
	   g.session == XR_NULL_HANDLE || !g.running)
		return false;
	const int requested = g.requestedGpuPerformanceMode;
	if(!force && g.activeGpuPerformanceMode == requested)
		return true;
	if(requested == PERFORMANCE_MODE_AUTO &&
	   !g.gpuPerformanceHintSucceededThisSession){
		g.activeGpuPerformanceMode = PERFORMANCE_MODE_AUTO;
		LOGI("OpenXR GPU performance Auto: no GPU hint issued");
		return true;
	}
	// OpenXR has no clear-hint operation. Returning to Auto after an explicit
	// request uses Sustained High until the next process/session restart.
	const XrPerfSettingsLevelEXT level =
		requested == PERFORMANCE_MODE_BOOST ?
			XR_PERF_SETTINGS_LEVEL_BOOST_EXT :
			XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT;
	const bool ok = setGpuPerformanceLevel(level,
		performanceModeName(requested));
	g.activeGpuPerformanceMode = ok ?
		(requested == PERFORMANCE_MODE_AUTO ?
		 PERFORMANCE_MODE_SUSTAINED_HIGH : requested) : -1;
	return ok;
}

void
finishPerformanceBoost(const char *reason, bool thermal)
{
	const bool wasBoostActive = g.cpuBoostActive;
	const int returnMode =
		g.boostReturnMode == PERFORMANCE_MODE_SUSTAINED_HIGH ?
			PERFORMANCE_MODE_SUSTAINED_HIGH : PERFORMANCE_MODE_AUTO;
	const bool steppedDown = setCpuPerformanceLevel(
		XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT, reason);
	g.requestedPerformanceMode = returnMode;
	if(thermal)
		g.boostThermallyBlocked = true;
	if(steppedDown){
		g.activePerformanceMode = PERFORMANCE_MODE_SUSTAINED_HIGH;
		g.cpuBoostActive = false;
		g.boostExpiryDisplayTime = 0;
		g.boostExpiryMonotonicNs = 0;
		LOGW("OpenXR CPU Boost ended (%s); requested mode restored to %s%s",
		     reason, performanceModeName(returnMode),
		     thermal ? "; Boost blocked until a new session" : "");
	}else if(wasBoostActive){
		// A transient runtime failure must not leave the last accepted Boost
		// hint active indefinitely. Keep the expiry pump armed and retry the
		// safe level at a low rate instead of spamming the OpenXR call.
		g.activePerformanceMode = PERFORMANCE_MODE_BOOST;
		g.cpuBoostActive = true;
		g.boostExpiryDisplayTime = g.predictedDisplayTime > 0 ?
			g.predictedDisplayTime + kPerformanceStepDownRetryNs : 0;
		g.boostExpiryMonotonicNs =
			monotonicTimeNs() + kPerformanceStepDownRetryNs;
		LOGW("OpenXR CPU Boost step-down failed (%s); retrying in one second",
		     reason);
	}else{
		g.activePerformanceMode = -1;
		g.cpuBoostActive = false;
		g.boostExpiryDisplayTime = 0;
		g.boostExpiryMonotonicNs = 0;
	}
}

// CPU-only on purpose. Traffic/render submission is CPU-bound; touching the
// GPU policy would add heat without addressing the measured bottleneck.
bool
applyPerformanceMode(bool force)
{
	if(!g.hasPerformanceSettingsExt || g.setPerformanceLevel == nullptr ||
	   g.session == XR_NULL_HANDLE || !g.running)
		return false;

	const int requested = g.requestedPerformanceMode;
	if(requested == PERFORMANCE_MODE_AUTO){
		if(!g.cpuPerformanceHintSucceededThisSession){
			g.activePerformanceMode = PERFORMANCE_MODE_AUTO;
			if(!g.performanceAutoLogged){
				LOGI("OpenXR performance mode Auto: no CPU hint issued");
				g.performanceAutoLogged = true;
			}
			return true;
		}

		// XR_EXT_performance_settings has no clear/default call. After any
		// successful CPU hint, Auto therefore means the safe sustained level
		// until the next session restores the runtime's untouched policy.
		const bool ok = setCpuPerformanceLevel(
			XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT,
			"Auto safe fallback");
		if(ok){
			g.activePerformanceMode = PERFORMANCE_MODE_SUSTAINED_HIGH;
			g.cpuBoostActive = false;
			g.boostExpiryDisplayTime = 0;
			g.boostExpiryMonotonicNs = 0;
		}else if(g.cpuBoostActive){
			// Preserve the retry pump if this Auto request is the return path
			// from a Boost whose first safe-level request was rejected.
			g.activePerformanceMode = PERFORMANCE_MODE_BOOST;
			g.boostExpiryDisplayTime = g.predictedDisplayTime > 0 ?
				g.predictedDisplayTime + kPerformanceStepDownRetryNs : 0;
			g.boostExpiryMonotonicNs =
				monotonicTimeNs() + kPerformanceStepDownRetryNs;
		}else
			g.activePerformanceMode = -1;
		if(!force)
			LOGW("OpenXR performance Auto requested after a CPU hint; "
			     "full runtime default returns after application restart");
		return ok;
	}

	if(requested == PERFORMANCE_MODE_BOOST && g.boostThermallyBlocked){
		LOGW("OpenXR CPU Boost rejected: thermally blocked until a new session");
		g.requestedPerformanceMode = g.boostReturnMode;
		return false;
	}
	if(requested == PERFORMANCE_MODE_BOOST && g.cpuBoostActive &&
	   g.boostExpiryMonotonicNs > 0 &&
	   monotonicTimeNs() >= g.boostExpiryMonotonicNs){
		finishPerformanceBoost("25 second timeout", false);
		return true;
	}
	if(!force && g.activePerformanceMode == requested)
		return true;

	const XrPerfSettingsLevelEXT cpuLevel =
		requested == PERFORMANCE_MODE_BOOST ?
			XR_PERF_SETTINGS_LEVEL_BOOST_EXT :
			XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT;
	const bool ok = setCpuPerformanceLevel(
		cpuLevel, performanceModeName(requested));
	if(!ok){
		g.activePerformanceMode = -1;
		return false;
	}
	g.activePerformanceMode = requested;
	if(requested == PERFORMANCE_MODE_BOOST){
		// Reasserting the hint after READY/FOCUSED must never extend the 25 s
		// diagnostic window.
		if(!g.cpuBoostActive){
			g.cpuBoostActive = true;
			g.boostExpiryMonotonicNs =
				monotonicTimeNs() + kPerformanceBoostDurationNs;
			g.boostExpiryDisplayTime = g.predictedDisplayTime > 0 ?
				g.predictedDisplayTime + kPerformanceBoostDurationNs : 0;
			LOGI("OpenXR CPU Boost armed for at most 25 seconds");
		}
	}else{
		g.cpuBoostActive = false;
		g.boostExpiryDisplayTime = 0;
		g.boostExpiryMonotonicNs = 0;
	}
	return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

bool
create(android_app *app)
{
	// create() is called by gameThreadMain, the single thread which performs
	// both CGame::Step and render submission. gettid supplies the kernel TID
	// required by XR_KHR_android_thread_settings (not pthread_t).
	g.applicationMainThreadId = (uint32_t)syscall(SYS_gettid);
	LOGI("OpenXR application-main candidate tid=%u (game/render thread)",
	     g.applicationMainThreadId);
	if(!initialiseLoader(app)) return false;
	if(!createInstance(app)) return false;
	if(!createVulkan()) return false;
	if(!createSession()) return false;
	// Action sets must be attached before the first xrSyncActions and can
	// never be attached twice, so this belongs here rather than in the frame
	// loop.
	if(!createActions()) return false;
	if(!createSwapchain()) return false;
	// The full game renderer only needs the OpenXR colour views and this
	// command pool (the latter also uploads the debug/menu quad). Keep the
	// large two-eye depth image and the rest of the standalone bring-up
	// renderer out of release builds so they do not compete with librw's own
	// full-resolution scene/depth allocations during startup.
	if(!createSwapchainImageViews()) return false;
#ifdef MIAMIVR_BRINGUP
	if(!createDepthBuffer()) return false;
	if(!createRenderPass()) return false;
	if(!createPipeline()) return false;
	if(!createPerImageBringupResources()) return false;

	if(!uploadThroughHostBuffer(kCubeVertices, sizeof(kCubeVertices),
	                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
	                            &g.vertexBuffer, &g.vertexMemory))
		return false;
	if(!uploadThroughHostBuffer(kCubeIndices, sizeof(kCubeIndices),
	                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
	                            &g.indexBuffer, &g.indexMemory))
		return false;
#else
	LOGI("release renderer bootstrap: standalone full-resolution depth/pipeline skipped");
#endif

	LOGI("OpenXR/Vulkan session ready");
	return true;
}

bool
getContext(GraphicsContext *out)
{
	if(out == nullptr || g.device == VK_NULL_HANDLE)
		return false;
	out->instance = g.vkInstance;
	out->physicalDevice = g.physicalDevice;
	out->device = g.device;
	out->fragmentStoresAndAtomicsEnabled = g.fragmentStoresAndAtomicsEnabled;
	out->queue = g.queue;
	out->queueFamilyIndex = g.queueFamily;
	out->width = g.renderWidth;
	out->height = g.renderHeight;
	out->renderScaleEffectivePercent = g.renderScaleEffectivePercent;
	out->viewCount = 2;
	out->colourFormat = (VkFormat)g.swapchainViewFormat;
	return true;
}

bool
getRenderScaleStatus(RenderScaleStatus *out)
{
	if(out == nullptr || g.renderWidth == 0 || g.renderHeight == 0)
		return false;
	out->requestedPercent = g.renderScaleRequestedPercent;
	out->selectedPresetPercent = g.renderScaleSelectedPresetPercent;
	out->effectivePercent = g.renderScaleEffectivePercent;
	out->recommendedWidth = g.renderScaleRecommendedWidth;
	out->recommendedHeight = g.renderScaleRecommendedHeight;
	out->actualWidth = g.renderWidth;
	out->actualHeight = g.renderHeight;
	out->runtimeMaxWidth = g.renderScaleRuntimeMaxWidth;
	out->runtimeMaxHeight = g.renderScaleRuntimeMaxHeight;
	out->fallbackReason = g.renderScaleFallbackReason;
	out->previousFallbackRequestedPercent =
		g.previousRenderScaleFallbackRequestedPercent;
	out->previousFallbackPercent = g.previousRenderScaleFallbackPercent;
	out->previousFallbackReason = g.previousRenderScaleFallbackReason;
	return true;
}

const char *
getRenderScaleFallbackReasonName(int reason)
{
	switch(reason){
	case RENDER_SCALE_FALLBACK_NONE:
		return "NONE";
	case RENDER_SCALE_FALLBACK_RUNTIME_LIMIT:
		return "RUNTIME LIMIT";
	case RENDER_SCALE_FALLBACK_SWAPCHAIN_ALLOCATION:
		return "SWAPCHAIN ALLOCATION";
	case RENDER_SCALE_FALLBACK_GAME_RENDERER_ALLOCATION:
		return "GAME RENDERER ALLOCATION";
	default:
		return "UNKNOWN";
	}
}

void
confirmRenderScaleRendererReady(void)
{
	// Do not clear a current fallback, and keep the most recent recovery note
	// visible while running at the conservative 100% value chosen for it.
	if(g.renderScaleFallbackReason != RENDER_SCALE_FALLBACK_NONE ||
	   g.renderScaleRequestedPercent <= 100 ||
	   g.previousRenderScaleFallbackReason == RENDER_SCALE_FALLBACK_NONE)
		return;

	char settingsPath[512];
	snprintf(settingsPath, sizeof(settingsPath), "%s/vr_settings.ini",
		platform::gameDataRoot());
	const bool cleared =
		WritePrivateProfileStringA("VR", "RenderScaleLastFallbackRequest",
			"0", settingsPath) &&
		WritePrivateProfileStringA("VR", "RenderScaleLastFallbackPercent",
			"0", settingsPath) &&
		WritePrivateProfileStringA("VR", "RenderScaleLastFallbackReason",
			"0", settingsPath);
	if(cleared){
		LOGI("complete renderer confirmed at %.2f%%; cleared old fallback history",
		     g.renderScaleEffectivePercent);
		g.previousRenderScaleFallbackRequestedPercent = 0;
		g.previousRenderScaleFallbackPercent = 0;
		g.previousRenderScaleFallbackReason = RENDER_SCALE_FALLBACK_NONE;
	}else
		LOGW("complete renderer succeeded but fallback history could not be cleared");
}

void
setPerformanceMetricsEnabled(bool enabled)
{
	if(!g.hasPerformanceMetricsExt || g.session == XR_NULL_HANDLE ||
	   g.setPerformanceMetricsState == nullptr)
		return;
	if(g.performanceMetricsEnabled == enabled)
		return;

	XrPerformanceMetricsStateMETA state = {
		XR_TYPE_PERFORMANCE_METRICS_STATE_META };
	state.enabled = enabled ? XR_TRUE : XR_FALSE;
	const XrResult result =
		g.setPerformanceMetricsState(g.session, &state);
	if(XR_SUCCEEDED(result)){
		g.performanceMetricsEnabled = enabled;
		g.appCpuFrameTimeValid = false;
		g.appGpuFrameTimeValid = false;
		LOGI("XR_META performance metrics %s",
		     enabled ? "enabled" : "disabled");
	}else
		LOGE("xrSetPerformanceMetricsStateMETA(%d) failed: %d",
		     enabled ? 1 : 0, (int)result);
}

float
getDisplayRefreshRateHz(void)
{
	return effectiveRefreshHz();
}

bool
getPerformanceMetrics(PerformanceMetrics *out)
{
	if(out == nullptr)
		return false;
	out->appCpuFrameMs = g.appCpuFrameTimeMs;
	out->appGpuFrameMs = g.appGpuFrameTimeMs;
	out->displayRefreshRateHz = effectiveRefreshHz();
	out->appCpuFrameValid = g.performanceMetricsEnabled &&
		g.appCpuFrameTimeValid;
	out->appGpuFrameValid = g.performanceMetricsEnabled &&
		g.appGpuFrameTimeValid;
	return g.hasPerformanceMetricsExt;
}

long long
getPredictedDisplayTimeNs(void)
{
	return g.lastPredictedDisplayTimeNs;
}

void
setDebugOverlay(const unsigned char *rgba, int width, int height)
{
	const bool validSize =
		width > 0 && height > 0 &&
		width <= g.debugWidth && height <= g.debugHeight;
	const bool wasVisible = g.debugVisible;
	const bool sameContentSize =
		width == g.debugContentWidth && height == g.debugContentHeight;
	g.debugVisible = rgba != nullptr && validSize;
	// The menu panel is placed in the world each time it opens.
	if(!g.debugVisible || !wasVisible)
		g.menuAnchorValid = false;
	if(rgba == nullptr || g.debugSwapchain == XR_NULL_HANDLE ||
	   g.commandPool == VK_NULL_HANDLE || !validSize)
		return;
	// The compact profiler changes slowly and its old implementation uploaded
	// 256 KiB through a separate queue submit + CPU fence wait every frame.
	// That measurement perturbed the workload it was meant to observe. Keep
	// full 1024x768 menus frame-responsive, but refresh compact diagnostics at
	// 2 Hz. Visibility and size transitions always upload immediately. The
	// Uploads are submitted asynchronously below. A slower refresh still keeps
	// the diagnostic probe out of nearly every timing window.
	const bool compact = width <= 512;
	const XrTime now = g.predictedDisplayTime;
	const XrTime compactInterval = (XrTime)500000000;
	if(compact && wasVisible && sameContentSize &&
	   g.lastCompactDebugUploadTime != 0 && now != 0 &&
	   now-g.lastCompactDebugUploadTime < compactInterval)
		return;
	const VkDeviceSize capacity =
		(VkDeviceSize)g.debugWidth * g.debugHeight * 4;
	if(g.debugStaging == VK_NULL_HANDLE){
		if(!createBuffer(capacity, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		                 &g.debugStaging, &g.debugStagingMemory))
			return;
		if(vkMapMemory(g.device, g.debugStagingMemory, 0, capacity, 0,
		               &g.debugStagingMapped) != VK_SUCCESS)
			return;

		VkCommandBufferAllocateInfo commandInfo = {};
		commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		commandInfo.commandPool = g.commandPool;
		commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		commandInfo.commandBufferCount = 1;
		vkAllocateCommandBuffers(g.device, &commandInfo, &g.debugCommand);
		VkFenceCreateInfo fenceInfo = {};
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		vkCreateFence(g.device, &fenceInfo, nullptr, &g.debugFence);
	}
	if(g.debugStagingMapped == nullptr || g.debugCommand == VK_NULL_HANDLE)
		return;

	// Never stop the game thread for the profiler/settings quad. The previous
	// upload normally completes long before the next compact refresh; a large
	// menu may need to skip one update on a busy frame, which is preferable to
	// missing an immersive display frame.
	if(g.debugSubmissionPending){
		const VkResult status = vkGetFenceStatus(g.device, g.debugFence);
		if(status == VK_NOT_READY)
			return;
		if(status != VK_SUCCESS){
			LOGE("debug overlay fence status failed: %d", (int)status);
			return;
		}
		g.debugSubmissionPending = false;
	}

	if(compact)
		g.lastCompactDebugUploadTime = now;
	g.debugContentWidth = width;
	g.debugContentHeight = height;
	const VkDeviceSize contentSize =
		(VkDeviceSize)g.debugContentWidth * g.debugContentHeight * 4;
	memcpy(g.debugStagingMapped, rgba, (size_t)contentSize);

	uint32_t imageIndex = 0;
	XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	if(XR_FAILED(xrAcquireSwapchainImage(g.debugSwapchain, &acquireInfo, &imageIndex)))
		return;
	XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	waitInfo.timeout = XR_INFINITE_DURATION;
	XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	if(XR_FAILED(xrWaitSwapchainImage(g.debugSwapchain, &waitInfo))){
		xrReleaseSwapchainImage(g.debugSwapchain, &releaseInfo);
		return;
	}

	VkCommandBufferBeginInfo begin = {};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkResetCommandBuffer(g.debugCommand, 0);
	vkBeginCommandBuffer(g.debugCommand, &begin);

	VkImageMemoryBarrier barrier = {};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = g.debugImages[imageIndex];
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(g.debugCommand, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
	                     0, nullptr, 1, &barrier);

	VkBufferImageCopy region = {};
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	region.imageExtent.width = (uint32_t)g.debugContentWidth;
	region.imageExtent.height = (uint32_t)g.debugContentHeight;
	region.imageExtent.depth = 1;
	vkCmdCopyBufferToImage(g.debugCommand, g.debugStaging,
	                       g.debugImages[imageIndex],
	                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(g.debugCommand, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
	                     0, nullptr, 1, &barrier);

	vkEndCommandBuffer(g.debugCommand);
	VkSubmitInfo submit = {};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &g.debugCommand;
	vkResetFences(g.device, 1, &g.debugFence);
	const VkResult submitted =
		vkQueueSubmit(g.queue, 1, &submit, g.debugFence);
	g.debugSubmissionPending = submitted == VK_SUCCESS;
	if(submitted != VK_SUCCESS)
		LOGE("debug overlay submit failed: %d", (int)submitted);

	xrReleaseSwapchainImage(g.debugSwapchain, &releaseInfo);
}

void
setFrameRenderer(FrameRenderer renderer)
{
	g.frameRenderer = renderer;
}

#if MIAMIVR_STEAM_FRAME
void
setTheaterStereo(bool enabled)
{
	if(enabled != g.theaterStereo)
		LOGI("[vr] theater screen %s", enabled ? "3D (one layer per eye)" : "flat");
	g.theaterStereo = enabled;
}

void
setPointerFrontend(bool up)
{
	if(up != g.pointerFrontendUp)
		LOGI("[ui.pointer] frontend %s", up ? "up" : "gone");
	g.pointerFrontendUp = up;
}

void
resetTheaterAnchor(void)
{
	// The screen latches in front of the head the next theater frame.
	g.theaterAnchorValid = false;
	// So does the VR menu panel.
	g.menuAnchorValid = false;
}
#endif

void
setTheaterMode(bool enabled)
{
	g.theaterMode = enabled;
	if(!enabled){
		g.theaterAnchorValid = false;
		g.theaterSpace = XR_NULL_HANDLE;
	}
}

void
getInput(ControllerInput *out)
{
	if(out != nullptr)
		*out = g.input;
#if MIAMIVR_STEAM_FRAME
	// Latched by pollEvents; handed over exactly once.
	g.input.systemRecentre = false;
#endif
}

void
setPreferredDisplayRefreshRate(float hz)
{
	if(hz < 60.0f || hz > 144.0f ||
	   fabsf(hz - g.targetRefreshRateHz) < 0.5f)
		return;
	g.targetRefreshRateHz = hz;
#if MIAMIVR_STEAM_FRAME
	// The one request of the session may already be spent; the setting is
	// then picked up next launch.
	LOGI("[vr] refresh: setting %.0f Hz%s", hz,
	     g.refreshRequestedThisSession ? " (applies next launch)" : "");
	return;
#endif
	g.requestedRefreshRateHz = 0.0f;
	// Re-arm the retry pump in renderFrame; it re-requests on the next
	// focused frame and keeps retrying until the display confirms.
	g.refreshRateRetryCount = 0;
	g.nextRefreshRateRetryTime = 0;
	LOGI("display refresh target set to %.1f Hz", hz);
}

void
setPerformanceMode(int mode)
{
	if(mode < PERFORMANCE_MODE_AUTO || mode > PERFORMANCE_MODE_BOOST){
		LOGW("invalid OpenXR performance mode %d; using Auto", mode);
		mode = PERFORMANCE_MODE_AUTO;
	}
	if(mode == PERFORMANCE_MODE_BOOST && g.boostThermallyBlocked){
		LOGW("OpenXR CPU Boost request rejected: thermally blocked until restart");
		return;
	}
	if(mode == PERFORMANCE_MODE_BOOST &&
	   g.requestedPerformanceMode != PERFORMANCE_MODE_BOOST)
		g.boostReturnMode =
			g.requestedPerformanceMode == PERFORMANCE_MODE_SUSTAINED_HIGH ?
				PERFORMANCE_MODE_SUSTAINED_HIGH : PERFORMANCE_MODE_AUTO;
	const bool changed = g.requestedPerformanceMode != mode;
	g.requestedPerformanceMode = mode;
	if(changed)
		LOGI("OpenXR performance mode requested: %s",
		     performanceModeName(mode));
	// Safe before create/session startup: the value remains pending and the
	// READY/FOCUSED event paths apply it once the session is usable.
	if(g.session != XR_NULL_HANDLE && g.running)
		applyPerformanceMode(false);
}

int
getPerformanceMode(void)
{
	return g.requestedPerformanceMode;
}

void
setGpuPerformanceMode(int mode)
{
	if(mode < PERFORMANCE_MODE_AUTO || mode > PERFORMANCE_MODE_BOOST){
		LOGW("invalid OpenXR GPU performance mode %d; using Sustained High", mode);
		mode = PERFORMANCE_MODE_SUSTAINED_HIGH;
	}
	const bool changed = g.requestedGpuPerformanceMode != mode;
	g.requestedGpuPerformanceMode = mode;
	if(changed)
		LOGI("OpenXR GPU performance mode requested: %s",
		     performanceModeName(mode));
	if(g.session != XR_NULL_HANDLE && g.running)
		applyGpuPerformanceMode(false);
}

int
getGpuPerformanceMode(void)
{
	return g.requestedGpuPerformanceMode;
}

int
getActiveGpuPerformanceMode(void)
{
	return g.activeGpuPerformanceMode;
}

bool
isPerformanceModeSupported(void)
{
	return g.hasPerformanceSettingsExt && g.setPerformanceLevel != nullptr;
}

bool
isPerformanceBoostBlocked(void)
{
	return g.boostThermallyBlocked;
}

int
getActivePerformanceMode(void)
{
	return g.activePerformanceMode;
}

void
triggerHaptic(int hand, float amplitude, float frequencyHz, float durationMs)
{
	if(hand < 0 || hand > 1 || g.session == XR_NULL_HANDLE ||
	   g.hapticAction == XR_NULL_HANDLE ||
	   g.sessionState != XR_SESSION_STATE_FOCUSED)
		return;
	XrHapticVibration vibration = { XR_TYPE_HAPTIC_VIBRATION };
	vibration.amplitude = amplitude < 0.f ? 0.f : amplitude > 1.f ? 1.f : amplitude;
	vibration.frequency = frequencyHz;
	vibration.duration = (XrDuration)(durationMs * 1000000.0f);
	XrHapticActionInfo info = { XR_TYPE_HAPTIC_ACTION_INFO };
	info.action = g.hapticAction;
	info.subactionPath = g.handPath[hand];
	xrApplyHapticFeedback(g.session, &info,
	                      (const XrHapticBaseHeader *)&vibration);
}

#if MIAMIVR_STEAM_FRAME
static void
logCurrentInteractionProfiles(void)
{
	for(int hand = 0; hand < 2; hand++){
		XrInteractionProfileState state = { XR_TYPE_INTERACTION_PROFILE_STATE };
		if(XR_FAILED(xrGetCurrentInteractionProfile(g.session, g.handPath[hand],
		                                            &state)))
			continue;
		char name[XR_MAX_PATH_LENGTH] = "none";
		uint32_t length = 0;
		if(state.interactionProfile != XR_NULL_PATH)
			xrPathToString(g.instance, state.interactionProfile,
			               sizeof(name), &length, name);
		LOGI("[vr] interaction profile %s: %s", hand ? "right" : "left", name);
	}
}
#endif

bool
pollEvents(void)
{
	XrEventDataBuffer event;
	for(;;){
		event = { XR_TYPE_EVENT_DATA_BUFFER };
		if(xrPollEvent(g.instance, &event) != XR_SUCCESS)
			break;

		switch(event.type){
		case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
			const XrEventDataSessionStateChanged &changed =
				*(const XrEventDataSessionStateChanged *)&event;
			g.sessionState = changed.state;
			LOGI("session state -> %d", (int)g.sessionState);

			if(g.sessionState == XR_SESSION_STATE_READY){
				XrSessionBeginInfo beginInfo = { XR_TYPE_SESSION_BEGIN_INFO };
				beginInfo.primaryViewConfigurationType =
					XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				if(XR_SUCCEEDED(xrBeginSession(g.session, &beginInfo))){
					g.running = true;
#if MIAMIVR_STEAM_FRAME
					// Frames are never held for a rate change on the Frame.
					g.refreshRateReadyForFrames = true;
#else
					g.refreshRateReadyForFrames = !g.hasRefreshRateExt;
#endif
					applyAndroidThreadSettings("session READY");
					g.refreshRateRequestAttempted = false;
					g.refreshRateRetryCount = 0;
					g.nextRefreshRateRetryTime = 0;
					g.requestedRefreshRateHz = 0.0f;
					applyPerformanceMode(true);
					applyGpuPerformanceMode(true);
				}
			}else if(g.sessionState == XR_SESSION_STATE_FOCUSED){
				applyAndroidThreadSettings("session FOCUSED");
				// Some runtimes re-evaluate clock policy as focus changes, so
				// reassert explicit hints after the session gains focus.
				applyPerformanceMode(true);
				applyGpuPerformanceMode(true);
				// Apply the stable 72 Hz target after the session owns focus.
				// This avoids frame-rate changes while the game is running.
				if(!g.refreshRateRequestAttempted){
					g.refreshRateRequestAttempted = true;
					const bool requested =
						requestPreferredDisplayRefreshRate();
#if MIAMIVR_STEAM_FRAME
					g.refreshRateReadyForFrames = true;
					(void)requested;
#else
					g.refreshRateReadyForFrames =
						!requested ||
						refreshAtGoal(g.currentRefreshRateHz);
#endif
					if(!g.refreshRateReadyForFrames)
						LOGI("holding application layers until display "
						     "refresh settles at %.1f Hz",
						     refreshGoalHz());
					g.refreshRateRetryCount = 1;
				}
			}else if(g.sessionState == XR_SESSION_STATE_STOPPING){
				g.running = false;
				xrEndSession(g.session);
			}else if(g.sessionState == XR_SESSION_STATE_EXITING ||
			         g.sessionState == XR_SESSION_STATE_LOSS_PENDING){
				g.running = false;
				g.exitRequested = true;
			}
			break;
		}
		case XR_TYPE_EVENT_DATA_PERF_SETTINGS_EXT: {
			const XrEventDataPerfSettingsEXT &changed =
				*(const XrEventDataPerfSettingsEXT *)&event;
			if(changed.toLevel == XR_PERF_SETTINGS_NOTIF_LEVEL_NORMAL_EXT)
				LOGI("OpenXR performance notification: %s/%s %d -> %d",
				     performanceDomainName(changed.domain),
				     performanceSubDomainName(changed.subDomain),
				     (int)changed.fromLevel, (int)changed.toLevel);
			else
				LOGW("OpenXR performance warning: %s/%s %d -> %d",
				     performanceDomainName(changed.domain),
				     performanceSubDomainName(changed.subDomain),
				     (int)changed.fromLevel, (int)changed.toLevel);
			if(changed.subDomain == XR_PERF_SETTINGS_SUB_DOMAIN_THERMAL_EXT &&
			   changed.toLevel != XR_PERF_SETTINGS_NOTIF_LEVEL_NORMAL_EXT){
				g.boostThermallyBlocked = true;
				if(g.cpuBoostActive ||
				   g.requestedPerformanceMode == PERFORMANCE_MODE_BOOST)
					finishPerformanceBoost("thermal warning", true);
				else
					LOGW("OpenXR CPU Boost blocked until a new session due to thermal warning");
			}
			break;
		}
		case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
			g.exitRequested = true;
			break;
#if MIAMIVR_STEAM_FRAME
		case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
			// A system recentre: treat it as the View hold.
			LOGI("[vr] reference space change pending: recentre");
			g.input.systemRecentre = true;
			break;
		case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
			logCurrentInteractionProfiles();
			break;
#endif
		case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
			const XrEventDataDisplayRefreshRateChangedFB &changed =
				*(const XrEventDataDisplayRefreshRateChangedFB *)&event;
			g.currentRefreshRateHz = changed.toDisplayRefreshRate;
			LOGI("display refresh changed: %.1f -> %.1f Hz",
			     changed.fromDisplayRefreshRate,
			     changed.toDisplayRefreshRate);
#if MIAMIVR_STEAM_FRAME
			perfTaintWindow();
#else
			g.refreshRateReadyForFrames =
				refreshAtGoal(changed.toDisplayRefreshRate);
			if(!g.refreshRateReadyForFrames){
				g.refreshRateRetryCount = 0;
				g.nextRefreshRateRetryTime = 0;
			}
#endif
			break;
		}
		default:
			break;
		}
	}
	return !g.exitRequested;
}

#if MIAMIVR_STEAM_FRAME
// The theater screen as a projection layer: per eye, a view with the screen's
// (yaw-only) orientation, the eye's own position, and the field of view the
// screen rectangle subtends from there. Returns false when an eye is not in
// front of the screen, which leaves the flat quad in place.
static bool
makeTheaterProjectionViews(const XrView views[2],
                           XrCompositionLayerProjectionView out[2])
{
	const XrQuaternionf &q = g.theaterPose.orientation;
	for(int eye = 0; eye < 2; eye++){
		// The eye in the screen's frame: rotate (eye - centre) by q^-1.
		// q is a rotation about +Y only (x = z = 0).
		const float dx = views[eye].pose.position.x - g.theaterPose.position.x;
		const float dy = views[eye].pose.position.y - g.theaterPose.position.y;
		const float dz = views[eye].pose.position.z - g.theaterPose.position.z;
		const float c = q.w*q.w - q.y*q.y;      // cos(yaw)
		const float s = 2.0f*q.w*q.y;          // sin(yaw)
		const float ex = c*dx - s*dz;
		const float ey = dy;
		const float ez = s*dx + c*dz;
		// The screen faces +Z; an eye in front of it has ez > 0.
		if(ez < 0.1f)
			return false;
		const float halfW = 0.5f*kTheaterScreenWidth;
		const float halfH = 0.5f*kTheaterScreenHeight;
		out[eye] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
		out[eye].pose.orientation = q;
		out[eye].pose.position = views[eye].pose.position;
		out[eye].fov.angleLeft  = atan2f(-halfW - ex, ez);
		out[eye].fov.angleRight = atan2f( halfW - ex, ez);
		out[eye].fov.angleDown  = atan2f(-halfH - ey, ez);
		out[eye].fov.angleUp    = atan2f( halfH - ey, ez);
		out[eye].subImage.swapchain = g.swapchain;
		out[eye].subImage.imageRect.extent = {
			(int32_t)g.renderWidth, (int32_t)g.renderHeight };
		out[eye].subImage.imageArrayIndex = (uint32_t)eye;
	}
	return true;
}
#endif

bool
shouldRender(void)
{
	// Keep pumping xrWaitFrame/xrBeginFrame/xrEndFrame while a requested panel
	// refresh change is pending.  Quest does not advance the session to
	// FOCUSED or deliver the changed event until frames are pumped.
	return g.running;
}

bool
hasTrackedGameplayHeadPose(void)
{
	return g.headPoseTrackedForFrame && !g.theaterMode;
}

void
renderFrame(void)
{
	g.headPoseTrackedForFrame = false;
#if MIAMIVR_STEAM_FRAME
	// Polled before the running check so a stalled session still reports.
	perfPoll();
#endif
	if(!g.running)
		return;

	XrFrameWaitInfo waitInfo = { XR_TYPE_FRAME_WAIT_INFO };
	XrFrameState frameState = { XR_TYPE_FRAME_STATE };
	if(XR_FAILED(xrWaitFrame(g.session, &waitInfo, &frameState)))
		return;
#if MIAMIVR_STEAM_FRAME
	// Timed from here: xrWaitFrame is the pacing block, not work.
	const long long perfFrameStartNs = diagNowNs();
	gPerf.gameMs = 0.0f;
	g.diagShouldRender = frameState.shouldRender != XR_FALSE;
	g.diagSyntheticHead = false;
	// vr-diag.ini force_render: draw and step the game whatever the runtime
	// says, so a headset left on a desk (SYNCHRONIZED, shouldRender false)
	// still runs the game.
	const bool forceRender = frame_diag::config().forceRender;
#else
	const bool forceRender = false;
#endif
	g.predictedDisplayTime = frameState.predictedDisplayTime;
	g.lastPredictedDisplayTimeNs = (long long)frameState.predictedDisplayTime;
	if(frameState.predictedDisplayPeriod > 0){
		const float hz = 1.0e9f/(float)frameState.predictedDisplayPeriod;
		if(hz > 20.0f && hz < 300.0f)
			g.measuredRefreshHz = hz;
	}
	if(g.cpuBoostActive){
		if(g.boostExpiryDisplayTime == 0)
			g.boostExpiryDisplayTime = frameState.predictedDisplayTime +
				kPerformanceBoostDurationNs;
		const bool displayExpired =
			frameState.predictedDisplayTime >= g.boostExpiryDisplayTime;
		const bool monotonicExpired = g.boostExpiryMonotonicNs > 0 &&
			monotonicTimeNs() >= g.boostExpiryMonotonicNs;
		if(displayExpired || monotonicExpired)
			finishPerformanceBoost("25 second timeout", false);
	}

	// A focused Quest session can acknowledge a rate request before the display
	// has actually switched. Retry at a low rate for a bounded period, stopping
	// as soon as the runtime confirms the stable 72 Hz target.
#if !MIAMIVR_STEAM_FRAME
	if(g.sessionState == XR_SESSION_STATE_FOCUSED && g.hasRefreshRateExt &&
	   g.getRefreshRate != nullptr && g.refreshRateRetryCount < 8 &&
	   (g.nextRefreshRateRetryTime == 0 ||
	    frameState.predictedDisplayTime >= g.nextRefreshRateRetryTime)){
		float current = 0.0f;
		const XrResult currentResult =
			g.getRefreshRate(g.session, &current);
		if(XR_SUCCEEDED(currentResult) && refreshAtGoal(current)){
			g.currentRefreshRateHz = current;
			g.refreshRateReadyForFrames = true;
			LOGI("display refresh confirmed at %.1f Hz", current);
			g.refreshRateRetryCount = 8;
		}else{
			requestPreferredDisplayRefreshRate();
			g.refreshRateRetryCount++;
			g.nextRefreshRateRetryTime =
				frameState.predictedDisplayTime + (XrTime)1000000000;
		}
	}
#endif

	XrFrameBeginInfo beginInfo = { XR_TYPE_FRAME_BEGIN_INFO };
	if(XR_FAILED(xrBeginFrame(g.session, &beginInfo)))
		return;

	// Sampled once per frame, after the wait, so the state the game reads is
	// the freshest available for this display time.
	syncInput();

	XrCompositionLayerProjectionView projectionViews[2] = {};
	XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	XrCompositionLayerQuad theaterLayer = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	XrCompositionLayerQuad debugLayer = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	const XrCompositionLayerBaseHeader *layers[5] = {};
#if MIAMIVR_STEAM_FRAME
	XrCompositionLayerQuad pointerQuads[2] = { { XR_TYPE_COMPOSITION_LAYER_QUAD },
	                                           { XR_TYPE_COMPOSITION_LAYER_QUAD } };
#endif
	uint32_t layerCount = 0;
	bool menuSubmitted = false;

	if((frameState.shouldRender || forceRender) &&
	   (g.refreshRateReadyForFrames || forceRender)){
		XrViewState viewState = { XR_TYPE_VIEW_STATE };
		XrViewLocateInfo locateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
		locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		locateInfo.displayTime = frameState.predictedDisplayTime;
		locateInfo.space = g.space;

		XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
		uint32_t viewCount = 0;
		XrResult located = xrLocateViews(g.session, &locateInfo, &viewState,
		                                 2, &viewCount, views);

		// Fall back to LOCAL when the chosen space is not reporting an
		// orientation. STAGE stops doing so without a configured play area,
		// and refusing to render then is what left the compositor with nothing
		// to show but its placeholder panel.
		XrSpace submitSpace = g.space;
		if((XR_FAILED(located) ||
		    (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0) &&
		   g.localSpace != XR_NULL_HANDLE){
			locateInfo.space = g.localSpace;
			located = xrLocateViews(g.session, &locateInfo, &viewState,
			                        2, &viewCount, views);
			submitSpace = g.localSpace;
		}

		// Only orientation is required. A missing position means the runtime
		// could not place the head in the world, which is worth rendering
		// through with a stale position rather than dropping the frame.
		static XrViewStateFlags reportedFlags = ~(XrViewStateFlags)0;
		if(viewState.viewStateFlags != reportedFlags){
			reportedFlags = viewState.viewStateFlags;
			LOGI("view state flags 0x%llx", (unsigned long long)reportedFlags);
		}

#if MIAMIVR_STEAM_FRAME
		bool syntheticHead = false;
		if(XR_SUCCEEDED(located) && viewCount == 2)
			diagCacheFov(views);
		if(forceRender){
			// Off the desk the runtime gives no usable head pose (and before
			// FOCUSED it may give a stale one), so pose a fixed head instead.
			const bool orientationValid =
				XR_SUCCEEDED(located) && viewCount == 2 &&
				(viewState.viewStateFlags &
				 XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
			if(!orientationValid || g.sessionState != XR_SESSION_STATE_FOCUSED){
				diagMakeSyntheticViews(views, frame_diag::config().yawDeg);
				located = XR_SUCCESS;
				viewCount = 2;
				viewState.viewStateFlags |=
					XR_VIEW_STATE_POSITION_VALID_BIT |
					XR_VIEW_STATE_ORIENTATION_VALID_BIT;
				syntheticHead = true;
				static bool reportedSynthetic = false;
				if(!reportedSynthetic){
					reportedSynthetic = true;
					LOGI("[vr] diag synthetic head active (session state %s, "
					     "yaw %g deg, real orientation %s)",
					     diagSessionStateName(g.sessionState),
					     (double)frame_diag::config().yawDeg,
					     orientationValid ? "valid" : "invalid");
				}
			}
		}
		g.diagSyntheticHead = syntheticHead;
#endif

		const XrViewStateFlags trackedHeadFlags =
			XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT |
			XR_VIEW_STATE_POSITION_TRACKED_BIT | XR_VIEW_STATE_ORIENTATION_TRACKED_BIT;
		g.headPoseTrackedForFrame = XR_SUCCEEDED(located) && viewCount == 2 &&
			g.sessionState == XR_SESSION_STATE_FOCUSED &&
			(viewState.viewStateFlags & trackedHeadFlags) == trackedHeadFlags;
#if MIAMIVR_STEAM_FRAME
		// Gameplay head tracking is gated on this; the synthetic head is the
		// pose the game should follow.
		if(syntheticHead)
			g.headPoseTrackedForFrame = true;
#endif
		if(XR_SUCCEEDED(located) &&
		   (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0 &&
		   (locateControllerPoses(submitSpace,
		                          frameState.predictedDisplayTime), true) &&
		   renderLayer(views)){

			if(g.theaterMode){
				// Latch once on entry. Only yaw is retained, exactly like the
				// desktop cinema layer, so the screen stays upright and fixed
				// in the room while the player looks around.
				if(!g.theaterAnchorValid || g.theaterSpace != submitSpace){
					XrPosef head = views[0].pose;
					head.position.x =
						(views[0].pose.position.x +
						 views[1].pose.position.x)*0.5f;
					head.position.y =
						(views[0].pose.position.y +
						 views[1].pose.position.y)*0.5f;
					head.position.z =
						(views[0].pose.position.z +
						 views[1].pose.position.z)*0.5f;
					const XrQuaternionf &q = head.orientation;
					const float fx = -(2.0f*(q.x*q.z + q.y*q.w));
					const float fz =
						-(1.0f - 2.0f*(q.x*q.x + q.y*q.y));
					const float yaw = atan2f(-fx, -fz);
					g.theaterPose = {};
					g.theaterPose.orientation.y = sinf(yaw*0.5f);
					g.theaterPose.orientation.w = cosf(yaw*0.5f);
					const float forwardX = -sinf(yaw);
					const float forwardZ = -cosf(yaw);
					g.theaterPose.position.x =
						head.position.x + forwardX*2.0f;
					g.theaterPose.position.y = head.position.y;
					g.theaterPose.position.z =
						head.position.z + forwardZ*2.0f;
					g.theaterSpace = submitSpace;
					g.theaterAnchorValid = true;
				}

				theaterLayer.space = submitSpace;
				theaterLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
				theaterLayer.subImage.swapchain = g.swapchain;
				theaterLayer.subImage.imageRect.extent = {
					(int32_t)g.renderWidth, (int32_t)g.renderHeight };
				theaterLayer.subImage.imageArrayIndex = 0;
				theaterLayer.pose = g.theaterPose;
				// Keep the complete 16:9 frame comfortably inside the Quest
				// binocular field of view. The old 3.2 m quad filled roughly
				// 77 degrees horizontally at this two-metre distance.
				theaterLayer.size.width = kTheaterScreenWidth;
				theaterLayer.size.height = kTheaterScreenHeight;
				layers[0] =
					(const XrCompositionLayerBaseHeader *)&theaterLayer;
				layerCount = 1;
#if MIAMIVR_STEAM_FRAME
				// 3D cinema. The renderer drew the two layers from two cameras
				// converged on the screen plane, so each eye must see only its
				// own. SteamVR showed per-eye quads (eyeVisibility LEFT/RIGHT)
				// flat on the Frame, so the screen goes out as a projection
				// layer instead: each eye's view keeps the screen's
				// orientation and its field of view is exactly the screen
				// rectangle as that eye sees it. The image then lands on the
				// same rectangle in the room, with each eye's own picture.
				if(g.theaterStereo &&
				   makeTheaterProjectionViews(views, projectionViews)){
					layer.space = submitSpace;
					layer.viewCount = 2;
					layer.views = projectionViews;
					layers[0] = (const XrCompositionLayerBaseHeader *)&layer;
					layerCount = 1;
				}
#endif
			}else{
				for(int eye = 0; eye < 2; eye++){
					projectionViews[eye] = {
						XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
					projectionViews[eye].pose = views[eye].pose;
					projectionViews[eye].fov = views[eye].fov;
					projectionViews[eye].subImage.swapchain = g.swapchain;
					projectionViews[eye].subImage.imageRect.extent = {
						(int32_t)g.renderWidth,
						(int32_t)g.renderHeight };
					// Both eyes share one swapchain; the array index is what
					// separates them, matching the multiview layer.
					projectionViews[eye].subImage.imageArrayIndex =
						(uint32_t)eye;
				}

				layer.space = submitSpace;
				layer.viewCount = 2;
				layer.views = projectionViews;
				layers[0] =
					(const XrCompositionLayerBaseHeader *)&layer;
				layerCount = 1;
			}

			// Debug overlay and VR menu: size and distance are the desktop's.
			if(g.debugVisible && g.debugSwapchain != XR_NULL_HANDLE &&
			   g.viewSpace != XR_NULL_HANDLE){
				const bool fullMenu = g.debugContentWidth > 512;
				debugLayer.layerFlags =
					XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
				debugLayer.space = g.viewSpace;
				debugLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
				debugLayer.subImage.swapchain = g.debugSwapchain;
				debugLayer.subImage.imageRect.extent = {
					(int32_t)g.debugContentWidth,
					(int32_t)g.debugContentHeight };
				debugLayer.pose.orientation.w = 1.0f;
				debugLayer.pose.position.y = fullMenu ? 0.0f : -0.24f;
				debugLayer.pose.position.z = fullMenu ? -1.65f : -1.5f;
#if MIAMIVR_STEAM_FRAME
				// Never roll with the head (standard.md sections 3 and 4).
				// Head position and orientation, as for the theater anchor.
				XrPosef menuHead = views[0].pose;
				menuHead.position.x =
					(views[0].pose.position.x + views[1].pose.position.x)*0.5f;
				menuHead.position.y =
					(views[0].pose.position.y + views[1].pose.position.y)*0.5f;
				menuHead.position.z =
					(views[0].pose.position.z + views[1].pose.position.z)*0.5f;
				float menuYawHold = g.hudYawHold;
				const XrQuaternionf rollFree =
					rollFreeOrientation(menuHead.orientation, &menuYawHold);
				debugLayer.space = submitSpace;
				if(fullMenu){
					// World-locked panel with a yaw-only anchor, captured when
					// the menu opens and on recentre, at the head's eye height.
					if(!g.menuAnchorValid || g.menuSpace != submitSpace){
						const XrQuaternionf &q = menuHead.orientation;
						const float fx = -(2.0f*(q.x*q.z + q.y*q.w));
						const float fz = -(1.0f - 2.0f*(q.x*q.x + q.y*q.y));
						g.menuYaw = (fx*fx + fz*fz > 0.0025f) ?
							atan2f(-fx, -fz) : menuYawHold;
						g.menuHeadPos = menuHead.position;
						g.menuSpace = submitSpace;
						g.menuAnchorValid = true;
					}
					debugLayer.pose.orientation = {
						0.0f, sinf(g.menuYaw*0.5f), 0.0f,
						cosf(g.menuYaw*0.5f) };
					debugLayer.pose.position.x =
						g.menuHeadPos.x - sinf(g.menuYaw)*1.65f;
					debugLayer.pose.position.y = g.menuHeadPos.y;
					debugLayer.pose.position.z =
						g.menuHeadPos.z - cosf(g.menuYaw)*1.65f;
				}else{
					// The compact overlay keeps following the head, yaw and
					// pitch only.
					const XrQuaternionf &r = rollFree;
					const float v[3] = { 0.0f, -0.24f, -1.5f };
					const float cx = r.y*v[2] - r.z*v[1];
					const float cy = r.z*v[0] - r.x*v[2];
					const float cz = r.x*v[1] - r.y*v[0];
					const float dx = r.y*cz - r.z*cy;
					const float dy = r.z*cx - r.x*cz;
					const float dz = r.x*cy - r.y*cx;
					debugLayer.pose.orientation = rollFree;
					debugLayer.pose.position.x = menuHead.position.x +
						v[0] + 2.0f*(r.w*cx + dx);
					debugLayer.pose.position.y = menuHead.position.y +
						v[1] + 2.0f*(r.w*cy + dy);
					debugLayer.pose.position.z = menuHead.position.z +
						v[2] + 2.0f*(r.w*cz + dz);
				}
#endif
				debugLayer.size.width = fullMenu ? 2.15f : 1.2f;
				debugLayer.size.height =
					debugLayer.size.width*
					(float)g.debugContentHeight/
					(float)g.debugContentWidth;
				layers[layerCount++] =
					(const XrCompositionLayerBaseHeader *)&debugLayer;
				menuSubmitted = fullMenu;
			}

#if MIAMIVR_STEAM_FRAME
			// The laser pointer goes last: later layers composite over earlier
			// ones, so the beam and dot stay above the menu panel, the cinema
			// screen and the world. The ray is tested against the two quads
			// exactly as they were just submitted.
			{
				updatePointer(views, g.theaterMode, theaterLayer.pose,
				              menuSubmitted, debugLayer.pose, debugLayer.size);
				const int pointerLayers =
					presentPointerLayers(submitSpace, pointerQuads);
				for(int i = 0; i < pointerLayers; i++)
					layers[layerCount++] =
						(const XrCompositionLayerBaseHeader *)&pointerQuads[i];
				State::DiagLayers &snap = g.diagLayers;
				snap.valid = true;
				snap.head = views[0].pose;
				snap.head.position.x = (views[0].pose.position.x + views[1].pose.position.x)*0.5f;
				snap.head.position.y = (views[0].pose.position.y + views[1].pose.position.y)*0.5f;
				snap.head.position.z = (views[0].pose.position.z + views[1].pose.position.z)*0.5f;
				snap.theater = g.theaterMode;
				snap.theaterPose = theaterLayer.pose;
				snap.theaterSize = theaterLayer.size;
				snap.menu = g.debugVisible;
				snap.menuPose = debugLayer.pose;
				snap.menuSize = debugLayer.size;
				snap.pointerCount = pointerLayers;
				for(int i = 0; i < pointerLayers; i++){
					snap.pointerPose[i] = pointerQuads[i].pose;
					snap.pointerSize[i] = pointerQuads[i].size;
					snap.pointerRect[i][0] = pointerQuads[i].subImage.imageRect.offset.x;
					snap.pointerRect[i][1] = pointerQuads[i].subImage.imageRect.offset.y;
					snap.pointerRect[i][2] = pointerQuads[i].subImage.imageRect.extent.width;
					snap.pointerRect[i][3] = pointerQuads[i].subImage.imageRect.extent.height;
				}
			}
#endif
		}
	}

	g.headPoseTrackedForFrame = false;
	// An empty frame is what makes Horizon OS drop the app back to its "still
	// running" panel, so say the first few times it happens and why, instead of
	// letting it pass silently.
	if(layerCount == 0 && g.refreshRateReadyForFrames){
		static int reportedEmpty = 0;
		if(reportedEmpty < 5){
			reportedEmpty++;
			LOGE("empty frame submitted (shouldRender=%d): the compositor will "
			     "show the placeholder panel", (int)frameState.shouldRender);
		}
	}

	XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
	endInfo.displayTime = frameState.predictedDisplayTime;
	endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	endInfo.layerCount = layerCount;
	endInfo.layers = layers;
	const XrResult ended = xrEndFrame(g.session, &endInfo);
	if(XR_FAILED(ended)){
		static int reportedEnd = 0;
		if(reportedEnd < 5){
			reportedEnd++;
			LOGE("xrEndFrame failed: %d", (int)ended);
		}
	}

	// Sample after submission so these counters describe the latest app frame.
	// They are consumed by the next frame's debug overlay.
	if(g.performanceMetricsEnabled &&
	   g.queryPerformanceCounter != nullptr){
		auto queryFrameTime = [&](XrPath path, float *value,
		                          bool *valid){
			*valid = false;
			if(path == XR_NULL_PATH)
				return;
			XrPerformanceMetricsCounterMETA counter = {
				XR_TYPE_PERFORMANCE_METRICS_COUNTER_META };
			if(XR_FAILED(g.queryPerformanceCounter(
				g.session, path, &counter)))
				return;
			if(counter.counterUnit !=
			   XR_PERFORMANCE_METRICS_COUNTER_UNIT_MILLISECONDS_META)
				return;
			if((counter.counterFlags &
			    XR_PERFORMANCE_METRICS_COUNTER_FLOAT_VALUE_VALID_BIT_META)
			   != 0){
				*value = counter.floatValue;
				*valid = true;
			}else if((counter.counterFlags &
			          XR_PERFORMANCE_METRICS_COUNTER_UINT_VALUE_VALID_BIT_META)
			         != 0){
				*value = (float)counter.uintValue;
				*valid = true;
			}
		};
		queryFrameTime(g.appCpuFrameTimePath,
			&g.appCpuFrameTimeMs, &g.appCpuFrameTimeValid);
		queryFrameTime(g.appGpuFrameTimePath,
			&g.appGpuFrameTimeMs, &g.appGpuFrameTimeValid);
	}

#if MIAMIVR_STEAM_FRAME
	perfAddFrame(perfFrameStartNs, frameState.predictedDisplayTime,
	             frameState.predictedDisplayPeriod);
#endif
}

void
destroy(void)
{
	if(g.device != VK_NULL_HANDLE)
		vkDeviceWaitIdle(g.device);

	for(SwapchainImage &image : g.images){
		if(image.fence) vkDestroyFence(g.device, image.fence, nullptr);
		if(image.framebuffer) vkDestroyFramebuffer(g.device, image.framebuffer, nullptr);
		if(image.view) vkDestroyImageView(g.device, image.view, nullptr);
		if(image.uniformMapped)
			vkUnmapMemory(g.device, image.uniformMemory);
		if(image.uniformBuffer)
			vkDestroyBuffer(g.device, image.uniformBuffer, nullptr);
		if(image.uniformMemory)
			vkFreeMemory(g.device, image.uniformMemory, nullptr);
	}
	g.images.clear();

	if(g.vertexBuffer) vkDestroyBuffer(g.device, g.vertexBuffer, nullptr);
	if(g.vertexMemory) vkFreeMemory(g.device, g.vertexMemory, nullptr);
	if(g.indexBuffer) vkDestroyBuffer(g.device, g.indexBuffer, nullptr);
	if(g.indexMemory) vkFreeMemory(g.device, g.indexMemory, nullptr);
	if(g.depthView) vkDestroyImageView(g.device, g.depthView, nullptr);
	if(g.depthImage) vkDestroyImage(g.device, g.depthImage, nullptr);
	if(g.depthMemory) vkFreeMemory(g.device, g.depthMemory, nullptr);
	if(g.pipeline) vkDestroyPipeline(g.device, g.pipeline, nullptr);
	if(g.pipelineLayout) vkDestroyPipelineLayout(g.device, g.pipelineLayout, nullptr);
	if(g.descriptorPool) vkDestroyDescriptorPool(g.device, g.descriptorPool, nullptr);
	if(g.descriptorSetLayout)
		vkDestroyDescriptorSetLayout(g.device, g.descriptorSetLayout, nullptr);
	if(g.renderPass) vkDestroyRenderPass(g.device, g.renderPass, nullptr);
	if(g.commandPool) vkDestroyCommandPool(g.device, g.commandPool, nullptr);

	if(g.debugFence) vkDestroyFence(g.device, g.debugFence, nullptr);
	if(g.debugStagingMapped) vkUnmapMemory(g.device, g.debugStagingMemory);
	if(g.debugStaging) vkDestroyBuffer(g.device, g.debugStaging, nullptr);
	if(g.debugStagingMemory) vkFreeMemory(g.device, g.debugStagingMemory, nullptr);

#if MIAMIVR_STEAM_FRAME
	if(g.pointerFence) vkDestroyFence(g.device, g.pointerFence, nullptr);
	if(g.pointerStagingMapped) vkUnmapMemory(g.device, g.pointerStagingMemory);
	if(g.pointerStaging) vkDestroyBuffer(g.device, g.pointerStaging, nullptr);
	if(g.pointerStagingMemory) vkFreeMemory(g.device, g.pointerStagingMemory, nullptr);
	if(g.pointerSwapchain) xrDestroySwapchain(g.pointerSwapchain);
#endif
	if(g.debugSwapchain) xrDestroySwapchain(g.debugSwapchain);
	if(g.swapchain) xrDestroySwapchain(g.swapchain);
	for(int hand = 0; hand < 2; hand++){
		if(g.gripSpace[hand]) xrDestroySpace(g.gripSpace[hand]);
		if(g.aimSpace[hand]) xrDestroySpace(g.aimSpace[hand]);
	}
	if(g.viewSpace) xrDestroySpace(g.viewSpace);
	if(g.localSpace) xrDestroySpace(g.localSpace);
	if(g.space) xrDestroySpace(g.space);
	if(g.actionSet) xrDestroyActionSet(g.actionSet);
	if(g.session) xrDestroySession(g.session);
	if(g.device) vkDestroyDevice(g.device, nullptr);
	if(g.vkInstance) vkDestroyInstance(g.vkInstance, nullptr);
	if(g.instance) xrDestroyInstance(g.instance);

	g = State();
}

} // namespace xrvk
