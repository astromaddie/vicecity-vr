#include "common.h"

#include "android.h"
#include "CutsceneMgr.h"
#include "Frontend.h"
#include "PlayerInfo.h"
#include "PlayerPed.h"
#include "World.h"
#include "VRHandModel.h"
#include "vulkan/rwvk.h"
#ifdef GTA_VR_WEAPONS
#include "OculusVR.h"
#endif

#include <android/log.h>

extern bool gVrFirstPersonActive;
#if MIAMIVR_STEAM_FRAME
// Third person over the shoulder, on foot: the player is drawn whole and has
// no hands of his own to show or wear panels on.
extern bool gVrThirdPersonOnFoot;
#endif

namespace androidgame {
namespace {

#define HANDLOG(...) __android_log_print(ANDROID_LOG_INFO, "MiamiVR", __VA_ARGS__)

CVector
VectorFromArray(const float value[3])
{
	return CVector(value[0], value[1], value[2]);
}

bool
PoseToWorld(const PadInput::Pose &pose, CVector *position,
            CVector *right, CVector *up, CVector *forward)
{
	if(!pose.valid)
		return false;
	float worldRight[3], worldUp[3], worldForward[3], worldPosition[3];
	if(!rw::vulkan::playPoseToFirstPersonWorld(
	       pose.position, pose.orientation,
	       worldRight, worldUp, worldForward, worldPosition))
		return false;
	*position = VectorFromArray(worldPosition);
	*right = VectorFromArray(worldRight);
	*up = VectorFromArray(worldUp);
	*forward = VectorFromArray(worldForward);
	return true;
}

void
RenderHand(int hand, const PadInput &input)
{
	CVector position, gripRight, gripUp, gripForward;
	if(!PoseToWorld(input.gripPose[hand], &position,
	                &gripRight, &gripUp, &gripForward))
		return;

	CVector unusedPosition, unusedRight, unusedUp, forward;
	if(!PoseToWorld(input.aimPose[hand], &unusedPosition,
	                &unusedRight, &unusedUp, &forward))
		forward = gripForward;
	if(forward.MagnitudeSqr() < 0.0001f)
		return;
	forward.Normalise();

	// Match the approved desktop hand orientation. Anatomical handedness is
	// opposite the OpenXR hand index in the converted RenderWare basis.
	const int modelHand = 1-hand;
	CVector up = gripRight*(modelHand == 0 ? 1.0f : -1.0f);
	up -= forward*DotProduct(up, forward);
	if(up.MagnitudeSqr() < 0.0001f)
		up = gripUp;
	up.Normalise();
	CVector right = CrossProduct(up, forward);
	if(right.MagnitudeSqr() < 0.0001f)
		return;
	right.Normalise();
	if(DotProduct(right, gripForward) < 0.0f)
		right *= -1.0f;

	const float grip = hand == 0 ? input.leftGrip : input.rightGrip;
	const float trigger = hand == 0 ?
		input.leftTrigger : input.rightTrigger;
	const bool rendered =
		VRHandModel::Render(hand, position, right, up, forward, grip, trigger);
	static int renderState[2] = { -1, -1 };
	if(renderState[hand] != (int)rendered){
		renderState[hand] = rendered;
		HANDLOG("[hands] hand=%d pose=valid model=%s",
			hand, rendered ? "rendered" : "load-or-draw-failed");
	}
}

} // namespace

void
RenderTrackedHands(void)
{
	if(!::gVrFirstPersonActive)
		return;
#if MIAMIVR_STEAM_FRAME
	if(::gVrThirdPersonOnFoot)
		return;
#endif
	// A dead Tommy is drawn with his own arms, and the tracked pair would
	// be left floating over the body he is lying in. A cutscene is the
	// same case: the actors have their own arms, and the stereo cutscene
	// modes run through the first-person path that draws these. So is a
	// scripted scene the camera is watching from inside an actor: that
	// one is not a cutscene at all, only a camera parked on a head.
	CPlayerPed *player = FindPlayerPed();
	if(player == nil || player->DyingOrDead() ||
	   CCutsceneMgr::IsRunning() || CCutsceneMgr::IsCutsceneProcessing() ||
	   androidgame::VrCutsceneCameraActor() != nil)
		return;

	const PadInput &input = GetPadInput();
	static int poseState = -1;
	const int currentPoseState =
		(input.gripPose[0].valid ? 1 : 0) |
		(input.gripPose[1].valid ? 2 : 0) |
		(input.aimPose[0].valid ? 4 : 0) |
		(input.aimPose[1].valid ? 8 : 0);
	if(poseState != currentPoseState){
		poseState = currentPoseState;
		HANDLOG("[hands] pose mask=0x%x (grip L/R, aim L/R)",
			currentPoseState);
	}
	RenderHand(0, input);
	RenderHand(1, input);
}

// With no weapon in the hand the visual matrix is the plain grip pose taken to
// the world and back, so an existing panel calibration is unaffected.
bool
VrGetWristAnchorPose(int hand, float position[3], float side[3],
                     float palmUp[3], float backward[3])
{
	if(hand < 0 || hand > 1 || !position || !side || !palmUp || !backward)
		return false;
#ifdef GTA_VR_WEAPONS
	CMatrix matrix;
	if(!::gVrFirstPersonActive ||
#if MIAMIVR_STEAM_FRAME
	   ::gVrThirdPersonOnFoot ||
#endif
	   !OculusVR::GetTrackedVisualHandMatrix(hand, &matrix))
		return false;
	// PoseToMatrix builds Right/Up/Forward from the grip quaternion's local
	// +X/+Y/-Z, so the panel's "backward" is the negated forward axis.
	const CVector worldBackward = matrix.GetForward()*-1.0f;
	const CVector &worldPosition = matrix.GetPosition();
	if(!rw::vulkan::firstPersonWorldPositionToPlay(&worldPosition.x, position) ||
	   !rw::vulkan::firstPersonWorldVectorToPlay(&matrix.GetRight().x, side) ||
	   !rw::vulkan::firstPersonWorldVectorToPlay(&matrix.GetUp().x, palmUp) ||
	   !rw::vulkan::firstPersonWorldVectorToPlay(&worldBackward.x, backward))
		return false;
	return true;
#else
	(void)position; (void)side; (void)palmUp; (void)backward;
	return false;
#endif
}

// Dashboard anchor for the in-vehicle panels: the neutral control centre with
// the vehicle's basis, taken to play space the same way the wrist anchor is.
bool
VrGetWristVehicleAnchorPose(float position[3], float right[3], float up[3],
                            float forward[3])
{
	if(!position || !right || !up || !forward)
		return false;
#ifdef GTA_VR_WEAPONS
	CMatrix matrix;
	if(!::gVrFirstPersonActive || !OculusVR::GetQuestVehicleHudAnchor(&matrix))
		return false;
	const CVector &worldPosition = matrix.GetPosition();
	if(!rw::vulkan::firstPersonWorldPositionToPlay(&worldPosition.x, position) ||
	   !rw::vulkan::firstPersonWorldVectorToPlay(&matrix.GetRight().x, right) ||
	   !rw::vulkan::firstPersonWorldVectorToPlay(&matrix.GetUp().x, up) ||
	   !rw::vulkan::firstPersonWorldVectorToPlay(&matrix.GetForward().x, forward))
		return false;
	return true;
#else
	(void)position; (void)right; (void)up; (void)forward;
	return false;
#endif
}

} // namespace androidgame
