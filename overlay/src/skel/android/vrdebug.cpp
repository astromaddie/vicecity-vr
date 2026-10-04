// Port of the desktop VR debug overlay (src/vr/OpenXRVR.cpp): the software
// glyph font, DrawDebugText, the FPS line and the grips+A toggle chord are
// carried over verbatim. Only the delivery differs -- the pixels reach the
// compositor through the Android session's quad layer instead of a D3D12/GL
// swapchain upload.

#include "common.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

#include "android.h"
#include "platform_android.h"
#include "OculusVR.h"
#include "QuestProfiler.h"
#include "PhysicsDirector.h"
#include "VehicleVisualDirector.h"
#include "VehicleDeformation.h"
#include "xr_vulkan_session.h"
#include "WeaponType.h"
#include "IniFile.h"
#include "Population.h"
#include "ModelSets.h"
#include "CarCtrl.h"
#include "vulkan/rwvk.h"
#include "Pools.h"
#include "Game.h"
#include "Frontend.h"
#include "Renderer.h"
#include "Shadows.h"
#include "PointLights.h"
#include "Timecycle.h"
#include "VrRagdoll.h"
#include "VrCheatMenu.h"
#include "VrCullViz.h"
#include "ParticleObject.h"
#include "CutsceneMgr.h"
#include "Camera.h"
#include "PlayerInfo.h"
#include "PlayerPed.h"
#include "Timer.h"
#include "crossplatform.h"
#include <android/log.h>

extern int GetVrCheatCount(void);
extern const char *GetVrCheatName(int index);
extern bool ActivateVrCheat(int index);
extern bool CycleVrCheatSelection(int index, int direction);
extern bool GetVrCheatToggleState(int index, bool *enabled);
extern int GetVrMissionCategoryCount(void);
extern const char *GetVrMissionCategoryName(int category);
extern int GetVrMissionCount(int category);
extern const char *GetVrMissionName(int category, int item);
extern bool ActivateVrMission(int category, int item);
extern int GetVrWeaponTypeForSlot(int slot);
extern const char *GetVrWeaponName(int weaponType);

enum {
	VR_DEBUG_WIDTH = 512,
	VR_DEBUG_HEIGHT = 437,
	VR_FPS_HEIGHT = 128,
	VR_MENU_WIDTH = 1024,
	VR_MENU_HEIGHT = 768,
	VR_CHEAT_ITEMS_PER_PAGE = 14,
};

// --- verbatim from OpenXRVR.cpp -------------------------------------------
struct DebugGlyph { char character; uint8 rows[7]; };
static const DebugGlyph gDebugGlyphs[] = {
	{' ',{0x00,0x00,0x00,0x00,0x00,0x00,0x00}}, {'%',{0x11,0x12,0x02,0x04,0x08,0x09,0x11}},
	{'+',{0x00,0x04,0x04,0x1F,0x04,0x04,0x00}},
	{'.',{0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}},
	{'/',{0x01,0x02,0x02,0x04,0x08,0x08,0x10}},
	{'-',{0x00,0x00,0x00,0x1F,0x00,0x00,0x00}}, {'<',{0x02,0x04,0x08,0x10,0x08,0x04,0x02}},
	{'>',{0x08,0x04,0x02,0x01,0x02,0x04,0x08}},
	{'[',{0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}},
	{']',{0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}},
	{':',{0x00,0x04,0x04,0x00,0x04,0x04,0x00}}, {'0',{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
	{'1',{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}}, {'2',{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
	{'3',{0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}}, {'4',{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
	{'5',{0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E}}, {'6',{0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E}},
	{'7',{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}}, {'8',{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
	{'9',{0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E}}, {'A',{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
	{'B',{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
	{'C',{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}}, {'D',{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}},
	{'E',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}}, {'F',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
	{'G',{0x0E,0x11,0x10,0x17,0x11,0x11,0x0E}}, {'H',{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
	{'I',{0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}}, {'J',{0x01,0x01,0x01,0x01,0x11,0x11,0x0E}},
	{'K',{0x11,0x12,0x14,0x18,0x14,0x12,0x11}}, {'L',{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
	{'M',{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}}, {'N',{0x11,0x19,0x19,0x15,0x13,0x13,0x11}},
	{'O',{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}}, {'P',{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
	{'Q',{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},
	{'R',{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}}, {'S',{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
	{'T',{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}}, {'U',{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
	{'V',{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}}, {'W',{0x11,0x11,0x11,0x15,0x15,0x15,0x0A}},
	{'X',{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}}, {'Y',{0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
	{'Z',{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}}
};

// The headset named in menu text.
#if MIAMIVR_STEAM_FRAME
#define VR_PLATFORM_NAME "STEAM FRAME"
#else
#define VR_PLATFORM_NAME "QUEST"
#endif

static bool gDebugVisible;
static uint8 gDebugPixels[VR_DEBUG_WIDTH*VR_DEBUG_HEIGHT*4];
static uint8 gVrMenuPixels[VR_MENU_WIDTH*VR_MENU_HEIGHT*4];
static int gDebugFps;
static double gDebugPreviousFrameMs;
static float gDebugSmoothedFrameMs;
static bool gTouchRecenterShortcutDown;
// A pad looks behind on R3 and crouches on L3. Both are off in VR: the view
// is the player's own head, so looking behind turns the world around him, and
// a crouch fires from the same click that recenters. CONTROLS hands back
// either one for anyone who wants it.
static int gVrStickLookBehind;
static int gVrStickCrouch;

bool
androidgame::VrStickLookBehindEnabled(void)
{
	return gVrStickLookBehind != 0;
}

bool
androidgame::VrStickCrouchEnabled(void)
{
	return gVrStickCrouch != 0;
}

// Rebuild the reference space around the pose being held now. Physical wheel
// grabs belong to the basis being dropped, so they are released with it.
void
androidgame::VrRecenterView(void)
{
	rw::vulkan::setFirstPersonAnchor(nil, 0.0f, 0, 0);
	OculusVR::ResetQuestDrivingInteraction();
}
static bool gVrMenuVisible;
static bool gVrMenuShortcutDown;
static bool gVrCheatShortcutDown;
static bool gVrMenuSelectDown;
static bool gVrMenuBackDown;
static int gVrMenuNavigateDirection;
static double gVrMenuNavigateRepeatAt;
static bool gVrMenuIncreaseDown;
static bool gVrMenuDecreaseDown;
static bool gVrCheatCycleDown;
static double gVrMenuIncreaseRepeatAt;
static double gVrMenuDecreaseRepeatAt;
static double gVrMenuIncreaseHoldStart;
static double gVrMenuDecreaseHoldStart;
static bool gVrSettingsLoaded;
static bool gViceCityColorEnabled = true;
// Stable single-frame FXAA. The stronger 3x3 experiment produced no visible
// headset improvement over this path, so do not expose duplicate choices.
static int gSpatialAaMode = 1;
// Vice City ships single-level world textures, so distant fences and
// signs are minified straight off the full-size image and crawl. The
// chain is built on a worker thread, so the only cost the player pays
// is about a third more texture memory.
static int gGenerateMipmaps = 1;
// The PS2 two-pass alpha rule the desktop backends have always run: the
// solid half of a blended mesh writes depth, the faint half only blends.
// Without it the invisible border around a railing cuts its own outline
// out of the wall behind it.
// Off by default all the same: every masked mesh goes twice, and a
// street of railings, fences and foliage is most of what a Vice City
// block is made of. The fringe it removes is faint and daylight-only;
// the draws are paid everywhere. On for anyone who would rather have
// it the other way round.
static int gPs2AlphaTest = 0;
// The game's point lights -- headlights, explosions, street lamps --
// evaluated per pixel against the world instead of being folded into the
// per-entity directionals the fixed function used. 0 off, 1 on, 2 adds the
// air glow that makes a source visible in the air and not only as its pool.
// Off until the player turns it on: it is new, it costs GPU on a block
// with a lamp every few metres, and off it costs nothing at all -- the
// shaders skip the block when the frame carries no lights.
static int gDynamicLights = 0;
// Percent scales for the light sum and the glow: neon-dense blocks like
// Ocean Drive stack several lamps over one stretch of pavement and the sum
// can read as overexposure, so the player picks the level, not the code.
static int gDynamicLightIntensity = 100;
static int gDynamicLightGlowIntensity = 100;
// How many of the nearest lights the backend is handed. Fewer is the perf
// lever for lamp-dense streets; visually four is already hard to tell from
// eight, which is why four is the default.
static int gDynamicLightMax = 4;
// Reflections on vehicle env-map materials: the previous frame
// reprojected onto the bodywork, over the DFF's own streak art as a
// panorama where that lookup has no answer. The original drew these
// as a second textured pass the Vulkan backend never picked up.
static int gCarReflections = 0;
// Player-tuned reflection strengths. Intensity scales the whole layer --
// at 100 some paints read glassy, lowering it is the cure. The SSR percent
// scales only the live previous-frame part, 0 leaves every car on the
// panorama; and the SSR distance draws the live part only within that many
// metres of the eye, 0 meaning no limit.
static int gCarReflectionIntensity = 100;
static int gCarReflectionSsr = 100;
static int gCarReflectionSsrDistance = 0;
// Modern water: world-anchored ripples, Fresnel, the same previous-frame
// city reflection the cars use, and the sun's path across the ripple field.
// Off by default: measured against the frame it costs about what the
// vehicle reflection does, and everything at that price ships off.
// One integer test per water pixel until someone turns it on.
static int gModernWater = 0;
// The WATER page sliders, percent of the shader's base recipe. WAVES is the
// ripple steepness, DISTORTION how far the ripples smear the mirrored
// picture, REFLECTION the mirror itself, SHEEN the flat-sky fallback where
// the mirror has no answer, GLINT and SPARKS the sun and dynamic-light
// speculars.
static int gWaterWaves = 100;
static int gWaterSpeed = 80;
static int gWaterDistortion = 55;
static int gWaterReflection = 80;
static int gWaterSheen = 100;
static int gWaterGlint = 100;
static int gWaterSparks = 100;
// How far the finished frame is reduced before the reflection samples
// it, with 1 reading the frame itself. It changes how soft the mirrored
// street looks and how much memory the copy takes; it does not change
// the frame time, which is what it was added to test. Read by the
// backend at startup, hence the restart.
static int gCarReflectionScale = 1;
// Renderer counters in logcat. A shipped build has no business
// writing to a player log every ten seconds, so this stays off
// unless it is asked for.
static int gRenderDiagnostics;
// How far down the mip chain masked geometry is sampled, in half
// levels. A leaf mask is finer than a pixel at any distance worth
// looking at, so it sparkles; nothing spatial can resolve it, and
// softening it is what a temporal filter buys on desktop.
static int gFoliageSoftness;
// A cutscene runs its own director camera. CINEMA puts that camera on
// the flat theater screen, which is where it has always gone; the other
// modes let the headset render it in stereo instead, so the scene has
// depth and the player can look around inside the shot.
static int gCutsceneMode;
// The Frame adds 3D cinema: the same flat screen, but each eye gets its own
// view of the director's shot, so the scene has depth behind the frame.
// Three depth levels share the CUTSCENES row (the page has no room for a row
// of its own); 0 and 1 keep their meaning, so a Quest settings file reads the
// same here.
enum
{
	VR_CUTSCENE_CINEMA = 0,
	VR_CUTSCENE_STEREO = 1,
	VR_CUTSCENE_CINEMA_3D_SUBTLE = 2,
	VR_CUTSCENE_CINEMA_3D = 3,
	VR_CUTSCENE_CINEMA_3D_DEEP = 4,
};
#if MIAMIVR_STEAM_FRAME
// The order the row steps through.
static const int kCutsceneModeOrder[] = {
	VR_CUTSCENE_CINEMA, VR_CUTSCENE_CINEMA_3D_SUBTLE, VR_CUTSCENE_CINEMA_3D,
	VR_CUTSCENE_CINEMA_3D_DEEP, VR_CUTSCENE_STEREO
};
// A third person view for the player who finds first person too intense.
// FIRST PERSON is the shipped view; the other two both play from Vice City's
// own camera, either on the cinema screen (the whole game as a flat picture
// with 3D depth) or in stereo from behind the shoulder. The style is the
// last third person one chosen, so a short press of View swings between it
// and first person.
enum
{
	VR_VIEW_FIRST_PERSON = 0,
	VR_VIEW_THIRD_SCREEN = 1,
	VR_VIEW_THIRD_SHOULDER = 2,
};
static int gViewMode;
static int gThirdPersonStyle = VR_VIEW_THIRD_SCREEN;
// Third person on foot: 0 the ped turns to face the direction he walks (classic
// third person), 1 he stays facing where the camera looks and strafes.
static int gThirdPersonMovement;
#endif
// Which camera this cutscene is being watched from: 0 is the director's
// own, 1 and up are the staged actors in the order the scene put them
// there. Picking the right one automatically never worked -- what the
// shot is about changes from scene to scene -- so the player cycles it
// and keeps the one they liked, per scene, for next time.
static int gCutsceneCamera;
static char gCutsceneCameraScene[32];
static bool gCutsceneCycleDown;
static bool gCutsceneStoreDown;
#if defined(MIAMIVR_DEV_TOOLS) && MIAMIVR_DEV_TOOLS
static bool gQuestQuickTestStart;
#endif
static int gQuestRenderScalePercent = 100;
static int gQuestSgsrMode = rw::vulkan::SGSR_OFF;
static int gQuestMsaaSamples = 1;
static int gOcclusionCullingMode = VR_OCCLUSION_CULLING_AUTHORED;
static bool gGameplayHudEnabled = true;
// Wrist panels: pieces of the interface ride a controller like a watch
// instead of sitting on the head-locked plane. The map is one, the money /
// health / armour / wanted readout is the other.
enum {
	WRIST_PANEL_MAP = 0,
	WRIST_PANEL_STATUS,
	WRIST_PANEL_CLOCK,
	// The round count, worn on the hand that holds the weapon.
	WRIST_PANEL_AMMO,
	WRIST_PANEL_COUNT
};
// Where a panel actually sits. A controller grip pose says nothing about the
// wrist behind it, and the outside and the inside of a wrist are not the same
// place, so the two sides are calibrated separately. Only the left hand is
// ever adjusted: the right one is the mirror image of it and is derived.
// Distances are tenths of a centimetre, angles tenths of a degree.
enum {
	WRIST_SIDE_OUTER = 0,
	WRIST_SIDE_INNER,
	WRIST_SIDE_COUNT
};
// A panel worn for walking is in the wrong place for a wheel or a set of
// handlebars: the arms are held differently and the panel has to follow. Each
// context carries a full placement of its own. A vehicle one starts out
// inheriting whatever the default is and only becomes its own once the player
// edits it, so nobody has to calibrate three times to change one thing.
enum {
	WRIST_CONTEXT_DEFAULT = 0,
	WRIST_CONTEXT_CAR,
	WRIST_CONTEXT_BIKE,
	WRIST_CONTEXT_COUNT
};
enum { WRIST_SLOT_COUNT = WRIST_CONTEXT_COUNT*WRIST_SIDE_COUNT };
#define WRIST_SLOT(context, side) ((context)*WRIST_SIDE_COUNT+(side))
static const char *const kWristContextKey[WRIST_CONTEXT_COUNT] = {
	"", "Car", "Bike"
};
static const char *const kWristContextName[WRIST_CONTEXT_COUNT] = {
	"ON FOOT", "IN A CAR", "ON A BIKE"
};
// Defaults are a placement worn and tuned in the headset rather than zeroes,
// so the calibration page is there to suit a different arm, not to make the
// panels usable at all.
struct WristPlacement {
	int along, across, lift, pitch, yaw, roll, size;
};
// The ammo counter is the odd one out: the hand it is worn on is holding a
// weapon, and the visual hand matrix follows that grip, so the panel rides
// the gun without any special anchoring.
static const WristPlacement kWristDefault[WRIST_PANEL_COUNT][WRIST_SIDE_COUNT] = {
	{	// minimap, outer then inner
		{ -61, -28, -25,  95, -955,  1200,  55 },
		{ -55, -33, -52,  95, -955,  -539,  55 }
	},
	{	// status readout
		{ -62, -26, -16,  30, -867, -1521, 100 },
		{ -62, -32, -16,  30, -803,  -132, 100 }
	},
	{	// watch, worn on the outside of the wrist
		{ -63, -28, -62, -100, -968, -1632, 100 },
		{ -55, -33, -52,  95, -955,  -539, 100 }
	},
	{	// ammo, sitting behind the weapon along the barrel
		{ -30, -20, -30,   0, -900,     0,  70 },
		{ -30, -20, -30,   0, -900,     0,  70 }
	}
};
// Driving places the panels on the vehicle instead of the arms, so the two
// vehicle contexts ship their own dashboard layout rather than inheriting a
// placement authored for a wrist. Only along/across/lift and size are read
// there: a dashboard does not carry the wrist rotations.
static const WristPlacement kVehicleHudDefault[2][WRIST_PANEL_COUNT] = {
	{	// in a car, arranged around the wheel without covering its centre
		{ -74, -96,  23, 0, 0, 0,  55 },
		{ -74,  76,  33, 0, 0, 0, 100 },
		{ -74,  97,  -9, 0, 0, 0, 100 },
		{ -74, -96, -20, 0, 0, 0,  70 }
	},
	{	// on a bike, a vertical stack centred between the two grips
		{ -22, -118, 50, 0, 0, 0,  85 },
		{   0,   -1, 80, 0, 0, 0, 100 },
		{   0,    0,105, 0, 0, 0, 100 },
		{   0,    0, 55, 0, 0, 0,  70 }
	}
};
// Raised when the shipped vehicle layout changes, so a calibration authored
// against the old wrist-follows-hand anchor is replaced instead of leaving
// the panels behind or below the driver.
enum { VEHICLE_HUD_ANCHOR_VERSION = 1 };
// Distance fog, as the desktop game draws it. Off shows the bare far clip,
// which is what the port looked like before the backend was given the
// planes; it is here to compare the two.
static bool gDistanceFog = true;
// Ammo readout colour. Green by default: it has to be legible against a
// street at night and against a wall in daylight, and it must not be
// mistaken for the health or the wanted stars.
static int gAmmoColour[3] = { 90, 235, 120 };
static bool gWristPanelOn[WRIST_PANEL_COUNT] = { false, false, false, false };
// The watch is the odd one out: both wrists already have a panel underneath,
// so it is worn where a watch is worn, on the outside of the right one.
static bool gWristPanelUnderside[WRIST_PANEL_COUNT] = { true, true, false, false };
// The map on the left, the readout and the watch on the right.
static int gWristPanelHand[WRIST_PANEL_COUNT] = { 0, 1, 1, 1 };
// One switch for all four: they are one interface worn in pieces, and
// half of it fading is not a HUD. It comes with IMMERSIVE, which is the
// preset that puts the readouts on the arms in the first place -- an arm
// at rest carrying a lit map is the thing that preset is trying to avoid.
static bool gWristPanelGaze = true;
// The reach of that glance: nearer than this and a panel is worth
// showing, beyond it the arm is not being read.
static int gWristPanelGazeRangeCm = 60;
// Driving normally hands the interface back to the head-locked plane. Immersive
// driving keeps the arms on the wheel, so the panels can stay on them there.
static bool gWristPanelsInVehicle;
static int gWristAlong[WRIST_PANEL_COUNT][WRIST_SLOT_COUNT];
static int gWristAcross[WRIST_PANEL_COUNT][WRIST_SLOT_COUNT];
static int gWristLift[WRIST_PANEL_COUNT][WRIST_SLOT_COUNT];
static int gWristPitch[WRIST_PANEL_COUNT][WRIST_SLOT_COUNT];
static int gWristYaw[WRIST_PANEL_COUNT][WRIST_SLOT_COUNT];
static int gWristRoll[WRIST_PANEL_COUNT][WRIST_SLOT_COUNT];
static int gWristSize[WRIST_PANEL_COUNT][WRIST_SLOT_COUNT];
// False while a vehicle context is still inheriting the default one.
static bool gWristContextOwned[WRIST_PANEL_COUNT][WRIST_CONTEXT_COUNT];
// Which context the calibration page is editing.
static int gVrWristContextEdit;
// Settings key prefixes. The map keeps the names it shipped with so an
// existing calibration is read back unchanged.
static const char *
WristPanelName(int panel)
{
	switch(panel){
	case WRIST_PANEL_STATUS: return "STATUS READOUT";
	case WRIST_PANEL_CLOCK: return "CLOCK";
	case WRIST_PANEL_AMMO: return "AMMO COUNTER";
	}
	return "MINIMAP";
}
static const char *const kWristPanelKey[WRIST_PANEL_COUNT] = {
	"", "Status", "Clock", "Ammo"
};
// Weapon icon, ammo counter and clock. Part of the classic interface, which is
// what a new player gets: the immersive layout is what switches them off.
static bool gHudWeaponPanel = true;
static bool gHudClock = true;
static int gHudWidthPercent = 100;
static int gHudScalePercent = 130;
static int gHudOffsetXCm;
static int gHudOffsetYCm;
static int gQuestMovementOrientation = 1;
// 0.4.1 PC parity: walking head bob is off by default; the anchor holds a
// stable eye height while the legs animate.
static int gQuestHeadBobbing = 0;
// 72 is the safe default; the Quest 3 display also offers 80/90/120 and the
// game holds 90 with headroom after the traffic director work.
static int gQuestRefreshRateHz = 72;
static int gQuestTurnMode;
static int gQuestTurnSensitivityPercent = 100;
static int gQuestHeadSteeringSensitivityPercent = 50;
static int gQuestSnapTurnAngleDegrees = 30;
// Button assignment, one entry per physical input, filled in by LoadVrSettings
// and read through androidgame::VrPadBinding. The defaults reproduce the layout
// the port shipped with. The triggers are in the table because the pad assembly
// walks all of it, not because the CONTROLS page offers them: they are the
// accelerator and the brake, and the weapon trigger on foot.
static const int kVrPadBindingDefault[androidgame::VR_PAD_SOURCE_COUNT] = {
	androidgame::VR_PAD_TARGET_CROSS,     // A
	androidgame::VR_PAD_TARGET_CIRCLE,    // B
	androidgame::VR_PAD_TARGET_SQUARE,    // X
	androidgame::VR_PAD_TARGET_TRIANGLE,  // Y
	androidgame::VR_PAD_TARGET_SQUARE,    // left trigger
	androidgame::VR_PAD_TARGET_CROSS,     // right trigger
	androidgame::VR_PAD_TARGET_L1,        // left grip
	androidgame::VR_PAD_TARGET_R1,        // right grip
	androidgame::VR_PAD_TARGET_L3,        // left stick click
	androidgame::VR_PAD_TARGET_R3         // right stick click
};
static const char *const kVrPadBindingKey[androidgame::VR_PAD_SOURCE_COUNT] = {
	"BindA", "BindB", "BindX", "BindY",
	"BindLeftTrigger", "BindRightTrigger",
	"BindLeftGrip", "BindRightGrip",
	"BindLeftStickClick", "BindRightStickClick"
};
static int gVrPadBinding[androidgame::VR_PAD_SOURCE_COUNT];
static int gVrMenuPage;
static int gVrMenuSelection;
static int gVrGraphicsSelection;
static int gVrLightingSelection;
static int gVrWaterSelection;
static int gVrWeaponsSelection;
static int gVrHudSelection;
static int gVrWristRadarSelection;
// Which panel the calibration page is editing: the row that opened it decides.
static int gVrWristPanelEdit;
static int gVrTrafficSelection;
static int gVrRagdollSelection;
static int gVrDeformationSelection;
static int gVrModelAssetsSelection;
static int gVrCheatSelection;
static int gVrCheatCategory = -1;
static int gVrCheatCategorySelection;
static int gVrVehicleSelection;
static int gVrVehicleCalibrationSelection;
static int gVrVehicleCalibrationHand;
static int gVrLocomotionSelection;
static int gVrControlsSelection;
static int gVrCalibrationSelection;
static int gVrCalibrationHand = 1;
static int gVrHolsterSelection;
static bool gVrWelcomePending;
static bool gVrWelcomeFirstRun;
static bool gVrAboutDismissArmed;
static bool gVrAboutReleaseGate;
static bool gVrWelcomeBaselineValid;
static CVector gVrWelcomeBaseline;
static int gVrCheatStatusFrames;
static char gVrCheatStatus[48];
static int gVrMissionCategory = -1;
static int gVrMissionCategorySelection;
static int gVrMissionSelection;
static int gTrafficPedPercent = 135;
static int gTrafficCarPercent = 100;
// Verlet ragdolls for dying ambient peds; the canned death animations
// play when this is off.
static int gRagdolls;
static int gQuestCpuPerformanceMode;
static int gQuestCpuPerformanceSavedMode;
static int gQuestGpuPerformanceMode = 1; // SUSTAINED; enum is declared below.

enum {
	VR_MENU_PAGE_SETTINGS,
	VR_MENU_PAGE_GRAPHICS,
	VR_MENU_PAGE_LIGHTING,
	VR_MENU_PAGE_WATER,
	VR_MENU_PAGE_WEAPONS,
	VR_MENU_PAGE_HUD,
	VR_MENU_PAGE_WRIST_RADAR,
	VR_MENU_PAGE_TRAFFIC,
	VR_MENU_PAGE_RAGDOLL,
	VR_MENU_PAGE_DEFORMATION,
	VR_MENU_PAGE_MODEL_ASSETS,
	VR_MENU_PAGE_VEHICLE,
	VR_MENU_PAGE_VEHICLE_CALIBRATION,
	VR_MENU_PAGE_LOCOMOTION,
	VR_MENU_PAGE_CONTROLS,
	VR_MENU_PAGE_CALIBRATION,
	VR_MENU_PAGE_HOLSTERS,
	VR_MENU_PAGE_CHEATS,
	VR_MENU_PAGE_MISSIONS,
	VR_MENU_PAGE_ABOUT,
};

enum eVrLightingMenuItem {
	VR_LIGHTING_MODE = 0,
	VR_LIGHTING_INTENSITY,
	VR_LIGHTING_GLOW_INTENSITY,
	VR_LIGHTING_MAX_LIGHTS,
	VR_LIGHTING_CAR_REFLECTIONS,
	VR_LIGHTING_REFLECTION_INTENSITY,
	VR_LIGHTING_REFLECTION_SSR,
	VR_LIGHTING_SSR_DISTANCE,
	VR_LIGHTING_SSR_SCALE,
	VR_LIGHTING_MODERN_WATER,
	VR_LIGHTING_BACK,
	VR_LIGHTING_ITEM_COUNT
};

enum eVrWaterMenuItem {
	VR_WATER_ENABLED = 0,
	VR_WATER_WAVES,
	VR_WATER_SPEED,
	VR_WATER_DISTORTION,
	VR_WATER_REFLECTION,
	VR_WATER_SHEEN,
	VR_WATER_GLINT,
	VR_WATER_SPARKS,
	// The same reduced copy the cars sample; this row edits the same
	// setting as the EFFECTS page so neither hides the other.
	VR_WATER_SSR_SCALE,
	VR_WATER_BACK,
	VR_WATER_ITEM_COUNT
};

enum eVrTrafficMenuItem {
	VR_TRAFFIC_PEDESTRIANS = 0,
	VR_TRAFFIC_VEHICLES,
	VR_TRAFFIC_PHYSICS_DIRECTOR,
	VR_TRAFFIC_PHYSICS_PRESET,
	VR_TRAFFIC_VISUAL_BUDGET,
	VR_TRAFFIC_VEHICLE_DEFORMATION,
	VR_TRAFFIC_RAGDOLLS,
	VR_TRAFFIC_DEFAULTS,
	VR_TRAFFIC_BACK,
	VR_TRAFFIC_ITEM_COUNT
};

enum eVrRagdollMenuItem {
	VR_RAGDOLL_ENABLED = 0,
	VR_RAGDOLL_BRAKING,
	VR_RAGDOLL_WEIGHT,
	VR_RAGDOLL_GRIP,
	VR_RAGDOLL_SHOTS,
	VR_RAGDOLL_DEFAULTS,
	VR_RAGDOLL_BACK,
	VR_RAGDOLL_ITEM_COUNT
};

enum eVrDeformationMenuItem {
	VR_DEFORMATION_ENABLED = 0,
	VR_DEFORMATION_STRENGTH,
	VR_DEFORMATION_RADIUS,
	VR_DEFORMATION_MAX_DENT,
	VR_DEFORMATION_THRESHOLD,
	VR_DEFORMATION_DEFAULTS,
	VR_DEFORMATION_BACK,
	VR_DEFORMATION_ITEM_COUNT
};

enum eVrHudMenuItem {
	VR_HUD_PRESET = 0,
	VR_HUD_ENABLED,
	VR_HUD_HORIZONTAL_SCALE,
	VR_HUD_SCALE,
	VR_HUD_OFFSET_X,
	VR_HUD_OFFSET_Y,
	// The four panels are the same object with different contents, so they
	// share one set of controls and a row that says which one is being
	// worked on. Three copies of an identical interface filled the page.
	VR_HUD_PANEL,
	VR_HUD_PANEL_SHOW,
	VR_HUD_PANEL_HAND,
	VR_HUD_PANEL_SIDE,
	VR_HUD_PANEL_CALIBRATE,
	// The reveal belongs to the worn set rather than to the panel being
	// edited above, so it sits below the selector's own rows.
	VR_HUD_PANEL_GAZE,
	VR_HUD_PANEL_GAZE_RANGE,
	VR_HUD_WRIST_IN_VEHICLE,
	VR_HUD_WEAPON_PANEL,
	VR_HUD_CLOCK,
	VR_HUD_BACK,
	VR_HUD_ITEM_COUNT
};

// Placement of the wrist minimap. Outside and inside of a wrist are separate
// sets: they are different places on the arm and do not share numbers.
enum eVrWristRadarMenuItem {
	VR_WRIST_CONTEXT = 0,
	VR_WRIST_SIDE,
	VR_WRIST_HAND,
	VR_WRIST_ALONG,
	VR_WRIST_ACROSS,
	VR_WRIST_LIFT,
	VR_WRIST_PITCH,
	VR_WRIST_YAW,
	VR_WRIST_ROLL,
	VR_WRIST_SIZE,
	// Only the ammo counter is text the port draws itself, so only it has a
	// colour to choose. The rows are hidden for the other three.
	VR_WRIST_COLOUR_RED,
	VR_WRIST_COLOUR_GREEN,
	VR_WRIST_COLOUR_BLUE,
	VR_WRIST_COPY,
	VR_WRIST_RESET,
	VR_WRIST_BACK,
	VR_WRIST_ITEM_COUNT
};

// Native Quest exposes only settings backed by its Vulkan/OpenXR runtime.
// Desktop-only D3D12 features are deliberately omitted so every visible row
// is actionable on the headset.
enum eVrMainMenuItem {
	VR_MAIN_GRAPHICS = 0,
	VR_MAIN_TRAFFIC_SETTINGS,
	VR_MAIN_HUD,
	VR_MAIN_MODEL_ASSETS,
	VR_MAIN_VEHICLE_SETTINGS,
	VR_MAIN_LOCOMOTION_SETTINGS,
	VR_MAIN_CONTROLS,
	VR_MAIN_WEAPONS,
	VR_MAIN_HOLSTERS,
	VR_MAIN_CHEATS,
	VR_MAIN_ABOUT,
	VR_MAIN_ITEM_COUNT
};

enum eVrGraphicsMenuItem {
	VR_GRAPHICS_RENDER_SCALE = 0,
	VR_GRAPHICS_SGSR,
	VR_GRAPHICS_MSAA,
	VR_GRAPHICS_FXAA,
	VR_GRAPHICS_MIPMAPS,
	VR_GRAPHICS_PS2_ALPHA,
	VR_GRAPHICS_FOLIAGE,
	VR_GRAPHICS_DIAGNOSTICS,
	VR_GRAPHICS_COLOR,
	VR_GRAPHICS_PROFILER,
	VR_GRAPHICS_CPU_PERFORMANCE,
	VR_GRAPHICS_GPU_PERFORMANCE,
	VR_GRAPHICS_SHADOWS,
	VR_GRAPHICS_DYNAMIC_LIGHTS,
	VR_GRAPHICS_OCCLUSION,
#if defined(MIAMIVR_DEV_TOOLS) && MIAMIVR_DEV_TOOLS
	VR_GRAPHICS_CULL_VIZ,
#endif
	VR_GRAPHICS_FOUNTAIN,
	VR_GRAPHICS_FOG,
#if defined(MIAMIVR_DEV_TOOLS) && MIAMIVR_DEV_TOOLS
	VR_GRAPHICS_QUICK_START,
#endif
	VR_GRAPHICS_BACK,
	VR_GRAPHICS_ITEM_COUNT
};

enum eVrWeaponsMenuItem {
	VR_WEAPONS_HANDS = 0,
	VR_WEAPONS_LASER,
	VR_WEAPONS_HOLSTER_HIGHLIGHTS,
	VR_WEAPONS_MANUAL_RELOAD,
	VR_WEAPONS_SCOPE_AIM,
	VR_WEAPONS_GRIP_LOCK,
	VR_WEAPONS_CALIBRATION,
	VR_WEAPONS_BACK,
	VR_WEAPONS_ITEM_COUNT
};

enum eVrModelAssetsMenuItem {
	VR_MODEL_ASSETS_PRESET = 0,
	VR_MODEL_ASSETS_WORLD,
	VR_MODEL_ASSETS_VEGETATION,
	VR_MODEL_ASSETS_VEHICLES,
	VR_MODEL_ASSETS_PEDS,
	VR_MODEL_ASSETS_WEAPONS,
	VR_MODEL_ASSETS_BACK,
	VR_MODEL_ASSETS_ITEM_COUNT
};

enum eQuestCpuPerformanceMode {
	QUEST_CPU_PERFORMANCE_AUTO = 0,
	QUEST_CPU_PERFORMANCE_SUSTAINED,
	QUEST_CPU_PERFORMANCE_BOOST,
	QUEST_CPU_PERFORMANCE_COUNT
};

enum eVrVehicleMenuItem {
	// The page works on one kind of vehicle at a time. Everything under the
	// selector belongs to the kind it names; nothing else is drawn.
	VR_VEHICLE_KIND = 0,
	VR_VEHICLE_DRIVING_TYPE,
	VR_VEHICLE_THIRD_PERSON,
	VR_VEHICLE_FORWARD_FIRE,
	VR_VEHICLE_DEFAULT_SEAT_HEIGHT,
	VR_VEHICLE_DEFAULT_SEAT_FORWARD,
	VR_VEHICLE_GLOBAL_SEAT_HEIGHT,
	VR_VEHICLE_GLOBAL_SEAT_FORWARD,
	VR_VEHICLE_MODEL_SEAT_HEIGHT,
	VR_VEHICLE_MODEL_SEAT_FORWARD,
	VR_VEHICLE_MOTION_HAND,
	VR_VEHICLE_WHEEL_VISIBLE,
	VR_VEHICLE_MODEL_WHEEL_VISIBLE,
	VR_VEHICLE_WHEEL_HAND_PULL_BACK,
	VR_VEHICLE_HANDLE_HIGHLIGHTS,
	VR_VEHICLE_BIKE_LOCK_HORIZON,
	VR_VEHICLE_BIKE_THROTTLE,
	VR_VEHICLE_BIKE_VISUAL_LEAN,
	VR_VEHICLE_BIKE_VIEW_TILT,
	VR_VEHICLE_BIKE_THROW_RIDER,
	VR_VEHICLE_CALIBRATION,
	VR_VEHICLE_BACK,
	VR_VEHICLE_ITEM_COUNT
};

enum eVrLocomotionMenuItem {
#if MIAMIVR_STEAM_FRAME
	VR_LOCOMOTION_VIEW = 0,
	VR_LOCOMOTION_MOVEMENT_ORIENTATION,
	VR_LOCOMOTION_THIRD_PERSON_MOVEMENT,
#else
	VR_LOCOMOTION_MOVEMENT_ORIENTATION = 0,
#endif
	VR_LOCOMOTION_TURN_MODE,
	VR_LOCOMOTION_TURN_SENSITIVITY,
	VR_LOCOMOTION_SNAP_ANGLE,
	VR_LOCOMOTION_HEAD_BOBBING,
	VR_LOCOMOTION_CUTSCENES,
	VR_LOCOMOTION_REFRESH_RATE,
	VR_LOCOMOTION_RECENTER,
	VR_LOCOMOTION_BACK,
	VR_LOCOMOTION_ITEM_COUNT
};

// Which physical input each row of the CONTROLS page edits; the triggers are
// deliberately absent. VrControlsRowSource maps a row back to its input.
enum eVrControlsMenuItem {
	VR_CONTROLS_LAYOUT = 0,
	VR_CONTROLS_A,
	VR_CONTROLS_B,
	VR_CONTROLS_X,
	VR_CONTROLS_Y,
	VR_CONTROLS_LEFT_GRIP,
	VR_CONTROLS_RIGHT_GRIP,
	VR_CONTROLS_LEFT_STICK_CLICK,
	VR_CONTROLS_RIGHT_STICK_CLICK,
	VR_CONTROLS_LOOK_BEHIND,
	VR_CONTROLS_CROUCH,
	VR_CONTROLS_RESET,
	VR_CONTROLS_BACK,
	VR_CONTROLS_ITEM_COUNT
};

// SWAPPED HANDS is the reason the page exists: it moves jump and enter/exit to
// the right controller and attack and sprint to the left, for players who read
// the shipped layout as inverted.
enum eVrControlsLayout {
	VR_CONTROLS_LAYOUT_DEFAULT = 0,
	VR_CONTROLS_LAYOUT_SWAPPED_HANDS,
	VR_CONTROLS_LAYOUT_CUSTOM,
	VR_CONTROLS_LAYOUT_COUNT
};

enum eQuestMovementOrientation {
	QUEST_MOVEMENT_ORIENTATION_BODY = 0,
	QUEST_MOVEMENT_ORIENTATION_HEAD,
	QUEST_MOVEMENT_ORIENTATION_HEAD_TURN_EXPERIMENTAL,
	QUEST_MOVEMENT_ORIENTATION_COUNT
};

enum eQuestTurnMode {
	QUEST_TURN_SMOOTH = 0,
	QUEST_TURN_SNAP,
	QUEST_TURN_MODE_COUNT
};

static const char *
QuestCpuPerformanceModeName(int mode)
{
	static const char *const names[QUEST_CPU_PERFORMANCE_COUNT] = {
		"AUTO", "SUSTAINED", "BOOST (TEST)"
	};
	return mode >= 0 && mode < QUEST_CPU_PERFORMANCE_COUNT ?
		names[mode] : "UNKNOWN";
}

static const uint8 *
FindDebugGlyph(char character)
{
	if(character >= 'a' && character <= 'z')
		character = character-'a'+'A';
	for(uint32 i = 0; i < ARRAY_SIZE(gDebugGlyphs); i++)
		if(gDebugGlyphs[i].character == character)
			return gDebugGlyphs[i].rows;
	return gDebugGlyphs[0].rows;
}

static void
PutDebugPixel(int x, int y, uint8 red, uint8 green, uint8 blue, uint8 alpha)
{
	if(x < 0 || y < 0 || x >= VR_DEBUG_WIDTH || y >= VR_DEBUG_HEIGHT) return;
	// Vulkan images are top-down, same as the desktop's D3D12 branch.
	const int offset = (y*VR_DEBUG_WIDTH+x)*4;
	gDebugPixels[offset+0] = red; gDebugPixels[offset+1] = green;
	gDebugPixels[offset+2] = blue; gDebugPixels[offset+3] = alpha;
}

static void
DrawDebugText(const char *value, int centreX, int y, int scale,
              uint8 red, uint8 green, uint8 blue)
{
	const int advance = scale*6;
	int x = centreX-(int)strlen(value)*advance/2;
	for(const char *ch = value; *ch; ch++, x += advance){
		const uint8 *rows = FindDebugGlyph(*ch);
		for(int row = 0; row < 7; row++) for(int column = 0; column < 5; column++)
			if(rows[row]&(1<<(4-column)))
				for(int py = 0; py < scale; py++) for(int px = 0; px < scale; px++)
					PutDebugPixel(x+column*scale+px, y+row*scale+py, red, green, blue, 255);
	}
}

static void
PutVrMenuPixel(int x, int y, uint8 red, uint8 green, uint8 blue, uint8 alpha)
{
	if(x < 0 || y < 0 || x >= VR_MENU_WIDTH || y >= VR_MENU_HEIGHT)
		return;
	const int offset = (y*VR_MENU_WIDTH+x)*4;
	gVrMenuPixels[offset+0] = red;
	gVrMenuPixels[offset+1] = green;
	gVrMenuPixels[offset+2] = blue;
	gVrMenuPixels[offset+3] = alpha;
}

static void
FillVrMenuRect(int left, int top, int right, int bottom,
               uint8 red, uint8 green, uint8 blue, uint8 alpha)
{
	left = Max(0, left);
	top = Max(0, top);
	right = Min(VR_MENU_WIDTH, right);
	bottom = Min(VR_MENU_HEIGHT, bottom);
	for(int y = top; y < bottom; y++)
		for(int x = left; x < right; x++)
			PutVrMenuPixel(x, y, red, green, blue, alpha);
}

static void
DrawVrMenuText(const char *value, int centreX, int y, int scale,
               uint8 red, uint8 green, uint8 blue)
{
	const int advance = scale*6;
	int x = centreX-(int)strlen(value)*advance/2;
	for(const char *ch = value; *ch; ch++, x += advance){
		const uint8 *rows = FindDebugGlyph(*ch);
		for(int row = 0; row < 7; row++)
			for(int column = 0; column < 5; column++)
				if(rows[row]&(1<<(4-column)))
					for(int py = 0; py < scale; py++)
						for(int px = 0; px < scale; px++)
							PutVrMenuPixel(
								x+column*scale+px,
								y+row*scale+py,
								red, green, blue, 255);
	}
}
// --------------------------------------------------------------------------

namespace androidgame {

static void SaveVrInteger(const char *key, int value);
static void SaveWristPanels(void);

// Marks a vehicle context as carrying its own values rather than the on-foot
// ones. Kept out of the placement keys so an older settings file still reads
// back as plain inheritance.
static const char *
WristContextKey(int panel, int context)
{
	static char key[64];
	snprintf(key, sizeof(key), "Wrist%s%sCustom",
		kWristPanelKey[panel], kWristContextKey[context]);
	return key;
}

// Where the player is right now, which decides the placement in use.
static int
ActiveWristContext(void)
{
	CVehicle *vehicle = FindPlayerVehicle();
	if(vehicle == nil)
		return WRIST_CONTEXT_DEFAULT;
	return vehicle->IsBike() ? WRIST_CONTEXT_BIKE : WRIST_CONTEXT_CAR;
}

// ...and the one whose values apply. Every context carries a usable
// placement of its own, so the active one is always the one that counts.
static int
ResolvedWristContext(int panel)
{
	(void)panel;
	return ActiveWristContext();
}

// The vehicle slots already hold the shipped dashboard layout, so claiming one
// only records that the player has started editing it.
static void
OwnWristContext(int panel, int context)
{
	if(context == WRIST_CONTEXT_DEFAULT)
		return;
	gWristContextOwned[panel][context] = true;
}

static void
ApplyTrafficSettings(void)
{
	CIniFile::PedNumberMultiplier = gTrafficPedPercent/100.0f;
	CIniFile::CarNumberMultiplier = gTrafficCarPercent/100.0f;
	CPopulation::MaxNumberOfPedsInUse =
		(int32)(25.0f*CIniFile::PedNumberMultiplier);
	CPopulation::MaxNumberOfPedsInUseInterior =
		(int32)(40.0f*CIniFile::PedNumberMultiplier);
	CCarCtrl::MaxNumberOfCarsInUse =
		(int32)(12.0f*CIniFile::CarNumberMultiplier);
}

static void
LoadRagdollSettings(void)
{
	gRagdolls = GetPrivateProfileIntA("VR", "Ragdolls", 0,
		".\\vr_settings.ini") != 0 ? 1 : 0;
	VrRagdoll::SetEnabled(gRagdolls);
	VrRagdoll::SetBrakePercent(GetPrivateProfileIntA("VR",
		"RagdollBrakePercent", 200, ".\\vr_settings.ini"));
	VrRagdoll::SetGripPercent(GetPrivateProfileIntA("VR",
		"RagdollGripPercent", 150, ".\\vr_settings.ini"));
	VrRagdoll::SetShotPercent(GetPrivateProfileIntA("VR",
		"RagdollShotPercent", 100, ".\\vr_settings.ini"));
	VrRagdoll::SetWeightPercent(GetPrivateProfileIntA("VR",
		"RagdollWeightPercent", 200, ".\\vr_settings.ini"));
}

static void
LoadVehicleDeformationSettings(void)
{
	VehicleDeformation::SetStrengthPercent(GetPrivateProfileIntA("VR",
		"VehicleDeformationStrengthPercent", 100, ".\\vr_settings.ini"));
	VehicleDeformation::SetRadiusPercent(GetPrivateProfileIntA("VR",
		"VehicleDeformationRadiusPercent", 100, ".\\vr_settings.ini"));
	VehicleDeformation::SetMaxDentCentimeters(GetPrivateProfileIntA("VR",
		"VehicleDeformationMaxDentCm", 32, ".\\vr_settings.ini"));
	VehicleDeformation::SetThresholdPercent(GetPrivateProfileIntA("VR",
		"VehicleDeformationThresholdPercent", 100, ".\\vr_settings.ini"));
	VehicleDeformation::SetVehicleDeformationEnabled(GetPrivateProfileIntA("VR",
		"VehicleDeformation", 0, ".\\vr_settings.ini") != 0);
}

static void
LoadVrSettings(void)
{
	if(gVrSettingsLoaded)
		return;
	gViceCityColorEnabled =
		GetPrivateProfileIntA("VR", "ViceCityColor", 1,
			".\\vr_settings.ini") != 0;
	gGenerateMipmaps = GetPrivateProfileIntA("VR", "GenerateMipmaps", 1,
		".\\vr_settings.ini") != 0 ? 1 : 0;
	gPs2AlphaTest = GetPrivateProfileIntA("VR", "Ps2AlphaTest", 0,
		".\\vr_settings.ini") != 0 ? 1 : 0;
	gDynamicLights = clamp((int)(int32)GetPrivateProfileIntA("VR",
		"DynamicLights", 0, ".\\vr_settings.ini"), 0, 2);
	gDynamicLightIntensity = clamp((int)(int32)GetPrivateProfileIntA("VR",
		"DynamicLightIntensity", 100, ".\\vr_settings.ini"), 25, 200);
	gDynamicLightGlowIntensity = clamp((int)(int32)GetPrivateProfileIntA("VR",
		"DynamicLightGlowIntensity", 100, ".\\vr_settings.ini"), 25, 200);
	gDynamicLightMax = clamp((int)(int32)GetPrivateProfileIntA("VR",
		"DynamicLightMax", 4, ".\\vr_settings.ini"), 2, 8) & ~1;
	gCarReflections = GetPrivateProfileIntA("VR", "CarReflections", 0,
		".\\vr_settings.ini") != 0 ? 1 : 0;
	gCarReflectionIntensity = clamp((int)(int32)GetPrivateProfileIntA("VR",
		"CarReflectionIntensity", 100, ".\\vr_settings.ini"), 10, 200);
	gCarReflectionSsr = clamp((int)(int32)GetPrivateProfileIntA("VR",
		"CarReflectionSsr", 100, ".\\vr_settings.ini"), 0, 150);
	gCarReflectionScale = GetPrivateProfileIntA("VR",
		"CarReflectionScale", 1, ".\\vr_settings.ini");
	if(gCarReflectionScale != 1 && gCarReflectionScale != 2 &&
	   gCarReflectionScale != 4 && gCarReflectionScale != 8)
		gCarReflectionScale = 4;
	gCarReflectionSsrDistance = clamp((int)(int32)GetPrivateProfileIntA("VR",
		"CarReflectionSsrDistance", 0, ".\\vr_settings.ini"), 0, 150);
	if(gCarReflectionSsrDistance != 0 && gCarReflectionSsrDistance < 5)
		gCarReflectionSsrDistance = 5;
	gModernWater = GetPrivateProfileIntA("VR", "ModernWater", 0,
		".\\vr_settings.ini") != 0 ? 1 : 0;
	gWaterWaves = clamp((int)(int32)GetPrivateProfileIntA("VR",
		"WaterWaves", 100, ".\\vr_settings.ini"), 0, 200);
	gWaterSpeed = clamp((int)(int32)GetPrivateProfileIntA("VR",
		"WaterSpeed", 80, ".\\vr_settings.ini"), 0, 200);
	gWaterDistortion = clamp((int)(int32)GetPrivateProfileIntA("VR",
		"WaterDistortion", 55, ".\\vr_settings.ini"), 0, 200);
	gWaterReflection = clamp((int)(int32)GetPrivateProfileIntA("VR",
		"WaterReflection", 80, ".\\vr_settings.ini"), 0, 200);
	gWaterSheen = clamp((int)(int32)GetPrivateProfileIntA("VR",
		"WaterSheen", 100, ".\\vr_settings.ini"), 0, 200);
	gWaterGlint = clamp((int)(int32)GetPrivateProfileIntA("VR",
		"WaterGlint", 100, ".\\vr_settings.ini"), 0, 200);
	gWaterSparks = clamp((int)(int32)GetPrivateProfileIntA("VR",
		"WaterSparks", 100, ".\\vr_settings.ini"), 0, 200);
	gRenderDiagnostics = GetPrivateProfileIntA("VR",
		"RenderDiagnostics", 0, ".\\vr_settings.ini") != 0 ? 1 : 0;
	gFoliageSoftness = clamp((int)(int32)GetPrivateProfileIntA("VR",
		"FoliageSoftness", 0, ".\\vr_settings.ini"), 0, 3);
#if MIAMIVR_STEAM_FRAME
	gCutsceneMode = clamp((int)(int32)GetPrivateProfileIntA("VR",
		"CutsceneMode", VR_CUTSCENE_CINEMA_3D, ".\\vr_settings.ini"),
		VR_CUTSCENE_CINEMA, VR_CUTSCENE_CINEMA_3D_DEEP);
	gViewMode = clamp((int)(int32)GetPrivateProfileIntA("VR",
		"ViewMode", VR_VIEW_FIRST_PERSON, ".\\vr_settings.ini"),
		VR_VIEW_FIRST_PERSON, VR_VIEW_THIRD_SHOULDER);
	gThirdPersonStyle = clamp((int)(int32)GetPrivateProfileIntA("VR",
		"ThirdPersonStyle", VR_VIEW_THIRD_SCREEN, ".\\vr_settings.ini"),
		VR_VIEW_THIRD_SCREEN, VR_VIEW_THIRD_SHOULDER);
	gThirdPersonMovement = GetPrivateProfileIntA("VR",
		"ThirdPersonMovement", 0, ".\\vr_settings.ini") != 0 ? 1 : 0;
#else
	gCutsceneMode = clamp((int)(int32)GetPrivateProfileIntA("VR",
		"CutsceneMode", 0, ".\\vr_settings.ini"), 0, 1);
#endif
	// Remembered across launches so a build handed over for testing can
	// come up already showing it. Off for everyone else.
	QuestProfilerSetEnabled(GetPrivateProfileIntA("VR", "ShowProfiler",
		0, ".\\vr_settings.ini") != 0);
	gSpatialAaMode = GetPrivateProfileIntA("VR", "AntiAliasing", 1,
		".\\vr_settings.ini") != 0;
#if defined(MIAMIVR_DEV_TOOLS) && MIAMIVR_DEV_TOOLS
	gQuestQuickTestStart =
		GetPrivateProfileIntA("VR", "QuickTestStart", 0,
			".\\vr_settings.ini") != 0;
#endif
	{
		static const int scales[] = { 100, 125, 150, 175 };
		const int savedScale = GetPrivateProfileIntA("VR", "RenderScalePercent",
			100, ".\\vr_settings.ini");
		int best = 0;
		for(int i = 1; i < (int)ARRAY_SIZE(scales); i++)
			if(Abs(scales[i]-savedScale) < Abs(scales[best]-savedScale))
				best = i;
	gQuestRenderScalePercent = scales[best];
	}
	// Temporal AA is intentionally unavailable until its stereo instability is
	// solved. Preserve an exact native fallback instead of letting an old INI
	// silently reactivate an eye-straining mode.
	gQuestSgsrMode = rw::vulkan::SGSR_OFF;
	SaveVrInteger("Sgsr2Mode", gQuestSgsrMode);
	gQuestMsaaSamples = GetPrivateProfileIntA("VR", "MsaaSamples", 1,
		".\\vr_settings.ini");
	// Off, 2x or 4x and nothing else: the swapchain is built from
	// this and an ini carrying anything else has to land somewhere.
	gQuestMsaaSamples = gQuestMsaaSamples >= 4 ? 4 :
		gQuestMsaaSamples >= 2 ? 2 : 1;
	CParticleObject::SetVrFountainQuality(Min(Max(GetPrivateProfileIntA("VR",
		"FountainQuality", VR_FOUNTAIN_OPTIMIZED, ".\\vr_settings.ini"),
		(int)VR_FOUNTAIN_OFF), (int)VR_FOUNTAIN_QUALITY_COUNT-1));
	{
		// Migrate the old boolean without rewriting it. OFF remains the exact
		// original full-360 render path; AUTHORED is the recommended default.
		const int legacyOcclusion = GetPrivateProfileIntA("VR",
			"OcclusionCulling", 1, ".\\vr_settings.ini") != 0 ?
			VR_OCCLUSION_CULLING_AUTHORED : VR_OCCLUSION_CULLING_OFF;
		// V2 adds a correctness-first STEREO SAFE mode before the two authored
		// experimental modes. Do not reinterpret the old value 2 (AGGRESSIVE) as
		// a new experimental mode after an update; migrate everyone to SAFE/OFF.
		gOcclusionCullingMode = Min(Max(GetPrivateProfileIntA("VR",
			"OcclusionCullingModeV2", legacyOcclusion, ".\\vr_settings.ini"),
			(int)VR_OCCLUSION_CULLING_OFF),
			(int)VR_OCCLUSION_CULLING_MODE_COUNT-1);
		CRenderer::SetVrOcclusionCullingMode(gOcclusionCullingMode);
	}
	CShadows::SetRenderEnabled(GetPrivateProfileIntA("VR", "ShadowsEnabled",
		1, ".\\vr_settings.ini") != 0);
	gWristPanelOn[WRIST_PANEL_MAP] = GetPrivateProfileIntA("VR",
		"WristRadar", 1, ".\\vr_settings.ini") != 0;
	gWristPanelUnderside[WRIST_PANEL_MAP] = GetPrivateProfileIntA("VR",
		"WristRadarUnderside", 1, ".\\vr_settings.ini") != 0;
	gWristPanelHand[WRIST_PANEL_MAP] = GetPrivateProfileIntA("VR",
		"WristRadarHand", 0, ".\\vr_settings.ini") != 0 ? 1 : 0;
	gWristPanelOn[WRIST_PANEL_STATUS] = GetPrivateProfileIntA("VR",
		"WristStatus", 1, ".\\vr_settings.ini") != 0;
	gWristPanelUnderside[WRIST_PANEL_STATUS] = GetPrivateProfileIntA("VR",
		"WristStatusUnderside", 1, ".\\vr_settings.ini") != 0;
	gWristPanelHand[WRIST_PANEL_STATUS] = GetPrivateProfileIntA("VR",
		"WristStatusHand", 1, ".\\vr_settings.ini") != 0 ? 1 : 0;
	gWristPanelOn[WRIST_PANEL_AMMO] = GetPrivateProfileIntA("VR",
		"WristAmmo", 1, ".\\vr_settings.ini") != 0;
	gWristPanelUnderside[WRIST_PANEL_AMMO] = GetPrivateProfileIntA("VR",
		"WristAmmoUnderside", 0, ".\\vr_settings.ini") != 0;
	gWristPanelHand[WRIST_PANEL_AMMO] = GetPrivateProfileIntA("VR",
		"WristAmmoHand", 1, ".\\vr_settings.ini") != 0 ? 1 : 0;
	gWristPanelOn[WRIST_PANEL_CLOCK] = GetPrivateProfileIntA("VR",
		"WristClock", 1, ".\\vr_settings.ini") != 0;
	gWristPanelUnderside[WRIST_PANEL_CLOCK] = GetPrivateProfileIntA("VR",
		"WristClockUnderside", 0, ".\\vr_settings.ini") != 0;
	gWristPanelHand[WRIST_PANEL_CLOCK] = GetPrivateProfileIntA("VR",
		"WristClockHand", 1, ".\\vr_settings.ini") != 0 ? 1 : 0;
	gWristPanelGaze = GetPrivateProfileIntA("VR",
		"WristPanelGazeReveal", 1, ".\\vr_settings.ini") != 0;
	gWristPanelGazeRangeCm = clamp((int)(int32)GetPrivateProfileIntA("VR",
		"WristPanelGazeRangeCm", 60, ".\\vr_settings.ini"), 20, 200);
	gWristPanelsInVehicle = GetPrivateProfileIntA("VR",
		"WristPanelsInVehicle", 1, ".\\vr_settings.ini") != 0;
	static const char *const fieldNames[7] = {
		"Along", "Across", "Lift", "Pitch", "Yaw", "Roll", "Size"
	};
	static const int fieldLimit[7][2] = {
		{ -300, 300 }, { -300, 300 }, { -300, 300 },
		{ -1800, 1800 }, { -1800, 1800 }, { -1800, 1800 }, { 40, 250 }
	};
	for(int panel = 0; panel < WRIST_PANEL_COUNT; panel++)
		for(int context = 0; context < WRIST_CONTEXT_COUNT; context++){
			gWristContextOwned[panel][context] =
				context == WRIST_CONTEXT_DEFAULT ||
				GetPrivateProfileIntA("VR",
					WristContextKey(panel, context),
					0, ".\\vr_settings.ini") != 0;
			for(int side = 0; side < WRIST_SIDE_COUNT; side++){
				const WristPlacement &shipped = kWristDefault[panel][side];
				const int slot = WRIST_SLOT(context, side);
				// A vehicle context that has never been edited reads back the
				// on-foot values, so switching to it shows what is on screen.
				const WristPlacement &dashboard =
					kVehicleHudDefault[context == WRIST_CONTEXT_BIKE ?
						1 : 0][panel];
				int *field[7] = {
					&gWristAlong[panel][slot], &gWristAcross[panel][slot],
					&gWristLift[panel][slot], &gWristPitch[panel][slot],
					&gWristYaw[panel][slot], &gWristRoll[panel][slot],
					&gWristSize[panel][slot]
				};
				const int shippedValue[7] = {
					shipped.along, shipped.across, shipped.lift,
					shipped.pitch, shipped.yaw, shipped.roll, shipped.size
				};
				const int dashboardValue[7] = {
					dashboard.along, dashboard.across, dashboard.lift,
					dashboard.pitch, dashboard.yaw, dashboard.roll,
					dashboard.size
				};
				for(int item = 0; item < 7; item++){
					char key[64];
					snprintf(key, sizeof(key), "Wrist%s%s%s%s",
						kWristPanelKey[panel],
						kWristContextKey[context],
						side == WRIST_SIDE_INNER ? "Inner" : "Outer",
						fieldNames[item]);
					const int fallback =
						context == WRIST_CONTEXT_DEFAULT ?
							shippedValue[item] : dashboardValue[item];
					// A vehicle context nobody has edited takes the
					// shipped dashboard layout, ignoring whatever an
					// older build left in the file for it.
					const int loaded =
						context != WRIST_CONTEXT_DEFAULT &&
						!gWristContextOwned[panel][context] ?
							fallback : (int)(int32)
							GetPrivateProfileIntA("VR", key,
								fallback, ".\\vr_settings.ini");
					*field[item] = Min(Max(loaded,
						fieldLimit[item][0]), fieldLimit[item][1]);
				}
			}
		}
	if(GetPrivateProfileIntA("VR", "VehicleHudAnchorVersion", 0,
	     ".\\vr_settings.ini") < VEHICLE_HUD_ANCHOR_VERSION){
		// Those values were authored around a panel that rode the hand and
		// can leave the dashboard one behind or below the driver. Only the
		// vehicle contexts are touched; on-foot calibration is kept.
		for(int panel = 0; panel < WRIST_PANEL_COUNT; panel++)
			for(int context = WRIST_CONTEXT_CAR;
			    context < WRIST_CONTEXT_COUNT; context++){
				const WristPlacement &dashboard =
					kVehicleHudDefault[
						context == WRIST_CONTEXT_BIKE ? 1 : 0][panel];
				gWristContextOwned[panel][context] = false;
				for(int side = 0; side < WRIST_SIDE_COUNT; side++){
					const int slot = WRIST_SLOT(context, side);
					gWristAlong[panel][slot] = dashboard.along;
					gWristAcross[panel][slot] = dashboard.across;
					gWristLift[panel][slot] = dashboard.lift;
					gWristPitch[panel][slot] = 0;
					gWristYaw[panel][slot] = 0;
					gWristRoll[panel][slot] = 0;
					gWristSize[panel][slot] = dashboard.size;
				}
			}
		SaveWristPanels();
		SaveVrInteger("VehicleHudAnchorVersion",
			VEHICLE_HUD_ANCHOR_VERSION);
	}
	// The arms carry the interface out of the box. Both corner panels belong
	// to the CLASSIC preset, which is one row away on the HUD page.
	gDistanceFog = GetPrivateProfileIntA("VR", "DistanceFog", 1,
		".\\vr_settings.ini") != 0;
	static const char *const ammoColourKey[3] = {
		"AmmoColourRed", "AmmoColourGreen", "AmmoColourBlue"
	};
	static const int ammoColourDefault[3] = { 90, 235, 120 };
	for(int channel = 0; channel < 3; channel++)
		gAmmoColour[channel] = Min(Max(GetPrivateProfileIntA("VR",
			ammoColourKey[channel], ammoColourDefault[channel],
			".\\vr_settings.ini"), 0), 255);
	gHudWeaponPanel = GetPrivateProfileIntA("VR", "HudWeaponPanel", 0,
		".\\vr_settings.ini") != 0;
	gHudClock = GetPrivateProfileIntA("VR", "HudClock", 0,
		".\\vr_settings.ini") != 0;
	gGameplayHudEnabled =
		GetPrivateProfileIntA("VR", "GameplayHud", 1,
			".\\vr_settings.ini") != 0;
	gHudWidthPercent = Min(Max(GetPrivateProfileIntA("VR",
		"HudWidthPercent", 100, ".\\vr_settings.ini"), 50), 200);
	gHudScalePercent = Min(Max(GetPrivateProfileIntA("VR",
		"HudScalePercent", 130, ".\\vr_settings.ini"), 50), 200);
	// Win32-style profile reads return an unsigned value. Cast back to signed
	// before clamping so a saved negative offset cannot become +100.
	gHudOffsetXCm = Min(Max((int)(int32)GetPrivateProfileIntA("VR",
		"HudOffsetXCm", 0, ".\\vr_settings.ini"), -100), 100);
	gHudOffsetYCm = Min(Max((int)(int32)GetPrivateProfileIntA("VR",
		"HudOffsetYCm", 0, ".\\vr_settings.ini"), -100), 100);
	gQuestMovementOrientation = Min(Max(GetPrivateProfileIntA("VR",
		"MovementOrientation", QUEST_MOVEMENT_ORIENTATION_HEAD,
		".\\vr_settings.ini"),
		(int)QUEST_MOVEMENT_ORIENTATION_BODY),
		(int)QUEST_MOVEMENT_ORIENTATION_COUNT-1);
	gQuestHeadBobbing = GetPrivateProfileIntA("VR", "HeadBobbing", 0,
		".\\vr_settings.ini") != 0;
	gQuestRefreshRateHz = Min(Max(GetPrivateProfileIntA("VR",
		"RefreshRate", 72, ".\\vr_settings.ini"), 72), 120);
	androidgame::SetPreferredRefreshRate(gQuestRefreshRateHz);
	const int persistedCpuPerformanceMode = GetPrivateProfileIntA("VR",
		"CpuPerformanceMode", QUEST_CPU_PERFORMANCE_SUSTAINED,
		".\\vr_settings.ini");
	// Boost is deliberately session-only. Old test builds may have persisted
	// value 2; treat that as Auto rather than re-entering Boost on startup.
	gQuestCpuPerformanceSavedMode =
		persistedCpuPerformanceMode >= QUEST_CPU_PERFORMANCE_AUTO &&
		persistedCpuPerformanceMode <= QUEST_CPU_PERFORMANCE_SUSTAINED ?
		persistedCpuPerformanceMode : QUEST_CPU_PERFORMANCE_SUSTAINED;
	gQuestCpuPerformanceMode = gQuestCpuPerformanceSavedMode;
	xrvk::setPerformanceMode(gQuestCpuPerformanceMode);
	gQuestGpuPerformanceMode = Min(Max(GetPrivateProfileIntA("VR",
		"GpuPerformanceMode", QUEST_CPU_PERFORMANCE_SUSTAINED,
		".\\vr_settings.ini"), (int)QUEST_CPU_PERFORMANCE_AUTO),
		(int)QUEST_CPU_PERFORMANCE_BOOST);
	xrvk::setGpuPerformanceMode(gQuestGpuPerformanceMode);
	gQuestTurnMode = Min(Max(GetPrivateProfileIntA("VR", "TurnMode",
		QUEST_TURN_SMOOTH, ".\\vr_settings.ini"),
		(int)QUEST_TURN_SMOOTH), (int)QUEST_TURN_MODE_COUNT-1);
	gQuestTurnSensitivityPercent = Min(Max(GetPrivateProfileIntA("VR",
		"TurnSensitivityPercent", 100, ".\\vr_settings.ini"), 25), 300);
	gQuestHeadSteeringSensitivityPercent = Min(Max(
		GetPrivateProfileIntA("VR", "HeadSteeringSensitivityPercent",
			50, ".\\vr_settings.ini"), 25), 300);
	gQuestSnapTurnAngleDegrees = Min(Max(GetPrivateProfileIntA("VR",
		"SnapTurnAngleDegrees", 30, ".\\vr_settings.ini"), 15), 90);
	gVrStickLookBehind = GetPrivateProfileIntA("VR", "StickLookBehind",
		0, ".\\vr_settings.ini") != 0 ? 1 : 0;
	gVrStickCrouch = GetPrivateProfileIntA("VR", "StickCrouch",
		0, ".\\vr_settings.ini") != 0 ? 1 : 0;
	const int defaultPedPercent = Min(Max(
		(int)(CIniFile::PedNumberMultiplier*100.0f+0.5f), 50), 300);
	// Vehicles stay at the stock count. reVC opens at 135% and it is too
	// much for a headset: a full street is the most expensive thing the
	// GPU draws, and the pedestrians carry the life of the place anyway.
	const int defaultCarPercent = 100;
	// Down to nothing at all. Half density was the old floor, which
	// left no way to actually empty a street -- for looking at one
	// car, or for measuring anything against a stable frame.
	gTrafficPedPercent = Min(Max(GetPrivateProfileIntA("VR",
		"PedTrafficPercent", defaultPedPercent,
		".\\vr_settings.ini"), 0), 300);
	gTrafficCarPercent = Min(Max(GetPrivateProfileIntA("VR",
		"CarTrafficPercent", defaultCarPercent,
		".\\vr_settings.ini"), 0), 300);
	LoadRagdollSettings();
	LoadVehicleDeformationSettings();
	QuestPhysicsDirectorSetMode(Min(Max(GetPrivateProfileIntA("VR",
		"PhysicsDirectorMode", QUEST_PHYSICS_DIRECTOR_ADAPTIVE,
		".\\vr_settings.ini"),
		(int)QUEST_PHYSICS_DIRECTOR_OFF),
		(int)QUEST_PHYSICS_DIRECTOR_MODE_COUNT-1));
	QuestPhysicsDirectorSetPreset(Min(Max(GetPrivateProfileIntA("VR",
		"PhysicsDirectorPreset", QUEST_PHYSICS_PRESET_QUALITY,
		".\\vr_settings.ini"),
		(int)QUEST_PHYSICS_PRESET_QUALITY),
		(int)QUEST_PHYSICS_PRESET_COUNT-1));
	QuestVehicleVisualBudgetSetMode(Min(Max(GetPrivateProfileIntA("VR",
		"VehicleVisualBudgetMode", QUEST_VEHICLE_VISUAL_STOCK,
		".\\vr_settings.ini"),
		(int)QUEST_VEHICLE_VISUAL_STOCK),
		(int)QUEST_VEHICLE_VISUAL_MODE_COUNT-1));
	gVrWelcomePending = GetPrivateProfileIntA("VR", "WelcomeShown", 0,
		".\\vr_settings.ini") == 0;
	gVrWelcomeFirstRun = gVrWelcomePending;
	// A binding out of range is an INI written by a different build; fall back
	// to the shipped assignment for that input rather than to no button at all.
	for(int source = 0; source < VR_PAD_SOURCE_COUNT; source++){
		const int target = GetPrivateProfileIntA("VR",
			kVrPadBindingKey[source], kVrPadBindingDefault[source],
			".\\vr_settings.ini");
		gVrPadBinding[source] =
			target >= VR_PAD_TARGET_NONE && target < VR_PAD_TARGET_COUNT ?
				target : kVrPadBindingDefault[source];
	}
	ApplyTrafficSettings();
	gVrSettingsLoaded = true;
}

static void
SaveViceCityColor(void)
{
	WritePrivateProfileStringA("VR", "ViceCityColor",
		gViceCityColorEnabled ? "1" : "0", ".\\vr_settings.ini");
}

static void
SaveFxaa(void)
{
	char value[8];
	snprintf(value, sizeof(value), "%d", gSpatialAaMode);
	WritePrivateProfileStringA("VR", "AntiAliasing", value,
	                         ".\\vr_settings.ini");
}

static void
SaveGameplayHud(void)
{
	WritePrivateProfileStringA("VR", "GameplayHud",
		gGameplayHudEnabled ? "1" : "0", ".\\vr_settings.ini");
}

// One key per cutscene name under its own section, so the list grows with
// whatever the player actually watches instead of shipping a table.
static void
LoadCutsceneCameraForScene(const char *scene)
{
	gCutsceneCamera = GetPrivateProfileIntA("VRCutsceneCamera",
		scene, 0, ".\\vr_settings.ini");
	if(gCutsceneCamera < 0)
		gCutsceneCamera = 0;
}

static void
SaveCutsceneCameraForScene(const char *scene)
{
	char value[16];
	snprintf(value, sizeof(value), "%d", gCutsceneCamera);
	WritePrivateProfileStringA("VRCutsceneCamera", scene, value,
		".\\vr_settings.ini");
}

static void
SaveWristPanels(void)
{
	WritePrivateProfileStringA("VR", "WristRadar",
		gWristPanelOn[WRIST_PANEL_MAP] ? "1" : "0", ".\\vr_settings.ini");
	WritePrivateProfileStringA("VR", "WristRadarUnderside",
		gWristPanelUnderside[WRIST_PANEL_MAP] ? "1" : "0",
		".\\vr_settings.ini");
	WritePrivateProfileStringA("VR", "WristRadarHand",
		gWristPanelHand[WRIST_PANEL_MAP] ? "1" : "0", ".\\vr_settings.ini");
	WritePrivateProfileStringA("VR", "WristStatus",
		gWristPanelOn[WRIST_PANEL_STATUS] ? "1" : "0", ".\\vr_settings.ini");
	WritePrivateProfileStringA("VR", "WristStatusUnderside",
		gWristPanelUnderside[WRIST_PANEL_STATUS] ? "1" : "0",
		".\\vr_settings.ini");
	WritePrivateProfileStringA("VR", "WristStatusHand",
		gWristPanelHand[WRIST_PANEL_STATUS] ? "1" : "0",
		".\\vr_settings.ini");
	WritePrivateProfileStringA("VR", "WristClock",
		gWristPanelOn[WRIST_PANEL_CLOCK] ? "1" : "0", ".\\vr_settings.ini");
	WritePrivateProfileStringA("VR", "WristClockUnderside",
		gWristPanelUnderside[WRIST_PANEL_CLOCK] ? "1" : "0",
		".\\vr_settings.ini");
	WritePrivateProfileStringA("VR", "WristClockHand",
		gWristPanelHand[WRIST_PANEL_CLOCK] ? "1" : "0", ".\\vr_settings.ini");
	WritePrivateProfileStringA("VR", "WristAmmo",
		gWristPanelOn[WRIST_PANEL_AMMO] ? "1" : "0", ".\\vr_settings.ini");
	WritePrivateProfileStringA("VR", "WristAmmoUnderside",
		gWristPanelUnderside[WRIST_PANEL_AMMO] ? "1" : "0",
		".\\vr_settings.ini");
	WritePrivateProfileStringA("VR", "WristAmmoHand",
		gWristPanelHand[WRIST_PANEL_AMMO] ? "1" : "0", ".\\vr_settings.ini");
	WritePrivateProfileStringA("VR", "WristPanelGazeReveal",
		gWristPanelGaze ? "1" : "0", ".\\vr_settings.ini");
	SaveVrInteger("WristPanelGazeRangeCm", gWristPanelGazeRangeCm);
	WritePrivateProfileStringA("VR", "WristPanelsInVehicle",
		gWristPanelsInVehicle ? "1" : "0", ".\\vr_settings.ini");
	static const char *const names[7] = {
		"Along", "Across", "Lift", "Pitch", "Yaw", "Roll", "Size"
	};
	for(int panel = 0; panel < WRIST_PANEL_COUNT; panel++)
		for(int context = 0; context < WRIST_CONTEXT_COUNT; context++){
			if(context != WRIST_CONTEXT_DEFAULT)
				WritePrivateProfileStringA("VR",
					WristContextKey(panel, context),
					gWristContextOwned[panel][context] ? "1" : "0",
					".\\vr_settings.ini");
			for(int side = 0; side < WRIST_SIDE_COUNT; side++){
				const int slot = WRIST_SLOT(context, side);
				const int values[7] = {
					gWristAlong[panel][slot], gWristAcross[panel][slot],
					gWristLift[panel][slot], gWristPitch[panel][slot],
					gWristYaw[panel][slot], gWristRoll[panel][slot],
					gWristSize[panel][slot]
				};
				for(int item = 0; item < 7; item++){
					char key[64], value[32];
					snprintf(key, sizeof(key), "Wrist%s%s%s%s",
						kWristPanelKey[panel],
						kWristContextKey[context],
						side == WRIST_SIDE_INNER ? "Inner" : "Outer",
						names[item]);
					snprintf(value, sizeof(value), "%d", values[item]);
					WritePrivateProfileStringA("VR", key, value,
						".\\vr_settings.ini");
				}
			}
		}
}

// IMMERSIVE puts what has to be read on the arms and drops the corner
// clutter; CLASSIC is the desktop game's interface, all of it, in front of
// the face. Classic is what a new player gets.
enum {
	VR_HUD_PRESET_IMMERSIVE = 0,
	VR_HUD_PRESET_CLASSIC,
	VR_HUD_PRESET_CUSTOM
};

static int
CurrentHudPreset(void)
{
	if(gWristPanelOn[WRIST_PANEL_MAP] && gWristPanelOn[WRIST_PANEL_STATUS] &&
	   gWristPanelOn[WRIST_PANEL_CLOCK] && gWristPanelsInVehicle &&
	   !gHudWeaponPanel && !gHudClock)
		return VR_HUD_PRESET_IMMERSIVE;
	if(!gWristPanelOn[WRIST_PANEL_MAP] && !gWristPanelOn[WRIST_PANEL_STATUS] &&
	   !gWristPanelOn[WRIST_PANEL_CLOCK] && !gWristPanelsInVehicle &&
	   gHudWeaponPanel && gHudClock)
		return VR_HUD_PRESET_CLASSIC;
	return VR_HUD_PRESET_CUSTOM;
}

static void
ApplyHudPreset(int preset)
{
	const bool immersive = preset == VR_HUD_PRESET_IMMERSIVE;
	gWristPanelOn[WRIST_PANEL_MAP] = immersive;
	gWristPanelOn[WRIST_PANEL_STATUS] = immersive;
	gWristPanelOn[WRIST_PANEL_CLOCK] = immersive;
	gWristPanelsInVehicle = immersive;
	gWristPanelGaze = immersive;
	gHudWeaponPanel = !immersive;
	gHudClock = !immersive;
	SaveWristPanels();
	SaveVrInteger("HudWeaponPanel", gHudWeaponPanel ? 1 : 0);
	SaveVrInteger("HudClock", gHudClock ? 1 : 0);
}

static void
SaveVrInteger(const char *key, int value)
{
	char text[16];
	snprintf(text, sizeof(text), "%d", value);
	WritePrivateProfileStringA("VR", key, text, ".\\vr_settings.ini");
}

#if MIAMIVR_STEAM_FRAME
static void
SetViewMode(int mode)
{
	mode = clamp(mode, (int)VR_VIEW_FIRST_PERSON, (int)VR_VIEW_THIRD_SHOULDER);
	if(mode == gViewMode)
		return;
	gViewMode = mode;
	if(mode != VR_VIEW_FIRST_PERSON){
		gThirdPersonStyle = mode;
		SaveVrInteger("ThirdPersonStyle", gThirdPersonStyle);
	}
	SaveVrInteger("ViewMode", gViewMode);
	// The anchor was latched against the old view's camera; relatching now
	// keeps the first frame of the new one from turning the world.
	androidgame::VrRecenterView();
	__android_log_print(ANDROID_LOG_INFO, "MiamiVR", "[vr] view mode -> %s",
		mode == VR_VIEW_THIRD_SCREEN ? "third person, 3D screen" :
		mode == VR_VIEW_THIRD_SHOULDER ? "third person, over shoulder" :
		"first person");
}
#endif

static int
VrControlsRowSource(int item)
{
	switch(item){
	case VR_CONTROLS_A: return androidgame::VR_PAD_SOURCE_A;
	case VR_CONTROLS_B: return androidgame::VR_PAD_SOURCE_B;
	case VR_CONTROLS_X: return androidgame::VR_PAD_SOURCE_X;
	case VR_CONTROLS_Y: return androidgame::VR_PAD_SOURCE_Y;
	case VR_CONTROLS_LEFT_GRIP: return androidgame::VR_PAD_SOURCE_LEFT_GRIP;
	case VR_CONTROLS_RIGHT_GRIP: return androidgame::VR_PAD_SOURCE_RIGHT_GRIP;
	case VR_CONTROLS_LEFT_STICK_CLICK:
		return androidgame::VR_PAD_SOURCE_LEFT_STICK_CLICK;
	case VR_CONTROLS_RIGHT_STICK_CLICK:
		return androidgame::VR_PAD_SOURCE_RIGHT_STICK_CLICK;
	}
	return -1;
}

static const char *
VrPadSourceName(int source)
{
	switch(source){
	case androidgame::VR_PAD_SOURCE_A: return "A BUTTON";
	case androidgame::VR_PAD_SOURCE_B: return "B BUTTON";
	case androidgame::VR_PAD_SOURCE_X: return "X BUTTON";
	case androidgame::VR_PAD_SOURCE_Y: return "Y BUTTON";
	case androidgame::VR_PAD_SOURCE_LEFT_GRIP: return "LEFT GRIP";
	case androidgame::VR_PAD_SOURCE_RIGHT_GRIP: return "RIGHT GRIP";
	case androidgame::VR_PAD_SOURCE_LEFT_STICK_CLICK: return "LEFT STICK CLICK";
	case androidgame::VR_PAD_SOURCE_RIGHT_STICK_CLICK:
		return "RIGHT STICK CLICK";
	}
	return "UNKNOWN";
}

// The action is what the default controller setup does with that pad button on
// foot; the pad button itself is spelled out beside it because the frontend can
// still change what it means.
static const char *
VrPadTargetName(int target)
{
	switch(target){
	case androidgame::VR_PAD_TARGET_SQUARE: return "JUMP [SQUARE]";
	case androidgame::VR_PAD_TARGET_CROSS: return "SPRINT [CROSS]";
	case androidgame::VR_PAD_TARGET_CIRCLE: return "ATTACK [CIRCLE]";
	case androidgame::VR_PAD_TARGET_TRIANGLE: return "ENTER / EXIT [TRIANGLE]";
	case androidgame::VR_PAD_TARGET_L1: return "PICK UP / RADIO [L1]";
	case androidgame::VR_PAD_TARGET_R1: return "TARGET [R1]";
	case androidgame::VR_PAD_TARGET_L2: return "PREVIOUS WEAPON [L2]";
	case androidgame::VR_PAD_TARGET_R2: return "NEXT WEAPON [R2]";
	case androidgame::VR_PAD_TARGET_L3: return "HORN / CROUCH [L3]";
	case androidgame::VR_PAD_TARGET_R3: return "LOOK BEHIND [R3]";
	}
	return "UNUSED";
}

static void
SavePadBinding(int source)
{
	SaveVrInteger(kVrPadBindingKey[source], gVrPadBinding[source]);
}

static const int kVrSwappedHandsBinding[androidgame::VR_PAD_SOURCE_COUNT] = {
	androidgame::VR_PAD_TARGET_SQUARE,    // A
	androidgame::VR_PAD_TARGET_TRIANGLE,  // B
	androidgame::VR_PAD_TARGET_CROSS,     // X
	androidgame::VR_PAD_TARGET_CIRCLE,    // Y
	androidgame::VR_PAD_TARGET_SQUARE,    // left trigger
	androidgame::VR_PAD_TARGET_CROSS,     // right trigger
	androidgame::VR_PAD_TARGET_L1,        // left grip
	androidgame::VR_PAD_TARGET_R1,        // right grip
	androidgame::VR_PAD_TARGET_L3,        // left stick click
	androidgame::VR_PAD_TARGET_R3         // right stick click
};

static bool
PadBindingsMatch(const int *layout)
{
	for(int source = 0; source < androidgame::VR_PAD_SOURCE_COUNT; source++)
		if(gVrPadBinding[source] != layout[source])
			return false;
	return true;
}

static int
CurrentControlsLayout(void)
{
	if(PadBindingsMatch(kVrPadBindingDefault))
		return VR_CONTROLS_LAYOUT_DEFAULT;
	if(PadBindingsMatch(kVrSwappedHandsBinding))
		return VR_CONTROLS_LAYOUT_SWAPPED_HANDS;
	return VR_CONTROLS_LAYOUT_CUSTOM;
}

static void
ApplyControlsLayout(const int *layout)
{
	for(int source = 0; source < androidgame::VR_PAD_SOURCE_COUNT; source++){
		gVrPadBinding[source] = layout[source];
		SavePadBinding(source);
	}
}

static void
CopyMenuText(char *destination, int capacity, const char *source)
{
	if(capacity <= 0)
		return;
	if(!source)
		source = "UNKNOWN";
	const int maxCharacters = capacity-1;
	int count = (int)strlen(source);
	if(count <= maxCharacters){
		memcpy(destination, source, count+1);
		return;
	}
	if(maxCharacters <= 3){
		destination[0] = '\0';
		return;
	}
	memcpy(destination, source, maxCharacters-3);
	memcpy(destination+maxCharacters-3, "...", 4);
}

static double
MonotonicMilliseconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec*1000.0+ts.tv_nsec/1e6;
}

static bool
MenuRepeatPulse(bool down, bool &wasDown, double &repeatAt,
	double &holdStart, double now,
                bool allowRepeat)
{
	if(!down){
		wasDown = false;
		repeatAt = 0.0;
		holdStart = 0.0;
		return false;
	}
	if(!wasDown){
		wasDown = true;
		holdStart = now;
		repeatAt = now+420.0;
		return true;
	}
	if(!allowRepeat)
		return false;
	if(now >= repeatAt){
		repeatAt = now+85.0;
		return true;
	}
	return false;
}

static int
MenuRepeatMagnitude(bool down, double holdStart, double now)
{
	if(!down || holdStart <= 0.0)
		return 1;
	const double held = now-holdStart;
	return held >= 3500.0 ? 10 : (held >= 1500.0 ? 3 : 1);
}

// Navigation needs two things at once: a deliberate tap must always move one
// row, while a held stick must eventually scroll long lists such as CHEATS.
// Separate engage/release thresholds prevent Touch-stick noise around a single
// threshold from manufacturing extra taps.
static int
MenuNavigationPulse(float axis, double now)
{
	const float engage = 0.68f;
	const float release = 0.34f;
	int requested = axis >= engage ? -1 : (axis <= -engage ? 1 : 0);

	if(gVrMenuNavigateDirection == 0){
		if(requested == 0)
			return 0;
		gVrMenuNavigateDirection = requested;
		gVrMenuNavigateRepeatAt = now+430.0;
		return requested;
	}

	if(Abs(axis) <= release){
		gVrMenuNavigateDirection = 0;
		gVrMenuNavigateRepeatAt = 0.0;
		return 0;
	}

	// A deliberate reversal is a new navigation edge even if the stick crossed
	// the centre too quickly for a rendered frame to observe the release band.
	if(requested != 0 && requested != gVrMenuNavigateDirection){
		gVrMenuNavigateDirection = requested;
		gVrMenuNavigateRepeatAt = now+430.0;
		return requested;
	}

	if(now >= gVrMenuNavigateRepeatAt){
		gVrMenuNavigateRepeatAt = now+110.0;
		return gVrMenuNavigateDirection;
	}
	return 0;
}

static void
ResetMenuNavigationRepeat(void)
{
	gVrMenuNavigateDirection = 0;
	gVrMenuNavigateRepeatAt = 0.0;
}

static void
OpenRagdollMenu(void)
{
	gVrMenuPage = VR_MENU_PAGE_RAGDOLL;
	gVrRagdollSelection = VR_RAGDOLL_ENABLED;
	ResetMenuNavigationRepeat();
}

static void
ReturnFromRagdollMenu(void)
{
	gVrMenuPage = VR_MENU_PAGE_TRAFFIC;
	gVrTrafficSelection = VR_TRAFFIC_RAGDOLLS;
	ResetMenuNavigationRepeat();
}

static void
AdjustRagdollMenuValue(int direction)
{
	switch(gVrRagdollSelection){
	case VR_RAGDOLL_ENABLED:
		gRagdolls = !VrRagdoll::IsEnabled();
		VrRagdoll::SetEnabled(gRagdolls);
		SaveVrInteger("Ragdolls", gRagdolls);
		break;
	case VR_RAGDOLL_BRAKING: {
		const int current = VrRagdoll::GetBrakePercent();
		const int step = current > 500 || (current == 500 && direction > 0) ? 100 : 25;
		const int next = current+direction*step;
		VrRagdoll::SetBrakePercent(current > 500 && direction < 0 ? Max(500,next) : next);
		SaveVrInteger("RagdollBrakePercent", VrRagdoll::GetBrakePercent());
		break;
	}
	case VR_RAGDOLL_GRIP:
		VrRagdoll::SetGripPercent(VrRagdoll::GetGripPercent()+direction*25);
		SaveVrInteger("RagdollGripPercent", VrRagdoll::GetGripPercent());
		break;
	case VR_RAGDOLL_WEIGHT:
		VrRagdoll::SetWeightPercent(VrRagdoll::GetWeightPercent()+direction*25);
		SaveVrInteger("RagdollWeightPercent", VrRagdoll::GetWeightPercent());
		break;
	case VR_RAGDOLL_SHOTS:
		VrRagdoll::SetShotPercent(VrRagdoll::GetShotPercent()+direction*25);
		SaveVrInteger("RagdollShotPercent", VrRagdoll::GetShotPercent());
		break;
	case VR_RAGDOLL_DEFAULTS:
		gRagdolls = 0;
		VrRagdoll::SetEnabled(false);
		SaveVrInteger("Ragdolls", 0);
		VrRagdoll::SetBrakePercent(200);
		VrRagdoll::SetGripPercent(150);
		VrRagdoll::SetShotPercent(100);
		VrRagdoll::SetWeightPercent(200);
		SaveVrInteger("RagdollBrakePercent", VrRagdoll::GetBrakePercent());
		SaveVrInteger("RagdollGripPercent", VrRagdoll::GetGripPercent());
		SaveVrInteger("RagdollShotPercent", VrRagdoll::GetShotPercent());
		SaveVrInteger("RagdollWeightPercent", VrRagdoll::GetWeightPercent());
		break;
	case VR_RAGDOLL_BACK:
		ReturnFromRagdollMenu();
		break;
	}
}

static void
OpenDeformationMenu(void)
{
	gVrMenuPage = VR_MENU_PAGE_DEFORMATION;
	gVrDeformationSelection = VR_DEFORMATION_ENABLED;
	ResetMenuNavigationRepeat();
}

static void
ReturnFromDeformationMenu(void)
{
	gVrMenuPage = VR_MENU_PAGE_TRAFFIC;
	gVrTrafficSelection = VR_TRAFFIC_VEHICLE_DEFORMATION;
	ResetMenuNavigationRepeat();
}

static void
SaveDeformationTuning(void)
{
	SaveVrInteger("VehicleDeformationStrengthPercent", VehicleDeformation::GetStrengthPercent());
	SaveVrInteger("VehicleDeformationRadiusPercent", VehicleDeformation::GetRadiusPercent());
	SaveVrInteger("VehicleDeformationMaxDentCm", VehicleDeformation::GetMaxDentCentimeters());
	SaveVrInteger("VehicleDeformationThresholdPercent", VehicleDeformation::GetThresholdPercent());
}

static void
AdjustDeformationMenuValue(int direction)
{
	switch(gVrDeformationSelection){
	case VR_DEFORMATION_ENABLED:
		VehicleDeformation::SetVehicleDeformationEnabled(!VehicleDeformation::IsVehicleDeformationEnabled());
		SaveVrInteger("VehicleDeformation", VehicleDeformation::IsVehicleDeformationEnabled() ? 1 : 0);
		break;
	case VR_DEFORMATION_STRENGTH:
		VehicleDeformation::SetStrengthPercent(VehicleDeformation::GetStrengthPercent()+direction*25);
		SaveVrInteger("VehicleDeformationStrengthPercent", VehicleDeformation::GetStrengthPercent());
		break;
	case VR_DEFORMATION_RADIUS:
		VehicleDeformation::SetRadiusPercent(VehicleDeformation::GetRadiusPercent()+direction*10);
		SaveVrInteger("VehicleDeformationRadiusPercent", VehicleDeformation::GetRadiusPercent());
		break;
	case VR_DEFORMATION_MAX_DENT:
		VehicleDeformation::SetMaxDentCentimeters(VehicleDeformation::GetMaxDentCentimeters()+direction*2);
		SaveVrInteger("VehicleDeformationMaxDentCm", VehicleDeformation::GetMaxDentCentimeters());
		break;
	case VR_DEFORMATION_THRESHOLD:
		VehicleDeformation::SetThresholdPercent(VehicleDeformation::GetThresholdPercent()+direction*25);
		SaveVrInteger("VehicleDeformationThresholdPercent", VehicleDeformation::GetThresholdPercent());
		break;
	case VR_DEFORMATION_DEFAULTS:
		VehicleDeformation::SetVehicleDeformationEnabled(false);
		SaveVrInteger("VehicleDeformation", 0);
		VehicleDeformation::ResetTuning();
		SaveDeformationTuning();
		break;
	case VR_DEFORMATION_BACK:
		ReturnFromDeformationMenu();
		break;
	}
}

static int
QuestWeaponSettingForWeaponItem(int item)
{
	switch(item){
	case VR_WEAPONS_HANDS: return 0;
	case VR_WEAPONS_LASER: return 1;
	case VR_WEAPONS_HOLSTER_HIGHLIGHTS: return 2;
	case VR_WEAPONS_MANUAL_RELOAD: return 3;
	case VR_WEAPONS_SCOPE_AIM: return 4;
	case VR_WEAPONS_GRIP_LOCK: return 5;
	default: return -1;
	}
}

static void
OpenCheatMenu(void)
{
	gVrMenuPage = VR_MENU_PAGE_CHEATS;
	gVrCheatCategory = -1;
	gVrCheatCategorySelection = 0;
	gVrCheatSelection = 0;
	gVrCheatStatusFrames = 0;
	ResetMenuNavigationRepeat();
}

static void
ReturnFromCheatMenu(void)
{
	if(gVrCheatCategory >= 0){
		gVrCheatCategorySelection = gVrCheatCategory;
		gVrCheatCategory = -1;
	}else{
		gVrMenuPage = VR_MENU_PAGE_SETTINGS;
		gVrMenuSelection = VR_MAIN_CHEATS;
	}
	gVrCheatStatusFrames = 0;
	ResetMenuNavigationRepeat();
}

static bool
CycleQuestCheatMenuSelection(int direction)
{
	const int source = VrCheatMenu::SourceIndex(gVrCheatCategory,
		gVrCheatSelection, GetVrCheatCount());
	if(source < 0 || !CycleVrCheatSelection(source, direction))
		return false;
	snprintf(gVrCheatStatus, sizeof(gVrCheatStatus), "MODEL SELECTED");
	gVrCheatStatusFrames = 60;
	return true;
}

static void
ActivateQuestCheatMenuSelection(void)
{
	if(gVrCheatCategory < 0){
		if(gVrCheatCategorySelection == VrCheatMenu::CATEGORY_COUNT){
			gVrMenuPage = VR_MENU_PAGE_MISSIONS;
			gVrMissionCategory = -1;
			gVrMissionCategorySelection = 0;
			gVrMissionSelection = 0;
		}else if(gVrCheatCategorySelection == VrCheatMenu::CATEGORY_COUNT+1){
			ReturnFromCheatMenu();
		}else if(gVrCheatCategorySelection >= 0 &&
		         gVrCheatCategorySelection < VrCheatMenu::CATEGORY_COUNT){
			gVrCheatCategory = gVrCheatCategorySelection;
			gVrCheatSelection = 0;
		}
		gVrCheatStatusFrames = 0;
		ResetMenuNavigationRepeat();
		return;
	}
	const int count = VrCheatMenu::Count(gVrCheatCategory, GetVrCheatCount());
	if(gVrCheatSelection == count){
		ReturnFromCheatMenu();
		return;
	}
	const int source = VrCheatMenu::SourceIndex(gVrCheatCategory,
		gVrCheatSelection, GetVrCheatCount());
	if(source < 0) return;
	const bool activated = ActivateVrCheat(source);
	snprintf(gVrCheatStatus, sizeof(gVrCheatStatus), "%s",
		activated ? "CHEAT ACTIVATED" : "UNAVAILABLE RIGHT NOW");
	gVrCheatStatusFrames = 120;
}

static int *
CurrentMenuSelection(void)
{
	switch(gVrMenuPage){
	case VR_MENU_PAGE_GRAPHICS: return &gVrGraphicsSelection;
	case VR_MENU_PAGE_LIGHTING: return &gVrLightingSelection;
	case VR_MENU_PAGE_WATER: return &gVrWaterSelection;
	case VR_MENU_PAGE_WEAPONS: return &gVrWeaponsSelection;
	case VR_MENU_PAGE_HUD: return &gVrHudSelection;
	case VR_MENU_PAGE_WRIST_RADAR: return &gVrWristRadarSelection;
	case VR_MENU_PAGE_TRAFFIC: return &gVrTrafficSelection;
	case VR_MENU_PAGE_RAGDOLL: return &gVrRagdollSelection;
	case VR_MENU_PAGE_DEFORMATION: return &gVrDeformationSelection;
	case VR_MENU_PAGE_MODEL_ASSETS: return &gVrModelAssetsSelection;
	case VR_MENU_PAGE_VEHICLE: return &gVrVehicleSelection;
	case VR_MENU_PAGE_VEHICLE_CALIBRATION:
		return &gVrVehicleCalibrationSelection;
	case VR_MENU_PAGE_LOCOMOTION: return &gVrLocomotionSelection;
	case VR_MENU_PAGE_CONTROLS: return &gVrControlsSelection;
	case VR_MENU_PAGE_CALIBRATION: return &gVrCalibrationSelection;
	case VR_MENU_PAGE_HOLSTERS: return &gVrHolsterSelection;
	case VR_MENU_PAGE_CHEATS:
		return gVrCheatCategory < 0 ?
			&gVrCheatCategorySelection : &gVrCheatSelection;
	case VR_MENU_PAGE_MISSIONS:
		return gVrMissionCategory < 0 ?
			&gVrMissionCategorySelection : &gVrMissionSelection;
	default: return &gVrMenuSelection;
	}
}

static int
CurrentMenuItemCount(void)
{
	switch(gVrMenuPage){
	case VR_MENU_PAGE_SETTINGS: return VR_MAIN_ITEM_COUNT;
	case VR_MENU_PAGE_GRAPHICS: return VR_GRAPHICS_ITEM_COUNT;
	case VR_MENU_PAGE_LIGHTING: return VR_LIGHTING_ITEM_COUNT;
	case VR_MENU_PAGE_WATER: return VR_WATER_ITEM_COUNT;
	case VR_MENU_PAGE_WEAPONS: return VR_WEAPONS_ITEM_COUNT;
	case VR_MENU_PAGE_HUD: return VR_HUD_ITEM_COUNT;
	case VR_MENU_PAGE_WRIST_RADAR: return VR_WRIST_ITEM_COUNT;
	case VR_MENU_PAGE_TRAFFIC: return VR_TRAFFIC_ITEM_COUNT;
	case VR_MENU_PAGE_RAGDOLL: return VR_RAGDOLL_ITEM_COUNT;
	case VR_MENU_PAGE_DEFORMATION: return VR_DEFORMATION_ITEM_COUNT;
	case VR_MENU_PAGE_MODEL_ASSETS: return VR_MODEL_ASSETS_ITEM_COUNT;
	case VR_MENU_PAGE_VEHICLE: return VR_VEHICLE_ITEM_COUNT;
	case VR_MENU_PAGE_VEHICLE_CALIBRATION:
		return Max(1,
			OculusVR::GetQuestVehicleCalibrationItemCount()+1);
	case VR_MENU_PAGE_LOCOMOTION: return VR_LOCOMOTION_ITEM_COUNT;
	case VR_MENU_PAGE_CONTROLS: return VR_CONTROLS_ITEM_COUNT;
	case VR_MENU_PAGE_CALIBRATION: return 22;
	case VR_MENU_PAGE_HOLSTERS:
		return OculusVR::GetQuestHolsterPointCount()+1;
	case VR_MENU_PAGE_CHEATS:
		return gVrCheatCategory < 0 ? VrCheatMenu::CATEGORY_COUNT+2 :
			VrCheatMenu::Count(gVrCheatCategory, GetVrCheatCount())+1;
	case VR_MENU_PAGE_MISSIONS:
		return Max(1, gVrMissionCategory < 0 ?
			GetVrMissionCategoryCount() :
			GetVrMissionCount(gVrMissionCategory));
	default: return 1;
	}
}

// Rows that cannot do anything in the current configuration are not drawn and
// cannot be reached: a wrist placement under CLASSIC, or a steering wheel
// under a DEFAULT view, is a control with nothing behind it.
static bool
IsMenuItemVisible(int page, int item)
{
	if(page == VR_MENU_PAGE_HUD){
		const int preset = CurrentHudPreset();
		switch(item){
		case VR_HUD_HORIZONTAL_SCALE:
		case VR_HUD_SCALE:
		case VR_HUD_OFFSET_X:
		case VR_HUD_OFFSET_Y:
		case VR_HUD_WEAPON_PANEL:
		case VR_HUD_CLOCK:
			return preset != VR_HUD_PRESET_IMMERSIVE;
		case VR_HUD_PANEL:
		case VR_HUD_PANEL_SHOW:
		case VR_HUD_PANEL_HAND:
		case VR_HUD_PANEL_SIDE:
		case VR_HUD_PANEL_GAZE:
		case VR_HUD_PANEL_CALIBRATE:
			return preset != VR_HUD_PRESET_CLASSIC;
		case VR_HUD_PANEL_GAZE_RANGE:
			// Nothing to set a range for while everything is always up.
			return preset != VR_HUD_PRESET_CLASSIC && gWristPanelGaze;
		case VR_HUD_WRIST_IN_VEHICLE:
			return preset != VR_HUD_PRESET_CLASSIC;
		}
		return true;
	}
	if(page == VR_MENU_PAGE_WRIST_RADAR){
		if(item == VR_WRIST_COLOUR_RED || item == VR_WRIST_COLOUR_GREEN ||
		   item == VR_WRIST_COLOUR_BLUE)
			return gVrWristPanelEdit == WRIST_PANEL_AMMO;
		if(gVrWristContextEdit == WRIST_CONTEXT_DEFAULT)
			return true;
		// A vehicle panel is bolted to the control centre. Its hand, the
		// wrist side it is worn on and the panel rotations have nothing to
		// act on there.
		switch(item){
		case VR_WRIST_CONTEXT:
		case VR_WRIST_ALONG:
		case VR_WRIST_ACROSS:
		case VR_WRIST_LIFT:
		case VR_WRIST_SIZE:
		case VR_WRIST_RESET:
		case VR_WRIST_BACK:
			return true;
		}
		return false;
	}
	if(page == VR_MENU_PAGE_VEHICLE){
		const int kind = OculusVR::GetQuestVehicleKind();
		const bool thirdPerson = OculusVR::IsQuestVehicleKindThirdPerson();
		const bool driven =
			!thirdPerson && !OculusVR::IsQuestVehicleKindDrivingDefault();
		const bool wheelKind = kind != 1;
		// A helm is the same wheel as a car's, so those rows serve both.
		const bool carDriven = driven && wheelKind;
		const bool bikeDriven = driven && !wheelKind;
		switch(item){
		case VR_VEHICLE_FORWARD_FIRE:
			return !driven && kind != 2;
		case VR_VEHICLE_WHEEL_VISIBLE:
		case VR_VEHICLE_MODEL_WHEEL_VISIBLE:
		case VR_VEHICLE_WHEEL_HAND_PULL_BACK:
			return carDriven;
		case VR_VEHICLE_BIKE_LOCK_HORIZON:
		case VR_VEHICLE_BIKE_THROTTLE:
		case VR_VEHICLE_BIKE_VISUAL_LEAN:
		case VR_VEHICLE_BIKE_VIEW_TILT:
		case VR_VEHICLE_BIKE_THROW_RIDER:
			return bikeDriven;
		case VR_VEHICLE_HANDLE_HIGHLIGHTS:
		case VR_VEHICLE_MOTION_HAND:
		case VR_VEHICLE_CALIBRATION:
			return carDriven || bikeDriven;
		case VR_VEHICLE_DEFAULT_SEAT_HEIGHT:
		case VR_VEHICLE_DEFAULT_SEAT_FORWARD:
			return OculusVR::HasQuestDefaultVehicleViewOffsetTarget();
		case VR_VEHICLE_GLOBAL_SEAT_HEIGHT:
		case VR_VEHICLE_GLOBAL_SEAT_FORWARD:
			return OculusVR::HasQuestVehicleSeatCalibrationTarget() &&
				!OculusVR::HasQuestDefaultVehicleViewOffsetTarget();
		case VR_VEHICLE_MODEL_SEAT_HEIGHT:
		case VR_VEHICLE_MODEL_SEAT_FORWARD:
			return OculusVR::HasQuestVehicleSeatCalibrationTarget();
		}
		return true;
	}
	return true;
}

static bool
CurrentMenuValueRepeats(void)
{
	if(gVrMenuPage == VR_MENU_PAGE_CALIBRATION)
		return gVrCalibrationSelection >= 1 &&
			gVrCalibrationSelection <= 18;
	if(gVrMenuPage == VR_MENU_PAGE_HUD)
		return gVrHudSelection >= VR_HUD_HORIZONTAL_SCALE &&
			gVrHudSelection <= VR_HUD_OFFSET_Y;
	if(gVrMenuPage == VR_MENU_PAGE_WRIST_RADAR)
		return gVrWristRadarSelection >= VR_WRIST_ALONG &&
			gVrWristRadarSelection <= VR_WRIST_SIZE;
	if(gVrMenuPage == VR_MENU_PAGE_TRAFFIC)
		return gVrTrafficSelection == VR_TRAFFIC_PEDESTRIANS ||
			gVrTrafficSelection == VR_TRAFFIC_VEHICLES;
	if(gVrMenuPage == VR_MENU_PAGE_RAGDOLL)
		return gVrRagdollSelection >= VR_RAGDOLL_BRAKING &&
			gVrRagdollSelection <= VR_RAGDOLL_SHOTS;
	if(gVrMenuPage == VR_MENU_PAGE_DEFORMATION)
		return gVrDeformationSelection >= VR_DEFORMATION_STRENGTH &&
			gVrDeformationSelection <= VR_DEFORMATION_THRESHOLD;
	if(gVrMenuPage == VR_MENU_PAGE_LIGHTING)
		return gVrLightingSelection == VR_LIGHTING_INTENSITY ||
			gVrLightingSelection == VR_LIGHTING_GLOW_INTENSITY ||
			gVrLightingSelection == VR_LIGHTING_REFLECTION_INTENSITY ||
			gVrLightingSelection == VR_LIGHTING_REFLECTION_SSR ||
			gVrLightingSelection == VR_LIGHTING_SSR_DISTANCE;
	if(gVrMenuPage == VR_MENU_PAGE_WATER)
		return gVrWaterSelection >= VR_WATER_WAVES &&
			gVrWaterSelection <= VR_WATER_SPARKS;
	if(gVrMenuPage == VR_MENU_PAGE_VEHICLE)
		return gVrVehicleSelection >= VR_VEHICLE_DEFAULT_SEAT_HEIGHT &&
			gVrVehicleSelection <= VR_VEHICLE_MODEL_SEAT_FORWARD;
	if(gVrMenuPage == VR_MENU_PAGE_VEHICLE_CALIBRATION){
		const int row = gVrVehicleCalibrationSelection;
		const int item = OculusVR::GetQuestVehicleCalibrationItemForRow(row);
		return row < OculusVR::GetQuestVehicleCalibrationItemCount() &&
			item != OculusVR::QUEST_VEHICLE_CAL_HAND;
	}
	if(gVrMenuPage == VR_MENU_PAGE_LOCOMOTION)
		return gVrLocomotionSelection ==
			VR_LOCOMOTION_TURN_SENSITIVITY;
	return false;
}

static void
ReturnFromCurrentMenuPage(void)
{
	if(gVrMenuPage == VR_MENU_PAGE_CHEATS){
		ReturnFromCheatMenu();
		return;
	}
	if(gVrMenuPage == VR_MENU_PAGE_RAGDOLL){
		ReturnFromRagdollMenu();
		return;
	}
	if(gVrMenuPage == VR_MENU_PAGE_DEFORMATION){
		ReturnFromDeformationMenu();
		return;
	}
	if(gVrMenuPage == VR_MENU_PAGE_SETTINGS){
		gVrMenuVisible = false;
		return;
	}
	if(gVrMenuPage == VR_MENU_PAGE_MISSIONS){
		if(gVrMissionCategory >= 0)
			gVrMissionCategory = -1;
		else{
			gVrMenuPage = VR_MENU_PAGE_CHEATS;
			gVrCheatCategory = -1;
			gVrCheatCategorySelection = VrCheatMenu::CATEGORY_COUNT;
		}
		gVrCheatStatusFrames = 0;
		ResetMenuNavigationRepeat();
		return;
	}
	if(gVrMenuPage == VR_MENU_PAGE_VEHICLE_CALIBRATION){
		OculusVR::SetQuestVehicleCalibrationPreview(false);
		gVrMenuPage = VR_MENU_PAGE_VEHICLE;
		return;
	}
	if(gVrMenuPage == VR_MENU_PAGE_WRIST_RADAR){
		gVrMenuPage = VR_MENU_PAGE_HUD;
		return;
	}
	if(gVrMenuPage == VR_MENU_PAGE_LIGHTING){
		gVrMenuPage = VR_MENU_PAGE_GRAPHICS;
		return;
	}
	if(gVrMenuPage == VR_MENU_PAGE_WATER){
		gVrMenuPage = VR_MENU_PAGE_LIGHTING;
		return;
	}
	gVrMenuPage = VR_MENU_PAGE_SETTINGS;
	gVrCheatStatusFrames = 0;
}

static bool
AnyVrAboutButtonDown(const PadInput &in)
{
	return in.a || in.b || in.x || in.y || in.menu ||
		in.leftStickClick || in.rightStickClick ||
		in.leftTrigger >= 0.55f || in.rightTrigger >= 0.55f ||
		in.leftGrip >= 0.65f || in.rightGrip >= 0.65f;
}

static bool
DidPlayerTakeWelcomeStep(const PadInput &in)
{
	CPlayerPed *player = FindPlayerPed();
	if(!player || gGameState != GS_PLAYING_GAME ||
	   FrontEndMenuManager.m_bMenuActive || CCutsceneMgr::IsRunning() ||
	   FindPlayerVehicle()){
		gVrWelcomeBaselineValid = false;
		return false;
	}
	const CVector position = player->GetPosition();
	if(!gVrWelcomeBaselineValid){
		gVrWelcomeBaseline = position;
		gVrWelcomeBaselineValid = true;
		return false;
	}
	const bool movementInput = Abs(in.leftStickX) >= 0.20f ||
		Abs(in.leftStickY) >= 0.20f;
	const CVector moved = position-gVrWelcomeBaseline;
	return movementInput && moved.x*moved.x+moved.y*moved.y >= 0.0025f;
}

// Runs once per rendered frame from the pad path. All menu state changes are
// edge/repeat driven so a tap remains one exact step while a held trigger can
// traverse large calibration ranges without hundreds of clicks.
void
VrDebugUpdate(const PadInput &in)
{
	LoadVrSettings();
	gQuestCpuPerformanceMode = Min(Max(xrvk::getPerformanceMode(),
		(int)QUEST_CPU_PERFORMANCE_AUTO),
		(int)QUEST_CPU_PERFORMANCE_COUNT-1);
	// The VR menu is available before CGame::Initialise loads gta3.ini. Keep
	// the persisted headset values authoritative after that later load too.
	ApplyTrafficSettings();
	const double now = MonotonicMilliseconds();
	if(gVrWelcomePending && DidPlayerTakeWelcomeStep(in)){
		gVrWelcomePending = false;
		gVrMenuVisible = true;
		gVrMenuPage = VR_MENU_PAGE_ABOUT;
		gVrWelcomeFirstRun = true;
		gVrAboutDismissArmed = false;
		gVrAboutReleaseGate = false;
	}
	if(gVrMenuVisible && gVrMenuPage == VR_MENU_PAGE_ABOUT){
		const bool button = AnyVrAboutButtonDown(in);
		if(!button)
			gVrAboutDismissArmed = true;
		else if(gVrAboutDismissArmed){
			gVrMenuVisible = false;
			gVrMenuPage = VR_MENU_PAGE_SETTINGS;
			gVrAboutDismissArmed = false;
			gVrAboutReleaseGate = true;
			if(gVrWelcomeFirstRun){
				WritePrivateProfileStringA("VR", "WelcomeShown", "1",
					".\\vr_settings.ini");
				gVrWelcomeFirstRun = false;
			}
		}
		return;
	}
	if(gVrAboutReleaseGate){
		if(!AnyVrAboutButtonDown(in))
			gVrAboutReleaseGate = false;
		return;
	}
	const bool bothGrips =
		in.leftGrip >= 0.75f && in.rightGrip >= 0.75f;
	// As on desktop, service shortcuts other than the settings panel must not
	// steal B/A while the player is holding a physical gun or detonator.
	const bool modifier = bothGrips &&
		OculusVR::GetHeldWeaponSlot(0) < 0 &&
		OculusVR::GetHeldWeaponSlot(1) < 0 &&
		!OculusVR::IsTrackedDetonatorActive(0) &&
		!OculusVR::IsTrackedDetonatorActive(1);
	// Match the desktop service chords.  Menu remains the primary shortcut,
	// while both triggers + X is the fallback for controllers which do not
	// expose a usable Menu action to the application.
	const bool alternateMenuShortcut = bothGrips &&
		in.leftTrigger >= 0.75f && in.rightTrigger >= 0.75f &&
		(in.x || in.leftStickClick);
	const bool menuShortcut =
		(bothGrips && in.menu) || alternateMenuShortcut;
	if(menuShortcut && !gVrMenuShortcutDown){
		if(gVrMenuVisible &&
		   gVrMenuPage == VR_MENU_PAGE_VEHICLE_CALIBRATION)
			OculusVR::SetQuestVehicleCalibrationPreview(false);
		gVrMenuVisible = !gVrMenuVisible;
		if(gVrMenuVisible)
			OculusVR::InvalidateQuestWeaponLaserOverrides();
		gVrMenuPage = VR_MENU_PAGE_SETTINGS;
		gVrMenuSelection = 0;
		gVrMenuSelectDown = false;
		gVrMenuBackDown = in.b || in.leftStickClick;
		ResetMenuNavigationRepeat();
		// The fallback chord holds both triggers.  Seed their latches so
		// opening the panel cannot also change its first value.
		gVrMenuIncreaseDown = in.rightTrigger >= 0.55f;
		gVrMenuDecreaseDown = in.leftTrigger >= 0.55f;
		gVrMenuIncreaseRepeatAt = 0.0;
		gVrMenuDecreaseRepeatAt = 0.0;
	}
	gVrMenuShortcutDown = menuShortcut;

	// Keep B available for drive-by shooting; latch the chord during driving.
	const bool cheatShortcut = modifier && in.b;
	if(cheatShortcut && !gVrCheatShortcutDown && !FindPlayerVehicle()){
		const bool closingCheats =
			gVrMenuVisible && (gVrMenuPage == VR_MENU_PAGE_CHEATS ||
			gVrMenuPage == VR_MENU_PAGE_MISSIONS);
		if(gVrMenuVisible &&
		   gVrMenuPage == VR_MENU_PAGE_VEHICLE_CALIBRATION)
			OculusVR::SetQuestVehicleCalibrationPreview(false);
		gVrMenuVisible = !closingCheats;
		if(gVrMenuVisible)
			OculusVR::InvalidateQuestWeaponLaserOverrides();
		OpenCheatMenu();
		gVrMenuSelectDown = false;
		gVrMenuBackDown = in.b || in.leftStickClick;
		ResetMenuNavigationRepeat();
		gVrMenuIncreaseDown = false;
		gVrMenuDecreaseDown = false;
		gVrMenuIncreaseRepeatAt = 0.0;
		gVrMenuDecreaseRepeatAt = 0.0;
	}
	gVrCheatShortcutDown = cheatShortcut;

	// A stereo cutscene owns the stick clicks while it runs: R3 steps to
	// the next camera, L3 keeps the one on screen for this scene. The
	// scene name is the key, so the choice comes back the next time the
	// same cutscene plays.
	const bool cutsceneCameras = !gVrMenuVisible &&
		gCutsceneMode == VR_CUTSCENE_STEREO &&
		(CCutsceneMgr::IsRunning() || TheCamera.m_WideScreenOn);
	if(cutsceneCameras){
		// A scripted sequence has no name of its own, so every one of
		// them shares a key. They are rare and they are all the same
		// kind of shot.
		const char *scene = CCutsceneMgr::IsRunning() ?
			CCutsceneMgr::GetCutsceneName() : "scripted";
		if(scene != nil &&
		   strncmp(scene, gCutsceneCameraScene,
		     sizeof(gCutsceneCameraScene)-1) != 0){
			strncpy(gCutsceneCameraScene, scene,
				sizeof(gCutsceneCameraScene)-1);
			gCutsceneCameraScene[sizeof(gCutsceneCameraScene)-1] = '\0';
			LoadCutsceneCameraForScene(gCutsceneCameraScene);
		}
		const bool cycle = in.rightStickClick && !in.leftStickClick;
		const bool store = in.leftStickClick && !in.rightStickClick;
		if(cycle && !gCutsceneCycleDown){
			// One past the last actor wraps back to the director.
			const int cameras = androidgame::VrCutsceneCameraCount();
			gCutsceneCamera = cameras > 0 ?
				(gCutsceneCamera+1)%cameras : 0;
		}
		if(store && !gCutsceneStoreDown &&
		   gCutsceneCameraScene[0] != '\0')
			SaveCutsceneCameraForScene(gCutsceneCameraScene);
		gCutsceneCycleDown = cycle;
		gCutsceneStoreDown = store;
	}else{
		gCutsceneCycleDown = false;
		gCutsceneStoreDown = false;
		gCutsceneCameraScene[0] = '\0';
	}

	// Both stick clicks rebuild the gameplay reference space, the desktop
	// chord's job. No grips in it: reaching for one picks a weapon up. The
	// clicks are free for it because R3 and L3 hand their pad behaviour to
	// CONTROLS, off by default.
#if MIAMIVR_STEAM_FRAME
	// The Frame recentres on a View hold (android_main.cpp). Stick clicks
	// stay game actions there, so the chord is off.
	const bool recenterShortcut = false;
#else
	const bool recenterShortcut =
		!gVrMenuVisible && !VrShouldUseTheaterMode() && !cutsceneCameras &&
		in.leftStickClick && in.rightStickClick;
#endif
	if(recenterShortcut && !gTouchRecenterShortcutDown)
		androidgame::VrRecenterView();
	gTouchRecenterShortcutDown = recenterShortcut;

#if MIAMIVR_STEAM_FRAME
	// View's short press swaps first person and the chosen third person style
	// during play. The menu, the frontend and cutscenes already own that press
	// as "back" (or have no use for it), so it only counts when none of them
	// is up. gVrMenuVisible is read before this frame's menu code can close
	// the menu with the same press.
	if(in.viewBack && !gVrMenuVisible && gGameState == GS_PLAYING_GAME &&
	   !FrontEndMenuManager.m_bMenuActive &&
	   !FrontEndMenuManager.m_bGameNotLoaded &&
	   !FrontEndMenuManager.m_bWantToRestart &&
	   !FrontEndMenuManager.m_bWantToLoad &&
	   !CGame::playingIntro && FindPlayerPed() != nil &&
	   !CCutsceneMgr::IsRunning() && !CCutsceneMgr::IsCutsceneProcessing() &&
	   !TheCamera.m_WideScreenOn)
		androidgame::VrToggleThirdPerson();
#endif

	// The profiler had a chord of its own, both grips and Y. Both grips are
	// how a weapon is picked up, so players kept summoning the counters by
	// accident and could not read the game through them. It is a developer
	// tool with a row on the GRAPHICS page; that is route enough.

	// Armed from the page itself rather than cleared on every exit: the menu
	// can be left in half a dozen ways and each one would have to remember.
	OculusVR::SetQuestWeaponCalibrationPreview(
		gVrMenuVisible && gVrMenuPage == VR_MENU_PAGE_CALIBRATION);
	if(gVrMenuVisible){
		if(gVrMenuPage == VR_MENU_PAGE_VEHICLE_CALIBRATION &&
		   !OculusVR::IsQuestVehicleCalibrationAvailable()){
			OculusVR::SetQuestVehicleCalibrationPreview(false);
			gVrMenuPage = VR_MENU_PAGE_VEHICLE;
		}
		const int itemCount = CurrentMenuItemCount();
		const int navigationPulse = MenuNavigationPulse(in.leftStickY, now);
		if(navigationPulse != 0 && itemCount > 0){
			int *selection = CurrentMenuSelection();
			int guard = itemCount;
			do{
				*selection =
					(*selection+navigationPulse+itemCount)%itemCount;
			}while(!IsMenuItemVisible(gVrMenuPage, *selection) &&
			       --guard > 0);
		}
		const bool cheatCycle = gVrMenuPage == VR_MENU_PAGE_CHEATS &&
			(Abs(in.leftStickX) >= 0.65f);
		if(cheatCycle && !gVrCheatCycleDown){
			const int direction = in.leftStickX > 0.0f ? 1 : -1;
			CycleQuestCheatMenuSelection(direction);
		}
		gVrCheatCycleDown = cheatCycle;

		const bool select = in.a || in.rightStickClick;
		const bool selectPulse = select && !gVrMenuSelectDown;
		gVrMenuSelectDown = select;
		const bool repeatValue = CurrentMenuValueRepeats();
		const bool increasePulse = MenuRepeatPulse(
			in.rightTrigger >= 0.55f, gVrMenuIncreaseDown,
			gVrMenuIncreaseRepeatAt, gVrMenuIncreaseHoldStart,
			now, repeatValue);
		const bool decreasePulse = MenuRepeatPulse(
			in.leftTrigger >= 0.55f, gVrMenuDecreaseDown,
			gVrMenuDecreaseRepeatAt, gVrMenuDecreaseHoldStart,
			now, repeatValue);
		const int repeatMagnitude = decreasePulse ?
			MenuRepeatMagnitude(in.leftTrigger >= 0.55f,
				gVrMenuDecreaseHoldStart, now) :
			MenuRepeatMagnitude(in.rightTrigger >= 0.55f,
				gVrMenuIncreaseHoldStart, now);
		const bool positivePulse = selectPulse || increasePulse;

		if(gVrMenuPage == VR_MENU_PAGE_SETTINGS &&
		   (positivePulse || decreasePulse)){
			if(gVrMenuSelection == VR_MAIN_GRAPHICS){
				gVrMenuPage = VR_MENU_PAGE_GRAPHICS;
				gVrGraphicsSelection = 0;
			}else if(gVrMenuSelection == VR_MAIN_HUD){
				gVrMenuPage = VR_MENU_PAGE_HUD;
				gVrHudSelection = 0;
			}else if(gVrMenuSelection == VR_MAIN_TRAFFIC_SETTINGS){
				gVrMenuPage = VR_MENU_PAGE_TRAFFIC;
				gVrTrafficSelection = 0;
			}else if(gVrMenuSelection == VR_MAIN_MODEL_ASSETS){
				gVrMenuPage = VR_MENU_PAGE_MODEL_ASSETS;
				gVrModelAssetsSelection = 0;
			}else if(gVrMenuSelection == VR_MAIN_VEHICLE_SETTINGS){
				gVrMenuPage = VR_MENU_PAGE_VEHICLE;
				gVrVehicleSelection = 0;
			}else if(gVrMenuSelection == VR_MAIN_LOCOMOTION_SETTINGS){
				gVrMenuPage = VR_MENU_PAGE_LOCOMOTION;
				gVrLocomotionSelection = 0;
			}else if(gVrMenuSelection == VR_MAIN_CONTROLS){
				gVrMenuPage = VR_MENU_PAGE_CONTROLS;
				gVrControlsSelection = 0;
			}else if(gVrMenuSelection == VR_MAIN_WEAPONS){
				gVrMenuPage = VR_MENU_PAGE_WEAPONS;
				gVrWeaponsSelection = 0;
			}else if(gVrMenuSelection == VR_MAIN_HOLSTERS){
				gVrMenuPage = VR_MENU_PAGE_HOLSTERS;
				gVrHolsterSelection = 0;
			}else if(gVrMenuSelection == VR_MAIN_CHEATS){
				OpenCheatMenu();
			}else if(gVrMenuSelection == VR_MAIN_ABOUT){
				gVrMenuPage = VR_MENU_PAGE_ABOUT;
				gVrWelcomeFirstRun = false;
				gVrWelcomePending = false;
				gVrAboutDismissArmed = false;
			}
		}else if(gVrMenuPage == VR_MENU_PAGE_GRAPHICS &&
		         (positivePulse || decreasePulse)){
			if(gVrGraphicsSelection == VR_GRAPHICS_RENDER_SCALE){
				static const int scales[] = { 100, 125, 150, 175 };
				int index = 0;
				for(int i = 0; i < (int)ARRAY_SIZE(scales); i++)
					if(scales[i] == gQuestRenderScalePercent)
						index = i;
				const int direction = decreasePulse ? -1 : 1;
				index = (index+direction+(int)ARRAY_SIZE(scales)) %
					(int)ARRAY_SIZE(scales);
				gQuestRenderScalePercent = scales[index];
				SaveVrInteger("RenderScalePercent", gQuestRenderScalePercent);
			}else if(gVrGraphicsSelection == VR_GRAPHICS_SGSR){
				// Reserved for a future stereo-stable temporal AA path.
			}else if(gVrGraphicsSelection == VR_GRAPHICS_MSAA){
				// Off, 2x, 4x. It smooths the geometry it was made
				// for -- railings, poles, wires. Masks are left to the
				// mip chain; turning them into coverage instead put a
				// dither lattice across every leaf and neon tube.
				gQuestMsaaSamples = gQuestMsaaSamples >= 4 ? 1 :
					gQuestMsaaSamples >= 2 ? 4 : 2;
				SaveVrInteger("MsaaSamples", gQuestMsaaSamples);
			}else if(gVrGraphicsSelection == VR_GRAPHICS_FOLIAGE){
				gFoliageSoftness = (gFoliageSoftness+4+
					(decreasePulse ? -1 : 1))%4;
				SaveVrInteger("FoliageSoftness", gFoliageSoftness);
			}else if(gVrGraphicsSelection == VR_GRAPHICS_DIAGNOSTICS){
				gRenderDiagnostics = !gRenderDiagnostics;
				SaveVrInteger("RenderDiagnostics", gRenderDiagnostics);
				rw::vulkan::setRenderDiagnostics(gRenderDiagnostics);
			}else if(gVrGraphicsSelection == VR_GRAPHICS_PS2_ALPHA){
				gPs2AlphaTest = !gPs2AlphaTest;
				SaveVrInteger("Ps2AlphaTest", gPs2AlphaTest);
				rw::vulkan::setPs2AlphaTestEnabled(gPs2AlphaTest);
			}else if(gVrGraphicsSelection == VR_GRAPHICS_MIPMAPS){
				gGenerateMipmaps = !gGenerateMipmaps;
				SaveVrInteger("GenerateMipmaps", gGenerateMipmaps);
				rw::vulkan::setGenerateMipmaps(gGenerateMipmaps);
			}else if(gVrGraphicsSelection == VR_GRAPHICS_FXAA){
				gSpatialAaMode = !gSpatialAaMode;
				SaveFxaa();
			}else if(gVrGraphicsSelection == VR_GRAPHICS_COLOR){
				gViceCityColorEnabled = !gViceCityColorEnabled;
				SaveViceCityColor();
			}else if(gVrGraphicsSelection == VR_GRAPHICS_PROFILER){
				QuestProfilerSetEnabled(!QuestProfilerIsEnabled());
				SaveVrInteger("ShowProfiler",
					QuestProfilerIsEnabled() ? 1 : 0);
			}else if(gVrGraphicsSelection == VR_GRAPHICS_CPU_PERFORMANCE){
				if(xrvk::isPerformanceModeSupported()){
					const int direction = decreasePulse ? -1 : 1;
					gQuestCpuPerformanceMode =
						(gQuestCpuPerformanceMode+direction+
						 QUEST_CPU_PERFORMANCE_COUNT)%
						QUEST_CPU_PERFORMANCE_COUNT;
					if(gQuestCpuPerformanceMode <=
					   QUEST_CPU_PERFORMANCE_SUSTAINED){
						gQuestCpuPerformanceSavedMode =
							gQuestCpuPerformanceMode;
						SaveVrInteger("CpuPerformanceMode",
							gQuestCpuPerformanceSavedMode);
					}
					xrvk::setPerformanceMode(gQuestCpuPerformanceMode);
				}
			}else if(gVrGraphicsSelection == VR_GRAPHICS_GPU_PERFORMANCE){
				if(xrvk::isPerformanceModeSupported()){
					const int direction = decreasePulse ? -1 : 1;
					gQuestGpuPerformanceMode =
						(gQuestGpuPerformanceMode+direction+
						 QUEST_CPU_PERFORMANCE_COUNT)%
						QUEST_CPU_PERFORMANCE_COUNT;
					SaveVrInteger("GpuPerformanceMode",
					              gQuestGpuPerformanceMode);
					xrvk::setGpuPerformanceMode(gQuestGpuPerformanceMode);
				}
			}else if(gVrGraphicsSelection == VR_GRAPHICS_SHADOWS){
				CShadows::SetRenderEnabled(!CShadows::IsRenderEnabled());
				SaveVrInteger("ShadowsEnabled",
					CShadows::IsRenderEnabled() ? 1 : 0);
			}else if(gVrGraphicsSelection == VR_GRAPHICS_DYNAMIC_LIGHTS){
				gVrMenuPage = VR_MENU_PAGE_LIGHTING;
				gVrLightingSelection = 0;
			}else if(gVrGraphicsSelection == VR_GRAPHICS_OCCLUSION){
				const int direction = decreasePulse ? -1 : 1;
				gOcclusionCullingMode =
					(gOcclusionCullingMode+direction+
					 VR_OCCLUSION_CULLING_MODE_COUNT)%
					VR_OCCLUSION_CULLING_MODE_COUNT;
				CRenderer::SetVrOcclusionCullingMode(gOcclusionCullingMode);
				SaveVrInteger("OcclusionCullingModeV2", gOcclusionCullingMode);
				// Keep older builds able to honour the explicit OFF fallback.
				SaveVrInteger("OcclusionCulling",
					gOcclusionCullingMode != VR_OCCLUSION_CULLING_OFF ? 1 : 0);
			}else if(gVrGraphicsSelection == VR_GRAPHICS_FOUNTAIN){
				const int direction = decreasePulse ? -1 : 1;
				const int quality =
					(CParticleObject::GetVrFountainQuality()+direction+
					 VR_FOUNTAIN_QUALITY_COUNT)%VR_FOUNTAIN_QUALITY_COUNT;
				CParticleObject::SetVrFountainQuality(quality);
				SaveVrInteger("FountainQuality", quality);
			}else if(gVrGraphicsSelection == VR_GRAPHICS_FOG){
				gDistanceFog = !gDistanceFog;
				SaveVrInteger("DistanceFog", gDistanceFog ? 1 : 0);
#if defined(MIAMIVR_DEV_TOOLS) && MIAMIVR_DEV_TOOLS
			}else if(gVrGraphicsSelection == VR_GRAPHICS_CULL_VIZ){
				VrCullViz::Toggle();
			}else if(gVrGraphicsSelection == VR_GRAPHICS_QUICK_START){
				gQuestQuickTestStart = !gQuestQuickTestStart;
				SaveVrInteger("QuickTestStart",
					gQuestQuickTestStart ? 1 : 0);
#endif
			}else if(gVrGraphicsSelection == VR_GRAPHICS_BACK){
				gVrMenuPage = VR_MENU_PAGE_SETTINGS;
			}
		}else if(gVrMenuPage == VR_MENU_PAGE_WEAPONS &&
		         (positivePulse || decreasePulse)){
			const int weaponSetting =
				QuestWeaponSettingForWeaponItem(gVrWeaponsSelection);
			if(weaponSetting >= 0 && weaponSetting <
			   OculusVR::GetQuestWeaponSettingCount())
				OculusVR::ToggleQuestWeaponSetting(weaponSetting);
			else if(gVrWeaponsSelection == VR_WEAPONS_CALIBRATION){
				gVrMenuPage = VR_MENU_PAGE_CALIBRATION;
				gVrCalibrationSelection = 0;
			}else if(gVrWeaponsSelection == VR_WEAPONS_BACK){
				gVrMenuPage = VR_MENU_PAGE_SETTINGS;
			}
		}else if(gVrMenuPage == VR_MENU_PAGE_MODEL_ASSETS &&
		         (positivePulse || decreasePulse)){
			const int direction = decreasePulse ? -1 : 1;
			if(gVrModelAssetsSelection == VR_MODEL_ASSETS_PRESET){
				ModelSets::CycleRequested(direction);
				const ModelSets::eModelSet preset = ModelSets::GetRequested();
				for(int category = 0; category < ModelSets::MODEL_CATEGORY_COUNT;
				    category++){
					const ModelSets::eModelSet requested =
						preset == ModelSets::MODEL_SET_MODERN &&
						(category == ModelSets::MODEL_CATEGORY_WORLD ||
						 category == ModelSets::MODEL_CATEGORY_WEAPONS) ?
						ModelSets::MODEL_SET_MODERN :
						(preset == ModelSets::MODEL_SET_XBOX &&
						 category == ModelSets::MODEL_CATEGORY_VEHICLES ?
						 ModelSets::MODEL_SET_XBOX : ModelSets::MODEL_SET_CLASSIC);
					ModelSets::SetRequestedForCategory(
						(ModelSets::eModelCategory)category, requested);
				}
			}else if(gVrModelAssetsSelection >= VR_MODEL_ASSETS_WORLD &&
			         gVrModelAssetsSelection <= VR_MODEL_ASSETS_WEAPONS){
				ModelSets::CycleRequestedCategory(
					(ModelSets::eModelCategory)(gVrModelAssetsSelection-1),
					direction);
			}else if(gVrModelAssetsSelection == VR_MODEL_ASSETS_BACK &&
			         positivePulse)
				gVrMenuPage = VR_MENU_PAGE_SETTINGS;
		}else if(gVrMenuPage == VR_MENU_PAGE_LIGHTING &&
		         (positivePulse || decreasePulse)){
			const int direction = decreasePulse ? -1 : 1;
			const int step = direction*Max(1, repeatMagnitude);
			if(gVrLightingSelection == VR_LIGHTING_MODE){
				gDynamicLights = (gDynamicLights+3+direction)%3;
				SaveVrInteger("DynamicLights", gDynamicLights);
			}else if(gVrLightingSelection == VR_LIGHTING_INTENSITY){
				gDynamicLightIntensity = clamp(
					gDynamicLightIntensity+step*5, 25, 200);
				SaveVrInteger("DynamicLightIntensity",
					gDynamicLightIntensity);
			}else if(gVrLightingSelection == VR_LIGHTING_GLOW_INTENSITY){
				gDynamicLightGlowIntensity = clamp(
					gDynamicLightGlowIntensity+step*5, 25, 200);
				SaveVrInteger("DynamicLightGlowIntensity",
					gDynamicLightGlowIntensity);
			}else if(gVrLightingSelection == VR_LIGHTING_MAX_LIGHTS){
				gDynamicLightMax = clamp(gDynamicLightMax+direction*2,
					2, 8);
				SaveVrInteger("DynamicLightMax", gDynamicLightMax);
			}else if(gVrLightingSelection == VR_LIGHTING_CAR_REFLECTIONS){
				gCarReflections = !gCarReflections;
				SaveVrInteger("CarReflections", gCarReflections);
			}else if(gVrLightingSelection ==
			         VR_LIGHTING_REFLECTION_INTENSITY){
				gCarReflectionIntensity = clamp(
					gCarReflectionIntensity+step*5, 10, 200);
				SaveVrInteger("CarReflectionIntensity",
					gCarReflectionIntensity);
			}else if(gVrLightingSelection == VR_LIGHTING_REFLECTION_SSR){
				gCarReflectionSsr = clamp(
					gCarReflectionSsr+step*5, 0, 150);
				SaveVrInteger("CarReflectionSsr", gCarReflectionSsr);
			}else if(gVrLightingSelection == VR_LIGHTING_SSR_DISTANCE){
				// 5..150 metres in fives, with 0 standing for no limit at
				// the top of the range.
				int linear = gCarReflectionSsrDistance == 0 ?
					155 : gCarReflectionSsrDistance;
				linear = clamp(linear+step*5, 5, 155);
				gCarReflectionSsrDistance = linear >= 155 ? 0 : linear;
				SaveVrInteger("CarReflectionSsrDistance",
					gCarReflectionSsrDistance);
			}else if(gVrLightingSelection == VR_LIGHTING_SSR_SCALE){
				static const int scales[] = { 1, 2, 4, 8 };
				int index = 0;
				for(int i = 0; i < (int)ARRAY_SIZE(scales); i++)
					if(scales[i] == gCarReflectionScale)
						index = i;
				index = (index+direction+(int)ARRAY_SIZE(scales))%
					(int)ARRAY_SIZE(scales);
				gCarReflectionScale = scales[index];
				SaveVrInteger("CarReflectionScale", gCarReflectionScale);
			}else if(gVrLightingSelection == VR_LIGHTING_MODERN_WATER){
				gVrMenuPage = VR_MENU_PAGE_WATER;
				gVrWaterSelection = 0;
			}else if(gVrLightingSelection == VR_LIGHTING_BACK){
				gVrMenuPage = VR_MENU_PAGE_GRAPHICS;
			}
		}else if(gVrMenuPage == VR_MENU_PAGE_WATER &&
		         (positivePulse || decreasePulse)){
			const int direction = decreasePulse ? -1 : 1;
			const int step = direction*Max(1, repeatMagnitude);
			if(gVrWaterSelection == VR_WATER_ENABLED){
				gModernWater = !gModernWater;
				SaveVrInteger("ModernWater", gModernWater);
			}else if(gVrWaterSelection == VR_WATER_WAVES){
				gWaterWaves = clamp(gWaterWaves+step*5, 0, 200);
				SaveVrInteger("WaterWaves", gWaterWaves);
			}else if(gVrWaterSelection == VR_WATER_SPEED){
				gWaterSpeed = clamp(gWaterSpeed+step*5, 0, 200);
				SaveVrInteger("WaterSpeed", gWaterSpeed);
			}else if(gVrWaterSelection == VR_WATER_DISTORTION){
				gWaterDistortion = clamp(
					gWaterDistortion+step*5, 0, 200);
				SaveVrInteger("WaterDistortion", gWaterDistortion);
			}else if(gVrWaterSelection == VR_WATER_REFLECTION){
				gWaterReflection = clamp(
					gWaterReflection+step*5, 0, 200);
				SaveVrInteger("WaterReflection", gWaterReflection);
			}else if(gVrWaterSelection == VR_WATER_SHEEN){
				gWaterSheen = clamp(gWaterSheen+step*5, 0, 200);
				SaveVrInteger("WaterSheen", gWaterSheen);
			}else if(gVrWaterSelection == VR_WATER_GLINT){
				gWaterGlint = clamp(gWaterGlint+step*5, 0, 200);
				SaveVrInteger("WaterGlint", gWaterGlint);
			}else if(gVrWaterSelection == VR_WATER_SPARKS){
				gWaterSparks = clamp(gWaterSparks+step*5, 0, 200);
				SaveVrInteger("WaterSparks", gWaterSparks);
			}else if(gVrWaterSelection == VR_WATER_SSR_SCALE){
				static const int scales[] = { 1, 2, 4, 8 };
				int index = 0;
				for(int i = 0; i < (int)ARRAY_SIZE(scales); i++)
					if(scales[i] == gCarReflectionScale)
						index = i;
				index = (index+direction+(int)ARRAY_SIZE(scales))%
					(int)ARRAY_SIZE(scales);
				gCarReflectionScale = scales[index];
				SaveVrInteger("CarReflectionScale", gCarReflectionScale);
			}else if(gVrWaterSelection == VR_WATER_BACK){
				gVrMenuPage = VR_MENU_PAGE_LIGHTING;
			}
		}else if(gVrMenuPage == VR_MENU_PAGE_TRAFFIC &&
		         (positivePulse || decreasePulse)){
			const int direction = decreasePulse ? -1 : 1;
			switch(gVrTrafficSelection){
			case VR_TRAFFIC_PEDESTRIANS:
				gTrafficPedPercent = Min(Max(
					gTrafficPedPercent+direction*5, 0), 300);
				SaveVrInteger("PedTrafficPercent",
					gTrafficPedPercent);
				ApplyTrafficSettings();
				break;
			case VR_TRAFFIC_VEHICLES:
				gTrafficCarPercent = Min(Max(
					gTrafficCarPercent+direction*5, 0), 300);
				SaveVrInteger("CarTrafficPercent",
					gTrafficCarPercent);
				ApplyTrafficSettings();
				break;
			case VR_TRAFFIC_PHYSICS_DIRECTOR: {
				const int mode =
					(QuestPhysicsDirectorGetMode()+direction+
					 QUEST_PHYSICS_DIRECTOR_MODE_COUNT)%
					QUEST_PHYSICS_DIRECTOR_MODE_COUNT;
				QuestPhysicsDirectorSetMode(mode);
				SaveVrInteger("PhysicsDirectorMode", mode);
				break;
			}
			case VR_TRAFFIC_RAGDOLLS:
				OpenRagdollMenu();
				break;
			case VR_TRAFFIC_VEHICLE_DEFORMATION:
				OpenDeformationMenu();
				break;
			case VR_TRAFFIC_PHYSICS_PRESET: {
				const int preset =
					(QuestPhysicsDirectorGetPreset()+direction+
					 QUEST_PHYSICS_PRESET_COUNT)%
					QUEST_PHYSICS_PRESET_COUNT;
				QuestPhysicsDirectorSetPreset(preset);
				SaveVrInteger("PhysicsDirectorPreset", preset);
				break;
			}
			case VR_TRAFFIC_VISUAL_BUDGET: {
				const int mode =
					(QuestVehicleVisualBudgetGetMode()+direction+
					 QUEST_VEHICLE_VISUAL_MODE_COUNT)%
					QUEST_VEHICLE_VISUAL_MODE_COUNT;
				QuestVehicleVisualBudgetSetMode(mode);
				SaveVrInteger("VehicleVisualBudgetMode", mode);
				break;
			}
			case VR_TRAFFIC_DEFAULTS:
				gTrafficPedPercent = 135;
				gTrafficCarPercent = 100;
				QuestPhysicsDirectorSetMode(
					QUEST_PHYSICS_DIRECTOR_ADAPTIVE);
				QuestPhysicsDirectorSetPreset(
					QUEST_PHYSICS_PRESET_QUALITY);
				QuestVehicleVisualBudgetSetMode(
					QUEST_VEHICLE_VISUAL_STOCK);
				VehicleDeformation::SetVehicleDeformationEnabled(false);
				SaveVrInteger("VehicleDeformation", 0);
				SaveVrInteger("PedTrafficPercent",
					gTrafficPedPercent);
				SaveVrInteger("CarTrafficPercent",
					gTrafficCarPercent);
				SaveVrInteger("PhysicsDirectorMode",
					QUEST_PHYSICS_DIRECTOR_ADAPTIVE);
				SaveVrInteger("PhysicsDirectorPreset",
					QUEST_PHYSICS_PRESET_QUALITY);
				SaveVrInteger("VehicleVisualBudgetMode",
					QUEST_VEHICLE_VISUAL_STOCK);
				ApplyTrafficSettings();
				break;
			case VR_TRAFFIC_BACK:
				gVrMenuPage = VR_MENU_PAGE_SETTINGS;
				break;
			}
		}else if(gVrMenuPage == VR_MENU_PAGE_RAGDOLL &&
		         (positivePulse || decreasePulse)){
			AdjustRagdollMenuValue(decreasePulse ? -1 : 1);
		}else if(gVrMenuPage == VR_MENU_PAGE_DEFORMATION &&
		         (positivePulse || decreasePulse)){
			AdjustDeformationMenuValue(decreasePulse ? -1 : 1);
		}else if(gVrMenuPage == VR_MENU_PAGE_HUD &&
		         (positivePulse || decreasePulse)){
			const int direction = decreasePulse ? -1 : 1;
			switch(gVrHudSelection){
			case VR_HUD_PRESET:
				// From anything custom the switch lands on immersive.
				ApplyHudPreset(
					CurrentHudPreset() == VR_HUD_PRESET_IMMERSIVE ?
						VR_HUD_PRESET_CLASSIC :
						VR_HUD_PRESET_IMMERSIVE);
				break;
			case VR_HUD_ENABLED:
				gGameplayHudEnabled = !gGameplayHudEnabled;
				SaveGameplayHud();
				break;
			case VR_HUD_HORIZONTAL_SCALE:
				gHudWidthPercent = Min(Max(
					gHudWidthPercent+direction*5, 50), 200);
				SaveVrInteger("HudWidthPercent", gHudWidthPercent);
				break;
			case VR_HUD_SCALE:
				gHudScalePercent = Min(Max(
					gHudScalePercent+direction*5, 50), 200);
				SaveVrInteger("HudScalePercent", gHudScalePercent);
				break;
			case VR_HUD_OFFSET_X:
				gHudOffsetXCm = Min(Max(
					gHudOffsetXCm+direction, -100), 100);
				SaveVrInteger("HudOffsetXCm", gHudOffsetXCm);
				break;
			case VR_HUD_OFFSET_Y:
				gHudOffsetYCm = Min(Max(
					gHudOffsetYCm+direction, -100), 100);
				SaveVrInteger("HudOffsetYCm", gHudOffsetYCm);
				break;
			case VR_HUD_PANEL:
				gVrWristPanelEdit = (gVrWristPanelEdit+
					WRIST_PANEL_COUNT+direction)%WRIST_PANEL_COUNT;
				break;
			case VR_HUD_PANEL_SHOW:
				gWristPanelOn[gVrWristPanelEdit] =
					!gWristPanelOn[gVrWristPanelEdit];
				SaveWristPanels();
				break;
			case VR_HUD_PANEL_HAND:
				gWristPanelHand[gVrWristPanelEdit] =
					gWristPanelHand[gVrWristPanelEdit] ? 0 : 1;
				SaveWristPanels();
				break;
			case VR_HUD_PANEL_SIDE:
				gWristPanelUnderside[gVrWristPanelEdit] =
					!gWristPanelUnderside[gVrWristPanelEdit];
				SaveWristPanels();
				break;
			case VR_HUD_PANEL_GAZE:
				gWristPanelGaze = !gWristPanelGaze;
				SaveWristPanels();
				break;
			case VR_HUD_PANEL_GAZE_RANGE:
				gWristPanelGazeRangeCm = clamp(
					gWristPanelGazeRangeCm+direction, 20, 200);
				SaveWristPanels();
				break;
			case VR_HUD_PANEL_CALIBRATE:
				gVrMenuPage = VR_MENU_PAGE_WRIST_RADAR;
				gVrWristRadarSelection = 0;
				break;
			case VR_HUD_WRIST_IN_VEHICLE:
				gWristPanelsInVehicle = !gWristPanelsInVehicle;
				SaveWristPanels();
				break;
			case VR_HUD_WEAPON_PANEL:
				gHudWeaponPanel = !gHudWeaponPanel;
				SaveVrInteger("HudWeaponPanel",
					gHudWeaponPanel ? 1 : 0);
				break;
			case VR_HUD_CLOCK:
				gHudClock = !gHudClock;
				SaveVrInteger("HudClock", gHudClock ? 1 : 0);
				break;
			case VR_HUD_BACK:
				gVrMenuPage = VR_MENU_PAGE_SETTINGS;
				break;
			}
		}else if(gVrMenuPage == VR_MENU_PAGE_WRIST_RADAR &&
		         (positivePulse || decreasePulse)){
			const int direction = decreasePulse ? -1 : 1;
			const int step = direction*repeatMagnitude;
			const int panel = gVrWristPanelEdit;
			const int side = gWristPanelUnderside[panel] ?
				WRIST_SIDE_INNER : WRIST_SIDE_OUTER;
			const int other = side == WRIST_SIDE_INNER ?
				WRIST_SIDE_OUTER : WRIST_SIDE_INNER;
			const int context = gVrWristContextEdit;
			const int slot = WRIST_SLOT(context, side);
			const int otherSlot = WRIST_SLOT(context, other);
			// Any placement change claims the context being edited; the two
			// rows above it belong to the panel itself and do not.
			if(gVrWristRadarSelection > VR_WRIST_HAND &&
			   gVrWristRadarSelection < VR_WRIST_BACK)
				OwnWristContext(panel, context);
			switch(gVrWristRadarSelection){
			case VR_WRIST_CONTEXT:
				gVrWristContextEdit =
					(gVrWristContextEdit+WRIST_CONTEXT_COUNT+
					 (step > 0 ? 1 : -1)) % WRIST_CONTEXT_COUNT;
				break;
			case VR_WRIST_SIDE:
				// The same setting the HUD page shows: switching it here both
				// moves the panel and picks the values being edited.
				gWristPanelUnderside[panel] = !gWristPanelUnderside[panel];
				SaveWristPanels();
				break;
			case VR_WRIST_HAND:
				gWristPanelHand[panel] = 1-gWristPanelHand[panel];
				SaveWristPanels();
				break;
			case VR_WRIST_ALONG:
				gWristAlong[panel][slot] = Min(Max(
					gWristAlong[panel][slot]+step, -300), 300);
				SaveWristPanels();
				break;
			case VR_WRIST_ACROSS:
				gWristAcross[panel][slot] = Min(Max(
					gWristAcross[panel][slot]+step, -300), 300);
				SaveWristPanels();
				break;
			case VR_WRIST_LIFT:
				gWristLift[panel][slot] = Min(Max(
					gWristLift[panel][slot]+step, -300), 300);
				SaveWristPanels();
				break;
			case VR_WRIST_PITCH:
				gWristPitch[panel][slot] = Min(Max(
					gWristPitch[panel][slot]+step, -1800), 1800);
				SaveWristPanels();
				break;
			case VR_WRIST_YAW:
				gWristYaw[panel][slot] = Min(Max(
					gWristYaw[panel][slot]+step, -1800), 1800);
				SaveWristPanels();
				break;
			case VR_WRIST_ROLL:
				gWristRoll[panel][slot] = Min(Max(
					gWristRoll[panel][slot]+step, -1800), 1800);
				SaveWristPanels();
				break;
			case VR_WRIST_COLOUR_RED:
			case VR_WRIST_COLOUR_GREEN:
			case VR_WRIST_COLOUR_BLUE: {
				static const char *const key[3] = {
					"AmmoColourRed", "AmmoColourGreen",
					"AmmoColourBlue"
				};
				const int channel =
					gVrWristRadarSelection-VR_WRIST_COLOUR_RED;
				gAmmoColour[channel] = Min(Max(
					gAmmoColour[channel]+step*5, 0), 255);
				SaveVrInteger(key[channel], gAmmoColour[channel]);
				break;
			}
			case VR_WRIST_SIZE:
				gWristSize[panel][slot] = Min(Max(
					gWristSize[panel][slot]+step, 40), 250);
				SaveWristPanels();
				break;
			case VR_WRIST_COPY:
				// A starting point, not a result: the other side is
				// usually close enough to calibrate from.
				gWristAlong[panel][slot] = gWristAlong[panel][otherSlot];
				gWristAcross[panel][slot] = gWristAcross[panel][otherSlot];
				gWristLift[panel][slot] = gWristLift[panel][otherSlot];
				gWristPitch[panel][slot] = gWristPitch[panel][otherSlot];
				gWristYaw[panel][slot] = gWristYaw[panel][otherSlot];
				gWristRoll[panel][slot] = gWristRoll[panel][otherSlot];
				gWristSize[panel][slot] = gWristSize[panel][otherSlot];
				SaveWristPanels();
				break;
			case VR_WRIST_RESET:
				// A vehicle context resets by going back to inheriting the
				// on-foot placement, which is more useful than zeroes.
				if(context != WRIST_CONTEXT_DEFAULT){
					gWristContextOwned[panel][context] = false;
					SaveWristPanels();
					break;
				}
				gWristAlong[panel][slot] = 0;
				gWristAcross[panel][slot] = 0;
				gWristLift[panel][slot] = 0;
				gWristPitch[panel][slot] = 0;
				gWristYaw[panel][slot] = 0;
				gWristRoll[panel][slot] = 0;
				gWristSize[panel][slot] = 100;
				SaveWristPanels();
				break;
			case VR_WRIST_BACK:
				gVrMenuPage = VR_MENU_PAGE_HUD;
				break;
			}
		}else if(gVrMenuPage == VR_MENU_PAGE_VEHICLE &&
		         (positivePulse || decreasePulse)){
			const int direction = decreasePulse ? -1 : 1;
			// Forward button fire also applies to the third-person fallback.
			const bool cockpitLocked =
				OculusVR::IsQuestVehicleThirdPerson() &&
				gVrVehicleSelection != VR_VEHICLE_THIRD_PERSON &&
				gVrVehicleSelection != VR_VEHICLE_FORWARD_FIRE &&
				gVrVehicleSelection != VR_VEHICLE_BACK;
			if(!cockpitLocked)
			switch(gVrVehicleSelection){
			case VR_VEHICLE_THIRD_PERSON:
				OculusVR::ToggleQuestVehicleThirdPerson();
				break;
			case VR_VEHICLE_KIND:
				OculusVR::CycleQuestVehicleKind(direction);
				break;
			case VR_VEHICLE_DRIVING_TYPE:
				OculusVR::CycleQuestVehicleKindDrivingType(direction);
				break;
			case VR_VEHICLE_FORWARD_FIRE:
				OculusVR::SetQuestVehicleForwardFireEnabled(
					!OculusVR::IsQuestVehicleForwardFireEnabled());
				break;
			case VR_VEHICLE_DEFAULT_SEAT_HEIGHT:
				OculusVR::AdjustQuestDefaultVehicleSeatHeightCm(
					direction*repeatMagnitude);
				break;
			case VR_VEHICLE_DEFAULT_SEAT_FORWARD:
				OculusVR::AdjustQuestDefaultVehicleSeatDistanceCm(
					direction*repeatMagnitude);
				break;
			case VR_VEHICLE_GLOBAL_SEAT_HEIGHT:
				if(!OculusVR::HasQuestDefaultVehicleViewOffsetTarget())
					OculusVR::AdjustQuestVehicleGlobalSeatHeightCm(
						direction*repeatMagnitude);
				break;
			case VR_VEHICLE_GLOBAL_SEAT_FORWARD:
				if(!OculusVR::HasQuestDefaultVehicleViewOffsetTarget())
					OculusVR::AdjustQuestVehicleGlobalSeatDistanceCm(
						direction*repeatMagnitude);
				break;
			case VR_VEHICLE_MODEL_SEAT_HEIGHT:
				OculusVR::AdjustQuestVehicleModelSeatHeightCm(
					direction*repeatMagnitude);
				break;
			case VR_VEHICLE_MODEL_SEAT_FORWARD:
				OculusVR::AdjustQuestVehicleModelSeatDistanceCm(
					direction*repeatMagnitude);
				break;
			case VR_VEHICLE_MOTION_HAND:
				OculusVR::ToggleQuestMotionSteeringHand();
				break;
			case VR_VEHICLE_WHEEL_VISIBLE:
				OculusVR::ToggleQuestImmersiveCarWheelVisible();
				break;
			case VR_VEHICLE_MODEL_WHEEL_VISIBLE:
				OculusVR::ToggleQuestVehicleModelWheelVisibility();
				break;
			case VR_VEHICLE_WHEEL_HAND_PULL_BACK:
				OculusVR::AdjustQuestWheelHandPullBackMm(direction);
				break;
			case VR_VEHICLE_HANDLE_HIGHLIGHTS:
				OculusVR::ToggleQuestVehicleHandleHighlights();
				break;
			case VR_VEHICLE_BIKE_LOCK_HORIZON:
				OculusVR::ToggleQuestBikeHorizonLock();
				break;
			case VR_VEHICLE_BIKE_THROTTLE:
				OculusVR::ToggleQuestBikeManualThrottle();
				break;
			case VR_VEHICLE_BIKE_VISUAL_LEAN:
				OculusVR::AdjustQuestBikeVisualLeanPercent(direction);
				break;
			case VR_VEHICLE_BIKE_VIEW_TILT:
				OculusVR::ToggleQuestBikeViewFollowsTilt();
				break;
			case VR_VEHICLE_BIKE_THROW_RIDER:
				OculusVR::ToggleQuestBikeRiderCanBeThrown();
				break;
			case VR_VEHICLE_CALIBRATION:
				if(OculusVR::IsQuestVehicleCalibrationAvailable()){
					gVrMenuPage =
						VR_MENU_PAGE_VEHICLE_CALIBRATION;
					gVrVehicleCalibrationSelection = 0;
					gVrVehicleCalibrationHand = 0;
					OculusVR::SetQuestVehicleCalibrationPreview(
						true);
				}
				break;
			case VR_VEHICLE_BACK:
				gVrMenuPage = VR_MENU_PAGE_SETTINGS;
				break;
			}
		}else if(gVrMenuPage ==
		         VR_MENU_PAGE_VEHICLE_CALIBRATION &&
		         (positivePulse || decreasePulse)){
			const int valueCount =
				OculusVR::GetQuestVehicleCalibrationItemCount();
			const int direction = decreasePulse ? -1 : 1;
			const int item = OculusVR::GetQuestVehicleCalibrationItemForRow(
				gVrVehicleCalibrationSelection);
			if(item == OculusVR::QUEST_VEHICLE_CAL_HAND)
				gVrVehicleCalibrationHand =
					1-gVrVehicleCalibrationHand;
			else if(gVrVehicleCalibrationSelection < valueCount)
				OculusVR::AdjustQuestVehicleCalibrationValue(
					gVrVehicleCalibrationHand,
					item,
					direction*repeatMagnitude);
			else{
				OculusVR::SetQuestVehicleCalibrationPreview(
					false);
				gVrMenuPage = VR_MENU_PAGE_VEHICLE;
			}
		}else if(gVrMenuPage == VR_MENU_PAGE_LOCOMOTION &&
		         (positivePulse || decreasePulse)){
			const int direction = decreasePulse ? -1 : 1;
			switch(gVrLocomotionSelection){
#if MIAMIVR_STEAM_FRAME
			case VR_LOCOMOTION_VIEW:
				SetViewMode((gViewMode+3+direction)%3);
				break;
#endif
			case VR_LOCOMOTION_MOVEMENT_ORIENTATION:
				// BODY ignores physical rotation: a standing player who turns
				// around and pushes forward walks the way they used to face.
				// It stays on the page for anyone who wants it, but a brushed
				// stick or trigger steps over it -- only a deliberate press
				// selects it. Leaving BODY works either way.
				if(selectPulse)
					gQuestMovementOrientation =
						(gQuestMovementOrientation+1) %
						QUEST_MOVEMENT_ORIENTATION_COUNT;
				else{
					int next = gQuestMovementOrientation;
					do
						next = (next+
							QUEST_MOVEMENT_ORIENTATION_COUNT+
							direction) %
							QUEST_MOVEMENT_ORIENTATION_COUNT;
					while(next == QUEST_MOVEMENT_ORIENTATION_BODY);
					gQuestMovementOrientation = next;
				}
				SaveVrInteger("MovementOrientation",
					gQuestMovementOrientation);
				break;
#if MIAMIVR_STEAM_FRAME
			case VR_LOCOMOTION_THIRD_PERSON_MOVEMENT:
				gThirdPersonMovement = !gThirdPersonMovement;
				SaveVrInteger("ThirdPersonMovement",
					gThirdPersonMovement);
				break;
#endif
			case VR_LOCOMOTION_TURN_MODE:
				gQuestTurnMode =
					(gQuestTurnMode+QUEST_TURN_MODE_COUNT+
					 direction) % QUEST_TURN_MODE_COUNT;
				SaveVrInteger("TurnMode", gQuestTurnMode);
				break;
			case VR_LOCOMOTION_TURN_SENSITIVITY:
				if(gQuestMovementOrientation ==
				   QUEST_MOVEMENT_ORIENTATION_HEAD_TURN_EXPERIMENTAL){
					gQuestHeadSteeringSensitivityPercent = Min(Max(
						gQuestHeadSteeringSensitivityPercent+
						 direction*5, 25), 300);
					SaveVrInteger("HeadSteeringSensitivityPercent",
						gQuestHeadSteeringSensitivityPercent);
				}else{
					gQuestTurnSensitivityPercent = Min(Max(
						gQuestTurnSensitivityPercent+
						 direction*5, 25), 300);
					SaveVrInteger("TurnSensitivityPercent",
						gQuestTurnSensitivityPercent);
				}
				break;
			case VR_LOCOMOTION_CUTSCENES:
#if MIAMIVR_STEAM_FRAME
			{
				const int count = (int)(sizeof(kCutsceneModeOrder)/
					sizeof(kCutsceneModeOrder[0]));
				int index = 0;
				while(index < count &&
				      kCutsceneModeOrder[index] != gCutsceneMode)
					index++;
				index = (index%count+count+direction)%count;
				gCutsceneMode = kCutsceneModeOrder[index];
				SaveVrInteger("CutsceneMode", gCutsceneMode);
				break;
			}
#else
				gCutsceneMode = (gCutsceneMode+2+
					(decreasePulse ? -1 : 1))%2;
				SaveVrInteger("CutsceneMode", gCutsceneMode);
				break;
#endif
			case VR_LOCOMOTION_HEAD_BOBBING:
				gQuestHeadBobbing = !gQuestHeadBobbing;
				SaveVrInteger("HeadBobbing", gQuestHeadBobbing);
				break;
			case VR_LOCOMOTION_REFRESH_RATE: {
				static const int rates[] = { 72, 80, 90, 120 };
				const int count =
					(int)(sizeof(rates)/sizeof(rates[0]));
				int index = 0;
				for(int i = 0; i < count; i++)
					if(rates[i] == gQuestRefreshRateHz)
						index = i;
				index = (index + direction + count) % count;
				gQuestRefreshRateHz = rates[index];
				SaveVrInteger("RefreshRate", gQuestRefreshRateHz);
				androidgame::SetPreferredRefreshRate(
					gQuestRefreshRateHz);
				break;
			}
			case VR_LOCOMOTION_SNAP_ANGLE:
				gQuestSnapTurnAngleDegrees += direction*15;
				if(gQuestSnapTurnAngleDegrees < 15)
					gQuestSnapTurnAngleDegrees = 90;
				if(gQuestSnapTurnAngleDegrees > 90)
					gQuestSnapTurnAngleDegrees = 15;
				SaveVrInteger("SnapTurnAngleDegrees",
					gQuestSnapTurnAngleDegrees);
				break;
			case VR_LOCOMOTION_RECENTER:
				// Same operation as the both-grips chord: drop the anchor and
				// let VrUpdateFirstPersonAnchor latch the pose being held now.
				// A player who sat down to start and then stood up is riding an
				// eye height taken while seated until this runs.
				androidgame::VrRecenterView();
				gVrMenuVisible = false;
				break;
			case VR_LOCOMOTION_BACK:
				gVrMenuPage = VR_MENU_PAGE_SETTINGS;
				break;
			}
		}else if(gVrMenuPage == VR_MENU_PAGE_CONTROLS &&
		         (positivePulse || decreasePulse)){
			const int direction = decreasePulse ? -1 : 1;
			const int source = VrControlsRowSource(gVrControlsSelection);
			if(source >= 0){
				// NONE is a real choice here: it is how a player switches an
				// input off, and the row it belongs to says UNUSED.
				int target = gVrPadBinding[source]+direction;
				if(target < VR_PAD_TARGET_NONE)
					target = VR_PAD_TARGET_COUNT-1;
				if(target >= VR_PAD_TARGET_COUNT)
					target = VR_PAD_TARGET_NONE;
				gVrPadBinding[source] = target;
				SavePadBinding(source);
			}else if(gVrControlsSelection == VR_CONTROLS_LAYOUT){
				// CUSTOM describes an assignment, it is not one to step into:
				// the row cycles between the two layouts that are.
				ApplyControlsLayout(
					CurrentControlsLayout() ==
						VR_CONTROLS_LAYOUT_DEFAULT ?
						kVrSwappedHandsBinding :
						kVrPadBindingDefault);
			}else if(gVrControlsSelection == VR_CONTROLS_LOOK_BEHIND){
				gVrStickLookBehind = !gVrStickLookBehind;
				SaveVrInteger("StickLookBehind", gVrStickLookBehind);
			}else if(gVrControlsSelection == VR_CONTROLS_CROUCH){
				gVrStickCrouch = !gVrStickCrouch;
				SaveVrInteger("StickCrouch", gVrStickCrouch);
			}else if(gVrControlsSelection == VR_CONTROLS_RESET){
				ApplyControlsLayout(kVrPadBindingDefault);
			}else if(gVrControlsSelection == VR_CONTROLS_BACK){
				gVrMenuPage = VR_MENU_PAGE_SETTINGS;
			}
		}else if(gVrMenuPage == VR_MENU_PAGE_CALIBRATION){
			if(gVrCalibrationSelection == 0 &&
			   (positivePulse || decreasePulse))
				gVrCalibrationHand = 1-gVrCalibrationHand;
			else if(gVrCalibrationSelection >= 1 &&
			        gVrCalibrationSelection <= 19 &&
			        (positivePulse || decreasePulse)){
				const int weaponType =
					OculusVR::GetQuestCalibrationWeaponType(
						gVrCalibrationHand);
				OculusVR::AdjustQuestCalibrationValue(
					gVrCalibrationHand, weaponType,
					gVrCalibrationSelection-1,
					(decreasePulse ? -1 : 1)*repeatMagnitude);
			}else if(gVrCalibrationSelection == 20 &&
			         (positivePulse || decreasePulse)){
				OculusVR::CycleQuestWeaponLaserOverride(
					OculusVR::GetQuestCalibrationWeaponType(
						gVrCalibrationHand),
					decreasePulse ? -1 : 1);
			}else if(gVrCalibrationSelection == 21 &&
			         positivePulse)
				gVrMenuPage = VR_MENU_PAGE_SETTINGS;
		}else if(gVrMenuPage == VR_MENU_PAGE_HOLSTERS){
			const int points = OculusVR::GetQuestHolsterPointCount();
			if(gVrHolsterSelection < points &&
			   (positivePulse || decreasePulse))
				OculusVR::CycleQuestHolsterPointSlot(
					gVrHolsterSelection,
					decreasePulse ? -1 : 1);
			else if(gVrHolsterSelection == points && positivePulse)
				gVrMenuPage = VR_MENU_PAGE_SETTINGS;
		}else if(gVrMenuPage == VR_MENU_PAGE_CHEATS){
			if((increasePulse || decreasePulse) && !selectPulse)
				CycleQuestCheatMenuSelection(decreasePulse ? -1 : 1);
			else if(selectPulse)
				ActivateQuestCheatMenuSelection();
		}else if(gVrMenuPage == VR_MENU_PAGE_MISSIONS && selectPulse){
			if(gVrMissionCategory < 0){
				gVrMissionCategory = gVrMissionCategorySelection;
				gVrMissionSelection = 0;
			}else if(ActivateVrMission(gVrMissionCategory,
			          gVrMissionSelection)){
				gVrMenuVisible = false;
				gVrMenuPage = VR_MENU_PAGE_SETTINGS;
			}else{
				snprintf(gVrCheatStatus, sizeof(gVrCheatStatus),
					"MISSION NOT SAFE RIGHT NOW");
				gVrCheatStatusFrames = 120;
			}
		}

#if MIAMIVR_STEAM_FRAME
		// View's short press is the Frame's "back" (standard.md section 1).
		const bool back = in.b || in.leftStickClick || in.viewBack;
#else
		const bool back = in.b || in.leftStickClick;
#endif
		if(back && !gVrMenuBackDown)
			ReturnFromCurrentMenuPage();
		gVrMenuBackDown = back;
		if(gVrCheatStatusFrames > 0)
			gVrCheatStatusFrames--;
	}else{
		gVrMenuIncreaseDown = false;
		gVrMenuDecreaseDown = false;
		gVrCheatCycleDown = false;
	}

	if(gDebugPreviousFrameMs > 0.0){
		const float frameMs = (float)(now-gDebugPreviousFrameMs);
		gDebugSmoothedFrameMs = gDebugSmoothedFrameMs > 0.0f ?
			gDebugSmoothedFrameMs*0.9f+frameMs*0.1f : frameMs;
		gDebugFps = gDebugSmoothedFrameMs > 0.0f ?
			(int)(1000.0f/gDebugSmoothedFrameMs+0.5f) : 0;
	}
	gDebugPreviousFrameMs = now;
}

static void
QuestMainCategoryColour(int item, uint8 *red, uint8 *green, uint8 *blue)
{
	static const uint8 colours[VR_MAIN_ITEM_COUNT][3] = {
		{100,225,255}, {125,255,145}, {255,205,90},
		{190,145,255}, {255,155,90}, {90,220,210},
		{145,200,255}, {255,120,205}, {240,180,105},
		{255,105,105}, {170,190,210}
	};
	*red = colours[item][0]; *green = colours[item][1];
	*blue = colours[item][2];
}

// A submenu wears the colour its category has on the settings page, and the
// pages opened from a submenu keep their parent's, so the palette says where
// the player is without reading the heading.
static void
QuestMenuPageColour(uint8 *red, uint8 *green, uint8 *blue)
{
	int category = -1;
	switch(gVrMenuPage){
	case VR_MENU_PAGE_GRAPHICS:
	case VR_MENU_PAGE_LIGHTING:
	case VR_MENU_PAGE_WATER: category = VR_MAIN_GRAPHICS; break;
	case VR_MENU_PAGE_TRAFFIC:
	case VR_MENU_PAGE_DEFORMATION:
	case VR_MENU_PAGE_RAGDOLL: category = VR_MAIN_TRAFFIC_SETTINGS; break;
	case VR_MENU_PAGE_HUD:
	case VR_MENU_PAGE_WRIST_RADAR: category = VR_MAIN_HUD; break;
	case VR_MENU_PAGE_MODEL_ASSETS: category = VR_MAIN_MODEL_ASSETS; break;
	case VR_MENU_PAGE_VEHICLE:
	case VR_MENU_PAGE_VEHICLE_CALIBRATION:
		category = VR_MAIN_VEHICLE_SETTINGS; break;
	case VR_MENU_PAGE_LOCOMOTION:
		category = VR_MAIN_LOCOMOTION_SETTINGS; break;
	case VR_MENU_PAGE_CONTROLS: category = VR_MAIN_CONTROLS; break;
	case VR_MENU_PAGE_WEAPONS:
	case VR_MENU_PAGE_CALIBRATION: category = VR_MAIN_WEAPONS; break;
	case VR_MENU_PAGE_HOLSTERS: category = VR_MAIN_HOLSTERS; break;
	case VR_MENU_PAGE_CHEATS:
	case VR_MENU_PAGE_MISSIONS: category = VR_MAIN_CHEATS; break;
	case VR_MENU_PAGE_ABOUT: category = VR_MAIN_ABOUT; break;
	}
	if(category < 0){
		*red = 100; *green = 225; *blue = 255;
		return;
	}
	QuestMainCategoryColour(category, red, green, blue);
}

static void
BeginFullVrMenuPage(const char *heading, const char *subtitle = nil)
{
	FillVrMenuRect(0, 0, VR_MENU_WIDTH, VR_MENU_HEIGHT,
		5, 10, 20, 238);
	FillVrMenuRect(28, 28, VR_MENU_WIDTH-28, VR_MENU_HEIGHT-28,
		10, 22, 38, 245);
	DrawVrMenuText("VICE CITY VR", VR_MENU_WIDTH/2, 55, 7,
		255, 120, 205);
	uint8 red, green, blue;
	QuestMenuPageColour(&red, &green, &blue);
	DrawVrMenuText(heading, VR_MENU_WIDTH/2, 112, 4, red, green, blue);
	if(subtitle && subtitle[0] != '\0')
		DrawVrMenuText(subtitle, VR_MENU_WIDTH/2, 146, 2,
			170, 190, 210);
}

static void
DrawFullVrMenuRow(const char *text, int y, int scale, bool selected,
                  bool available = true, bool warning = false,
                  bool positive = false, bool highlight = false)
{
	// A row worth finding on a page where every line reads alike, in
	// the title pink because nothing else on a page uses it.
	if(highlight){
		if(selected)
			FillVrMenuRect(85, y-5, VR_MENU_WIDTH-85,
				y+scale*7+4, 95, 32, 78, 245);
		DrawVrMenuText(text, VR_MENU_WIDTH/2, y, scale,
			255, selected ? 165 : 120, selected ? 228 : 205);
		return;
	}
	if(selected)
		FillVrMenuRect(85, y-5, VR_MENU_WIDTH-85,
			y+scale*7+4,
			warning ? 125 : (positive ? 20 : (available ? 25 : 55)),
			warning ? 35 : (positive ? 105 : (available ? 95 : 62)),
			warning ? 30 : (positive ? 60 : (available ? 135 : 72)), 245);
	if(!warning && !positive && !selected && available){
		uint8 red, green, blue;
		QuestMenuPageColour(&red, &green, &blue);
		DrawVrMenuText(text, VR_MENU_WIDTH/2, y, scale, red, green, blue);
		return;
	}
	DrawVrMenuText(text, VR_MENU_WIDTH/2, y, scale,
		warning ? 255 : (positive ? (selected ? 210 : 105) :
			(selected ? 255 : (available ? 205 : 120))),
		warning ? (selected ? 225 : 105) :
			(positive ? 245 : (selected ? (available ? 245 : 180) :
			 (available ? 215 : 135))),
		warning ? (selected ? 120 : 95) :
			(positive ? 145 : (selected ? (available ? 110 : 130) :
			 (available ? 225 : 145))));
}

static void
DrawQuestSettingsPage(void)
{
	gQuestCpuPerformanceMode = Min(Max(xrvk::getPerformanceMode(),
		(int)QUEST_CPU_PERFORMANCE_AUTO),
		(int)QUEST_CPU_PERFORMANCE_COUNT-1);
	BeginFullVrMenuPage("SETTINGS");
	char rows[VR_MAIN_ITEM_COUNT][112];
	strcpy(rows[VR_MAIN_GRAPHICS], "GRAPHICS  < OPEN >");
	strcpy(rows[VR_MAIN_TRAFFIC_SETTINGS],
		"TRAFFIC  < OPEN >");
	strcpy(rows[VR_MAIN_HUD], "HUD  < OPEN >");
	strcpy(rows[VR_MAIN_MODEL_ASSETS],
		"MODEL ASSETS  < OPEN >");
	strcpy(rows[VR_MAIN_VEHICLE_SETTINGS],
		"VEHICLE  < OPEN >");
	strcpy(rows[VR_MAIN_LOCOMOTION_SETTINGS],
		"LOCOMOTION  < OPEN >");
	strcpy(rows[VR_MAIN_CONTROLS], "CONTROLS  < OPEN >");
	strcpy(rows[VR_MAIN_WEAPONS], "WEAPONS  < OPEN >");
	strcpy(rows[VR_MAIN_HOLSTERS], "HOLSTER LOADOUT  < OPEN >");
	strcpy(rows[VR_MAIN_CHEATS], "CHEAT MENU  < OPEN >");
	strcpy(rows[VR_MAIN_ABOUT], "ABOUT VICE CITY VR  < OPEN >");

	for(int item = 0; item < VR_MAIN_ITEM_COUNT; item++){
		DrawFullVrMenuRow(rows[item], 160+item*50, 3,
			item == gVrMenuSelection, true);
		uint8 red, green, blue;
		QuestMainCategoryColour(item, &red, &green, &blue);
		if(item != gVrMenuSelection)
			DrawVrMenuText(rows[item], VR_MENU_WIDTH/2,
				160+item*50, 3, red, green, blue);
	}
	DrawVrMenuText(
		"LEFT STICK SELECT   L2 PREVIOUS   R2 OR A NEXT   B CLOSE",
		VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
}

static void
DrawQuestGraphicsPage(void)
{
	BeginFullVrMenuPage("GRAPHICS", VR_PLATFORM_NAME " RENDERING AND PERFORMANCE");
	xrvk::RenderScaleStatus scaleStatus = {};
	const bool scaleStatusValid = xrvk::getRenderScaleStatus(&scaleStatus);
	char rows[VR_GRAPHICS_ITEM_COUNT][112];
	if(scaleStatusValid)
		snprintf(rows[VR_GRAPHICS_RENDER_SCALE], sizeof(rows[0]),
			"OUTPUT REQUEST < %d%% >  ACTIVE %.0f%% - RESTART",
			gQuestRenderScalePercent, scaleStatus.effectivePercent);
	else
		snprintf(rows[VR_GRAPHICS_RENDER_SCALE], sizeof(rows[0]),
			"OUTPUT REQUEST  < %d%% - RESTART >",
			gQuestRenderScalePercent);
	snprintf(rows[VR_GRAPHICS_SGSR], sizeof(rows[0]),
		"TEMPORAL AA < DISABLED - UNSTABLE >");
	snprintf(rows[VR_GRAPHICS_MSAA], sizeof(rows[0]),
		"* STEREO MSAA  < %s - RESTART >",
		gQuestMsaaSamples >= 4 ? "4X" :
		gQuestMsaaSamples >= 2 ? "2X" : "OFF");
	snprintf(rows[VR_GRAPHICS_FXAA], sizeof(rows[0]),
		"SPATIAL AA  < %s >", gSpatialAaMode ? "ON" : "OFF");
	snprintf(rows[VR_GRAPHICS_PS2_ALPHA], sizeof(rows[0]),
		"* PS2 ALPHA (FENCE FIX)  < %s >",
		gPs2AlphaTest ? "ON" : "OFF");
	snprintf(rows[VR_GRAPHICS_MIPMAPS], sizeof(rows[0]),
		"* GENERATE MIPMAPS  < %s >",
		gGenerateMipmaps ? "ON (RESTART)" : "OFF");
	snprintf(rows[VR_GRAPHICS_FOLIAGE], sizeof(rows[0]),
		"* FOLIAGE SOFTNESS  < %s >",
		gFoliageSoftness == 3 ? "1.5 LEVELS" :
		gFoliageSoftness == 2 ? "1.0 LEVEL" :
		gFoliageSoftness == 1 ? "0.5 LEVEL" : "OFF");
	snprintf(rows[VR_GRAPHICS_DIAGNOSTICS], sizeof(rows[0]),
		"* RENDERER LOG  < %s >",
		gRenderDiagnostics ? "ON" : "OFF");
	snprintf(rows[VR_GRAPHICS_COLOR], sizeof(rows[0]),
		"COLOR FILTER  < %s >", gViceCityColorEnabled ? "ON" : "OFF");
	snprintf(rows[VR_GRAPHICS_PROFILER], sizeof(rows[0]),
		"* PERFORMANCE PROFILER (FPS)  < %s >",
		QuestProfilerIsEnabled() ? "ON" : "OFF");
	snprintf(rows[VR_GRAPHICS_CPU_PERFORMANCE], sizeof(rows[0]),
		"CPU PERFORMANCE < %s > ACTIVE %s",
		QuestCpuPerformanceModeName(gQuestCpuPerformanceMode),
		xrvk::getActivePerformanceMode() >= 0 ?
			QuestCpuPerformanceModeName(xrvk::getActivePerformanceMode()) :
			"PENDING");
	snprintf(rows[VR_GRAPHICS_GPU_PERFORMANCE], sizeof(rows[0]),
		"GPU PERFORMANCE < %s > ACTIVE %s",
		QuestCpuPerformanceModeName(gQuestGpuPerformanceMode),
		xrvk::getActiveGpuPerformanceMode() >= 0 ?
			QuestCpuPerformanceModeName(xrvk::getActiveGpuPerformanceMode()) :
			"PENDING");
	snprintf(rows[VR_GRAPHICS_SHADOWS], sizeof(rows[0]),
		"WORLD SHADOWS  < %s >",
		CShadows::IsRenderEnabled() ? "ON" : "OFF");
	snprintf(rows[VR_GRAPHICS_DYNAMIC_LIGHTS], sizeof(rows[0]),
		"EFFECTS  < OPEN >  LIGHTS %s",
		gDynamicLights >= 2 ? "ON + AIR GLOW" :
		gDynamicLights == 1 ? "ON" : "OFF");
	snprintf(rows[VR_GRAPHICS_OCCLUSION], sizeof(rows[0]),
		"OCCLUSION CULLING  < %s >",
		CRenderer::GetVrOcclusionCullingModeName());
#if defined(MIAMIVR_DEV_TOOLS) && MIAMIVR_DEV_TOOLS
	if(VrCullViz::IsActive())
		snprintf(rows[VR_GRAPHICS_CULL_VIZ], sizeof(rows[0]),
			"CULLING VISUALIZER  < FROZEN - SELECT TO RESUME >");
	else
		snprintf(rows[VR_GRAPHICS_CULL_VIZ], sizeof(rows[0]),
			"CULLING VISUALIZER  < FREEZE AND PAINT >");
#endif
	snprintf(rows[VR_GRAPHICS_FOUNTAIN], sizeof(rows[0]),
		"FOUNTAIN PARTICLES  < %s >",
		CParticleObject::GetVrFountainQualityName());
	snprintf(rows[VR_GRAPHICS_FOG], sizeof(rows[0]),
		"DISTANCE FOG  < %s >", gDistanceFog ? "ON" : "OFF");
#if defined(MIAMIVR_DEV_TOOLS) && MIAMIVR_DEV_TOOLS
	snprintf(rows[VR_GRAPHICS_QUICK_START], sizeof(rows[0]),
		"QUICK TEST START  < %s - NEXT LAUNCH >",
		gQuestQuickTestStart ? "ON" : "OFF");
#endif
	strcpy(rows[VR_GRAPHICS_BACK], "BACK TO SETTINGS");
	for(int item = 0; item < VR_GRAPHICS_ITEM_COUNT; item++)
		// Compact rows leave room for the status footer.
		DrawFullVrMenuRow(rows[item], 174+item*25, 2,
			item == gVrGraphicsSelection, true, false,
			item == VR_GRAPHICS_PROFILER ||
				item == VR_GRAPHICS_MIPMAPS ||
				item == VR_GRAPHICS_PS2_ALPHA ||
				item == VR_GRAPHICS_MSAA ||
				item == VR_GRAPHICS_DIAGNOSTICS ||
				item == VR_GRAPHICS_FOLIAGE,
			// Highlight the lighting submenu.
			item == VR_GRAPHICS_DYNAMIC_LIGHTS);
	if(scaleStatusValid){
		char activeScale[192];
		snprintf(activeScale, sizeof(activeScale),
			"OUTPUT ACTIVE %.0f%%  %u X %u  BASE %u X %u%s%s",
			scaleStatus.effectivePercent,
			scaleStatus.actualWidth, scaleStatus.actualHeight,
			scaleStatus.recommendedWidth, scaleStatus.recommendedHeight,
			scaleStatus.fallbackReason !=
				xrvk::RENDER_SCALE_FALLBACK_NONE ? "  FALLBACK: " : "",
			scaleStatus.fallbackReason !=
				xrvk::RENDER_SCALE_FALLBACK_NONE ?
				xrvk::getRenderScaleFallbackReasonName(
					scaleStatus.fallbackReason) : "");
		const bool fallback = scaleStatus.fallbackReason !=
			xrvk::RENDER_SCALE_FALLBACK_NONE;
		DrawVrMenuText(activeScale, VR_MENU_WIDTH/2, 674, 2,
			fallback ? 255 : 120, fallback ? 95 : 220,
			fallback ? 85 : 255);
		DrawVrMenuText("NATIVE SCENE  SPATIAL AA SINGLE-FRAME",
			VR_MENU_WIDTH/2, 692, 2, 170, 190, 210);
		if(scaleStatus.previousFallbackReason !=
		   xrvk::RENDER_SCALE_FALLBACK_NONE){
			char recoveredScale[192];
			snprintf(recoveredScale, sizeof(recoveredScale),
				"LAST FALLBACK %d%% TO %d%%: %s",
				scaleStatus.previousFallbackRequestedPercent,
				scaleStatus.previousFallbackPercent,
				xrvk::getRenderScaleFallbackReasonName(
					scaleStatus.previousFallbackReason));
			DrawVrMenuText(recoveredScale, VR_MENU_WIDTH/2, 710, 2,
				255, 105, 85);
		}
	}
#if defined(MIAMIVR_DEV_TOOLS) && MIAMIVR_DEV_TOOLS
	if(VrCullViz::IsActive()){
		DrawVrMenuText(VrCullViz::StatusLine(),
			VR_MENU_WIDTH/2, 728, 2, 210, 220, 235);
		DrawVrMenuText("BLUE DRAWN   RED OFF-FRUSTUM   ORANGE OCCLUDED   YELLOW DISTANCE",
			VR_MENU_WIDTH/2, 746, 2, 200, 210, 225);
	}
	else
#endif
	if(gOcclusionCullingMode >= VR_OCCLUSION_CULLING_AUTHORED)
		DrawVrMenuText("AUTHORED CULLING IS EXPERIMENTAL: USE STEREO SAFE IF EYES DISAGREE",
			VR_MENU_WIDTH/2, 746, 2, 255, 105, 95);
	else if(gQuestRenderScalePercent >= 150)
		DrawVrMenuText("150/175% IS EXPERIMENTAL: HIGH GPU AND MEMORY LOAD",
			VR_MENU_WIDTH/2, 746, 2, 255, 120, 95);
	else if(gQuestRenderScalePercent > 100)
		DrawVrMenuText("HIGHER SCALE SHARPENS THE IMAGE BUT INCREASES GPU LOAD",
			VR_MENU_WIDTH/2, 746, 2, 255, 175, 95);
}

static void
DrawQuestWeaponsPage(void)
{
	BeginFullVrMenuPage("WEAPONS", "PHYSICAL HANDS, GRIPS AND CALIBRATION");
	char rows[VR_WEAPONS_ITEM_COUNT][112];
	for(int item = VR_WEAPONS_HANDS; item <= VR_WEAPONS_GRIP_LOCK; item++){
		const int setting = QuestWeaponSettingForWeaponItem(item);
		snprintf(rows[item], sizeof(rows[item]), "%s  < %s >",
			OculusVR::GetQuestWeaponSettingName(setting),
			OculusVR::GetQuestWeaponSetting(setting) ? "ON" : "OFF");
	}
	strcpy(rows[VR_WEAPONS_CALIBRATION], "WEAPON CALIBRATION  < OPEN >");
	strcpy(rows[VR_WEAPONS_BACK], "BACK TO SETTINGS");
	for(int item = 0; item < VR_WEAPONS_ITEM_COUNT; item++)
		DrawFullVrMenuRow(rows[item], 170+item*55, 3,
			item == gVrWeaponsSelection);
}

static void
DrawQuestLightingPage(void)
{
	BeginFullVrMenuPage("EFFECTS",
		"DYNAMIC LIGHTS, AIR GLOW AND CAR REFLECTIONS");
	char rows[VR_LIGHTING_ITEM_COUNT][112];
	snprintf(rows[VR_LIGHTING_MODE], sizeof(rows[0]),
		"MODE  < %s >",
		gDynamicLights >= 2 ? "ON + AIR GLOW" :
		gDynamicLights == 1 ? "ON" : "OFF");
	snprintf(rows[VR_LIGHTING_INTENSITY], sizeof(rows[0]),
		"LIGHT INTENSITY  < %d%% >", gDynamicLightIntensity);
	snprintf(rows[VR_LIGHTING_GLOW_INTENSITY], sizeof(rows[0]),
		"AIR GLOW INTENSITY  < %d%% >", gDynamicLightGlowIntensity);
	snprintf(rows[VR_LIGHTING_MAX_LIGHTS], sizeof(rows[0]),
		"MAX LIGHTS  < %d >", gDynamicLightMax);
	snprintf(rows[VR_LIGHTING_CAR_REFLECTIONS], sizeof(rows[0]),
		"CAR REFLECTIONS  < %s >",
		gCarReflections ? "ON" : "OFF");
	snprintf(rows[VR_LIGHTING_REFLECTION_INTENSITY], sizeof(rows[0]),
		"REFLECTION INTENSITY  < %d%% >", gCarReflectionIntensity);
	snprintf(rows[VR_LIGHTING_REFLECTION_SSR], sizeof(rows[0]),
		"SSR STRENGTH  < %d%% >", gCarReflectionSsr);
	if(gCarReflectionSsrDistance == 0)
		snprintf(rows[VR_LIGHTING_SSR_DISTANCE], sizeof(rows[0]),
			"SSR DISTANCE  < UNLIMITED >");
	else
		snprintf(rows[VR_LIGHTING_SSR_DISTANCE], sizeof(rows[0]),
			"SSR DISTANCE  < %dM >", gCarReflectionSsrDistance);
	snprintf(rows[VR_LIGHTING_SSR_SCALE], sizeof(rows[0]),
		"SSR RESOLUTION  < %s - RESTART >",
		gCarReflectionScale == 1 ? "FULL" :
		gCarReflectionScale == 2 ? "HALF" :
		gCarReflectionScale == 4 ? "QUARTER" : "EIGHTH");
	snprintf(rows[VR_LIGHTING_MODERN_WATER], sizeof(rows[0]),
		"MODERN WATER  < OPEN - %s >", gModernWater ? "ON" : "OFF");
	strcpy(rows[VR_LIGHTING_BACK], "BACK TO GRAPHICS");
	for(int item = 0; item < VR_LIGHTING_ITEM_COUNT; item++)
		DrawFullVrMenuRow(rows[item], 166+item*40, 2,
			item == gVrLightingSelection, true, false);
	DrawVrMenuText("INTENSITY TAMES NEON-DENSE STREETS LIKE OCEAN DRIVE",
		VR_MENU_WIDTH/2, 620, 2, 170, 190, 210);
	DrawVrMenuText("SSR 0% OR A SHORT SSR DISTANCE GIVES THE CLASSIC LOOK",
		VR_MENU_WIDTH/2, 644, 2, 170, 190, 210);
}

static void
DrawQuestWaterPage(void)
{
	BeginFullVrMenuPage("MODERN WATER",
		"WAVES, MIRROR AND SPECULARS OVER THE ORIGINAL WATER");
	char rows[VR_WATER_ITEM_COUNT][112];
	snprintf(rows[VR_WATER_ENABLED], sizeof(rows[0]),
		"MODERN WATER  < %s >", gModernWater ? "ON" : "OFF");
	snprintf(rows[VR_WATER_WAVES], sizeof(rows[0]),
		"WAVE STRENGTH  < %d%% >", gWaterWaves);
	snprintf(rows[VR_WATER_SPEED], sizeof(rows[0]),
		"WAVE SPEED  < %d%% >", gWaterSpeed);
	snprintf(rows[VR_WATER_DISTORTION], sizeof(rows[0]),
		"MIRROR DISTORTION  < %d%% >", gWaterDistortion);
	snprintf(rows[VR_WATER_REFLECTION], sizeof(rows[0]),
		"REFLECTION  < %d%% >", gWaterReflection);
	snprintf(rows[VR_WATER_SHEEN], sizeof(rows[0]),
		"SKY SHEEN  < %d%% >", gWaterSheen);
	snprintf(rows[VR_WATER_GLINT], sizeof(rows[0]),
		"SUN GLINT  < %d%% >", gWaterGlint);
	snprintf(rows[VR_WATER_SPARKS], sizeof(rows[0]),
		"LIGHT SPARKS  < %d%% >", gWaterSparks);
	snprintf(rows[VR_WATER_SSR_SCALE], sizeof(rows[0]),
		"SSR RESOLUTION  < %s - RESTART >",
		gCarReflectionScale == 1 ? "FULL" :
		gCarReflectionScale == 2 ? "HALF" :
		gCarReflectionScale == 4 ? "QUARTER" : "EIGHTH");
	strcpy(rows[VR_WATER_BACK], "BACK TO EFFECTS");
	for(int item = 0; item < VR_WATER_ITEM_COUNT; item++)
		DrawFullVrMenuRow(rows[item], 166+item*40, 2,
			item == gVrWaterSelection, true, false);
	DrawVrMenuText("BLUR AND SHIMMER LIVE IN WAVES AND DISTORTION - TURN THEM DOWN",
		VR_MENU_WIDTH/2, 620, 2, 170, 190, 210);
	DrawVrMenuText("REFLECTION IS THE MIRROR, SHEEN THE SKY WHERE IT CANNOT REACH",
		VR_MENU_WIDTH/2, 644, 2, 170, 190, 210);
	DrawVrMenuText("SSR RESOLUTION IS ONE COPY SHARED WITH CAR REFLECTIONS",
		VR_MENU_WIDTH/2, 668, 2, 170, 190, 210);
}

static void
DrawQuestTrafficPage(void)
{
	BeginFullVrMenuPage("TRAFFIC SETTINGS",
		"LIVE DENSITY - SAFE ENTITIES CONVERGE WITHOUT HARD DELETION");
	char rows[VR_TRAFFIC_ITEM_COUNT][112];
	// Zero reads as OFF: no new spawns. What is already on the street
	// stays and leaves on its own, which is what the page promises.
	if(gTrafficPedPercent == 0)
		strcpy(rows[VR_TRAFFIC_PEDESTRIANS], "PEDESTRIANS  < OFF >");
	else
		snprintf(rows[VR_TRAFFIC_PEDESTRIANS], sizeof(rows[0]),
			"PEDESTRIANS  < %d%% >", gTrafficPedPercent);
	if(gTrafficCarPercent == 0)
		strcpy(rows[VR_TRAFFIC_VEHICLES], "VEHICLES  < OFF >");
	else
		snprintf(rows[VR_TRAFFIC_VEHICLES], sizeof(rows[0]),
			"VEHICLES  < %d%% >", gTrafficCarPercent);
	snprintf(rows[VR_TRAFFIC_PHYSICS_DIRECTOR], sizeof(rows[0]),
		"PHYSICS DIRECTOR  < %s >",
		QuestPhysicsDirectorGetModeName());
	snprintf(rows[VR_TRAFFIC_PHYSICS_PRESET], sizeof(rows[0]),
		"PHYSICS PRESET  < %s >",
		QuestPhysicsDirectorGetPresetName());
	snprintf(rows[VR_TRAFFIC_VISUAL_BUDGET], sizeof(rows[0]),
		"MODERN CAR VISUALS  < %s >",
		QuestVehicleVisualBudgetGetModeName());
	snprintf(rows[VR_TRAFFIC_VEHICLE_DEFORMATION], sizeof(rows[0]),
		"VEHICLE DEFORMATION  < OPEN - %s >",
		VehicleDeformation::IsVehicleDeformationEnabled() ? "ON" : "OFF");
	strcpy(rows[VR_TRAFFIC_RAGDOLLS], "RAGDOLL  < OPEN >");
	strcpy(rows[VR_TRAFFIC_DEFAULTS],
		"RESTORE DEFAULTS < 135/100% / MEASURE / BALANCED / STOCK / DEFORM OFF >");
	strcpy(rows[VR_TRAFFIC_BACK], "BACK TO SETTINGS");
	const bool modernVehicles = ModelSets::GetActiveForCategory(
		ModelSets::MODEL_CATEGORY_VEHICLES) == ModelSets::MODEL_SET_MODERN;
	for(int item = 0; item < VR_TRAFFIC_ITEM_COUNT; item++){
		const bool gpuWarning = modernVehicles &&
			(item == VR_TRAFFIC_VEHICLES ||
			 item == VR_TRAFFIC_VISUAL_BUDGET);
		DrawFullVrMenuRow(rows[item], 166+item*36, 2,
			item == gVrTrafficSelection, true, gpuWarning);
	}

	char status[112];
	const QuestPhysicsDirectorSnapshot physics =
		QuestPhysicsDirectorGetSnapshot();
	if(QuestPhysicsDirectorGetMode() == QUEST_PHYSICS_DIRECTOR_MEASURE &&
	   !QuestProfilerIsEnabled())
		strcpy(status, "ENABLE PROFILER FOR PHYSICS TIERS");
	else
		snprintf(status, sizeof(status),
			"%s  F/R/R/P %d/%d/%d/%d  %.2f/%.2fMS",
			QuestPhysicsDirectorGetStatusName(),
			physics.tierCount[QUEST_VEHICLE_PHYSICS_FULL],
			physics.tierCount[QUEST_VEHICLE_PHYSICS_REDUCED],
			physics.tierCount[QUEST_VEHICLE_PHYSICS_RAIL],
			physics.tierCount[QUEST_VEHICLE_PHYSICS_PROXY],
			physics.managedAverageMs, physics.budgetMs);
	DrawVrMenuText(status, VR_MENU_WIDTH/2, 478, 2,
		125, 210, 255);
	const QuestVehicleVisualBudgetSnapshot visual =
		QuestVehicleVisualBudgetGetSnapshot();
	if(visual.mode != QUEST_VEHICLE_VISUAL_STOCK)
		DrawVrMenuText("FORCED VLO CAN POP IN VR - STOCK IS RECOMMENDED",
			VR_MENU_WIDTH/2, 690, 2, 255, 105, 95);
	snprintf(status, sizeof(status),
		"VIS VHI/VLO %llu / %llu   SKIP/OCC %llu / %llu",
		(unsigned long long)visual.highVehicleSubmissions,
		(unsigned long long)visual.vloVehicleSubmissions,
		(unsigned long long)visual.atomicsSkipped,
		(unsigned long long)visual.occupantsSkipped);
	DrawVrMenuText(status, VR_MENU_WIDTH/2, 504, 2,
		255, 190, 115);
	snprintf(status, sizeof(status),
		"WALKERS %u / %.1f   CAP %d",
		CPopulation::ms_nTotalPeds, CPopulation::VrTargetAmbientPeds,
		CGame::IsInInterior() ?
			CPopulation::MaxNumberOfPedsInUseInterior :
			CPopulation::MaxNumberOfPedsInUse);
	DrawVrMenuText(status, VR_MENU_WIDTH/2, 530, 3,
		125, 255, 145);
	CPedPool *pedPool = CPools::GetPedPool();
	snprintf(status, sizeof(status), "PED POOL %d / %d",
		pedPool != nil ? pedPool->GetNoOfUsedSpaces() : 0,
		pedPool != nil ? pedPool->GetSize() : 0);
	DrawVrMenuText(status, VR_MENU_WIDTH/2, 566, 3,
		125, 255, 145);
	snprintf(status, sizeof(status),
		"CARS %d + %d PROXY   LOCAL %.1f / %.1f",
		CCarCtrl::NumVrEffectiveAmbient,
		CCarCtrl::NumVrActiveProxies,
		CCarCtrl::VrLocalServed, CCarCtrl::VrLocalDesired);
	DrawVrMenuText(status, VR_MENU_WIDTH/2, 602, 3,
		125, 255, 145);
	DrawVrMenuText("RANGES 50-300%   STEP 5%",
		VR_MENU_WIDTH/2, 664, 2, 255, 180, 225);
	DrawVrMenuText(
		"LEFT STICK SELECT   L2 MINUS   R2 OR A PLUS   B BACK",
		VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
}

static void
DrawQuestRagdollPage(void)
{
	BeginFullVrMenuPage("RAGDOLL", "BODY AND VEHICLE RESPONSE");
	char rows[VR_RAGDOLL_ITEM_COUNT][80];
	snprintf(rows[VR_RAGDOLL_ENABLED], sizeof(rows[0]),
		"RAGDOLL  < %s >", VrRagdoll::IsEnabled() ? "ON" : "OFF");
	snprintf(rows[VR_RAGDOLL_BRAKING], sizeof(rows[0]),
		"VEHICLE BRAKING  < %d%% >", VrRagdoll::GetBrakePercent());
	snprintf(rows[VR_RAGDOLL_WEIGHT], sizeof(rows[0]),
		"BODY WEIGHT  < %d%% >", VrRagdoll::GetWeightPercent());
	snprintf(rows[VR_RAGDOLL_GRIP], sizeof(rows[0]),
		"CAR BODY GRIP  < %d%% >", VrRagdoll::GetGripPercent());
	snprintf(rows[VR_RAGDOLL_SHOTS], sizeof(rows[0]),
		"BULLET REACTION  < %d%% >", VrRagdoll::GetShotPercent());
	strcpy(rows[VR_RAGDOLL_DEFAULTS], "RESET RAGDOLL DEFAULTS");
	strcpy(rows[VR_RAGDOLL_BACK], "BACK TO TRAFFIC");
	for(int item = 0; item < VR_RAGDOLL_ITEM_COUNT; item++)
		DrawFullVrMenuRow(rows[item], 180+item*40, 3,
			item == gVrRagdollSelection);

	static const char *help[VR_RAGDOLL_ITEM_COUNT][2] = {
		{"PHYSICAL FALLS AND CORPSE REACTIONS",
		 "OFF USES STANDARD DEATH ANIMATIONS"},
		{"HOW MUCH A BODY SLOWS A MOVING CAR",
		 "200%: DEFAULT   1000-2000%: VERY STRONG BRAKING"},
		{"MORE MASS: SMALLER RECOIL AND STRONGER CAR RESISTANCE",
		 "50-300%   100%: 70 KG   FALL SPEED UNCHANGED"},
		{"HIGHER VALUES HELP BODIES STAY ON THE HOOD",
		 "BODIES CAN STILL SLIDE OFF WHEN TURNING OR BRAKING"},
		{"HOW STRONGLY BODIES REACT TO BULLETS",
		 "100%: DEFAULT   LOWER: SUBTLER HITS"},
		{"RESTORES WEIGHT 200% / BRAKE 200% / GRIP 150%",
		 "SHOTS 100% / RAGDOLL OFF"},
		{"RETURN TO TRAFFIC SETTINGS", ""}
	};
	const int selected = Min(Max(gVrRagdollSelection, 0),
		VR_RAGDOLL_ITEM_COUNT-1);
	DrawVrMenuText(help[selected][0], VR_MENU_WIDTH/2, 470, 2,
		255, 180, 225);
	DrawVrMenuText(help[selected][1], VR_MENU_WIDTH/2, 494, 2,
		170, 190, 210);
	DrawVrMenuText("WEIGHT 50-300%   BRAKE 0-2000%   GRIP 0-300%   SHOTS 25-200%",
		VR_MENU_WIDTH/2, 548, 2, 170, 190, 210);
	DrawVrMenuText("CHANGES APPLY NOW AND SAVE AUTOMATICALLY",
		VR_MENU_WIDTH/2, 576, 2, 170, 190, 210);
#if defined(MIAMIVR_DEV_TOOLS) && MIAMIVR_DEV_TOOLS
	DrawVrMenuText(VrRagdoll::DebugLine(), VR_MENU_WIDTH/2, 620, 2,
		125, 210, 255);
	DrawVrMenuText(VrRagdoll::VehicleDebugLine(), VR_MENU_WIDTH/2, 648, 2,
		125, 210, 255);
#endif
	DrawVrMenuText(
		"LEFT STICK SELECT   L2 MINUS   R2 OR A PLUS   B BACK",
		VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
}

static void
DrawQuestDeformationPage(void)
{
	BeginFullVrMenuPage("VEHICLE DEFORMATION", "CAR BODY RESPONSE");
	char rows[VR_DEFORMATION_ITEM_COUNT][80];
	snprintf(rows[VR_DEFORMATION_ENABLED], sizeof(rows[0]),
		"DEFORMATION  < %s >", VehicleDeformation::IsVehicleDeformationEnabled() ? "ON" : "OFF");
	snprintf(rows[VR_DEFORMATION_STRENGTH], sizeof(rows[0]),
		"IMPACT STRENGTH  < %d%% >", VehicleDeformation::GetStrengthPercent());
	snprintf(rows[VR_DEFORMATION_RADIUS], sizeof(rows[0]),
		"IMPACT RADIUS  < %d%% >", VehicleDeformation::GetRadiusPercent());
	snprintf(rows[VR_DEFORMATION_MAX_DENT], sizeof(rows[0]),
		"MAXIMUM DENT  < %d CM >", VehicleDeformation::GetMaxDentCentimeters());
	snprintf(rows[VR_DEFORMATION_THRESHOLD], sizeof(rows[0]),
		"IMPACT THRESHOLD  < %d%% >", VehicleDeformation::GetThresholdPercent());
	strcpy(rows[VR_DEFORMATION_DEFAULTS], "RESET DEFORMATION DEFAULTS");
	strcpy(rows[VR_DEFORMATION_BACK], "BACK TO TRAFFIC");
	for(int item = 0; item < VR_DEFORMATION_ITEM_COUNT; item++)
		DrawFullVrMenuRow(rows[item], 180+item*40, 3,
			item == gVrDeformationSelection);
	static const char *help[VR_DEFORMATION_ITEM_COUNT][2] = {
		{"PHYSICAL DENTS IN THE CAR BODY", "OFF RESTORES THE ORIGINAL BODY SHAPE"},
		{"HIGHER: MORE DEFORMATION FROM THE SAME HIT", "25-400%   STEP 25%"},
		{"HOW WIDE THE DENT SPREADS AROUND THE IMPACT", "50-200%   STEP 10%"},
		{"LIMITS HOW FAR THE BODY CAN MOVE", "10-60 CM   STEP 2 CM"},
		{"LOWER: SMALLER IMPACTS CAN DENT THE BODY", "25-200%   STEP 25%"},
		{"STRENGTH 100% / RADIUS 100% / MAX DENT 32 CM", "THRESHOLD 100% / DEFORMATION OFF"},
		{"RETURN TO TRAFFIC SETTINGS", ""}
	};
	const int selected = Min(Max(gVrDeformationSelection, 0), VR_DEFORMATION_ITEM_COUNT-1);
	DrawVrMenuText(help[selected][0], VR_MENU_WIDTH/2, 470, 2, 255, 180, 225);
	DrawVrMenuText(help[selected][1], VR_MENU_WIDTH/2, 494, 2, 170, 190, 210);
	DrawVrMenuText("CHANGES APPLY NOW AND SAVE AUTOMATICALLY",
		VR_MENU_WIDTH/2, 576, 2, 170, 190, 210);
	DrawVrMenuText("LEFT STICK SELECT   L2 MINUS   R2 OR A PLUS   B BACK",
		VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
}

static void
DrawQuestModelAssetsPage(void)
{
	BeginFullVrMenuPage("MODEL ASSETS",
		"CLASSIC / MODERN / XBOX - FULL APP RESTART REQUIRED");
	char rows[VR_MODEL_ASSETS_ITEM_COUNT][112];
	snprintf(rows[VR_MODEL_ASSETS_PRESET], sizeof(rows[0]),
		"PRESET  < %s >",
		ModelSets::GetSourceName(ModelSets::GetRequested()));
	for(int category = 0; category < ModelSets::MODEL_CATEGORY_COUNT;
	    category++){
		const bool available = ModelSets::IsCategoryAvailable(
			(ModelSets::eModelCategory)category);
		snprintf(rows[category+1], sizeof(rows[0]), "%s  < %s >%s%s",
			ModelSets::GetCategoryName((ModelSets::eModelCategory)category),
			ModelSets::GetSourceName(ModelSets::GetRequestedForCategory(
				(ModelSets::eModelCategory)category)),
			available ? "" : "  UNAVAILABLE",
			ModelSets::IsCategoryRestartRequired(
				(ModelSets::eModelCategory)category) ? "  RESTART" : "");
	}
	strcpy(rows[VR_MODEL_ASSETS_BACK], "BACK TO SETTINGS");
	for(int item = 0; item < VR_MODEL_ASSETS_ITEM_COUNT; item++){
		const bool available = item == VR_MODEL_ASSETS_PRESET ||
			item == VR_MODEL_ASSETS_BACK ||
			ModelSets::IsCategoryAvailable(
				(ModelSets::eModelCategory)(item-1));
		const bool modernVehicleWarning =
			item == VR_MODEL_ASSETS_VEHICLES &&
			ModelSets::GetRequestedForCategory(
				ModelSets::MODEL_CATEGORY_VEHICLES) ==
				ModelSets::MODEL_SET_MODERN;
		DrawFullVrMenuRow(rows[item], 210+item*42, 2,
			item == gVrModelAssetsSelection, available,
			modernVehicleWarning);
	}
	if(ModelSets::GetRequestedForCategory(
	   ModelSets::MODEL_CATEGORY_VEHICLES) ==
	   ModelSets::MODEL_SET_MODERN){
		DrawVrMenuText("WARNING: MODERN VEHICLES ARE VERY GPU HEAVY",
			VR_MENU_WIDTH/2, 630, 2, 255, 95, 85);
		DrawVrMenuText("USE CLASSIC VEHICLES OR REDUCE 300% TRAFFIC IF FPS DROPS",
			VR_MENU_WIDTH/2, 660, 2, 255, 155, 105);
	}else
		DrawVrMenuText("CLASSIC VEGETATION IS DEFAULT FOR " VR_PLATFORM_NAME " PERFORMANCE",
			VR_MENU_WIDTH/2, 650, 2, 245, 205, 90);
}

static void
DrawQuestHudPage(void)
{
	BeginFullVrMenuPage("HUD SETTINGS",
		"HEAD-LOCKED GAMEPLAY INTERFACE");
	char rows[VR_HUD_ITEM_COUNT][112];
	const int preset = CurrentHudPreset();
	snprintf(rows[VR_HUD_PRESET], sizeof(rows[0]),
		"HUD PRESET  < %s >",
		preset == VR_HUD_PRESET_IMMERSIVE ? "IMMERSIVE (EXPERIMENTAL)" :
			(preset == VR_HUD_PRESET_CLASSIC ? "CLASSIC (ON SCREEN)" :
			"CUSTOM"));
	snprintf(rows[VR_HUD_ENABLED], sizeof(rows[0]),
		"GAMEPLAY HUD  < %s >", gGameplayHudEnabled ? "ON" : "OFF");
	snprintf(rows[VR_HUD_HORIZONTAL_SCALE], sizeof(rows[0]),
		"HORIZONTAL WIDTH  < %d%% >", gHudWidthPercent);
	snprintf(rows[VR_HUD_SCALE], sizeof(rows[0]),
		"UNIFORM SCALE  < %d%% >", gHudScalePercent);
	snprintf(rows[VR_HUD_OFFSET_X], sizeof(rows[0]),
		"HORIZONTAL OFFSET  < %+d CM >", gHudOffsetXCm);
	snprintf(rows[VR_HUD_OFFSET_Y], sizeof(rows[0]),
		"VERTICAL OFFSET  < %+d CM >", gHudOffsetYCm);
	const int panel = gVrWristPanelEdit;
	snprintf(rows[VR_HUD_PANEL], sizeof(rows[0]),
		"PANEL  < %s >", WristPanelName(panel));
	snprintf(rows[VR_HUD_PANEL_SHOW], sizeof(rows[0]),
		"%s  < %s >", WristPanelName(panel),
		gWristPanelOn[panel] ? "WORN" : "OFF");
	snprintf(rows[VR_HUD_PANEL_HAND], sizeof(rows[0]),
		"WORN ON  < %s HAND >",
		gWristPanelHand[panel] ? "RIGHT" : "LEFT");
	snprintf(rows[VR_HUD_PANEL_SIDE], sizeof(rows[0]),
		"WORN  < %s >",
		gWristPanelUnderside[panel] ? "UNDER THE WRIST" : "ON TOP");
	snprintf(rows[VR_HUD_PANEL_CALIBRATE], sizeof(rows[0]),
		"PLACE AND SIZE %s  < OPEN >", WristPanelName(panel));
	snprintf(rows[VR_HUD_PANEL_GAZE], sizeof(rows[0]),
		"SHOW PANELS  < %s >",
		gWristPanelGaze ? "WHEN LOOKED AT" : "ALWAYS");
	snprintf(rows[VR_HUD_PANEL_GAZE_RANGE], sizeof(rows[0]),
		"LOOK RANGE  < %d CM >", gWristPanelGazeRangeCm);
	snprintf(rows[VR_HUD_WRIST_IN_VEHICLE], sizeof(rows[0]),
		"WRIST PANELS WHILE DRIVING  < %s >",
		gWristPanelsInVehicle ? "IMMERSIVE ONLY" : "OFF");
	snprintf(rows[VR_HUD_WEAPON_PANEL], sizeof(rows[0]),
		"WEAPON ICON AND AMMO  < %s >", gHudWeaponPanel ? "ON" : "OFF");
	snprintf(rows[VR_HUD_CLOCK], sizeof(rows[0]),
		"CLOCK  < %s >", gHudClock ? "ON" : "OFF");
	strcpy(rows[VR_HUD_BACK], "BACK TO SETTINGS");
	int visibleRow = 0;
	for(int item = 0; item < VR_HUD_ITEM_COUNT; item++){
		if(!IsMenuItemVisible(VR_MENU_PAGE_HUD, item))
			continue;
		DrawFullVrMenuRow(rows[item], 146+visibleRow++*33, 3,
			item == gVrHudSelection);
	}
	DrawVrMenuText(
		"LEFT STICK SELECT   L2 MINUS   R2 OR A PLUS   B BACK",
		VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
}

static void
DrawQuestWristRadarPage(void)
{
	const int panel = gVrWristPanelEdit;
	const int side = gWristPanelUnderside[panel] ?
		WRIST_SIDE_INNER : WRIST_SIDE_OUTER;
	const char *sideName = side == WRIST_SIDE_INNER ? "INNER" : "OUTER";
	const char *otherName = side == WRIST_SIDE_INNER ? "OUTER" : "INNER";
	const char *panelName = WristPanelName(panel);
	const int context = gVrWristContextEdit;
	const int slot = WRIST_SLOT(context, side);
	char heading[96];
	snprintf(heading, sizeof(heading), "WRIST %s - %s SIDE - %s",
		panelName, sideName, kWristContextName[context]);
	BeginFullVrMenuPage(heading,
		"CALIBRATE THE LEFT HAND - THE RIGHT ONE MIRRORS IT");
	char rows[VR_WRIST_ITEM_COUNT][112];
	snprintf(rows[VR_WRIST_CONTEXT], sizeof(rows[0]),
		"CALIBRATING FOR  < %s%s >", kWristContextName[context],
		context == WRIST_CONTEXT_DEFAULT ? "" :
			(gWristContextOwned[panel][context] ? "" :
				" - SAME AS ON FOOT"));
	snprintf(rows[VR_WRIST_SIDE], sizeof(rows[0]),
		"SIDE  < %s >", side == WRIST_SIDE_INNER ?
			"INNER (UNDER WRIST)" : "OUTER (TOP OF WRIST)");
	snprintf(rows[VR_WRIST_HAND], sizeof(rows[0]),
		"WORN ON  < %s HAND >",
		gWristPanelHand[panel] ? "RIGHT" : "LEFT");
	snprintf(rows[VR_WRIST_ALONG], sizeof(rows[0]),
		"ALONG THE ARM  < %+.1f CM >",
		(float)gWristAlong[panel][slot]*0.1f);
	snprintf(rows[VR_WRIST_ACROSS], sizeof(rows[0]),
		"ACROSS THE WRIST  < %+.1f CM >",
		(float)gWristAcross[panel][slot]*0.1f);
	snprintf(rows[VR_WRIST_LIFT], sizeof(rows[0]),
		"OFF THE WRIST  < %+.1f CM >", (float)gWristLift[panel][slot]*0.1f);
	snprintf(rows[VR_WRIST_PITCH], sizeof(rows[0]),
		"TILT  < %+.1f DEG >", (float)gWristPitch[panel][slot]*0.1f);
	snprintf(rows[VR_WRIST_YAW], sizeof(rows[0]),
		"TURN  < %+.1f DEG >", (float)gWristYaw[panel][slot]*0.1f);
	snprintf(rows[VR_WRIST_ROLL], sizeof(rows[0]),
		"SPIN  < %+.1f DEG >", (float)gWristRoll[panel][slot]*0.1f);
	snprintf(rows[VR_WRIST_SIZE], sizeof(rows[0]),
		"SIZE  < %d%% >", gWristSize[panel][slot]);
	static const char *const colourName[3] = { "RED", "GREEN", "BLUE" };
	for(int channel = 0; channel < 3; channel++)
		snprintf(rows[VR_WRIST_COLOUR_RED+channel], sizeof(rows[0]),
			"TEXT %s  < %d >", colourName[channel],
			gAmmoColour[channel]);
	snprintf(rows[VR_WRIST_COPY], sizeof(rows[0]),
		"COPY FROM %s SIDE", otherName);
	if(context == WRIST_CONTEXT_DEFAULT)
		snprintf(rows[VR_WRIST_RESET], sizeof(rows[0]),
			"RESET %s SIDE", sideName);
	else
		strcpy(rows[VR_WRIST_RESET], "GO BACK TO THE ON FOOT PLACEMENT");
	strcpy(rows[VR_WRIST_BACK], "BACK TO HUD SETTINGS");
	int visibleRow = 0;
	for(int item = 0; item < VR_WRIST_ITEM_COUNT; item++){
		if(!IsMenuItemVisible(VR_MENU_PAGE_WRIST_RADAR, item))
			continue;
		DrawFullVrMenuRow(rows[item], 148+visibleRow++*42, 3,
			item == gVrWristRadarSelection,
			item <= VR_WRIST_HAND || gWristPanelOn[panel]);
	}
	if(!gWristPanelOn[panel])
		DrawVrMenuText("THIS PANEL IS OFF - TURN IT ON IN HUD SETTINGS",
			VR_MENU_WIDTH/2, 690, 2, 245, 205, 90);
	DrawVrMenuText(
		"LEFT STICK SELECT   L2 MINUS   R2 OR A PLUS   B BACK",
		VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
}

static void
DrawQuestVehiclePage(void)
{
	BeginFullVrMenuPage("VEHICLE SETTINGS",
		"DRIVING CONTROLS AND PER-VEHICLE CALIBRATION");
	char rows[VR_VEHICLE_ITEM_COUNT][112];
	// Third person is a per-kind choice now, so the page reads the one it is
	// showing rather than whatever the player happens to be sitting in.
	const bool thirdPersonView = OculusVR::IsQuestVehicleKindThirdPerson();
	snprintf(rows[VR_VEHICLE_KIND], sizeof(rows[0]),
		"SETTINGS FOR  < %s >", OculusVR::GetQuestVehicleKindName());
	snprintf(rows[VR_VEHICLE_DRIVING_TYPE], sizeof(rows[0]),
		"DRIVING TYPE  < %s >", thirdPersonView ?
			"DEFAULT (THIRD PERSON)" :
			OculusVR::GetQuestVehicleKindDrivingTypeName());
	snprintf(rows[VR_VEHICLE_THIRD_PERSON], sizeof(rows[0]),
		"VIEW  < %s >",
		thirdPersonView ? "THIRD PERSON" : "FIRST PERSON");
	snprintf(rows[VR_VEHICLE_FORWARD_FIRE], sizeof(rows[0]),
		"DEFAULT FORWARD FIRE (B)  < %s >",
		OculusVR::IsQuestVehicleForwardFireEnabled() ? "ON" : "OFF");
	if(OculusVR::HasQuestDefaultVehicleViewOffsetTarget()){
		snprintf(rows[VR_VEHICLE_DEFAULT_SEAT_HEIGHT], sizeof(rows[0]),
			"DEFAULT %s HEIGHT  < %+d CM >",
			OculusVR::GetQuestDefaultVehicleViewOffsetName(),
			OculusVR::GetQuestDefaultVehicleSeatHeightCm());
		snprintf(rows[VR_VEHICLE_DEFAULT_SEAT_FORWARD], sizeof(rows[0]),
			"DEFAULT %s FORWARD  < %+d CM >",
			OculusVR::GetQuestDefaultVehicleViewOffsetName(),
			OculusVR::GetQuestDefaultVehicleSeatDistanceCm());
	}else{
		strcpy(rows[VR_VEHICLE_DEFAULT_SEAT_HEIGHT],
			"DEFAULT VIEW HEIGHT  < ENTER DEFAULT CAR / BIKE >");
		strcpy(rows[VR_VEHICLE_DEFAULT_SEAT_FORWARD],
			"DEFAULT VIEW FORWARD  < ENTER DEFAULT CAR / BIKE >");
	}
	if(OculusVR::HasQuestVehicleSeatCalibrationTarget() &&
	   !OculusVR::HasQuestDefaultVehicleViewOffsetTarget()){
		snprintf(rows[VR_VEHICLE_GLOBAL_SEAT_HEIGHT], sizeof(rows[0]),
			"%s GLOBAL HEIGHT  < %+d CM >", OculusVR::GetQuestVehicleCategoryName(),
			OculusVR::GetQuestVehicleGlobalSeatHeightCm());
		snprintf(rows[VR_VEHICLE_GLOBAL_SEAT_FORWARD], sizeof(rows[0]),
			"%s GLOBAL FORWARD  < %+d CM >", OculusVR::GetQuestVehicleCategoryName(),
			OculusVR::GetQuestVehicleGlobalSeatDistanceCm());
		snprintf(rows[VR_VEHICLE_MODEL_SEAT_HEIGHT], sizeof(rows[0]),
			"MODEL HEIGHT  < %+d CM >", OculusVR::GetQuestVehicleModelSeatHeightCm());
		snprintf(rows[VR_VEHICLE_MODEL_SEAT_FORWARD], sizeof(rows[0]),
			"MODEL FORWARD  < %+d CM >", OculusVR::GetQuestVehicleModelSeatDistanceCm());
	}else if(OculusVR::HasQuestDefaultVehicleViewOffsetTarget()){
		strcpy(rows[VR_VEHICLE_GLOBAL_SEAT_HEIGHT],
			"GLOBAL HEIGHT  < IMMERSIVE / MOTION ONLY >");
		strcpy(rows[VR_VEHICLE_GLOBAL_SEAT_FORWARD],
			"GLOBAL FORWARD  < IMMERSIVE / MOTION ONLY >");
		snprintf(rows[VR_VEHICLE_MODEL_SEAT_HEIGHT], sizeof(rows[0]),
			"MODEL HEIGHT  < %+d CM >",
			OculusVR::GetQuestVehicleModelSeatHeightCm());
		snprintf(rows[VR_VEHICLE_MODEL_SEAT_FORWARD], sizeof(rows[0]),
			"MODEL FORWARD  < %+d CM >",
			OculusVR::GetQuestVehicleModelSeatDistanceCm());
	}else{
		strcpy(rows[VR_VEHICLE_GLOBAL_SEAT_HEIGHT], "GLOBAL HEIGHT  < ENTER VEHICLE >");
		strcpy(rows[VR_VEHICLE_GLOBAL_SEAT_FORWARD], "GLOBAL FORWARD  < ENTER VEHICLE >");
		strcpy(rows[VR_VEHICLE_MODEL_SEAT_HEIGHT], "MODEL HEIGHT  < ENTER VEHICLE >");
		strcpy(rows[VR_VEHICLE_MODEL_SEAT_FORWARD], "MODEL FORWARD  < ENTER VEHICLE >");
	}
	snprintf(rows[VR_VEHICLE_MOTION_HAND], sizeof(rows[0]),
		"MOTION STEERING HAND  < %s >",
		OculusVR::GetQuestMotionSteeringHand() == 0 ?
			"LEFT" : "RIGHT");
	snprintf(rows[VR_VEHICLE_WHEEL_VISIBLE], sizeof(rows[0]),
		"ALL CARS VIRTUAL WHEEL  < %s >",
		OculusVR::IsQuestImmersiveCarWheelVisible() ? "VISIBLE" : "HIDDEN");
	snprintf(rows[VR_VEHICLE_MODEL_WHEEL_VISIBLE], sizeof(rows[0]),
		"THIS MODEL VIRTUAL WHEEL  < %s >",
		OculusVR::GetQuestVehicleModelWheelVisibilityName());
	snprintf(rows[VR_VEHICLE_WHEEL_HAND_PULL_BACK], sizeof(rows[0]),
		"WHEEL HAND PULL BACK  < %+d MM >",
		OculusVR::GetQuestWheelHandPullBackMm());
	snprintf(rows[VR_VEHICLE_HANDLE_HIGHLIGHTS], sizeof(rows[0]),
		"GRIP HIGHLIGHTS  < %s >",
		OculusVR::AreQuestVehicleHandleHighlightsEnabled() ?
			"ON" : "OFF");
	snprintf(rows[VR_VEHICLE_BIKE_LOCK_HORIZON], sizeof(rows[0]),
		"BIKE LOCK HORIZON  < %s >",
		OculusVR::IsQuestBikeHorizonLocked() ? "ON" : "OFF");
	snprintf(rows[VR_VEHICLE_BIKE_THROTTLE], sizeof(rows[0]),
		"BIKE THROTTLE  < %s >",
		OculusVR::IsQuestBikeManualThrottle() ?
			"WRIST TWIST" : "RIGHT TRIGGER");
	snprintf(rows[VR_VEHICLE_BIKE_VISUAL_LEAN], sizeof(rows[0]),
		"BIKE LEAN SHOWN  < %d%% >",
		OculusVR::GetQuestBikeVisualLeanPercent());
	snprintf(rows[VR_VEHICLE_BIKE_VIEW_TILT], sizeof(rows[0]),
		"BIKE VIEW FOLLOWS TILT  < %s >",
		OculusVR::IsQuestBikeViewFollowingTilt() ? "ON" : "OFF");
	snprintf(rows[VR_VEHICLE_BIKE_THROW_RIDER], sizeof(rows[0]),
		"THROWN OFF ON A CRASH  < %s >",
		OculusVR::CanQuestBikeRiderBeThrown() ? "ON" : "OFF");
	snprintf(rows[VR_VEHICLE_CALIBRATION], sizeof(rows[0]),
		"CONTROL CALIBRATION  < %s >",
		OculusVR::IsQuestVehicleCalibrationAvailable() ?
			"OPEN" : "ENTER VEHICLE");
	strcpy(rows[VR_VEHICLE_BACK], "BACK TO SETTINGS");
	int visibleRow = 0;
	for(int item = 0; item < VR_VEHICLE_ITEM_COUNT; item++){
		if(!IsMenuItemVisible(VR_MENU_PAGE_VEHICLE, item))
			continue;
		// Which control the bike takes its throttle from decides whether the
		// thing moves at all, and it is the first row a new rider comes
		// looking for. It is marked so it can be found without reading.
		DrawFullVrMenuRow(rows[item], 158+visibleRow++*32, 2,
			item == gVrVehicleSelection, true,
			item == VR_VEHICLE_BIKE_THROTTLE);
	}
	DrawVrMenuText("VALUES ARE SAVED IN VR SETTINGS",
		VR_MENU_WIDTH/2, 694, 2, 125, 255, 145);
	DrawVrMenuText(
		"LEFT STICK SELECT   L2 PREVIOUS   R2 OR A NEXT   B BACK",
		VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
}

static void
DrawQuestVehicleCalibrationPage(void)
{
	char heading[128];
	snprintf(heading, sizeof(heading), "VEHICLE CONTROLS - %s",
		OculusVR::GetQuestActiveVehicleName());
	BeginFullVrMenuPage(heading,
		"VALUES ARE SAVED FOR THIS VEHICLE MODEL");
	const int valueCount =
		OculusVR::GetQuestVehicleCalibrationItemCount();
	for(int rowIndex = 0; rowIndex <= valueCount; rowIndex++){
		char row[112];
		const int item = OculusVR::GetQuestVehicleCalibrationItemForRow(rowIndex);
		const char *label = OculusVR::GetQuestVehicleCalibrationItemName(item);
		if(item == OculusVR::QUEST_VEHICLE_CAL_HAND)
			snprintf(row, sizeof(row), "%s  < %s >", label,
				gVrVehicleCalibrationHand == 0 ?
					"LEFT" : "RIGHT");
		else if(rowIndex < valueCount){
			const int value =
				OculusVR::GetQuestVehicleCalibrationValue(
					gVrVehicleCalibrationHand, item);
			if(OculusVR::IsQuestVehicleCalibrationRotation(item))
				snprintf(row, sizeof(row),
					"%s  < %+.1f DEG >", label,
					(float)value/2.0f);
			else if(OculusVR::IsQuestVehicleCalibrationWholeCentimeters(item))
				snprintf(row, sizeof(row), "%s  < %d CM >",
					label, value);
			else
				snprintf(row, sizeof(row),
					"%s  < %+.1f CM >", label,
					(float)value/2.0f);
		}else
			strcpy(row, "BACK TO VEHICLE SETTINGS");
		DrawFullVrMenuRow(row, 166+rowIndex*29, 2,
			rowIndex == gVrVehicleCalibrationSelection,
			true);
	}
	DrawVrMenuText(
		"LEFT STICK SELECT   L2 MINUS   R2 OR A PLUS   B BACK",
		VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
}

static void
DrawQuestLocomotionPage(void)
{
	BeginFullVrMenuPage("LOCOMOTION",
		"MOVEMENT AND COMFORT TURNING");
	char rows[VR_LOCOMOTION_ITEM_COUNT][112];
#if MIAMIVR_STEAM_FRAME
	snprintf(rows[VR_LOCOMOTION_VIEW], sizeof(rows[0]),
		"VIEW  < %s >",
		gViewMode == VR_VIEW_THIRD_SCREEN ? "THIRD: 3D SCREEN" :
		gViewMode == VR_VIEW_THIRD_SHOULDER ? "THIRD: OVER SHOULDER" :
		"FIRST PERSON");
#endif
	snprintf(rows[VR_LOCOMOTION_MOVEMENT_ORIENTATION], sizeof(rows[0]),
		"MOVEMENT DIRECTION  < %s >",
		gQuestMovementOrientation ==
			QUEST_MOVEMENT_ORIENTATION_HEAD_TURN_EXPERIMENTAL ?
				"HEAD TURN EXP" :
		gQuestMovementOrientation == QUEST_MOVEMENT_ORIENTATION_HEAD ?
			"HEAD" : "BODY");
#if MIAMIVR_STEAM_FRAME
	snprintf(rows[VR_LOCOMOTION_THIRD_PERSON_MOVEMENT], sizeof(rows[0]),
		"THIRD PERSON MOVE  < %s >",
		gThirdPersonMovement ? "FACE CAMERA" : "FACE MOVE");
#endif
	snprintf(rows[VR_LOCOMOTION_TURN_MODE], sizeof(rows[0]),
		"TURNING  < %s >",
		gQuestTurnMode == QUEST_TURN_SNAP ? "SNAP" : "SMOOTH");
	if(gQuestMovementOrientation ==
	   QUEST_MOVEMENT_ORIENTATION_HEAD_TURN_EXPERIMENTAL)
		snprintf(rows[VR_LOCOMOTION_TURN_SENSITIVITY], sizeof(rows[0]),
			"HEAD TURN SENSITIVITY  < %d%% >",
			gQuestHeadSteeringSensitivityPercent);
	else
		snprintf(rows[VR_LOCOMOTION_TURN_SENSITIVITY], sizeof(rows[0]),
			"SMOOTH TURN SENSITIVITY  < %d%% >",
			gQuestTurnSensitivityPercent);
	snprintf(rows[VR_LOCOMOTION_SNAP_ANGLE], sizeof(rows[0]),
		"SNAP TURN ANGLE  < %d DEG >", gQuestSnapTurnAngleDegrees);
	snprintf(rows[VR_LOCOMOTION_HEAD_BOBBING], sizeof(rows[0]),
		"WALKING HEAD BOB  < %s >", gQuestHeadBobbing ? "ON" : "OFF");
#if MIAMIVR_STEAM_FRAME
	snprintf(rows[VR_LOCOMOTION_CUTSCENES], sizeof(rows[0]),
		"CUTSCENES  < %s >",
		gCutsceneMode == VR_CUTSCENE_STEREO ? "STEREO  R3 CAMERA  L3 KEEP" :
		gCutsceneMode == VR_CUTSCENE_CINEMA_3D_SUBTLE ? "CINEMA 3D  SUBTLE" :
		gCutsceneMode == VR_CUTSCENE_CINEMA_3D ? "CINEMA 3D" :
		gCutsceneMode == VR_CUTSCENE_CINEMA_3D_DEEP ? "CINEMA 3D  DEEP" :
		"CINEMA SCREEN");
#else
	snprintf(rows[VR_LOCOMOTION_CUTSCENES], sizeof(rows[0]),
		"CUTSCENES  < %s >",
		gCutsceneMode ? "STEREO  R3 CAMERA  L3 KEEP" : "CINEMA SCREEN");
#endif
	snprintf(rows[VR_LOCOMOTION_REFRESH_RATE], sizeof(rows[0]),
		"REFRESH RATE  < %d HZ >", gQuestRefreshRateHz);
	strcpy(rows[VR_LOCOMOTION_RECENTER], "RECENTER VIEW");
	strcpy(rows[VR_LOCOMOTION_BACK], "BACK TO SETTINGS");
#if MIAMIVR_STEAM_FRAME
	// Eleven rows: a tighter pitch, with the hint and footer pushed down.
	const int rowPitch = 48, hintY = 704, footerY = 726;
#else
	const int rowPitch = 68, hintY = 668, footerY = 718;
#endif
	for(int item = 0; item < VR_LOCOMOTION_ITEM_COUNT; item++)
		DrawFullVrMenuRow(rows[item], 190+item*rowPitch, 3,
			item == gVrLocomotionSelection);
	// The one setting a standing player can be hurt by without knowing why.
	if(gVrLocomotionSelection == VR_LOCOMOTION_MOVEMENT_ORIENTATION)
		DrawVrMenuText(
			"BODY IGNORES PHYSICAL TURNS - PRESS A TO REACH IT",
			VR_MENU_WIDTH/2, hintY, 2, 255, 205, 90);
#if MIAMIVR_STEAM_FRAME
	else if(gVrLocomotionSelection == VR_LOCOMOTION_VIEW)
		DrawVrMenuText("SHORT PRESS VIEW TO SWITCH IN PLAY",
			VR_MENU_WIDTH/2, hintY, 2, 255, 205, 90);
	else if(gVrLocomotionSelection == VR_LOCOMOTION_THIRD_PERSON_MOVEMENT)
		DrawVrMenuText("THIRD PERSON ONLY - FIRST PERSON STRAFES",
			VR_MENU_WIDTH/2, hintY, 2, 255, 205, 90);
#endif
	DrawVrMenuText(
		"LEFT STICK SELECT   L2 PREVIOUS   R2 OR A NEXT   B BACK",
		VR_MENU_WIDTH/2, footerY, 2, 170, 190, 210);
}

static void
DrawQuestControlsPage(void)
{
	static const char *const layoutNames[VR_CONTROLS_LAYOUT_COUNT] = {
		"DEFAULT", "SWAPPED HANDS", "CUSTOM"
	};
	BeginFullVrMenuPage("CONTROLS",
		"ON FOOT BUTTONS - DRIVING AND WEAPON GESTURES ARE FIXED");
	char rows[VR_CONTROLS_ITEM_COUNT][112];
	snprintf(rows[VR_CONTROLS_LAYOUT], sizeof(rows[0]),
		"LAYOUT  < %s >", layoutNames[CurrentControlsLayout()]);
	for(int item = VR_CONTROLS_A; item <= VR_CONTROLS_RIGHT_STICK_CLICK;
	    item++){
		const int source = VrControlsRowSource(item);
		snprintf(rows[item], sizeof(rows[0]), "%s  < %s >",
			VrPadSourceName(source),
			VrPadTargetName(gVrPadBinding[source]));
	}
	snprintf(rows[VR_CONTROLS_LOOK_BEHIND], sizeof(rows[0]),
		"R3 LOOKS BEHIND  < %s >", gVrStickLookBehind ? "ON" : "OFF");
	snprintf(rows[VR_CONTROLS_CROUCH], sizeof(rows[0]),
		"L3 CROUCHES  < %s >", gVrStickCrouch ? "ON" : "OFF");
	strcpy(rows[VR_CONTROLS_RESET], "RESET TO DEFAULTS");
	strcpy(rows[VR_CONTROLS_BACK], "BACK TO SETTINGS");
	for(int item = 0; item < VR_CONTROLS_ITEM_COUNT; item++)
		DrawFullVrMenuRow(rows[item], 185+item*46, 3,
			item == gVrControlsSelection);
	DrawVrMenuText(
		"LEFT STICK SELECT   L2 PREVIOUS   R2 OR A NEXT   B BACK",
		VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
}

static void
DrawQuestWeaponCalibrationPage(void)
{
	const int weaponType =
		OculusVR::GetQuestCalibrationWeaponType(gVrCalibrationHand);
	char weaponName[48], heading[128];
	CopyMenuText(weaponName, ARRAY_SIZE(weaponName),
		GetVrWeaponName(weaponType));
	snprintf(heading, sizeof(heading),
		"WEAPON CALIBRATION - %s - %s HAND", weaponName,
		gVrCalibrationHand == 0 ? "LEFT" : "RIGHT");
	BeginFullVrMenuPage(heading,
		"VALUES ARE SAVED PER WEAPON AND PER HAND");
	static const char *labels[19] = {
		"AIM X OFFSET", "AIM Y OFFSET", "AIM Z OFFSET",
		"AIM LOCAL ROT X", "AIM LOCAL ROT Y", "AIM LOCAL ROT Z",
		"WEAPON X OFFSET", "WEAPON Y OFFSET", "WEAPON Z OFFSET",
		"WEAPON LOCAL ROT X", "WEAPON LOCAL ROT Y",
		"WEAPON LOCAL ROT Z", "SUPPORT GRIP X",
		"SUPPORT GRIP Y", "SUPPORT GRIP Z",
		"SUPPORT GRIP ROT X", "SUPPORT GRIP ROT Y",
		"SUPPORT GRIP ROT Z", "SUPPORT GRIP STYLE"
	};
	for(int item = 0; item < 22; item++){
		char row[112];
		if(item == 0)
			snprintf(row, sizeof(row), "EDIT HAND  < %s >",
				gVrCalibrationHand == 0 ? "LEFT" : "RIGHT");
		else if(item == 20){
			const int laser =
				OculusVR::GetQuestWeaponLaserOverride(weaponType);
			snprintf(row, sizeof(row), "LASER SIGHT  < %s >",
				laser < 0 ? "FOLLOW WEAPONS PAGE" :
					(laser ? "ON" : "OFF"));
		}
		else if(item <= 19){
			const int value =
				OculusVR::GetQuestCalibrationValue(
					gVrCalibrationHand, weaponType, item-1);
			if(item == 19)
				snprintf(row, sizeof(row), "%s  < %s >",
					labels[item-1], value == 1 ?
						"FROM BELOW" : "MAGAZINE");
			else
				snprintf(row, sizeof(row), "%s  < %+.1f %s >",
					labels[item-1], (float)value/2.0f,
					((item >= 4 && item <= 6) ||
					 (item >= 10 && item <= 12) ||
					 (item >= 16 && item <= 18)) ?
						"DEG" : "CM");
		}else
			strcpy(row, "BACK TO SETTINGS");
		DrawFullVrMenuRow(row, 154+item*25, 2,
			item == gVrCalibrationSelection);
	}
	DrawVrMenuText(
		"LEFT STICK SELECT   L2 MINUS   R2 OR A PLUS   B BACK",
		VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
}

static void
DrawQuestHolsterPage(void)
{
	BeginFullVrMenuPage("HOLSTER LOADOUT",
		"SEVEN BODY POINTS - CENTER THROWABLE SLOT IS FIXED");
	const int points = OculusVR::GetQuestHolsterPointCount();
	for(int item = 0; item <= points; item++){
		char row[128];
		if(item < points){
			const int slot =
				OculusVR::GetQuestHolsterPointSlot(item);
			const char *slotName = slot < 0 ? "EMPTY" :
				GetVrWeaponName(GetVrWeaponTypeForSlot(slot));
			snprintf(row, sizeof(row), "%s  %c %s %c",
				OculusVR::GetQuestHolsterPointName(item),
				item == 4 ? '[' : '<', slotName,
				item == 4 ? ']' : '>');
		}else
			strcpy(row, "BACK TO SETTINGS");
		DrawFullVrMenuRow(row, 180+item*55, 3,
			item == gVrHolsterSelection);
	}
	DrawVrMenuText(
		"CLEAR THE OLD POINT BEFORE MOVING AN ASSIGNED SLOT",
		VR_MENU_WIDTH/2, 650, 2, 255, 180, 225);
	DrawVrMenuText(
		"LEFT STICK SELECT   L2 PREVIOUS   R2 OR A NEXT   B BACK",
		VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
}

static void
DrawQuestCheatPage(void)
{
	if(gVrCheatCategory < 0){
		BeginFullVrMenuPage("CHEATS", "CHOOSE A CATEGORY");
		for(int item = 0; item < VrCheatMenu::CATEGORY_COUNT+2; ++item){
			char row[96];
			if(item < VrCheatMenu::CATEGORY_COUNT)
				snprintf(row, sizeof(row), "%s  < OPEN >",
					VrCheatMenu::CategoryName(item));
			else
				snprintf(row, sizeof(row), "%s",
					item == VrCheatMenu::CATEGORY_COUNT ?
					"MISSION SELECTOR  < OPEN >" : "BACK TO SETTINGS");
			DrawFullVrMenuRow(row, 166+item*49, 2,
				item == gVrCheatCategorySelection);
		}
		DrawVrMenuText("STICK UP/DOWN SELECT   A OPEN   B BACK TO SETTINGS",
			VR_MENU_WIDTH/2, 718, 2, 170, 190, 210);
		return;
	}
	char heading[96];
	snprintf(heading, sizeof(heading), "CHEATS / %s",
		VrCheatMenu::CategoryName(gVrCheatCategory));
	BeginFullVrMenuPage(heading);
	const int cheatCount = VrCheatMenu::Count(gVrCheatCategory,
		GetVrCheatCount());
	const int count = cheatCount+1;
	const int first = count > 0 ?
		(gVrCheatSelection/VR_CHEAT_ITEMS_PER_PAGE)*
			VR_CHEAT_ITEMS_PER_PAGE : 0;
	for(int row = 0;
	    row < VR_CHEAT_ITEMS_PER_PAGE && first+row < count;
	    row++){
		const int item = first+row;
		const int source = VrCheatMenu::SourceIndex(gVrCheatCategory,
			item, GetVrCheatCount());
		const char *name = source < 0 ?
			"BACK TO CHEAT CATEGORIES" : GetVrCheatName(source);
		bool toggleEnabled = false;
		const bool isToggle = source >= 0 &&
			GetVrCheatToggleState(source, &toggleEnabled);
		DrawFullVrMenuRow(name, 158+row*37, 2,
			item == gVrCheatSelection, true,
			isToggle && !toggleEnabled, isToggle && toggleEnabled);
	}
	char page[48];
	const int pageCount = Max(1,
		(count+VR_CHEAT_ITEMS_PER_PAGE-1)/VR_CHEAT_ITEMS_PER_PAGE);
	snprintf(page, sizeof(page), "PAGE %d OF %d",
		first/VR_CHEAT_ITEMS_PER_PAGE+1, pageCount);
	DrawVrMenuText(page, VR_MENU_WIDTH/2, 680, 2,
		120, 220, 255);
	DrawVrMenuText(
		gVrCheatStatusFrames > 0 ? gVrCheatStatus :
			"STICK UP/DOWN SELECT   LEFT/RIGHT MODEL   A ACTIVATE   B BACK",
		VR_MENU_WIDTH/2, 718, 2,
		gVrCheatStatusFrames > 0 ? 255 : 170,
		gVrCheatStatusFrames > 0 ? 220 : 190,
		gVrCheatStatusFrames > 0 ? 80 : 210);
}

static void
DrawQuestMissionPage(void)
{
	BeginFullVrMenuPage("CHEATS / MISSIONS",
		"TEST TOOL - STARTS IN THE CURRENT SAVE STATE");
	if(gVrMissionCategory < 0){
		const int count = GetVrMissionCategoryCount();
		for(int item = 0; item < count; item++){
			char row[112];
			snprintf(row, sizeof(row), "%s  < OPEN >",
				GetVrMissionCategoryName(item));
			DrawFullVrMenuRow(row, 235+item*90, 3,
				item == gVrMissionCategorySelection);
		}
		DrawVrMenuText("MISSION NUMBERS COME FROM THE ACTIVE MAIN.SCM TABLE",
			VR_MENU_WIDTH/2, 610, 2, 255, 180, 225);
		DrawVrMenuText(gVrCheatStatusFrames > 0 ? gVrCheatStatus :
			"UP/DOWN SELECT   A OPEN   B BACK TO CHEATS",
			VR_MENU_WIDTH/2, 718, 2,
			gVrCheatStatusFrames > 0 ? 255 : 170,
			gVrCheatStatusFrames > 0 ? 220 : 190,
			gVrCheatStatusFrames > 0 ? 80 : 210);
		return;
	}

	DrawVrMenuText(GetVrMissionCategoryName(gVrMissionCategory),
		VR_MENU_WIDTH/2, 132, 2, 255, 180, 225);
	const int count = GetVrMissionCount(gVrMissionCategory);
	const int first = (gVrMissionSelection/VR_CHEAT_ITEMS_PER_PAGE)*
		VR_CHEAT_ITEMS_PER_PAGE;
	for(int row = 0; row < VR_CHEAT_ITEMS_PER_PAGE && first+row < count;
	    row++){
		const int item = first+row;
		DrawFullVrMenuRow(GetVrMissionName(gVrMissionCategory, item),
			154+row*34, 2, item == gVrMissionSelection);
	}
	char page[48];
	const int pageCount = Max(1,
		(count+VR_CHEAT_ITEMS_PER_PAGE-1)/VR_CHEAT_ITEMS_PER_PAGE);
	snprintf(page, sizeof(page), "PAGE %d OF %d",
		first/VR_CHEAT_ITEMS_PER_PAGE+1, pageCount);
	DrawVrMenuText(page, VR_MENU_WIDTH/2, 650, 2, 120, 220, 255);
	DrawVrMenuText(gVrCheatStatusFrames > 0 ? gVrCheatStatus :
		"A STARTS THE MISSION   B RETURNS TO CATEGORIES",
		VR_MENU_WIDTH/2, 718, 2,
		gVrCheatStatusFrames > 0 ? 255 : 170,
		gVrCheatStatusFrames > 0 ? 220 : 190,
		gVrCheatStatusFrames > 0 ? 80 : 210);
}

static void
DrawQuestAboutPage(void)
{
	BeginFullVrMenuPage(gVrWelcomeFirstRun ?
		"WELCOME TO VICE CITY VR" : "ABOUT VICE CITY VR",
		"VERSION " MIAMIVR_VERSION_TEXT " - NOT FOR SALE");
	static const char *lines[] = {
		"OPEN THE VR MENU: HOLD BOTH GRIPS + MENU",
		"CHEATS: VR MENU OR ON FOOT BOTH GRIPS + B",
		"CHOOSE IMMERSIVE DRIVING IN VEHICLE SETTINGS",
		"CALIBRATE WEAPONS IF A MODEL OR GRIP IS MISALIGNED",
		"MODEL ASSETS CAN MIX CLASSIC AND MODERN CONTENT",
		"CLASSIC VEGETATION IS RECOMMENDED FOR " VR_PLATFORM_NAME " PERFORMANCE",
		"MODERN VEHICLES + HIGH TRAFFIC CAN HEAVILY LOAD THE GPU",
		"AUTHORED CULLING HELPS PERFORMANCE BUT MAY SHOW RARE ARTIFACTS",
		"IF EYES DISAGREE: GRAPHICS > OCCLUSION > STEREO SAFE OR OFF",
		"PHYSICS DIRECTOR V2 IS EXPERIMENTAL",
		"IF TRAFFIC MISBEHAVES: TRAFFIC > PHYSICS DIRECTOR > ORIGINAL",
		"THE FULL CAMPAIGN IS PLAYABLE, BUT THE MOD IS STILL IN DEVELOPMENT",
		"DISCUSSION: FLAT2VR DISCORD",
		"discord.com/channels/747967102895390741/1543691482861408276",
		"PRESS ANY BUTTON TO CLOSE"
	};
	for(int index = 0; index < (int)ARRAY_SIZE(lines); index++){
		const bool warning = index >= 6 && index <= 10;
		const bool close = index == (int)ARRAY_SIZE(lines)-1;
		DrawVrMenuText(lines[index], VR_MENU_WIDTH/2, 170+index*32,
			2, close ? 255 : (warning ? 255 : 215),
			close ? 205 : (warning ? 145 : 225),
			close ? 80 : (warning ? 70 : 235));
	}
}

static const char *
SpawnDenyReasonName(int reason)
{
	static const char *const names[16] = {
		"MODEL", "COORS", "DICE", "COLLIDE", "CLOSE", "LANES",
		"NOWHERE", "GROUND", "FAR", "VIEW", "APPROACH", "COPS",
		"OK", "CELL", "POOL", "DEMAND"
	};
	return reason >= 0 && reason < 16 ? names[reason] : "NONE";
}

// Fills the same 1024x768 menu surface as desktop. Compact diagnostics use
// 512x128 for the FPS line and 512x437 for the full profiler.
const unsigned char *
VrDebugPixels(int *width, int *height)
{
	const bool profilerVisible = QuestProfilerIsEnabled();
	*width = gVrMenuVisible ? VR_MENU_WIDTH : VR_DEBUG_WIDTH;
	*height = gVrMenuVisible ? VR_MENU_HEIGHT :
		(profilerVisible ? VR_DEBUG_HEIGHT : VR_FPS_HEIGHT);
	if(!gDebugVisible && !gVrMenuVisible && !profilerVisible)
		return nil;

	if(gVrMenuVisible){
		switch(gVrMenuPage){
		case VR_MENU_PAGE_GRAPHICS:
			DrawQuestGraphicsPage();
			break;
		case VR_MENU_PAGE_LIGHTING:
			DrawQuestLightingPage();
			break;
		case VR_MENU_PAGE_WATER:
			DrawQuestWaterPage();
			break;
		case VR_MENU_PAGE_WEAPONS:
			DrawQuestWeaponsPage();
			break;
		case VR_MENU_PAGE_HUD:
			DrawQuestHudPage();
			break;
		case VR_MENU_PAGE_WRIST_RADAR:
			DrawQuestWristRadarPage();
			break;
		case VR_MENU_PAGE_TRAFFIC:
			DrawQuestTrafficPage();
			break;
		case VR_MENU_PAGE_RAGDOLL:
			DrawQuestRagdollPage();
			break;
		case VR_MENU_PAGE_DEFORMATION:
			DrawQuestDeformationPage();
			break;
		case VR_MENU_PAGE_MODEL_ASSETS:
			DrawQuestModelAssetsPage();
			break;
		case VR_MENU_PAGE_VEHICLE:
			DrawQuestVehiclePage();
			break;
		case VR_MENU_PAGE_VEHICLE_CALIBRATION:
			DrawQuestVehicleCalibrationPage();
			break;
		case VR_MENU_PAGE_LOCOMOTION:
			DrawQuestLocomotionPage();
			break;
		case VR_MENU_PAGE_CONTROLS:
			DrawQuestControlsPage();
			break;
		case VR_MENU_PAGE_CALIBRATION:
			DrawQuestWeaponCalibrationPage();
			break;
		case VR_MENU_PAGE_HOLSTERS:
			DrawQuestHolsterPage();
			break;
		case VR_MENU_PAGE_CHEATS:
			DrawQuestCheatPage();
			break;
		case VR_MENU_PAGE_MISSIONS:
			DrawQuestMissionPage();
			break;
		case VR_MENU_PAGE_ABOUT:
			DrawQuestAboutPage();
			break;
		default:
			DrawQuestSettingsPage();
			break;
		}
		return gVrMenuPixels;
	}

	for(int pixel = 0; pixel < VR_DEBUG_WIDTH*(*height); pixel++){
		gDebugPixels[pixel*4+0] = 5; gDebugPixels[pixel*4+1] = 4;
		gDebugPixels[pixel*4+2] = 12; gDebugPixels[pixel*4+3] = 225;
	}

	char value[128];
	if(!profilerVisible){
		sprintf(value, "OPENXR FPS:%d", gDebugFps);
		DrawDebugText(value, VR_DEBUG_WIDTH/2, 10, 3,
			255, 230, 64);
		return gDebugPixels;
	}

	const QuestProfilerSnapshot perf = QuestProfilerGetSnapshot();
	sprintf(value, "FPS:%.1f FRAME:%.2f BUDGET:%.2f",
		perf.appFps > 0.0f ? perf.appFps : (float)gDebugFps,
		perf.cpuAppMs, perf.frameBudgetMs);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 1, 2,
		perf.cpuAppMs > perf.frameBudgetMs ? 255 : 100,
		perf.cpuAppMs > perf.frameBudgetMs ? 90 : 230, 120);
	sprintf(value, "CPU APP:%.2f MAX:%.2f STEP:%.2f",
		perf.cpuAppMs, perf.cpuAppMaxMs, perf.cpuStepMs);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 16, 2,
		perf.cpuAppMs > perf.frameBudgetMs ? 255 : 100,
		perf.cpuAppMs > perf.frameBudgetMs ? 90 : 230, 120);
	sprintf(value, "GAME:%.2f/%.2f RES:%.2f/%.2f STR:%.2f",
		perf.cpuGameMs, perf.cpuGameMaxMs,
		perf.cpuSimOtherMs, perf.cpuSimOtherMaxMs,
		perf.cpuStreamingMs);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 31, 2,
		perf.cpuGameMs > perf.frameBudgetMs ? 255 : 100,
		perf.cpuGameMs > perf.frameBudgetMs ? 90 : 230, 120);
	sprintf(value, "SCR:%.2f PRE:%.2f WRLD:%.2f POST:%.2f",
		perf.cpuScriptMs, perf.cpuPreWorldMiscMs,
		perf.cpuWorldProcessMs, perf.cpuPostWorldCameraFxMs);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 46, 2,
		120, 210, 255);
	sprintf(value, "PED A/C/P/COL/S/X:%.2f/%.2f/%.2f/%.2f/%.2f/%.2f",
		perf.cpuWorldEntityPhaseMs[QUEST_WORLD_ENTITY_PED]
			[QUEST_WORLD_SIM_PHASE_ANIMATION],
		perf.cpuWorldEntityPhaseMs[QUEST_WORLD_ENTITY_PED]
			[QUEST_WORLD_SIM_PHASE_CONTROL],
		perf.cpuWorldEntityPhaseMs[QUEST_WORLD_ENTITY_PED]
			[QUEST_WORLD_SIM_PHASE_POSTPONED_CONTROL],
		perf.cpuWorldEntityPhaseMs[QUEST_WORLD_ENTITY_PED]
			[QUEST_WORLD_SIM_PHASE_COLLISION],
		perf.cpuWorldEntityPhaseMs[QUEST_WORLD_ENTITY_PED]
			[QUEST_WORLD_SIM_PHASE_SHIFT],
		perf.cpuWorldEntityPhaseMs[QUEST_WORLD_ENTITY_PED]
			[QUEST_WORLD_SIM_PHASE_TRANSFORM]);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 61, 2,
		120, 210, 255);
	sprintf(value, "VEH A/C/P/COL/S/X:%.2f/%.2f/%.2f/%.2f/%.2f/%.2f",
		perf.cpuWorldEntityPhaseMs[QUEST_WORLD_ENTITY_VEHICLE]
			[QUEST_WORLD_SIM_PHASE_ANIMATION],
		perf.cpuWorldEntityPhaseMs[QUEST_WORLD_ENTITY_VEHICLE]
			[QUEST_WORLD_SIM_PHASE_CONTROL],
		perf.cpuWorldEntityPhaseMs[QUEST_WORLD_ENTITY_VEHICLE]
			[QUEST_WORLD_SIM_PHASE_POSTPONED_CONTROL],
		perf.cpuWorldEntityPhaseMs[QUEST_WORLD_ENTITY_VEHICLE]
			[QUEST_WORLD_SIM_PHASE_COLLISION],
		perf.cpuWorldEntityPhaseMs[QUEST_WORLD_ENTITY_VEHICLE]
			[QUEST_WORLD_SIM_PHASE_SHIFT],
		perf.cpuWorldEntityPhaseMs[QUEST_WORLD_ENTITY_VEHICLE]
			[QUEST_WORLD_SIM_PHASE_TRANSFORM]);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 76, 2,
		120, 210, 255);
	sprintf(value, "MGR P:%.2f C:%.2f RENDER:%.2f UI:%.2f",
		perf.cpuPopulationMs, perf.cpuCarMs,
		perf.cpuWorldRenderMs, perf.cpuUiMs);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 91, 2,
		120, 210, 255);
	const float limitingGpuMs = perf.gpuFrameRuntime && perf.gpuVulkanValid ?
		Max(perf.gpuFrameMs, perf.gpuVulkanMs) : perf.gpuFrameMs;
	if(perf.gpuFrameRuntime && perf.gpuVulkanValid)
		sprintf(value, "GPU META:%.2f VK:%.2f LIMIT:%.2f",
			perf.gpuFrameMs, perf.gpuVulkanMs, limitingGpuMs);
	else if(perf.gpuFrameValid)
		sprintf(value, "GPU:%.2f [%s]", perf.gpuFrameMs,
			perf.gpuFrameRuntime ? "META" : "VK");
	else
		sprintf(value, "GPU:N/A VK:N/A");
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 106, 2,
		perf.gpuFrameValid && limitingGpuMs > perf.frameBudgetMs ? 255 : 100,
		perf.gpuFrameValid && limitingGpuMs > perf.frameBudgetMs ? 90 : 210, 255);

	// The local APP bracket includes beginFrame's previous-GPU fence wait.
	// Do not call that a CPU bottleneck when META is unavailable; CPU STEP is
	// the wait-free game/recording workload in that fallback case.
	const float cpuMs = perf.cpuAppRuntime ?
		perf.cpuAppMs : perf.cpuStepMs;
	const char *bottleneck = "UNKNOWN";
	if(perf.gpuFrameValid){
		if(cpuMs < perf.frameBudgetMs*0.8f &&
		   limitingGpuMs < perf.frameBudgetMs*0.8f)
			bottleneck = "HEADROOM";
		else if(cpuMs > limitingGpuMs+0.5f)
			bottleneck = "CPU";
		else if(limitingGpuMs > cpuMs+0.5f)
			bottleneck = "GPU";
		else
			bottleneck = "BALANCED";
	}
	sprintf(value, "PART U:%.2f R:%.2f N:%d H:%d",
		perf.cpuParticleUpdateMs, perf.cpuParticleRenderMs,
		perf.particleActive, perf.heliDustActive);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 121, 2,
		180, 190, 200);
	sprintf(value, "STREAM Q:%d P:%d MEM:%llu/%lluM",
		perf.streamingRequested, perf.streamingPriority,
		(unsigned long long)(perf.streamingMemoryUsed/(1024*1024)),
		(unsigned long long)(perf.streamingMemoryAvailable/(1024*1024)));
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 136, 2,
		120, 210, 255);
	sprintf(value, "PED:%d/%.1f CAP:%d POOL:%d/%d",
		perf.ambientPeds, perf.targetAmbientPeds,
		perf.ambientPedCap, perf.pedPoolUsed, perf.pedPoolSize);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 151, 2,
		120, 210, 255);
	sprintf(value, "POOL V:%d/%d O:%d/%d E:%d/%d",
		perf.vehiclePoolUsed, perf.vehiclePoolSize,
		perf.objectPoolUsed, perf.objectPoolSize,
		perf.entryInfoPoolUsed, perf.entryInfoPoolSize);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 166, 2,
		120, 210, 255);
	sprintf(value, "CFG:%d/%d WANT:%.1f SERV:%.1f",
		perf.pedTrafficPercent, perf.carTrafficPercent,
		perf.trafficDesired, perf.trafficServed);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 181, 2,
		255, 220, 100);
	sprintf(value, "AMB:%d COP:%d PROXY:%d PEND:%d",
		perf.trafficAmbient, perf.trafficPursuit,
		perf.trafficProxies, perf.trafficPending);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 196, 2,
		255, 220, 100);
	sprintf(value, "DEB:%d STL:%d BLK:%d DIR:%.2f/%.2f",
		perf.trafficDebris, perf.trafficStalled,
		perf.trafficPoolReserveBlocks,
		perf.trafficDirectorMs, perf.trafficDirectorMaxMs);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 211, 2,
		255, 180, 100);
	sprintf(value, "SPAWN:%d OK:%d DENY:%d TOP:%s:%d",
		perf.spawnAttemptsPerSecond, perf.spawnSuccessPerSecond,
		perf.spawnDeniedPerSecond,
		SpawnDenyReasonName(perf.spawnTopDenyReason),
		perf.spawnTopDenyCount);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 226, 2,
		perf.spawnDeniedPerSecond > perf.spawnSuccessPerSecond ? 255 : 220,
		perf.spawnDeniedPerSecond > perf.spawnSuccessPerSecond ? 90 : 180,
		120);
	sprintf(value, "JOB D:%d S:%d H:%d M:%d",
		perf.trafficJobDispatchesPerSecond,
		perf.trafficJobSkipsPerSecond,
		perf.trafficJobHitsPerSecond, perf.trafficJobMissesPerSecond);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 241, 2,
		perf.trafficJobHitsPerSecond > perf.trafficJobMissesPerSecond ? 100 : 255,
		perf.trafficJobHitsPerSecond > perf.trafficJobMissesPerSecond ? 230 : 120,
		160);
	sprintf(value, "JOB W:%.2f/%.2f B:%.2f/%.2f",
		perf.trafficJobMs, perf.trafficJobMaxMs,
		perf.trafficJobBuildMs, perf.trafficJobBuildMaxMs);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 256, 2,
		140, 215, 255);
	sprintf(value, "FLAT S/B/R:%d/%d/%d F:%d I:%d SAVE:%d",
		perf.collisionFlatScopesPerSecond,
		perf.collisionFlatBuildsPerSecond,
		perf.collisionFlatReusesPerSecond,
		perf.collisionFlatStalePerSecond+
			perf.collisionFlatOverflowsPerSecond,
		perf.collisionFlatItemsPerSecond,
		perf.collisionFlatSavedNodeVisitsPerSecond);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 271, 2,
		perf.collisionFlatStalePerSecond == 0 &&
			perf.collisionFlatOverflowsPerSecond == 0 ? 100 : 255,
		perf.collisionFlatStalePerSecond == 0 &&
			perf.collisionFlatOverflowsPerSecond == 0 ? 230 : 120,
		160);
	sprintf(value, "R SKY:%.2f RD:%.2f WRLD:%.2f WTR:%.2f",
		perf.cpuRenderSkyMs, perf.cpuRenderRoadsMs,
		perf.cpuRenderWorldMs, perf.cpuRenderWaterMs);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 286, 2,
		140, 215, 255);
	sprintf(value, "R REF:%.2f FAD:%.2f WTH:%.2f FX:%.2f",
		perf.cpuRenderReflectionsMs, perf.cpuRenderFadingMs,
		perf.cpuRenderWeatherMs, perf.cpuRenderEffectsMs);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 301, 2,
		140, 215, 255);
	sprintf(value, "R OTHER:%.2f/%.2f TOTAL:%.2f/%.2f",
		perf.cpuRenderOtherMs, perf.cpuRenderOtherMaxMs,
		perf.cpuWorldRenderMs, perf.cpuWorldRenderMaxMs);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 316, 2,
		140, 215, 255);
	sprintf(value, "STATIC:%d RD:%d OCC:%d D:%d/%d/%d",
		perf.visibleBuildings, perf.visibleRoads,
		perf.activeOccluders,
		perf.renderEntityCalls[QUEST_RENDER_ENTITY_VEHICLE],
		perf.renderEntityCalls[QUEST_RENDER_ENTITY_PED],
		perf.renderEntityCalls[QUEST_RENDER_ENTITY_STATIC]);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 331, 2,
		140, 215, 255);
	sprintf(value, "RFAD V:%.2f/%d P:%.2f/%d S:%.2f/%d",
		perf.cpuRenderFadingEntityMs[QUEST_RENDER_ENTITY_VEHICLE],
		perf.renderFadingEntityCalls[QUEST_RENDER_ENTITY_VEHICLE],
		perf.cpuRenderFadingEntityMs[QUEST_RENDER_ENTITY_PED],
		perf.renderFadingEntityCalls[QUEST_RENDER_ENTITY_PED],
		perf.cpuRenderFadingEntityMs[QUEST_RENDER_ENTITY_STATIC],
		perf.renderFadingEntityCalls[QUEST_RENDER_ENTITY_STATIC]);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 346, 2,
		140, 215, 255);
	sprintf(value, "RV %.2f/%.2f/%.2f/%.2f V:%d/%d S:%d O:%d",
		perf.cpuVehicleRenderPhaseMs[QUEST_VEHICLE_RENDER_PHASE_LIGHTING],
		perf.cpuVehicleRenderPhaseMs[QUEST_VEHICLE_RENDER_PHASE_OCCUPANTS],
		perf.cpuVehicleRenderPhaseMs[QUEST_VEHICLE_RENDER_PHASE_BODY_SUBMIT],
		perf.cpuVehicleRenderPhaseMs[QUEST_VEHICLE_RENDER_PHASE_ALPHA_ATOMICS],
		perf.vehicleVisualHighPerSecond,
		perf.vehicleVisualVloPerSecond,
		perf.vehicleVisualAtomicsSkippedPerSecond,
		perf.vehicleVisualOccupantsSkippedPerSecond);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 361, 2,
		140, 215, 255);
	gQuestCpuPerformanceMode = Min(Max(xrvk::getPerformanceMode(),
		(int)QUEST_CPU_PERFORMANCE_AUTO),
		(int)QUEST_CPU_PERFORMANCE_COUNT-1);
	if(!xrvk::isPerformanceModeSupported())
		sprintf(value, "CPU PERF:%s UNSUPPORTED",
			QuestCpuPerformanceModeName(gQuestCpuPerformanceMode));
	else if(xrvk::isPerformanceBoostBlocked())
		sprintf(value, "CPU PERF:%s THERMAL BLOCKED",
			QuestCpuPerformanceModeName(gQuestCpuPerformanceMode));
	else if(xrvk::getActivePerformanceMode() < 0)
		sprintf(value, "CPU PERF REQ:%s ACTIVE:PENDING",
			QuestCpuPerformanceModeName(gQuestCpuPerformanceMode));
	else
		sprintf(value, "CPU PERF REQ:%s ACTIVE:%s",
			QuestCpuPerformanceModeName(gQuestCpuPerformanceMode),
			QuestCpuPerformanceModeName(
				xrvk::getActivePerformanceMode()));
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 376, 2,
		180, 205, 120);
	sprintf(value, "PHY M:%d F/RD/RL/P:%d/%d/%d/%d COL:%.2f",
		perf.physicsDirectorMode, perf.physicsFull,
		perf.physicsReduced, perf.physicsRail, perf.physicsProxy,
		perf.cpuWorldEntityPhaseMs[QUEST_WORLD_ENTITY_VEHICLE]
			[QUEST_WORLD_SIM_PHASE_COLLISION]);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 391, 2,
		120, 230, 185);
	sprintf(value, "PHY C/S/K:%d/%d/%d N/P:%d/%d",
		perf.physicsCheckCollisionPerSecond,
		perf.physicsCheckSimplePerSecond,
		perf.physicsSimpleSkippedPerSecond,
		perf.physicsSectorNodesPerSecond,
		perf.physicsPairsAfterFilteringPerSecond);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 406, 2,
		120, 230, 185);
	sprintf(value, "PHY CM/W/T:%d/%d/%d ST/R:%d/%d",
		perf.physicsProcessColModelsPerSecond,
		perf.physicsWheelLineTestsPerSecond,
		perf.physicsTriangleTestsPerSecond,
		perf.physicsExtraSubstepsPerSecond,
		perf.physicsRetryPassesPerSecond);
	DrawDebugText(value, VR_DEBUG_WIDTH/2, 421, 2,
		120, 230, 185);
	(void)bottleneck;
	/* LIMIT remains available in the CSV through CPU/GPU/budget columns; the
	 * compact screen spends its last lines on actionable traffic diagnostics. */
	return gDebugPixels;
}

bool
VrMenuConsumesInput(void)
{
	// Keep the service chord from leaking through as Pause when the menu is
	// opened or closed. Gameplay resumes only after Menu has been released.
	return gVrMenuVisible || gVrMenuShortcutDown;
}

bool
VrViceCityColorEnabled(void)
{
	LoadVrSettings();
	return gViceCityColorEnabled;
}

int
VrCutsceneCamera(void)
{
	return gCutsceneCamera;
}

// 1 when cutscenes are rendered immersively (STEREO), else 0: every cinema
// mode, 3D or not, is the theater screen as far as the callers are concerned.
int
VrCutsceneMode(void)
{
	LoadVrSettings();
	return gCutsceneMode == VR_CUTSCENE_STEREO ? 1 : 0;
}

#if MIAMIVR_STEAM_FRAME
// How deep the 3D cinema picture goes: the separation of the two images at
// infinity, as a fraction of the viewer's IPD (0 = flat). Under 1 keeps the
// eyes from diverging; the far background lands at screen distance / (1-k).
float
VrCinema3DDepth(void)
{
	LoadVrSettings();
	switch(gCutsceneMode){
	case VR_CUTSCENE_CINEMA_3D_SUBTLE: return 0.35f;
	case VR_CUTSCENE_CINEMA_3D:        return 0.55f;
	case VR_CUTSCENE_CINEMA_3D_DEEP:   return 0.75f;
	}
	return 0.0f;
}

int
VrViewMode(void)
{
	LoadVrSettings();
	return gViewMode;
}

bool
VrThirdPersonScreen(void)
{
	return VrViewMode() == VR_VIEW_THIRD_SCREEN;
}

bool
VrThirdPersonFacesMovement(void)
{
	LoadVrSettings();
	return gThirdPersonMovement == 0;
}

bool
VrThirdPersonShoulder(void)
{
	return VrViewMode() == VR_VIEW_THIRD_SHOULDER;
}

void
VrToggleThirdPerson(void)
{
	LoadVrSettings();
	SetViewMode(gViewMode == VR_VIEW_FIRST_PERSON ?
		gThirdPersonStyle : (int)VR_VIEW_FIRST_PERSON);
}
#endif

int
VrFoliageSoftness(void)
{
	LoadVrSettings();
	return gFoliageSoftness;
}

bool
VrRenderDiagnostics(void)
{
	LoadVrSettings();
	return gRenderDiagnostics != 0;
}

bool
VrPs2AlphaTest(void)
{
	LoadVrSettings();
	return gPs2AlphaTest != 0;
}

bool
VrGenerateMipmaps(void)
{
	LoadVrSettings();
	return gGenerateMipmaps != 0;
}

// Hands the backend this frame's dynamic lights, nearest first. Read before
// Step runs, so the list is the one the previous frame completed --
// InitPerFrame clears it early in Idle and the entities refill it during
// simulation and PreRender. One frame of latency on a light pool is
// invisible; reading a half-built list is not.
void
VrPushDynamicLights(void)
{
	LoadVrSettings();
	rw::vulkan::setCarReflectionsEnabled(gCarReflections);
	rw::vulkan::setCarReflectionParams(gCarReflectionIntensity/100.0f,
		gCarReflectionSsr/100.0f,
		gCarReflectionSsrDistance == 0 ?
			100000.0f : (float)gCarReflectionSsrDistance);
	rw::vulkan::setModernWaterEnabled(gModernWater);
	rw::vulkan::setWaterParams(gWaterWaves/100.0f, gWaterSpeed/100.0f,
		gWaterDistortion/100.0f, gWaterReflection/100.0f,
		gWaterSheen/100.0f, gWaterGlint/100.0f, gWaterSparks/100.0f);
	// Game time, so the ripples freeze with the pause menu. Wrapped as
	// milliseconds, before the float: past four hours of play a 32-bit
	// millisecond count no longer fits a float mantissa exactly, and
	// wrapping after the conversion would have left the ripples
	// stepping in whole frames.
	rw::vulkan::setEffectsTime(
		(float)(CTimer::GetTimeInMilliseconds() % 3600000u)*0.001f);
	const CVector &sunDirection = CTimeCycle::GetSunDirection();
	rw::vulkan::setWaterSun(&sunDirection.x,
		CTimeCycle::GetSunCoreRed()/255.0f,
		CTimeCycle::GetSunCoreGreen()/255.0f,
		CTimeCycle::GetSunCoreBlue()/255.0f);
	// The vehicle the player sits in, if any: its bonnet is the least
	// rewarding case a screen-space lookup has -- it mostly finds the car
	// itself -- so the backend keeps that one car on the panorama layer.
	const bool carSsrActive = gCarReflections &&
		gCarReflectionIntensity > 0 && gCarReflectionSsr > 0;
	CVehicle *playerVehicle = carSsrActive ? FindPlayerVehicle() : nil;
	if(playerVehicle != nil){
		const CVector vehiclePosition = playerVehicle->GetPosition();
		rw::vulkan::setPlayerVehicle(&vehiclePosition.x,
			playerVehicle->GetColModel()->boundingSphere.radius, true);
	}else
		rw::vulkan::setPlayerVehicle(nil, 0.0f, false);
	if(!gDynamicLights){
		rw::vulkan::setPointLights(nil, 0, 0.0f);
		return;
	}

	struct Candidate { float distance; int index; };
	Candidate candidates[NUMPOINTLIGHTS];
	int candidateCount = 0;
	const CVector camPos = TheCamera.GetPosition();
	for(int i = 0; i < CPointLights::NumLights; i++){
		const CRegisteredPointLight &light = CPointLights::aLights[i];
		// Fog-only entries feed CPointLights::RenderFogEffect, and DARKEN
		// scales the ambient down instead of adding anything; neither is a
		// light source the shaders could apply.
		if(light.type != CPointLights::LIGHT_POINT &&
		   light.type != CPointLights::LIGHT_DIRECTIONAL)
			continue;
		// Street lamps that are off still register a black light for their
		// static shadow; skip those and anything degenerate.
		if(light.radius <= 0.0f ||
		   (light.red <= 0.0f && light.green <= 0.0f && light.blue <= 0.0f))
			continue;
		Candidate candidate;
		candidate.distance =
			(light.coors - camPos).Magnitude() - light.radius;
		candidate.index = i;
		candidates[candidateCount++] = candidate;
	}

	// Selection hysteresis. With a small MAX LIGHTS the runner-up scores sit
	// close together, and while driving the winners traded places every few
	// frames -- the cabin pulsed between the tail light's red and the street
	// lamps' white. A light keeps its seat until a challenger beats it by a
	// clear margin; matching is by position, loose enough to follow the
	// player's own lights down the road.
	static CVector heldPicks[rw::vulkan::MAX_POINT_LIGHTS];
	static int heldPickCount;
	for(int i = 0; i < candidateCount; i++){
		const CVector &coors =
			CPointLights::aLights[candidates[i].index].coors;
		for(int j = 0; j < heldPickCount; j++)
			if((coors - heldPicks[j]).MagnitudeSqr() < 9.0f){
				candidates[i].distance -= 5.0f;
				break;
			}
	}

	// The backend takes eight of up to thirty-two; the ones whose spheres
	// reach closest to the camera matter most.
	for(int i = 1; i < candidateCount; i++){
		const Candidate candidate = candidates[i];
		int j = i-1;
		for(; j >= 0 && candidates[j].distance > candidate.distance; j--)
			candidates[j+1] = candidates[j];
		candidates[j+1] = candidate;
	}

	rw::vulkan::PointLight lights[rw::vulkan::MAX_POINT_LIGHTS];
	const int count = Min(candidateCount,
		Min(gDynamicLightMax, (int)rw::vulkan::MAX_POINT_LIGHTS));
	const float intensity = gDynamicLightIntensity/100.0f;
	// A light that joins the nearest-N set deep inside its own radius would
	// pop in at full strength -- driving down a lamp-lined street it reads
	// as blinking. Fading the tail of the selection against the first light
	// that did NOT make the cut keeps membership changes silent: by the
	// time a light crosses the boundary its weight is already zero.
	const float boundary = candidateCount > count ?
		candidates[count].distance : 1e9f;
	for(int i = 0; i < count; i++){
		const CRegisteredPointLight &src =
			CPointLights::aLights[candidates[i].index];
		const float fade =
			Min(1.0f, (boundary - candidates[i].distance)/6.0f);
		rw::vulkan::PointLight &dst = lights[i];
		dst.position[0] = src.coors.x;
		dst.position[1] = src.coors.y;
		dst.position[2] = src.coors.z;
		dst.radius = src.radius;
		dst.colour[0] = src.red*intensity*fade;
		dst.colour[1] = src.green*intensity*fade;
		dst.colour[2] = src.blue*intensity*fade;
		dst.spot = src.type == CPointLights::LIGHT_DIRECTIONAL;
		dst.direction[0] = src.dir.x;
		dst.direction[1] = src.dir.y;
		dst.direction[2] = src.dir.z;
	}
	heldPickCount = count;
	for(int i = 0; i < count; i++)
		heldPicks[i] = CPointLights::aLights[candidates[i].index].coors;

	// The glow scale is on top of the light colours, which already carry the
	// intensity; 0.14 is the tuned baseline the shader used to hard-code.
	rw::vulkan::setPointLights(lights, count, gDynamicLights >= 2 ?
		0.14f*(gDynamicLightGlowIntensity/100.0f) : 0.0f);
}

bool
VrFxaaEnabled(void)
{
	LoadVrSettings();
	return gSpatialAaMode != 0;
}

int
VrSpatialAaMode(void)
{
	LoadVrSettings();
	return gSpatialAaMode;
}

bool
VrWristPanelEnabled(int panel)
{
	LoadVrSettings();
	if(panel < 0 || panel >= WRIST_PANEL_COUNT)
		return false;
#if MIAMIVR_STEAM_FRAME
	// No tracked hands in third person, so nothing to wear a panel on: the
	// flat interface carries the minimap and the readout instead.
	if(gViewMode != VR_VIEW_FIRST_PERSON)
		return false;
#endif
	return gWristPanelOn[panel];
}

bool
VrWristPanelUnderside(int panel)
{
	LoadVrSettings();
	if(panel < 0 || panel >= WRIST_PANEL_COUNT)
		return false;
	return gWristPanelUnderside[panel];
}

bool
VrWristPanelGazeReveal(void)
{
	LoadVrSettings();
	// A panel being placed has to stay up while its numbers are read off
	// the menu, which is not where the arm is.
	if(gVrMenuVisible)
		return false;
	return gWristPanelGaze;
}

float
VrWristPanelGazeRange(void)
{
	LoadVrSettings();
	return (float)gWristPanelGazeRangeCm*0.01f;
}

int
VrWristPanelHand(int panel)
{
	LoadVrSettings();
	if(panel < 0 || panel >= WRIST_PANEL_COUNT)
		return 0;
	return gWristPanelHand[panel];
}

void
VrGetWristPanelCalibration(int panel, float *alongCm, float *acrossCm,
                           float *liftCm, float *pitchDeg, float *yawDeg,
                           float *rollDeg, float *scale)
{
	LoadVrSettings();
	if(panel < 0 || panel >= WRIST_PANEL_COUNT)
		panel = WRIST_PANEL_MAP;
	const int side = gWristPanelUnderside[panel] ?
		WRIST_SIDE_INNER : WRIST_SIDE_OUTER;
	const int slot = WRIST_SLOT(ResolvedWristContext(panel), side);
	if(alongCm) *alongCm = (float)gWristAlong[panel][slot]*0.1f;
	if(acrossCm) *acrossCm = (float)gWristAcross[panel][slot]*0.1f;
	if(liftCm) *liftCm = (float)gWristLift[panel][slot]*0.1f;
	if(pitchDeg) *pitchDeg = (float)gWristPitch[panel][slot]*0.1f;
	if(yawDeg) *yawDeg = (float)gWristYaw[panel][slot]*0.1f;
	if(rollDeg) *rollDeg = (float)gWristRoll[panel][slot]*0.1f;
	if(scale) *scale = (float)gWristSize[panel][slot]/100.0f;
}

bool
VrWristPanelsInVehicle(void)
{
	LoadVrSettings();
	return gWristPanelsInVehicle;
}

void
VrWristAmmoColour(unsigned char *red, unsigned char *green,
                  unsigned char *blue)
{
	LoadVrSettings();
	if(red) *red = (unsigned char)gAmmoColour[0];
	if(green) *green = (unsigned char)gAmmoColour[1];
	if(blue) *blue = (unsigned char)gAmmoColour[2];
}

bool
VrDistanceFogEnabled(void)
{
	LoadVrSettings();
	return gDistanceFog;
}

bool
VrHudWeaponPanelEnabled(void)
{
	LoadVrSettings();
#if MIAMIVR_STEAM_FRAME
	// With the wrist ammo counter gone, the corner readout is the only place
	// the round count is shown.
	if(gViewMode != VR_VIEW_FIRST_PERSON)
		return true;
#endif
	return gHudWeaponPanel;
}

bool
VrHudClockEnabled(void)
{
	LoadVrSettings();
	return gHudClock;
}

bool
VrGameplayHudEnabled(void)
{
	LoadVrSettings();
#if MIAMIVR_STEAM_FRAME
	// The interface has to be there in third person whatever the first
	// person preference says; nothing else carries it.
	if(gViewMode != VR_VIEW_FIRST_PERSON)
		return true;
#endif
	return gGameplayHudEnabled;
}

void
VrGetGameplayHudSettings(int *widthPercent, int *scalePercent,
                         int *offsetXCm, int *offsetYCm)
{
	LoadVrSettings();
	if(widthPercent) *widthPercent = gHudWidthPercent;
	if(scalePercent) *scalePercent = gHudScalePercent;
	if(offsetXCm) *offsetXCm = gHudOffsetXCm;
	if(offsetYCm) *offsetYCm = gHudOffsetYCm;
}

bool
VrHeadBobbingEnabled(void)
{
	LoadVrSettings();
	return gQuestHeadBobbing != 0;
}

bool
VrUsesHeadRelativeMovement(void)
{
	LoadVrSettings();
	return gQuestMovementOrientation != QUEST_MOVEMENT_ORIENTATION_BODY;
}

bool
VrUsesHeadDirectedMovement(void)
{
	LoadVrSettings();
	return gQuestMovementOrientation ==
		QUEST_MOVEMENT_ORIENTATION_HEAD;
}

bool
VrUsesExperimentalHeadTurning(void)
{
	LoadVrSettings();
	return gQuestMovementOrientation ==
		QUEST_MOVEMENT_ORIENTATION_HEAD_TURN_EXPERIMENTAL;
}

float
VrHeadTurnScale(void)
{
	LoadVrSettings();
	return (float)gQuestHeadSteeringSensitivityPercent/100.0f;
}

bool
VrUsesSnapTurn(void)
{
	LoadVrSettings();
	return gQuestTurnMode == QUEST_TURN_SNAP;
}

float
VrSmoothTurnScale(void)
{
	LoadVrSettings();
	return (float)gQuestTurnSensitivityPercent/100.0f;
}

int
VrSnapTurnAngleDegrees(void)
{
	LoadVrSettings();
	return gQuestSnapTurnAngleDegrees;
}

int
VrPadBinding(int source)
{
	LoadVrSettings();
	if(source < 0 || source >= VR_PAD_SOURCE_COUNT)
		return VR_PAD_TARGET_NONE;
	return gVrPadBinding[source];
}

int
VrPadBindingDefault(int source)
{
	if(source < 0 || source >= VR_PAD_SOURCE_COUNT)
		return VR_PAD_TARGET_NONE;
	return kVrPadBindingDefault[source];
}

float
VrScopeZoomFactor(void)
{
	if(!OculusVR::IsTrackedScopeActive())
		return 1.0f;
	switch(OculusVR::GetTrackedScopeWeaponType()){
	case WEAPONTYPE_SNIPERRIFLE: return 2.5f;
	case WEAPONTYPE_LASERSCOPE: return 3.0f;
	case WEAPONTYPE_ROCKETLAUNCHER: return 1.8f;
	case WEAPONTYPE_CAMERA: return 1.6f;
	default: return 1.0f;
	}
}

}
