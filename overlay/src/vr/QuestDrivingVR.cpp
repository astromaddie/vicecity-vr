#include "common.h"

#if defined(GTA_VR_WEAPONS) && defined(__ANDROID__)

#include "OculusVR.h"

#include "android.h"
#include "platform_android.h"
#include "Automobile.h"
#include "Bike.h"
#include "Camera.h"
#include "CutsceneMgr.h"
#include "Frontend.h"
#include "Game.h"
#include "main.h"
#include "ModelInfo.h"
#include "ModelIndices.h"
#include "ModelSets.h"
#include "Pad.h"
#include "PlayerInfo.h"
#include "PlayerPed.h"
#include "Vehicle.h"
#include "VehicleModelInfo.h"
#include "WeaponType.h"
#include "crossplatform.h"
#include "vulkan/rwvk.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern CVehicle *FindPlayerVehicle(void);
extern const char *GetVrVehicleModelName(int model);
extern bool gVrFirstPersonActive;

namespace OculusVR {
namespace {

enum {
	VR_HAND_COUNT = 2,
	VEHICLE_CALIBRATION_VALUE_SCALE = 2,
	VR_BIKE_MODEL_COUNT = 6,
	VR_CAR_MODEL_COUNT = MI_LAST_VEHICLE-MI_FIRST_VEHICLE+1,
	VR_CAR_WHEEL_DEFAULT_RADIUS_CM = 18,
	VR_CAR_WHEEL_MIN_RADIUS_CM = 8,
	VR_CAR_WHEEL_MAX_RADIUS_CM = 40,
	VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG = 180
};

enum eVrVehicleCategory
{
	VR_VEHICLE_CATEGORY_CAR = 0,
	VR_VEHICLE_CATEGORY_BIKE,
	VR_VEHICLE_CATEGORY_BOAT,
	VR_VEHICLE_CATEGORY_HELI,
	VR_VEHICLE_CATEGORY_COUNT,
	VR_VEHICLE_CATEGORY_INVALID = -1
};

enum eVrDrivingType
{
	VR_DRIVING_DEFAULT = 0,
	VR_DRIVING_IMMERSIVE,
	VR_DRIVING_MOTION,
	VR_DRIVING_TYPE_COUNT
};

const int gVrBikeModels[VR_BIKE_MODEL_COUNT] = {
	MI_ANGEL, MI_FREEWAY, MI_PCJ600,
	MI_FAGGIO, MI_PIZZABOY, MI_SANCHEZ
};

struct HandleCalibration
{
	int offsetX, offsetY, offsetZ;
	int rotationX, rotationY, rotationZ;
	bool valid;

	HandleCalibration() :
		offsetX(0), offsetY(0), offsetZ(0),
		rotationX(0), rotationY(0), rotationZ(0), valid(false)
	{}
};

struct VehicleViewCalibration
{
	int seatDistanceCm;
	int seatHeightCm;
	// DEFAULT driving keeps its own per-model height. The physical seat
	// above belongs to the immersive layers, where the eye sits behind a
	// wheel the player is holding; the two are not the same measurement.
	int defaultSeatHeightCm;
	int defaultSeatDistanceCm;
	int wheelCenterXCm, wheelCenterYCm, wheelCenterZCm;
	int carWheelRadiusCm;
	int wheelRadiusCm;
	int carWheelPitchHalfDeg, carWheelYawHalfDeg, carWheelRollHalfDeg;
	int carWheelVisibilityOverride;
	bool valid;

	VehicleViewCalibration() : seatDistanceCm(0), seatHeightCm(0),
		defaultSeatHeightCm(0), defaultSeatDistanceCm(0),
		wheelCenterXCm(0), wheelCenterYCm(0), wheelCenterZCm(0),
		carWheelRadiusCm(0), wheelRadiusCm(0),
		carWheelPitchHalfDeg(0), carWheelYawHalfDeg(0),
		carWheelRollHalfDeg(0), carWheelVisibilityOverride(-1),
		valid(false) {}
};

struct VehicleCategoryCalibration
{
	int seatDistanceCm;
	int seatHeightCm;
	int wheelCenterXCm, wheelCenterYCm, wheelCenterZCm;
	int carWheelRadiusCm;
	int wheelRadiusCm;
	int carWheelPitchHalfDeg, carWheelYawHalfDeg, carWheelRollHalfDeg;
	bool valid;
	VehicleCategoryCalibration() : seatDistanceCm(0), seatHeightCm(15),
		wheelCenterXCm(0), wheelCenterYCm(0), wheelCenterZCm(0),
		carWheelRadiusCm(VR_CAR_WHEEL_DEFAULT_RADIUS_CM), wheelRadiusCm(0),
		carWheelPitchHalfDeg(0), carWheelYawHalfDeg(0),
		carWheelRollHalfDeg(0), valid(false) {}
};

struct BikeLeanCalibration
{
	int wheelieHeightCm;
	int standHeightCm;
	bool valid;

	BikeLeanCalibration() :
		wheelieHeightCm(20), standHeightCm(20), valid(false)
	{}
};

static bool gSettingsLoaded;
static int gCarDrivingType;
static int gBikeDrivingType;
static int gBoatDrivingType;
static bool gVehicleForwardFireEnabled;
static CVehicle *gButtonDriveByVehicle;
static int gButtonDriveByDirection = -1;
static int gMotionSteeringHand = 1;
// How far the gripping hand is pulled off the wheel plane, towards the
// driver, so the rim does not end up inside its wrist.
static int gWheelHandPullBackMm;
static bool gBikeHorizonLocked = true;
// Third-person vehicle view: the ordinary chase camera in stereo, with the
// default controls. It is not a seating position, so no seat offset applies.
// One switch per kind of vehicle. Riding a bike from behind and sitting
// in a car cockpit are different preferences, and a single flag made
// choosing one mean losing the other.
enum {
	VR_VEHICLE_KIND_CAR = 0,
	VR_VEHICLE_KIND_BIKE,
	VR_VEHICLE_KIND_BOAT,
	VR_VEHICLE_KIND_COUNT
};
static bool gVehicleThirdPerson[VR_VEHICLE_KIND_COUNT];
// Which kind the vehicle page is showing. Everything on it belongs to
// one kind, so the page follows this rather than mixing all three.
static int gVehicleKindEdit;

static int
VehicleKindFor(CVehicle *vehicle)
{
	if(vehicle == nil)
		return gVehicleKindEdit;
	if(vehicle->IsBike())
		return VR_VEHICLE_KIND_BIKE;
	if(vehicle->IsBoat())
		return VR_VEHICLE_KIND_BOAT;
	return VR_VEHICLE_KIND_CAR;
}
// One switch per kind that has a wheel. Hiding the rim in cars, where the
// dashboard already draws one, says nothing about a boat, where the helm
// is the only thing to hold.
static bool gImmersiveCarWheelVisible[2] = { true, true };
// The marker teaches the grip, and every immersive vehicle has one to
// teach, so each kind carries its own switch. On until a player who has
// learned that kind turns it off; a rider who knows the bars may still
// want the helm marked.
static bool gHandleHighlights[VR_VEHICLE_KIND_COUNT] =
	{ true, true, true };
static bool gCalibrationPreview;

static HandleCalibration
	gBikeCalibration[VR_BIKE_MODEL_COUNT][VR_HAND_COUNT];
static HandleCalibration
	gCarCalibration[VR_CAR_MODEL_COUNT][VR_HAND_COUNT];
static VehicleViewCalibration
	gVehicleViewCalibration[VR_CAR_MODEL_COUNT];
static VehicleCategoryCalibration
	gVehicleCategoryCalibration[VR_VEHICLE_CATEGORY_COUNT];
enum eDefaultVehicleViewType
{
	VR_DEFAULT_VIEW_CAR = 0,
	VR_DEFAULT_VIEW_BIKE,
	// A helm seat is its own thing: neither a car cabin nor a saddle.
	VR_DEFAULT_VIEW_BOAT,
	VR_DEFAULT_VIEW_COUNT
};
struct DefaultVehicleViewOffset
{
	int seatDistanceCm;
	int seatHeightCm;
	DefaultVehicleViewOffset() : seatDistanceCm(0), seatHeightCm(15) {}
};
// DEFAULT car and DEFAULT bike camera placement are independent from each
// other and from the physical Immersive/Motion calibration layers.
static DefaultVehicleViewOffset
	gDefaultVehicleViewOffset[VR_DEFAULT_VIEW_COUNT];
static BikeLeanCalibration
	gBikeLeanCalibration[VR_BIKE_MODEL_COUNT];

static bool gBikeHandleGrabbed[VR_HAND_COUNT];
static bool gBikeHandleGripDown[VR_HAND_COUNT];
static float gBikeHandleDistance[VR_HAND_COUNT] = {
	1000.0f, 1000.0f
};
static float gImmersiveBikeSteering;
static float gImmersiveBikePhysicalAngle;
static float gImmersiveBikeDesiredAngle;
static float gImmersiveBikeSteeringOverflow;
static float gImmersiveBikeThrottle;
// The twist is a motorcycle gesture, not an easy one: the hand has to stay on
// the bar while the wrist turns. Off by default, the trigger drives the bike
// the way it drives a car.
static int gBikeManualThrottle;
// A bike leans far enough in the stock game that a rider whose hands are
// pinned to the bars is thrown sideways with it. The physical lean is left
// alone; only what is drawn -- model, camera, rider and the bars the hands
// hold -- uses this fraction of it.
static int gBikeVisualLeanPercent = 50;
// A bike carries every bump into the view through its own pitch and roll.
// With this off the seat keeps only the bike's heading and the horizon stays
// where the room is, which is the difference between riding and being shaken.
static int gBikeViewFollowsTilt;
// Landing a jump badly throws the rider off, as it does on a pad. It can read
// as a crash rather than a stunt in a headset, so it can be switched off.
static int gBikeRiderCanBeThrown = 1;
// How far the throttle is open, as road speed against the model's own top
// speed. The rider reads the bike's pace off his own wrist instead of a
// dial, which is the only instrument a motorcycle in this game has.
static float
BikeThrottleVisual(CBike *bike)
{
	if(!bike || gBikeManualThrottle || bike->pHandling == nil)
		return 0.0f;
	const float maximum = bike->pHandling->Transmission.fMaxVelocity;
	if(!(maximum > 0.01f))
		return 0.0f;
	return clamp(bike->m_vecMoveSpeed.Magnitude2D()/maximum, 0.0f, 1.0f);
}

static float gImmersiveBikeLean;
static bool gBikeThrottleGestureActive;
static bool gBikeThrottleReferenceValid;
static float gBikeThrottleReferenceOrientation[4];
static bool gBikeLeanReferenceValid[VR_HAND_COUNT];
static float gBikeLeanReferenceTrackingY[VR_HAND_COUNT];
static int gBikeLeanGestureState;

static bool gCarWheelGrabbed[VR_HAND_COUNT];
static bool gCarWheelGripDown[VR_HAND_COUNT];
static float gCarWheelDistance[VR_HAND_COUNT] = {
	1000.0f, 1000.0f
};
static float gCarWheelGrabReferenceAngle[VR_HAND_COUNT];
static float gCarWheelContinuousAngle[VR_HAND_COUNT];
static bool gCarWheelAngleValid[VR_HAND_COUNT];
// One hand reads the wheel by where it sits around the rim, so the
// answer is only as good as the spoke from the boss to the hand is
// long. A hand that drifts inwards leaves atan2 turning tracking noise
// into a full lock, so below this the wheel holds and waits for the
// hand to come back out to the rim.
static const float kCarWheelMinSpoke = 0.06f;
// No wrist turns a wheel this far between two frames. A step this big
// is a dropped frame or a branch the unwrap guessed wrong, and taking
// it was what threw the wheel across to the opposite lock.
static const float kCarWheelMaxStep = 0.7853982f;	// 45 degrees
// How far the virtual wheel turns between its two locks.
static const float kCarWheelMaxAngle = 1.3962634f;	// 80 degrees
static bool gCarWheelSpokeUsable[VR_HAND_COUNT];
static float gCarWheelTwoHandReferenceAngle;
static float gCarWheelTwoHandContinuousAngle;
static bool gCarWheelTwoHandAngleValid;
static float gImmersiveCarSteering;
static float gImmersiveCarPhysicalAngle;
static bool gImmersiveCarHornPressed;
static bool gCarHornContact[VR_HAND_COUNT];
static bool gCarHornArmed[VR_HAND_COUNT];
static float gCarHornPreviousDistance[VR_HAND_COUNT] = {
	1000.0f, 1000.0f
};

static bool gVrRadioButtonDown;
static bool gVrRadioChangeJustPressed;

static CVehicle *gMotionSteeringVehicle;
static float gMotionVehicleSteering;
static float gMotionVehiclePhysicalAngle;
static bool gMotionSteeringReferenceValid;
static float gMotionSteeringReferenceHeading;

struct ImmersiveSteeringChordState
{
	CVehicle *vehicle;
	uint32 realHandMask;
	bool valid;
	bool referenceActive;
	bool rebaseOnNextValid;
	float referenceAngle;
	float continuousAngle;
	float lastDelta;
	uint32 pointValidMask;
	uint32 captureCount;
	ImmersiveSteeringChordState() : vehicle(nil), realHandMask(0),
		valid(false), referenceActive(false), rebaseOnNextValid(false),
		referenceAngle(0.0f), continuousAngle(0.0f), lastDelta(0.0f),
		pointValidMask(0), captureCount(0) {}
};

struct BikeOneHandSteeringState
{
	CVehicle *vehicle;
	uint32 realHandMask;
	bool valid;
	CVector referenceHandTracking;
	CVector seedChordTracking;
	CVector rightTracking;
	CVector forwardTracking;
	float referencePhysicalAngle;
	BikeOneHandSteeringState() : vehicle(nil), realHandMask(0), valid(false),
		referenceHandTracking(0.0f, 0.0f, 0.0f),
		seedChordTracking(0.0f, 0.0f, 0.0f),
		rightTracking(0.0f, 0.0f, 0.0f),
		forwardTracking(0.0f, 0.0f, 0.0f),
		referencePhysicalAngle(0.0f) {}
};

static ImmersiveSteeringChordState gBikeSteeringChordState;
static BikeOneHandSteeringState gBikeOneHandSteeringState;

static const char *const kSettingsPath = ".\\vr_settings.ini";

static void
SaveSetting(const char *name, int value)
{
	char text[32];
	sprintf(text, "%d", value);
	WritePrivateProfileStringA("VR", name, text, kSettingsPath);
}

static void ReloadVehicleCalibration();

static void
LoadDrivingSettings()
{
	if(gSettingsLoaded)
		return;
	gVehicleForwardFireEnabled = GetPrivateProfileIntA("VR",
		"DefaultVehicleForwardFire", 0, kSettingsPath) != 0;
	char drivingType[16] = {};
	GetPrivateProfileStringA("VR", "DrivingType", "", drivingType,
		sizeof(drivingType), kSettingsPath);
	int legacyDrivingType;
	if(drivingType[0] != '\0')
		legacyDrivingType = atoi(drivingType);
	else
		legacyDrivingType =
			GetPrivateProfileIntA("VR", "ImmersiveDriving", 1,
				kSettingsPath) != 0 ?
				VR_DRIVING_IMMERSIVE : VR_DRIVING_DEFAULT;
	legacyDrivingType = clamp(legacyDrivingType, (int)VR_DRIVING_DEFAULT,
		(int)VR_DRIVING_TYPE_COUNT-1);
	gWheelHandPullBackMm = clamp((int)(int32)GetPrivateProfileIntA(
		"VR", "WheelHandPullBackMm", 50, kSettingsPath), -80, 80);
	gCarDrivingType = clamp((int)(int32)GetPrivateProfileIntA(
		"VR", "CarDrivingType", legacyDrivingType, kSettingsPath),
		(int)VR_DRIVING_DEFAULT, (int)VR_DRIVING_TYPE_COUNT-1);
	gBikeDrivingType = clamp((int)(int32)GetPrivateProfileIntA(
		"VR", "BikeDrivingType", legacyDrivingType, kSettingsPath),
		(int)VR_DRIVING_DEFAULT, (int)VR_DRIVING_TYPE_COUNT-1);
	// Boats were steered by the car setting when the wheel first reached
	// them. Inheriting it as the fallback keeps that choice for anyone who
	// already made it, and gives the helm a switch of its own from here.
	gBoatDrivingType = clamp((int)(int32)GetPrivateProfileIntA(
		"VR", "BoatDrivingType", gCarDrivingType, kSettingsPath),
		(int)VR_DRIVING_DEFAULT, (int)VR_DRIVING_TYPE_COUNT-1);
	gBikeManualThrottle = GetPrivateProfileIntA("VR",
		"ImmersiveBikeManualThrottle", 0, kSettingsPath) != 0 ? 1 : 0;
	gBikeVisualLeanPercent = clamp((int)(int32)GetPrivateProfileIntA(
		"VR", "BikeVisualLeanPercent", 50, kSettingsPath), 25, 100);
	gBikeViewFollowsTilt = GetPrivateProfileIntA("VR",
		"BikeViewFollowsTilt", 0, kSettingsPath) != 0 ? 1 : 0;
	gBikeRiderCanBeThrown = GetPrivateProfileIntA("VR",
		"BikeRiderCanBeThrown", 1, kSettingsPath) != 0 ? 1 : 0;
	gMotionSteeringHand = clamp((int)(int32)GetPrivateProfileIntA(
		"VR", "MotionSteeringHand", 1, kSettingsPath), 0,
		VR_HAND_COUNT-1);
	{
		static const char *const key[VR_VEHICLE_KIND_COUNT] = {
			"CarHandleHighlights", "BikeHandleHighlights",
			"BoatHandleHighlights"
		};
		// The bike key came first and stood for every vehicle, so it is
		// the fallback for all three.
		const int legacy = GetPrivateProfileIntA("VR",
			"BikeHandleHighlights", 1, kSettingsPath) != 0 ? 1 : 0;
		for(int kind = 0; kind < VR_VEHICLE_KIND_COUNT; kind++)
			gHandleHighlights[kind] = GetPrivateProfileIntA("VR",
				key[kind], legacy, kSettingsPath) != 0;
	}
	gBikeHorizonLocked = GetPrivateProfileIntA(
		"VR", "BikeLockHorizon", 1, kSettingsPath) != 0;
	{
		// The old single key is the fallback for all three, so a choice
		// already made survives the split.
		static const char *const key[VR_VEHICLE_KIND_COUNT] = {
			"VehicleThirdPersonCar", "VehicleThirdPersonBike",
			"VehicleThirdPersonBoat"
		};
		const int legacy = GetPrivateProfileIntA("VR",
			"VehicleThirdPerson", 0, kSettingsPath) != 0 ? 1 : 0;
		for(int kind = 0; kind < VR_VEHICLE_KIND_COUNT; kind++)
			gVehicleThirdPerson[kind] = GetPrivateProfileIntA("VR",
				key[kind], legacy, kSettingsPath) != 0;
	}
	gImmersiveCarWheelVisible[0] = GetPrivateProfileIntA(
		"VR", "ImmersiveCarWheelVisible", 1, kSettingsPath) != 0;
	// The car key is the boat's fallback, so a choice made before boats had
	// a helm carries over rather than resetting.
	gImmersiveCarWheelVisible[1] = GetPrivateProfileIntA(
		"VR", "ImmersiveBoatWheelVisible",
		gImmersiveCarWheelVisible[0] ? 1 : 0, kSettingsPath) != 0;
	ReloadVehicleCalibration();
	gSettingsLoaded = true;
}

static int
GetBikeModelIndex(int model)
{
	for(int index = 0; index < VR_BIKE_MODEL_COUNT; index++)
		if(gVrBikeModels[index] == model)
			return index;
	return -1;
}

static int
GetCarModelIndex(int model)
{
	if(model < MI_FIRST_VEHICLE || model > MI_LAST_VEHICLE)
		return -1;
	CBaseModelInfo *base = CModelInfo::GetModelInfo(model);
	if(!base ||
	   ((CVehicleModelInfo*)base)->m_vehicleType != VEHICLE_TYPE_CAR)
		return -1;
	return model-MI_FIRST_VEHICLE;
}

static CBike *
GetActivePlayerBike()
{
	CVehicle *vehicle = FindPlayerVehicle();
	return vehicle && vehicle->IsBike() ? (CBike*)vehicle : nil;
}

// A boat is steered from a wheel in front of a seat, exactly as a car is,
// and everything the wheel logic reads -- the seat position out of the
// model and the vehicle matrix -- a boat has. The one car-only piece is the
// rotating wheel node, and that is guarded where it is used: boat models do
// not carry one, so only the wheel the port draws itself turns.
static CVehicle *
GetActivePlayerCar()
{
	CVehicle *vehicle = FindPlayerVehicle();
	if(!vehicle)
		return nil;
	if(vehicle->IsBoat())
		return vehicle;
	return vehicle->IsCar() &&
		!vehicle->IsRealHeli() && !vehicle->IsRealPlane() ?
			vehicle : nil;
}

static int
GetDrivingTypeForVehicle(CVehicle *vehicle)
{
	if(!vehicle)
		return VR_DRIVING_DEFAULT;
	if(vehicle->IsBike())
		return gBikeDrivingType;
	if(vehicle->IsBoat())
		return gBoatDrivingType;
	if(vehicle->IsCar() && !vehicle->IsRealHeli() &&
	   !vehicle->IsRealPlane())
		return gCarDrivingType;
	return VR_DRIVING_DEFAULT;
}

static bool
IsDrivingEnvironmentActive()
{
	LoadDrivingSettings();
	CVehicle *vehicle = FindPlayerVehicle();
	// The third-person view has no cockpit to reach into, so the physical
	// wheel and motion steering stay off and the controls remain DEFAULT.
	if(IsQuestVehicleThirdPerson() ||
	   GetDrivingTypeForVehicle(vehicle) == VR_DRIVING_DEFAULT ||
	   !gVrFirstPersonActive ||
	   gGameState != GS_PLAYING_GAME ||
	   FrontEndMenuManager.m_bGameNotLoaded ||
	   FrontEndMenuManager.m_bWantToRestart ||
	   FrontEndMenuManager.m_bWantToLoad ||
	   CGame::playingIntro ||
	   CCutsceneMgr::IsRunning() ||
	   CCutsceneMgr::IsCutsceneProcessing() ||
	   TheCamera.m_WideScreenOn)
		return false;
	CPlayerPed *player = FindPlayerPed();
	return player && !player->DyingOrDead() && vehicle &&
		vehicle->pDriver == player;
}

static bool
IsImmersiveEnvironmentActive()
{
	return GetDrivingTypeForVehicle(FindPlayerVehicle()) ==
		VR_DRIVING_IMMERSIVE &&
		IsDrivingEnvironmentActive();
}

static bool
IsMotionEnvironmentActive()
{
	return GetDrivingTypeForVehicle(FindPlayerVehicle()) ==
		VR_DRIVING_MOTION &&
		IsDrivingEnvironmentActive();
}

static bool
IsImmersiveBikeActive(CVehicle *expected = nil)
{
	if(!IsImmersiveEnvironmentActive())
		return false;
	CBike *bike = GetActivePlayerBike();
	return bike && (!expected || expected == bike) &&
		GetBikeModelIndex(bike->GetModelIndex()) >= 0;
}

static bool
IsImmersiveCarActive(CVehicle *expected = nil)
{
	if(!IsImmersiveEnvironmentActive())
		return false;
	CVehicle *car = GetActivePlayerCar();
	return car && (!expected || expected == car);
}

static bool
IsVrBikeActive(CVehicle *expected = nil)
{
	if(!IsDrivingEnvironmentActive())
		return false;
	CBike *bike = GetActivePlayerBike();
	return bike && (!expected || expected == bike) &&
		GetBikeModelIndex(bike->GetModelIndex()) >= 0;
}

static bool
IsVrCarActive(CVehicle *expected = nil)
{
	if(!IsDrivingEnvironmentActive())
		return false;
	CVehicle *car = GetActivePlayerCar();
	return car && (!expected || expected == car);
}

static bool
IsVrDrivingActive(CVehicle *expected = nil)
{
	return IsVrBikeActive(expected) || IsVrCarActive(expected);
}

static int
GetDefaultBikeHandleOffsetX(int bikeIndex, int hand)
{
	static const int halfWidthCm[VR_BIKE_MODEL_COUNT] = {
		36, 36, 32, 26, 26, 32
	};
	if(bikeIndex < 0 || bikeIndex >= VR_BIKE_MODEL_COUNT ||
	   hand < 0 || hand >= VR_HAND_COUNT)
		return 0;
	return halfWidthCm[bikeIndex]*
		VEHICLE_CALIBRATION_VALUE_SCALE*
		(hand == 0 ? -1 : 1);
}

struct BuiltInBikeHandleDefaults
{
	int model;
	ModelSets::eModelSet modelSet;
	int hand[VR_HAND_COUNT][6];
};

// Hand order is LEFT, RIGHT; fields are offset XYZ then rotation XYZ, all in
// the existing half-centimetre / half-degree Quest calibration scale. The
// classic rows are the absolute v0.3.1 release baselines from the desktop
// calibration profile. A modern bike is a different mesh with the bars in a
// different place, so those that have been ridden and measured carry their own
// row; the rest still fall back to the classic figure.
static const BuiltInBikeHandleDefaults gBuiltInBikeHandleDefaults[] = {
	{ MI_ANGEL, ModelSets::MODEL_SET_CLASSIC, {
		{ -61,-83,82, 137,0,0 },
		{ 61,-82,84, 138,0,0 } } },
	{ MI_FREEWAY, ModelSets::MODEL_SET_CLASSIC, {
		{ -64,-82,85, 135,0,0 },
		{ 64,-82,85, 133,0,0 } } },
	{ MI_PCJ600, ModelSets::MODEL_SET_CLASSIC, {
		{ -66,-27,81, -193,1,-15 },
		{ 66,-27,83, -192,0,0 } } },
	{ MI_FAGGIO, ModelSets::MODEL_SET_CLASSIC, {
		{ -65,-35,84, 156,0,0 },
		{ 66,-33,84, 156,15,0 } } },
	{ MI_PIZZABOY, ModelSets::MODEL_SET_CLASSIC, {
		{ -68,-34,83, -202,0,0 },
		{ 68,-34,83, 164,0,0 } } },
	{ MI_SANCHEZ, ModelSets::MODEL_SET_CLASSIC, {
		{ -64,-23,46, -222,5,0 },
		{ 65,-24,46, 149,0,0 } } },
	{ MI_FREEWAY, ModelSets::MODEL_SET_MODERN, {
		{ -61,-21,-8, 135,0,0 },
		{ 61,-21,-8, 133,0,0 } } },
	{ MI_FAGGIO, ModelSets::MODEL_SET_MODERN, {
		{ -55,-65,80, 121,0,0 },
		{ 56,-63,80, 134,-10,0 } } },
	{ MI_PIZZABOY, ModelSets::MODEL_SET_MODERN, {
		{ -55,-63,80, -232,-13,-1 },
		{ 55,-63,83, 126,0,0 } } },
};

static bool
GetBuiltInBikeHandleCalibration(int model, int hand,
	HandleCalibration *calibration)
{
	if(!calibration || hand < 0 || hand >= VR_HAND_COUNT)
		return false;
	// Prefer the row measured for the set in use, then the classic baseline.
	const ModelSets::eModelSet active = ModelSets::GetActiveForCategory(
		ModelSets::MODEL_CATEGORY_VEHICLES);
	for(int pass = 0; pass < 2; pass++){
		const ModelSets::eModelSet wanted = pass == 0 ? active :
			ModelSets::MODEL_SET_CLASSIC;
		for(int index = 0;
		    index < (int)ARRAY_SIZE(gBuiltInBikeHandleDefaults); index++){
			const BuiltInBikeHandleDefaults &defaults =
				gBuiltInBikeHandleDefaults[index];
			if(defaults.model != model || defaults.modelSet != wanted)
				continue;
			const int *value = defaults.hand[hand];
			calibration->offsetX = value[0];
			calibration->offsetY = value[1];
			calibration->offsetZ = value[2];
			calibration->rotationX = value[3];
			calibration->rotationY = value[4];
			calibration->rotationZ = value[5];
			calibration->valid = true;
			return true;
		}
		if(active == ModelSets::MODEL_SET_CLASSIC)
			break;
	}
	return false;
}

static HandleCalibration *
GetBikeCalibration(int model, int hand)
{
	const int bikeIndex = GetBikeModelIndex(model);
	if(bikeIndex < 0 || hand < 0 || hand >= VR_HAND_COUNT)
		return nil;
	HandleCalibration &calibration = gBikeCalibration[bikeIndex][hand];
	if(calibration.valid)
		return &calibration;
	char section[64];
	sprintf(section, ModelSets::GetActiveForCategory(
		ModelSets::MODEL_CATEGORY_VEHICLES) == ModelSets::MODEL_SET_MODERN ?
		"BikeHandleModern_%d_%s" : "BikeHandle_%d_%s", model,
		hand == 0 ? "Left" : "Right");
	HandleCalibration defaults;
	if(!GetBuiltInBikeHandleCalibration(model, hand, &defaults))
		defaults.offsetX = GetDefaultBikeHandleOffsetX(bikeIndex, hand);
	calibration.offsetX = clamp((int)(int32)GetPrivateProfileIntA(
		section, "OffsetX",
		defaults.offsetX, kSettingsPath),
		-300, 300);
	calibration.offsetY = clamp((int)(int32)GetPrivateProfileIntA(
		section, "OffsetY", defaults.offsetY, kSettingsPath), -300, 300);
	calibration.offsetZ = clamp((int)(int32)GetPrivateProfileIntA(
		section, "OffsetZ", defaults.offsetZ, kSettingsPath), -300, 300);
	calibration.rotationX = clamp((int)(int32)GetPrivateProfileIntA(
		section, "RotationX", defaults.rotationX, kSettingsPath),
		-720, 720);
	calibration.rotationY = clamp((int)(int32)GetPrivateProfileIntA(
		section, "RotationY", defaults.rotationY, kSettingsPath),
		-720, 720);
	calibration.rotationZ = clamp((int)(int32)GetPrivateProfileIntA(
		section, "RotationZ", defaults.rotationZ, kSettingsPath),
		-720, 720);
	calibration.valid = true;
	return &calibration;
}

static HandleCalibration *
GetCarCalibration(int model, int hand)
{
	const int carIndex = GetCarModelIndex(model);
	if(carIndex < 0 || hand < 0 || hand >= VR_HAND_COUNT)
		return nil;
	HandleCalibration &calibration = gCarCalibration[carIndex][hand];
	if(calibration.valid)
		return &calibration;
	char section[64];
	sprintf(section, ModelSets::GetActiveForCategory(
		ModelSets::MODEL_CATEGORY_VEHICLES) == ModelSets::MODEL_SET_MODERN ?
		"CarWheelV2Modern_%d_%s" : "CarWheelV2_%d_%s", model,
		hand == 0 ? "Left" : "Right");
	const int defaultX = 18*VEHICLE_CALIBRATION_VALUE_SCALE*
		(hand == 0 ? -1 : 1);
	calibration.offsetX = clamp((int)(int32)GetPrivateProfileIntA(
		section, "OffsetX", defaultX, kSettingsPath), -300, 300);
	calibration.offsetY = clamp((int)(int32)GetPrivateProfileIntA(
		section, "OffsetY", 0, kSettingsPath), -300, 300);
	calibration.offsetZ = clamp((int)(int32)GetPrivateProfileIntA(
		section, "OffsetZ", 0, kSettingsPath), -300, 300);
	calibration.rotationX = clamp((int)(int32)GetPrivateProfileIntA(
		section, "RotationX", 0, kSettingsPath), -720, 720);
	calibration.rotationY = clamp((int)(int32)GetPrivateProfileIntA(
		section, "RotationY", 0, kSettingsPath), -720, 720);
	calibration.rotationZ = clamp((int)(int32)GetPrivateProfileIntA(
		section, "RotationZ", 0, kSettingsPath), -720, 720);
	calibration.valid = true;
	return &calibration;
}

static BikeLeanCalibration *
GetBikeLeanCalibration(int model)
{
	const int bikeIndex = GetBikeModelIndex(model);
	if(bikeIndex < 0)
		return nil;
	BikeLeanCalibration &calibration = gBikeLeanCalibration[bikeIndex];
	if(calibration.valid)
		return &calibration;
	char section[64];
	sprintf(section, ModelSets::GetActiveForCategory(
		ModelSets::MODEL_CATEGORY_VEHICLES) == ModelSets::MODEL_SET_MODERN ?
		"BikeControlModern_%d" : "BikeControl_%d", model);
	calibration.wheelieHeightCm = clamp((int)(int32)
		GetPrivateProfileIntA(section, "WheelieHeightCm", 20,
			kSettingsPath), 5, 100);
	calibration.standHeightCm = clamp((int)(int32)
		GetPrivateProfileIntA(section, "StandHeightCm", 20,
			kSettingsPath), 5, 100);
	calibration.valid = true;
	return &calibration;
}

static int
GetVehicleCategory(CVehicle *vehicle)
{
	if(!vehicle) return VR_VEHICLE_CATEGORY_INVALID;
	if(vehicle->IsBike()) return VR_VEHICLE_CATEGORY_BIKE;
	if(vehicle->IsBoat()) return VR_VEHICLE_CATEGORY_BOAT;
	if(vehicle->IsHeli() || vehicle->IsRealHeli())
		return VR_VEHICLE_CATEGORY_HELI;
	if(vehicle->IsCar() && !vehicle->IsRealPlane())
		return VR_VEHICLE_CATEGORY_CAR;
	return VR_VEHICLE_CATEGORY_INVALID;
}

static VehicleCategoryCalibration *
GetCategoryCalibration(CVehicle *vehicle)
{
	const int category = GetVehicleCategory(vehicle);
	return category >= 0 && category < VR_VEHICLE_CATEGORY_COUNT ?
		&gVehicleCategoryCalibration[category] : nil;
}

struct BuiltInVehicleCategoryDefaults
{
	ModelSets::eModelSet modelSet;
	int category;
	int value[10];
};

static const BuiltInVehicleCategoryDefaults gBuiltInVehicleCategoryDefaults[] = {
	{ ModelSets::MODEL_SET_XBOX, VR_VEHICLE_CATEGORY_CAR,
		{ 0,0, 0,0,0, 18,0, 0,0,0 } },
	{ ModelSets::MODEL_SET_CLASSIC, VR_VEHICLE_CATEGORY_CAR,
		{ 0,0, -1,4,8, 18,0, 0,0,0 } },
	{ ModelSets::MODEL_SET_CLASSIC, VR_VEHICLE_CATEGORY_BIKE,
		{ 0,0, 0,0,0, 18,0, 0,0,0 } },
	{ ModelSets::MODEL_SET_CLASSIC, VR_VEHICLE_CATEGORY_BOAT,
		{ 0,15, 0,0,0, 18,0, 0,0,0 } },
	{ ModelSets::MODEL_SET_CLASSIC, VR_VEHICLE_CATEGORY_HELI,
		{ 0,15, 0,0,0, 18,0, 0,0,0 } },
	{ ModelSets::MODEL_SET_MODERN, VR_VEHICLE_CATEGORY_CAR,
		{ 0,5, 0,0,0, 17,0, 0,0,0 } },
	{ ModelSets::MODEL_SET_MODERN, VR_VEHICLE_CATEGORY_BIKE,
		{ 0,0, 0,0,0, 18,0, 0,0,0 } },
	{ ModelSets::MODEL_SET_MODERN, VR_VEHICLE_CATEGORY_BOAT,
		{ 0,10, 0,0,0, 18,0, 0,0,0 } },
	{ ModelSets::MODEL_SET_MODERN, VR_VEHICLE_CATEGORY_HELI,
		{ 0,10, 0,0,0, 18,0, 0,0,0 } },
};

static void
GetDefaultCategoryCalibration(ModelSets::eModelSet modelSet, int category,
	VehicleCategoryCalibration *calibration)
{
	if(!calibration) return;
	*calibration = VehicleCategoryCalibration();
	for(int i = 0; i < (int)ARRAY_SIZE(gBuiltInVehicleCategoryDefaults); i++){
		const BuiltInVehicleCategoryDefaults &entry =
			gBuiltInVehicleCategoryDefaults[i];
		if(entry.modelSet != modelSet || entry.category != category)
			continue;
		calibration->seatDistanceCm = entry.value[0];
		calibration->seatHeightCm = entry.value[1];
		calibration->wheelCenterXCm = entry.value[2];
		calibration->wheelCenterYCm = entry.value[3];
		calibration->wheelCenterZCm = entry.value[4];
		calibration->carWheelRadiusCm = entry.value[5];
		calibration->wheelRadiusCm = entry.value[6];
		calibration->carWheelPitchHalfDeg = entry.value[7];
		calibration->carWheelYawHalfDeg = entry.value[8];
		calibration->carWheelRollHalfDeg = entry.value[9];
		return;
	}
}

static bool
ReadProfileInt(const char *section, const char *name, int *value)
{
	char text[32] = {};
	if(!value || GetPrivateProfileStringA(section, name, "", text,
	   sizeof(text), kSettingsPath) == 0 || text[0] == '\0')
		return false;
	*value = atoi(text);
	return true;
}

static const char *const gCategoryPrefixes[VR_VEHICLE_CATEGORY_COUNT] = {
	"Car", "Bike", "Boat", "Heli"
};

static int
ReadCategoryValue(int category, const char *suffix, int fallback,
	int minimum, int maximum)
{
	char key[64];
	int value;
	if(ModelSets::GetActiveForCategory(ModelSets::MODEL_CATEGORY_VEHICLES) ==
	   ModelSets::MODEL_SET_MODERN){
		sprintf(key, "%s%s", gCategoryPrefixes[category], suffix);
		if(ReadProfileInt("VR", key, &value)) fallback = value;
		sprintf(key, "Modern%s%s", gCategoryPrefixes[category], suffix);
		if(ReadProfileInt("VR", key, &value)) fallback = value;
	}else{
		sprintf(key, "%s%s", gCategoryPrefixes[category], suffix);
		if(ReadProfileInt("VR", key, &value)) fallback = value;
	}
	return clamp(fallback, minimum, maximum);
}

static void
ReloadVehicleCalibration()
{
	int legacyHeight = 15;
	const bool hasLegacyHeight =
		ReadProfileInt("VR", "DrivingYOffsetCm", &legacyHeight);
	legacyHeight = clamp(legacyHeight, -100, 150);
	static const char *heightKeys[VR_DEFAULT_VIEW_COUNT] = {
		"DefaultCarSeatHeightCm", "DefaultBikeSeatHeightCm",
		"DefaultBoatSeatHeightCm"
	};
	static const char *distanceKeys[VR_DEFAULT_VIEW_COUNT] = {
		"DefaultCarSeatDistanceCm", "DefaultBikeSeatDistanceCm",
		"DefaultBoatSeatDistanceCm"
	};
	for(int type = 0; type < VR_DEFAULT_VIEW_COUNT; type++){
		gDefaultVehicleViewOffset[type].seatHeightCm = clamp(
			(int)(int32)GetPrivateProfileIntA("VR", heightKeys[type],
				type == VR_DEFAULT_VIEW_CAR && ModelSets::GetActiveForCategory(
					ModelSets::MODEL_CATEGORY_VEHICLES) == ModelSets::MODEL_SET_XBOX ?
					0 : legacyHeight, kSettingsPath), -100, 150);
		gDefaultVehicleViewOffset[type].seatDistanceCm = clamp(
			(int)(int32)GetPrivateProfileIntA("VR", distanceKeys[type],
				0, kSettingsPath), -100, 100);
	}
	const ModelSets::eModelSet modelSet = ModelSets::GetActiveForCategory(
		ModelSets::MODEL_CATEGORY_VEHICLES);
	for(int category = 0; category < VR_VEHICLE_CATEGORY_COUNT; category++){
		VehicleCategoryCalibration defaults;
		GetDefaultCategoryCalibration(modelSet, category, &defaults);
		VehicleCategoryCalibration &calibration =
			gVehicleCategoryCalibration[category];
		calibration.seatDistanceCm = ReadCategoryValue(category,
			"SeatDistanceCm", defaults.seatDistanceCm, -100, 100);
		// Only a genuine old settings file overrides the table.
		calibration.seatHeightCm = ReadCategoryValue(category,
			"SeatHeightCm",
			hasLegacyHeight ? legacyHeight : defaults.seatHeightCm,
			-100, 150);
		calibration.wheelCenterXCm = ReadCategoryValue(category,
			"WheelCenterXCm", defaults.wheelCenterXCm, -100, 100);
		calibration.wheelCenterYCm = ReadCategoryValue(category,
			"WheelCenterYCm", defaults.wheelCenterYCm, -100, 100);
		calibration.wheelCenterZCm = ReadCategoryValue(category,
			"WheelCenterZCm", defaults.wheelCenterZCm, -100, 100);
		calibration.carWheelRadiusCm = ReadCategoryValue(category,
			"WheelRadiusV2Cm", defaults.carWheelRadiusCm,
			VR_CAR_WHEEL_MIN_RADIUS_CM, VR_CAR_WHEEL_MAX_RADIUS_CM);
		calibration.wheelRadiusCm = ReadCategoryValue(category,
			"WheelRadiusCm", defaults.wheelRadiusCm, -20, 40);
		calibration.carWheelPitchHalfDeg = ReadCategoryValue(category,
			"WheelPitchHalfDeg", defaults.carWheelPitchHalfDeg,
			-VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG,
			 VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG);
		calibration.carWheelYawHalfDeg = ReadCategoryValue(category,
			"WheelYawHalfDeg", defaults.carWheelYawHalfDeg,
			-VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG,
			 VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG);
		calibration.carWheelRollHalfDeg = ReadCategoryValue(category,
			"WheelRollHalfDeg", defaults.carWheelRollHalfDeg,
			-VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG,
			 VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG);
		calibration.valid = true;
	}
}

static void
SaveCategoryCalibrationValue(int category, const char *suffix, int value)
{
	if(category < 0 || category >= VR_VEHICLE_CATEGORY_COUNT || !suffix)
		return;
	char key[64], text[32];
	sprintf(key, "%s%s%s",
		ModelSets::GetActiveForCategory(ModelSets::MODEL_CATEGORY_VEHICLES) ==
			ModelSets::MODEL_SET_MODERN ? "Modern" : "",
		gCategoryPrefixes[category], suffix);
	sprintf(text, "%d", value);
	WritePrivateProfileStringA("VR", key, text, kSettingsPath);
}

struct BuiltInVehicleViewDefaults
{
	int model;
	ModelSets::eModelSet modelSet;
	int value[11];
};

static const BuiltInVehicleViewDefaults gBuiltInVehicleViewDefaults[] = {
	{ MI_PIZZABOY, ModelSets::MODEL_SET_CLASSIC,
		{ -15,-12, 0,0,0, 0,0, 0,0,0, -1 } },
	{ MI_PCJ600, ModelSets::MODEL_SET_CLASSIC,
		{ -8,0, 0,0,0, 0,0, 0,0,0, -1 } },
	{ MI_FAGGIO, ModelSets::MODEL_SET_CLASSIC,
		{ -7,-22, 0,0,0, 0,0, 0,0,0, -1 } },
	{ MI_SANCHEZ, ModelSets::MODEL_SET_CLASSIC,
		{ -15,-15, 0,0,0, 0,0, 0,0,0, -1 } },
	{ MI_STINGER, ModelSets::MODEL_SET_MODERN,
		{ 8,5, 0,19,9, 21,0, -42,0,0, 0 } },
	{ MI_INFERNUS, ModelSets::MODEL_SET_MODERN,
		{ 0,0, 1,-5,4, 0,0, -36,0,0, 0 } },
	{ MI_BANSHEE, ModelSets::MODEL_SET_MODERN,
		{ 0,6, 0,2,12, 18,0, -50,0,0, 0 } },
	{ MI_ADMIRAL, ModelSets::MODEL_SET_MODERN,
		{ 0,0, 2,7,7, 25,0, -57,0,0, 0 } },
	{ MI_PIZZABOY, ModelSets::MODEL_SET_MODERN,
		{ 0,-33, 0,0,0, 0,0, 0,0,0, -1 } },
	{ MI_FAGGIO, ModelSets::MODEL_SET_MODERN,
		{ 0,-38, 0,0,0, 0,0, 0,0,0, -1 } },
	{ MI_PCJ600, ModelSets::MODEL_SET_MODERN,
		{ -19,-16, 0,0,0, 0,0, 0,0,0, -1 } },
	{ MI_SANCHEZ, ModelSets::MODEL_SET_MODERN,
		{ -22,-27, 0,0,0, 0,0, 0,0,0, -1 } },
	// Helm, tuned in the headset. The two model sets put the console at
	// different heights, so the wheel centre and rake differ; the model's
	// own wheel is hidden because the one the port draws replaces it.
	{ MI_SPEEDER, ModelSets::MODEL_SET_CLASSIC,
		{ 0,0, 3,4,12, 14,0, -22,0,0, 0 } },
	{ MI_SPEEDER, ModelSets::MODEL_SET_MODERN,
		{ 0,0, 3,4,9, 16,0, -26,6,0, 0 } },
};

static VehicleViewCalibration
GetDefaultViewCalibration(int model)
{
	VehicleViewCalibration calibration;
	calibration.seatDistanceCm = model == MI_SANCHEZ ? -23 : 0;
	// Tuned in the headset: the DEFAULT view rides too high on the Sanchez.
	calibration.defaultSeatHeightCm = model == MI_SANCHEZ ? -32 : 0;
	const ModelSets::eModelSet set = ModelSets::GetActiveForCategory(
		ModelSets::MODEL_CATEGORY_VEHICLES);
	const BuiltInVehicleViewDefaults *found = nil;
	for(int pass = 0; pass < 2 && !found; pass++){
		const ModelSets::eModelSet wanted = pass == 0 ? set :
			ModelSets::MODEL_SET_CLASSIC;
		for(int i = 0; i < (int)ARRAY_SIZE(gBuiltInVehicleViewDefaults); i++)
			if(gBuiltInVehicleViewDefaults[i].model == model &&
			   gBuiltInVehicleViewDefaults[i].modelSet == wanted){
				found = &gBuiltInVehicleViewDefaults[i];
				break;
			}
		if(set == ModelSets::MODEL_SET_CLASSIC) break;
	}
	if(found){
		const int *v = found->value;
		calibration.seatDistanceCm=v[0]; calibration.seatHeightCm=v[1];
		calibration.wheelCenterXCm=v[2]; calibration.wheelCenterYCm=v[3];
		calibration.wheelCenterZCm=v[4]; calibration.carWheelRadiusCm=v[5];
		calibration.wheelRadiusCm=v[6]; calibration.carWheelPitchHalfDeg=v[7];
		calibration.carWheelYawHalfDeg=v[8]; calibration.carWheelRollHalfDeg=v[9];
		calibration.carWheelVisibilityOverride=v[10];
	}
	return calibration;
}

static int
ReadViewValue(int model, const char *name, int fallback)
{
	char classic[64], active[64];
	sprintf(classic, "VehicleView_%d", model);
	sprintf(active, ModelSets::GetActiveForCategory(
		ModelSets::MODEL_CATEGORY_VEHICLES) == ModelSets::MODEL_SET_MODERN ?
		"VehicleViewModern_%d" : "VehicleView_%d", model);
	int value;
	if(strcmp(active, classic) != 0 && ReadProfileInt(classic, name, &value))
		fallback = value;
	if(ReadProfileInt(active, name, &value)) fallback = value;
	return fallback;
}

static VehicleViewCalibration *
GetViewCalibration(int model)
{
	if(model < MI_FIRST_VEHICLE || model > MI_LAST_VEHICLE)
		return nil;
	VehicleViewCalibration &calibration =
		gVehicleViewCalibration[model-MI_FIRST_VEHICLE];
	if(calibration.valid)
		return &calibration;
	const VehicleViewCalibration defaults = GetDefaultViewCalibration(model);
	calibration.seatDistanceCm = clamp(ReadViewValue(model,
		"SeatDistanceCm", defaults.seatDistanceCm), -100, 100);
	calibration.seatHeightCm = clamp(ReadViewValue(model,
		"SeatHeightCm", defaults.seatHeightCm), -100, 100);
	calibration.defaultSeatHeightCm = clamp(ReadViewValue(model,
		"DefaultSeatHeightCm", defaults.defaultSeatHeightCm),
		-100, 100);
	calibration.defaultSeatDistanceCm = clamp(ReadViewValue(model,
		"DefaultSeatDistanceCm", defaults.defaultSeatDistanceCm),
		-100, 100);
	calibration.wheelCenterXCm = clamp(ReadViewValue(model,
		"WheelCenterXCm", defaults.wheelCenterXCm), -100, 100);
	calibration.wheelCenterYCm = clamp(ReadViewValue(model,
		"WheelCenterYCm", defaults.wheelCenterYCm), -100, 100);
	calibration.wheelCenterZCm = clamp(ReadViewValue(model,
		"WheelCenterZCm", defaults.wheelCenterZCm), -100, 100);
	calibration.carWheelRadiusCm = clamp(ReadViewValue(model,
		"WheelRadiusV2Cm", defaults.carWheelRadiusCm), 0,
		VR_CAR_WHEEL_MAX_RADIUS_CM);
	calibration.wheelRadiusCm = clamp(ReadViewValue(model,
		"WheelRadiusCm", defaults.wheelRadiusCm), -20, 40);
	calibration.carWheelPitchHalfDeg = clamp(ReadViewValue(model,
		"WheelPitchHalfDeg", defaults.carWheelPitchHalfDeg),
		-VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG,
		 VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG);
	calibration.carWheelYawHalfDeg = clamp(ReadViewValue(model,
		"WheelYawHalfDeg", defaults.carWheelYawHalfDeg),
		-VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG,
		 VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG);
	calibration.carWheelRollHalfDeg = clamp(ReadViewValue(model,
		"WheelRollHalfDeg", defaults.carWheelRollHalfDeg),
		-VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG,
		 VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG);
	calibration.carWheelVisibilityOverride = ReadViewValue(model,
		"VirtualWheelVisibility", defaults.carWheelVisibilityOverride) == 0 ? 0 : -1;
	calibration.valid = true;
	return &calibration;
}

static void
SaveViewCalibrationValue(int model, const char *name, int value)
{
	char section[64], text[32];
	sprintf(section, ModelSets::GetActiveForCategory(
		ModelSets::MODEL_CATEGORY_VEHICLES) == ModelSets::MODEL_SET_MODERN ?
		"VehicleViewModern_%d" : "VehicleView_%d", model);
	sprintf(text, "%d", value);
	WritePrivateProfileStringA(section, name, text, kSettingsPath);
}

static void
SaveCalibrationValue(const char *prefix, int model, int hand,
	const char *name, int value)
{
	if(!prefix || !name || hand < 0 || hand >= VR_HAND_COUNT)
		return;
	char section[64], text[32];
	sprintf(section, ModelSets::GetActiveForCategory(
		ModelSets::MODEL_CATEGORY_VEHICLES) == ModelSets::MODEL_SET_MODERN ?
		"%sModern_%d_%s" : "%s_%d_%s", prefix, model,
		hand == 0 ? "Left" : "Right");
	sprintf(text, "%d", value);
	WritePrivateProfileStringA(section, name, text, kSettingsPath);
}

static void
SaveBikeLeanValue(int model, const char *name, int value)
{
	char section[64], text[32];
	sprintf(section, ModelSets::GetActiveForCategory(
		ModelSets::MODEL_CATEGORY_VEHICLES) == ModelSets::MODEL_SET_MODERN ?
		"BikeControlModern_%d" : "BikeControl_%d", model);
	sprintf(text, "%d", value);
	WritePrivateProfileStringA(section, name, text, kSettingsPath);
}

static CVector
RotateAroundAxis(const CVector &value, CVector axis, float angle)
{
	if(axis.MagnitudeSqr() < 0.000001f || angle == 0.0f)
		return value;
	axis.Normalise();
	const float cosine = Cos(angle);
	const float sine = Sin(angle);
	return value*cosine + CrossProduct(axis, value)*sine +
		axis*(DotProduct(axis, value)*(1.0f-cosine));
}

static void
ApplyHandleRotation(CMatrix *matrix,
	const HandleCalibration &calibration)
{
	const float pitch = DEGTORAD((float)calibration.rotationX/
		VEHICLE_CALIBRATION_VALUE_SCALE);
	const float yaw = DEGTORAD((float)calibration.rotationY/
		VEHICLE_CALIBRATION_VALUE_SCALE);
	const float roll = DEGTORAD((float)calibration.rotationZ/
		VEHICLE_CALIBRATION_VALUE_SCALE);
	if(pitch != 0.0f){
		matrix->GetForward() = RotateAroundAxis(matrix->GetForward(),
			matrix->GetRight(), pitch);
		matrix->GetUp() = RotateAroundAxis(matrix->GetUp(),
			matrix->GetRight(), pitch);
	}
	if(yaw != 0.0f){
		matrix->GetRight() = RotateAroundAxis(matrix->GetRight(),
			matrix->GetUp(), yaw);
		matrix->GetForward() = RotateAroundAxis(matrix->GetForward(),
			matrix->GetUp(), yaw);
	}
	if(roll != 0.0f){
		matrix->GetRight() = RotateAroundAxis(matrix->GetRight(),
			matrix->GetForward(), roll);
		matrix->GetUp() = RotateAroundAxis(matrix->GetUp(),
			matrix->GetForward(), roll);
	}
	matrix->GetRight().Normalise();
	matrix->GetForward().Normalise();
	matrix->GetUp().Normalise();
}

struct BikeHandlePose
{
	CVector center;
	CVector right;
	CVector forward;
	CVector up;
};

static void
GetVehicleControlAdjustment(CVehicle *vehicle, CVector *centerOffset,
	float *radiusOffset)
{
	if(centerOffset)
		*centerOffset = CVector(0.0f, 0.0f, 0.0f);
	if(radiusOffset)
		*radiusOffset = 0.0f;
	if(!vehicle)
		return;
	VehicleCategoryCalibration *category = GetCategoryCalibration(vehicle);
	VehicleViewCalibration *model = GetViewCalibration(vehicle->GetModelIndex());
	if(!category || !model)
		return;
	if(centerOffset)
		*centerOffset = CVector(
			(float)(category->wheelCenterXCm+model->wheelCenterXCm)/100.0f,
			(float)(category->wheelCenterYCm+model->wheelCenterYCm)/100.0f,
			(float)(category->wheelCenterZCm+model->wheelCenterZCm)/100.0f);
	if(radiusOffset)
		*radiusOffset = (float)(category->wheelRadiusCm+
			model->wheelRadiusCm)/100.0f;
}

static bool
BuildBikeHandlePose(CBike *bike, BikeHandlePose *pose)
{
	if(!bike || !pose)
		return false;
	// The bars are drawn on the leaned chassis, so the frame the hands are
	// pinned to has to be the leaned one. Taking the upright body basis here
	// is what left the hands hanging beside the bike through every corner.
	bike->CalculateLeanMatrix();
	pose->right = bike->m_leanMatrix.GetRight();
	pose->forward = bike->m_leanMatrix.GetForward();
	pose->up = bike->m_leanMatrix.GetUp();
	if(pose->right.MagnitudeSqr() < 0.0001f ||
	   pose->forward.MagnitudeSqr() < 0.0001f ||
	   pose->up.MagnitudeSqr() < 0.0001f)
		return false;
	pose->right.Normalise();
	pose->forward.Normalise();
	pose->up.Normalise();
	pose->center = bike->GetPosition()+pose->forward*0.30f+pose->up*0.82f;
	if(bike->m_aBikeNodes[BIKE_HANDLEBARS]){
		RwMatrix *handle = RwFrameGetLTM(bike->m_aBikeNodes[BIKE_HANDLEBARS]);
		if(handle)
			pose->center = CVector(handle->pos);
	}
	CVector controlCenterOffset;
	GetVehicleControlAdjustment(bike, &controlCenterOffset, nil);
	pose->center += pose->right*controlCenterOffset.x+
		pose->forward*controlCenterOffset.y+
		pose->up*controlCenterOffset.z;
	return true;
}

// The handlebars are not simply yawed: PreRender turns them by -m_fWheelAngle
// about the model's raked fork axis, and the node also carries the lean and
// the front suspension. Rebuilding that from the body basis leaves the hands
// beside the grips, so read the frame the game has already posed.
static bool
BuildDrawnBikeHandlePose(CBike *bike, BikeHandlePose *pose)
{
	if(!bike || !pose || bike->m_aBikeNodes[BIKE_HANDLEBARS] == nil)
		return false;
	RwMatrix *handle = RwFrameGetLTM(bike->m_aBikeNodes[BIKE_HANDLEBARS]);
	if(handle == nil)
		return false;
	pose->right = CVector(handle->right.x, handle->right.y, handle->right.z);
	pose->forward = CVector(handle->up.x, handle->up.y, handle->up.z);
	pose->up = CVector(handle->at.x, handle->at.y, handle->at.z);
	pose->center = CVector(handle->pos.x, handle->pos.y, handle->pos.z);
	if(pose->right.MagnitudeSqr() < 0.0001f ||
	   pose->forward.MagnitudeSqr() < 0.0001f ||
	   pose->up.MagnitudeSqr() < 0.0001f)
		return false;
	pose->right.Normalise();
	pose->forward.Normalise();
	pose->up.Normalise();
	CVector controlCenterOffset;
	GetVehicleControlAdjustment(bike, &controlCenterOffset, nil);
	pose->center += pose->right*controlCenterOffset.x+
		pose->forward*controlCenterOffset.y+
		pose->up*controlCenterOffset.z;
	return true;
}

static bool
BuildBikeHandleMatrix(int hand, CMatrix *matrix, bool applySteering)
{
	if(!matrix || hand < 0 || hand >= VR_HAND_COUNT ||
	   !IsImmersiveBikeActive())
		return false;
	CBike *bike = GetActivePlayerBike();
	HandleCalibration *calibration =
		GetBikeCalibration(bike->GetModelIndex(), hand);
	if(!calibration)
		return false;
	BikeHandlePose pose;
	if(!BuildBikeHandlePose(bike, &pose))
		return false;
	// The neutral frame stays the reference for anything that must not swing
	// with the bars, the dashboard HUD above all. The hands take the drawn one.
	const bool onDrawnBars =
		applySteering && BuildDrawnBikeHandlePose(bike, &pose);
	float controlRadiusOffset = 0.0f;
	GetVehicleControlAdjustment(bike, nil, &controlRadiusOffset);
	matrix->SetUnity();
	matrix->GetRight() = pose.up*(hand == 0 ? -1.0f : 1.0f);
	matrix->GetForward() = pose.forward;
	matrix->GetUp() =
		CrossProduct(matrix->GetRight(), matrix->GetForward());
	matrix->GetUp().Normalise();
	float lateral = (float)calibration->offsetX/200.0f+
		(hand == 0 ? -controlRadiusOffset : controlRadiusOffset);
	lateral = hand == 0 ? Min(lateral, -0.08f) : Max(lateral, 0.08f);
	matrix->GetPosition() = pose.center+
		pose.right*lateral+
		pose.forward*((float)calibration->offsetY/200.0f)+
		pose.up*((float)calibration->offsetZ/200.0f);
	ApplyHandleRotation(matrix, *calibration);
	const float steeringAngle = IsImmersiveBikeActive(bike) ?
		gImmersiveBikePhysicalAngle : bike->m_fWheelAngle;
	if(applySteering && !onDrawnBars && steeringAngle != 0.0f){
		matrix->GetPosition() = pose.center+RotateAroundAxis(
			matrix->GetPosition()-pose.center, pose.up, steeringAngle);
		matrix->GetRight() = RotateAroundAxis(
			matrix->GetRight(), pose.up, steeringAngle);
		matrix->GetForward() = RotateAroundAxis(
			matrix->GetForward(), pose.up, steeringAngle);
		matrix->GetUp() = RotateAroundAxis(
			matrix->GetUp(), pose.up, steeringAngle);
	}
	if(hand == 1){
		// The throttle hand turns with road speed, over the same travel the
		// physical twist uses and about the axis that solver measures. It is
		// the rendered wrist only; the accelerator is still the trigger.
		const float twist = BikeThrottleVisual(bike)*DEGTORAD(43.0f);
		if(twist > 0.0f){
			// Opening the throttle rolls the wrist back towards the
			// rider, which is the negative direction about this axis.
			matrix->GetRight() = RotateAroundAxis(
				matrix->GetRight(), matrix->GetForward(), -twist);
			matrix->GetUp() = RotateAroundAxis(
				matrix->GetUp(), matrix->GetForward(), -twist);
		}
	}
	return true;
}

struct CarWheelPose
{
	CVector center;
	CVector right;
	CVector up;
	CVector normal;
	float radius;
};

static bool
BuildCarWheelPose(CVehicle *car, CarWheelPose *pose)
{
	if(!car || !pose)
		return false;
	CVehicleModelInfo *model = (CVehicleModelInfo*)
		CModelInfo::GetModelInfo(car->GetModelIndex());
	if(!model)
		return false;
	CVector local = model->GetFrontSeatPosn();
	// CPed seats a driver by the same rule: a car stores the passenger side
	// and has to be mirrored, a boat already stores the helm side. Mirroring
	// a boat too put the wheel across the deck from the seat.
	if(!car->IsBoat())
		local.x = -local.x;
	local.y += 0.33f;
	local.z += 0.30f;
	CVector controlCenterOffset;
	GetVehicleControlAdjustment(car, &controlCenterOffset, nil);
	local += controlCenterOffset;
	pose->center = car->GetPosition()+Multiply3x3(car->GetMatrix(), local);
	pose->normal = car->GetForward();
	pose->right = car->GetRight();
	if(pose->normal.MagnitudeSqr() < 0.0001f ||
	   pose->right.MagnitudeSqr() < 0.0001f)
		return false;
	pose->normal.Normalise();
	pose->right -= pose->normal*DotProduct(pose->right, pose->normal);
	if(pose->right.MagnitudeSqr() < 0.0001f)
		return false;
	pose->right.Normalise();
	pose->up = CrossProduct(pose->right, pose->normal);
	if(pose->up.MagnitudeSqr() < 0.0001f)
		return false;
	pose->up.Normalise();
	VehicleCategoryCalibration *category = GetCategoryCalibration(car);
	VehicleViewCalibration *view = GetViewCalibration(car->GetModelIndex());
	const int pitchHalfDeg = clamp(
		(category ? category->carWheelPitchHalfDeg : 0)+
		(view ? view->carWheelPitchHalfDeg : 0),
		-VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG,
		 VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG);
	const int yawHalfDeg = clamp(
		(category ? category->carWheelYawHalfDeg : 0)+
		(view ? view->carWheelYawHalfDeg : 0),
		-VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG,
		 VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG);
	const int rollHalfDeg = clamp(
		(category ? category->carWheelRollHalfDeg : 0)+
		(view ? view->carWheelRollHalfDeg : 0),
		-VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG,
		 VR_CAR_WHEEL_MAX_ROTATION_HALF_DEG);
	const float pitch = DEGTORAD((float)pitchHalfDeg/2.0f);
	const float yaw = DEGTORAD((float)yawHalfDeg/2.0f);
	const float roll = DEGTORAD((float)rollHalfDeg/2.0f);
	if(pitch != 0.0f){
		pose->normal = RotateAroundAxis(pose->normal, pose->right, pitch);
		pose->up = RotateAroundAxis(pose->up, pose->right, pitch);
	}
	if(yaw != 0.0f){
		pose->right = RotateAroundAxis(pose->right, pose->up, yaw);
		pose->normal = RotateAroundAxis(pose->normal, pose->up, yaw);
	}
	if(roll != 0.0f){
		pose->right = RotateAroundAxis(pose->right, pose->normal, roll);
		pose->up = RotateAroundAxis(pose->up, pose->normal, roll);
	}
	pose->normal.Normalise();
	pose->right -= pose->normal*DotProduct(pose->right, pose->normal);
	if(pose->right.MagnitudeSqr() < 0.0001f)
		return false;
	pose->right.Normalise();
	pose->up = CrossProduct(pose->right, pose->normal);
	pose->up.Normalise();
	int radiusCm = category ? category->carWheelRadiusCm :
		VR_CAR_WHEEL_DEFAULT_RADIUS_CM;
	if(view && view->carWheelRadiusCm != 0)
		radiusCm = view->carWheelRadiusCm;
	pose->radius = (float)clamp(radiusCm,
		VR_CAR_WHEEL_MIN_RADIUS_CM, VR_CAR_WHEEL_MAX_RADIUS_CM)/100.0f;
	return true;
}

static bool
BuildCarWheelCenter(CVehicle *car, CVector *center, CVector *axis)
{
	CarWheelPose pose;
	if(!center || !axis || !BuildCarWheelPose(car, &pose))
		return false;
	*center = pose.center;
	*axis = pose.normal;
	return true;
}

static bool
BuildCarWheelMatrix(int hand, CMatrix *matrix, bool applySteering)
{
	if(!matrix || hand < 0 || hand >= VR_HAND_COUNT ||
	   !IsVrCarActive())
		return false;
	CVehicle *car = GetActivePlayerCar();
	CarWheelPose pose;
	if(!BuildCarWheelPose(car, &pose))
		return false;
	matrix->SetUnity();
	matrix->GetRight() = pose.right;
	matrix->GetForward() = pose.normal;
	matrix->GetUp() =
		CrossProduct(matrix->GetRight(), matrix->GetForward());
	matrix->GetUp().Normalise();
	matrix->GetPosition() = pose.center+
		pose.right*((hand == 0 ? -1.0f : 1.0f)*pose.radius);
	const float physicalAngle = IsImmersiveCarActive() ?
		gImmersiveCarPhysicalAngle : gMotionVehiclePhysicalAngle;
	if(applySteering && physicalAngle != 0.0f){
		const float angle = -physicalAngle;
		matrix->GetPosition() = pose.center+RotateAroundAxis(
			matrix->GetPosition()-pose.center, pose.normal, angle);
		matrix->GetRight() = RotateAroundAxis(
			matrix->GetRight(), pose.normal, angle);
		matrix->GetForward() = RotateAroundAxis(
			matrix->GetForward(), pose.normal, angle);
		matrix->GetUp() = RotateAroundAxis(
			matrix->GetUp(), pose.normal, angle);
	}
	return true;
}

static void
UpdateImmersiveCarModelSteeringWheelInternal(CVehicle *vehicle)
{
	if(!vehicle || !vehicle->IsCar())
		return;
	CAutomobile *car = (CAutomobile*)vehicle;
	RwFrame *wheelFrame = car->m_aCarNodes[CAR_STEERING_WHEEL];
	if(!wheelFrame || !car->m_bVrSteeringWheelNeutralValid)
		return;

	float physicalAngle = 0.0f;
	if(IsImmersiveCarActive(car))
		physicalAngle = gImmersiveCarPhysicalAngle;
	else if(IsMotionEnvironmentActive() && IsVrCarActive(car))
		physicalAngle = gMotionVehiclePhysicalAngle;
	const float visualAngle = -physicalAngle;
	if(Abs(visualAngle-car->m_fVrSteeringWheelAppliedAngle) < 0.0001f)
		return;

	CMatrix wheelMatrix(RwFrameGetMatrix(wheelFrame), false);
	wheelMatrix.CopyOnlyMatrix(car->m_vrSteeringWheelNeutralMatrix);
	if(visualAngle != 0.0f){
		RwFrame *parent = RwFrameGetParent(wheelFrame);
		CarWheelPose wheelPose;
		if(!parent || !BuildCarWheelPose(car, &wheelPose)){
			wheelMatrix.UpdateRW();
			car->m_fVrSteeringWheelAppliedAngle = 0.0f;
			return;
		}
		// Candidate atomics do not share a common authored local axis or pivot.
		// Convert the authoritative calibrated wheel centre and normal into the
		// frame parent's space, then rotate both basis and origin around that pivot.
		// This also handles DFFs whose isolated wheel geometry is authored in car
		// coordinates with a zero frame origin; rotating the frame at its own origin
		// would otherwise swing the wheel through the dashboard.
		CMatrix parentWorld(RwFrameGetLTM(parent), false);
		CMatrix inverseParent;
		Invert(parentWorld, inverseParent);
		CVector localAxis = Multiply3x3(inverseParent, wheelPose.normal);
		if(localAxis.MagnitudeSqr() < 0.0001f){
			wheelMatrix.UpdateRW();
			car->m_fVrSteeringWheelAppliedAngle = 0.0f;
			return;
		}
		localAxis.Normalise();
		const CVector localPivot = inverseParent*wheelPose.center;
		wheelMatrix.GetPosition() = localPivot+RotateAroundAxis(
			wheelMatrix.GetPosition()-localPivot, localAxis, visualAngle);
		wheelMatrix.GetRight() = RotateAroundAxis(
			wheelMatrix.GetRight(), localAxis, visualAngle);
		wheelMatrix.GetForward() = RotateAroundAxis(
			wheelMatrix.GetForward(), localAxis, visualAngle);
		wheelMatrix.GetUp() = RotateAroundAxis(
			wheelMatrix.GetUp(), localAxis, visualAngle);
	}
	wheelMatrix.UpdateRW();
	car->m_fVrSteeringWheelAppliedAngle = visualAngle;
}

static float
WrapAngle(float angle)
{
	while(angle > PI) angle -= TWOPI;
	while(angle < -PI) angle += TWOPI;
	return angle;
}

static float
UnwrapAngle(float angle, float reference)
{
	while(angle-reference > PI) angle -= TWOPI;
	while(angle-reference < -PI) angle += TWOPI;
	return angle;
}

static float
PlanarAngle(const CVector &vector, const CVector &right,
	const CVector &up)
{
	return atan2f(DotProduct(vector, up), DotProduct(vector, right));
}

static bool
WorldVectorToTracking(const CVector &world, CVector *tracking)
{
	if(!tracking)
		return false;
	const float source[3] = { world.x, world.y, world.z };
	float result[3];
	if(!rw::vulkan::firstPersonWorldVectorToPlay(source, result))
		return false;
	*tracking = CVector(result[0], result[1], result[2]);
	return true;
}

static bool
GetTrackingHandPosition(const androidgame::PadInput &input, int hand,
	CVector *position)
{
	if(!position || hand < 0 || hand >= VR_HAND_COUNT ||
	   !input.gripPose[hand].valid)
		return false;
	*position = CVector(input.gripPose[hand].position[0],
		input.gripPose[hand].position[1],
		input.gripPose[hand].position[2]);
	return isfinite(position->x) && isfinite(position->y) &&
		isfinite(position->z);
}

static bool
CaptureBikeOneHandSteering(CBike *bike, uint32 realHandMask,
	const CVector &neutralChord, const BikeHandlePose &pose,
	float physicalAngle, const androidgame::PadInput &input)
{
	if(!bike || (realHandMask != 1u && realHandMask != 2u))
		return false;
	gBikeOneHandSteeringState = BikeOneHandSteeringState();
	const int hand = realHandMask == 1u ? 0 : 1;
	CVector handTracking;
	if(!GetTrackingHandPosition(input, hand, &handTracking))
		return false;

	CVector rightTracking, forwardTracking;
	if(!WorldVectorToTracking(pose.right, &rightTracking) ||
	   !WorldVectorToTracking(pose.forward, &forwardTracking) ||
	   rightTracking.MagnitudeSqr() < 0.0001f ||
	   forwardTracking.MagnitudeSqr() < 0.0001f)
		return false;
	rightTracking.Normalise();
	forwardTracking -= rightTracking*
		DotProduct(forwardTracking, rightTracking);
	if(forwardTracking.MagnitudeSqr() < 0.0001f)
		return false;
	forwardTracking.Normalise();

	CVector seedChord = neutralChord;
	if(physicalAngle != 0.0f)
		seedChord = RotateAroundAxis(seedChord, pose.up, physicalAngle);
	CVector seedTracking;
	if(!WorldVectorToTracking(seedChord, &seedTracking))
		return false;
	const float seedRight = DotProduct(seedTracking, rightTracking);
	const float seedForward = DotProduct(seedTracking, forwardTracking);
	if(seedRight*seedRight+seedForward*seedForward <= 0.0001f)
		return false;

	gBikeOneHandSteeringState.vehicle = bike;
	gBikeOneHandSteeringState.realHandMask = realHandMask;
	gBikeOneHandSteeringState.valid = true;
	gBikeOneHandSteeringState.referenceHandTracking = handTracking;
	gBikeOneHandSteeringState.seedChordTracking = seedTracking;
	gBikeOneHandSteeringState.rightTracking = rightTracking;
	gBikeOneHandSteeringState.forwardTracking = forwardTracking;
	gBikeOneHandSteeringState.referencePhysicalAngle = physicalAngle;
	return true;
}

static bool
RebaseBikeOneHandSteering(CBike *bike, uint32 realHandMask,
	float appliedAngle, const androidgame::PadInput &input)
{
	if(!bike || !gBikeOneHandSteeringState.valid ||
	   gBikeOneHandSteeringState.vehicle != bike ||
	   gBikeOneHandSteeringState.realHandMask != realHandMask ||
	   (realHandMask != 1u && realHandMask != 2u))
		return false;
	const int hand = realHandMask == 1u ? 0 : 1;
	CVector currentHandTracking;
	if(!GetTrackingHandPosition(input, hand, &currentHandTracking))
		return false;
	CVector steeringAxis = CrossProduct(
		gBikeOneHandSteeringState.rightTracking,
		gBikeOneHandSteeringState.forwardTracking);
	if(steeringAxis.MagnitudeSqr() < 0.0001f)
		return false;
	steeringAxis.Normalise();
	const float appliedDelta = WrapAngle(appliedAngle-
		gBikeOneHandSteeringState.referencePhysicalAngle);
	gBikeOneHandSteeringState.seedChordTracking = RotateAroundAxis(
		gBikeOneHandSteeringState.seedChordTracking, steeringAxis,
		appliedDelta);
	gBikeOneHandSteeringState.referenceHandTracking = currentHandTracking;
	gBikeOneHandSteeringState.referencePhysicalAngle = appliedAngle;
	return true;
}

static bool
SolveBikeOneHandSteering(CBike *bike, uint32 realHandMask,
	float *desiredAngle, const androidgame::PadInput &input)
{
	if(!bike || !desiredAngle || !gBikeOneHandSteeringState.valid ||
	   gBikeOneHandSteeringState.vehicle != bike ||
	   gBikeOneHandSteeringState.realHandMask != realHandMask ||
	   (realHandMask != 1u && realHandMask != 2u))
		return false;
	const int hand = realHandMask == 1u ? 0 : 1;
	CVector currentHandTracking;
	if(!GetTrackingHandPosition(input, hand, &currentHandTracking))
		return false;
	const float handSign = realHandMask == 2u ? 2.0f : -2.0f;
	const CVector actualChord =
		gBikeOneHandSteeringState.seedChordTracking+
		(currentHandTracking-
		 gBikeOneHandSteeringState.referenceHandTracking)*handSign;
	const float actualRight = DotProduct(actualChord,
		gBikeOneHandSteeringState.rightTracking);
	const float actualForward = DotProduct(actualChord,
		gBikeOneHandSteeringState.forwardTracking);
	if(actualRight*actualRight+actualForward*actualForward <= 0.0001f ||
	   !isfinite(actualChord.x) || !isfinite(actualChord.y) ||
	   !isfinite(actualChord.z))
		return false;
	const float seedAngle = PlanarAngle(
		gBikeOneHandSteeringState.seedChordTracking,
		gBikeOneHandSteeringState.rightTracking,
		gBikeOneHandSteeringState.forwardTracking);
	const float currentAngle = PlanarAngle(actualChord,
		gBikeOneHandSteeringState.rightTracking,
		gBikeOneHandSteeringState.forwardTracking);
	*desiredAngle = gBikeOneHandSteeringState.referencePhysicalAngle+
		WrapAngle(currentAngle-seedAngle);
	return isfinite(*desiredAngle);
}

static void
ResetBikeInteraction()
{
	for(int hand = 0; hand < VR_HAND_COUNT; hand++){
		gBikeHandleGrabbed[hand] = false;
		gBikeHandleGripDown[hand] = false;
		gBikeHandleDistance[hand] = 1000.0f;
		gBikeLeanReferenceValid[hand] = false;
		gBikeLeanReferenceTrackingY[hand] = 0.0f;
	}
	gImmersiveBikeSteering = 0.0f;
	gImmersiveBikePhysicalAngle = 0.0f;
	gImmersiveBikeDesiredAngle = 0.0f;
	gImmersiveBikeSteeringOverflow = 0.0f;
	gImmersiveBikeThrottle = 0.0f;
	gImmersiveBikeLean = 0.0f;
	gBikeLeanGestureState = 0;
	gBikeThrottleGestureActive = false;
	gBikeThrottleReferenceValid = false;
	gBikeSteeringChordState = ImmersiveSteeringChordState();
	gBikeOneHandSteeringState = BikeOneHandSteeringState();
}

static void
ResetCarInteraction()
{
	for(int hand = 0; hand < VR_HAND_COUNT; hand++){
		gCarWheelGrabbed[hand] = false;
		gCarWheelGripDown[hand] = false;
		gCarWheelDistance[hand] = 1000.0f;
		gCarWheelGrabReferenceAngle[hand] = 0.0f;
		gCarWheelContinuousAngle[hand] = 0.0f;
		gCarWheelAngleValid[hand] = false;
		gCarWheelSpokeUsable[hand] = false;
		gCarHornContact[hand] = false;
		gCarHornArmed[hand] = false;
		gCarHornPreviousDistance[hand] = 1000.0f;
	}
	gImmersiveCarSteering = 0.0f;
	gImmersiveCarPhysicalAngle = 0.0f;
	gCarWheelTwoHandReferenceAngle = 0.0f;
	gCarWheelTwoHandContinuousAngle = 0.0f;
	gCarWheelTwoHandAngleValid = false;
	gImmersiveCarHornPressed = false;
}

static void
ResetMotionInteraction()
{
	gMotionSteeringVehicle = nil;
	gMotionVehicleSteering = 0.0f;
	gMotionVehiclePhysicalAngle = 0.0f;
	gMotionSteeringReferenceValid = false;
	gMotionSteeringReferenceHeading = 0.0f;
}

static bool
GetRawHandPosition(int hand, CVector *position)
{
	CMatrix matrix;
	if(!position || !GetQuestRawTrackedHandMatrix(hand, &matrix))
		return false;
	*position = matrix.GetPosition();
	return true;
}

static void
UpdateCarHorn(CVehicle *car, const CVector &center,
	uint32 blockedHands)
{
	if(!car){
		gImmersiveCarHornPressed = false;
		return;
	}
	bool pressed = false;
	for(int hand = 0; hand < VR_HAND_COUNT; hand++){
		const bool unavailable =
			(blockedHands & (1u << hand)) != 0 ||
			gCarWheelGrabbed[hand] ||
			IsQuestDrivingHandUnavailable(hand);
		CVector handPosition;
		if(unavailable ||
		   !GetRawHandPosition(hand, &handPosition)){
			gCarHornContact[hand] = false;
			gCarHornArmed[hand] = false;
			gCarHornPreviousDistance[hand] = 1000.0f;
			continue;
		}
		const float distance = (handPosition-center).Magnitude();
		const float previous = gCarHornPreviousDistance[hand];
		if(gCarHornContact[hand]){
			if(distance > 0.15f)
				gCarHornContact[hand] = false;
		}else{
			if(distance >= 0.12f && distance <= 0.26f &&
			   previous < 999.0f && distance < previous-0.002f)
				gCarHornArmed[hand] = true;
			if(distance > 0.26f)
				gCarHornArmed[hand] = false;
			if(gCarHornArmed[hand] && distance <= 0.105f &&
			   previous < 999.0f && distance < previous){
				gCarHornContact[hand] = true;
				gCarHornArmed[hand] = false;
			}
		}
		gCarHornPreviousDistance[hand] = distance;
		pressed = pressed || gCarHornContact[hand];
	}
	gImmersiveCarHornPressed = pressed;
}

static uint32
UpdateImmersiveCarInput(const float *grips, uint32 blockedHands)
{
	if(!grips || !IsImmersiveCarActive()){
		ResetCarInteraction();
		return 0;
	}
	CVehicle *car = GetActivePlayerCar();
	if(!car){
		ResetCarInteraction();
		return 0;
	}
	RestrictQuestVehicleWeaponsToSidearms();
	CMatrix anchors[VR_HAND_COUNT];
	CVector positions[VR_HAND_COUNT];
	bool anchorValid[VR_HAND_COUNT] = {};
	bool poseValid[VR_HAND_COUNT] = {};
	bool justGrabbed[VR_HAND_COUNT] = {};
	for(int hand = 0; hand < VR_HAND_COUNT; hand++){
		anchorValid[hand] =
			BuildCarWheelMatrix(hand, &anchors[hand], false);
		poseValid[hand] =
			GetRawHandPosition(hand, &positions[hand]);
		gCarWheelDistance[hand] =
			anchorValid[hand] && poseValid[hand] ?
				(positions[hand]-
				 anchors[hand].GetPosition()).Magnitude() :
				1000.0f;
		const bool unavailable =
			(blockedHands & (1u << hand)) != 0 ||
			!anchorValid[hand] || !poseValid[hand] ||
			IsQuestDrivingHandUnavailable(hand);
		if(gCarWheelGrabbed[hand] &&
		   (unavailable || grips[hand] <= 0.30f)){
			gCarWheelGrabbed[hand] = false;
			gCarWheelAngleValid[hand] = false;
			gCarWheelSpokeUsable[hand] = false;
		}
		if(!gCarWheelGrabbed[hand] && !unavailable &&
		   grips[hand] >= 0.65f && !gCarWheelGripDown[hand] &&
		   gCarWheelDistance[hand] <= 0.23f){
			gCarWheelGrabbed[hand] = true;
			justGrabbed[hand] = true;
		}
		if(grips[hand] <= 0.30f)
			gCarWheelGripDown[hand] = false;
		else if(grips[hand] >= 0.65f)
			gCarWheelGripDown[hand] = true;
	}

	const bool left = gCarWheelGrabbed[0] && anchorValid[0];
	const bool right = gCarWheelGrabbed[1] && anchorValid[1];
	CVector center, axis;
	if(!BuildCarWheelCenter(car, &center, &axis)){
		ResetCarInteraction();
		return 0;
	}
	if(anchorValid[0] && anchorValid[1])
		center = (anchors[0].GetPosition()+
			anchors[1].GetPosition())*0.5f;
	CVector carRight = car->GetRight();
	CVector carUp = car->GetUp();
	carRight.Normalise();
	carUp.Normalise();
	for(int hand = 0; hand < VR_HAND_COUNT; hand++){
		if(!justGrabbed[hand])
			continue;
		const float angle = PlanarAngle(
			positions[hand]-center, carRight, carUp);
		gCarWheelGrabReferenceAngle[hand] =
			WrapAngle(angle-gImmersiveCarPhysicalAngle);
		gCarWheelContinuousAngle[hand] =
			gImmersiveCarPhysicalAngle;
		gCarWheelAngleValid[hand] = true;
		gCarWheelSpokeUsable[hand] = true;
	}

	float steeringAngle = 0.0f;
	if(left && right){
		const float chordAngle = PlanarAngle(
			positions[1]-positions[0], carRight, carUp);
		if(!gCarWheelTwoHandAngleValid ||
		   justGrabbed[0] || justGrabbed[1]){
			gCarWheelTwoHandReferenceAngle =
				WrapAngle(chordAngle-gImmersiveCarPhysicalAngle);
			gCarWheelTwoHandContinuousAngle =
				gImmersiveCarPhysicalAngle;
			gCarWheelTwoHandAngleValid = true;
		}
		steeringAngle = WrapAngle(
			chordAngle-gCarWheelTwoHandReferenceAngle);
		steeringAngle = UnwrapAngle(steeringAngle,
			gCarWheelTwoHandContinuousAngle);
		gCarWheelTwoHandContinuousAngle = steeringAngle;
		for(int hand = 0; hand < VR_HAND_COUNT; hand++){
			const float handAngle = PlanarAngle(
				positions[hand]-center, carRight, carUp);
			gCarWheelGrabReferenceAngle[hand] =
				WrapAngle(handAngle-steeringAngle);
			gCarWheelContinuousAngle[hand] = steeringAngle;
			gCarWheelAngleValid[hand] = true;
		}
	}else{
		gCarWheelTwoHandAngleValid = false;
		for(int hand = 0; hand < VR_HAND_COUNT; hand++){
			if(!gCarWheelGrabbed[hand])
				continue;
			const CVector spoke = positions[hand]-center;
			const float spokeRight = DotProduct(spoke, carRight);
			const float spokeUp = DotProduct(spoke, carUp);
			const float raw = PlanarAngle(spoke, carRight, carUp);
			float angle = gCarWheelContinuousAngle[hand];
			const bool usable =
				spokeRight*spokeRight+spokeUp*spokeUp >=
				kCarWheelMinSpoke*kCarWheelMinSpoke;
			bool reseat = !gCarWheelAngleValid[hand];
			if(usable && gCarWheelAngleValid[hand]){
				// Coming back out to the rim, or recovering from a step
				// that cannot have been made: take the reading as the new
				// reference for where the wheel already is, so the hand
				// resumes from here instead of dragging the wheel with it.
				const float measured = UnwrapAngle(WrapAngle(raw-
					gCarWheelGrabReferenceAngle[hand]), angle);
				if(!gCarWheelSpokeUsable[hand] ||
				   Abs(measured-angle) > kCarWheelMaxStep)
					reseat = true;
				else
					angle = measured;
			}else if(!usable)
				reseat = false;
			// Past the lock the wheel cannot turn any further, so let the
			// hand slip around it rather than winding up an angle that has
			// to be unwound before the wheel answers again.
			const float locked =
				clamp(angle, -kCarWheelMaxAngle, kCarWheelMaxAngle);
			if(reseat || locked != angle){
				angle = locked;
				if(usable)
					gCarWheelGrabReferenceAngle[hand] =
						WrapAngle(raw-angle);
			}
			gCarWheelSpokeUsable[hand] = usable;
			gCarWheelContinuousAngle[hand] = angle;
			gCarWheelAngleValid[hand] = true;
			steeringAngle = angle;
			break;
		}
	}

	const float maxSteering = kCarWheelMaxAngle;
	gImmersiveCarPhysicalAngle =
		clamp(steeringAngle, -maxSteering, maxSteering);
	float steering =
		clamp(steeringAngle/maxSteering, -1.0f, 1.0f);
	const float deadZone = 0.03f;
	if(Abs(steering) <= deadZone)
		steering = 0.0f;
	else
		steering = (steering > 0.0f ? 1.0f : -1.0f)*
			(Abs(steering)-deadZone)/(1.0f-deadZone);
	gImmersiveCarSteering = steering;
	const uint32 captured =
		(left ? 1u : 0u) | (right ? 2u : 0u);
	UpdateCarHorn(car, center, blockedHands | captured);
	return captured;
}

static bool
GetMotionHeading(const androidgame::PadInput &input, float *heading)
{
	const int hand = clamp(gMotionSteeringHand, 0, VR_HAND_COUNT-1);
	const androidgame::PadInput::Pose &pose = input.aimPose[hand];
	if(!heading || !pose.valid)
		return false;
	const float x = pose.orientation[0];
	const float y = pose.orientation[1];
	const float z = pose.orientation[2];
	const float w = pose.orientation[3];
	const float forwardX = -2.0f*(x*z+w*y);
	const float forwardZ = -(1.0f-2.0f*(x*x+y*y));
	if(forwardX*forwardX+forwardZ*forwardZ < 0.01f)
		return false;
	*heading = atan2f(forwardX, -forwardZ);
	return true;
}

static void
UpdateMotionInput(const androidgame::PadInput &input, bool blocked)
{
	if(blocked || !IsMotionEnvironmentActive()){
		ResetMotionInteraction();
		return;
	}
	CVehicle *vehicle = FindPlayerVehicle();
	if(!vehicle ||
	   (!IsVrCarActive(vehicle) && !IsVrBikeActive(vehicle))){
		ResetMotionInteraction();
		return;
	}
	RestrictQuestVehicleWeaponsToSidearms();
	if(gMotionSteeringVehicle != vehicle){
		gMotionSteeringVehicle = vehicle;
		gMotionVehicleSteering = 0.0f;
		gMotionVehiclePhysicalAngle = 0.0f;
		gMotionSteeringReferenceValid = false;
		gMotionSteeringReferenceHeading = 0.0f;
	}
	float heading;
	if(!GetMotionHeading(input, &heading)){
		gMotionVehicleSteering = 0.0f;
		gMotionVehiclePhysicalAngle = 0.0f;
		return;
	}
	if(!gMotionSteeringReferenceValid){
		gMotionVehicleSteering = 0.0f;
		gMotionVehiclePhysicalAngle = 0.0f;
		if(input.rightTrigger < 0.15f)
			return;
		gMotionSteeringReferenceHeading = heading;
		gMotionSteeringReferenceValid = true;
	}
	const float maxAngle = DEGTORAD(90.0f);
	const float midAngle = DEGTORAD(30.0f);
	const float fineScale = 0.5f;
	float angle = clamp(-WrapAngle(
		heading-gMotionSteeringReferenceHeading),
		-maxAngle, maxAngle);
	if(Abs(angle) < DEGTORAD(3.0f))
		angle = 0.0f;
	gMotionVehiclePhysicalAngle = angle;
	const float absolute = Abs(angle);
	float steering;
	if(absolute <= midAngle)
		steering = angle/midAngle*fineScale;
	else{
		const float sign = angle >= 0.0f ? 1.0f : -1.0f;
		steering = sign*(fineScale+
			(absolute-midAngle)/(maxAngle-midAngle)*
			(1.0f-fineScale));
	}
	gMotionVehicleSteering = Abs(steering) < 0.01f ?
		0.0f : clamp(steering, -1.0f, 1.0f);
}

static bool
NormalizeQuaternion(const float input[4], float output[4])
{
	if(!input || !output)
		return false;
	const float lengthSqr = input[0]*input[0]+input[1]*input[1]+
		input[2]*input[2]+input[3]*input[3];
	if(!isfinite(lengthSqr) || lengthSqr < 0.000001f)
		return false;
	const float inverseLength = 1.0f/sqrtf(lengthSqr);
	for(int i = 0; i < 4; i++)
		output[i] = input[i]*inverseLength;
	return true;
}

static float
DotQuaternion(const float a[4], const float b[4])
{
	return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]+a[3]*b[3];
}

static void
MultiplyQuaternion(const float a[4], const float b[4], float result[4])
{
	result[0] = a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1];
	result[1] = a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0];
	result[2] = a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3];
	result[3] = a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2];
}

static bool
CaptureBikeThrottleReference(const androidgame::PadInput &input)
{
	return input.gripPose[1].valid &&
		NormalizeQuaternion(input.gripPose[1].orientation,
			gBikeThrottleReferenceOrientation);
}

static bool
GetBikeThrottleTwistAngle(const androidgame::PadInput &input, float *angle)
{
	if(!angle || !input.gripPose[1].valid)
		return false;
	float current[4];
	if(!NormalizeQuaternion(input.gripPose[1].orientation, current))
		return false;
	if(DotQuaternion(gBikeThrottleReferenceOrientation, current) < 0.0f)
		for(int i = 0; i < 4; i++) current[i] = -current[i];
	const float inverseReference[4] = {
		-gBikeThrottleReferenceOrientation[0],
		-gBikeThrottleReferenceOrientation[1],
		-gBikeThrottleReferenceOrientation[2],
		 gBikeThrottleReferenceOrientation[3]
	};
	float relative[4], normalizedRelative[4];
	MultiplyQuaternion(inverseReference, current, relative);
	if(!NormalizeQuaternion(relative, normalizedRelative))
		return false;
	float twist[4] = { 0.0f, 0.0f, normalizedRelative[2],
		normalizedRelative[3] };
	float normalizedTwist[4];
	if(!NormalizeQuaternion(twist, normalizedTwist))
		return false;
	*angle = WrapAngle(2.0f*atan2f(normalizedTwist[2],
		normalizedTwist[3]));
	return isfinite(*angle);
}

static void
UpdateBikeThrottle(CBike *bike, bool rightGrabbed,
	const androidgame::PadInput &input)
{
	if(!gBikeManualThrottle || !bike || !rightGrabbed ||
	   input.rightTrigger < 0.45f){
		gImmersiveBikeThrottle = 0.0f;
		gBikeThrottleGestureActive = false;
		gBikeThrottleReferenceValid = false;
		return;
	}
	if(!gBikeThrottleGestureActive){
		gBikeThrottleReferenceValid = CaptureBikeThrottleReference(input);
		gBikeThrottleGestureActive = gBikeThrottleReferenceValid;
		gImmersiveBikeThrottle = 0.0f;
		return;
	}
	if(!gBikeThrottleReferenceValid){
		gImmersiveBikeThrottle = 0.0f;
		return;
	}
	float delta;
	if(!GetBikeThrottleTwistAngle(input, &delta)){
		gImmersiveBikeThrottle = 0.0f;
		return;
	}
	const float deadZone = DEGTORAD(1.5f);
	const float fullThrottleAngle = DEGTORAD(43.0f);
	gImmersiveBikeThrottle = delta <= deadZone ? 0.0f :
		clamp((delta-deadZone)/(fullThrottleAngle-deadZone),
			0.0f, 1.0f);
}

static void
UpdateBikeLean(CBike *bike, const androidgame::PadInput &input,
	bool left, bool right)
{
	// One-hand steering and ordinary reaching must never become a wheelie or
	// stoppie gesture. Match the PC solver: deliberate lean gestures require
	// both real hands on the bars.
	if(!bike || !left || !right){
		gImmersiveBikeLean = 0.0f;
		gBikeLeanGestureState = 0;
		for(int hand = 0; hand < VR_HAND_COUNT; hand++)
			gBikeLeanReferenceValid[hand] = false;
		return;
	}
	float heightTotal = 0.0f;
	int heightHands = 0;
	for(int hand = 0; hand < VR_HAND_COUNT; hand++){
		const bool grabbed = hand == 0 ? left : right;
		if(!grabbed || !input.gripPose[hand].valid){
			gBikeLeanReferenceValid[hand] = false;
			continue;
		}
		const float currentY =
			input.gripPose[hand].position[1];
		if(!gBikeLeanReferenceValid[hand]){
			gBikeLeanReferenceTrackingY[hand] = currentY;
			gBikeLeanReferenceValid[hand] = true;
			continue;
		}
		heightTotal +=
			currentY-gBikeLeanReferenceTrackingY[hand];
		heightHands++;
	}
	if(heightHands == 0){
		gImmersiveBikeLean = 0.0f;
		gBikeLeanGestureState = 0;
		return;
	}
	BikeLeanCalibration *calibration =
		GetBikeLeanCalibration(bike->GetModelIndex());
	if(!calibration){
		gImmersiveBikeLean = 0.0f;
		gBikeLeanGestureState = 0;
		return;
	}
	const float height = heightTotal/(float)heightHands;
	const float wheelie =
		(float)calibration->wheelieHeightCm/100.0f;
	const float stand =
		(float)calibration->standHeightCm/100.0f;
	if(gBikeLeanGestureState == 0){
		if(height >= wheelie)
			gBikeLeanGestureState = -1;
		else if(height <= -stand)
			gBikeLeanGestureState = 1;
	}else if(gBikeLeanGestureState < 0){
		if(height <= Max(wheelie*0.50f, 0.025f))
			gBikeLeanGestureState = 0;
	}else if(height >= -Max(stand*0.50f, 0.025f))
		gBikeLeanGestureState = 0;
	gImmersiveBikeLean = (float)gBikeLeanGestureState;
}

static uint32
UpdateImmersiveBikeInput(const float *grips,
	const androidgame::PadInput &input, uint32 blockedHands)
{
	if(!grips || !IsImmersiveBikeActive()){
		ResetBikeInteraction();
		return 0;
	}
	CBike *bike = GetActivePlayerBike();
	if(!bike){
		ResetBikeInteraction();
		return 0;
	}
	RestrictQuestVehicleWeaponsToSidearms();
	CMatrix anchors[VR_HAND_COUNT];
	CVector positions[VR_HAND_COUNT];
	bool anchorValid[VR_HAND_COUNT] = {};
	bool poseValid[VR_HAND_COUNT] = {};
	bool justGrabbed[VR_HAND_COUNT] = {};
	for(int hand = 0; hand < VR_HAND_COUNT; hand++){
		anchorValid[hand] =
			BuildBikeHandleMatrix(hand, &anchors[hand], false);
		poseValid[hand] =
			GetRawHandPosition(hand, &positions[hand]);
		gBikeHandleDistance[hand] =
			anchorValid[hand] && poseValid[hand] ?
				(positions[hand]-
				 anchors[hand].GetPosition()).Magnitude() :
				1000.0f;
		const bool unavailable =
			(blockedHands & (1u << hand)) != 0 ||
			!anchorValid[hand] || !poseValid[hand] ||
			IsQuestDrivingHandUnavailable(hand);
		if(gBikeHandleGrabbed[hand] &&
		   (unavailable || grips[hand] <= 0.30f)){
			gBikeHandleGrabbed[hand] = false;
			gBikeLeanReferenceValid[hand] = false;
		}
		if(!gBikeHandleGrabbed[hand] && !unavailable &&
		   grips[hand] >= 0.65f && !gBikeHandleGripDown[hand] &&
		   gBikeHandleDistance[hand] <= 0.17f){
			gBikeHandleGrabbed[hand] = true;
			justGrabbed[hand] = true;
			gBikeLeanReferenceValid[hand] = false;
		}
		if(grips[hand] <= 0.30f)
			gBikeHandleGripDown[hand] = false;
		else if(grips[hand] >= 0.65f)
			gBikeHandleGripDown[hand] = true;
	}
	const bool left =
		gBikeHandleGrabbed[0] && anchorValid[0];
	const bool right =
		gBikeHandleGrabbed[1] && anchorValid[1];
	const uint32 realHandMask =
		(left ? 1u : 0u) | (right ? 2u : 0u);
	BikeHandlePose handlePose;
	if(!BuildBikeHandlePose(bike, &handlePose)){
		ResetBikeInteraction();
		return 0;
	}
	const float maxSteering = DEGTORAD(35.0f);
	gImmersiveBikeDesiredAngle = 0.0f;
	gImmersiveBikeSteeringOverflow = 0.0f;
	float steeringAngle = 0.0f;
	if(realHandMask != 0u){
		const CVector neutral =
			anchors[1].GetPosition()-anchors[0].GetPosition();
		if(realHandMask == 3u){
			gBikeOneHandSteeringState = BikeOneHandSteeringState();
			const CVector actual = positions[1]-positions[0];
			const float neutralRight = DotProduct(neutral, handlePose.right);
			const float neutralForward = DotProduct(neutral, handlePose.forward);
			const float actualRight = DotProduct(actual, handlePose.right);
			const float actualForward = DotProduct(actual, handlePose.forward);
			const bool pointValid =
				neutralRight*neutralRight+neutralForward*neutralForward > 0.0001f &&
				actualRight*actualRight+actualForward*actualForward > 0.0001f &&
				isfinite(actual.x) && isfinite(actual.y) && isfinite(actual.z);
			float rawAngle = 0.0f;
			if(pointValid)
				rawAngle = WrapAngle(
					PlanarAngle(actual, handlePose.right, handlePose.forward)-
					PlanarAngle(neutral, handlePose.right, handlePose.forward));
			if(pointValid){
				const bool ownershipChanged =
					!gBikeSteeringChordState.valid ||
					gBikeSteeringChordState.vehicle != bike ||
					gBikeSteeringChordState.realHandMask != realHandMask ||
					justGrabbed[0] || justGrabbed[1];
				if(ownershipChanged){
					const uint32 captures =
						gBikeSteeringChordState.captureCount+1;
					gBikeSteeringChordState = ImmersiveSteeringChordState();
					gBikeSteeringChordState.vehicle = bike;
					gBikeSteeringChordState.realHandMask = realHandMask;
					gBikeSteeringChordState.captureCount = captures;
					gBikeSteeringChordState.valid = true;
					// A newly added second hand owns a new chord. Latch it to the
					// angle already applied by the one-hand solver, otherwise the
					// first two-hand frame can become an instant steering impulse.
					gBikeSteeringChordState.referenceActive = true;
					gBikeSteeringChordState.referenceAngle =
						WrapAngle(rawAngle-gImmersiveBikePhysicalAngle);
				}
				steeringAngle = gBikeSteeringChordState.referenceActive ?
					WrapAngle(rawAngle-gBikeSteeringChordState.referenceAngle) :
					rawAngle;
				const float physicalDelta = WrapAngle(
					steeringAngle-gImmersiveBikePhysicalAngle);
				const bool discontinuity = !isfinite(steeringAngle) ||
					Abs(physicalDelta) > DEGTORAD(45.0f);
				const float desiredAngle = steeringAngle;
				steeringAngle = discontinuity ? gImmersiveBikePhysicalAngle :
					clamp(desiredAngle, -maxSteering, maxSteering);
				if(discontinuity || steeringAngle != desiredAngle){
					gBikeSteeringChordState.referenceAngle =
						WrapAngle(rawAngle-steeringAngle);
					gBikeSteeringChordState.referenceActive = true;
				}
			}else{
				const uint32 captures = gBikeSteeringChordState.captureCount;
				gBikeSteeringChordState = ImmersiveSteeringChordState();
				gBikeSteeringChordState.captureCount = captures;
				gBikeSteeringChordState.rebaseOnNextValid = true;
				steeringAngle = gImmersiveBikePhysicalAngle;
			}
		}else{
			const bool ownershipChanged =
				!gBikeOneHandSteeringState.valid ||
				gBikeOneHandSteeringState.vehicle != bike ||
				gBikeOneHandSteeringState.realHandMask != realHandMask ||
				!gBikeSteeringChordState.valid ||
				gBikeSteeringChordState.vehicle != bike ||
				gBikeSteeringChordState.realHandMask != realHandMask ||
				justGrabbed[realHandMask == 1u ? 0 : 1];
			if(ownershipChanged){
				const uint32 captures = gBikeSteeringChordState.captureCount+1;
				gBikeSteeringChordState = ImmersiveSteeringChordState();
				gBikeSteeringChordState.vehicle = bike;
				gBikeSteeringChordState.realHandMask = realHandMask;
				gBikeSteeringChordState.captureCount = captures;
				gBikeSteeringChordState.valid = CaptureBikeOneHandSteering(
					bike, realHandMask, neutral, handlePose,
					gImmersiveBikePhysicalAngle, input);
				gBikeSteeringChordState.referenceActive = true;
			}
			float desiredAngle = gImmersiveBikePhysicalAngle;
			const bool pointValid = gBikeSteeringChordState.valid &&
				SolveBikeOneHandSteering(bike, realHandMask,
					&desiredAngle, input);
			steeringAngle = desiredAngle;
			gImmersiveBikeDesiredAngle = desiredAngle;
			const float physicalDelta = WrapAngle(
				desiredAngle-gImmersiveBikePhysicalAngle);
			const bool discontinuity = !pointValid || !isfinite(desiredAngle) ||
				Abs(physicalDelta) > DEGTORAD(45.0f);
			steeringAngle = discontinuity ? gImmersiveBikePhysicalAngle :
				clamp(desiredAngle, -maxSteering, maxSteering);
			gImmersiveBikeSteeringOverflow =
				WrapAngle(desiredAngle-steeringAngle);
			if(discontinuity || steeringAngle != desiredAngle){
				gBikeSteeringChordState.valid = RebaseBikeOneHandSteering(
					bike, realHandMask, steeringAngle, input);
				gBikeSteeringChordState.captureCount++;
			}
		}
		gImmersiveBikePhysicalAngle = clamp(
			steeringAngle, -maxSteering, maxSteering);
	}else{
		gBikeSteeringChordState = ImmersiveSteeringChordState();
		gBikeOneHandSteeringState = BikeOneHandSteeringState();
		gImmersiveBikePhysicalAngle = 0.0f;
	}
	float steering =
		clamp(steeringAngle/maxSteering, -1.0f, 1.0f);
	const float deadZone = 0.035f;
	if(Abs(steering) <= deadZone)
		steering = 0.0f;
	else
		steering = (steering > 0.0f ? 1.0f : -1.0f)*
			(Abs(steering)-deadZone)/(1.0f-deadZone);
	gImmersiveBikeSteering = steering;
	UpdateBikeLean(bike, input, left, right);
	UpdateBikeThrottle(bike, right, input);
	return (left ? 1u : 0u) | (right ? 2u : 0u);
}

static void
MapHornToPad(CControllerState *state)
{
	if(!state || !gImmersiveCarHornPressed)
		return;
	switch(CPad::GetPad(0)->GetMode()){
	case 1:
		state->LeftShoulder1 =
			Max(state->LeftShoulder1, (int16)255);
		break;
	case 2:
		state->RightShoulder1 =
			Max(state->RightShoulder1, (int16)255);
		break;
	default:
		state->LeftShock =
			Max(state->LeftShock, (int16)255);
		break;
	}
}

static void
MapHandbrakeToPad(CControllerState *state, bool pressed)
{
	if(!state || !pressed)
		return;
	switch(CPad::GetPad(0)->GetMode()){
	case 2:
		state->Triangle = Max(state->Triangle, (int16)255);
		break;
	case 3:
		state->LeftShoulder1 = Max(state->LeftShoulder1, (int16)255);
		break;
	default:
		state->RightShoulder1 = Max(state->RightShoulder1, (int16)255);
		break;
	}
}

static void
MapButtonDriveBy(CControllerState *state,
	const androidgame::PadInput &input, bool blocked)
{
	gButtonDriveByVehicle = nil;
	gButtonDriveByDirection = -1;
	CVehicle *vehicle = FindPlayerVehicle();
	if(!state || blocked || !vehicle ||
	   vehicle->pDriver != FindPlayerPed() ||
	   vehicle->IsRealHeli() || vehicle->IsRealPlane())
		return;
	// Pad mode 3 uses R1 for firing; the raw right grip must not fire on its own.
	if(CPad::GetPad(0)->GetMode() == 3)
		state->RightShoulder1 = 0;
	if(!input.b || (IsVrDrivingActive() &&
	    (IsTrackedWeaponHeld(0) || IsTrackedWeaponHeld(1))))
		return;
	// Both grips select the driver's window; one right grip selects the right.
	const bool left = input.leftGrip >= 0.55f;
	const bool right =
		input.rightGrip >= 0.55f && input.leftGrip < 0.55f;
	const bool effectiveDefault =
		GetDrivingTypeForVehicle(vehicle) == VR_DRIVING_DEFAULT ||
		IsQuestVehicleThirdPerson();
	if(!left && !right && (!gVehicleForwardFireEnabled || !effectiveDefault ||
	    (!vehicle->IsCar() && !vehicle->IsBike()))){
		state->Circle = 0;
		return;
	}
	gButtonDriveByVehicle = vehicle;
	gButtonDriveByDirection = left ? 1 : right ? 2 : 0;
	// Grips select the shot, including partially held grips on the other
	// controller. They must not also apply the handbrake or horn.
	state->LeftShoulder1 = 0;
	state->RightShoulder1 = 0;
	state->LeftShoulder2 = left ? 255 : 0;
	state->RightShoulder2 = right ? 255 : 0;
	if(CPad::GetPad(0)->GetMode() == 3)
		state->RightShoulder1 = 255;
	else
		state->Circle = 255;
}

} // namespace

uint32
UpdateQuestDrivingInput(CControllerState *state, bool blocked)
{
	if(!state)
		return 0;
	LoadDrivingSettings();
	const androidgame::PadInput &input = androidgame::GetPadInput();
	CVehicle *vehicle = FindPlayerVehicle();

	const bool radioPressed =
		!blocked && vehicle && input.x;
	gVrRadioChangeJustPressed =
		radioPressed && !gVrRadioButtonDown;
	gVrRadioButtonDown = radioPressed;
	if(vehicle && !blocked){
		// X is a dedicated radio edge in VR. Preserve L2 brake/reverse, but
		// remove CapturePad's face-X contribution to Square. A is the dedicated
		// VR handbrake; acceleration remains on R2, so remove its legacy Cross
		// contribution too. CapturePad already
		// cleared every game input while the VR menu owns the controllers, so
		// never reconstruct L2 from the raw state in that case.
		state->Square = (int16)(clamp(
			input.leftTrigger, 0.0f, 1.0f)*255.0f);
		state->Cross = (int16)(clamp(
			input.rightTrigger, 0.0f, 1.0f)*255.0f);
	}
	const float grips[VR_HAND_COUNT] = {
		input.leftGrip, input.rightGrip
	};
	const uint32 blockedHands = blocked ? 3u : 0u;
	uint32 captured = UpdateImmersiveBikeInput(
		grips, input, blockedHands);
	captured |= UpdateImmersiveCarInput(grips, blockedHands);
	UpdateMotionInput(input, blocked);

	if(IsVrCarActive()){
		// Car grips belong to the virtual wheel/horn system, even while the
		// hands are approaching it. Never leak them into classic radio actions.
		state->LeftShoulder1 = 0;
		state->RightShoulder1 = 0;
	}else{
		// A hand on the bars is steering and a hand holding a sidearm is
		// holding a sidearm; neither is the player reaching for R1. Only
		// the grabbed hand used to be cleared, so a drawn weapon left its
		// grip on R1 -- the handbrake in the shipped pad modes -- and the
		// rider pulled the throttle against a locked wheel.
		const bool bike = IsVrBikeActive();
		for(int hand = 0; hand < VR_HAND_COUNT; hand++){
			if((captured & (1u << hand)) == 0 &&
			   !(bike && IsQuestDrivingHandUnavailable(hand)))
				continue;
			if(hand == 0)
				state->LeftShoulder1 = 0;
			else
				state->RightShoulder1 = 0;
		}
	}
	MapHornToPad(state);
	return captured;
}

void
ApplyQuestVehicleButtonInput(CControllerState *state, bool blocked)
{
	const androidgame::PadInput &input = androidgame::GetPadInput();
	MapButtonDriveBy(state, input, blocked);
	// The explicit handbrake remains available while firing.
	MapHandbrakeToPad(state, FindPlayerVehicle() && !blocked && input.a);
}

void
ResetQuestDrivingInteraction()
{
	gButtonDriveByVehicle = nil;
	gButtonDriveByDirection = -1;
	ResetBikeInteraction();
	ResetCarInteraction();
	ResetMotionInteraction();
	gVrRadioButtonDown = false;
	gVrRadioChangeJustPressed = false;
}

void
UpdateImmersiveCarModelSteeringWheel(CVehicle *vehicle)
{
	UpdateImmersiveCarModelSteeringWheelInternal(vehicle);
}

bool IsImmersiveDrivingActive() { return IsVrDrivingActive(); }

float
QuestBikeVisualLeanScale(CVehicle *vehicle)
{
	LoadDrivingSettings();
	if(!vehicle || !vehicle->IsBike() || !IsVrBikeActive((CBike*)vehicle))
		return 1.0f;
	return (float)gBikeVisualLeanPercent/100.0f;
}

bool
IsQuestBikeViewFollowingTilt()
{
	LoadDrivingSettings();
	return gBikeViewFollowsTilt != 0;
}

void
ToggleQuestBikeViewFollowsTilt()
{
	LoadDrivingSettings();
	gBikeViewFollowsTilt = gBikeViewFollowsTilt ? 0 : 1;
	SaveSetting("BikeViewFollowsTilt", gBikeViewFollowsTilt);
}

bool
CanQuestBikeRiderBeThrown()
{
	LoadDrivingSettings();
	return gBikeRiderCanBeThrown != 0;
}

void
ToggleQuestBikeRiderCanBeThrown()
{
	LoadDrivingSettings();
	gBikeRiderCanBeThrown = gBikeRiderCanBeThrown ? 0 : 1;
	SaveSetting("BikeRiderCanBeThrown", gBikeRiderCanBeThrown);
}

int
GetQuestBikeVisualLeanPercent()
{
	LoadDrivingSettings();
	return gBikeVisualLeanPercent;
}

void
AdjustQuestBikeVisualLeanPercent(int direction)
{
	LoadDrivingSettings();
	gBikeVisualLeanPercent = clamp(gBikeVisualLeanPercent+
		(direction < 0 ? -5 : 5), 25, 100);
	SaveSetting("BikeVisualLeanPercent", gBikeVisualLeanPercent);
}

bool
IsQuestBikeManualThrottle()
{
	LoadDrivingSettings();
	return gBikeManualThrottle != 0;
}

void
ToggleQuestBikeManualThrottle()
{
	LoadDrivingSettings();
	gBikeManualThrottle = gBikeManualThrottle ? 0 : 1;
	SaveSetting("ImmersiveBikeManualThrottle", gBikeManualThrottle);
}

bool
IsQuestCarDrivingDefault()
{
	LoadDrivingSettings();
	return gCarDrivingType == VR_DRIVING_DEFAULT;
}

bool
IsQuestBikeDrivingDefault()
{
	LoadDrivingSettings();
	return gBikeDrivingType == VR_DRIVING_DEFAULT;
}

// In a vehicle the immersive HUD is dashboard instrumentation, not a sticker
// on a controller. The anchor is the neutral control centre carrying the
// vehicle's own basis, with physical steering deliberately left out, so the
// panels neither swing with the hands nor turn with the wheel.
bool
GetQuestVehicleHudAnchor(CMatrix *matrix)
{
	if(!matrix || !IsVrDrivingActive())
		return false;
	CVehicle *vehicle = FindPlayerVehicle();
	if(!vehicle)
		return false;
	CMatrix left, right;
	if(vehicle->IsBike()){
		BikeHandlePose pose;
		if(!BuildBikeHandlePose((CBike*)vehicle, &pose))
			return false;
		if(BuildBikeHandleMatrix(0, &left, false) &&
		   BuildBikeHandleMatrix(1, &right, false))
			pose.center =
				(left.GetPosition()+right.GetPosition())*0.5f;
		matrix->SetUnity();
		matrix->GetRight() = pose.right;
		matrix->GetUp() = pose.up;
		matrix->GetForward() = pose.forward;
		matrix->GetPosition() = pose.center;
		return true;
	}
	if(!BuildCarWheelMatrix(0, &left, false) ||
	   !BuildCarWheelMatrix(1, &right, false))
		return false;
	*matrix = left;
	matrix->GetPosition() = (left.GetPosition()+right.GetPosition())*0.5f;
	matrix->GetRight().Normalise();
	matrix->GetUp().Normalise();
	matrix->GetForward().Normalise();
	return true;
}
bool IsImmersiveCarDrivingActive() { return IsImmersiveCarActive(); }
bool IsImmersiveBikeDrivingActive() { return IsImmersiveBikeActive(); }
bool IsVrCarDrivingActive() { return IsVrCarActive(); }
bool IsVrBikeDrivingActive() { return IsVrBikeActive(); }

int
GetQuestVehicleButtonFireDirection(CVehicle *vehicle)
{
	return vehicle && vehicle == gButtonDriveByVehicle ?
		gButtonDriveByDirection : -1;
}

bool
IsQuestVehicleForwardFireEnabled()
{
	LoadDrivingSettings();
	return gVehicleForwardFireEnabled;
}

void
SetQuestVehicleForwardFireEnabled(bool enabled)
{
	LoadDrivingSettings();
	gVehicleForwardFireEnabled = enabled;
	SaveSetting("DefaultVehicleForwardFire", enabled ? 1 : 0);
}

bool
IsVrRadioControlActive()
{
	// VrHands controls only whether the hand meshes are drawn. Controller
	// tracking and vehicle input stay active when a player hides those meshes.
	return gVrFirstPersonActive && FindPlayerVehicle() != nil;
}

bool
ConsumeVrRadioChange()
{
	if(!IsVrRadioControlActive()){
		gVrRadioChangeJustPressed = false;
		return false;
	}
	const bool pressed = gVrRadioChangeJustPressed;
	gVrRadioChangeJustPressed = false;
	return pressed;
}

bool
GetImmersiveCarSteering(CVehicle *car, float *steering)
{
	if(!steering)
		return false;
	if(IsImmersiveCarActive(car)){
		*steering = gImmersiveCarSteering;
		return true;
	}
	if(IsMotionEnvironmentActive() && IsVrCarActive(car)){
		*steering = gMotionVehicleSteering;
		return true;
	}
	return false;
}

bool
GetImmersiveBikeSteering(CVehicle *bike, float *steering)
{
	if(!steering)
		return false;
	if(IsImmersiveBikeActive(bike)){
		*steering = gImmersiveBikeSteering;
		return true;
	}
	if(IsMotionEnvironmentActive() && IsVrBikeActive(bike)){
		*steering = gMotionVehicleSteering;
		return true;
	}
	return false;
}

// The angle the rider has actually turned the bars through. The fork carries
// the front wheel angle, which is a couple of degrees; this is the one his
// hands are holding.
bool
GetQuestBikeVisualSteerAngle(CVehicle *bike, float *angle)
{
	if(!angle || !IsImmersiveBikeActive(bike))
		return false;
	if(!isfinite(gImmersiveBikePhysicalAngle) ||
	   Abs(gImmersiveBikePhysicalAngle) < 0.00001f)
		return false;
	*angle = gImmersiveBikePhysicalAngle;
	return true;
}

bool
GetImmersiveBikeThrottle(CVehicle *bike, float *throttle)
{
	// Refusing here hands the bike back to the pad accelerator in Bike.cpp,
	// which is the whole point of the option being off.
	if(!throttle || !gBikeManualThrottle || !IsImmersiveBikeActive(bike))
		return false;
	*throttle = gImmersiveBikeThrottle;
	return true;
}

bool
GetImmersiveBikeLean(CVehicle *bike, float *lean)
{
	if(!lean || !IsImmersiveBikeActive(bike))
		return false;
	*lean = gImmersiveBikeLean;
	return true;
}

bool
GetImmersiveBikeHandleMatrix(int hand, CMatrix *matrix)
{
	return BuildBikeHandleMatrix(hand, matrix, true);
}

bool
IsImmersiveBikeHandleGrabbed(int hand)
{
	return hand >= 0 && hand < VR_HAND_COUNT &&
		IsImmersiveBikeActive() && gBikeHandleGrabbed[hand];
}

// The highlight is an instruction, not decoration: players who do not know
// the bars have to be taken hold of never find out otherwise. It goes out
// for the hand that has taken hold, so a rider who knows sees nothing.
// Calibration keeps both lit, since that is what is being aimed at.
bool
ShouldRenderImmersiveBikeHandleMarker(int hand)
{
	if(hand < 0 || hand >= VR_HAND_COUNT || !IsImmersiveBikeActive())
		return false;
	if(gCalibrationPreview)
		return true;
	return gHandleHighlights[VR_VEHICLE_KIND_BIKE] &&
		!gBikeHandleGrabbed[hand];
}

bool
GetImmersiveSteeringHandleMatrix(int hand, CMatrix *matrix)
{
	if(IsVrCarActive())
		return BuildCarWheelMatrix(hand, matrix, true);
	return BuildBikeHandleMatrix(hand, matrix, true);
}

bool
IsImmersiveSteeringHandleGrabbed(int hand)
{
	if(hand < 0 || hand >= VR_HAND_COUNT)
		return false;
	if(IsImmersiveCarActive())
		return gCarWheelGrabbed[hand];
	return IsImmersiveBikeHandleGrabbed(hand);
}

bool
ShouldRenderImmersiveSteeringHandleMarker(int hand)
{
	if(hand < 0 || hand >= VR_HAND_COUNT)
		return false;
	if(IsImmersiveCarActive()){
		if(gCalibrationPreview)
			return true;
		return gHandleHighlights[VehicleKindFor(GetActivePlayerCar())] &&
			!gCarWheelGrabbed[hand];
	}
	return ShouldRenderImmersiveBikeHandleMarker(hand);
}

bool
IsImmersiveBikeSidearm(int weaponType)
{
	return IsImmersiveVehicleSidearm(weaponType);
}

bool
IsImmersiveVehicleSidearm(int weaponType)
{
	switch(weaponType){
	case WEAPONTYPE_COLT45:
	case WEAPONTYPE_PYTHON:
	case WEAPONTYPE_TEC9:
	case WEAPONTYPE_UZI:
	case WEAPONTYPE_SILENCED_INGRAM:
		return true;
	default:
		return false;
	}
}

static const char *
DrivingTypeName(int type)
{
	switch(type){
	case VR_DRIVING_IMMERSIVE: return "IMMERSIVE";
	case VR_DRIVING_MOTION: return "MOTION";
	default: return "DEFAULT";
	}
}

const char *
GetQuestCarDrivingTypeName()
{
	LoadDrivingSettings();
	return DrivingTypeName(gCarDrivingType);
}

const char *
GetQuestBoatDrivingTypeName()
{
	LoadDrivingSettings();
	return DrivingTypeName(gBoatDrivingType);
}

int
GetQuestVehicleKind()
{
	LoadDrivingSettings();
	return gVehicleKindEdit;
}

const char *
GetQuestVehicleKindName()
{
	LoadDrivingSettings();
	switch(gVehicleKindEdit){
	case VR_VEHICLE_KIND_BIKE: return "BIKES";
	case VR_VEHICLE_KIND_BOAT: return "BOATS";
	}
	return "CARS";
}

void
CycleQuestVehicleKind(int direction)
{
	LoadDrivingSettings();
	gVehicleKindEdit = (gVehicleKindEdit+VR_VEHICLE_KIND_COUNT+
		(direction < 0 ? -1 : 1)) % VR_VEHICLE_KIND_COUNT;
}

// The driving type, third person switch and calibration all follow the
// kind the page is showing rather than the vehicle underfoot: a player
// sets bikes up while standing in the street.
const char *
GetQuestVehicleKindDrivingTypeName()
{
	LoadDrivingSettings();
	switch(gVehicleKindEdit){
	case VR_VEHICLE_KIND_BIKE: return DrivingTypeName(gBikeDrivingType);
	case VR_VEHICLE_KIND_BOAT: return DrivingTypeName(gBoatDrivingType);
	}
	return DrivingTypeName(gCarDrivingType);
}

void
CycleQuestVehicleKindDrivingType(int direction)
{
	LoadDrivingSettings();
	switch(gVehicleKindEdit){
	case VR_VEHICLE_KIND_BIKE: CycleQuestBikeDrivingType(direction); break;
	case VR_VEHICLE_KIND_BOAT: CycleQuestBoatDrivingType(direction); break;
	default: CycleQuestCarDrivingType(direction); break;
	}
}

bool
IsQuestVehicleKindDrivingDefault()
{
	LoadDrivingSettings();
	switch(gVehicleKindEdit){
	case VR_VEHICLE_KIND_BIKE: return gBikeDrivingType == VR_DRIVING_DEFAULT;
	case VR_VEHICLE_KIND_BOAT: return gBoatDrivingType == VR_DRIVING_DEFAULT;
	}
	return gCarDrivingType == VR_DRIVING_DEFAULT;
}

bool
IsQuestVehicleKindThirdPerson()
{
	LoadDrivingSettings();
	return gVehicleThirdPerson[gVehicleKindEdit];
}

bool
IsQuestBoatDrivingDefault()
{
	LoadDrivingSettings();
	return gBoatDrivingType == VR_DRIVING_DEFAULT;
}

void
CycleQuestBoatDrivingType(int direction)
{
	LoadDrivingSettings();
	gBoatDrivingType =
		(gBoatDrivingType+VR_DRIVING_TYPE_COUNT+
		 (direction < 0 ? -1 : 1)) % VR_DRIVING_TYPE_COUNT;
	SaveSetting("BoatDrivingType", gBoatDrivingType);
}

const char *
GetQuestBikeDrivingTypeName()
{
	LoadDrivingSettings();
	return DrivingTypeName(gBikeDrivingType);
}

void
CycleQuestCarDrivingType(int direction)
{
	LoadDrivingSettings();
	gCarDrivingType =
		(gCarDrivingType+VR_DRIVING_TYPE_COUNT+
		 (direction < 0 ? -1 : 1)) % VR_DRIVING_TYPE_COUNT;
	SaveSetting("CarDrivingType", gCarDrivingType);
	ResetQuestDrivingInteraction();
}

void
CycleQuestBikeDrivingType(int direction)
{
	LoadDrivingSettings();
	gBikeDrivingType =
		(gBikeDrivingType+VR_DRIVING_TYPE_COUNT+
		 (direction < 0 ? -1 : 1)) % VR_DRIVING_TYPE_COUNT;
	SaveSetting("BikeDrivingType", gBikeDrivingType);
	ResetQuestDrivingInteraction();
}

static int
GetDefaultVehicleViewTypeForCurrentVehicle()
{
	CVehicle *vehicle = FindPlayerVehicle();
	if(!vehicle || GetDrivingTypeForVehicle(vehicle) != VR_DRIVING_DEFAULT)
		return -1;
	if(vehicle->IsBike())
		return VR_DEFAULT_VIEW_BIKE;
	if(vehicle->IsBoat())
		return VR_DEFAULT_VIEW_BOAT;
	if(vehicle->IsCar() && !vehicle->IsRealHeli() && !vehicle->IsRealPlane())
		return VR_DEFAULT_VIEW_CAR;
	return -1;
}

bool
HasQuestDefaultVehicleViewOffsetTarget()
{
	LoadDrivingSettings();
	return GetDefaultVehicleViewTypeForCurrentVehicle() >= 0;
}

const char *
GetQuestDefaultVehicleViewOffsetName()
{
	LoadDrivingSettings();
	switch(GetDefaultVehicleViewTypeForCurrentVehicle()){
	case VR_DEFAULT_VIEW_BIKE: return "BIKE";
	case VR_DEFAULT_VIEW_BOAT: return "BOAT";
	}
	return "CAR";
}

int
GetQuestDefaultVehicleSeatHeightCm()
{
	LoadDrivingSettings();
	const int type = GetDefaultVehicleViewTypeForCurrentVehicle();
	return type >= 0 ? gDefaultVehicleViewOffset[type].seatHeightCm : 0;
}

int
GetQuestDefaultVehicleSeatDistanceCm()
{
	LoadDrivingSettings();
	const int type = GetDefaultVehicleViewTypeForCurrentVehicle();
	return type >= 0 ? gDefaultVehicleViewOffset[type].seatDistanceCm : 0;
}

void
AdjustQuestDefaultVehicleSeatHeightCm(int direction)
{
	LoadDrivingSettings();
	const int type = GetDefaultVehicleViewTypeForCurrentVehicle();
	if(type < 0 || direction == 0)
		return;
	DefaultVehicleViewOffset &offset = gDefaultVehicleViewOffset[type];
	offset.seatHeightCm = clamp(offset.seatHeightCm+direction, -100, 150);
	static const char *const key[VR_DEFAULT_VIEW_COUNT] = {
		"DefaultCarSeatHeightCm", "DefaultBikeSeatHeightCm",
		"DefaultBoatSeatHeightCm"
	};
	SaveSetting(key[type], offset.seatHeightCm);
}

void
AdjustQuestDefaultVehicleSeatDistanceCm(int direction)
{
	LoadDrivingSettings();
	const int type = GetDefaultVehicleViewTypeForCurrentVehicle();
	if(type < 0 || direction == 0)
		return;
	DefaultVehicleViewOffset &offset = gDefaultVehicleViewOffset[type];
	offset.seatDistanceCm = clamp(offset.seatDistanceCm+direction, -100, 100);
	static const char *const key[VR_DEFAULT_VIEW_COUNT] = {
		"DefaultCarSeatDistanceCm", "DefaultBikeSeatDistanceCm",
		"DefaultBoatSeatDistanceCm"
	};
	SaveSetting(key[type], offset.seatDistanceCm);
}

const char *
GetQuestVehicleCategoryName()
{
	static const char *const names[VR_VEHICLE_CATEGORY_COUNT] = {
		"CAR", "BIKE", "BOAT", "HELI"
	};
	const int category = GetVehicleCategory(FindPlayerVehicle());
	return category >= 0 && category < VR_VEHICLE_CATEGORY_COUNT ?
		names[category] : "VEHICLE";
}

bool
HasQuestVehicleSeatCalibrationTarget()
{
	CVehicle *vehicle = FindPlayerVehicle();
	return vehicle && GetViewCalibration(
		vehicle->GetModelIndex()) != nil;
}

int GetQuestVehicleGlobalSeatHeightCm()
{
	VehicleCategoryCalibration *calibration =
		GetCategoryCalibration(FindPlayerVehicle());
	return calibration ? calibration->seatHeightCm : 0;
}

int GetQuestVehicleGlobalSeatDistanceCm()
{
	VehicleCategoryCalibration *calibration =
		GetCategoryCalibration(FindPlayerVehicle());
	return calibration ? calibration->seatDistanceCm : 0;
}

int GetQuestVehicleModelSeatHeightCm()
{
	CVehicle *vehicle = FindPlayerVehicle();
	VehicleViewCalibration *calibration = vehicle ?
		GetViewCalibration(vehicle->GetModelIndex()) : nil;
	if(!calibration)
		return 0;
	return GetDrivingTypeForVehicle(vehicle) == VR_DRIVING_DEFAULT ?
		calibration->defaultSeatHeightCm : calibration->seatHeightCm;
}

int GetQuestVehicleModelSeatDistanceCm()
{
	CVehicle *vehicle = FindPlayerVehicle();
	VehicleViewCalibration *calibration = vehicle ?
		GetViewCalibration(vehicle->GetModelIndex()) : nil;
	if(!calibration)
		return 0;
	return GetDrivingTypeForVehicle(vehicle) == VR_DRIVING_DEFAULT ?
		calibration->defaultSeatDistanceCm : calibration->seatDistanceCm;
}

static void
AdjustQuestVehicleCategorySeat(const char *suffix, int *value, int direction,
	int minimum, int maximum)
{
	CVehicle *vehicle = FindPlayerVehicle();
	const int category = GetVehicleCategory(vehicle);
	if(!value || category < 0 || direction == 0)
		return;
	*value = clamp(*value+direction, minimum, maximum);
	SaveCategoryCalibrationValue(category, suffix, *value);
}

void AdjustQuestVehicleGlobalSeatHeightCm(int direction)
{
	VehicleCategoryCalibration *calibration =
		GetCategoryCalibration(FindPlayerVehicle());
	if(calibration) AdjustQuestVehicleCategorySeat("SeatHeightCm",
		&calibration->seatHeightCm, direction, -100, 150);
}

void AdjustQuestVehicleGlobalSeatDistanceCm(int direction)
{
	VehicleCategoryCalibration *calibration =
		GetCategoryCalibration(FindPlayerVehicle());
	if(calibration) AdjustQuestVehicleCategorySeat("SeatDistanceCm",
		&calibration->seatDistanceCm, direction, -100, 100);
}

static void
AdjustQuestVehicleModelSeat(const char *key, int *value, int direction)
{
	CVehicle *vehicle = FindPlayerVehicle();
	if(!vehicle || !value || direction == 0)
		return;
	*value = clamp(*value+direction, -100, 100);
	SaveViewCalibrationValue(vehicle->GetModelIndex(), key, *value);
}

void AdjustQuestVehicleModelSeatHeightCm(int direction)
{
	CVehicle *vehicle = FindPlayerVehicle();
	VehicleViewCalibration *calibration = vehicle ?
		GetViewCalibration(vehicle->GetModelIndex()) : nil;
	if(!calibration)
		return;
	if(GetDrivingTypeForVehicle(vehicle) == VR_DRIVING_DEFAULT)
		AdjustQuestVehicleModelSeat("DefaultSeatHeightCm",
			&calibration->defaultSeatHeightCm, direction);
	else
		AdjustQuestVehicleModelSeat("SeatHeightCm",
			&calibration->seatHeightCm, direction);
}

void AdjustQuestVehicleModelSeatDistanceCm(int direction)
{
	CVehicle *vehicle = FindPlayerVehicle();
	VehicleViewCalibration *calibration = vehicle ?
		GetViewCalibration(vehicle->GetModelIndex()) : nil;
	if(!calibration)
		return;
	if(GetDrivingTypeForVehicle(vehicle) == VR_DRIVING_DEFAULT)
		AdjustQuestVehicleModelSeat("DefaultSeatDistanceCm",
			&calibration->defaultSeatDistanceCm, direction);
	else
		AdjustQuestVehicleModelSeat("SeatDistanceCm",
			&calibration->seatDistanceCm, direction);
}

int
GetQuestMotionSteeringHand()
{
	LoadDrivingSettings();
	return gMotionSteeringHand;
}

void
ToggleQuestMotionSteeringHand()
{
	LoadDrivingSettings();
	gMotionSteeringHand = 1-gMotionSteeringHand;
	SaveSetting("MotionSteeringHand", gMotionSteeringHand);
	ResetMotionInteraction();
}

// The page reads the kind it is showing; the world reads the kind being
// driven.
bool IsQuestImmersiveCarWheelVisible()
{
	LoadDrivingSettings();
	return gImmersiveCarWheelVisible[
		gVehicleKindEdit == VR_VEHICLE_KIND_BOAT ? 1 : 0];
}

bool ShouldRenderQuestImmersiveCarWheel()
{
	LoadDrivingSettings();
	CVehicle *vehicle = GetActivePlayerCar();
	VehicleViewCalibration *calibration = vehicle ?
		GetViewCalibration(vehicle->GetModelIndex()) : nil;
	return gImmersiveCarWheelVisible[
		(vehicle && vehicle->IsBoat()) ? 1 : 0] &&
		(!calibration || calibration->carWheelVisibilityOverride != 0);
}

void ToggleQuestImmersiveCarWheelVisible()
{
	LoadDrivingSettings();
	const int slot = gVehicleKindEdit == VR_VEHICLE_KIND_BOAT ? 1 : 0;
	gImmersiveCarWheelVisible[slot] = !gImmersiveCarWheelVisible[slot];
	SaveSetting(slot ? "ImmersiveBoatWheelVisible" :
		"ImmersiveCarWheelVisible", gImmersiveCarWheelVisible[slot]);
}

const char *GetQuestVehicleModelWheelVisibilityName()
{
	// Anything steered by a wheel, which since the helm arrived means boats
	// as well. IsCar also answered yes for helicopters, which have no wheel
	// to hide; the active-vehicle helper draws the line in the right place.
	CVehicle *vehicle = GetActivePlayerCar();
	VehicleViewCalibration *calibration = vehicle ?
		GetViewCalibration(vehicle->GetModelIndex()) : nil;
	if(!calibration) return "ENTER A CAR OR BOAT";
	return calibration->carWheelVisibilityOverride == 0 ? "HIDE" : "INHERIT";
}

void ToggleQuestVehicleModelWheelVisibility()
{
	CVehicle *vehicle = GetActivePlayerCar();
	VehicleViewCalibration *calibration = vehicle ?
		GetViewCalibration(vehicle->GetModelIndex()) : nil;
	if(!calibration) return;
	calibration->carWheelVisibilityOverride =
		calibration->carWheelVisibilityOverride == 0 ? -1 : 0;
	SaveViewCalibrationValue(vehicle->GetModelIndex(), "VirtualWheelVisibility",
		calibration->carWheelVisibilityOverride);
}

bool
AreQuestVehicleHandleHighlightsEnabled()
{
	LoadDrivingSettings();
	return gHandleHighlights[gVehicleKindEdit];
}

int
GetQuestWheelHandPullBackMm()
{
	LoadDrivingSettings();
	return gWheelHandPullBackMm;
}

void
AdjustQuestWheelHandPullBackMm(int direction)
{
	LoadDrivingSettings();
	gWheelHandPullBackMm = clamp(gWheelHandPullBackMm+direction*5, -80, 80);
	SaveSetting("WheelHandPullBackMm", gWheelHandPullBackMm);
}

void
ApplyQuestWheelHandPullBack(int hand, CMatrix *matrix)
{
	LoadDrivingSettings();
	if(!matrix || hand < 0 || hand >= VR_HAND_COUNT)
		return;
	if(!IsVrCarActive() || !gCarWheelGrabbed[hand] ||
	   gWheelHandPullBackMm == 0)
		return;
	CMatrix wheel;
	if(!BuildCarWheelMatrix(hand, &wheel, true))
		return;
	// Forward is the wheel's own axis and points down the car, so the
	// negative is towards the driver. Moving within the wheel plane can
	// never separate the rim from a wrist; only leaving the plane can.
	matrix->GetPosition() -= wheel.GetForward()*
		(gWheelHandPullBackMm*0.001f);
}

void
ToggleQuestVehicleHandleHighlights()
{
	LoadDrivingSettings();
	const int kind = gVehicleKindEdit;
	static const char *const key[VR_VEHICLE_KIND_COUNT] = {
		"CarHandleHighlights", "BikeHandleHighlights",
		"BoatHandleHighlights"
	};
	gHandleHighlights[kind] = !gHandleHighlights[kind];
	SaveSetting(key[kind], gHandleHighlights[kind]);
}

bool
IsQuestBikeHorizonLocked()
{
	LoadDrivingSettings();
	return gBikeHorizonLocked;
}

void
ToggleQuestBikeHorizonLock()
{
	LoadDrivingSettings();
	gBikeHorizonLocked = !gBikeHorizonLocked;
	SaveSetting("BikeLockHorizon", gBikeHorizonLocked);
}

bool
IsQuestVehicleThirdPerson()
{
	LoadDrivingSettings();
#if MIAMIVR_STEAM_FRAME
	// THIRD: OVER SHOULDER puts every vehicle on the chase camera, whatever
	// the per-kind settings below say; they are left as they were. On foot
	// there is no vehicle to speak of, and the VEHICLE page keeps showing
	// the stored setting it edits.
	if(androidgame::VrThirdPersonShoulder() && FindPlayerVehicle() != nil)
		return true;
#endif
	// The kind being driven decides, so a player who wants the chase
	// camera on a bike still gets the cockpit in a car.
	return gVehicleThirdPerson[VehicleKindFor(FindPlayerVehicle())];
}

void
ToggleQuestVehicleThirdPerson()
{
	LoadDrivingSettings();
	const int kind = gVehicleKindEdit;
	static const char *const key[VR_VEHICLE_KIND_COUNT] = {
		"VehicleThirdPersonCar", "VehicleThirdPersonBike",
		"VehicleThirdPersonBoat"
	};
	gVehicleThirdPerson[kind] = !gVehicleThirdPerson[kind];
	SaveSetting(key[kind], gVehicleThirdPerson[kind]);
	ResetQuestDrivingInteraction();
}

const char *
GetQuestActiveVehicleName()
{
	CVehicle *vehicle = FindPlayerVehicle();
	return vehicle ?
		GetVrVehicleModelName(vehicle->GetModelIndex()) :
		"NO VEHICLE";
}

bool IsQuestVehicleCalibrationAvailable()
{
	return IsImmersiveBikeActive() || IsImmersiveCarActive();
}

bool IsQuestVehicleCalibrationBike()
{
	return IsImmersiveBikeActive();
}

int
GetQuestVehicleCalibrationItemCount()
{
	// Editable rows only. The menu owns its trailing Back row.
	return IsImmersiveCarActive() ? 14 : 17;
}

int
GetQuestVehicleCalibrationItemForRow(int row)
{
	static const int bikeItems[] = {
		QUEST_VEHICLE_CAL_HAND,
		QUEST_VEHICLE_CAL_OFFSET_X, QUEST_VEHICLE_CAL_OFFSET_Y,
		QUEST_VEHICLE_CAL_OFFSET_Z, QUEST_VEHICLE_CAL_ROT_X,
		QUEST_VEHICLE_CAL_ROT_Y, QUEST_VEHICLE_CAL_ROT_Z,
		QUEST_VEHICLE_CAL_GLOBAL_CENTER_X,
		QUEST_VEHICLE_CAL_GLOBAL_CENTER_Y,
		QUEST_VEHICLE_CAL_GLOBAL_CENTER_Z,
		QUEST_VEHICLE_CAL_GLOBAL_RADIUS,
		QUEST_VEHICLE_CAL_MODEL_CENTER_X,
		QUEST_VEHICLE_CAL_MODEL_CENTER_Y,
		QUEST_VEHICLE_CAL_MODEL_CENTER_Z,
		QUEST_VEHICLE_CAL_MODEL_RADIUS,
		QUEST_VEHICLE_CAL_WHEELIE_HEIGHT,
		QUEST_VEHICLE_CAL_STAND_HEIGHT
	};
	static const int carItems[] = {
		QUEST_VEHICLE_CAL_GLOBAL_CENTER_X,
		QUEST_VEHICLE_CAL_GLOBAL_CENTER_Y,
		QUEST_VEHICLE_CAL_GLOBAL_CENTER_Z,
		QUEST_VEHICLE_CAL_GLOBAL_RADIUS,
		QUEST_VEHICLE_CAL_GLOBAL_PITCH,
		QUEST_VEHICLE_CAL_GLOBAL_YAW,
		QUEST_VEHICLE_CAL_GLOBAL_ROLL,
		QUEST_VEHICLE_CAL_MODEL_CENTER_X,
		QUEST_VEHICLE_CAL_MODEL_CENTER_Y,
		QUEST_VEHICLE_CAL_MODEL_CENTER_Z,
		QUEST_VEHICLE_CAL_MODEL_RADIUS,
		QUEST_VEHICLE_CAL_MODEL_PITCH,
		QUEST_VEHICLE_CAL_MODEL_YAW,
		QUEST_VEHICLE_CAL_MODEL_ROLL
	};
	if(IsImmersiveCarActive())
		return row >= 0 && row < (int)ARRAY_SIZE(carItems) ?
			carItems[row] : -1;
	return row >= 0 && row < (int)ARRAY_SIZE(bikeItems) ?
		bikeItems[row] : -1;
}

const char *
GetQuestVehicleCalibrationItemName(int item)
{
	static const char *const names[QUEST_VEHICLE_CAL_ITEM_COUNT] = {
		"EDIT HANDLE", "LOCAL X OFFSET", "LOCAL Y OFFSET",
		"LOCAL Z OFFSET", "LOCAL ROT X", "LOCAL ROT Y", "LOCAL ROT Z",
		"GLOBAL CENTER X", "GLOBAL CENTER Y", "GLOBAL CENTER Z",
		"GLOBAL RADIUS", "MODEL CENTER X", "MODEL CENTER Y",
		"MODEL CENTER Z", "MODEL RADIUS", "GLOBAL WHEEL PITCH",
		"GLOBAL WHEEL YAW", "GLOBAL WHEEL ROLL", "MODEL WHEEL PITCH",
		"MODEL WHEEL YAW", "MODEL WHEEL ROLL", "WHEELIE HAND HEIGHT",
		"STAND HAND DROP"
	};
	return item >= 0 && item < QUEST_VEHICLE_CAL_ITEM_COUNT ?
		names[item] : "VALUE";
}

bool IsQuestVehicleCalibrationRotation(int item)
{
	return (item >= QUEST_VEHICLE_CAL_ROT_X &&
		item <= QUEST_VEHICLE_CAL_ROT_Z) ||
		(item >= QUEST_VEHICLE_CAL_GLOBAL_PITCH &&
		 item <= QUEST_VEHICLE_CAL_MODEL_ROLL);
}

bool IsQuestVehicleCalibrationWholeCentimeters(int item)
{
	return (item >= QUEST_VEHICLE_CAL_GLOBAL_CENTER_X &&
		item <= QUEST_VEHICLE_CAL_MODEL_RADIUS) ||
		item == QUEST_VEHICLE_CAL_WHEELIE_HEIGHT ||
		item == QUEST_VEHICLE_CAL_STAND_HEIGHT;
}

int
GetQuestVehicleCalibrationValue(int hand, int item)
{
	CVehicle *vehicle = FindPlayerVehicle();
	if(!vehicle || hand < 0 || hand >= VR_HAND_COUNT)
		return 0;
	const bool bike = vehicle->IsBike();
	HandleCalibration *calibration = bike ?
		GetBikeCalibration(vehicle->GetModelIndex(), hand) :
		nil;
	BikeLeanCalibration *lean = bike ?
		GetBikeLeanCalibration(vehicle->GetModelIndex()) : nil;
	VehicleCategoryCalibration *category = GetCategoryCalibration(vehicle);
	VehicleViewCalibration *view = GetViewCalibration(vehicle->GetModelIndex());
	if(item == QUEST_VEHICLE_CAL_HAND)
		return hand;
	switch(item){
	case QUEST_VEHICLE_CAL_OFFSET_X: return calibration ? calibration->offsetX : 0;
	case QUEST_VEHICLE_CAL_OFFSET_Y: return calibration ? calibration->offsetY : 0;
	case QUEST_VEHICLE_CAL_OFFSET_Z: return calibration ? calibration->offsetZ : 0;
	case QUEST_VEHICLE_CAL_ROT_X: return calibration ? calibration->rotationX : 0;
	case QUEST_VEHICLE_CAL_ROT_Y: return calibration ? calibration->rotationY : 0;
	case QUEST_VEHICLE_CAL_ROT_Z: return calibration ? calibration->rotationZ : 0;
	case QUEST_VEHICLE_CAL_GLOBAL_CENTER_X: return category ? category->wheelCenterXCm : 0;
	case QUEST_VEHICLE_CAL_GLOBAL_CENTER_Y: return category ? category->wheelCenterYCm : 0;
	case QUEST_VEHICLE_CAL_GLOBAL_CENTER_Z: return category ? category->wheelCenterZCm : 0;
	case QUEST_VEHICLE_CAL_GLOBAL_RADIUS: return category ?
		(bike ? category->wheelRadiusCm : category->carWheelRadiusCm) : 0;
	case QUEST_VEHICLE_CAL_MODEL_CENTER_X: return view ? view->wheelCenterXCm : 0;
	case QUEST_VEHICLE_CAL_MODEL_CENTER_Y: return view ? view->wheelCenterYCm : 0;
	case QUEST_VEHICLE_CAL_MODEL_CENTER_Z: return view ? view->wheelCenterZCm : 0;
	case QUEST_VEHICLE_CAL_MODEL_RADIUS: return view ?
		(bike ? view->wheelRadiusCm : view->carWheelRadiusCm) : 0;
	case QUEST_VEHICLE_CAL_GLOBAL_PITCH: return category ? category->carWheelPitchHalfDeg : 0;
	case QUEST_VEHICLE_CAL_GLOBAL_YAW: return category ? category->carWheelYawHalfDeg : 0;
	case QUEST_VEHICLE_CAL_GLOBAL_ROLL: return category ? category->carWheelRollHalfDeg : 0;
	case QUEST_VEHICLE_CAL_MODEL_PITCH: return view ? view->carWheelPitchHalfDeg : 0;
	case QUEST_VEHICLE_CAL_MODEL_YAW: return view ? view->carWheelYawHalfDeg : 0;
	case QUEST_VEHICLE_CAL_MODEL_ROLL: return view ? view->carWheelRollHalfDeg : 0;
	case QUEST_VEHICLE_CAL_WHEELIE_HEIGHT:
		return lean ? lean->wheelieHeightCm : 0;
	case QUEST_VEHICLE_CAL_STAND_HEIGHT:
		return lean ? lean->standHeightCm : 0;
	default:
		return 0;
	}
}

void
AdjustQuestVehicleCalibrationValue(int hand, int item, int direction)
{
	CVehicle *vehicle = FindPlayerVehicle();
	if(!vehicle || hand < 0 || hand >= VR_HAND_COUNT ||
	   direction == 0)
		return;
	const bool bike = vehicle->IsBike();
	const int model = vehicle->GetModelIndex();
	HandleCalibration *calibration = bike ?
		GetBikeCalibration(model, hand) :
		nil;
	BikeLeanCalibration *lean = bike ?
		GetBikeLeanCalibration(model) : nil;
	VehicleCategoryCalibration *category = GetCategoryCalibration(vehicle);
	VehicleViewCalibration *view = GetViewCalibration(model);
	const int categoryIndex = GetVehicleCategory(vehicle);
	const int step = direction;
	const char *key = nil;
	int *value = nil;
	int minimum = -300;
	int maximum = 300;
	switch(item){
	case QUEST_VEHICLE_CAL_OFFSET_X:
		if(calibration){ key = "OffsetX"; value = &calibration->offsetX; } break;
	case QUEST_VEHICLE_CAL_OFFSET_Y:
		if(calibration){ key = "OffsetY"; value = &calibration->offsetY; } break;
	case QUEST_VEHICLE_CAL_OFFSET_Z:
		if(calibration){ key = "OffsetZ"; value = &calibration->offsetZ; } break;
	case QUEST_VEHICLE_CAL_ROT_X:
		if(calibration){ key = "RotationX"; value = &calibration->rotationX; }
		minimum = -720; maximum = 720; break;
	case QUEST_VEHICLE_CAL_ROT_Y:
		if(calibration){ key = "RotationY"; value = &calibration->rotationY; }
		minimum = -720; maximum = 720; break;
	case QUEST_VEHICLE_CAL_ROT_Z:
		if(calibration){ key = "RotationZ"; value = &calibration->rotationZ; }
		minimum = -720; maximum = 720; break;
	case QUEST_VEHICLE_CAL_GLOBAL_CENTER_X:
		key="WheelCenterXCm"; value=category ? &category->wheelCenterXCm:nil; minimum=-100; maximum=100; break;
	case QUEST_VEHICLE_CAL_GLOBAL_CENTER_Y:
		key="WheelCenterYCm"; value=category ? &category->wheelCenterYCm:nil; minimum=-100; maximum=100; break;
	case QUEST_VEHICLE_CAL_GLOBAL_CENTER_Z:
		key="WheelCenterZCm"; value=category ? &category->wheelCenterZCm:nil; minimum=-100; maximum=100; break;
	case QUEST_VEHICLE_CAL_GLOBAL_RADIUS:
		key=bike?"WheelRadiusCm":"WheelRadiusV2Cm";
		value=category ? (bike?&category->wheelRadiusCm:&category->carWheelRadiusCm):nil;
		minimum=bike?-20:VR_CAR_WHEEL_MIN_RADIUS_CM; maximum=40; break;
	case QUEST_VEHICLE_CAL_MODEL_CENTER_X:
		key="WheelCenterXCm"; value=view?&view->wheelCenterXCm:nil; minimum=-100; maximum=100; break;
	case QUEST_VEHICLE_CAL_MODEL_CENTER_Y:
		key="WheelCenterYCm"; value=view?&view->wheelCenterYCm:nil; minimum=-100; maximum=100; break;
	case QUEST_VEHICLE_CAL_MODEL_CENTER_Z:
		key="WheelCenterZCm"; value=view?&view->wheelCenterZCm:nil; minimum=-100; maximum=100; break;
	case QUEST_VEHICLE_CAL_MODEL_RADIUS:
		key=bike?"WheelRadiusCm":"WheelRadiusV2Cm";
		value=view?(bike?&view->wheelRadiusCm:&view->carWheelRadiusCm):nil;
		minimum=bike?-20:0; maximum=40; break;
	case QUEST_VEHICLE_CAL_GLOBAL_PITCH:
		key="WheelPitchHalfDeg"; value=category?&category->carWheelPitchHalfDeg:nil; minimum=-180; maximum=180; break;
	case QUEST_VEHICLE_CAL_GLOBAL_YAW:
		key="WheelYawHalfDeg"; value=category?&category->carWheelYawHalfDeg:nil; minimum=-180; maximum=180; break;
	case QUEST_VEHICLE_CAL_GLOBAL_ROLL:
		key="WheelRollHalfDeg"; value=category?&category->carWheelRollHalfDeg:nil; minimum=-180; maximum=180; break;
	case QUEST_VEHICLE_CAL_MODEL_PITCH:
		key="WheelPitchHalfDeg"; value=view?&view->carWheelPitchHalfDeg:nil; minimum=-180; maximum=180; break;
	case QUEST_VEHICLE_CAL_MODEL_YAW:
		key="WheelYawHalfDeg"; value=view?&view->carWheelYawHalfDeg:nil; minimum=-180; maximum=180; break;
	case QUEST_VEHICLE_CAL_MODEL_ROLL:
		key="WheelRollHalfDeg"; value=view?&view->carWheelRollHalfDeg:nil; minimum=-180; maximum=180; break;
	case QUEST_VEHICLE_CAL_WHEELIE_HEIGHT:
		if(lean){
			lean->wheelieHeightCm = clamp(
				lean->wheelieHeightCm+step, 5, 100);
			SaveBikeLeanValue(model, "WheelieHeightCm",
				lean->wheelieHeightCm);
		}
		return;
	case QUEST_VEHICLE_CAL_STAND_HEIGHT:
		if(lean){
			lean->standHeightCm = clamp(
				lean->standHeightCm+step, 5, 100);
			SaveBikeLeanValue(model, "StandHeightCm",
				lean->standHeightCm);
		}
		return;
	default:
		return;
	}
	if(!value || !key)
		return;
	*value = clamp(*value+step, minimum, maximum);
	if(item >= QUEST_VEHICLE_CAL_OFFSET_X &&
	   item <= QUEST_VEHICLE_CAL_ROT_Z)
		SaveCalibrationValue("BikeHandle", model, hand, key, *value);
	else if(item == QUEST_VEHICLE_CAL_GLOBAL_CENTER_X ||
	        item == QUEST_VEHICLE_CAL_GLOBAL_CENTER_Y ||
	        item == QUEST_VEHICLE_CAL_GLOBAL_CENTER_Z ||
	        item == QUEST_VEHICLE_CAL_GLOBAL_RADIUS ||
	        item == QUEST_VEHICLE_CAL_GLOBAL_PITCH ||
	        item == QUEST_VEHICLE_CAL_GLOBAL_YAW ||
	        item == QUEST_VEHICLE_CAL_GLOBAL_ROLL)
		SaveCategoryCalibrationValue(categoryIndex, key, *value);
	else
		SaveViewCalibrationValue(model, key, *value);
	if(vehicle->IsCar()){
		// The isolated DFF wheel caches the last steering angle. Rebuild it from
		// its authored neutral frame after any live centre/radius/axis edit so the
		// new calibration is visible even while the steering angle is unchanged.
		((CAutomobile*)vehicle)->m_fVrSteeringWheelAppliedAngle = 1000.0f;
	}
}

void
SetQuestVehicleCalibrationPreview(bool visible)
{
	gCalibrationPreview = visible;
}

void
ApplyQuestVehicleViewOffset(CMatrix *eyeCamera)
{
	if(!eyeCamera)
		return;
	LoadDrivingSettings();
	CVehicle *vehicle = FindPlayerVehicle();
	if(!vehicle)
		return;
	const int categoryIndex = GetVehicleCategory(vehicle);
	const int defaultViewType = GetDefaultVehicleViewTypeForCurrentVehicle();
	if(defaultViewType >= 0){
		const int type = defaultViewType;
		const DefaultVehicleViewOffset &offset =
			gDefaultVehicleViewOffset[type];
		VehicleViewCalibration *model =
			GetViewCalibration(vehicle->GetModelIndex());
		eyeCamera->GetPosition().z +=
			(float)(offset.seatHeightCm+
			 (model ? model->defaultSeatHeightCm : 0))/100.0f;
		eyeCamera->GetPosition() += vehicle->GetForward()*
			((float)(offset.seatDistanceCm+
			 (model ? model->defaultSeatDistanceCm : 0))/100.0f);
		return;
	}
	VehicleCategoryCalibration *category = GetCategoryCalibration(vehicle);
	VehicleViewCalibration *calibration =
		GetViewCalibration(vehicle->GetModelIndex());
	if(category && calibration){
		eyeCamera->GetPosition().z +=
			(float)(category->seatHeightCm+calibration->seatHeightCm)/100.0f;
		eyeCamera->GetPosition() += vehicle->GetForward()*
			((float)(category->seatDistanceCm+
			 calibration->seatDistanceCm)/100.0f);
	}
}

void
ApplyQuestBikeHorizonLock(CMatrix *baseCamera)
{
	if(!baseCamera)
		return;
	LoadDrivingSettings();
	CVehicle *vehicle = FindPlayerVehicle();
	if(!gBikeHorizonLocked || !vehicle || !vehicle->IsBike() ||
	   gGameState != GS_PLAYING_GAME ||
	   CCutsceneMgr::IsRunning() ||
	   CCutsceneMgr::IsCutsceneProcessing() ||
	   TheCamera.m_WideScreenOn)
		return;
	CVector forward = baseCamera->GetForward();
	if(forward.MagnitudeSqr() <= 0.0001f)
		return;
	forward.Normalise();
	const CVector worldUp(0.0f, 0.0f, 1.0f);
	CVector left = CrossProduct(worldUp, forward);
	if(left.MagnitudeSqr() <= 0.0001f)
		return;
	left.Normalise();
	CVector up = CrossProduct(forward, left);
	up.Normalise();
	baseCamera->GetRight() = left;
	baseCamera->GetUp() = up;
	baseCamera->GetForward() = forward;
}

} // namespace OculusVR

#endif
