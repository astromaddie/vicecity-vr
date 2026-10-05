#!/usr/bin/env python3
"""Binding table sanity for the Steam Frame build (standard.md section 10).

Two mistakes have cost earlier Frame ports a whole wear session:
  - one component path a profile lacks makes the runtime refuse that whole
    profile's suggestion (XR_ERROR_PATH_UNSUPPORTED), so it gets no bindings;
  - two actions sharing a name or localized name make xrCreateAction fail
    (XR_ERROR_LOCALIZED_NAME_DUPLICATED), and the game gets no input at all.

Reads overlay/android/app/src/main/cpp/xr_vulkan_session.cpp: the binding
tables frameBindings (Steam Frame), indexBindings (Valve Index) and bindings
(Quest Touch), and every makeAction call. Component lists are the OpenXR
spec's, and Valve's for the Frame (as Halo CE VR, F-Zero X and GoldenEye VR
bind it).

It also checks the pad layout for vehicles (overlay/src/skel/android/android.cpp
and overlay/src/vr/QuestDrivingVR.cpp): on the Frame a grip is an action/interact
button, so in a vehicle it must drive the Action control (L1) or nothing, never
the handbrake (R1 in the shipped pad modes), and the handbrake stays on A.

Run: tools/steam_frame/tests/check_bindings.py [xr_vulkan_session.cpp [android.cpp [QuestDrivingVR.cpp]]]
Exit 1 on a bad path, a duplicate name, a Frame input the game needs that
is not bound, or a vehicle layout that puts the handbrake on a grip.
"""
import re
import sys
from pathlib import Path

KIT = Path(__file__).resolve().parents[3]
SRC = KIT / "overlay/android/app/src/main/cpp/xr_vulkan_session.cpp"
PAD_SRC = KIT / "overlay/src/skel/android/android.cpp"
DRIVING_SRC = KIT / "overlay/src/vr/QuestDrivingVR.cpp"

POSES = {"grip/pose", "aim/pose"}
HAPTIC = {"output/haptic"}
STICK = {"thumbstick", "thumbstick/x", "thumbstick/y", "thumbstick/click", "thumbstick/touch"}

PROFILES = {
    "touch": ({"left", "right"},
              {"x/click", "x/touch", "y/click", "y/touch", "menu/click", "a/click", "a/touch", "b/click",
               "b/touch", "squeeze/value", "trigger/value", "trigger/touch", "thumbrest/touch"}
              | STICK | POSES | HAPTIC),
    "index": ({"left", "right"},
              {"system/click", "system/touch", "a/click", "a/touch", "b/click", "b/touch", "squeeze/value",
               "squeeze/force", "trigger/click", "trigger/value", "trigger/touch", "trackpad", "trackpad/x",
               "trackpad/y", "trackpad/touch", "trackpad/force"} | STICK | POSES | HAPTIC),
    "frame": ({"left", "right"},
              {"a/click", "b/click", "x/click", "y/click", "menu/click", "view/click", "bumper/click",
               "trigger/value", "trigger/click", "squeeze/value", "dpad_up/click", "dpad_down/click",
               "dpad_left/click", "dpad_right/click"} | STICK | POSES | HAPTIC),
}
# Touch-only components have a hand; so do the Frame's (a split gamepad).
HAND_ONLY = {
    "touch": {"x/click": "left", "y/click": "left", "menu/click": "left", "a/click": "right", "b/click": "right"},
    "frame": {"a/click": "right", "b/click": "right", "x/click": "right", "y/click": "right",
              "menu/click": "right", "view/click": "left", "dpad_up/click": "left", "dpad_down/click": "left",
              "dpad_left/click": "left", "dpad_right/click": "left"},
}
TABLES = {"frameBindings": "frame", "indexBindings": "index", "bindings": "touch"}
# Every action the game reads must be reachable on the Frame.
FRAME_NEEDS = ["g.gripPoseAction", "g.aimPoseAction", "g.hapticAction", "g.thumbstickAction", "g.triggerAction",
               "g.squeezeAction", "g.thumbClickAction", "g.aAction", "g.bAction", "g.xAction", "g.yAction",
               "g.menuAction", "g.viewAction", "g.dpadAction[0]", "g.dpadAction[1]", "g.dpadAction[2]",
               "g.dpadAction[3]", "g.bumperAction"]
# The system layer needs Menu and View on every fallback too.
SYSTEM_NEEDS = ["g.menuAction", "g.viewAction"]


def tables(text):
    for m in re.finditer(r"const Binding (\w+)\[\] = \{(.*?)\n\t\};", text, re.S):
        yield m.group(1), m.group(2)


def vehicle_grips(pad_text, driving_text):
    """The Frame's vehicle pad layout: grips are Action (L1) or nothing, A is the handbrake."""
    bad = 0
    m = re.search(r"static int\s+VrVehiclePadTarget\(int source, int padMode\)\s*\{(.*?)\n\}", pad_text, re.S)
    if not m:
        print("FAIL: VrVehiclePadTarget not found in android.cpp")
        return 1
    body = m.group(1)
    frame = body.split("#if MIAMIVR_STEAM_FRAME", 1)[-1].split("#else", 1)[0]
    for grip in ("VR_PAD_SOURCE_LEFT_GRIP", "VR_PAD_SOURCE_RIGHT_GRIP"):
        if grip not in frame:
            print(f"FAIL: VrVehiclePadTarget: {grip} is not handled on the Frame")
            bad += 1
    targets = set(re.findall(r"VR_PAD_TARGET_(\w+)", frame))
    if targets - {"L1", "NONE"} or "L1" not in targets:
        print(f"FAIL: VrVehiclePadTarget: a grip may drive only Action (L1) or nothing, found {sorted(targets)}")
        bad += 1
    if not re.search(r"padMode == 0 \|\| padMode == 1", frame):
        print("FAIL: VrVehiclePadTarget: L1 is a vehicle control in pad modes 2 and 3; grips must be none there")
        bad += 1
    apply_ = re.search(r"VrApplyPadBindings\(CControllerState \*state.*?\n\}", pad_text, re.S)
    if not apply_ or "VrVehiclePadTarget(source, padMode)" not in apply_.group(0) or \
       "VrPadBindingDefault" in apply_.group(0):
        print("FAIL: VrApplyPadBindings must route vehicles through VrVehiclePadTarget")
        bad += 1
    if not re.search(r"MapHandbrakeToPad\(state,[^;]*input\.a\)", driving_text):
        print("FAIL: the handbrake is no longer driven by A (QuestDrivingVR.cpp)")
        bad += 1
    print("vehicle pad layout: grips = Action (L1) or nothing, handbrake on A" if not bad else "")
    return bad


def main():
    text = (Path(sys.argv[1]) if len(sys.argv) > 1 else SRC).read_text()
    bad = 0

    names, localized = {}, {}
    for m in re.finditer(r'makeAction\("([^"]+)",\s*"([^"]+)"', text):
        for seen, key in ((names, m.group(1)), (localized, m.group(2))):
            if key in seen:
                print(f"FAIL: duplicate action name {key!r}")
                bad += 1
            seen[key] = True
    print(f"{len(names)} actions, names and localized names unique" if not bad else "")

    found = set()
    for table, body in tables(text):
        profile = TABLES.get(table)
        if profile is None:
            continue
        found.add(table)
        hands, comps = PROFILES[profile]
        actions = set()
        for line in body.splitlines():
            m = re.search(r'\{\s*(g\.\w+(?:\[\d\])?),\s*"/user/hand/(\w+)/(input|output)/([\w/]+)"', line)
            if not m:
                continue
            action, hand, kind, comp = m.groups()
            comp = comp if kind == "input" else "output/" + comp
            actions.add(action)
            if hand not in hands or comp not in comps:
                print(f"FAIL: {table}: /user/hand/{hand}/{kind}/{m.group(4)} is not a {profile} component")
                bad += 1
            want = HAND_ONLY.get(profile, {}).get(comp)
            if want and want != hand:
                print(f"FAIL: {table}: {comp} exists only on the {want} {profile} controller, bound on {hand}")
                bad += 1
        needs = FRAME_NEEDS if profile == "frame" else SYSTEM_NEEDS if profile == "index" else []
        for action in needs:
            if action not in actions:
                print(f"FAIL: {table}: {action} is not bound")
                bad += 1
        print(f"{table} ({profile}): {len(actions)} actions checked")

    for table in TABLES:
        if table not in found:
            print(f"FAIL: binding table {table} not found")
            bad += 1
    pad = Path(sys.argv[2]) if len(sys.argv) > 2 else PAD_SRC
    driving = Path(sys.argv[3]) if len(sys.argv) > 3 else DRIVING_SRC
    bad += vehicle_grips(pad.read_text(), driving.read_text())
    print("bindings OK" if not bad else f"{bad} problem(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
