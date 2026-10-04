#pragma once

// Bridge between the Android/OpenXR application layer and the game.
//
// On Windows the skeleton owns main() and spins its own loop. Here the loop
// belongs to OpenXR: xrWaitFrame paces the application, and the game has to be
// stepped from inside a frame that has already begun. So the skeleton is
// reduced to init / step / shutdown and the app layer drives it.

#include <vulkan/vulkan.h>

class CControllerState;

namespace androidgame {

// The Vulkan objects the OpenXR session created. librw's Vulkan backend adopts
// these rather than opening a device of its own, so they have to reach
// rsRWINITIALIZE intact: RwEngineOpen forwards displayID straight into
// rw::EngineOpenParams on this platform.
struct VulkanContext
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

// Runs rsINITIALIZE and rsRWINITIALIZE against the device the OpenXR session
// already created.
bool Initialise(const VulkanContext &context,
	bool *renderTargetStartupFailure = nullptr);

// Completes the one-time game/frontend setup before OpenXR starts asking the
// game for frames.  InitialiseOnceAfterRW can take hundreds of milliseconds;
// doing it from Step would leave an already-begun XR frame open for that whole
// time and make the compositor reproject a half-transitioned startup image.
bool PrepareFrontendBeforeFrames(void);

// Controller state pushed in from the OpenXR layer once per frame. CapturePad
// turns it into the game's pad state, which is the same seam the desktop build
// uses for its tracked controllers.
struct PadInput
{
	struct Pose
	{
		float position[3];
		float orientation[4];
		bool valid;
	};

	float leftStickX, leftStickY;
	float rightStickX, rightStickY;
	float leftTrigger, rightTrigger;
	float leftGrip, rightGrip;
	bool a, b, x, y;
	bool menu;
	bool leftStickClick, rightStickClick;
	Pose gripPose[2];
	Pose aimPose[2];
#if MIAMIVR_STEAM_FRAME
	// Steam Frame: the left D-pad, and View's short press as a one-frame
	// "back" pulse (the hold is the recentre, handled before the game sees
	// the pad; see android_main.cpp).
	bool dpadUp, dpadDown, dpadLeft, dpadRight;
	// The shoulder bumpers, left and right (previous / next weapon in third
	// person; unbound in first person).
	bool leftBumper, rightBumper;
	bool viewBack;
#endif
};

// Controller remapping. Every physical Touch input the game reads as a plain
// button drives one of Vice City's pad buttons, and the CONTROLS page of the VR
// menu decides which. It is an on-foot feature: the VR driving layer reads X, A
// and B straight from the controller for the radio, the handbrake and the
// drive-by weapon, so a vehicle keeps the shipped assignment rather than firing
// a moved binding alongside the gesture. The thumbsticks, the menu button and
// the weapon triggers are outside it entirely.
enum eVrPadSource
{
	VR_PAD_SOURCE_A = 0,
	VR_PAD_SOURCE_B,
	VR_PAD_SOURCE_X,
	VR_PAD_SOURCE_Y,
	VR_PAD_SOURCE_LEFT_TRIGGER,
	VR_PAD_SOURCE_RIGHT_TRIGGER,
	VR_PAD_SOURCE_LEFT_GRIP,
	VR_PAD_SOURCE_RIGHT_GRIP,
	VR_PAD_SOURCE_LEFT_STICK_CLICK,
	VR_PAD_SOURCE_RIGHT_STICK_CLICK,
	VR_PAD_SOURCE_COUNT
};

// The PlayStation pad buttons a source can be routed to. What each one does is
// the game's business and follows the controller setup chosen in the frontend;
// with the default setup SQUARE jumps, CROSS sprints, CIRCLE attacks and
// TRIANGLE enters or leaves a vehicle.
enum eVrPadTarget
{
	VR_PAD_TARGET_NONE = 0,
	VR_PAD_TARGET_SQUARE,
	VR_PAD_TARGET_CROSS,
	VR_PAD_TARGET_CIRCLE,
	VR_PAD_TARGET_TRIANGLE,
	VR_PAD_TARGET_L1,
	VR_PAD_TARGET_R1,
	VR_PAD_TARGET_L2,
	VR_PAD_TARGET_R2,
	VR_PAD_TARGET_L3,
	VR_PAD_TARGET_R3,
	VR_PAD_TARGET_COUNT
};

int VrPadBinding(int source);
int VrPadBindingDefault(int source);
// Clears every pad button a binding can reach and rebuilds them from the
// current assignment. The triggers are the analogue accelerator and brake in a
// vehicle and belong to the weapon on foot, so the on-foot pass leaves them
// out instead of letting them arrive as a second button press.
void VrApplyPadBindings(CControllerState *state, const PadInput &input,
                        bool includeTriggers);

void SetPadInput(const PadInput &input);
const PadInput &GetPadInput(void);
// Weapon-fire vibration on one controller; forwarded to the OpenXR session.
void TriggerWeaponHaptic(int hand, float strength);
// Preferred display refresh rate in Hz; forwarded to the OpenXR session.
void SetPreferredRefreshRate(int hz);

// Renders the native Quest tracked hands after the world/effects pass. The
// function is a no-op in cinema mode and until both OpenXR and the player
// camera have produced a valid pose for the current frame.
void RenderTrackedHands(void);

// Play-space frame a wrist panel rides: the rendered hand's, not the
// controller's. They differ whenever the weapon layer has moved a hand onto a
// grip or a socket. Axes follow the OpenXR grip convention -- +X across the
// palm, +Y out of its back, +Z back towards the wrist. False when there is no
// tracked hand to place anything on.
bool VrGetWristVehicleAnchorPose(float position[3], float right[3],
                                 float up[3], float forward[3]);
bool VrGetWristAnchorPose(int hand, float position[3], float side[3],
                          float palmUp[3], float backward[3]);

// Predicted display time of the frame about to be stepped, in nanoseconds.
// Drives the game clock: it is vsync-quantised where the wall clock jitters
// with scheduling, and that jitter reads as world motion stuttering in the
// headset. Pass 0 to fall back to the wall clock.
void SetFrameTimeNs(long long ns);

// Horizontal field of view of one eye, degrees. The game's sprite and
// screen mathematics run against it during rendering, as on the desktop.
void SetEyeFovDeg(float fov);

// The game camera's view window (tangent of the half angles). The Im2D plane
// has to subtend exactly this, or screen coordinates map to the wrong
// direction and world sprites drift as the player moves.
void GetIm2DViewWindow(float *x, float *y);

// Desktop debug overlay port (vrdebug.cpp): chord handling + FPS smoothing,
// then the RGBA pixel block for the compositor quad layer (nil = hidden).
void VrDebugUpdate(const PadInput &input);
const unsigned char *VrDebugPixels(int *width, int *height);
bool VrMenuConsumesInput(void);
bool VrViceCityColorEnabled(void);
bool VrFxaaEnabled(void);
// Build a mip chain for textures whose TXD carries a single level.
bool VrGenerateMipmaps(void);
// The PS2 two-pass alpha rule for masked geometry.
bool VrPs2AlphaTest(void);
// Hands the backend the game's point lights for per-pixel lighting; call
// once per frame before Step. Pushes an empty list when the player has the
// setting off.
void VrPushDynamicLights(void);
// Renderer counters in logcat, off unless asked for.
bool VrRenderDiagnostics(void);
// Mip bias for masked geometry, in half levels.
int VrFoliageSoftness(void);
// How a cutscene is presented: 0 the flat theater screen, 1 stereo from
// the director camera.
int VrCutsceneMode(void);
#if MIAMIVR_STEAM_FRAME
float VrCinema3DDepth(void);
// The 3D cinema depth for this frame: VrCinema3DDepth() while a cutscene is
// on the theater screen, 0 for menus, loading, the intro and gameplay.
float VrCinema3DDepthThisFrame(void);
// Third person view (vr_settings.ini ViewMode): 0 first person, 1 the whole
// game on the theater screen from the stock camera, 2 the stock chase camera
// in stereo. A short press of View toggles between first person and the last
// third person style chosen.
int VrViewMode(void);
bool VrThirdPersonScreen(void);
bool VrThirdPersonShoulder(void);
// vr_settings.ini ThirdPersonMovement: 0 (default) the ped turns to the direction
// he walks, 1 he keeps facing the camera and strafes.
bool VrThirdPersonFacesMovement(void);
// True while the player ped should turn to face the way the stick walks him
// instead of being locked to the camera heading: a third person view, on foot,
// FACE MOVE, and not aiming or firing a ranged weapon.
bool VrThirdPersonFaceMove(void);
void VrToggleThirdPerson(void);
// Mode 1 and gameplay is running: the stock camera is on the theater screen.
bool VrScreenGameplayActive(void);
#endif
// Which camera a stereo cutscene is watched from: 0 the director's own,
// 1 and up the staged actors. The player cycles it with R3.
int VrCutsceneCamera(void);
// The cutscene actor the eye is sitting inside, or null. Its head is
// collapsed for that frame so the shot is not filmed from inside a face.
void *VrCutsceneCameraActor(void);
// The director camera plus one per staged head.
int VrCutsceneCameraCount(void);
int VrSpatialAaMode(void);
// Wrist panels: the minimap and the money/health/wanted readout, each on its
// own arm. Index with the WRIST_PANEL_* values from librw's rwvk.h.
bool VrWristPanelEnabled(int panel);
bool VrWristPanelUnderside(int panel);
// Which wrist a panel is worn on, and where exactly it sits there. Nothing
// about a grip pose locates the wrist behind it, so this is calibrated in the
// HUD menu. The values belong to the side of the wrist in use, not to a hand:
// only the left hand is ever calibrated and the right one mirrors it.
int VrWristPanelHand(int panel);
// True when the worn panels are only shown while the player is looking at
// them, so an arm at rest carries nothing. One answer for the whole set.
bool VrWristPanelGazeReveal(void);
// The reveal range, in metres.
float VrWristPanelGazeRange(void);
// How much of the panel the glance has brought up, 0 to 1. The quad is
// drawn at this alpha so the panel fades in rather than appearing.
float VrWristPanelOpacity(int panel);
// Whether the panels stay on the arms behind a wheel. Only immersive driving
// keeps the hands where a panel can be read, so that is the one case it covers.
bool VrWristPanelsInVehicle(void);
void VrGetWristPanelCalibration(int panel, float *alongCm, float *acrossCm,
                                float *liftCm, float *pitchDeg, float *yawDeg,
                                float *rollDeg, float *scale);
// The weapon icon and ammo counter in the corner of the interface, and the
// clock above them. Both are switches of their own on the headset.
bool VrHudWeaponPanelEnabled(void);
bool VrDistanceFogEnabled(void);
// Colour of the ammo readout, chosen on its calibration page.
void VrWristAmmoColour(unsigned char *red, unsigned char *green,
                       unsigned char *blue);
bool VrHudClockEnabled(void);
bool VrGameplayHudEnabled(void);
// Puts the interface plane on a wrist and hands back that panel's texture to
// bind, or null when there is nothing to show yet; End restores the
// head-locked plane. Each call also asks the backend to render the panel at
// the top of the next frame -- that request is one-shot on purpose.
// hand: -1 uses the panel own setting, 0/1 places it on that arm instead.
void *BeginVrWristPanel(int panel, float centreX, float centreY, float width,
                        float height, int hand = -1);
void EndVrWristPanel(void);
void VrGetGameplayHudSettings(int *widthPercent, int *scalePercent,
                              int *offsetXCm, int *offsetYCm);
bool VrUsesHeadRelativeMovement(void);
// HEAD DIRECTED only: Tommy is reoriented onto the movement direction.
bool VrUsesHeadDirectedMovement(void);
bool VrHeadBobbingEnabled(void);
bool VrUsesExperimentalHeadTurning(void);
float VrHeadTurnScale(void);
bool VrUsesSnapTurn(void);
float VrSmoothTurnScale(void);
int VrSnapTurnAngleDegrees(void);
float VrScopeZoomFactor(void);

// Frontend, loading and cinematic frames are flat content. They are rendered
// once and submitted to both eyes on a world-locked cinema quad instead of
// being interpreted as an immersive stereo world.
bool VrShouldUseTheaterMode(void);

// The basis the frame is actually rendered with, in game space. Effects that
// ask what is in front of the viewer have to read this rather than TheCamera:
// the gameplay camera's own matrix keeps the heading the character faces, and
// that heading stops following the player the moment the headset turns instead
// of the stick. Any pointer may be nil. False when the game camera is the view.
bool VrGetViewBasis(float position[3], float right[3], float up[3],
                    float forward[3]);

// Latch the pose being held now as the neutral one. A player who started
// seated and then stood up is riding an eye height taken while seated until
// this runs.
void VrRecenterView(void);

// A pad looks behind on R3 and crouches on L3. Both are off on foot in VR,
// where the view is the player's own head and the two clicks recenter it
// instead; the CONTROLS page hands either behaviour back.
bool VrStickLookBehindEnabled(void);
bool VrStickCrouchEnabled(void);
float VrTheaterAspectRatio(void);

// One iteration of the game's gGameState machine, including rsIDLE. Must be
// called between vulkan::beginFrame and vulkan::endFrame.
void Step(void);

// True once the game asked to exit.
bool WantsToQuit(void);

void Shutdown(void);

} // namespace androidgame
