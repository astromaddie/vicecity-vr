#include "platform_android.h"
#include "xr_vulkan_session.h"
#if MIAMIVR_STEAM_FRAME
#include "frame_diag.h"
#include "sfvr/sfvr.h"
#endif

#include <android_native_app_glue.h>
#include <android/log.h>
#include <math.h>
#include <stdio.h>
#include <time.h>

#include <atomic>
#include <thread>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

#ifndef MIAMIVR_BRINGUP
#include "common.h"
#include "android.h"
#include "Camera.h"
#include "CutsceneMgr.h"
#include "Frontend.h"
#include "Game.h"
#include "PlayerInfo.h"
#include "QuestProfiler.h"
#include "crossplatform.h"
#endif

namespace {

struct AppState
{
	std::atomic<bool> resumed{false};
	std::atomic<bool> destroyRequested{false};
	std::atomic<bool> stopRequested{false};
	std::atomic<bool> gameThreadFinished{false};
};

#ifndef MIAMIVR_BRINGUP
// A high-resolution OpenXR colour swapchain can succeed before librw allocates
// its two full-resolution scene/depth frame contexts. If that later Vulkan
// allocation fails, this process cannot safely resize the live swapchain and
// restart partially initialised RenderWare. Persist a conservative value so
// the user's next explicit launch is recoverable instead of boot-looping at
// the same oversized request.
void
autoResetRenderScaleAfterGameInitialiseFailure(void)
{
	const char *dataRoot = platform::gameDataRoot();
	if(dataRoot == nullptr || dataRoot[0] == '\0'){
		LOGE("render-scale recovery unavailable: game data root is empty");
		return;
	}
	char settingsPath[512];
	const int pathLength = snprintf(settingsPath, sizeof(settingsPath),
		"%s/vr_settings.ini", dataRoot);
	if(pathLength < 0 || pathLength >= (int)sizeof(settingsPath)){
		LOGE("render-scale recovery unavailable: settings path is too long");
		return;
	}

	const int persistedScale = (int)GetPrivateProfileIntA(
		"VR", "RenderScalePercent", 100, settingsPath);
	if(persistedScale <= 100)
		return;

	char failedRequest[16];
	snprintf(failedRequest, sizeof(failedRequest), "%d", persistedScale);
	char failureReason[16];
	snprintf(failureReason, sizeof(failureReason), "%d",
		xrvk::RENDER_SCALE_FALLBACK_GAME_RENDERER_ALLOCATION);
	const bool historyWritten =
		WritePrivateProfileStringA("VR", "RenderScaleLastFallbackRequest",
			failedRequest, settingsPath) &&
		WritePrivateProfileStringA("VR", "RenderScaleLastFallbackPercent",
			"100", settingsPath) &&
		WritePrivateProfileStringA("VR", "RenderScaleLastFallbackReason",
			failureReason, settingsPath);
	if(WritePrivateProfileStringA(
	     "VR", "RenderScalePercent", "100", settingsPath)){
		LOGE("game initialisation failed with persisted render scale %d%%; "
		     "auto-reset to 100%% for next launch (%s, history %s)",
		     persistedScale, settingsPath,
		     historyWritten ? "saved" : "FAILED");
	}else{
		LOGE("game initialisation failed with persisted render scale %d%%; "
		     "AUTO-RESET TO 100%% FAILED (%s)",
		     persistedScale, settingsPath);
	}
}

// The headset swapchain is portrait per eye, but theatre content is shown on
// a 16:9 physical quad. Build a conventional mono camera for that physical
// aspect so the picture does not inherit head pose, eye separation or lens
// asymmetry. Stretching the portrait storage onto the matching 16:9 quad is
// intentional: projection and display aspect cancel, leaving normal desktop
// proportions without another full-size render target.
void
makeTheaterMatrices(float viewProj[16], float im2dWorld[16],
                    float *distanceOut, float im2dDistance = 2.0f)
{
	for(int i = 0; i < 16; i++){
		viewProj[i] = 0.0f;
		im2dWorld[i] = 0.0f;
	}

	const float aspect = androidgame::VrTheaterAspectRatio();
	const float nearZ = 0.05f;
	const float farZ = 1000.0f;
	const float tanHalfY = tanf(60.0f*0.5f*3.1415926535f/180.0f);
	viewProj[0] = 1.0f/(tanHalfY*aspect);
	// Vulkan's framebuffer Y axis points down.
	viewProj[5] = -1.0f/tanHalfY;
	viewProj[10] = -farZ/(farZ-nearZ);
	viewProj[11] = -1.0f;
	viewProj[14] = -(farZ*nearZ)/(farZ-nearZ);

	// The 2D layer's plane in camera space. Flat cinema puts it at 2 m; the
	// 3D cinema puts it on the zero-parallax plane, so subtitles and the
	// letterbox sit exactly on the screen.
	const float distance = im2dDistance;
	const float planeHeight = 2.0f*distance*tanHalfY;
	const float planeWidth = planeHeight*aspect;
	const float screenWidth = RsGlobal.width > 0 ? (float)RsGlobal.width : 1.0f;
	const float screenHeight = RsGlobal.height > 0 ? (float)RsGlobal.height : 1.0f;
	im2dWorld[0] = planeWidth/screenWidth;
	im2dWorld[5] = -planeHeight/screenHeight;
	im2dWorld[10] = 1.0f;
	im2dWorld[12] = -planeWidth*0.5f;
	im2dWorld[13] = planeHeight*0.5f;
	im2dWorld[14] = -distance;
	im2dWorld[15] = 1.0f;
	*distanceOut = distance;
}

#if MIAMIVR_STEAM_FRAME
// 3D cinema. The director's camera is split into two parallel cameras, each
// with an off-axis frustum, converged on a plane kCinemaConvergence metres in
// front of the camera. That plane lands exactly on the theater screen; nearer
// things come out of it a little, everything further sits behind it, like a
// window. The camera separation is not the viewer's IPD: it is solved so that
// the two images of something at infinity are depth*IPD apart on the screen,
// which puts the far background at screen distance / (1 - depth) and never
// asks the eyes to diverge (depth < 1). IPD comes from the runtime's eyes.
static const float kCinemaConvergence = 1.5f;

static void
makeCinema3DMatrices(float viewProj[2][16], float im2dWorld[16],
                     float *distanceOut, float depth, const float eyePos[2][3])
{
	float mono[16];
	makeTheaterMatrices(mono, im2dWorld, distanceOut, kCinemaConvergence);

	float ipd = 0.0f;
	for(int axis = 0; axis < 3; axis++){
		const float d = eyePos[1][axis] - eyePos[0][axis];
		ipd += d*d;
	}
	ipd = sqrtf(ipd);
	if(!(ipd > 0.045f && ipd < 0.085f))
		ipd = 0.063f;    // no plausible pose (standby, synthetic head)

	// The screen shows NDC x in [-1, 1] across its width, so an NDC disparity
	// of delta is delta*width/2 metres there. At infinity the two images are
	// sx*baseline/C apart in NDC.
	const float sx = mono[0];
	const float C = kCinemaConvergence;
	const float baseline =
		depth*ipd*2.0f*C/(sx*xrvk::kTheaterScreenWidth);

	for(int eye = 0; eye < 2; eye++){
		const float eyeX = (eye == 0 ? -0.5f : 0.5f)*baseline;
		// x_ndc = sx*(x - eyeX)/(-z) + shift, with shift = sx*eyeX/C so the
		// two eyes agree on the plane z = -C. In clip space, with w = -z:
		// x_clip = sx*x - sx*eyeX - shift*z.
		const float shift = sx*eyeX/C;
		memcpy(viewProj[eye], mono, sizeof(mono));
		viewProj[eye][8] = -shift;
		viewProj[eye][12] = -sx*eyeX;
	}

	static float reportedDepth = -1.0f;
	if(depth != reportedDepth){
		reportedDepth = depth;
		LOGI("[vr] cinema 3D: depth %.2f, ipd %.1f mm, camera baseline %.1f mm, "
		     "zero parallax at %.2f m", depth, ipd*1000.0f, baseline*1000.0f, C);
	}
}
#endif

struct MenuTransitionTraceState
{
	bool initialized;
	uint64 frame;
	int theaterBefore;
	int theaterAfter;
	int gameState;
	int gameNotLoaded;
	int menuActive;
	int wantRestart;
	int wantLoad;
	int playingIntro;
	int cutsceneProcessing;
	int cutsceneRunning;
	int widescreen;
	int playerPresent;
	int currentScreen;
	uint32 colourMode;
};

MenuTransitionTraceState gMenuTransitionTrace = {};

int64_t
monotonicNowNs(void)
{
	timespec now = {};
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec*1000000000LL + now.tv_nsec;
}

// The startup artifact is only visible for a fraction of a second, so log the
// first few seconds and every later state transition to a small persistent
// file.  This is intentionally outside logcat: the user can reproduce once,
// quit, and we can pull the exact transition without keeping adb attached.
void
traceMenuTransition(bool theaterBefore, bool theaterAfter,
	bool theaterSubmitted, uint32 colourMode,
	int64_t frameStartNs, int64_t beginStartNs, int64_t beginEndNs,
	int64_t stepEndNs, int64_t endFrameEndNs)
{
	MenuTransitionTraceState current = {};
	current.initialized = true;
	current.frame = gMenuTransitionTrace.frame;
	current.theaterBefore = theaterBefore;
	current.theaterAfter = theaterAfter;
	current.gameState = (int)gGameState;
	current.gameNotLoaded = FrontEndMenuManager.m_bGameNotLoaded;
	current.menuActive = FrontEndMenuManager.m_bMenuActive;
	current.wantRestart = FrontEndMenuManager.m_bWantToRestart;
	current.wantLoad = FrontEndMenuManager.m_bWantToLoad;
	current.playingIntro = CGame::playingIntro;
	current.cutsceneProcessing = CCutsceneMgr::IsCutsceneProcessing();
	current.cutsceneRunning = CCutsceneMgr::IsRunning();
	current.widescreen = TheCamera.m_WideScreenOn;
	current.playerPresent = FindPlayerPed() != nil;
	current.currentScreen = FrontEndMenuManager.m_nCurrScreen;
	current.colourMode = colourMode;

	const MenuTransitionTraceState &previous = gMenuTransitionTrace;
	const bool changed = !previous.initialized ||
		current.theaterBefore != previous.theaterBefore ||
		current.theaterAfter != previous.theaterAfter ||
		current.gameState != previous.gameState ||
		current.gameNotLoaded != previous.gameNotLoaded ||
		current.menuActive != previous.menuActive ||
		current.wantRestart != previous.wantRestart ||
		current.wantLoad != previous.wantLoad ||
		current.playingIntro != previous.playingIntro ||
		current.cutsceneProcessing != previous.cutsceneProcessing ||
		current.cutsceneRunning != previous.cutsceneRunning ||
		current.widescreen != previous.widescreen ||
		current.playerPresent != previous.playerPresent ||
		current.currentScreen != previous.currentScreen ||
		current.colourMode != previous.colourMode;
	const bool earlyFrame = current.frame < 288; // Four seconds at 72 Hz.

	if(earlyFrame || changed){
		static FILE *traceFile = nullptr;
		if(traceFile == nullptr){
			const char *root = platform::storageRoot();
			if(root != nullptr && root[0] != '\0'){
				char path[512];
				const int pathLength = snprintf(path, sizeof(path),
					"%s/menu_transition.log", root);
				if(pathLength > 0 && pathLength < (int)sizeof(path))
					traceFile = fopen(path, "w");
			}
			if(traceFile != nullptr){
				xrvk::RenderScaleStatus scale = {};
				uint32 temporalMode = 0, sceneWidth = 0, sceneHeight = 0;
				uint32 outputWidth = 0, outputHeight = 0;
				const bool scaleValid = xrvk::getRenderScaleStatus(&scale);
				const bool temporalValid = rw::vulkan::getSgsrStatus(
					&temporalMode, &sceneWidth, &sceneHeight,
					&outputWidth, &outputHeight);
				fprintf(traceFile,
					"# scale valid=%d request=%d selected=%d effective=%.2f "
					"base=%ux%u actual=%ux%u fallback=%d\n",
					scaleValid, scale.requestedPercent,
					scale.selectedPresetPercent, scale.effectivePercent,
					scale.recommendedWidth, scale.recommendedHeight,
					scale.actualWidth, scale.actualHeight,
					scale.fallbackReason);
				fprintf(traceFile,
					"# temporal valid=%d mode=%u scene=%ux%u output=%ux%u\n",
					temporalValid, temporalMode, sceneWidth, sceneHeight,
					outputWidth, outputHeight);
				fprintf(traceFile,
					"frame,time_ns,theater_before,theater_after,theater_submitted,"
					"game_state,game_not_loaded,menu_active,want_restart,want_load,"
					"playing_intro,cutscene_processing,cutscene_running,widescreen,"
					"player_present,screen,colour_mode,cpu_setup_ms,cpu_begin_ms,"
					"cpu_step_ms,cpu_end_ms,cpu_total_ms\n");
			}
		}
		if(traceFile != nullptr){
			fprintf(traceFile,
				"%llu,%lld,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%u,"
				"%.3f,%.3f,%.3f,%.3f,%.3f\n",
				(unsigned long long)current.frame,
				(long long)xrvk::getPredictedDisplayTimeNs(),
				current.theaterBefore, current.theaterAfter,
				theaterSubmitted, current.gameState,
				current.gameNotLoaded, current.menuActive,
				current.wantRestart, current.wantLoad,
				current.playingIntro, current.cutsceneProcessing,
				current.cutsceneRunning, current.widescreen,
				current.playerPresent, current.currentScreen,
				current.colourMode,
				(double)(beginStartNs-frameStartNs)/1000000.0,
				(double)(beginEndNs-beginStartNs)/1000000.0,
				(double)(stepEndNs-beginEndNs)/1000000.0,
				(double)(endFrameEndNs-stepEndNs)/1000000.0,
				(double)(endFrameEndNs-frameStartNs)/1000000.0);
			fflush(traceFile);
		}
	}

	current.frame++;
	gMenuTransitionTrace = current;
}

// Installed as the OpenXR layer's frame renderer once the game is up. Opens
// librw's multiview render pass against the swapchain image the runtime just

// ---------------------------------------------------------------------------
// Wrist minimap
// ---------------------------------------------------------------------------
// The 2D interface reaches the eyes through one matrix that maps screen pixels
// onto a plane in the world (see rw_im2d.vert). Swapping that matrix for the
// duration of the radar draw therefore moves the whole minimap -- disc, map
// tiles and blips alike -- onto a small panel carried by the left controller,
// without touching a single drawing call.
static float gHudIm2D[16];
static float gHudIm2DDistance = 2.0f;
static float gHudIm2DEye[3];
static bool gHudIm2DValid;

static void
StoreHudIm2D(const float transform[16], float distance, const float eye[3])
{
	memcpy(gHudIm2D, transform, sizeof(gHudIm2D));
	gHudIm2DDistance = distance;
	memcpy(gHudIm2DEye, eye, sizeof(gHudIm2DEye));
	gHudIm2DValid = true;
}

#if MIAMIVR_STEAM_FRAME
// The Steam Frame system layer (standard.md section 1), on sfvr's View timing:
// a short View press is "back", holding it 1 s recentres and 3 s recentres
// again (this port has no standing-height calibration), each with the system
// buzz on both hands. A system recentre from the runtime does the same.
// Returns true for one frame on a short press.
static bool
updateFrameSystemLayer(const xrvk::ControllerInput &controllers)
{
	static sfvr_view_button view;
	static sfvr_view_config config;
	static bool initialised;
	if(!initialised){
		sfvr_view_config_default(&config);
		sfvr_view_reset(&view);
		initialised = true;
	}
	// Button timing on the predicted display clock, like everything else.
	const double now = (double)xrvk::getPredictedDisplayTimeNs()*1e-9;
	const unsigned events = sfvr_view_update(&view, &config,
		controllers.view ? 1 : 0, now);
	const bool recentre = controllers.systemRecentre ||
		(events & (SFVR_VIEW_RECENTRE | SFVR_VIEW_RECALIBRATE_HEIGHT)) != 0;
	if(recentre){
		LOGI("[vr] recentre (%s)", controllers.systemRecentre ? "system" :
		     (events & SFVR_VIEW_RECALIBRATE_HEIGHT) ? "View hold 3 s" :
		     "View hold 1 s");
		androidgame::VrRecenterView();
		const sfvr_haptic_pulse buzz = sfvr_haptic_default(SFVR_HAPTIC_SYSTEM);
		for(int hand = 0; hand < 2; hand++)
			xrvk::triggerHaptic(hand, buzz.amplitude, 0.0f,
			                    buzz.seconds*1000.0f);
	}
	return (events & SFVR_VIEW_SHORT_PRESS) != 0;
}
#endif

// handed over, steps the game inside it, and submits.
void
renderGameFrame(VkImage image, VkImageView view, const float viewProj[2][16],
                const float im2dWorld[16], float im2dDistance,
                const float headPos[3], float headYaw,
                const float headQuat[4], float eyeFovDeg,
                const float eyePos[2][3], const float eyeQuat[2][4])
{
	const int64_t frameStartNs = monotonicNowNs();
	androidgame::QuestProfilerBeginAppFrame();
	xrvk::ControllerInput controllers;
	xrvk::getInput(&controllers);
	androidgame::PadInput pad;
	pad.leftStickX = controllers.leftStickX;
	pad.leftStickY = controllers.leftStickY;
	pad.rightStickX = controllers.rightStickX;
	pad.rightStickY = controllers.rightStickY;
	pad.leftTrigger = controllers.leftTrigger;
	pad.rightTrigger = controllers.rightTrigger;
	pad.leftGrip = controllers.leftGrip;
	pad.rightGrip = controllers.rightGrip;
	pad.a = controllers.a;
	pad.b = controllers.b;
	pad.x = controllers.x;
	pad.y = controllers.y;
	pad.menu = controllers.menu;
	pad.leftStickClick = controllers.leftStickClick;
	pad.rightStickClick = controllers.rightStickClick;
#if MIAMIVR_STEAM_FRAME
	pad.dpadUp = controllers.dpadUp;
	pad.dpadDown = controllers.dpadDown;
	pad.dpadLeft = controllers.dpadLeft;
	pad.dpadRight = controllers.dpadRight;
	pad.viewBack = updateFrameSystemLayer(controllers);
#endif
	for(int hand = 0; hand < 2; hand++){
		for(int axis = 0; axis < 3; axis++){
			pad.gripPose[hand].position[axis] =
				controllers.gripPose[hand].position[axis];
			pad.aimPose[hand].position[axis] =
				controllers.aimPose[hand].position[axis];
		}
		for(int axis = 0; axis < 4; axis++){
			pad.gripPose[hand].orientation[axis] =
				controllers.gripPose[hand].orientation[axis];
			pad.aimPose[hand].orientation[axis] =
				controllers.aimPose[hand].orientation[axis];
		}
		pad.gripPose[hand].valid = controllers.gripPose[hand].valid;
		pad.aimPose[hand].valid = controllers.aimPose[hand].valid;
	}
	androidgame::SetPadInput(pad);

	// Debug overlay: the game side owns the chord and the pixels (its port of
	// the desktop panel); the session just carries them to a quad layer.
	androidgame::VrDebugUpdate(pad);
	{
		int width = 0, height = 0;
		const unsigned char *pixels =
			androidgame::VrDebugPixels(&width, &height);
		xrvk::setDebugOverlay(pixels, width, height);
	}

	const bool theaterBefore = androidgame::VrShouldUseTheaterMode();
	xrvk::setTheaterMode(theaterBefore);
	if(theaterBefore){
		float theaterViewProj[16], theaterIm2D[16], theaterDistance;
		const float origin[3] = { 0.0f, 0.0f, 0.0f };
		const float identityQuat[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
#if MIAMIVR_STEAM_FRAME
		const float depth = androidgame::VrCinema3DDepthThisFrame();
		xrvk::setTheaterStereo(depth > 0.0f);
		if(depth > 0.0f){
			float eyeViewProj[2][16];
			makeCinema3DMatrices(eyeViewProj, theaterIm2D, &theaterDistance,
			                     depth, eyePos);
			rw::vulkan::setStereoViewProjection(eyeViewProj[0],
			                                    eyeViewProj[1]);
		}else{
			makeTheaterMatrices(theaterViewProj, theaterIm2D,
			                    &theaterDistance);
			rw::vulkan::setStereoViewProjection(
				theaterViewProj, theaterViewProj);
		}
#else
		makeTheaterMatrices(theaterViewProj, theaterIm2D, &theaterDistance);
		rw::vulkan::setStereoViewProjection(
			theaterViewProj, theaterViewProj);
#endif
		rw::vulkan::setIm2DTransform(
			theaterIm2D, theaterDistance, origin);
		rw::vulkan::setHeadPose(origin, 0.0f, identityQuat);
		rw::vulkan::clearFirstPersonEyePoses();
	}else{
		rw::vulkan::setStereoViewProjection(viewProj[0], viewProj[1]);
		rw::vulkan::setSgsrHorizontalFovDegrees(eyeFovDeg);
		rw::vulkan::setIm2DTransform(im2dWorld, im2dDistance, headPos);
		StoreHudIm2D(im2dWorld, im2dDistance, headPos);
		rw::vulkan::setHeadPose(headPos, headYaw, headQuat);
		rw::vulkan::setFirstPersonEyePoses(eyePos, eyeQuat);
	}
	androidgame::SetEyeFovDeg(eyeFovDeg);
	// Game time follows the display clock, not the wall clock; see
	// getPredictedDisplayTimeNs.
	androidgame::SetFrameTimeNs(xrvk::getPredictedDisplayTimeNs());
	// The previous Step's completed light list, before beginFrame writes the
	// scene block. This Step's own lights land a frame later, which no light
	// pool is sharp enough to show.
	androidgame::VrPushDynamicLights();
	platform::setCheckpoint("vk/beginFrame");
	androidgame::QuestProfilerBeginVkBegin();
	const int64_t beginStartNs = monotonicNowNs();
	if(!rw::vulkan::beginFrame(image, view)){
		androidgame::QuestProfilerCancelAppFrame();
		return;
	}
	const int64_t beginEndNs = monotonicNowNs();
	androidgame::QuestProfilerEndVkBegin();
	platform::setCheckpoint("game/Step");
	androidgame::QuestProfilerBeginStep();
	androidgame::Step();
	const int64_t stepEndNs = monotonicNowNs();
	androidgame::QuestProfilerEndStep();

	// Step owns both simulation and rendering, so a cutscene/menu transition
	// can occur inside it. Submit this frame with the same mode and matrices it
	// started with; switching the compositor layer to theaterAfter here used to
	// present an immersive-rendered image as a flat quad for one frame.
	const bool theaterAfter = androidgame::VrShouldUseTheaterMode();
	const bool theaterFrame = theaterBefore;

	// A cutscene can end inside Step.  That transition frame was prepared
	// above with the theater identity head pose, so first-person gameplay may
	// have latched yaw=0 before the real headset pose is restored.  Drop that
	// provisional anchor now; the next immersive frame will relatch against
	// the actual HMD yaw instead of leaving movement/view rotated by the
	// player's physical facing at the end of the cutscene.
	if(theaterBefore && !theaterAfter)
		rw::vulkan::setFirstPersonAnchor(nullptr, 0.0f, 0, 0);

	// Match the desktop VR renderer: Vice City's colour filter is a final-image
	// operation, after the world and HUD have been assembled. The Vulkan
	// backend applies it to both multiview layers in its resolve subpass.
	// EXTENDED_COLOURFILTER intentionally remains disabled for the old
	// camera-raster implementation on Vulkan. This final-image implementation
	// is independent of it and reproduces the default POSTFX_NORMAL path
	// directly, using the blur colour populated by Vice City's timecycle.
	const bool cutsceneFrame =
		CGame::playingIntro ||
		CCutsceneMgr::IsCutsceneProcessing() ||
		CCutsceneMgr::IsRunning() ||
		TheCamera.m_WideScreenOn;
	const uint32 colourMode =
		(androidgame::VrViceCityColorEnabled() &&
		 (!theaterFrame || cutsceneFrame) &&
		 TheCamera.m_BlurType != MOTION_BLUR_NONE ? 1u : 0u);
	rw::vulkan::setPostFx(colourMode,
		(uint32)TheCamera.m_BlurRed,
		(uint32)TheCamera.m_BlurGreen,
		(uint32)TheCamera.m_BlurBlue,
		1.0f);
	rw::vulkan::setSpatialAaMode((RwUInt32)androidgame::VrSpatialAaMode());
	rw::vulkan::setGenerateMipmaps(androidgame::VrGenerateMipmaps());
	rw::vulkan::setPs2AlphaTestEnabled(androidgame::VrPs2AlphaTest());
	rw::vulkan::setRenderDiagnostics(androidgame::VrRenderDiagnostics());
	rw::vulkan::setMaskedMipBias(
		(rw::uint32)androidgame::VrFoliageSoftness());
	static uint32 lastColourMode = ~0u;
	if(colourMode != lastColourMode){
		__android_log_print(ANDROID_LOG_INFO, "MiamiVR",
			"Vice City postfx %s (type=%d rgb=%d,%d,%d)",
			colourMode == 2u ? "STARTUP BLACK" :
				(colourMode == 1u ? "COLOUR ON" : "COLOUR OFF"),
			TheCamera.m_BlurType,
			TheCamera.m_BlurRed, TheCamera.m_BlurGreen,
			TheCamera.m_BlurBlue);
		lastColourMode = colourMode;
	}
	platform::setCheckpoint("vk/endFrame");
	androidgame::QuestProfilerBeginVkEnd();
	rw::vulkan::endFrame();
	const int64_t endFrameEndNs = monotonicNowNs();
	androidgame::QuestProfilerEndVkEnd();
	androidgame::QuestProfilerEndAppFrame();
	traceMenuTransition(theaterBefore, theaterAfter, theaterFrame, colourMode,
		frameStartNs, beginStartNs, beginEndNs, stepEndNs, endFrameEndNs);
	platform::setCheckpoint("frame/done");
}
#endif

void
handleAppCmd(android_app *app, int32_t cmd)
{
	AppState *state = (AppState *)app->userData;
	switch(cmd){
	case APP_CMD_RESUME:
		state->resumed = true;
		LOGI("activity lifecycle: RESUME");
		break;
	case APP_CMD_PAUSE:
	case APP_CMD_STOP:
		state->resumed = false;
		LOGI("activity lifecycle: %s",
		     cmd == APP_CMD_PAUSE ? "PAUSE" : "STOP");
		break;
	case APP_CMD_DESTROY:
		state->destroyRequested = true;
		LOGI("activity lifecycle: DESTROY");
		break;
	default:
		break;
	}
}

// The OpenXR session, the game and all rendering live here, off the thread that
// android_main runs on.
//
// This is not an optimisation. android_main's thread is what services the
// native-activity looper: lifecycle commands and the input queue. Loading a
// save or starting a new game keeps CGame inside a single Step() for tens of
// seconds, and while that ran on this thread the looper went unserviced, the UI
// thread blocked on a synchronous command, and Horizon OS raised an ANR --
// whose dialog then sat in the headset blocking further launches.
void
gameThreadMain(android_app *app, AppState *state)
{
	// The OpenXR loader and the runtime both call into Java from whichever
	// thread uses them, so this one has to be known to the VM.
	JNIEnv *env = nullptr;
	app->activity->vm->AttachCurrentThread(&env, nullptr);

	bool sessionCreated = false;
	bool gameStarted = false;
	int createAttempts = 0;

	while(!state->stopRequested.load()){
		if(!sessionCreated){
			// Creation used to wait for APP_CMD_RESUME. It does not need to:
			// the session simply sits in IDLE until the runtime is ready, and
			// waiting meant that whenever Horizon OS chose not to resume the
			// activity the app came up completely inert, which is impossible
			// to tell apart from a hang. Try, and retry on failure.
			if(!xrvk::create(app)){
				// create() can fail after allocating only part of the OpenXR
				// or Vulkan state. A retry must start from an empty State;
				// otherwise the second attempt reuses stale handles.
				xrvk::destroy();
				if(++createAttempts >= 50){
					LOGE("giving up on the OpenXR/Vulkan session");
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
				continue;
			}
			sessionCreated = true;
			createAttempts = 0;
		}

		if(!xrvk::pollEvents())
			break;

#ifndef MIAMIVR_BRINGUP
		// Engine setup runs as soon as the session exists rather than waiting
		// for the first frame: rsINITIALIZE and rsRWINITIALIZE only adopt the
		// Vulkan device and build pipelines, they draw nothing.
		if(!gameStarted){
			xrvk::GraphicsContext graphics;
			if(!xrvk::getContext(&graphics)){
				LOGE("no Vulkan context to start the game with");
				break;
			}
			androidgame::VulkanContext context;
			context.instance = graphics.instance;
			context.physicalDevice = graphics.physicalDevice;
			context.device = graphics.device;
			context.fragmentStoresAndAtomicsEnabled =
				graphics.fragmentStoresAndAtomicsEnabled;
			context.queue = graphics.queue;
			context.queueFamilyIndex = graphics.queueFamilyIndex;
			context.width = graphics.width;
			context.height = graphics.height;
			context.renderScaleEffectivePercent =
				graphics.renderScaleEffectivePercent;
			context.viewCount = graphics.viewCount;
			context.colourFormat = graphics.colourFormat;

			bool renderTargetStartupFailure = false;
			if(!androidgame::Initialise(
			     context, &renderTargetStartupFailure)){
				if(renderTargetStartupFailure)
					autoResetRenderScaleAfterGameInitialiseFailure();
				LOGE("game initialisation failed");
				break;
			}
			// Finish the expensive one-time game/frontend setup before an
			// OpenXR frame can be begun.  Running this from Step held the first
			// submitted frame open for hundreds of milliseconds and produced a
			// repeatable compositor/reprojection flash in the main menu.
			if(!androidgame::PrepareFrontendBeforeFrames()){
				LOGE("pre-frame frontend preparation failed");
				androidgame::Shutdown();
				break;
			}
			xrvk::confirmRenderScaleRendererReady();
			xrvk::setFrameRenderer(renderGameFrame);
			gameStarted = true;
		}
		if(gameStarted && androidgame::WantsToQuit())
			break;
#endif

		// The OpenXR runtime owns its own lifecycle, but Android can pause the
		// NativeActivity a little before the STOPPING event reaches us. Do not
		// enter another xrWaitFrame or advance the game during that gap. Event
		// polling above remains active so STOPPING/READY are still consumed and
		// the same session can resume normally from the system UI.
		const bool resumed = state->resumed.load();
		if(resumed && xrvk::shouldRender()){
			// Steps the game inside the frame, through the renderer callback.
			platform::setCheckpoint("xr/renderFrame");
			xrvk::renderFrame();
		}else{
#ifndef MIAMIVR_BRINGUP
			// No frame to draw into, but the game should still advance: a load
			// takes tens of seconds and there is no reason to stall it until
			// the headset is picked up. Every draw path checks for an open
			// frame and does nothing, so this is logic only.
			if(resumed && gameStarted)
				androidgame::Step();
#endif
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	}

#ifndef MIAMIVR_BRINGUP
	if(gameStarted){
		xrvk::setFrameRenderer(nullptr);
		androidgame::Shutdown();
	}
#endif
	if(sessionCreated)
		xrvk::destroy();

	app->activity->vm->DetachCurrentThread();
	LOGI("game thread finished");
	state->gameThreadFinished.store(true);
	// android_main normally sleeps indefinitely in ALooper_pollOnce. Wake it
	// when OpenXR asks the game thread to exit; otherwise the Activity and
	// process stay alive without a renderer and the next launch resumes that
	// dead instance.
	ALooper_wake(app->looper);
}

} // namespace

// Defined by the HUD. Each draws one panel's contents in screen coordinates,
// and each is called back here with that panel's texture open rather than from
// CHud::Draw.
void VrDrawWristRadarContents(void);
void VrDrawWristStatusContents(void);
void VrDrawWristClockContents(void);
void VrDrawWristAmmoContents(void);

namespace androidgame {

// Screen rect each panel occupies, as the HUD lays it out. The panels are
// rendered at the top of the *next* frame, before any game code runs, so the
// rects have to be remembered rather than passed down.
static float gWristPanelCentre[rw::vulkan::WRIST_PANEL_COUNT][2];
static float gWristPanelExtent[rw::vulkan::WRIST_PANEL_COUNT][2];
// How wide each panel ends up on the arm. Height follows from the rect, so
// nothing is stretched.
static const float kWristPanelMetres[rw::vulkan::WRIST_PANEL_COUNT] = {
	0.115f, 0.130f, 0.055f,
	// The ammo counter is read mid-fight, so it is wider than the watch.
	0.070f
};
// Where they sit relative to the grip pose: back along the arm from the hand,
// and off the wrist by roughly the thickness of one.
static const float kWristPanelBack = 0.07f;
static const float kWristPanelLift = 0.035f;

// Draws one panel into its texture. Called by the Vulkan backend from inside
// beginFrame, with that target open and nothing else drawn yet.
static void
RenderWristPanel(int panel)
{
	if(!gHudIm2DValid || gWristPanelExtent[panel][0] < 1.0f ||
	   gWristPanelExtent[panel][1] < 1.0f)
		return;

	// Screen pixels straight to the little target's clip space. The interface
	// draws in screen coordinates exactly as it always has; this is what lands
	// it in the texture instead of on the plane in front of the face.
	float projection[16] = {};
	projection[0] = 2.0f/gWristPanelExtent[panel][0];
	projection[5] = 2.0f/gWristPanelExtent[panel][1];
	projection[10] = 1.0f;
	projection[12] = -2.0f*gWristPanelCentre[panel][0]/
		gWristPanelExtent[panel][0];
	projection[13] = -2.0f*gWristPanelCentre[panel][1]/
		gWristPanelExtent[panel][1];
	projection[15] = 1.0f;
	rw::vulkan::setIm2DTransform(projection, gHudIm2DDistance, gHudIm2DEye);
	if(panel == rw::vulkan::WRIST_PANEL_STATUS)
		VrDrawWristStatusContents();
	else if(panel == rw::vulkan::WRIST_PANEL_CLOCK)
		VrDrawWristClockContents();
	else if(panel == rw::vulkan::WRIST_PANEL_AMMO)
		VrDrawWristAmmoContents();
	else
		VrDrawWristRadarContents();
	rw::vulkan::setIm2DTransform(gHudIm2D, gHudIm2DDistance, gHudIm2DEye);
}

// The backend takes plain function pointers, one per panel.
static void
RenderWristPanelMap(void)
{
	RenderWristPanel(rw::vulkan::WRIST_PANEL_MAP);
}

static void
RenderWristPanelStatus(void)
{
	RenderWristPanel(rw::vulkan::WRIST_PANEL_STATUS);
}

static void
RenderWristPanelClock(void)
{
	RenderWristPanel(rw::vulkan::WRIST_PANEL_CLOCK);
}

static void
RenderWristPanelAmmo(void)
{
	RenderWristPanel(rw::vulkan::WRIST_PANEL_AMMO);
}

// Rotates an orthonormal pair in its own plane. Used to swing a panel's frame
// around each of its own axes in turn.
static void
RotateWristAxes(float *first, float *second, float radians)
{
	const float c = cosf(radians), s = sinf(radians);
	for(int axis = 0; axis < 3; axis++){
		const float a = first[axis], b = second[axis];
		first[axis] = a*c+b*s;
		second[axis] = b*c-a*s;
	}
}

// The plane a wrist quad lives on: screen pixels to metres, centred on the
// panel's rect and lying on the wrist, plus whatever the player calibrated.
static bool
BuildWristPanelPlane(int panel, int hand, float plane[16], float centreX,
                     float centreY, float widthPixels)
{
	if(widthPixels < 1.0f)
		return false;
	float alongCm = 0.0f, acrossCm = 0.0f, liftCm = 0.0f;
	float pitchDeg = 0.0f, yawDeg = 0.0f, rollDeg = 0.0f, sizeScale = 1.0f;
	VrGetWristPanelCalibration(panel, &alongCm, &acrossCm, &liftCm,
		&pitchDeg, &yawDeg, &rollDeg, &sizeScale);

	// Driving: the panel is dashboard instrumentation on the vehicle's own
	// control centre, facing the driver. It is not worn, so none of the wrist
	// geometry below applies -- no hand mirroring, no arm offsets and no panel
	// rotations, only the three offsets from that centre.
	{
		float anchorPos[3], anchorRight[3], anchorUp[3], anchorForward[3];
		if(VrGetWristVehicleAnchorPose(anchorPos, anchorRight, anchorUp,
		     anchorForward)){
			float centre[3], normal[3], up[3], right[3];
			for(int axis = 0; axis < 3; axis++){
				right[axis] = anchorRight[axis];
				up[axis] = anchorUp[axis];
				normal[axis] = -anchorForward[axis];
				centre[axis] = anchorPos[axis]+
					anchorForward[axis]*(alongCm*0.01f)+
					right[axis]*(acrossCm*0.01f)+
					up[axis]*(liftCm*0.01f);
			}
			const float scale =
				kWristPanelMetres[panel]*sizeScale/widthPixels;
			memset(plane, 0, sizeof(float)*16);
			for(int axis = 0; axis < 3; axis++){
				plane[axis] = right[axis]*scale;
				plane[4+axis] = -up[axis]*scale;
				plane[8+axis] = normal[axis];
				plane[12+axis] = centre[axis]-
					right[axis]*(centreX*scale)+
					up[axis]*(centreY*scale);
			}
			plane[15] = 1.0f;
			return true;
		}
	}

	// Controller axes: the OpenXR grip pose looks along -Z out of the hand, so
	// +Z runs back towards the wrist and +Y stands away from the palm.
	//
	// Only the left hand is calibrated. OpenXR defines the grip X axis as the
	// palm normal and mirrors it between hands, which mirrors the Y axis built
	// from it as well, so the right hand is the left one with both of those
	// flipped: two flips keep the frame right-handed -- the panel is not drawn
	// backwards -- while putting it on the outside of that arm instead of
	// inside it.
	const float handSign = hand ? -1.0f : 1.0f;
	float origin[3], side[3], palmUp[3], backward[3];
	// The panel is worn on the arm, so it follows the hand the player can see:
	// a hand moved onto a weapon socket or a steering handle takes it along.
	if(!VrGetWristAnchorPose(hand, origin, side, palmUp, backward)){
		xrvk::ControllerInput input;
		xrvk::getInput(&input);
		const xrvk::TrackedPose &grip = input.gripPose[hand];
		if(!grip.valid)
			return false;
		const float *q = grip.orientation;
		origin[0] = grip.position[0];
		origin[1] = grip.position[1];
		origin[2] = grip.position[2];
		side[0] = 1.0f-2.0f*(q[1]*q[1]+q[2]*q[2]);
		side[1] = 2.0f*(q[0]*q[1]+q[2]*q[3]);
		side[2] = 2.0f*(q[0]*q[2]-q[1]*q[3]);
		palmUp[0] = 2.0f*(q[0]*q[1]-q[2]*q[3]);
		palmUp[1] = 1.0f-2.0f*(q[0]*q[0]+q[2]*q[2]);
		palmUp[2] = 2.0f*(q[1]*q[2]+q[0]*q[3]);
		backward[0] = 2.0f*(q[0]*q[2]+q[1]*q[3]);
		backward[1] = 2.0f*(q[1]*q[2]-q[0]*q[3]);
		backward[2] = 1.0f-2.0f*(q[0]*q[0]+q[1]*q[1]);
	}
	for(int axis = 0; axis < 3; axis++){
		side[axis] *= handSign;
		palmUp[axis] *= handSign;
	}

	// The face lies on the wrist looking away from the arm, twelve o'clock
	// towards the fingers, and is not turned towards the head: it is an object
	// on the arm and reads when the arm is turned to look at it. Normal, up and
	// right form a right-handed frame so nothing is mirrored -- with
	// up = -backward and normal = +/-palmUp, the third axis works out as the
	// controller's own side axis.
	const float faceSign = VrWristPanelUnderside(panel) ? -1.0f : 1.0f;
	float centre[3], normal[3], up[3], right[3];
	for(int axis = 0; axis < 3; axis++){
		normal[axis] = palmUp[axis]*faceSign;
		up[axis] = -backward[axis];
		right[axis] = side[axis]*faceSign;
		// Calibration offsets ride the wrist's own axes, before any of the
		// rotations below, so moving and turning a panel stay independent.
		centre[axis] = origin[axis]+
			backward[axis]*(kWristPanelBack+alongCm*0.01f)+
			palmUp[axis]*faceSign*(kWristPanelLift+liftCm*0.01f)+
			right[axis]*acrossCm*0.01f;
	}

	// The right arm is the mirrored copy of the calibration, and that pair of
	// flips is what leaves its text reading forwards. The left arm gets the
	// values unmirrored and reads backwards, so its horizontal axis is turned
	// here. Only the axis: the panel stays exactly where it was placed.
	if(panel == rw::vulkan::WRIST_PANEL_AMMO && hand == 0)
		for(int axis = 0; axis < 3; axis++)
			right[axis] = -right[axis];

	static const float kToRadians = 3.14159265f/180.0f;
	if(yawDeg != 0.0f)
		RotateWristAxes(normal, right, yawDeg*kToRadians);
	if(pitchDeg != 0.0f)
		RotateWristAxes(up, normal, pitchDeg*kToRadians);
	if(rollDeg != 0.0f)
		RotateWristAxes(right, up, rollDeg*kToRadians);

	const float scale = kWristPanelMetres[panel]*sizeScale/widthPixels;
	memset(plane, 0, sizeof(float)*16);
	for(int axis = 0; axis < 3; axis++){
		plane[axis] = right[axis]*scale;
		plane[4+axis] = -up[axis]*scale;	// screen Y runs downwards
		plane[8+axis] = normal[axis];
		plane[12+axis] = centre[axis]-
			right[axis]*scale*centreX+
			up[axis]*scale*centreY;
	}
	plane[15] = 1.0f;
	return true;
}

// A panel worn on the arm is a thing the player looks at, not a display
// pinned to the world, and with the arm down it should be as absent as a
// watch under a sleeve. Two questions decide it: whether the head is
// turned towards the panel, and whether the arm has been brought near
// enough to read. Both answer over a band rather than at a line, and the
// arm crosses that band in its own time, so there is nothing to smooth
// and nothing to pop.
static const float kWristGazeFullCos = 0.94f;	// about 20 degrees out
static const float kWristGazeGoneCos = 0.83f;	// about 34 degrees out
// The last stretch of the player's chosen range is the fade, so a panel
// held at arm's length dims rather than blinking off.
static const float kWristGazeFadeMetres = 0.15f;
static float gWristPanelOpacity[rw::vulkan::WRIST_PANEL_COUNT] = {
	1.0f, 1.0f, 1.0f, 1.0f
};

static float
WristPanelGazeOpacity(const float plane[16], float centreX,
                      float centreY)
{
	if(!VrWristPanelGazeReveal())
		return 1.0f;
	// The panel is placed in play space, so the head has to be asked for in
	// the world and brought across the same way the wrist was.
	float worldEye[3], worldForward[3], eye[3], forward[3];
	if(!VrGetViewBasis(worldEye, nil, nil, worldForward) ||
	   !rw::vulkan::firstPersonWorldPositionToPlay(worldEye, eye) ||
	   !rw::vulkan::firstPersonWorldVectorToPlay(worldForward, forward))
		return 1.0f;
	// The plane's translation is where screen (0,0) lands, so walking out
	// along its two axes to the centre pixel arrives at the middle of the
	// panel.
	float toPanel[3], distance = 0.0f;
	for(int axis = 0; axis < 3; axis++){
		toPanel[axis] = plane[12+axis]+plane[axis]*centreX+
			plane[4+axis]*centreY-eye[axis];
		distance += toPanel[axis]*toPanel[axis];
	}
	distance = sqrtf(distance);
	if(distance < 0.01f)
		return 1.0f;
	const float range = VrWristPanelGazeRange();
	if(distance >= range)
		return 0.0f;
	float facing = 0.0f;
	for(int axis = 0; axis < 3; axis++)
		facing += toPanel[axis]/distance*forward[axis];
	if(facing <= kWristGazeGoneCos)
		return 0.0f;
	const float aim = facing >= kWristGazeFullCos ? 1.0f :
		(facing-kWristGazeGoneCos)/
			(kWristGazeFullCos-kWristGazeGoneCos);
	const float reach = distance <= range-kWristGazeFadeMetres ? 1.0f :
		(range-distance)/kWristGazeFadeMetres;
	return aim < reach ? aim : reach;
}

// Puts the interface plane on the wrist and hands back the finished panel to
// bind, or null when there is nothing to draw yet. Every call also asks the
// backend for the next frame's render: the request is one-shot, so a paused or
// menu frame stops feeding it by simply not asking.
void *
// hand is -1 for the panel's own setting, or a hand index to place the same
// panel on the other arm. The calibration is written for the left hand and
// mirrored, so a second copy needs nothing but the index.
BeginVrWristPanel(int panel, float centreX, float centreY, float width,
                  float height, int hand)
{
	if(panel < 0 || panel >= rw::vulkan::WRIST_PANEL_COUNT)
		return nil;
	gWristPanelCentre[panel][0] = centreX;
	gWristPanelCentre[panel][1] = centreY;
	gWristPanelExtent[panel][0] = width;
	gWristPanelExtent[panel][1] = height;
	static bool registered = false;
	if(!registered){
		rw::vulkan::setWristPanelRenderer(rw::vulkan::WRIST_PANEL_MAP,
			&RenderWristPanelMap);
		rw::vulkan::setWristPanelRenderer(rw::vulkan::WRIST_PANEL_STATUS,
			&RenderWristPanelStatus);
		rw::vulkan::setWristPanelRenderer(rw::vulkan::WRIST_PANEL_CLOCK,
			&RenderWristPanelClock);
		rw::vulkan::setWristPanelRenderer(rw::vulkan::WRIST_PANEL_AMMO,
			&RenderWristPanelAmmo);
		registered = true;
	}
	float plane[16];
	const bool placed = gHudIm2DValid &&
		BuildWristPanelPlane(panel, hand < 0 ?
			(VrWristPanelHand(panel) ? 1 : 0) : hand,
			plane, centreX, centreY, width);
	gWristPanelOpacity[panel] = placed ?
		WristPanelGazeOpacity(plane, centreX, centreY) : 1.0f;
	// A panel the player has looked away from is not asked for either: the
	// offscreen render is what a hidden map would otherwise still cost.
	if(gWristPanelOpacity[panel] <= 0.0f)
		return nil;
	rw::vulkan::setWristPanelWanted(panel, true);

	// Null on the first frame a panel is switched on: its texture only exists
	// once a render has been through it.
	rw::Raster *texture = rw::vulkan::getWristPanelRaster(panel);
	if(texture == nil || !placed)
		return nil;
	rw::vulkan::setIm2DTransform(plane, gHudIm2DDistance, gHudIm2DEye);
	rw::vulkan::setIm2DSafeAreaSuspended(true);
	return texture;
}

float
VrWristPanelOpacity(int panel)
{
	return panel >= 0 && panel < rw::vulkan::WRIST_PANEL_COUNT ?
		gWristPanelOpacity[panel] : 1.0f;
}

void
EndVrWristPanel(void)
{
	rw::vulkan::setIm2DSafeAreaSuspended(false);
	if(gHudIm2DValid)
		rw::vulkan::setIm2DTransform(gHudIm2D, gHudIm2DDistance,
			gHudIm2DEye);
}

}  // namespace androidgame

#if MIAMIVR_STEAM_FRAME
static const char kFrameStorageRoot[] = "/sdcard/Documents/ViceCityVR";

// Returns kFrameStorageRoot when it exists (or can be created) and accepts a
// write, else nullptr.
static const char *
frameStorageRoot(void)
{
	mkdir("/sdcard/Documents", 0775);
	if(mkdir(kFrameStorageRoot, 0775) != 0 && errno != EEXIST)
		return nullptr;
	char probe[sizeof(kFrameStorageRoot) + 32];
	snprintf(probe, sizeof(probe), "%s/.write-probe", kFrameStorageRoot);
	FILE *f = fopen(probe, "w");
	if(f == nullptr)
		return nullptr;
	fclose(f);
	unlink(probe);
	return kFrameStorageRoot;
}
#endif

void
android_main(android_app *app)
{
	AppState state;
	app->userData = &state;
	app->onAppCmd = handleAppCmd;

	// App-specific external storage. Needs no runtime permission and survives
	// app updates, which makes it the right home for a multi-gigabyte game
	// data set the user pushes over adb once.
	platform::redirectStdioToLog();
	platform::startStallWatchdog();
	platform::setNativeActivity(app->activity);
	const char *storageRoot = app->activity->externalDataPath;
#if MIAMIVR_STEAM_FRAME
	// Steam Frame: Lepton wipes the app and its files when the container is
	// reset, so the game data, saves and settings live in the headset's
	// ~/Documents/ViceCityVR instead (Lepton maps /sdcard/Documents to it).
	const char *frameRoot = frameStorageRoot();
	if(frameRoot != nullptr)
		storageRoot = frameRoot;
	else
		LOGW("frame: %s is not writable; using %s (wiped by a Lepton reset)",
		     kFrameStorageRoot, storageRoot);
#endif
	platform::setStorageRoot(storageRoot);
#if MIAMIVR_STEAM_FRAME
	// vr-diag.ini lives in the storage root; read it once here, before the
	// session exists, because dump_frame changes the swapchain's usage flags.
	frame_diag::init();
#endif
#ifndef MIAMIVR_BRINGUP
	androidgame::QuestProfilerSetLogDirectory(storageRoot);
#endif

#if MIAMIVR_STEAM_FRAME
	LOGI("Vice City VR (Steam Frame) starting");
#else
	LOGI("Vice City VR (Quest) starting");
#endif

	std::thread gameThread(gameThreadMain, app, &state);

	// This thread now does nothing but keep the looper serviced, so lifecycle
	// commands and input dispatch are always answered promptly no matter how
	// long the game blocks.
	while(!app->destroyRequested && !state.destroyRequested.load() &&
	      !state.gameThreadFinished.load()){
		int events = 0;
		android_poll_source *source = nullptr;
		if(ALooper_pollOnce(-1, nullptr, &events, (void **)&source) < 0)
			continue;
		if(source != nullptr)
			source->process(app, source);
	}

	state.stopRequested = true;
	gameThread.join();

	LOGI("Vice City VR (Quest) exiting");
	ANativeActivity_finish(app->activity);

	// This reverse-engineered game has process-lifetime singletons throughout
	// its audio, streaming and RenderWare state. Horizon may retain a process
	// after the only NativeActivity finishes, then invoke android_main again
	// inside that stale address space. End the application process after a
	// fully joined/cleaned game thread so every explicit relaunch starts from
	// the same state as the first launch.
	_exit(0);
}
