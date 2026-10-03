#include "common.h"

#if defined(GTA_VR_WEAPONS) && defined(__ANDROID__)

#include "OculusVR.h"

#include "android.h"
#include "Camera.h"
#include "CutsceneMgr.h"
#include "Frontend.h"
#include "Pad.h"
#include "PlayerInfo.h"
#include "PlayerPed.h"
#include "platform_android.h"
#include "ProjectileInfo.h"
#include "Timer.h"
#include "Vehicle.h"
#include "VRHandModel.h"
#include "WeaponInfo.h"
#include "WeaponType.h"
#include "World.h"
#include "ModelSets.h"
#include "vulkan/rwvk.h"

#include <android/log.h>
#include <math.h>
#include <string.h>

extern bool gVrFirstPersonActive;
#if MIAMIVR_STEAM_FRAME
// Third person over the shoulder, on foot: the stock weapon handling, the
// crosshair and no tracked hands (set in android.cpp).
extern bool gVrThirdPersonOnFoot;
#endif
extern int GetVrWeaponTypeForSlot(int slot);
extern const char *GetVrWeaponName(int weaponType);

namespace OculusVR {
namespace {

enum {
	VR_HAND_COUNT = 2,
	WEAPON_VALUE_SCALE = 2,
	SUPPORT_GRIP_LEGACY_VERSION = 3,
	SUPPORT_GRIP_MODEL_BOUND_V4 = 4,
	SUPPORT_GRIP_POSE_VERSION = 5,
	WEAPON_AIM_ALIGNMENT_SCALE = 1000000
};

enum SupportGripType {
	SUPPORT_GRIP_MAGAZINE = 0,
	SUPPORT_GRIP_FROM_BELOW,
	SUPPORT_GRIP_TYPE_COUNT
};

enum { SUPPORT_GRIP_FROM_BELOW_ROLL_HALF_DEG = -292 };

enum {
	HOLSTER_WAIST_LEFT = 0,
	HOLSTER_WAIST_RIGHT,
	HOLSTER_CHEST_LEFT,
	HOLSTER_CHEST_RIGHT,
	HOLSTER_CHEST_CENTRE,
	HOLSTER_BACK_LEFT,
	HOLSTER_BACK_RIGHT,
	HOLSTER_POINT_COUNT
};

struct WeaponCalibration
{
	int offsetX, offsetY, offsetZ;
	int aimOffsetX, aimOffsetY, aimOffsetZ;
	int aimRotationX, aimRotationY, aimRotationZ;
	int rotationX, rotationY, rotationZ;
	int supportX, supportY, supportZ;
	int supportRotationX, supportRotationY, supportRotationZ;
	int supportGripType, supportPoseVersion;
	bool valid;

	WeaponCalibration() :
		offsetX(0), offsetY(2), offsetZ(-10),
		aimOffsetX(0), aimOffsetY(8), aimOffsetZ(0),
		aimRotationX(0), aimRotationY(0), aimRotationZ(0),
		rotationX(0), rotationY(18), rotationZ(14),
		supportX(0), supportY(60), supportZ(-10),
		supportRotationX(0), supportRotationY(360), supportRotationZ(0),
		supportGripType(SUPPORT_GRIP_MAGAZINE),
		supportPoseVersion(SUPPORT_GRIP_POSE_VERSION), valid(false)
	{}
};

struct BuiltInHandCalibration
{
	int main[12];
	int support[3];
};

struct BuiltInWeaponDefaults
{
	int weaponType;
	BuiltInHandCalibration hand[VR_HAND_COUNT];
};

struct BuiltInWeaponSetDefaults
{
	int weaponType;
	ModelSets::eModelSet modelSet;
	BuiltInHandCalibration hand[VR_HAND_COUNT];
};

struct BuiltInSupportGripDefaults
{
	int weaponType;
	ModelSets::eModelSet modelSet;
	int offset[3];
	int rotation[3];
	int gripType;
};

// Stable v0.3.1 release baselines captured from the desktop project's
// calibrated vr_settings.ini. Values are absolute half-centimetres /
// half-degrees; a configured user value replaces its matching default and is
// never added to it. Hand order is LEFT, RIGHT.
static const BuiltInWeaponDefaults gBuiltInWeaponDefaults[] = {
	{ WEAPONTYPE_UNARMED, {
		{ { -12,0,-8, 0,8,0, 0,0,0, 0,8,12 }, { 0,60,-10 } },
		{ { -12,0,-8, 0,8,0, 0,0,0, 0,8,12 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_BRASSKNUCKLE, {
		{ { 0,0,-12, 0,0,0, 0,0,0, -2,0,34 }, { 0,60,-10 } },
		{ { 0,0,-12, 0,0,0, 0,0,0, -2,0,34 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_KNIFE, {
		{ { 0,-12,-12, 0,8,0, 0,0,0, 0,6,12 }, { 0,60,-10 } },
		{ { 3,-9,-12, 0,8,0, 0,0,0, 0,6,12 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_BASEBALLBAT, {
		{ { 0,-5,-10, 0,0,0, 0,0,0, 0,0,0 }, { 0,60,-10 } },
		{ { 0,-5,-11, 2,0,0, 0,0,0, 0,0,0 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_HAMMER, {
		{ { 0,-12,-12, 0,8,0, 0,0,0, 0,6,12 }, { 0,60,-10 } },
		{ { 0,-12,-12, 0,8,0, 0,0,0, 0,6,12 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_KATANA, {
		{ { 24,-4,-10, 0,0,0, 0,0,0, 0,0,0 }, { 0,60,-10 } },
		{ { 24,-4,-10, 0,0,0, 0,0,0, 0,0,0 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_CHAINSAW, {
		{ { 8,0,-13, 0,0,0, 0,0,0, 0,44,-9 }, { 0,60,-10 } },
		{ { 8,0,-13, 0,0,0, 0,0,0, 0,44,-9 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_GRENADE, {
		{ { 0,0,-16, 0,0,0, 0,0,0, 0,0,0 }, { 0,60,-10 } },
		{ { 0,0,-16, 0,0,0, 0,0,0, 0,0,0 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_DETONATOR_GRENADE, {
		{ { 5,0,-16, 0,1,0, 0,0,0, 0,0,0 }, { 0,60,-10 } },
		{ { 5,0,-16, 0,1,0, 0,0,0, 0,0,0 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_MOLOTOV, {
		{ { 24,0,-16, 0,0,0, 0,0,0, 0,0,0 }, { 0,60,-10 } },
		{ { 24,0,-16, 0,0,0, 0,0,0, 0,0,0 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_COLT45, {
		{ { 0,-2,-6, 1,9,-8, 0,0,0, -4,16,-8 }, { 0,60,-10 } },
		{ { 2,-3,-7, -2,10,-8, 0,0,0, -4,16,-8 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_PYTHON, {
		{ { 0,0,-11, 4,15,20, 7,-5,2, 0,0,0 }, { 0,60,-10 } },
		{ { 0,0,-11, -5,15,20, 7,4,2, 0,0,0 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_SHOTGUN, {
		{ { -2,2,-10, 0,7,0, 0,0,0, 0,18,14 }, { 0,60,-10 } },
		{ { -2,2,-10, 0,7,0, 0,0,0, 0,18,14 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_SPAS12_SHOTGUN, {
		{ { 0,0,-11, 3,18,20, 17,-4,1, 0,0,8 }, { 11,60,-13 } },
		{ { 0,0,-11, -3,18,20, 17,4,1, 0,0,8 }, { 9,60,-12 } } } },
	{ WEAPONTYPE_STUBBY_SHOTGUN, {
		{ { 3,-2,-17, 8,14,0, -13,-3,0, 2,33,11 }, { 0,60,-16 } },
		{ { 3,-2,-17, -10,14,0, -14,1,0, 2,33,11 }, { 0,60,-17 } } } },
	{ WEAPONTYPE_TEC9, {
		{ { 1,-4,-9, 0,12,0, 0,0,0, -2,8,6 }, { 0,60,-10 } },
		{ { 1,-4,-9, 0,12,0, 0,0,0, -2,8,6 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_SILENCED_INGRAM, {
		{ { 0,0,-16, 9,13,0, 5,-2,0, 0,0,0 }, { 0,60,-10 } },
		{ { 0,0,-16, -8,14,0, 5,2,0, 0,0,0 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_MP5, {
		{ { 0,0,-11, 4,23,20, 13,-7,1, 0,0,8 }, { 0,45,-19 } },
		{ { 0,0,-11, -5,23,20, 13,7,1, 0,0,8 }, { 0,39,-13 } } } },
	{ WEAPONTYPE_M4, {
		{ { 5,0,-12, 6,25,65, 0,0,0, 0,0,11 }, { -1,41,-20 } },
		{ { 5,0,-12, -6,25,65, 0,0,0, 0,0,11 }, { -1,41,-20 } } } },
	{ WEAPONTYPE_RUGER, {
		{ { 1,0,-11, 0,20,0, 0,0,0, 0,8,11 }, { 0,51,-7 } },
		{ { -1,0,-11, 0,20,0, 0,0,0, 0,8,11 }, { 0,50,-6 } } } },
	{ WEAPONTYPE_SNIPERRIFLE, {
		{ { -3,0,-6, 0,10,100, 0,0,0, 0,10,11 }, { 0,60,-10 } },
		{ { -3,0,-6, 0,10,100, 0,0,0, 0,10,11 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_LASERSCOPE, {
		{ { 5,0,-11, -6,25,100, 0,0,0, 0,0,13 }, { 0,60,-10 } },
		{ { 5,0,-11, -6,25,100, 0,0,0, 0,0,13 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_ROCKETLAUNCHER, {
		{ { 5,0,-16, -8,21,71, 0,0,0, 0,0,0 }, { 0,60,-10 } },
		{ { 5,30,-16, -8,21,71, 0,0,0, 0,0,0 }, { 0,45,-10 } } } },
	{ WEAPONTYPE_FLAMETHROWER, {
		{ { 21,1,-10, 0,-7,100, 0,0,0, -3,56,8 }, { 0,60,-10 } },
		{ { 21,1,-10, 0,-7,100, 0,0,0, -3,56,8 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_M60, {
		{ { 0,0,-7, -9,18,0, 7,-5,75, -10,7,12 }, { 5,60,0 } },
		{ { 0,0,-7, -9,18,0, 7,-5,75, -10,7,12 }, { 5,60,0 } } } },
	{ WEAPONTYPE_MINIGUN, {
		{ { 25,6,-18, -6,-19,47, -10,5,0, 0,67,4 }, { 0,60,-10 } },
		{ { 25,6,-18, -6,-19,47, -10,5,0, 0,67,4 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_DETONATOR, {
		{ { 5,0,-16, 0,1,0, 0,0,0, 0,0,0 }, { 0,60,-10 } },
		{ { 5,0,-16, 0,1,0, 0,0,0, 0,0,0 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_CAMERA, {
		{ { -3,0,-39, 0,8,0, 0,0,0, 0,37,12 }, { 0,60,-10 } },
		{ { -3,0,-39, 0,8,0, 0,0,0, 0,37,12 }, { 0,60,-10 } } } },
};

// Desktop v0.5.0 calibration corrections which are independent of the
// graphics backend. Keeping them as an override table makes the Quest update
// explicit while preserving its original fallback rows for untouched weapons.
static const BuiltInWeaponDefaults gBuiltInWeaponV050Overrides[] = {
	{ WEAPONTYPE_SCREWDRIVER, {
		{ { 0,-8,-10, 0,8,0, 0,0,0, 0,18,14 }, { 0,60,-10 } },
		{ { 0,-8,-10, 0,8,0, 0,0,0, 0,18,14 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_GOLFCLUB, {
		{ { 0,-3,-10, 0,8,0, 0,0,0, 0,18,14 }, { 0,60,-10 } },
		{ { 0,-3,-10, 0,8,0, 0,0,0, 0,18,14 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_NIGHTSTICK, {
		{ { 2,-6,-10, 0,8,0, 0,0,0, 0,18,14 }, { 0,60,-10 } },
		{ { 2,-6,-10, 0,8,0, 0,0,0, 0,18,14 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_KNIFE, {
		{ { -3,-9,-9, 0,8,0, 0,0,0, 0,6,12 }, { 0,60,-10 } },
		{ { -3,-9,-9, 0,8,0, 0,0,0, 0,6,12 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_HAMMER, {
		{ { -4,-10,-12, 0,8,0, 0,0,0, 0,38,12 }, { 0,60,-10 } },
		{ { -4,-10,-12, 0,8,0, 0,0,0, 0,38,12 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_CLEAVER, {
		{ { 4,-6,-12, 0,8,0, 0,0,0, 0,37,14 }, { 0,60,-10 } },
		{ { 4,-6,-12, 0,8,0, 0,0,0, 0,37,14 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_MACHETE, {
		{ { 7,-5,-8, 0,8,0, 0,0,0, 0,24,14 }, { 0,60,-10 } },
		{ { 7,-5,-8, 0,8,0, 0,0,0, 0,24,14 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_COLT45, {
		{ { 0,-3,-7, 2,10,-8, 0,0,0, -4,16,-8 }, { 0,60,-10 } },
		{ { 0,-3,-7, -2,10,-8, 0,0,0, -4,16,-8 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_PYTHON, {
		{ { 0,-6,-11, 2,15,10, 7,-4,-2, 0,0,0 }, { 0,60,-10 } },
		{ { 0,-6,-8, -2,15,10, 7,4,2, 0,0,0 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_SHOTGUN, {
		{ { 3,1,-10, 4,12,0, 0,0,0, 2,19,11 }, { 0,60,-10 } },
		{ { 3,1,-10, -4,12,0, 0,0,0, 2,19,11 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_SPAS12_SHOTGUN, {
		{ { 0,0,-11, 3,18,20, 17,-4,-1, 0,0,8 }, { 11,60,-13 } },
		{ { 0,0,-11, -3,18,20, 17,4,1, 0,0,8 }, { 9,60,-12 } } } },
	{ WEAPONTYPE_STUBBY_SHOTGUN, {
		{ { 0,-5,-10, 10,14,0, -14,-1,0, 2,33,11 }, { 0,60,-16 } },
		{ { 0,-5,-10, -10,14,0, -14,1,0, 2,33,11 }, { 0,60,-17 } } } },
	{ WEAPONTYPE_UZI, {
		{ { 0,-3,-10, 2,8,0, -10,2,0, 0,18,14 }, { 0,60,-10 } },
		{ { 0,-3,-10, -2,8,0, -10,-2,0, 0,18,14 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_SILENCED_INGRAM, {
		{ { 0,0,-16, 8,14,0, 5,-2,0, 0,0,0 }, { 0,60,-10 } },
		{ { 0,0,-16, -8,14,0, 5,2,0, 0,0,0 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_MP5, {
		{ { -4,-7,-8, 5,23,20, 13,-7,-1, 0,0,8 }, { 0,45,-19 } },
		{ { -4,-7,-8, -5,23,20, 13,7,1, 0,0,8 }, { 0,39,-13 } } } },
	{ WEAPONTYPE_RUGER, {
		{ { -1,0,-11, 0,20,0, 0,0,0, 0,8,11 }, { 0,51,-7 } },
		{ { -1,0,-11, 0,20,0, 0,0,0, 0,8,11 }, { 0,50,-6 } } } },
	{ WEAPONTYPE_SNIPERRIFLE, {
		{ { -2,-5,-9, 0,10,100, 0,0,0, 0,10,0 }, { 0,60,-10 } },
		{ { -2,-5,-9, 0,10,100, 0,0,0, 0,10,0 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_LASERSCOPE, {
		{ { 5,0,-11, 6,25,100, 0,0,0, 0,0,13 }, { 0,60,-10 } },
		{ { 5,0,-11, -6,25,100, 0,0,0, 0,0,13 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_ROCKETLAUNCHER, {
		{ { -2,26,-11, 8,21,71, 0,0,0, 0,0,0 }, { 0,60,-10 } },
		{ { -2,26,-11, -8,21,71, 0,0,0, 0,0,0 }, { 0,45,-10 } } } },
	{ WEAPONTYPE_M60, {
		{ { 0,0,-12, 9,18,0, 7,5,-75, -10,8,12 }, { 5,60,0 } },
		{ { 0,0,-12, -9,18,0, 7,-5,75, -10,8,12 }, { 5,60,0 } } } },
	{ WEAPONTYPE_MINIGUN, {
		{ { 25,6,-18, 6,-19,47, -10,-5,0, 0,67,4 }, { 0,60,-10 } },
		{ { 25,6,-18, -6,-19,47, -10,5,0, 0,67,4 }, { 0,60,-10 } } } },
};

// Model-specific v0.5.0 baselines. Missing rows fall back to the corrected
// Classic table above; user INI data still has higher priority.
static const BuiltInWeaponSetDefaults gBuiltInWeaponSetDefaults[] = {
	{ WEAPONTYPE_SCREWDRIVER, ModelSets::MODEL_SET_MODERN, {
		{ { 0,-8,-10, 0,8,0, 0,0,0, 0,18,14 }, { 0,60,-10 } },
		{ { 0,-8,-10, 0,8,0, 0,0,0, 0,18,14 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_GOLFCLUB, ModelSets::MODEL_SET_MODERN, {
		{ { 0,-3,-10, 0,8,0, 0,0,0, 0,18,14 }, { 0,60,-10 } },
		{ { 0,-3,-10, 0,8,0, 0,0,0, 0,18,14 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_NIGHTSTICK, ModelSets::MODEL_SET_MODERN, {
		{ { 2,-6,-10, 0,8,0, 0,0,0, 0,18,14 }, { 0,60,-10 } },
		{ { 2,-6,-10, 0,8,0, 0,0,0, 0,18,14 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_KNIFE, ModelSets::MODEL_SET_MODERN, {
		{ { -3,-9,-9, 0,8,0, 0,0,0, 0,6,12 }, { 0,60,-10 } },
		{ { -3,-9,-9, 0,8,0, 0,0,0, 0,6,12 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_HAMMER, ModelSets::MODEL_SET_MODERN, {
		{ { -4,-10,-12, 0,8,0, 0,0,0, 0,38,12 }, { 0,60,-10 } },
		{ { -4,-10,-12, 0,8,0, 0,0,0, 0,38,12 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_CLEAVER, ModelSets::MODEL_SET_MODERN, {
		{ { 4,-6,-12, 0,8,0, 0,0,0, 0,37,14 }, { 0,60,-10 } },
		{ { 4,-6,-12, 0,8,0, 0,0,0, 0,37,14 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_MACHETE, ModelSets::MODEL_SET_MODERN, {
		{ { 7,-5,-8, 0,8,0, 0,0,0, 0,24,14 }, { 0,60,-10 } },
		{ { 7,-5,-8, 0,8,0, 0,0,0, 0,24,14 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_COLT45, ModelSets::MODEL_SET_MODERN, {
		{ { 0,-3,-7, 2,10,-8, 0,0,0, -4,16,-8 }, { 0,60,-10 } },
		{ { 0,-3,-7, -2,10,-8, 0,0,0, -4,16,-8 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_PYTHON, ModelSets::MODEL_SET_MODERN, {
		{ { 0,-6,-11, 3,13,8, 7,-4,-2, 0,0,0 }, { 0,60,-10 } },
		{ { 0,-6,-11, -3,13,8, 7,4,2, 0,0,0 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_SHOTGUN, ModelSets::MODEL_SET_MODERN, {
		{ { 3,1,-10, 0,7,0, 0,0,0, 2,19,11 }, { 0,60,-10 } },
		{ { 3,1,-10, 0,7,0, 0,0,0, 2,19,11 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_STUBBY_SHOTGUN, ModelSets::MODEL_SET_MODERN, {
		{ { 0,-5,-10, 4,9,0, -15,-6,0, 2,33,11 }, { 0,60,-16 } },
		{ { 0,-5,-10, -4,9,0, -15,6,0, 2,33,11 }, { 0,60,-17 } } } },
	{ WEAPONTYPE_TEC9, ModelSets::MODEL_SET_MODERN, {
		{ { 1,-4,-9, 0,16,0, 7,0,0, -2,8,6 }, { 0,60,-10 } },
		{ { 1,-4,-9, 0,16,0, 7,0,0, -2,8,6 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_UZI, ModelSets::MODEL_SET_MODERN, {
		{ { 0,-3,-10, 2,9,0, -10,-1,0, 0,18,14 }, { 0,60,-10 } },
		{ { 0,-3,-10, -2,9,0, -10,1,0, 0,18,14 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_SILENCED_INGRAM, ModelSets::MODEL_SET_MODERN, {
		{ { 0,0,-16, 1,10,-10, 5,5,0, 0,0,0 }, { 0,60,-10 } },
		{ { -3,-4,-11, -1,10,-10, 5,-5,0, 0,0,0 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_MP5, ModelSets::MODEL_SET_MODERN, {
		{ { -4,-7,-8, 3,14,20, 13,-7,-1, 0,0,8 }, { 0,45,-19 } },
		{ { -4,-7,-8, -3,14,20, 13,7,1, 0,0,8 }, { 0,39,-13 } } } },
	{ WEAPONTYPE_M4, ModelSets::MODEL_SET_MODERN, {
		{ { 5,0,-12, 2,16,48, 2,0,0, 0,0,11 }, { -1,41,-20 } },
		{ { 1,-5,-8, -2,16,48, 2,0,0, 0,0,11 }, { -1,41,-20 } } } },
	{ WEAPONTYPE_RUGER, ModelSets::MODEL_SET_MODERN, {
		{ { -1,0,-11, 2,8,87, 1,-3,0, 0,8,11 }, { 0,51,-7 } },
		{ { 1,-6,-12, -2,8,87, 1,3,0, 0,8,11 }, { 0,50,-6 } } } },
	{ WEAPONTYPE_LASERSCOPE, ModelSets::MODEL_SET_MODERN, {
		{ { 5,0,-11, 6,25,100, 0,0,0, 0,0,13 }, { 0,60,-10 } },
		{ { -1,-8,-11, -6,25,100, 0,0,0, 0,0,13 }, { 0,60,-10 } } } },
	{ WEAPONTYPE_ROCKETLAUNCHER, ModelSets::MODEL_SET_MODERN, {
		{ { -2,26,-11, 4,15,71, 0,0,0, 0,0,0 }, { 0,60,-10 } },
		{ { -2,26,-11, -4,15,71, 0,0,0, 0,0,0 }, { 0,45,-10 } } } },
	{ WEAPONTYPE_M60, ModelSets::MODEL_SET_MODERN, {
		{ { 0,0,-12, 11,11,41, 7,5,-75, -10,8,12 }, { 5,60,0 } },
		{ { 0,0,-12, -11,11,41, 7,-5,75, -10,8,12 }, { 5,60,0 } } } },
};

// PC v0.5.0 support-hand profiles. The socket is RIGHT-canonical and bound to
// the visible weapon model: X=lateral, Y=barrel, Z=top. LEFT-primary gameplay
// derives the mirrored pose at runtime, so both hands and both model sets share
// one authoritative calibration.
static const BuiltInSupportGripDefaults gBuiltInSupportGripDefaults[] = {
	{ WEAPONTYPE_SHOTGUN, ModelSets::MODEL_SET_CLASSIC,
		{ -13,80,-6 }, { -32,320,-360 }, SUPPORT_GRIP_FROM_BELOW },
	{ WEAPONTYPE_SPAS12_SHOTGUN, ModelSets::MODEL_SET_CLASSIC,
		{ -13,79,-8 }, { 0,297,-354 }, SUPPORT_GRIP_FROM_BELOW },
	{ WEAPONTYPE_STUBBY_SHOTGUN, ModelSets::MODEL_SET_CLASSIC,
		{ -12,76,-9 }, { -13,339,353 }, SUPPORT_GRIP_FROM_BELOW },
	{ WEAPONTYPE_MP5, ModelSets::MODEL_SET_CLASSIC,
		{ -7,39,-10 }, { -64,360,32 }, SUPPORT_GRIP_MAGAZINE },
	{ WEAPONTYPE_M4, ModelSets::MODEL_SET_CLASSIC,
		{ -15,75,-6 }, { -66,359,-292 }, SUPPORT_GRIP_FROM_BELOW },
	{ WEAPONTYPE_RUGER, ModelSets::MODEL_SET_CLASSIC,
		{ -14,92,-8 }, { -54,360,-295 }, SUPPORT_GRIP_FROM_BELOW },
	{ WEAPONTYPE_SNIPERRIFLE, ModelSets::MODEL_SET_CLASSIC,
		{ -15,71,-4 }, { -47,300,-332 }, SUPPORT_GRIP_FROM_BELOW },
	{ WEAPONTYPE_LASERSCOPE, ModelSets::MODEL_SET_CLASSIC,
		{ -5,47,-8 }, { -70,360,0 }, SUPPORT_GRIP_MAGAZINE },
	{ WEAPONTYPE_ROCKETLAUNCHER, ModelSets::MODEL_SET_CLASSIC,
		{ -7,12,-1 }, { -75,-360,0 }, SUPPORT_GRIP_MAGAZINE },
	{ WEAPONTYPE_FLAMETHROWER, ModelSets::MODEL_SET_CLASSIC,
		{ -30,48,0 }, { 0,234,0 }, SUPPORT_GRIP_FROM_BELOW },
	{ WEAPONTYPE_M60, ModelSets::MODEL_SET_CLASSIC,
		{ -12,62,-2 }, { 0,360,347 }, SUPPORT_GRIP_FROM_BELOW },
	{ WEAPONTYPE_MINIGUN, ModelSets::MODEL_SET_CLASSIC,
		{ -37,53,3 }, { -22,268,22 }, SUPPORT_GRIP_FROM_BELOW },
	{ WEAPONTYPE_SPAS12_SHOTGUN, ModelSets::MODEL_SET_MODERN,
		{ -18,79,-8 }, { 0,297,-354 }, SUPPORT_GRIP_FROM_BELOW },
	// Tuned in the headset against the Modern rifle, whose handguard sits
	// further forward and lower than the classic model.
	{ WEAPONTYPE_RUGER, ModelSets::MODEL_SET_MODERN,
		{ -11,98,-4 }, { -29,359,-360 }, SUPPORT_GRIP_FROM_BELOW },
	{ WEAPONTYPE_M4, ModelSets::MODEL_SET_MODERN,
		{ -12,70,-6 }, { -74,360,-332 }, SUPPORT_GRIP_FROM_BELOW },
};

struct DroppedWeapon
{
	int slot;
	CMatrix matrix;
	CVector velocity;
	CVector angularAxis;
	float angularSpeed;
	uint32 startTime;

	DroppedWeapon() : slot(-1), velocity(0.0f, 0.0f, 0.0f),
		angularAxis(0.0f, 0.0f, 1.0f), angularSpeed(0.0f),
		startTime(0)
	{}
};

struct MeleeStrike
{
	bool pending;
	int slot, weaponType;
	CVector tipStart, tipEnd;
	CVector rootStart, rootEnd;
	float speed;
	uint32 frame;

	MeleeStrike() : pending(false), slot(-1), weaponType(-1),
		speed(0.0f), frame(0)
	{}
};

struct MeleeMotion
{
	bool valid, armed;
	int slot, weaponType;
	CVector previousTip, previousRoot;
	CVector previousTrackingTip, previousTrackingRoot;
	uint32 previousFrame, lastStrikeTime, calmSince;
	bool strikeInProgress;
	float strikePeakSpeed;
	uint32 strikeContinueUntil;

	MeleeMotion() : valid(false), armed(false), slot(-1), weaponType(-1),
		previousFrame(0), lastStrikeTime(0), calmSince(0),
		strikeInProgress(false), strikePeakSpeed(0.0f),
		strikeContinueUntil(0)
	{}
};

struct ManualReloadState
{
	bool active;
	bool requested;
	bool movedAwayFromSpot;
	int weaponHand;
	int magazineHand;
	int slot;
	int weaponType;
	uint32 grabbedAt;

	ManualReloadState() : active(false), requested(false),
		movedAwayFromSpot(false), weaponHand(-1), magazineHand(-1),
		slot(-1), weaponType(-1), grabbedAt(0)
	{}
};

static bool gPoseValid[VR_HAND_COUNT];
static CMatrix gGripMatrix[VR_HAND_COUNT];
static CMatrix gAimMatrix[VR_HAND_COUNT];
static CVector gHandVelocity[VR_HAND_COUNT];
static CVector gPreviousHandPosition[VR_HAND_COUNT];
static uint32 gPreviousPoseTime[VR_HAND_COUNT];
static float gGrip[VR_HAND_COUNT];
static float gTrigger[VR_HAND_COUNT];
static bool gTriggerPressed[VR_HAND_COUNT];
static bool gTriggerJustPressed[VR_HAND_COUNT];
static bool gTriggerJustReleased[VR_HAND_COUNT];
static bool gGripLatched[VR_HAND_COUNT];

static uint32 gWeaponHolsterMask;
static int gHolsterPointSlot[HOLSTER_POINT_COUNT] = {
	WEAPONSLOT_SUBMACHINEGUN, WEAPONSLOT_HANDGUN, WEAPONSLOT_SHOTGUN,
	WEAPONSLOT_MELEE, WEAPONSLOT_PROJECTILE, WEAPONSLOT_HEAVY,
	WEAPONSLOT_RIFLE
};
static int gHeldSlot[VR_HAND_COUNT] = { -1, -1 };
static int gHolsterSelection[VR_HAND_COUNT] = { -1, -1 };
// Scripted mounted-gun sequences attach Tommy to another entity instead of
// putting him in a normal vehicle seat. Mirror the script-owned gun into the
// tracked right hand only for the lifetime of that attachment.
static bool gAttachedMissionWeaponForced;
static int gAttachedMissionWeaponSlot = -1;
static int gSupportHand[VR_HAND_COUNT] = { -1, -1 };
// Set while the weapon calibration page is open. The free hand takes the
// foregrip on its own there and keeps it, so the sockets being edited can
// be judged against a hand that is actually on them.
static bool gWeaponCalibrationPreview;
static CMatrix gWeaponRenderMatrix[VR_HAND_COUNT];
static int gWeaponRenderSlot[VR_HAND_COUNT] = { -1, -1 };
static CMatrix gWeaponContactMatrix[VR_HAND_COUNT];
static int gWeaponContactSlot[VR_HAND_COUNT] = { -1, -1 };
static int gWeaponContactType[VR_HAND_COUNT] = { -1, -1 };
static uint32 gWeaponContactFrame[VR_HAND_COUNT];
static DroppedWeapon gDropped[VR_HAND_COUNT];

static WeaponCalibration
	gCalibration[VR_HAND_COUNT][WEAPONTYPE_TOTALWEAPONS];
static bool gSettingsLoaded;
static bool gHandsEnabled = true;
static bool gHolsterHighlights = true;
static bool gWeaponLaser;
struct WeaponLaserOverride {
	bool loaded;
	int value;
};
static WeaponLaserOverride gWeaponLaserOverrides[2][WEAPONTYPE_TOTALWEAPONS];
static bool gManualReload;
static bool gScopeAim = true;
static bool gGripLock;
static bool gRunWithoutLimits;
static CVector gBodyForward(0.0f, 1.0f, 0.0f);
static bool gBodyForwardValid;

static bool gActiveFire;
static int gActiveFireHand = -1;
static int gActiveFireWeaponType = -1;
static CVector gActiveFireSource;
static CVector gActiveFireDirection;
static bool gThrowablePreview[VR_HAND_COUNT];
static CVector gActiveThrowableSource;
static CVector gActiveThrowableVelocity;
static bool gActiveThrowable;

static MeleeStrike gMeleeStrike[VR_HAND_COUNT];
static MeleeMotion gMeleeMotion[VR_HAND_COUNT];

static int gScopeHand = -1;
static int gScopeWeaponType = -1;
static int gScopeCandidateHand = -1;
static int gScopeCandidateType = -1;
static uint32 gScopeCandidateSince;
static uint32 gScopeReleaseSince;

static ManualReloadState gManualReloadState[VR_HAND_COUNT];
static bool gManualReloadGripDown[VR_HAND_COUNT];
static int gTrackedDetonatorHand = -1;
static bool gTrackedDetonatorWasActive[VR_HAND_COUNT];
static bool gTrackedDetonatorWaitForRelease[VR_HAND_COUNT];
static bool gTrackedDetonatorJustPressed[VR_HAND_COUNT];

static const char *kSettingsPath = ".\\vr_settings.ini";

static bool IsTrackedDetonatorHandReserved(int hand);

#define QUEST_WEAPON_LOG(...) \
	__android_log_print(ANDROID_LOG_INFO, "MiamiVR", __VA_ARGS__)

static CVector
VectorFromArray(const float value[3])
{
	return CVector(value[0], value[1], value[2]);
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

static bool
PoseToMatrix(const androidgame::PadInput::Pose &pose, CMatrix *matrix)
{
	if(!matrix || !pose.valid)
		return false;
	float right[3], up[3], forward[3], position[3];
	if(!rw::vulkan::playPoseToFirstPersonWorld(
	   pose.position, pose.orientation, right, up, forward, position))
		return false;
	matrix->SetUnity();
	matrix->GetRight() = VectorFromArray(right);
	matrix->GetUp() = VectorFromArray(up);
	matrix->GetForward() = VectorFromArray(forward);
	matrix->GetPosition() = VectorFromArray(position);
	return true;
}

static bool
IsGameplayAvailable()
{
	CPlayerPed *player = FindPlayerPed();
	return gVrFirstPersonActive && player && !player->DyingOrDead() &&
#if MIAMIVR_STEAM_FRAME
		!gVrThirdPersonOnFoot &&
#endif
		!FrontEndMenuManager.m_bMenuActive &&
		!CCutsceneMgr::IsRunning() && !CCutsceneMgr::IsCutsceneProcessing() &&
		!TheCamera.m_WideScreenOn && !CTimer::GetIsPaused();
}

static void
LoadSettings()
{
	if(gSettingsLoaded)
		return;
	InvalidateQuestWeaponLaserOverrides();
	gHandsEnabled = GetPrivateProfileIntA("VR", "VrHands", 1,
		kSettingsPath) != 0;
	gWeaponLaser = GetPrivateProfileIntA("VR", "WeaponLaser", 0,
		kSettingsPath) != 0;
	gHolsterHighlights = GetPrivateProfileIntA("VR", "HolsterHighlights", 0,
		kSettingsPath) != 0;
	gManualReload = GetPrivateProfileIntA("VR", "ManualReloading", 0,
		kSettingsPath) != 0;
	gScopeAim = GetPrivateProfileIntA("VR", "PhysicalScopeAim", 1,
		kSettingsPath) != 0;
	gGripLock = GetPrivateProfileIntA("VR", "WeaponGripLock", 0,
		kSettingsPath) != 0;
	gRunWithoutLimits = GetPrivateProfileIntA("VR", "RunWithoutLimits", 0,
		kSettingsPath) != 0;
	static const char *keys[HOLSTER_POINT_COUNT] = {
		"HolsterWaistLeftSlot", "HolsterWaistRightSlot",
		"HolsterChestLeftSlot", "HolsterChestRightSlot",
		nil, "HolsterBackLeftSlot", "HolsterBackRightSlot"
	};
	for(int point = 0; point < HOLSTER_POINT_COUNT; point++){
		if(!keys[point])
			continue;
		const int stored = (int)(int32)GetPrivateProfileIntA("VR", keys[point],
			gHolsterPointSlot[point], kSettingsPath);
		if(stored >= -1 && stored < TOTAL_WEAPON_SLOTS)
			gHolsterPointSlot[point] = stored;
	}
	// The centre chest point is intentionally dedicated to throwables. It
	// remains usable even if an older settings file assigned another slot.
	gHolsterPointSlot[HOLSTER_CHEST_CENTRE] = WEAPONSLOT_PROJECTILE;
	gSettingsLoaded = true;
}

static const char *
CalibrationKey(int hand, const char *name, char *storage)
{
	if(hand == 0){
		sprintf(storage, "Left%s", name);
		return storage;
	}
	return name;
}

static int
ReadCalibrationValue(const char *section, int hand, const char *name,
	int fallback)
{
	char key[64];
	return (int)(int32)GetPrivateProfileIntA(section,
		CalibrationKey(hand, name, key), fallback, kSettingsPath);
}

// The laser override lives beside the weapon's calibration, in the same
// section, so it travels with everything else authored for that weapon.
// -1 follows the global switch, 0 is off, 1 is on.
static void
WeaponLaserSection(int weaponType, bool modern, char *section, int size)
{
	snprintf(section, size, modern ? "VRWeaponModern_%02d_%s" :
		"VRWeapon_%02d_%s", weaponType, GetVrWeaponName(weaponType));
}

static int
ReadWeaponLaserOverride(int weaponType)
{
	if(weaponType < 0 || weaponType >= WEAPONTYPE_TOTALWEAPONS)
		return -1;
	const bool modern = ModelSets::GetActiveForCategory(
		ModelSets::MODEL_CATEGORY_WEAPONS) == ModelSets::MODEL_SET_MODERN;
	WeaponLaserOverride &cached = gWeaponLaserOverrides[modern ? 1 : 0][weaponType];
	if(cached.loaded)
		return cached.value;
	char section[96];
	WeaponLaserSection(weaponType, modern, section, sizeof(section));
	const int stored = (int)(int32)GetPrivateProfileIntA(section,
		"Laser", -1, kSettingsPath);
	cached.value = stored < 0 ? -1 : (stored != 0 ? 1 : 0);
	cached.loaded = true;
	return cached.value;
}

static void
WriteWeaponLaserOverride(int weaponType, int value)
{
	if(weaponType < 0 || weaponType >= WEAPONTYPE_TOTALWEAPONS)
		return;
	const bool modern = ModelSets::GetActiveForCategory(
		ModelSets::MODEL_CATEGORY_WEAPONS) == ModelSets::MODEL_SET_MODERN;
	WeaponLaserOverride &cached = gWeaponLaserOverrides[modern ? 1 : 0][weaponType];
	char section[96], text[16];
	WeaponLaserSection(weaponType, modern, section, sizeof(section));
	snprintf(text, sizeof(text), "%d", value);
	cached.loaded = WritePrivateProfileStringA(section, "Laser", text, kSettingsPath) != 0;
	if(cached.loaded)
		cached.value = value < 0 ? -1 : (value != 0 ? 1 : 0);
}

static bool
CalibrationConfigured(const char *section, int hand)
{
	return ReadCalibrationValue(section, hand, "Configured", 0) != 0;
}

static bool
SupportCalibrationConfigured(const char *section, int hand)
{
	return ReadCalibrationValue(section, hand,
		"SupportGripConfigured", 0) != 0;
}

static bool
SectionHasCalibration(const char *section)
{
	return CalibrationConfigured(section, 0) ||
		CalibrationConfigured(section, 1) ||
		SupportCalibrationConfigured(section, 0) ||
		SupportCalibrationConfigured(section, 1);
}

static void
ApplyBuiltInHandCalibration(const BuiltInHandCalibration &value,
	WeaponCalibration *calibration)
{
	calibration->offsetX = value.main[0];
	calibration->offsetY = value.main[1];
	calibration->offsetZ = value.main[2];
	calibration->aimOffsetX = value.main[3];
	calibration->aimOffsetY = value.main[4];
	calibration->aimOffsetZ = value.main[5];
	calibration->aimRotationX = value.main[6];
	calibration->aimRotationY = value.main[7];
	calibration->aimRotationZ = value.main[8];
	calibration->rotationX = value.main[9];
	calibration->rotationY = value.main[10];
	calibration->rotationZ = value.main[11];
	calibration->supportX = value.support[0];
	calibration->supportY = value.support[1];
	calibration->supportZ = value.support[2];
}

static const BuiltInSupportGripDefaults *
FindBuiltInSupportGripDefaults(int weaponType, ModelSets::eModelSet modelSet)
{
	for(int index = 0; index < (int)ARRAY_SIZE(gBuiltInSupportGripDefaults);
	    index++){
		const BuiltInSupportGripDefaults &value =
			gBuiltInSupportGripDefaults[index];
		if(value.weaponType == weaponType && value.modelSet == modelSet)
			return &value;
	}
	return nil;
}

static void
ApplyMirroredSupportGrip(const WeaponCalibration &source,
	WeaponCalibration *mirrored)
{
	if(!mirrored)
		return;
	// V5 socket reflection is lateral only. Wrist Euler values are axial and
	// use the proven PC v0.5.0 local-basis conjugation (-X,+Y,-Z).
	mirrored->supportX = -source.supportX;
	mirrored->supportY = source.supportY;
	mirrored->supportZ = source.supportZ;
	mirrored->supportRotationX = -source.supportRotationX;
	mirrored->supportRotationY = source.supportRotationY;
	mirrored->supportRotationZ = -source.supportRotationZ;
	mirrored->supportGripType = source.supportGripType;
	mirrored->supportPoseVersion = source.supportPoseVersion;
}

static void
ApplyDefaultSupportGrip(int weaponType, int hand,
	ModelSets::eModelSet modelSet, WeaponCalibration *calibration)
{
	if(!calibration)
		return;
	const BuiltInSupportGripDefaults *value =
		FindBuiltInSupportGripDefaults(weaponType, modelSet);
	if(!value && modelSet == ModelSets::MODEL_SET_MODERN)
		value = FindBuiltInSupportGripDefaults(weaponType,
			ModelSets::MODEL_SET_CLASSIC);
	WeaponCalibration canonical = *calibration;
	if(value){
		canonical.supportX = value->offset[0];
		canonical.supportY = value->offset[1];
		canonical.supportZ = value->offset[2];
		canonical.supportRotationX = value->rotation[0];
		canonical.supportRotationY = value->rotation[1];
		canonical.supportRotationZ = value->rotation[2];
		canonical.supportGripType = value->gripType;
	}else{
		// Untuned weapons keep their established RIGHT support coordinates, but
		// enter the deterministic model-bound v5 contract.
		canonical.supportRotationX = 0;
		canonical.supportRotationY = 360;
		canonical.supportRotationZ = 0;
		canonical.supportGripType = SUPPORT_GRIP_MAGAZINE;
	}
	canonical.supportPoseVersion = SUPPORT_GRIP_POSE_VERSION;
	if(hand == 0)
		ApplyMirroredSupportGrip(canonical, calibration);
	else{
		calibration->supportX = canonical.supportX;
		calibration->supportY = canonical.supportY;
		calibration->supportZ = canonical.supportZ;
		calibration->supportRotationX = canonical.supportRotationX;
		calibration->supportRotationY = canonical.supportRotationY;
		calibration->supportRotationZ = canonical.supportRotationZ;
		calibration->supportGripType = canonical.supportGripType;
		calibration->supportPoseVersion = canonical.supportPoseVersion;
	}
}

static void
GetDefaultCalibrationForSet(int weaponType, int hand,
	ModelSets::eModelSet modelSet,
	WeaponCalibration *calibration)
{
	if(!calibration)
		return;
	*calibration = WeaponCalibration();
	if(hand < 0 || hand >= VR_HAND_COUNT){
		calibration->valid = true;
		return;
	}
	for(int index = 0;
	    index < (int)ARRAY_SIZE(gBuiltInWeaponDefaults); index++){
		const BuiltInWeaponDefaults &defaults =
			gBuiltInWeaponDefaults[index];
		if(defaults.weaponType != weaponType)
			continue;
		ApplyBuiltInHandCalibration(defaults.hand[hand], calibration);
		break;
	}
	for(int index = 0;
	    index < (int)ARRAY_SIZE(gBuiltInWeaponV050Overrides); index++){
		const BuiltInWeaponDefaults &defaults =
			gBuiltInWeaponV050Overrides[index];
		if(defaults.weaponType == weaponType){
			ApplyBuiltInHandCalibration(defaults.hand[hand], calibration);
			break;
		}
	}
	if(modelSet == ModelSets::MODEL_SET_MODERN){
		for(int index = 0;
		    index < (int)ARRAY_SIZE(gBuiltInWeaponSetDefaults); index++){
			const BuiltInWeaponSetDefaults &defaults =
				gBuiltInWeaponSetDefaults[index];
			if(defaults.weaponType == weaponType &&
			   defaults.modelSet == modelSet){
				ApplyBuiltInHandCalibration(defaults.hand[hand], calibration);
				break;
			}
		}
	}
	ApplyDefaultSupportGrip(weaponType, hand, modelSet, calibration);
	calibration->valid = true;
}

static void
GetDefaultCalibration(int weaponType, int hand,
	WeaponCalibration *calibration)
{
	GetDefaultCalibrationForSet(weaponType, hand,
		ModelSets::GetActiveForCategory(ModelSets::MODEL_CATEGORY_WEAPONS),
		calibration);
}

static WeaponCalibration *
GetCalibration(int hand, int weaponType)
{
	if(hand < 0 || hand >= VR_HAND_COUNT || weaponType < 0 ||
	   weaponType >= WEAPONTYPE_TOTALWEAPONS)
		return nil;
	WeaponCalibration &calibration = gCalibration[hand][weaponType];
	if(calibration.valid)
		return &calibration;
	char section[96];
	const bool modern = ModelSets::GetActiveForCategory(
		ModelSets::MODEL_CATEGORY_WEAPONS) == ModelSets::MODEL_SET_MODERN;
	ModelSets::eModelSet calibrationSet = modern ?
		ModelSets::MODEL_SET_MODERN : ModelSets::MODEL_SET_CLASSIC;
	sprintf(section, modern ? "VRWeaponModern_%02d_%s" :
		"VRWeapon_%02d_%s", weaponType, GetVrWeaponName(weaponType));
	if(modern && !SectionHasCalibration(section)){
		char classicSection[96];
		sprintf(classicSection, "VRWeapon_%02d_%s", weaponType,
			GetVrWeaponName(weaponType));
		if(SectionHasCalibration(classicSection)){
			strcpy(section, classicSection);
			calibrationSet = ModelSets::MODEL_SET_CLASSIC;
		}
	}
	GetDefaultCalibrationForSet(weaponType, hand, calibrationSet,
		&calibration);

	int readHand = hand;
	bool configured = CalibrationConfigured(section, hand);
	if(!configured && hand == 0 && CalibrationConfigured(section, 1)){
		readHand = 1;
		configured = true;
	}
	if(configured){
		WeaponCalibration defaults;
		GetDefaultCalibrationForSet(weaponType, readHand, calibrationSet,
			&defaults);
		const int storedScale =
			ReadCalibrationValue(section, readHand, "ValueScale", 1);
		const int conversion =
			storedScale == WEAPON_VALUE_SCALE ? 1 : WEAPON_VALUE_SCALE;
		const int fallbackDivisor = conversion;
		calibration.offsetX = clamp(ReadCalibrationValue(section, readHand,
			"OffsetX", defaults.offsetX/fallbackDivisor)*conversion,
			-100, 100);
		calibration.offsetY = clamp(ReadCalibrationValue(section, readHand,
			"OffsetY", defaults.offsetY/fallbackDivisor)*conversion,
			-100, 100);
		calibration.offsetZ = clamp(ReadCalibrationValue(section, readHand,
			"OffsetZ", defaults.offsetZ/fallbackDivisor)*conversion,
			-100, 100);
		calibration.aimOffsetX = clamp(ReadCalibrationValue(section, readHand,
			"AimOffsetX", defaults.aimOffsetX/fallbackDivisor)*conversion,
			-100, 100);
		calibration.aimOffsetY = clamp(ReadCalibrationValue(section, readHand,
			"AimOffsetY", defaults.aimOffsetY/fallbackDivisor)*conversion,
			-100, 100);
		calibration.aimOffsetZ = clamp(ReadCalibrationValue(section, readHand,
			"AimOffsetZ", defaults.aimOffsetZ/fallbackDivisor)*conversion,
			-100, 100);
		calibration.aimRotationX = clamp(ReadCalibrationValue(section,
			readHand, "AimRotationX",
			defaults.aimRotationX/fallbackDivisor)*conversion, -360, 360);
		calibration.aimRotationY = clamp(ReadCalibrationValue(section,
			readHand, "AimRotationY",
			defaults.aimRotationY/fallbackDivisor)*conversion, -360, 360);
		calibration.aimRotationZ = clamp(ReadCalibrationValue(section,
			readHand, "AimRotationZ",
			defaults.aimRotationZ/fallbackDivisor)*conversion, -360, 360);
		calibration.rotationX = clamp(ReadCalibrationValue(section, readHand,
			"RotationX", defaults.rotationX/fallbackDivisor)*conversion,
			-360, 360);
		calibration.rotationY = clamp(ReadCalibrationValue(section, readHand,
			"RotationY", defaults.rotationY/fallbackDivisor)*conversion,
			-360, 360);
		calibration.rotationZ = clamp(ReadCalibrationValue(section, readHand,
			"RotationZ", defaults.rotationZ/fallbackDivisor)*conversion,
			-360, 360);
	}

	int supportHand = hand;
	bool mirrorSupportFromRight = false;
	bool supportConfigured = SupportCalibrationConfigured(section, hand);
	if(!supportConfigured && hand == 0 &&
	   SupportCalibrationConfigured(section, 1)){
		supportHand = 1;
		supportConfigured = true;
		mirrorSupportFromRight = true;
	}
	if(supportConfigured){
		WeaponCalibration defaults;
		GetDefaultCalibrationForSet(weaponType, supportHand, calibrationSet,
			&defaults);
		const int storedScale = ReadCalibrationValue(section, supportHand,
			"SupportGripValueScale", 1);
		const int conversion =
			storedScale == WEAPON_VALUE_SCALE ? 1 : WEAPON_VALUE_SCALE;
		const int fallbackDivisor = conversion;
		calibration.supportX = clamp(ReadCalibrationValue(section, supportHand,
			"SupportGripOffsetX",
			defaults.supportX/fallbackDivisor)*conversion, -200, 200);
		calibration.supportY = clamp(ReadCalibrationValue(section, supportHand,
			"SupportGripOffsetY",
			defaults.supportY/fallbackDivisor)*conversion, -200, 200);
		calibration.supportZ = clamp(ReadCalibrationValue(section, supportHand,
			"SupportGripOffsetZ",
			defaults.supportZ/fallbackDivisor)*conversion, -200, 200);
		calibration.supportRotationX = clamp(ReadCalibrationValue(section,
			supportHand, "SupportGripRotationX",
			defaults.supportRotationX/fallbackDivisor)*conversion, -360, 360);
		calibration.supportRotationY = clamp(ReadCalibrationValue(section,
			supportHand, "SupportGripRotationY",
			defaults.supportRotationY/fallbackDivisor)*conversion, -360, 360);
		calibration.supportRotationZ = clamp(ReadCalibrationValue(section,
			supportHand, "SupportGripRotationZ",
			defaults.supportRotationZ/fallbackDivisor)*conversion, -360, 360);
		calibration.supportPoseVersion = clamp(ReadCalibrationValue(section,
			supportHand, "SupportGripPoseVersion",
			SUPPORT_GRIP_LEGACY_VERSION), 1, SUPPORT_GRIP_POSE_VERSION);
		calibration.supportGripType = clamp((int)(int32)GetPrivateProfileIntA(
			section, "SupportGripStyle", defaults.supportGripType,
			kSettingsPath), 0, SUPPORT_GRIP_TYPE_COUNT-1);
		if(calibration.supportPoseVersion == SUPPORT_GRIP_MODEL_BOUND_V4){
			const int oldX = calibration.supportX;
			const int oldY = calibration.supportY;
			const int oldZ = calibration.supportZ;
			calibration.supportX = -oldZ;
			calibration.supportY = oldX;
			calibration.supportZ = -oldY;
			calibration.supportPoseVersion = SUPPORT_GRIP_POSE_VERSION;
		}
		if(mirrorSupportFromRight){
			WeaponCalibration canonical = calibration;
			ApplyMirroredSupportGrip(canonical, &calibration);
		}
	}
	calibration.valid = true;
	return &calibration;
}

static bool
IsPhysicalGun(int weaponType)
{
	switch(weaponType){
	case WEAPONTYPE_COLT45:
	case WEAPONTYPE_PYTHON:
	case WEAPONTYPE_SHOTGUN:
	case WEAPONTYPE_SPAS12_SHOTGUN:
	case WEAPONTYPE_STUBBY_SHOTGUN:
	case WEAPONTYPE_TEC9:
	case WEAPONTYPE_UZI:
	case WEAPONTYPE_SILENCED_INGRAM:
	case WEAPONTYPE_MP5:
	case WEAPONTYPE_M4:
	case WEAPONTYPE_RUGER:
	case WEAPONTYPE_SNIPERRIFLE:
	case WEAPONTYPE_LASERSCOPE:
	case WEAPONTYPE_ROCKETLAUNCHER:
	case WEAPONTYPE_FLAMETHROWER:
	case WEAPONTYPE_M60:
	case WEAPONTYPE_MINIGUN:
	case WEAPONTYPE_CAMERA:
		return true;
	default:
		return false;
	}
}

static bool
IsPhysicalMelee(int weaponType)
{
	return weaponType >= WEAPONTYPE_UNARMED &&
		weaponType <= WEAPONTYPE_CHAINSAW;
}

static bool
IsPhysicalThrowable(int weaponType)
{
	return weaponType == WEAPONTYPE_GRENADE ||
		weaponType == WEAPONTYPE_DETONATOR_GRENADE ||
		weaponType == WEAPONTYPE_TEARGAS ||
		weaponType == WEAPONTYPE_MOLOTOV;
}

static bool
IsTwoHanded(int weaponType)
{
	switch(weaponType){
	case WEAPONTYPE_SHOTGUN:
	case WEAPONTYPE_SPAS12_SHOTGUN:
	case WEAPONTYPE_STUBBY_SHOTGUN:
	case WEAPONTYPE_MP5:
	case WEAPONTYPE_M4:
	case WEAPONTYPE_RUGER:
	case WEAPONTYPE_SNIPERRIFLE:
	case WEAPONTYPE_LASERSCOPE:
	case WEAPONTYPE_ROCKETLAUNCHER:
	case WEAPONTYPE_FLAMETHROWER:
	case WEAPONTYPE_M60:
	case WEAPONTYPE_MINIGUN:
		return true;
	default:
		return false;
	}
}

static float
GetOneHandAimSwayDegrees(int weaponType)
{
	switch(weaponType){
	case WEAPONTYPE_MP5: return 1.25f;
	case WEAPONTYPE_SHOTGUN:
	case WEAPONTYPE_SPAS12_SHOTGUN: return 1.50f;
	case WEAPONTYPE_STUBBY_SHOTGUN: return 2.00f;
	case WEAPONTYPE_M4:
	case WEAPONTYPE_RUGER: return 1.75f;
	case WEAPONTYPE_SNIPERRIFLE: return 2.25f;
	case WEAPONTYPE_LASERSCOPE: return 2.00f;
	case WEAPONTYPE_ROCKETLAUNCHER: return 3.00f;
	case WEAPONTYPE_FLAMETHROWER: return 2.25f;
	case WEAPONTYPE_M60: return 2.50f;
	case WEAPONTYPE_MINIGUN: return 3.25f;
	default: return 0.0f;
	}
}

static bool
IsScopeWeapon(int weaponType)
{
	return weaponType == WEAPONTYPE_SNIPERRIFLE ||
		weaponType == WEAPONTYPE_LASERSCOPE ||
		weaponType == WEAPONTYPE_ROCKETLAUNCHER ||
		weaponType == WEAPONTYPE_CAMERA;
}

static bool
IsSupportHand(int hand)
{
	for(int primary = 0; primary < VR_HAND_COUNT; primary++)
		if(gSupportHand[primary] == hand)
			return true;
	return false;
}

static void
ClearSupportForHand(int hand)
{
	if(hand < 0 || hand >= VR_HAND_COUNT)
		return;
	gSupportHand[hand] = -1;
	for(int primary = 0; primary < VR_HAND_COUNT; primary++)
		if(gSupportHand[primary] == hand)
			gSupportHand[primary] = -1;
}

static bool
GetBodyFrame(CVector *origin, CVector *right, CVector *up,
	CVector *forward)
{
	CPlayerPed *player = FindPlayerPed();
	if(!player || !origin || !right || !up || !forward)
		return false;
	float vrRight[3], vrUp[3], vrAt[3], vrPosition[3];
	if(rw::vulkan::getFirstPersonViewFrame(
	   vrRight, vrUp, vrAt, vrPosition)){
		*origin = VectorFromArray(vrPosition);
		// Body sockets follow the HMD's yaw continuously and stay anchored to
		// the real room-space head position.  Ignore pitch/roll so looking up
		// cannot spin the belt or move it in front of the player.
		CVector headForward = VectorFromArray(vrAt);
		headForward.z = 0.0f;
		if(headForward.MagnitudeSqr() >= 0.0001f){
			headForward.Normalise();
			gBodyForward = headForward;
			gBodyForwardValid = true;
		}
		*forward = gBodyForwardValid ? gBodyForward :
			CVector(0.0f, 1.0f, 0.0f);
		*up = CVector(0.0f, 0.0f, 1.0f);
		*right = CrossProduct(*forward, *up);
		right->Normalise();
	}else{
		*right = player->GetRight();
		*up = player->GetUp();
		*forward = player->GetForward();
		if(right->MagnitudeSqr() < 0.0001f ||
		   up->MagnitudeSqr() < 0.0001f ||
		   forward->MagnitudeSqr() < 0.0001f)
			return false;
		right->Normalise();
		up->Normalise();
		forward->Normalise();
		*origin = player->GetPosition()+*up*1.55f;
	}
	return true;
}

static int
FindHolsterPoint(int slot)
{
	for(int point = 0; point < HOLSTER_POINT_COUNT; point++)
		if(gHolsterPointSlot[point] == slot)
			return point;
	return -1;
}

static bool
BuildTrackedWeaponHandBasis(int hand, CVector *position, CVector *right,
	CVector *up, CVector *forward)
{
	if(hand < 0 || hand >= VR_HAND_COUNT || !position || !right || !up ||
	   !forward || !gPoseValid[hand])
		return false;
	*position = gGripMatrix[hand].GetPosition();
	*forward = gAimMatrix[hand].GetForward();
	forward->Normalise();
	const int modelHand = 1-hand;
	*up = gGripMatrix[hand].GetRight()*(modelHand == 0 ? 1.0f : -1.0f);
	*up -= *forward*DotProduct(*up, *forward);
	if(up->MagnitudeSqr() < 0.0001f)
		return false;
	up->Normalise();
	*right = CrossProduct(*up, *forward);
	if(right->MagnitudeSqr() < 0.0001f)
		return false;
	right->Normalise();
	if(DotProduct(*right, gGripMatrix[hand].GetForward()) < 0.0f)
		*right *= -1.0f;
	return true;
}

static bool
BuildWeaponModelMatrix(int hand, const WeaponCalibration &calibration,
	CMatrix *matrix)
{
	if(!matrix)
		return false;
	CVector position, right, up, forward;
	if(!BuildTrackedWeaponHandBasis(hand, &position, &right, &up, &forward))
		return false;
	matrix->SetUnity();
	matrix->GetRight() = right*-1.0f;
	matrix->GetForward() = forward*-1.0f;
	matrix->GetUp() = up;
	matrix->GetPosition() = position+
		right*((float)calibration.offsetX/200.0f)+
		forward*((float)calibration.offsetY/200.0f)+
		up*((float)calibration.offsetZ/200.0f);
	CMatrix base, rotationX, rotationY, rotationZ;
	base.SetRotate(DEGTORAD(90.0f), 0.0f, DEGTORAD(-90.0f));
	rotationX.SetRotateX(DEGTORAD((float)calibration.rotationX/
		WEAPON_VALUE_SCALE));
	rotationY.SetRotateY(DEGTORAD((float)calibration.rotationY/
		WEAPON_VALUE_SCALE));
	rotationZ.SetRotateZ(DEGTORAD((float)calibration.rotationZ/
		WEAPON_VALUE_SCALE));
	*matrix = *matrix*base*rotationX*rotationY*rotationZ;
	return true;
}

static bool
BuildCanonicalSupportFrame(const CVector &forwardCandidate,
	const CVector &upCandidate, CVector *right, CVector *up,
	CVector *forward)
{
	if(!right || !up || !forward ||
	   !isfinite(forwardCandidate.x) || !isfinite(forwardCandidate.y) ||
	   !isfinite(forwardCandidate.z) || !isfinite(upCandidate.x) ||
	   !isfinite(upCandidate.y) || !isfinite(upCandidate.z))
		return false;
	*forward = forwardCandidate;
	if(forward->MagnitudeSqr() < 0.0001f)
		return false;
	forward->Normalise();
	*up = upCandidate-*forward*DotProduct(upCandidate, *forward);
	if(up->MagnitudeSqr() < 0.0001f)
		return false;
	up->Normalise();
	*right = CrossProduct(*forward, *up);
	if(right->MagnitudeSqr() < 0.0001f)
		return false;
	right->Normalise();
	*up = CrossProduct(*right, *forward);
	up->Normalise();
	return true;
}

static bool
BuildHolsterMatrix(int slot, CMatrix *matrix)
{
	if(!matrix || slot <= WEAPONSLOT_UNARMED ||
	   slot >= TOTAL_WEAPON_SLOTS ||
	   (FindPlayerVehicle() &&
	    (!IsImmersiveDrivingActive() ||
	     !IsImmersiveVehicleSidearm(GetVrWeaponTypeForSlot(slot)))))
		return false;
	const int point = FindHolsterPoint(slot);
	if(point < 0)
		return false;
	static const float lateral[HOLSTER_POINT_COUNT] = {
		-0.27f, 0.27f, -0.21f, 0.21f, 0.0f, 0.24f, -0.24f
	};
	static const float vertical[HOLSTER_POINT_COUNT] = {
		-0.58f, -0.58f, -0.30f, -0.30f, -0.36f, -0.14f, -0.14f
	};
	static const float depth[HOLSTER_POINT_COUNT] = {
		0.07f, 0.07f, 0.12f, 0.12f, 0.14f, -0.23f, -0.23f
	};
	CVector origin, right, up, forward;
	if(!GetBodyFrame(&origin, &right, &up, &forward))
		return false;
	const bool behind =
		point == HOLSTER_BACK_LEFT || point == HOLSTER_BACK_RIGHT;
	matrix->SetUnity();
	matrix->GetRight() = forward*(behind ? 1.0f : -1.0f);
	matrix->GetForward() = up;
	matrix->GetUp() = right*(behind ? 1.0f : -1.0f);
	matrix->GetPosition() = origin + right*lateral[point] +
		up*vertical[point] + forward*depth[point];
	return true;
}

static bool
BuildSupportVector(int primary, int weaponType, CVector *pivot,
	CVector *expected, CVector *frameRight = nil, CVector *frameUp = nil,
	CVector *frameForward = nil)
{
	if(primary < 0 || primary >= VR_HAND_COUNT || !pivot || !expected ||
	   !gPoseValid[primary] || !IsTwoHanded(weaponType) ||
	   ((frameRight || frameUp || frameForward) &&
	    (!frameRight || !frameUp || !frameForward)))
		return false;
	WeaponCalibration *calibration =
		GetCalibration(primary, weaponType);
	if(!calibration)
		return false;
	CVector rawRight, rawUp, rawForward;
	if(!BuildTrackedWeaponHandBasis(primary, pivot, &rawRight, &rawUp,
	   &rawForward))
		return false;
	CVector socketRight = rawRight;
	CVector socketUp = rawUp;
	CVector socketForward = rawForward;
	CVector visualRight = rawRight;
	CVector visualUp = rawUp;
	CVector visualForward = rawForward;
	CVector socketOrigin = *pivot;
	if(calibration->supportPoseVersion >= SUPPORT_GRIP_POSE_VERSION){
		CMatrix weaponModel;
		if(!BuildWeaponModelMatrix(primary, *calibration, &weaponModel))
			return false;
		CVector barrel = weaponModel.GetRight();
		CVector top = weaponModel.GetForward();
		CVector lateral;
		if(!BuildCanonicalSupportFrame(barrel, top,
		   &lateral, &top, &barrel))
			return false;
		socketRight = lateral;
		socketForward = barrel;
		socketUp = top;
		visualRight = barrel;
		visualUp = lateral*-1.0f;
		visualForward = top*-1.0f;
		socketOrigin = weaponModel.GetPosition();
	}
	const CVector socket = socketOrigin+
		socketRight*((float)calibration->supportX/200.0f)+
		socketForward*((float)calibration->supportY/200.0f)+
		socketUp*((float)calibration->supportZ/200.0f);
	*expected = socket-*pivot;
	if(frameRight){
		if(calibration->supportPoseVersion < SUPPORT_GRIP_POSE_VERSION &&
		   !BuildCanonicalSupportFrame(visualForward, visualUp,
		   &visualRight, &visualUp, &visualForward))
			return false;
		*frameRight = visualRight;
		*frameUp = visualUp;
		*frameForward = visualForward;
	}
	return expected->MagnitudeSqr() > 0.0001f;
}

static bool
BuildTwoHandRotation(int primary, int weaponType, CVector *pivot,
	CVector *axis, float *angle)
{
	if(!pivot || !axis || !angle || primary < 0 ||
	   primary >= VR_HAND_COUNT)
		return false;
	const int support = gSupportHand[primary];
	if(support < 0 || support >= VR_HAND_COUNT || !gPoseValid[support])
		return false;
	CVector expected;
	if(!BuildSupportVector(primary, weaponType, pivot, &expected))
		return false;
	CVector actual = gGripMatrix[support].GetPosition()-*pivot;
	if(actual.MagnitudeSqr() < 0.0001f)
		return false;
	expected.Normalise();
	actual.Normalise();
	const float dot = clamp(DotProduct(expected, actual), -1.0f, 1.0f);
	*axis = CrossProduct(expected, actual);
	if(axis->MagnitudeSqr() < 0.000001f){
		*axis = CVector(0.0f, 0.0f, 1.0f);
		*angle = 0.0f;
		return true;
	}
	const float sine = axis->Magnitude();
	*axis *= 1.0f/sine;
	*angle = Min(Atan2(sine, dot), DEGTORAD(90.0f));
	return true;
}

static void
GiveWeaponToHand(int hand, int slot)
{
	ClearSupportForHand(hand);
	gHeldSlot[hand] = slot;
	gHolsterSelection[hand] = slot;
	gWeaponRenderSlot[hand] = -1;
	gWeaponContactSlot[hand] = -1;
	gMeleeMotion[hand] = MeleeMotion();
	QUEST_WEAPON_LOG("[weapons] hand=%d grabbed slot=%d type=%d",
		hand, slot, GetVrWeaponTypeForSlot(slot));
}

static bool
BuildDroppedMatrix(int hand, CMatrix *matrix)
{
	if(!matrix || hand < 0 || hand >= VR_HAND_COUNT ||
	   gDropped[hand].slot < 0)
		return false;
	const uint32 now = CTimer::GetTimeInMillisecondsNonClipped();
	const float elapsed =
		(float)(now-gDropped[hand].startTime)*0.001f;
	if(elapsed > 1.8f)
		return false;
	*matrix = gDropped[hand].matrix;
	if(gDropped[hand].angularSpeed > 0.001f){
		const float angle = gDropped[hand].angularSpeed*elapsed;
		matrix->GetRight() = RotateAroundAxis(matrix->GetRight(),
			gDropped[hand].angularAxis, angle);
		matrix->GetUp() = RotateAroundAxis(matrix->GetUp(),
			gDropped[hand].angularAxis, angle);
		matrix->GetForward() = RotateAroundAxis(matrix->GetForward(),
			gDropped[hand].angularAxis, angle);
	}
	matrix->GetPosition() = gDropped[hand].matrix.GetPosition() +
		gDropped[hand].velocity*elapsed -
		CVector(0.0f, 0.0f, 1.0f)*(2.1f*elapsed*elapsed);
	return true;
}

static void
StartDroppedWeapon(int hand, int slot)
{
	if(hand < 0 || hand >= VR_HAND_COUNT || slot < 0)
		return;
	DroppedWeapon dropped;
	dropped.slot = slot;
	dropped.startTime = CTimer::GetTimeInMillisecondsNonClipped();
	if(gWeaponRenderSlot[hand] == slot)
		dropped.matrix = gWeaponRenderMatrix[hand];
	else
		dropped.matrix = gGripMatrix[hand];
	dropped.velocity = gHandVelocity[hand];
	const float speed = dropped.velocity.Magnitude();
	if(speed > 6.0f)
		dropped.velocity *= 6.0f/speed;
	dropped.angularAxis =
		CrossProduct(gGripMatrix[hand].GetForward(),
			gAimMatrix[hand].GetForward());
	if(dropped.angularAxis.MagnitudeSqr() < 0.0001f)
		dropped.angularAxis = gGripMatrix[hand].GetRight();
	dropped.angularAxis.Normalise();
	dropped.angularSpeed = 4.5f+Min(speed, 4.0f);
	gDropped[hand] = dropped;
	gWeaponRenderSlot[hand] = -1;
	gWeaponContactSlot[hand] = -1;
}

static void
ExpireDroppedWeapons()
{
	const uint32 now = CTimer::GetTimeInMillisecondsNonClipped();
	for(int hand = 0; hand < VR_HAND_COUNT; hand++)
		if(gDropped[hand].slot >= 0 &&
		   now-gDropped[hand].startTime > 1800U)
			gDropped[hand] = DroppedWeapon();
}

static bool
ManualReloadAvailable()
{
	LoadSettings();
	return gManualReload && gHandsEnabled && IsGameplayAvailable() &&
		FindPlayerVehicle() == nil;
}

static bool
IsMagazineHandInUse(int hand)
{
	for(int weaponHand = 0; weaponHand < VR_HAND_COUNT; weaponHand++)
		if(gManualReloadState[weaponHand].active &&
		   gManualReloadState[weaponHand].magazineHand == hand)
			return true;
	return false;
}

static bool
BuildManualReloadSpotMatrix(int slot, CMatrix *matrix)
{
	if(!matrix)
		return false;
	CMatrix holster;
	const bool assigned = BuildHolsterMatrix(slot, &holster);
	CVector origin, right, up, forward;
	if(!GetBodyFrame(&origin, &right, &up, &forward))
		return false;
	matrix->SetUnity();
	matrix->GetRight() = right;
	matrix->GetUp() = up;
	matrix->GetForward() = forward;
	matrix->GetPosition() = assigned ? holster.GetPosition() :
		origin-up*0.34f+forward*0.14f;
	return true;
}

static bool
BuildManualReloadHeldMatrix(int hand, CMatrix *matrix)
{
	if(!matrix || hand < 0 || hand >= VR_HAND_COUNT ||
	   !gPoseValid[hand])
		return false;
	const CMatrix &pose = gGripMatrix[hand];
	matrix->SetUnity();
	matrix->GetRight() = pose.GetRight();
	matrix->GetUp() = pose.GetForward();
	matrix->GetForward() = pose.GetUp();
	matrix->GetPosition() = pose.GetPosition()+
		pose.GetForward()*0.035f-pose.GetUp()*0.015f;
	return true;
}

static bool
BuildManualReloadSocketPosition(int weaponHand, CVector *position)
{
	if(!position || weaponHand < 0 || weaponHand >= VR_HAND_COUNT ||
	   !gPoseValid[weaponHand])
		return false;
	const CMatrix &pose = gAimMatrix[weaponHand];
	CVector up = pose.GetUp();
	CVector forward = pose.GetForward();
	up.Normalise();
	forward.Normalise();
	*position = pose.GetPosition()-up*0.060f+forward*0.020f;
	return true;
}

static void
ResetManualReloadState(ManualReloadState &state)
{
	state = ManualReloadState();
}

static void
UpdateManualReloadInput(bool blocked)
{
	const uint32 now = CTimer::GetTimeInMillisecondsNonClipped();
	if(!ManualReloadAvailable()){
		for(int weaponHand = 0; weaponHand < VR_HAND_COUNT; weaponHand++)
			ResetManualReloadState(gManualReloadState[weaponHand]);
		for(int hand = 0; hand < VR_HAND_COUNT; hand++)
			gManualReloadGripDown[hand] = gGrip[hand] >= 0.45f;
		return;
	}

	for(int weaponHand = 0; weaponHand < VR_HAND_COUNT; weaponHand++){
		ManualReloadState &reload = gManualReloadState[weaponHand];
		if(reload.active &&
		   (gHeldSlot[weaponHand] != reload.slot ||
		    !gPoseValid[weaponHand]))
			ResetManualReloadState(reload);
	}

	// A held magazine must first leave its body socket, then enter the
	// magwell. This prevents an instant reload when the gun is close to the
	// same chest/waist holster.
	for(int weaponHand = 0; weaponHand < VR_HAND_COUNT; weaponHand++){
		ManualReloadState &reload = gManualReloadState[weaponHand];
		const int magazineHand = reload.magazineHand;
		if(!reload.active || reload.requested || magazineHand < 0 ||
		   blocked)
			continue;
		if(gGrip[magazineHand] <= 0.30f){
			reload.magazineHand = -1;
			reload.movedAwayFromSpot = false;
			reload.grabbedAt = 0;
			continue;
		}
		CMatrix heldMatrix, spotMatrix;
		CVector socketPosition;
		if(!BuildManualReloadHeldMatrix(magazineHand, &heldMatrix) ||
		   !BuildManualReloadSpotMatrix(reload.slot, &spotMatrix) ||
		   !BuildManualReloadSocketPosition(weaponHand, &socketPosition))
			continue;
		if((heldMatrix.GetPosition()-spotMatrix.GetPosition()).Magnitude() >=
		   0.08f)
			reload.movedAwayFromSpot = true;
		if(reload.movedAwayFromSpot && now-reload.grabbedAt >= 150U &&
		   (heldMatrix.GetPosition()-socketPosition).Magnitude() <= 0.115f){
			reload.requested = true;
			QUEST_WEAPON_LOG("[weapons] reload inserted hand=%d slot=%d",
				weaponHand, reload.slot);
		}
	}

	// A free hand grips the nearest available magazine.
	if(!blocked){
		for(int hand = 0; hand < VR_HAND_COUNT; hand++){
			if(IsMagazineHandInUse(hand) || IsSupportHand(hand) ||
			   IsTrackedDetonatorHandReserved(hand) ||
			   gHeldSlot[hand] >= 0 || !gPoseValid[hand] ||
			   gGrip[hand] < 0.65f || gManualReloadGripDown[hand])
				continue;
			float closestDistance = 0.20f;
			int closestWeaponHand = -1;
			for(int weaponHand = 0;
			    weaponHand < VR_HAND_COUNT; weaponHand++){
				ManualReloadState &reload =
					gManualReloadState[weaponHand];
				if(!reload.active || reload.requested ||
				   reload.magazineHand >= 0 || hand == weaponHand)
					continue;
				CMatrix spotMatrix;
				if(!BuildManualReloadSpotMatrix(reload.slot,
				   &spotMatrix))
					continue;
				const float distance =
					(gGripMatrix[hand].GetPosition()-
					 spotMatrix.GetPosition()).Magnitude();
				if(distance < closestDistance){
					closestDistance = distance;
					closestWeaponHand = weaponHand;
				}
			}
			if(closestWeaponHand >= 0){
				ManualReloadState &reload =
					gManualReloadState[closestWeaponHand];
				reload.magazineHand = hand;
				reload.movedAwayFromSpot = false;
				reload.grabbedAt = now;
				gGripLatched[hand] = true;
				QUEST_WEAPON_LOG(
					"[weapons] reload magazine grabbed hand=%d slot=%d",
					hand, reload.slot);
			}
		}
	}

	for(int hand = 0; hand < VR_HAND_COUNT; hand++){
		if(gGrip[hand] <= 0.30f)
			gManualReloadGripDown[hand] = false;
		else if(gGrip[hand] >= 0.65f)
			gManualReloadGripDown[hand] = true;
	}
}

static int
GetTrackedRemoteGrenadeHand()
{
	for(int hand = 0; hand < VR_HAND_COUNT; hand++)
		if(gHeldSlot[hand] == WEAPONSLOT_PROJECTILE &&
		   GetVrWeaponTypeForSlot(WEAPONSLOT_PROJECTILE) ==
			WEAPONTYPE_DETONATOR_GRENADE)
			return hand;
	return -1;
}

static bool
HasTrackedRemoteCharges()
{
	CPlayerPed *player = FindPlayerPed();
	return player && CProjectileInfo::HasDetonatorProjectile(player);
}

static bool
IsTrackedDetonatorHandReserved(int hand)
{
	if(hand < 0 || hand >= VR_HAND_COUNT || !gHandsEnabled ||
	   !IsGameplayAvailable() || FindPlayerVehicle() ||
	   !HasTrackedRemoteCharges() && GetTrackedRemoteGrenadeHand() < 0)
		return false;
	const int grenadeHand = GetTrackedRemoteGrenadeHand();
	const int desiredHand = grenadeHand >= 0 ? 1-grenadeHand :
		gTrackedDetonatorHand;
	return desiredHand == hand && gHeldSlot[hand] < 0 &&
		!IsSupportHand(hand) && !IsMagazineHandInUse(hand);
}

static void
UpdateTrackedDetonatorInput(bool blocked)
{
	for(int hand = 0; hand < VR_HAND_COUNT; hand++){
		const bool active = IsTrackedDetonatorHandReserved(hand) &&
			gPoseValid[hand];
		gTrackedDetonatorJustPressed[hand] = false;
		if(!active){
			gTrackedDetonatorWasActive[hand] = false;
			gTrackedDetonatorWaitForRelease[hand] = false;
			continue;
		}
		if(!gTrackedDetonatorWasActive[hand])
			gTrackedDetonatorWaitForRelease[hand] =
				gTrigger[hand] > 0.30f;
		gTrackedDetonatorWasActive[hand] = true;
		if(blocked){
			if(gTrigger[hand] > 0.30f)
				gTrackedDetonatorWaitForRelease[hand] = true;
			continue;
		}
		if(gTrackedDetonatorWaitForRelease[hand]){
			if(gTrigger[hand] <= 0.30f)
				gTrackedDetonatorWaitForRelease[hand] = false;
			continue;
		}
		gTrackedDetonatorJustPressed[hand] =
			gTriggerJustPressed[hand];
	}
}

static void
ClearAttachedMissionWeaponState()
{
	if(!gAttachedMissionWeaponForced)
		return;
	for(int hand = 0; hand < VR_HAND_COUNT; hand++){
		if(gHeldSlot[hand] == gAttachedMissionWeaponSlot)
			gHeldSlot[hand] = -1;
		ClearSupportForHand(hand);
		gHolsterSelection[hand] = -1;
		gWeaponRenderSlot[hand] = -1;
		gWeaponContactSlot[hand] = -1;
		gDropped[hand] = DroppedWeapon();
		ResetManualReloadState(gManualReloadState[hand]);
		gManualReloadGripDown[hand] = gGrip[hand] >= 0.45f;
		gGripLatched[hand] = gGrip[hand] >= 0.45f;
		gMeleeMotion[hand] = MeleeMotion();
		gMeleeStrike[hand] = MeleeStrike();
		gThrowablePreview[hand] = false;
	}
	gAttachedMissionWeaponForced = false;
	gAttachedMissionWeaponSlot = -1;
}

static bool
UpdateAttachedMissionWeaponInput()
{
	CPlayerPed *player = FindPlayerPed();
	const bool active = player && player->m_attachedTo &&
		player->m_currentWeapon > WEAPONSLOT_UNARMED &&
		player->m_currentWeapon < TOTAL_WEAPON_SLOTS &&
		IsPhysicalGun(player->GetWeapon()->m_eWeaponType);
	if(!active){
		ClearAttachedMissionWeaponState();
		return false;
	}

	const int slot = player->m_currentWeapon;
	const int weaponHand = 1;
	if(!gAttachedMissionWeaponForced ||
	   gAttachedMissionWeaponSlot != slot ||
	   gHeldSlot[weaponHand] != slot){
		for(int hand = 0; hand < VR_HAND_COUNT; hand++){
			ClearSupportForHand(hand);
			gHeldSlot[hand] = -1;
			gHolsterSelection[hand] = -1;
			gWeaponRenderSlot[hand] = -1;
			gWeaponContactSlot[hand] = -1;
			gDropped[hand] = DroppedWeapon();
			ResetManualReloadState(gManualReloadState[hand]);
			gMeleeMotion[hand] = MeleeMotion();
			gMeleeStrike[hand] = MeleeStrike();
			gThrowablePreview[hand] = false;
		}
		GiveWeaponToHand(weaponHand, slot);
		// PlayerControlM16 consumes the tracked trigger directly; do not emit a
		// normal holster selection for ProcessPlayerWeapon as well.
		gHolsterSelection[weaponHand] = -1;
		gAttachedMissionWeaponForced = true;
		gAttachedMissionWeaponSlot = slot;
	}

	// The free hand, whichever that is. Hard-coding the left one left a
	// left-handed grip supporting itself, which can never engage.
	const int supportHand = 1-weaponHand;
	const int weaponType = player->GetWeapon()->m_eWeaponType;
	if(gSupportHand[weaponHand] == supportHand){
		if(!gPoseValid[supportHand] || gGrip[supportHand] <= 0.30f)
			gSupportHand[weaponHand] = -1;
	}else{
		gSupportHand[weaponHand] = -1;
		if(gPoseValid[supportHand] && gGrip[supportHand] >= 0.65f){
			CVector pivot, expected;
			if(BuildSupportVector(weaponHand, weaponType, &pivot, &expected) &&
			   (gGripMatrix[supportHand].GetPosition()-
			    (pivot+expected)).Magnitude() <= 0.18f){
				gSupportHand[weaponHand] = supportHand;
				gGripLatched[supportHand] = true;
			}
		}
	}
	for(int hand = 0; hand < VR_HAND_COUNT; hand++)
		if(gGrip[hand] <= 0.30f)
			gGripLatched[hand] = false;
	return true;
}

static bool
IsAuxiliaryHandReserved(int hand)
{
	return IsMagazineHandInUse(hand) ||
		IsTrackedDetonatorHandReserved(hand);
}

static void
UpdatePoses(const androidgame::PadInput &input)
{
	const uint32 now = CTimer::GetTimeInMillisecondsNonClipped();
	for(int hand = 0; hand < VR_HAND_COUNT; hand++){
		CMatrix gripMatrix, aimMatrix;
		const bool gripValid = PoseToMatrix(input.gripPose[hand], &gripMatrix);
		const bool aimValid = PoseToMatrix(input.aimPose[hand], &aimMatrix);
		gPoseValid[hand] = gripValid;
		if(!gripValid){
			gPreviousPoseTime[hand] = 0;
			gHandVelocity[hand] = CVector(0.0f, 0.0f, 0.0f);
			continue;
		}
		gGripMatrix[hand] = gripMatrix;
		gAimMatrix[hand] = aimValid ? aimMatrix : gripMatrix;
		if(gPreviousPoseTime[hand] != 0 && now > gPreviousPoseTime[hand]){
			const float dt =
				(float)(now-gPreviousPoseTime[hand])*0.001f;
			if(dt > 0.001f && dt < 0.12f)
				gHandVelocity[hand] =
					(gripMatrix.GetPosition()-
					 gPreviousHandPosition[hand])/dt;
			else
				gHandVelocity[hand] = CVector(0.0f, 0.0f, 0.0f);
		}
		gPreviousHandPosition[hand] = gripMatrix.GetPosition();
		gPreviousPoseTime[hand] = now;
	}
	gGrip[0] = input.leftGrip;
	gGrip[1] = input.rightGrip;
	gTrigger[0] = input.leftTrigger;
	gTrigger[1] = input.rightTrigger;
}

static void
UpdateWeaponTriggerEdges(bool blocked, uint32 blockedHands,
	const androidgame::PadInput &input)
{
	const bool inVehicle = FindPlayerVehicle() != nil;
	const bool vehicleWeaponButtonAvailable =
		inVehicle && IsImmersiveDrivingActive();
	for(int hand = 0; hand < VR_HAND_COUNT; hand++){
		const bool useVehicleWeaponButton =
			vehicleWeaponButtonAvailable && gHeldSlot[hand] >= 0;
		const bool pressed = !blocked &&
			(useVehicleWeaponButton ||
			 (blockedHands & (1u << hand)) == 0) &&
			gPoseValid[hand] &&
			(useVehicleWeaponButton ? input.b : !inVehicle &&
			 (gTriggerPressed[hand] ? gTrigger[hand] >= 0.45f :
			  gTrigger[hand] >= 0.55f));
		gTriggerJustPressed[hand] =
			pressed && !gTriggerPressed[hand];
		gTriggerJustReleased[hand] =
			!pressed && gTriggerPressed[hand];
		gTriggerPressed[hand] = pressed;
	}
}

// The holster update below runs only during gameplay with no menu up, and
// the calibration page is a menu. The one thing that still has to happen
// there is the free hand taking the foregrip: that socket is exactly what
// the page edits, and it cannot be judged against a hand beside it.
static void
UpdateWeaponCalibrationSupportPreview(void)
{
	for(int primary = 0; primary < VR_HAND_COUNT; primary++){
		const int support = 1-primary;
		const int slot = gHeldSlot[primary];
		const int weaponType =
			slot >= 0 ? GetVrWeaponTypeForSlot(slot) : -1;
		if(slot < 0 || gHeldSlot[support] >= 0 ||
		   !IsTwoHanded(weaponType) || !gPoseValid[support]){
			if(gSupportHand[primary] == support)
				gSupportHand[primary] = -1;
			continue;
		}
		gSupportHand[primary] = support;
	}
}

static void
UpdateHolsterInput(uint32 blockedHands)
{
	ExpireDroppedWeapons();
	if(UpdateAttachedMissionWeaponInput())
		return;

	// Keep/release established support grips first. Releasing the primary while
	// the support hand remains closed naturally hands the weapon over.
	for(int primary = 0; primary < VR_HAND_COUNT; primary++){
		const int support = gSupportHand[primary];
		if(support < 0)
			continue;
		if((blockedHands & (1u << primary)) != 0 ||
		   (blockedHands & (1u << support)) != 0 ||
		   gHeldSlot[primary] < 0 || !gPoseValid[support] ||
		   IsAuxiliaryHandReserved(support)){
			gSupportHand[primary] = -1;
			continue;
		}
		if(gGrip[support] <= 0.30f && !gWeaponCalibrationPreview){
			gSupportHand[primary] = -1;
			continue;
		}
		if(gGrip[primary] <= 0.30f && !gGripLock){
			const int slot = gHeldSlot[primary];
			gHeldSlot[primary] = -1;
			gSupportHand[primary] = -1;
			GiveWeaponToHand(support, slot);
		}
	}

	// A fresh grip near the saved foregrip becomes the support hand.
	for(int primary = 0; primary < VR_HAND_COUNT; primary++){
		const int support = 1-primary;
		const int slot = gHeldSlot[primary];
		const int weaponType =
			slot >= 0 ? GetVrWeaponTypeForSlot(slot) : -1;
		if((blockedHands & (1u << support)) != 0 ||
		   slot < 0 || gSupportHand[primary] >= 0 ||
		   gHeldSlot[support] >= 0 || IsSupportHand(support) ||
		   IsAuxiliaryHandReserved(support) ||
		   !IsTwoHanded(weaponType) || !gPoseValid[support])
			continue;
		if(gGrip[support] < 0.65f || gGripLatched[support])
			continue;
		CVector pivot, expected;
		if(BuildSupportVector(primary, weaponType, &pivot, &expected) &&
		   (gGripMatrix[support].GetPosition()-
		    (pivot+expected)).Magnitude() <= 0.18f){
			gSupportHand[primary] = support;
			gGripLatched[support] = true;
		}
	}

	// Explicit hand-to-hand transfer at the gun body.
	for(int receiver = 0; receiver < VR_HAND_COUNT; receiver++){
		const int source = 1-receiver;
		if((blockedHands & (1u << receiver)) != 0 ||
		   gHeldSlot[receiver] >= 0 || gHeldSlot[source] < 0 ||
		   IsSupportHand(receiver) || !gPoseValid[receiver] ||
		   IsAuxiliaryHandReserved(receiver) ||
		   gGrip[receiver] < 0.65f || gGripLatched[receiver])
			continue;
		const CVector gunPosition =
			gWeaponRenderSlot[source] == gHeldSlot[source] ?
			gWeaponRenderMatrix[source].GetPosition() :
			gGripMatrix[source].GetPosition();
		if((gGripMatrix[receiver].GetPosition()-gunPosition).Magnitude() >
		   0.20f)
			continue;
		const int slot = gHeldSlot[source];
		ClearSupportForHand(source);
		gHeldSlot[source] = -1;
		GiveWeaponToHand(receiver, slot);
		gGripLatched[receiver] = true;
	}

	// A dropped gun can be caught by either free closed hand.
	for(int receiver = 0; receiver < VR_HAND_COUNT; receiver++){
		if((blockedHands & (1u << receiver)) != 0 ||
		   gHeldSlot[receiver] >= 0 || IsSupportHand(receiver) ||
		   IsAuxiliaryHandReserved(receiver) ||
		   !gPoseValid[receiver] || gGrip[receiver] < 0.55f)
			continue;
		for(int source = 0; source < VR_HAND_COUNT; source++){
			CMatrix dropped;
			if(!BuildDroppedMatrix(source, &dropped) ||
			   (gGripMatrix[receiver].GetPosition()-
			    dropped.GetPosition()).Magnitude() > 0.22f)
				continue;
			const int slot = gDropped[source].slot;
			gDropped[source] = DroppedWeapon();
			GiveWeaponToHand(receiver, slot);
			gWeaponRenderMatrix[receiver] = dropped;
			gWeaponRenderSlot[receiver] = slot;
			gGripLatched[receiver] = true;
			break;
		}
	}

	for(int hand = 0; hand < VR_HAND_COUNT; hand++){
		if((blockedHands & (1u << hand)) != 0){
			gGripLatched[hand] = gGrip[hand] >= 0.45f;
			continue;
		}
		if(IsAuxiliaryHandReserved(hand)){
			if(gGrip[hand] <= 0.30f)
				gGripLatched[hand] = false;
			continue;
		}
		if(IsSupportHand(hand)){
			if(gGrip[hand] <= 0.30f)
				gGripLatched[hand] = false;
			continue;
		}
		if(gHeldSlot[hand] >= 0 && gGrip[hand] <= 0.30f){
			const int slot = gHeldSlot[hand];
			CMatrix holster;
			const bool returnToHolster = gPoseValid[hand] &&
				BuildHolsterMatrix(slot, &holster) &&
				(gGripMatrix[hand].GetPosition()-holster.GetPosition()).Magnitude()
					<= 0.24f;
			if(gGripLock && !returnToHolster)
				continue;
			ClearSupportForHand(hand);
			gHeldSlot[hand] = -1;
			gHolsterSelection[hand] = -1;
			if(!returnToHolster)
				StartDroppedWeapon(hand, slot);
		}
		if(gHeldSlot[hand] < 0 && gPoseValid[hand] &&
		   gGrip[hand] >= 0.65f && !gGripLatched[hand]){
			float closestDistance = 1000.0f;
			int closestSlot = -1;
			for(int slot = WEAPONSLOT_MELEE;
			    slot < TOTAL_WEAPON_SLOTS; slot++){
				if((gWeaponHolsterMask & (1u << slot)) == 0)
					continue;
				bool alreadyUsed = false;
				for(int other = 0; other < VR_HAND_COUNT; other++)
					if(gHeldSlot[other] == slot ||
					   gDropped[other].slot == slot)
						alreadyUsed = true;
				if(alreadyUsed)
					continue;
				CMatrix holster;
				if(!BuildHolsterMatrix(slot, &holster))
					continue;
				const float distance =
					(gGripMatrix[hand].GetPosition()-
					 holster.GetPosition()).Magnitude();
				if(distance < closestDistance){
					closestDistance = distance;
					closestSlot = slot;
				}
			}
			if(closestSlot >= 0 && closestDistance <= 0.27f){
				GiveWeaponToHand(hand, closestSlot);
				gGripLatched[hand] = true;
			}
		}
		if(gGrip[hand] <= 0.30f)
			gGripLatched[hand] = false;
		else if(gGrip[hand] >= 0.65f)
			gGripLatched[hand] = true;
	}
}

static CVector
MeleeModelTip(int weaponType)
{
	switch(weaponType){
	case WEAPONTYPE_SCREWDRIVER: return CVector(0.160f, -0.017f, 0.277f);
	case WEAPONTYPE_GOLFCLUB: return CVector(0.029f, 0.0f, 0.848f);
	case WEAPONTYPE_NIGHTSTICK: return CVector(0.050f, 0.030f, 0.451f);
	case WEAPONTYPE_KNIFE: return CVector(0.133f, 0.040f, 0.416f);
	case WEAPONTYPE_BASEBALLBAT: return CVector(0.068f, -0.019f, 0.680f);
	case WEAPONTYPE_HAMMER: return CVector(0.045f, 0.036f, 0.317f);
	case WEAPONTYPE_CLEAVER: return CVector(0.075f, 0.036f, 0.270f);
	case WEAPONTYPE_MACHETE: return CVector(0.079f, 0.023f, 0.572f);
	case WEAPONTYPE_KATANA: return CVector(0.0468f, -0.0246f, 0.9847f);
	case WEAPONTYPE_CHAINSAW: return CVector(0.865f, 0.033f, 0.335f);
	default: return CVector(0.0f, 0.0f, 0.11f);
	}
}

static CVector
MeleeModelRoot(int weaponType)
{
	switch(weaponType){
	case WEAPONTYPE_SCREWDRIVER: return CVector(0.078f, 0.027f, 0.0f);
	case WEAPONTYPE_GOLFCLUB: return CVector(0.064f, 0.035f, 0.0f);
	case WEAPONTYPE_NIGHTSTICK: return CVector(0.081f, 0.030f, 0.0f);
	case WEAPONTYPE_KNIFE: return CVector(0.089f, 0.038f, 0.0f);
	case WEAPONTYPE_BASEBALLBAT: return CVector(0.066f, 0.035f, 0.0f);
	case WEAPONTYPE_HAMMER: return CVector(0.078f, 0.035f, 0.0f);
	case WEAPONTYPE_CLEAVER: return CVector(0.073f, 0.036f, 0.0f);
	case WEAPONTYPE_MACHETE: return CVector(0.073f, 0.018f, 0.0f);
	case WEAPONTYPE_KATANA: return CVector(0.0725f, 0.0245f, 0.0467f);
	case WEAPONTYPE_CHAINSAW: return CVector(0.485f, 0.038f, 0.160f);
	default: return CVector(0.0f, 0.0f, 0.0f);
	}
}

// Express a world point relative to the first-person camera frame. Gesture
// speeds must come from this space: Tommy's locomotion, snap turns and
// anchor refreshes move the world-space blade at tens of metres per second,
// which kept the swing detector permanently above its calm threshold. The
// desktop implementation measures in the equivalent camera-relative space.
static CVector
ToMeleeTrackingSpace(const CVector &worldPoint)
{
	float right[3], up[3], at[3], position[3];
	if(!rw::vulkan::getFirstPersonViewFrame(right, up, at, position))
		return worldPoint;
	const CVector r = VectorFromArray(right);
	const CVector u = VectorFromArray(up);
	const CVector f = VectorFromArray(at);
	const CVector relative = worldPoint-VectorFromArray(position);
	return CVector(DotProduct(relative, r), DotProduct(relative, f),
		DotProduct(relative, u));
}

static void
UpdateMelee()
{
	const uint32 frame = CTimer::GetFrameCounter();
	const uint32 now = CTimer::GetTimeInMillisecondsNonClipped();
	const float dt = Max(0.001f, CTimer::GetTimeStepNonClippedInSeconds());
	for(int hand = 0; hand < VR_HAND_COUNT; hand++){
		if(gMeleeStrike[hand].pending &&
		   frame-gMeleeStrike[hand].frame > 1U)
			gMeleeStrike[hand].pending = false;
		if(!IsGameplayAvailable() || FindPlayerVehicle() ||
		   !gPoseValid[hand] || IsSupportHand(hand)){
			gMeleeMotion[hand] = MeleeMotion();
			continue;
		}
		int slot = gHeldSlot[hand];
		int weaponType = -1;
		bool enabled = false;
		CVector root, tip;
		if(slot >= 0){
			weaponType = GetVrWeaponTypeForSlot(slot);
			enabled = IsPhysicalMelee(weaponType);
			if(!enabled){
				gMeleeMotion[hand] = MeleeMotion();
				continue;
			}
			// Build the calibrated visible blade from this input sample. The
			// render cache belongs to the previous frame and camera anchor; using
			// it here both delays a hit and measures old world points against the
			// new camera. Missing renders also used to shorten a katana to 45 cm.
			CMatrix model;
			WeaponCalibration *calibration = GetCalibration(hand, weaponType);
			if(!calibration || !BuildWeaponModelMatrix(hand, *calibration, &model)){
				gMeleeMotion[hand] = MeleeMotion();
				continue;
			}
			root = model*MeleeModelRoot(weaponType);
			tip = model*MeleeModelTip(weaponType);
		}else{
			slot = WEAPONSLOT_UNARMED;
			weaponType = GetVrWeaponTypeForSlot(slot);
			enabled = IsPhysicalMelee(weaponType) &&
				gGrip[hand] >= 0.65f && gTrigger[hand] >= 0.45f;
			root = gGripMatrix[hand].GetPosition();
			tip = root+gAimMatrix[hand].GetForward()*0.11f;
		}
		if(!IsPhysicalMelee(weaponType)){
			gMeleeMotion[hand] = MeleeMotion();
			continue;
		}
		const CVector trackingRoot = ToMeleeTrackingSpace(root);
		const CVector trackingTip = ToMeleeTrackingSpace(tip);
		MeleeMotion &motion = gMeleeMotion[hand];
		if(!motion.valid || motion.slot != slot ||
		   motion.weaponType != weaponType ||
		   frame-motion.previousFrame > 2U || dt > 0.10f){
			motion = MeleeMotion();
			motion.valid = true;
			// The first sample establishes a baseline and never hits. Permit a
			// deliberate swing on the next sample: requiring a perfectly calm
			// long blade before its first hit could leave it disarmed forever
			// when the player grabbed it directly into a series of swings.
			motion.armed = true;
			motion.slot = slot;
			motion.weaponType = weaponType;
			motion.previousRoot = root;
			motion.previousTip = tip;
			motion.previousTrackingRoot = trackingRoot;
			motion.previousTrackingTip = trackingTip;
			motion.previousFrame = frame;
			motion.calmSince = now;
			continue;
		}
		const float trackingTravel =
			(trackingTip-motion.previousTrackingTip).Magnitude();
		const float tipSpeed = trackingTravel/dt;
		const float rootSpeed =
			(trackingRoot-motion.previousTrackingRoot).Magnitude()/dt;
		const float threshold =
			slot == WEAPONSLOT_UNARMED ? 1.25f : 0.70f;
		const bool calm = slot == WEAPONSLOT_UNARMED ?
			tipSpeed <= 0.48f :
			tipSpeed <= 0.40f && rootSpeed <= 0.30f;
		if(calm){
			if(motion.calmSince == 0)
				motion.calmSince = now;
			if(slot == WEAPONSLOT_UNARMED ||
			   now-motion.calmSince >= 70U)
				motion.armed = true;
		}else
			motion.calmSince = 0;
		if(!motion.armed && motion.lastStrikeTime &&
		   now-motion.lastStrikeTime >= 450U)
			motion.armed = true;
		// Large discontinuities are tracking loss or recenter events, never
		// superhuman attacks. Keep the new sample but require calm to rearm.
		if(trackingTravel > 0.65f){
			motion.armed = false;
			motion.previousRoot = root;
			motion.previousTip = tip;
			motion.previousTrackingRoot = trackingRoot;
			motion.previousTrackingTip = trackingTip;
			motion.previousFrame = frame;
			continue;
		}
		const bool deliberate = slot == WEAPONSLOT_UNARMED ?
			rootSpeed >= 0.75f : rootSpeed >= 0.25f ||
			(trackingTip-trackingRoot-motion.previousTrackingTip+
			 motion.previousTrackingRoot).Magnitude()/dt >= 0.50f;
		if(motion.strikeInProgress && now > motion.strikeContinueUntil){
			motion.strikeInProgress = false;
			motion.strikePeakSpeed = 0.0f;
		}
		const bool fastStrikeSample = enabled && motion.armed &&
			tipSpeed >= threshold && deliberate &&
			now-motion.lastStrikeTime >= 220U;
		if(fastStrikeSample){
			motion.strikeInProgress = true;
			motion.strikePeakSpeed =
				Max(motion.strikePeakSpeed, tipSpeed);
			motion.strikeContinueUntil = now+180U;
		}
		// Once a genuine fast swing has started, keep publishing its slower
		// tail. The trigger frame's short sweep segment usually hasn't reached
		// the target yet; the weapon must keep dealing contact while it
		// travels. Without this a swing only landed when the player's own
		// movement carried the one-frame segment onto the target -- running
		// into someone worked, standing still and swinging a bat did not.
		// Contact ends the window immediately, so this cannot repeat-hit.
		const bool continuationSample =
			motion.strikeInProgress &&
			now <= motion.strikeContinueUntil && tipSpeed >= 0.08f;
		if(enabled && motion.armed &&
		   (fastStrikeSample || continuationSample) &&
		   !gMeleeStrike[hand].pending){
			MeleeStrike &strike = gMeleeStrike[hand];
			strike.pending = true;
			strike.slot = slot;
			strike.weaponType = weaponType;
			strike.tipStart = motion.previousTip;
			strike.tipEnd = tip;
			strike.rootStart = motion.previousRoot;
			strike.rootEnd = root;
			strike.speed = Max(tipSpeed, motion.strikePeakSpeed);
			strike.frame = frame;
		}
		motion.previousRoot = root;
		motion.previousTip = tip;
		motion.previousTrackingRoot = trackingRoot;
		motion.previousTrackingTip = trackingTip;
		motion.previousFrame = frame;
	}
}

static bool
BuildAim(int hand, int weaponType, CVector *source, CVector *direction)
{
	if(!source || !direction || hand < 0 || hand >= VR_HAND_COUNT ||
	   !gPoseValid[hand] || gHeldSlot[hand] < 0)
		return false;
	WeaponCalibration *calibration =
		GetCalibration(hand, weaponType);
	if(!calibration)
		return false;
	CVector right = gAimMatrix[hand].GetRight();
	CVector up = gAimMatrix[hand].GetUp();
	CVector forward = gAimMatrix[hand].GetForward();
	forward.Normalise();
	up -= forward*DotProduct(up, forward);
	up.Normalise();
	// Preserve the pose's own lateral axis. CrossProduct(up, forward) is
	// -right in this coordinate system, which mirrored aimOffsetX and
	// aimRotationX against the shared desktop calibration: every laser sat
	// beside the barrel, displaced toward the wrong side.
	right -= forward*DotProduct(right, forward);
	right -= up*DotProduct(right, up);
	right.Normalise();
	const float pitch =
		DEGTORAD((float)calibration->aimRotationX/WEAPON_VALUE_SCALE);
	const float yaw =
		DEGTORAD((float)calibration->aimRotationY/WEAPON_VALUE_SCALE);
	const float roll =
		DEGTORAD((float)calibration->aimRotationZ/WEAPON_VALUE_SCALE);
	if(pitch != 0.0f){
		forward = RotateAroundAxis(forward, right, pitch);
		up = RotateAroundAxis(up, right, pitch);
	}
	if(yaw != 0.0f){
		forward = RotateAroundAxis(forward, up, yaw);
		right = RotateAroundAxis(right, up, yaw);
	}
	if(roll != 0.0f){
		right = RotateAroundAxis(right, forward, roll);
		up = RotateAroundAxis(up, forward, roll);
	}
	forward.Normalise();
	*direction = forward;
	*source = gAimMatrix[hand].GetPosition() +
		right*((float)calibration->aimOffsetX/200.0f) +
		up*((float)calibration->aimOffsetY/200.0f) +
		forward*(0.18f+(float)calibration->aimOffsetZ/200.0f);
	CVector pivot, axis;
	float angle;
	if(BuildTwoHandRotation(hand, weaponType, &pivot, &axis, &angle)){
		*source = pivot+RotateAroundAxis(*source-pivot, axis, angle);
		*direction = RotateAroundAxis(*direction, axis, angle);
		direction->Normalise();
	}
	return true;
}

static bool
BuildScopeCentreTarget(CVector *target)
{
	if(!target || !IsGameplayAvailable() || FindPlayerVehicle())
		return false;
	float viewRight[3], viewUp[3], viewAt[3], viewPosition[3];
	if(!rw::vulkan::getFirstPersonViewFrame(
	   viewRight, viewUp, viewAt, viewPosition))
		return false;

	const CVector origin = VectorFromArray(viewPosition);
	CVector forward = VectorFromArray(viewAt);
	if(forward.MagnitudeSqr() < 0.0001f)
		return false;
	forward.Normalise();

	// The scope/camera overlay is centred on the HMD view, so its centre ray is
	// authoritative while the optic is active. Converge the tracked muzzle on
	// the first world point under that ray, matching the desktop implementation.
	*target = origin+forward*150.0f;
	CColPoint point;
	CEntity *hitEntity = nil;
	CEntity *savedIgnoreEntity = CWorld::pIgnoreEntity;
	CWorld::pIgnoreEntity = FindPlayerPed();
	if(CWorld::ProcessLineOfSight(origin, *target, point, hitEntity,
	   true, true, true, true, true, false, false, false))
		*target = point.point;
	CWorld::pIgnoreEntity = savedIgnoreEntity;
	return true;
}

static void
UpdateScope()
{
	if(!IsGameplayAvailable() || FindPlayerVehicle()){
		gScopeHand = -1;
		gScopeWeaponType = -1;
		gScopeCandidateHand = -1;
		gScopeCandidateType = -1;
		gScopeCandidateSince = gScopeReleaseSince = 0;
		return;
	}
	float viewRight[3], viewUp[3], viewAt[3], viewPosition[3];
	if(!rw::vulkan::getFirstPersonViewFrame(
	   viewRight, viewUp, viewAt, viewPosition)){
		gScopeHand = -1;
		gScopeWeaponType = -1;
		return;
	}
	const CVector head = VectorFromArray(viewPosition);
	CVector headForward = VectorFromArray(viewAt);
	CVector headUp = VectorFromArray(viewUp);
	CVector headRight = VectorFromArray(viewRight);
	headForward.Normalise();
	headUp.Normalise();
	headRight.Normalise();
	const uint32 now = CTimer::GetTimeInMillisecondsNonClipped();
	int candidateHand = -1;
	int candidateType = -1;
	float candidateScore = 1000.0f;
	for(int hand = 0; hand < VR_HAND_COUNT; hand++){
		const int slot = gHeldSlot[hand];
		const int weaponType =
			slot >= 0 ? GetVrWeaponTypeForSlot(slot) : -1;
		if(!IsScopeWeapon(weaponType) || !gPoseValid[hand] ||
		   (!gScopeAim && weaponType != WEAPONTYPE_CAMERA))
			continue;
		if(IsTwoHanded(weaponType) && gSupportHand[hand] < 0)
			continue;
		const CVector relative =
			gAimMatrix[hand].GetPosition()-head;
		const float distance = relative.Magnitude();
		const float forward = DotProduct(relative, headForward);
		const float height = DotProduct(relative, headUp);
		const float lateral = DotProduct(relative, headRight);
		CVector aimSource, aim;
		if(!BuildAim(hand, weaponType, &aimSource, &aim))
			continue;
		aim.Normalise();
		const float alignment = DotProduct(headForward, aim);
		if(distance < 0.04f || distance > 0.65f ||
		   forward < -0.03f || forward > 0.58f ||
		   height < -0.30f || height > 0.14f ||
		   Abs(lateral) > 0.30f || alignment < 0.70f)
			continue;
		const float score = distance+Abs(lateral)*1.5f+
			(1.0f-alignment)*0.8f;
		if(score < candidateScore){
			candidateScore = score;
			candidateHand = hand;
			candidateType = weaponType;
		}
	}
	if(candidateHand >= 0){
		if(gScopeHand == candidateHand &&
		   gScopeWeaponType == candidateType){
			gScopeReleaseSince = 0;
			gScopeCandidateHand = candidateHand;
			gScopeCandidateType = candidateType;
			gScopeCandidateSince = 0;
			return;
		}
		if(gScopeCandidateHand != candidateHand ||
		   gScopeCandidateType != candidateType){
			gScopeCandidateHand = candidateHand;
			gScopeCandidateType = candidateType;
			gScopeCandidateSince = now;
		}
		if(gScopeCandidateSince == 0)
			gScopeCandidateSince = now;
		if(now-gScopeCandidateSince >= 120U){
			gScopeHand = candidateHand;
			gScopeWeaponType = candidateType;
			gScopeReleaseSince = 0;
		}
	}else{
		gScopeCandidateHand = -1;
		gScopeCandidateType = -1;
		gScopeCandidateSince = 0;
		if(gScopeHand >= 0){
			if(gScopeReleaseSince == 0)
				gScopeReleaseSince = now;
			if(now-gScopeReleaseSince >= 180U){
				gScopeHand = -1;
				gScopeWeaponType = -1;
				gScopeReleaseSince = 0;
			}
		}
	}
}

static void
ResetInteraction()
{
	ClearAttachedMissionWeaponState();
	for(int hand = 0; hand < VR_HAND_COUNT; hand++){
		ClearSupportForHand(hand);
		gHeldSlot[hand] = -1;
		gHolsterSelection[hand] = -1;
		gWeaponRenderSlot[hand] = -1;
		gWeaponContactSlot[hand] = -1;
		gDropped[hand] = DroppedWeapon();
		gMeleeMotion[hand] = MeleeMotion();
		gMeleeStrike[hand] = MeleeStrike();
		gThrowablePreview[hand] = false;
		ResetManualReloadState(gManualReloadState[hand]);
		gManualReloadGripDown[hand] = gGrip[hand] >= 0.45f;
		gTrackedDetonatorWasActive[hand] = false;
		gTrackedDetonatorWaitForRelease[hand] = false;
		gTrackedDetonatorJustPressed[hand] = false;
	}
	gScopeHand = -1;
	gScopeWeaponType = -1;
	gScopeCandidateHand = -1;
	gScopeCandidateType = -1;
	gScopeCandidateSince = gScopeReleaseSince = 0;
}

} // namespace

bool
ApplyTouchInput(CControllerState *state)
{
	if(!state)
		return false;
	LoadSettings();
	const androidgame::PadInput &input = androidgame::GetPadInput();
	UpdatePoses(input);
	const bool menu = androidgame::VrMenuConsumesInput();
	const bool gameplay = IsGameplayAvailable();
	const bool shortcutModifier =
		input.leftGrip >= 0.75f && input.rightGrip >= 0.75f &&
		gHeldSlot[0] < 0 && gHeldSlot[1] < 0 &&
		!IsTrackedDetonatorHandReserved(0) &&
		!IsTrackedDetonatorHandReserved(1);
	const bool weaponStickHorizontal =
		fabsf(input.rightStickX) >= 0.65f &&
		fabsf(input.rightStickX) > fabsf(input.rightStickY);
	// Desktop fallback for players who disable physical VR hands: both grips
	// turn the right stick into one-shot previous/next weapon input instead of
	// camera rotation. CPad owns edge detection through its normal old/new
	// Shoulder2 states, so holding the stick cannot race through every slot.
	const bool weaponCycleLeft = gameplay && !menu && !gHandsEnabled &&
		!FindPlayerVehicle() && shortcutModifier &&
		weaponStickHorizontal && input.rightStickX < 0.0f;
	const bool weaponCycleRight = gameplay && !menu && !gHandsEnabled &&
		!FindPlayerVehicle() && shortcutModifier &&
		weaponStickHorizontal && input.rightStickX > 0.0f;
	const uint32 vehicleCapturedHands =
		UpdateQuestDrivingInput(state, menu || !gameplay);
	const bool inVehicle = FindPlayerVehicle() != nil;
	if(!inVehicle)
		UpdateWeaponTriggerEdges(menu || !gameplay, vehicleCapturedHands, input);
	UpdateTrackedDetonatorInput(menu || !gameplay);
	// Quest has no in-vehicle magazine insertion path. Entering any vehicle
	// makes ManualReloadAvailable false, which also clears an in-progress
	// reload instead of leaving a magazine hand reserved behind the wheel.
	UpdateManualReloadInput(menu || !gameplay || FindPlayerVehicle());
	if(gameplay && !menu &&
	   (!FindPlayerVehicle() || IsImmersiveDrivingActive()))
		UpdateHolsterInput(vehicleCapturedHands);
	else if(gWeaponCalibrationPreview && gameplay)
		UpdateWeaponCalibrationSupportPreview();
	else if(!gameplay)
		ResetInteraction();
	// A draw or holster can change which firing path owns B during this frame.
	if(inVehicle)
		UpdateWeaponTriggerEdges(menu || !gameplay, vehicleCapturedHands, input);
	ApplyQuestVehicleButtonInput(state, menu || !gameplay);
	const bool vehicleWeaponButtonConsumed =
		gameplay && !menu && inVehicle && IsImmersiveDrivingActive() &&
		(gHeldSlot[0] >= 0 || gHeldSlot[1] >= 0);
	UpdateMelee();
	UpdateScope();

	// CapturePad has to map triggers to classic acceleration/brake for vehicles.
	// On foot those same states must not become a second animation-driven shot:
	// retain the face buttons, but remove only the trigger contribution.
	// In Immersive/Motion driving B is consumed by the physically aimed weapon;
	// do not also pass it into the legacy vehicle action.
	if(vehicleWeaponButtonConsumed)
		state->Circle = 0;
	if(gameplay && !menu && !FindPlayerVehicle()){
		const int padMode = CPad::GetPad(0)->GetMode();
		// Rebuild the mapped buttons from the CONTROLS assignment without the
		// triggers. The attack button of the current pad mode then belongs to
		// the physically aimed weapon, whichever input reaches it.
		androidgame::VrApplyPadBindings(state, input, false);
		if(padMode == 2)
			state->Cross = 0;
		if(padMode == 0 || padMode == 1)
			state->Circle = 0;
		if(gHeldSlot[0] >= 0 || IsSupportHand(0) ||
		   IsAuxiliaryHandReserved(0))
			state->LeftShoulder1 = 0;
		if(gHeldSlot[1] >= 0 || IsSupportHand(1) ||
		   IsAuxiliaryHandReserved(1) || padMode == 3)
			state->RightShoulder1 = 0;
		if(gAttachedMissionWeaponForced){
			// Both grips belong to the mounted gun and its optional support
			// grip. Never leak either into Vice City's L1/radio bindings.
			state->LeftShoulder1 = 0;
			state->RightShoulder1 = 0;
		}
	}
	if(weaponCycleLeft || weaponCycleRight){
		state->RightStickX = 0;
		state->LeftShoulder1 = 0;
		state->RightShoulder1 = 0;
		if(weaponCycleLeft)
			state->LeftShoulder2 = Max(
				state->LeftShoulder2, (int16)255);
		if(weaponCycleRight)
			state->RightShoulder2 = Max(
				state->RightShoulder2, (int16)255);
	}
	return true;
}

bool AreTrackedHandsEnabled()
{
	LoadSettings();
#if MIAMIVR_STEAM_FRAME
	if(gVrThirdPersonOnFoot)
		return false;
#endif
	return gHandsEnabled && gVrFirstPersonActive;
}
bool ShouldUseTrackedHands()
{
	return AreTrackedHandsEnabled() &&
		(!FindPlayerVehicle() || IsImmersiveDrivingActive());
}
bool IsTrackedHandReady(int hand)
{
	return hand >= 0 && hand < VR_HAND_COUNT &&
		gPoseValid[hand] && ShouldUseTrackedHands();
}
// The calibration page has to show what is being calibrated, whatever the
// player has done with the highlights during ordinary play.
void
SetQuestWeaponCalibrationPreview(bool visible)
{
	gWeaponCalibrationPreview = visible;
}

bool AreWeaponHolsterHighlightsEnabled()
{
	LoadSettings();
	return gHolsterHighlights || gWeaponCalibrationPreview;
}
bool IsTrackedWeaponLaserEnabled() { LoadSettings(); return gWeaponLaser; }

void
InvalidateQuestWeaponLaserOverrides()
{
	memset(gWeaponLaserOverrides, 0, sizeof(gWeaponLaserOverrides));
}

// Per weapon, with the page-wide switch as the fallback. A laser earns its
// place on a pistol and gets in the way on a rifle with iron sights, so the
// choice belongs to the weapon rather than to the whole loadout.
bool
IsTrackedWeaponLaserEnabledForType(int weaponType)
{
	LoadSettings();
	const int stored = ReadWeaponLaserOverride(weaponType);
	return stored < 0 ? gWeaponLaser : stored != 0;
}

int
GetQuestWeaponLaserOverride(int weaponType)
{
	LoadSettings();
	return ReadWeaponLaserOverride(weaponType);
}

void
CycleQuestWeaponLaserOverride(int weaponType, int direction)
{
	LoadSettings();
	// Three states in a ring: follow the global switch, always on, off.
	int stored = ReadWeaponLaserOverride(weaponType)+1;
	stored = (stored+3+(direction < 0 ? -1 : 1)) % 3;
	WriteWeaponLaserOverride(weaponType, stored-1);
}
bool IsTrackedScopeActive() { return gScopeHand >= 0; }
bool IsTrackedScopeActiveForHand(int hand) { return gScopeHand == hand; }
int GetTrackedScopeWeaponType() { return gScopeWeaponType; }

bool
ShouldUseTrackedWeapon(int hand)
{
	return hand >= 0 && hand < VR_HAND_COUNT &&
		gHeldSlot[hand] >= 0 && gPoseValid[hand] &&
		IsGameplayAvailable();
}

bool IsTrackedWeaponTriggerPressed(int hand)
{
	return hand >= 0 && hand < VR_HAND_COUNT &&
		gTriggerPressed[hand] &&
		(!FindPlayerVehicle() || IsImmersiveDrivingActive());
}
bool IsTrackedWeaponTriggerJustPressed(int hand)
{
	return IsTrackedWeaponTriggerPressed(hand) &&
		gTriggerJustPressed[hand];
}

static int
GetTrackedWeaponFireTriggerHand(int hand, int weaponType)
{
	if(hand < 0 || hand >= VR_HAND_COUNT || gHeldSlot[hand] < 0 ||
	   !gPoseValid[hand] || !IsGameplayAvailable())
		return -1;
	if(weaponType != WEAPONTYPE_ROCKETLAUNCHER || FindPlayerVehicle())
		return hand;
	const int opposite = 1-hand;
	// The RPG model is shouldered from the support side: right-held uses L2,
	// left-held uses R2. Never steal a trigger from another gun/detonator.
	if(gHeldSlot[opposite] >= 0 || IsTrackedDetonatorActive(opposite))
		return -1;
	return opposite;
}

bool IsTrackedWeaponFireTriggerPressed(int hand, int weaponType)
{
	const int triggerHand = GetTrackedWeaponFireTriggerHand(hand, weaponType);
	return triggerHand >= 0 && gTriggerPressed[triggerHand];
}

bool IsTrackedWeaponFireTriggerJustPressed(int hand, int weaponType)
{
	const int triggerHand = GetTrackedWeaponFireTriggerHand(hand, weaponType);
	return triggerHand >= 0 && gTriggerPressed[triggerHand] &&
		gTriggerJustPressed[triggerHand];
}
bool IsTrackedWeaponTriggerJustReleased(int hand)
{
	return hand >= 0 && hand < VR_HAND_COUNT &&
		gTriggerJustReleased[hand] &&
		(!FindPlayerVehicle() || IsImmersiveDrivingActive());
}
bool IsTrackedDetonatorActive(int hand) { return IsTrackedDetonatorHandReserved(hand) && gPoseValid[hand]; }
bool IsTrackedDetonatorTriggerJustPressed(int hand)
{
	return hand >= 0 && hand < VR_HAND_COUNT &&
		HasTrackedRemoteCharges() && IsTrackedDetonatorActive(hand) &&
		gTrackedDetonatorJustPressed[hand];
}
bool IsTrackedRemoteGrenadeFireActive()
{
	return gActiveFire && gActiveFireHand >= 0 &&
		gActiveFireWeaponType == WEAPONTYPE_DETONATOR_GRENADE;
}
bool ShouldKeepTrackedRemoteCharges(CEntity *source)
{
	return source && source == FindPlayerPed() &&
		CProjectileInfo::HasDetonatorProjectile(source);
}
void NotifyTrackedRemoteGrenadeThrown(int hand)
{
	if(hand < 0 || hand >= VR_HAND_COUNT)
		return;
	gTrackedDetonatorHand = 1-hand;
	gTrackedDetonatorWasActive[gTrackedDetonatorHand] = false;
	gTrackedDetonatorWaitForRelease[gTrackedDetonatorHand] =
		gTrigger[gTrackedDetonatorHand] > 0.30f;
}
void NotifyTrackedDetonatorActivated(int hand)
{
	if(hand < 0 || hand >= VR_HAND_COUNT)
		return;
	gTrackedDetonatorJustPressed[hand] = false;
	gTrackedDetonatorWaitForRelease[hand] = true;
}

bool
IsManualReloadWeaponType(int weaponType)
{
	return weaponType == WEAPONTYPE_COLT45 ||
		weaponType == WEAPONTYPE_TEC9 ||
		weaponType == WEAPONTYPE_UZI ||
		weaponType == WEAPONTYPE_SILENCED_INGRAM;
}

void
SetManualReloadWeaponState(int weaponHand, int slot, int weaponType,
	bool available)
{
	if(weaponHand < 0 || weaponHand >= VR_HAND_COUNT)
		return;
	ManualReloadState &reload = gManualReloadState[weaponHand];
	available = available && ManualReloadAvailable() &&
		IsManualReloadWeaponType(weaponType) &&
		slot > WEAPONSLOT_UNARMED && slot < TOTAL_WEAPON_SLOTS &&
		gHeldSlot[weaponHand] == slot;
	if(!available){
		ResetManualReloadState(reload);
		return;
	}
	if(gSupportHand[weaponHand] >= 0)
		gSupportHand[weaponHand] = -1;
	if(!reload.active || reload.slot != slot ||
	   reload.weaponType != weaponType){
		ResetManualReloadState(reload);
		reload.active = true;
		reload.weaponHand = weaponHand;
		reload.slot = slot;
		reload.weaponType = weaponType;
	}
}
bool ShouldUseManualReload() { return ManualReloadAvailable(); }
bool
ConsumeManualReloadRequest(int weaponHand, int slot, int weaponType)
{
	if(weaponHand < 0 || weaponHand >= VR_HAND_COUNT)
		return false;
	ManualReloadState &reload = gManualReloadState[weaponHand];
	const bool requested = reload.active && reload.requested &&
		reload.weaponHand == weaponHand && reload.slot == slot &&
		reload.weaponType == weaponType &&
		gHeldSlot[weaponHand] == slot;
	if(requested)
		ResetManualReloadState(reload);
	return requested;
}
bool
GetManualReloadMagazineMatrix(int weaponHand, CMatrix *matrix,
	int *weaponType, bool *held)
{
	if(!matrix || weaponHand < 0 || weaponHand >= VR_HAND_COUNT)
		return false;
	ManualReloadState &reload = gManualReloadState[weaponHand];
	if(!reload.active || reload.requested || !ManualReloadAvailable())
		return false;
	const bool magazineHeld = reload.magazineHand >= 0;
	const bool ready = magazineHeld ?
		BuildManualReloadHeldMatrix(reload.magazineHand, matrix) :
		BuildManualReloadSpotMatrix(reload.slot, matrix);
	if(!ready)
		return false;
	if(weaponType)
		*weaponType = reload.weaponType;
	if(held)
		*held = magazineHeld;
	return true;
}

void
SetWeaponHolsterMask(uint32 mask)
{
	gWeaponHolsterMask = mask;
	for(int hand = 0; hand < VR_HAND_COUNT; hand++){
		if(gHeldSlot[hand] >= 0 &&
		   (mask & (1u << gHeldSlot[hand])) == 0){
			ClearSupportForHand(hand);
			gHeldSlot[hand] = -1;
			gWeaponRenderSlot[hand] = -1;
		}
		if(gDropped[hand].slot >= 0 &&
		   (mask & (1u << gDropped[hand].slot)) == 0)
			gDropped[hand] = DroppedWeapon();
	}
}

bool
ConsumeWeaponHolsterSelection(int hand, int *slot)
{
	if(!slot || hand < 0 || hand >= VR_HAND_COUNT)
		return false;
	*slot = gHolsterSelection[hand];
	gHolsterSelection[hand] = -1;
	return *slot >= 0;
}

bool
GetWeaponHolsterMatrix(int slot, CMatrix *matrix)
{
	if(!AreTrackedHandsEnabled() ||
	   (FindPlayerVehicle() && !IsImmersiveDrivingActive()) ||
	   (gWeaponHolsterMask & (1u << slot)) == 0)
		return false;
	for(int hand = 0; hand < VR_HAND_COUNT; hand++)
		if(gHeldSlot[hand] == slot || gDropped[hand].slot == slot)
			return false;
	return BuildHolsterMatrix(slot, matrix);
}

bool IsPhysicalGunType(int weaponType) { return IsPhysicalGun(weaponType); }
bool IsPhysicalMeleeType(int weaponType) { return IsPhysicalMelee(weaponType); }
bool IsPhysicalThrowableType(int weaponType) { return IsPhysicalThrowable(weaponType); }
bool IsPhysicalWeaponType(int weaponType) { return IsPhysicalGun(weaponType) || IsPhysicalMelee(weaponType) || IsPhysicalThrowable(weaponType); }
bool IsPhysicalWeaponInteractionActive() { return AreTrackedHandsEnabled() && IsGameplayAvailable() && !FindPlayerVehicle(); }
bool IsQuestFirstPersonOnFoot()
{
#if MIAMIVR_STEAM_FRAME
	if(gVrThirdPersonOnFoot)
		return false;
#endif
	return gVrFirstPersonActive && !FindPlayerVehicle();
}
bool IsTrackedWeaponHeld(int hand)
{
	return hand >= 0 && hand < VR_HAND_COUNT &&
		gHeldSlot[hand] >= 0 &&
		(!FindPlayerVehicle() || IsImmersiveDrivingActive());
}
int GetHeldWeaponSlot(int hand) { return hand >= 0 && hand < VR_HAND_COUNT ? gHeldSlot[hand] : -1; }

bool IsRunWithoutLimitsEnabled()
{
	LoadSettings();
	return gRunWithoutLimits;
}

void SetRunWithoutLimitsEnabled(bool enabled)
{
	LoadSettings();
	gRunWithoutLimits = enabled;
	WritePrivateProfileStringA("VR", "RunWithoutLimits",
		enabled ? "1" : "0", kSettingsPath);
}

bool
IsQuestDrivingHandUnavailable(int hand)
{
	return hand < 0 || hand >= VR_HAND_COUNT ||
		gHeldSlot[hand] >= 0 || IsSupportHand(hand) ||
		IsAuxiliaryHandReserved(hand);
}

void
RestrictQuestVehicleWeaponsToSidearms()
{
	CPlayerPed *player = FindPlayerPed();
	if(!player)
		return;
	for(int hand = 0; hand < VR_HAND_COUNT; hand++){
		const int slot = gHeldSlot[hand];
		const int weaponType =
			slot >= 0 ? GetVrWeaponTypeForSlot(slot) : -1;
		if(slot >= 0 && !IsImmersiveVehicleSidearm(weaponType)){
			ClearSupportForHand(hand);
			gHeldSlot[hand] = -1;
			gHolsterSelection[hand] = -1;
			gWeaponRenderSlot[hand] = -1;
			gWeaponContactSlot[hand] = -1;
			gDropped[hand] = DroppedWeapon();
		}
	}
	uint32 mask = 0;
	for(int slot = WEAPONSLOT_HANDGUN;
	    slot <= WEAPONSLOT_SUBMACHINEGUN; slot++){
		if(player->HasWeaponSlot(slot) &&
		   IsImmersiveVehicleSidearm(GetVrWeaponTypeForSlot(slot)))
			mask |= 1u << slot;
	}
	for(int hand = 0; hand < VR_HAND_COUNT; hand++){
		if(gHeldSlot[hand] >= 0)
			mask &= ~(1u << gHeldSlot[hand]);
		if(gDropped[hand].slot >= 0)
			mask &= ~(1u << gDropped[hand].slot);
	}
	gWeaponHolsterMask = mask;
}

void
ReleaseTrackedWeaponAfterUse(int hand, int slot)
{
	if(hand < 0 || hand >= VR_HAND_COUNT || gHeldSlot[hand] != slot)
		return;
	ClearSupportForHand(hand);
	gHeldSlot[hand] = -1;
	gHolsterSelection[hand] = -1;
	gWeaponRenderSlot[hand] = -1;
	gWeaponContactSlot[hand] = -1;
	gGripLatched[hand] = true;
}

void
SetTrackedWeaponRenderMatrix(int hand, int slot, int weaponType,
	const CMatrix *matrix, const CMatrix *contactMatrix)
{
	if(hand < 0 || hand >= VR_HAND_COUNT || !matrix)
		return;
	gWeaponRenderMatrix[hand] = *matrix;
	gWeaponRenderSlot[hand] = slot;
	if(contactMatrix){
		gWeaponContactMatrix[hand] = *contactMatrix;
		gWeaponContactSlot[hand] = slot;
		gWeaponContactType[hand] = weaponType;
		gWeaponContactFrame[hand] = CTimer::GetFrameCounter();
	}
}

int GetDroppedWeaponSlot(int hand) { return hand >= 0 && hand < VR_HAND_COUNT ? gDropped[hand].slot : -1; }

bool
GetDroppedWeaponMatrix(int slot, CMatrix *matrix)
{
	for(int hand = 0; hand < VR_HAND_COUNT; hand++)
		if(gDropped[hand].slot == slot){
			if(BuildDroppedMatrix(hand, matrix))
				return true;
			gDropped[hand] = DroppedWeapon();
		}
	return false;
}

void GetTrackedWeaponOffset(float *x, float *y, float *z) { GetTrackedWeaponOffsetForType(1, GetVrWeaponTypeForSlot(Max(gHeldSlot[1], 0)), x, y, z); }
void GetTrackedWeaponRotation(float *x, float *y, float *z) { GetTrackedWeaponRotationForType(1, GetVrWeaponTypeForSlot(Max(gHeldSlot[1], 0)), x, y, z); }

void
GetTrackedWeaponOffsetForType(int hand, int weaponType,
	float *x, float *y, float *z)
{
	WeaponCalibration *calibration = GetCalibration(hand, weaponType);
	if(x) *x = calibration ? (float)calibration->offsetX/200.0f : 0.0f;
	if(y) *y = calibration ? (float)calibration->offsetY/200.0f : 0.0f;
	if(z) *z = calibration ? (float)calibration->offsetZ/200.0f : 0.0f;
}

void
GetTrackedWeaponRotationForType(int hand, int weaponType,
	float *x, float *y, float *z)
{
	WeaponCalibration *calibration = GetCalibration(hand, weaponType);
	if(x) *x = calibration ? (float)calibration->rotationX/WEAPON_VALUE_SCALE : 0.0f;
	if(y) *y = calibration ? (float)calibration->rotationY/WEAPON_VALUE_SCALE : 0.0f;
	if(z) *z = calibration ? (float)calibration->rotationZ/WEAPON_VALUE_SCALE : 0.0f;
}

bool
ApplyTrackedWeaponTwoHandTransform(int hand, int weaponType,
	CMatrix *matrix)
{
	if(!matrix)
		return false;
	CVector pivot, axis;
	float angle;
	if(!BuildTwoHandRotation(hand, weaponType, &pivot, &axis, &angle))
		return false;
	matrix->GetRight() =
		RotateAroundAxis(matrix->GetRight(), axis, angle);
	matrix->GetUp() =
		RotateAroundAxis(matrix->GetUp(), axis, angle);
	matrix->GetForward() =
		RotateAroundAxis(matrix->GetForward(), axis, angle);
	matrix->GetPosition() = pivot+
		RotateAroundAxis(matrix->GetPosition()-pivot, axis, angle);
	return true;
}

bool
GetTrackedWeaponSupportAnchor(int hand, int weaponType,
	CVector *position, bool *engaged)
{
	if(!position || hand < 0 || hand >= VR_HAND_COUNT ||
	   gHeldSlot[hand] < 0)
		return false;
	CVector pivot, expected;
	if(!BuildSupportVector(hand, weaponType, &pivot, &expected))
		return false;
	const int support = gSupportHand[hand] >= 0 ?
		gSupportHand[hand] : 1-hand;
	if(gSupportHand[hand] < 0){
		if(!gPoseValid[support] || gHeldSlot[support] >= 0 ||
		   (gGripMatrix[support].GetPosition()-
		    (pivot+expected)).Magnitude() > 0.45f)
			return false;
	}else{
		CVector axis;
		float angle;
		if(BuildTwoHandRotation(hand, weaponType, &pivot, &axis, &angle))
			expected = RotateAroundAxis(expected, axis, angle);
	}
	*position = pivot+expected;
	if(engaged)
		*engaged = gSupportHand[hand] == support;
	return true;
}

bool
GetTrackedWeaponAim(int hand, int weaponType, CVector *source,
	CVector *direction)
{
	if(!BuildAim(hand, weaponType, source, direction))
		return false;
	if(IsTwoHanded(weaponType) && gSupportHand[hand] < 0){
		const float amplitude =
			GetOneHandAimSwayDegrees(weaponType)*1.5f;
		if(amplitude > 0.0f){
			CVector aimUp = gAimMatrix[hand].GetUp();
			aimUp -= *direction*DotProduct(aimUp, *direction);
			if(aimUp.MagnitudeSqr() < 0.0001f)
				aimUp = CVector(0.0f, 0.0f, 1.0f);
			aimUp.Normalise();
			CVector aimRight = CrossProduct(aimUp, *direction);
			aimRight.Normalise();
			const float time = (float)(
				CTimer::GetTimeInMillisecondsNonClipped() %
				120000U)*0.001f;
			const float phase = weaponType*0.731f+hand*2.173f;
			const float yaw = DEGTORAD(amplitude*(
				0.68f*sinf(3.35f*time+phase)+
				0.32f*sinf(8.91f*time+1.73f*phase+0.4f)));
			const float pitch = DEGTORAD(0.78f*amplitude*(
				0.62f*sinf(2.87f*time+1.31f*phase+1.2f)+
				0.38f*sinf(7.43f*time+0.77f*phase)));
			CVector swayedRight =
				RotateAroundAxis(aimRight, aimUp, yaw);
			*direction =
				RotateAroundAxis(*direction, aimUp, yaw);
			*direction =
				RotateAroundAxis(*direction, swayedRight, pitch);
			direction->Normalise();
		}
	}
	if(gScopeHand != hand || gScopeWeaponType != weaponType)
		return true;

	CVector target;
	// Never silently fall back to the controller/barrel direction while the
	// scope is visible. If a transient frame has no valid centre ray, suppress
	// that shot instead of firing somewhere outside the reticle.
	if(!BuildScopeCentreTarget(&target))
		return false;
	CVector converged = target-*source;
	if(converged.MagnitudeSqr() <= 0.0001f)
		return false;
	converged.Normalise();
	*direction = converged;
	return true;
}

bool
GetTrackedThrowableLaunch(int hand, int weaponType, CVector *source,
	CVector *velocity)
{
	CVector direction;
	if(!IsPhysicalThrowable(weaponType) ||
	   !BuildAim(hand, weaponType, source, &direction))
		return false;
	float viewRight[3], viewUp[3], viewAt[3], viewPosition[3];
	if(rw::vulkan::getFirstPersonViewFrame(
	   viewRight, viewUp, viewAt, viewPosition)){
		const CVector anchor = VectorFromArray(viewPosition);
		CVector reach = *source-anchor;
		const float requestedDistance = reach.Magnitude();
		if(requestedDistance > 0.001f){
			reach *= 1.0f/requestedDistance;
			CColPoint point;
			CEntity *hitEntity = nil;
			CEntity *savedIgnoreEntity = CWorld::pIgnoreEntity;
			CWorld::pIgnoreEntity = FindPlayerPed();
			const bool hit = CWorld::ProcessLineOfSight(anchor, *source,
				point, hitEntity, true, true, true, true, true, false,
				false, false);
			CWorld::pIgnoreEntity = savedIgnoreEntity;
			if(hit){
				const float safeDistance = Max(0.0f,
					(point.point-anchor).Magnitude()-0.08f);
				*source = anchor+reach*safeDistance;
			}
		}
	}
	direction.Normalise();
	*velocity = direction*0.46f;
	return true;
}

void SetTrackedThrowablePreviewActive(int hand, bool active) { if(hand >= 0 && hand < VR_HAND_COUNT) gThrowablePreview[hand] = active; }
bool IsTrackedThrowablePreviewActive(int hand)
{
	return hand >= 0 && hand < VR_HAND_COUNT &&
		gThrowablePreview[hand] && gTriggerPressed[hand];
}

void
BeginTrackedWeaponFire(int hand, int weaponType, const CVector &source,
	const CVector &direction)
{
	gActiveFire = true;
	gActiveFireHand = hand;
	// 0.4.1 PC parity: controller kick on every shot.
	androidgame::TriggerWeaponHaptic(hand, 0.8f);
	gActiveFireWeaponType = weaponType;
	gActiveFireSource = source;
	gActiveFireDirection = direction;
	if(IsPhysicalThrowable(weaponType)){
		gActiveThrowable = GetTrackedThrowableLaunch(hand, weaponType,
			&gActiveThrowableSource, &gActiveThrowableVelocity);
	}else
		gActiveThrowable = false;
}

void
EndTrackedWeaponFire()
{
	gActiveFire = false;
	gActiveFireHand = -1;
	gActiveFireWeaponType = -1;
	gActiveThrowable = false;
}

bool
GetActiveTrackedWeaponAim(CVector *source, CVector *direction)
{
	if(!gActiveFire || !source || !direction)
		return false;
	*source = gActiveFireSource;
	*direction = gActiveFireDirection;
	return true;
}

// The aim ray of whichever tracked weapon the player is holding, in world
// space, or false when nothing is in hand. Worked out once a frame and kept:
// the threat scan asks for it from every ped that might be looking down the
// barrel, and none of them should pay for it twice.
bool
GetHeldTrackedWeaponAim(CVector *source, CVector *direction)
{
	static uint32 computedFrame = ~0u;
	static bool held;
	static CVector heldSource, heldDirection;

	const uint32 frame = CTimer::GetFrameCounter();
	if(frame != computedFrame){
		computedFrame = frame;
		held = false;
		CPlayerPed *player = FindPlayerPed();
		for(int hand = 0; player != nil && hand < 2 && !held; hand++){
			if(!IsTrackedWeaponHeld(hand))
				continue;
			const int slot = GetHeldWeaponSlot(hand);
			if(slot < 0 || slot >= TOTAL_WEAPON_SLOTS)
				continue;
			CWeapon *weapon = &player->GetWeapon(slot);
			// The same list the laser draws for: something with a barrel,
			// pointed where the hand points. A camera is not a threat and
			// neither is a fist.
			if(weapon == nil ||
			   !IsPhysicalGunType(weapon->m_eWeaponType) ||
			   weapon->m_eWeaponType == WEAPONTYPE_CAMERA)
				continue;
			if(!GetTrackedWeaponAim(hand, weapon->m_eWeaponType,
			   &heldSource, &heldDirection))
				continue;
			heldDirection.Normalise();
			held = true;
		}
	}
	if(!held || source == nil || direction == nil)
		return false;
	*source = heldSource;
	*direction = heldDirection;
	return true;
}

bool
GetActiveTrackedThrowableLaunch(CVector *source, CVector *velocity)
{
	if(!gActiveFire || !gActiveThrowable || !source || !velocity)
		return false;
	*source = gActiveThrowableSource;
	*velocity = gActiveThrowableVelocity;
	return true;
}

bool
GetQuestRawTrackedHandMatrix(int hand, CMatrix *matrix,
	float *grip, float *trigger)
{
	if(!matrix || hand < 0 || hand >= VR_HAND_COUNT || !gPoseValid[hand])
		return false;
	*matrix = gGripMatrix[hand];
	if(grip) *grip = gGrip[hand];
	if(trigger) *trigger = gTrigger[hand];
	return true;
}

void
RefreshQuestTrackedHandWorldPosesForRender()
{
	if(!gVrFirstPersonActive)
		return;

	const androidgame::PadInput &input = androidgame::GetPadInput();
	for(int hand = 0; hand < VR_HAND_COUNT; hand++){
		// ApplyTouchInput sampled this pose before CWorld::Process. The player
		// or vehicle can move substantially during that process, while the
		// first-person anchor is intentionally refreshed afterwards. Convert
		// the same OpenXR sample against that final anchor so the rendered
		// hands and weapons share the camera's simulation frame.
		CMatrix gripMatrix;
		if(!gPoseValid[hand] ||
		   !PoseToMatrix(input.gripPose[hand], &gripMatrix))
			continue;

		CMatrix aimMatrix;
		const bool aimValid =
			PoseToMatrix(input.aimPose[hand], &aimMatrix);
		gGripMatrix[hand] = gripMatrix;
		gAimMatrix[hand] = aimValid ? aimMatrix : gripMatrix;
	}
}

bool
GetQuestRawTrackedHandAimMatrix(int hand, CMatrix *matrix)
{
	if(!matrix || hand < 0 || hand >= VR_HAND_COUNT || !gPoseValid[hand])
		return false;
	*matrix = gAimMatrix[hand];
	return true;
}

static bool
ApplyTwoHandVisualHandLock(int hand, CMatrix *handMatrix,
	CVector *aimOrigin, CVector *aimDirection, float *grip, float *trigger)
{
	if(hand < 0 || hand >= VR_HAND_COUNT)
		return false;
	int primary = -1;
	int support = -1;
	int primarySlot = -1;
	int weaponType = -1;
	for(int candidate = 0; candidate < VR_HAND_COUNT; candidate++){
		const int slot = gHeldSlot[candidate];
		const int candidateSupport = gSupportHand[candidate];
		if(slot < 0 || candidateSupport < 0 ||
		   candidateSupport >= VR_HAND_COUNT)
			continue;
		const int candidateType = GetVrWeaponTypeForSlot(slot);
		if(!IsTwoHanded(candidateType) ||
		   (hand != candidate && hand != candidateSupport))
			continue;
		primary = candidate;
		support = candidateSupport;
		primarySlot = slot;
		weaponType = candidateType;
		break;
	}
	if(primary < 0)
		return false;

	CVector pivot, axis;
	float angle;
	if(!BuildTwoHandRotation(primary, weaponType, &pivot, &axis, &angle))
		return false;
	if(hand == primary){
		if(handMatrix && angle != 0.0f){
			handMatrix->GetRight() = RotateAroundAxis(
				handMatrix->GetRight(), axis, angle);
			handMatrix->GetUp() = RotateAroundAxis(
				handMatrix->GetUp(), axis, angle);
			handMatrix->GetForward() = RotateAroundAxis(
				handMatrix->GetForward(), axis, angle);
			handMatrix->GetPosition() = pivot+RotateAroundAxis(
				handMatrix->GetPosition()-pivot, axis, angle);
		}
		if(aimOrigin && angle != 0.0f)
			*aimOrigin = pivot+RotateAroundAxis(*aimOrigin-pivot, axis, angle);
		if(aimDirection && angle != 0.0f){
			*aimDirection = RotateAroundAxis(*aimDirection, axis, angle);
			aimDirection->Normalise();
		}
		return true;
	}

	CVector rawPivot, weaponRight, weaponUp, weaponForward, expected;
	if(hand != support || !BuildSupportVector(primary, weaponType,
	   &rawPivot, &expected, &weaponRight, &weaponUp, &weaponForward))
		return false;
	if(angle != 0.0f){
		expected = RotateAroundAxis(expected, axis, angle);
		weaponRight = RotateAroundAxis(weaponRight, axis, angle);
		weaponUp = RotateAroundAxis(weaponUp, axis, angle);
		weaponForward = RotateAroundAxis(weaponForward, axis, angle);
	}
	const CVector lockedSocket = pivot+expected;
	WeaponCalibration *calibration = GetCalibration(primary, weaponType);
	if(calibration &&
	   calibration->supportPoseVersion >= SUPPORT_GRIP_POSE_VERSION &&
	   gWeaponContactSlot[primary] == primarySlot &&
	   gWeaponContactType[primary] == weaponType &&
	   gWeaponContactFrame[primary] == CTimer::GetFrameCounter()){
		CVector barrel = gWeaponContactMatrix[primary].GetRight();
		CVector top = gWeaponContactMatrix[primary].GetForward();
		CVector lateral;
		if(BuildCanonicalSupportFrame(barrel, top,
		   &lateral, &top, &barrel)){
			weaponRight = barrel;
			weaponUp = lateral*-1.0f;
			weaponForward = top*-1.0f;
		}
	}
	if(!BuildCanonicalSupportFrame(weaponForward, weaponUp,
	   &weaponRight, &weaponUp, &weaponForward))
		return false;
	const int gripType = calibration ? calibration->supportGripType :
		SUPPORT_GRIP_MAGAZINE;
	const float palmSign = support == 0 ? -1.0f : 1.0f;
	CVector encodedPalmAxis = gripType == SUPPORT_GRIP_FROM_BELOW ?
		weaponUp*(-palmSign) : weaponRight;
	encodedPalmAxis.Normalise();
	CVector handUp = encodedPalmAxis*palmSign;
	CVector handForward = weaponForward;
	handForward.Normalise();
	CVector handRight = CrossProduct(handUp, handForward);
	if(handRight.MagnitudeSqr() < 0.0001f)
		return false;
	handRight.Normalise();
	const float rotationX = DEGTORAD((float)(calibration ?
		calibration->supportRotationX : 0)/WEAPON_VALUE_SCALE);
	const float rotationY = DEGTORAD((float)(calibration ?
		calibration->supportRotationY : 0)/WEAPON_VALUE_SCALE);
	const float rotationZ = DEGTORAD((float)(calibration ?
		calibration->supportRotationZ : 0)/WEAPON_VALUE_SCALE);
	if(rotationX != 0.0f){
		handUp = RotateAroundAxis(handUp, handRight, rotationX);
		handForward = RotateAroundAxis(handForward, handRight, rotationX);
	}
	if(rotationY != 0.0f){
		handRight = RotateAroundAxis(handRight, handUp, rotationY);
		handForward = RotateAroundAxis(handForward, handUp, rotationY);
	}
	if(rotationZ != 0.0f){
		handRight = RotateAroundAxis(handRight, handForward, rotationZ);
		handUp = RotateAroundAxis(handUp, handForward, rotationZ);
	}
	handForward.Normalise();
	handUp -= handForward*DotProduct(handUp, handForward);
	if(handUp.MagnitudeSqr() < 0.0001f)
		return false;
	handUp.Normalise();
	handRight = CrossProduct(handUp, handForward);
	handRight.Normalise();
	// The UltimateXR LEFT mesh was canonicalised during conversion and needs
	// this one visual roll; RIGHT is already in the authored support basis.
	if(support == 0){
		handUp *= -1.0f;
		handRight *= -1.0f;
	}
	encodedPalmAxis = handUp*palmSign;
	if(handMatrix){
		handMatrix->GetRight() = encodedPalmAxis;
		handMatrix->GetUp() = handForward;
		handMatrix->GetForward() = handRight*(support == 0 ? -1.0f : 1.0f);
		handMatrix->GetPosition() = lockedSocket;
	}
	if(aimOrigin)
		*aimOrigin = lockedSocket;
	if(aimDirection)
		*aimDirection = handForward;
	if(grip)
		*grip = gripType == SUPPORT_GRIP_FROM_BELOW ? 0.55f : 1.0f;
	if(trigger)
		*trigger = gripType == SUPPORT_GRIP_FROM_BELOW ? 0.65f : 1.0f;
	return true;
}

bool
GetTrackedHandMatrix(int hand, CMatrix *matrix, float *grip, float *trigger)
{
	if(!matrix || hand < 0 || hand >= VR_HAND_COUNT || !gPoseValid[hand])
		return false;
	if(IsImmersiveCarDrivingActive() &&
	   IsImmersiveSteeringHandleGrabbed(hand) &&
	   GetImmersiveSteeringHandleMatrix(hand, matrix)){
		// The interaction anchor remains exactly on the authoritative rim. Move
		// only the rendered palm a little outward, then close every finger around
		// the rim; the marker, radius and steering maths are deliberately unchanged.
		const float side = hand == 0 ? -1.0f : 1.0f;
		matrix->GetPosition() += matrix->GetRight()*(side*0.025f);
		const float wheelGrip = Max(gGrip[hand], 0.90f);
		if(grip) *grip = wheelGrip;
		if(trigger) *trigger = Max(gTrigger[hand], wheelGrip);
		return true;
	}
	if(IsImmersiveSteeringHandleGrabbed(hand) &&
	   GetImmersiveSteeringHandleMatrix(hand, matrix)){
		if(grip) *grip = gGrip[hand];
		if(trigger) *trigger = gTrigger[hand];
		return true;
	}
	return GetQuestRawTrackedHandMatrix(hand, matrix, grip, trigger);
}

static void
ApplyBikeVisualGripAlignment(int hand, CMatrix *matrix)
{
	if(!matrix || hand < 0 || hand >= VR_HAND_COUNT)
		return;
	// The calibrated socket axis runs along the cylindrical handlebar grip, but
	// the rendered fingers must wrap across it. Convert only the visual wrist;
	// interaction sockets, steering and saved calibration remain authoritative.
	const float angle = DEGTORAD(-90.0f);
	matrix->GetForward() = RotateAroundAxis(matrix->GetForward(),
		matrix->GetRight(), angle);
	matrix->GetUp() = RotateAroundAxis(matrix->GetUp(),
		matrix->GetRight(), angle);
	matrix->GetForward().Normalise();
	matrix->GetUp().Normalise();
	// A round grip has no intrinsic forward hemisphere. Resolve that ambiguity
	// deterministically against the bike so tiny pose noise cannot flip the hand
	// by 180 degrees from one frame to the next.
	CVehicle *vehicle = FindPlayerVehicle();
	if(vehicle && vehicle->IsBike() &&
	   DotProduct(matrix->GetForward(), vehicle->GetForward()) < 0.0f){
		matrix->GetForward() *= -1.0f;
		matrix->GetUp() *= -1.0f;
	}
}

bool
GetTrackedVisualHandMatrix(int hand, CMatrix *matrix, float *grip,
	float *trigger)
{
	if(!GetTrackedHandMatrix(hand, matrix, grip, trigger))
		return false;
	if(IsImmersiveBikeHandleGrabbed(hand))
		ApplyBikeVisualGripAlignment(hand, matrix);
	ApplyTwoHandVisualHandLock(hand, matrix, nil, nil, grip, trigger);
	// Last, over whatever produced the pose: the rendered hand only, never
	// the anchor the wheel maths and the grab test run against.
	ApplyQuestWheelHandPullBack(hand, matrix);
	return true;
}

bool
GetTrackedHandAimRay(int hand, CVector *origin, CVector *direction)
{
	if(!origin || !direction || hand < 0 ||
	   hand >= VR_HAND_COUNT || !gPoseValid[hand])
		return false;
	CMatrix handle;
	if(IsImmersiveSteeringHandleGrabbed(hand) &&
	   GetImmersiveSteeringHandleMatrix(hand, &handle)){
		*origin = handle.GetPosition();
		*direction = handle.GetForward();
		direction->Normalise();
		return true;
	}
	*origin = gAimMatrix[hand].GetPosition();
	*direction = gAimMatrix[hand].GetForward();
	direction->Normalise();
	return true;
}

bool
GetTrackedVisualHandAimRay(int hand, CVector *origin, CVector *direction)
{
	if(!origin || !direction)
		return false;
	if(IsImmersiveBikeHandleGrabbed(hand)){
		CMatrix visualHandle;
		if(GetImmersiveSteeringHandleMatrix(hand, &visualHandle)){
			ApplyBikeVisualGripAlignment(hand, &visualHandle);
			*origin = visualHandle.GetPosition();
			*direction = visualHandle.GetForward();
			direction->Normalise();
			return true;
		}
	}
	if(!GetTrackedHandAimRay(hand, origin, direction))
		return false;
	ApplyTwoHandVisualHandLock(hand, nil, origin, direction, nil, nil);
	return true;
}

bool ConsumePhysicalMeleeStrike(int hand, int *slot, int *weaponType,
	CVector *sweepStart, CVector *sweepEnd, float *speed,
	CVector *rootStart, CVector *rootEnd)
{
	if(hand < 0 || hand >= VR_HAND_COUNT || !slot || !weaponType ||
	   !sweepStart || !sweepEnd || !gMeleeStrike[hand].pending)
		return false;
	MeleeStrike &strike = gMeleeStrike[hand];
	if(CTimer::GetFrameCounter()-strike.frame > 1U){
		strike.pending = false;
		return false;
	}
	*slot = strike.slot;
	*weaponType = strike.weaponType;
	*sweepStart = strike.tipStart;
	*sweepEnd = strike.tipEnd;
	if(rootStart) *rootStart = strike.rootStart;
	if(rootEnd) *rootEnd = strike.rootEnd;
	if(speed) *speed = strike.speed;
	strike.pending = false;
	return true;
}

void
ResolvePhysicalMeleeStrike(int hand, bool contact)
{
	if(!contact || hand < 0 || hand >= VR_HAND_COUNT)
		return;
	// A miss leaves the swing live so its continuation segments can still
	// reach the target; a real contact latches until the hand slows down.
	gMeleeMotion[hand].armed = false;
	gMeleeMotion[hand].strikeInProgress = false;
	gMeleeMotion[hand].strikePeakSpeed = 0.0f;
	gMeleeMotion[hand].strikeContinueUntil = 0;
	gMeleeMotion[hand].lastStrikeTime =
		CTimer::GetTimeInMillisecondsNonClipped();
	gMeleeMotion[hand].calmSince = 0;
}

int
GetQuestWeaponSettingCount()
{
	return 6;
}

const char *
GetQuestWeaponSettingName(int setting)
{
	static const char *names[] = {
		"VR HANDS", "WEAPON LASER", "HOLSTER HIGHLIGHTS",
		"MANUAL RELOADING", "PHYSICAL SCOPE AIM",
		"WEAPON GRIP LOCK"
	};
	return setting >= 0 && setting < (int)ARRAY_SIZE(names) ?
		names[setting] : "UNKNOWN";
}

bool
GetQuestWeaponSetting(int setting)
{
	LoadSettings();
	switch(setting){
	case 0: return gHandsEnabled;
	case 1: return gWeaponLaser;
	case 2: return gHolsterHighlights;
	case 3: return gManualReload;
	case 4: return gScopeAim;
	case 5: return gGripLock;
	default: return false;
	}
}

void
ToggleQuestWeaponSetting(int setting)
{
	LoadSettings();
	bool *value = nil;
	const char *key = nil;
	switch(setting){
	case 0: value = &gHandsEnabled; key = "VrHands"; break;
	case 1: value = &gWeaponLaser; key = "WeaponLaser"; break;
	case 2: value = &gHolsterHighlights; key = "HolsterHighlights"; break;
	case 3: value = &gManualReload; key = "ManualReloading"; break;
	case 4: value = &gScopeAim; key = "PhysicalScopeAim"; break;
	case 5: value = &gGripLock; key = "WeaponGripLock"; break;
	default: return;
	}
	*value = !*value;
	WritePrivateProfileStringA("VR", key, *value ? "1" : "0",
		kSettingsPath);
	if(setting == 3 && !gManualReload)
		for(int hand = 0; hand < VR_HAND_COUNT; hand++)
			ResetManualReloadState(gManualReloadState[hand]);
	if(setting == 4 && !gScopeAim &&
	   gScopeWeaponType != WEAPONTYPE_CAMERA){
		gScopeHand = -1;
		gScopeWeaponType = -1;
	}
}

int
GetQuestCalibrationWeaponType(int hand)
{
	if(hand >= 0 && hand < VR_HAND_COUNT && gHeldSlot[hand] >= 0)
		return GetVrWeaponTypeForSlot(gHeldSlot[hand]);
	for(int other = 0; other < VR_HAND_COUNT; other++)
		if(gHeldSlot[other] >= 0)
			return GetVrWeaponTypeForSlot(gHeldSlot[other]);
	CPlayerPed *player = FindPlayerPed();
	return player ? player->GetWeapon()->m_eWeaponType :
		WEAPONTYPE_UNARMED;
}

static int *
QuestCalibrationValue(WeaponCalibration *calibration, int item,
	int *minimum, int *maximum)
{
	if(!calibration || !minimum || !maximum)
		return nil;
	*minimum = -100;
	*maximum = 100;
	switch(item){
	case 0: return &calibration->aimOffsetX;
	case 1: return &calibration->aimOffsetY;
	case 2: return &calibration->aimOffsetZ;
	case 3:
		*minimum = -360; *maximum = 360;
		return &calibration->aimRotationX;
	case 4:
		*minimum = -360; *maximum = 360;
		return &calibration->aimRotationY;
	case 5:
		*minimum = -360; *maximum = 360;
		return &calibration->aimRotationZ;
	case 6: return &calibration->offsetX;
	case 7: return &calibration->offsetY;
	case 8: return &calibration->offsetZ;
	case 9:
		*minimum = -360; *maximum = 360;
		return &calibration->rotationX;
	case 10:
		*minimum = -360; *maximum = 360;
		return &calibration->rotationY;
	case 11:
		*minimum = -360; *maximum = 360;
		return &calibration->rotationZ;
	case 12:
		*minimum = -200; *maximum = 200;
		return &calibration->supportX;
	case 13:
		*minimum = -200; *maximum = 200;
		return &calibration->supportY;
	case 14:
		*minimum = -200; *maximum = 200;
		return &calibration->supportZ;
	case 15:
		*minimum = -360; *maximum = 360;
		return &calibration->supportRotationX;
	case 16:
		*minimum = -360; *maximum = 360;
		return &calibration->supportRotationY;
	case 17:
		*minimum = -360; *maximum = 360;
		return &calibration->supportRotationZ;
	case 18:
		*minimum = 0; *maximum = SUPPORT_GRIP_TYPE_COUNT-1;
		return &calibration->supportGripType;
	default: return nil;
	}
}

static void
WriteCalibrationInt(const char *section, int hand, const char *name,
	int value)
{
	char key[64], text[24];
	sprintf(text, "%d", value);
	WritePrivateProfileStringA(section,
		CalibrationKey(hand, name, key), text, kSettingsPath);
}

static void
SaveQuestCalibration(int hand, int weaponType,
	const WeaponCalibration &calibration)
{
	char section[96];
	sprintf(section, ModelSets::GetActiveForCategory(
		ModelSets::MODEL_CATEGORY_WEAPONS) == ModelSets::MODEL_SET_MODERN ?
		"VRWeaponModern_%02d_%s" : "VRWeapon_%02d_%s", weaponType,
		GetVrWeaponName(weaponType));
	WriteCalibrationInt(section, hand, "Configured", 1);
	WriteCalibrationInt(section, hand, "ValueScale", WEAPON_VALUE_SCALE);
	WriteCalibrationInt(section, hand, "OffsetX", calibration.offsetX);
	WriteCalibrationInt(section, hand, "OffsetY", calibration.offsetY);
	WriteCalibrationInt(section, hand, "OffsetZ", calibration.offsetZ);
	WriteCalibrationInt(section, hand, "AimOffsetX",
		calibration.aimOffsetX);
	WriteCalibrationInt(section, hand, "AimOffsetY",
		calibration.aimOffsetY);
	WriteCalibrationInt(section, hand, "AimOffsetZ",
		calibration.aimOffsetZ);
	WriteCalibrationInt(section, hand, "AimRotationX",
		calibration.aimRotationX);
	WriteCalibrationInt(section, hand, "AimRotationY",
		calibration.aimRotationY);
	WriteCalibrationInt(section, hand, "AimRotationZ",
		calibration.aimRotationZ);
	WriteCalibrationInt(section, hand, "RotationX",
		calibration.rotationX);
	WriteCalibrationInt(section, hand, "RotationY",
		calibration.rotationY);
	WriteCalibrationInt(section, hand, "RotationZ",
		calibration.rotationZ);
	WriteCalibrationInt(section, hand, "SupportGripConfigured", 1);
	WriteCalibrationInt(section, hand, "SupportGripValueScale",
		WEAPON_VALUE_SCALE);
	WriteCalibrationInt(section, hand, "SupportGripOffsetX",
		calibration.supportX);
	WriteCalibrationInt(section, hand, "SupportGripOffsetY",
		calibration.supportY);
	WriteCalibrationInt(section, hand, "SupportGripOffsetZ",
		calibration.supportZ);
	WriteCalibrationInt(section, hand, "SupportGripRotationX",
		calibration.supportRotationX);
	WriteCalibrationInt(section, hand, "SupportGripRotationY",
		calibration.supportRotationY);
	WriteCalibrationInt(section, hand, "SupportGripRotationZ",
		calibration.supportRotationZ);
	WriteCalibrationInt(section, hand, "SupportGripPoseVersion",
		SUPPORT_GRIP_POSE_VERSION);
	char style[16];
	sprintf(style, "%d", calibration.supportGripType);
	WritePrivateProfileStringA(section, "SupportGripStyle", style,
		kSettingsPath);
}

int
GetQuestCalibrationValue(int hand, int weaponType, int item)
{
	int minimum, maximum;
	int *value = QuestCalibrationValue(
		GetCalibration(hand, weaponType), item, &minimum, &maximum);
	return value ? *value : 0;
}

void
AdjustQuestCalibrationValue(int hand, int weaponType, int item,
	int direction)
{
	if(direction == 0)
		return;
	WeaponCalibration *calibration = GetCalibration(hand, weaponType);
	int minimum, maximum;
	int *value = QuestCalibrationValue(calibration, item,
		&minimum, &maximum);
	if(!value)
		return;
	*value = clamp(*value+(direction < 0 ? -1 : 1),
		minimum, maximum);
	// The underhand grip starts palm down, which points the fingers at the
	// floor instead of around the grip. Seed the roll the first time the
	// style is picked; a wrist that has already been rolled keeps its own.
	if(item == 18 && *value == SUPPORT_GRIP_FROM_BELOW &&
	   calibration->supportRotationZ == 0)
		calibration->supportRotationZ =
			SUPPORT_GRIP_FROM_BELOW_ROLL_HALF_DEG;
	if(item >= 12)
		calibration->supportPoseVersion = SUPPORT_GRIP_POSE_VERSION;
	SaveQuestCalibration(hand, weaponType, *calibration);
}

int
GetQuestHolsterPointCount()
{
	return HOLSTER_POINT_COUNT;
}

const char *
GetQuestHolsterPointName(int point)
{
	static const char *names[HOLSTER_POINT_COUNT] = {
		"WAIST LEFT", "WAIST RIGHT", "CHEST LEFT", "CHEST RIGHT",
		"CHEST CENTER THROWABLE", "BACK LEFT", "BACK RIGHT"
	};
	return point >= 0 && point < HOLSTER_POINT_COUNT ?
		names[point] : "UNKNOWN";
}

int
GetQuestHolsterPointSlot(int point)
{
	LoadSettings();
	return point >= 0 && point < HOLSTER_POINT_COUNT ?
		gHolsterPointSlot[point] : -1;
}

void
CycleQuestHolsterPointSlot(int point, int direction)
{
	LoadSettings();
	if(point < 0 || point >= HOLSTER_POINT_COUNT || direction == 0 ||
	   point == HOLSTER_CHEST_CENTRE)
		return;
	static const int choices[] = {
		-1, WEAPONSLOT_MELEE, WEAPONSLOT_HANDGUN,
		WEAPONSLOT_SHOTGUN, WEAPONSLOT_SUBMACHINEGUN,
		WEAPONSLOT_RIFLE, WEAPONSLOT_HEAVY, WEAPONSLOT_SNIPER,
		WEAPONSLOT_OTHER
	};
	int choice = 0;
	for(int index = 0; index < (int)ARRAY_SIZE(choices); index++)
		if(choices[index] == gHolsterPointSlot[point])
			choice = index;
	for(int attempts = 0; attempts < (int)ARRAY_SIZE(choices);
	    attempts++){
		choice = (choice+(direction < 0 ? -1 : 1)+
			(int)ARRAY_SIZE(choices))%(int)ARRAY_SIZE(choices);
		const int candidate = choices[choice];
		bool used = false;
		if(candidate >= 0)
			for(int other = 0; other < HOLSTER_POINT_COUNT; other++)
				if(other != point &&
				   gHolsterPointSlot[other] == candidate)
					used = true;
		if(!used){
			gHolsterPointSlot[point] = candidate;
			break;
		}
	}
	static const char *keys[HOLSTER_POINT_COUNT] = {
		"HolsterWaistLeftSlot", "HolsterWaistRightSlot",
		"HolsterChestLeftSlot", "HolsterChestRightSlot", nil,
		"HolsterBackLeftSlot", "HolsterBackRightSlot"
	};
	if(keys[point]){
		char value[16];
		sprintf(value, "%d", gHolsterPointSlot[point]);
		WritePrivateProfileStringA("VR", keys[point], value,
			kSettingsPath);
	}
}

bool CanSkipDesktopGameplayRender() { return IsPhysicalWeaponInteractionActive(); }
void ToggleCheatMenu() {}

void
PrepareForGameShutdown()
{
	InvalidateQuestWeaponLaserOverrides();
	for(int hand = 0; hand < VR_HAND_COUNT; hand++){
		gPoseValid[hand] = false;
		gGrip[hand] = 0.0f;
		gTrigger[hand] = 0.0f;
		gTriggerPressed[hand] = false;
		gTriggerJustPressed[hand] = false;
		gTriggerJustReleased[hand] = false;
		gGripLatched[hand] = false;
		gHeldSlot[hand] = -1;
		gHolsterSelection[hand] = -1;
		gSupportHand[hand] = -1;
		gWeaponRenderSlot[hand] = -1;
		gWeaponContactSlot[hand] = -1;
		gWeaponContactType[hand] = -1;
		gWeaponContactFrame[hand] = 0;
		gDropped[hand] = DroppedWeapon();
		ResetManualReloadState(gManualReloadState[hand]);
		gManualReloadGripDown[hand] = false;
		gMeleeMotion[hand] = MeleeMotion();
		gMeleeStrike[hand] = MeleeStrike();
		gThrowablePreview[hand] = false;
		gTrackedDetonatorWasActive[hand] = false;
		gTrackedDetonatorWaitForRelease[hand] = false;
		gTrackedDetonatorJustPressed[hand] = false;
	}
	gWeaponHolsterMask = 0;
	gAttachedMissionWeaponForced = false;
	gAttachedMissionWeaponSlot = -1;
	gTrackedDetonatorHand = -1;
	gActiveFire = false;
	gActiveFireHand = -1;
	gActiveFireWeaponType = -1;
	gActiveThrowable = false;
	gScopeHand = -1;
	gScopeWeaponType = -1;
	gScopeCandidateHand = -1;
	gScopeCandidateType = -1;
	gBodyForward = CVector(0.0f, 1.0f, 0.0f);
	gBodyForwardValid = false;
	VRHandModel::Shutdown();
}

} // namespace OculusVR

#endif
