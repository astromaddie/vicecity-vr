#include "CutsceneSkipHold.h"
#include "CutsceneSkipRing.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

static int checks;

static void
Check(bool condition, const char *message)
{
	++checks;
	if(!condition){
		std::fprintf(stderr, "FAIL: %s\n", message);
		std::exit(1);
	}
}

int
main()
{
	CutsceneSkipHold hold;
	int finishes = 0;
	auto finish = [&finishes](){ ++finishes; };

	Check(!hold.UpdateAndFinish(true, true, true, 1000000000LL, "intro", 0, finish),
	      "fresh A press does not immediately finish a cutscene");
	Check(hold.ConsumesButton(true),
	      "physical A is consumed while its cutscene hold is pending");
	Check(!hold.UpdateAndFinish(true, true, true, 1250000000LL, "intro", 0, finish),
	      "intermediate focused frames preserve a continuous hold");
	Check(hold.Progress() == 0.5f,
	      "eligible hold exposes half progress for a future ring");
	Check(!hold.UpdateAndFinish(true, true, true, 1499000000LL, "intro", 0, finish),
	      "499 ms hold does not finish");
	Check(hold.Progress() > 0.99f && hold.Progress() < 1.0f,
	      "progress remains below full before the threshold");
	Check(hold.UpdateAndFinish(true, true, true, 1500000000LL, "intro", 0, finish),
	      "500 ms continuous hold finishes the formal cutscene");
	Check(hold.Progress() == 1.0f, "threshold reports full progress");
	Check(finishes == 1, "threshold invokes the native finish callback once");
	Check(!hold.UpdateAndFinish(true, true, true, 1514000000LL, "intro", 0, finish),
	      "continued hold cannot finish the same scene twice");
	Check(finishes == 1, "one held press produces at most one finish");
	Check(hold.ConsumesButton(true),
	      "fired A press stays consumed through cutscene cleanup");
	Check(!hold.UpdateAndFinish(false, false, false, 1528000000LL, "intro", 0, finish),
	      "release rearms without invoking finish");
	Check(!hold.ConsumesButton(false), "release clears the consumed A latch");
	Check(hold.Progress() == 0.0f, "release clears hold progress");

	Check(!hold.UpdateAndFinish(true, true, true, 2000000000LL, "intro", 0, finish),
	      "A tap test starts without skipping");
	Check(!hold.UpdateAndFinish(true, true, true, 2250000000LL, "intro", 0, finish),
	      "tap remains pending before its release");
	Check(!hold.UpdateAndFinish(true, true, true, 2499000000LL, "intro", 0, finish),
	      "tap may be held close to, but below, the threshold");
	Check(!hold.UpdateAndFinish(false, false, false, 2499000000LL, "intro", 0, finish),
	      "release before threshold is a non-skipping A tap");
	Check(finishes == 1, "A tap does not invoke finish");
	Check(!hold.ConsumesButton(false), "A tap release clears the consumed latch");

	Check(!hold.UpdateAndFinish(true, false, false, 3000000000LL, "intro", 0, finish),
	      "gameplay A press is not a cutscene skip");
	Check(!hold.ConsumesButton(true),
	      "gameplay A remains available to its normal action");
	Check(!hold.UpdateAndFinish(true, false, false, 3500000000LL, "intro", 0, finish),
	      "holding A through a gameplay-to-cutscene boundary stays disarmed");
	Check(!hold.UpdateAndFinish(true, true, true, 3600000000LL, "intro", 0, finish),
	      "a held gameplay press cannot skip a newly entered cutscene");
	Check(hold.ConsumesButton(true),
	      "A becomes consumed once the held press enters a cutscene");
	Check(!hold.UpdateAndFinish(false, false, false, 3614000000LL, "intro", 0, finish),
	      "release clears boundary disarm");

	Check(!hold.UpdateAndFinish(true, true, true, 4000000000LL, "intro", 0, finish),
	      "eligible press begins a hold");
	Check(!hold.UpdateAndFinish(true, true, false, 4200000000LL, "intro", 0, finish),
	      "pause, focus loss, or VR-menu opening interrupts the hold");
	Check(hold.Progress() == 0.0f,
	      "an ineligible hold exposes no progress for the visual indicator");
	Check(!hold.UpdateAndFinish(true, true, true, 4700000000LL, "intro", 0, finish),
	      "regaining eligibility while still held requires a fresh press");
	Check(!hold.UpdateAndFinish(false, false, false, 4714000000LL, "intro", 0, finish),
	      "interrupted press clears on release");

	Check(!hold.UpdateAndFinish(true, true, true, 5000000000LL, "intro", 0, finish),
	      "long-gap test starts a new press");
	Check(!hold.UpdateAndFinish(true, true, true, 5300000000LL, "intro", 0, finish),
	      "a 300 ms frame or focus gap disarms the hold");
	Check(!hold.UpdateAndFinish(true, true, true, 5500000000LL, "intro", 0, finish),
	      "a long-gap press remains disarmed until release");
	Check(finishes == 1, "interrupted attempts never invoke finish");

	Check(!CutsceneSkipHold::HasConflictingChord(false, false, 0.64f, 0.0f),
	      "a left grip below the threshold does not interrupt");
	Check(CutsceneSkipHold::HasConflictingChord(false, false, 0.65f, 0.0f),
	      "the camera modifier grip threshold interrupts the skip hold");
	Check(CutsceneSkipHold::HasConflictingChord(false, false, 0.0f, 0.65f),
	      "either physical grip interrupts the skip hold");
	Check(CutsceneSkipHold::HasConflictingChord(false, true, 0.0f, 0.0f),
	      "View interrupts the skip hold");
	Check(CutsceneSkipHold::HasConflictingChord(true, false, 0.0f, 0.0f),
	      "Menu interrupts the skip hold");
	CutsceneSkipHold chordHold;
	Check(!chordHold.UpdateAndFinish(true, true,
		!CutsceneSkipHold::HasConflictingChord(false, false, 0.0f, 0.0f),
		8000000000LL, "chord", 0, finish),
	      "a chord test starts through the production hold helper");
	Check(!chordHold.UpdateAndFinish(true, true,
		!CutsceneSkipHold::HasConflictingChord(false, true, 0.0f, 0.0f),
		8200000000LL, "chord", 200, finish),
	      "View cancellation reaches the production eligibility gate");
	Check(chordHold.Progress() == 0.0f,
	      "View cancellation clears the production ring progress immediately");
	Check(chordHold.ConsumesButton(true),
	      "the canceled physical A press remains consumed");
	Check(!chordHold.UpdateAndFinish(true, true,
		!CutsceneSkipHold::HasConflictingChord(false, false, 0.0f, 0.0f),
		8800000000LL, "chord", 800, finish),
	      "removing the chord while A stays down does not resume the hold");
	Check(!chordHold.UpdateAndFinish(false, true, true,
		8814000000LL, "chord", 814, finish),
	      "release after a chord cancellation rearms the hold");
	Check(!chordHold.UpdateAndFinish(true, true, true,
		9000000000LL, "chord", 1000, finish),
	      "a new physical press starts after chord release");

	Check(!hold.UpdateAndFinish(true, true, true, 6000000000LL,
	                            "intro", 100, finish),
	      "a new scene sample starts a fresh hold");
	Check(!hold.UpdateAndFinish(true, true, true, 6200000000LL,
	                            "outro", 10, finish),
	      "changing cutscene identity cancels an in-flight hold");
	Check(hold.Progress() == 0.0f,
	      "scene changes immediately clear the visible progress");
	Check(hold.ConsumesButton(true),
	      "a press crossing a scene boundary remains consumed");
	Check(!hold.UpdateAndFinish(true, true, true, 6800000000LL,
	                            "outro", 610, finish),
	      "a held press cannot resume after entering the next scene");
	Check(!hold.UpdateAndFinish(false, true, true, 6814000000LL,
	                            "outro", 624, finish),
	      "release after the scene change rearms the hold");
	Check(!hold.UpdateAndFinish(true, true, true, 7000000000LL,
	                            "outro", 810, finish),
	      "a new press can start after release in the new scene");
	Check(!hold.UpdateAndFinish(true, true, true, 7200000000LL,
	                            "outro", 12, finish),
	      "a same-name cutscene timer reset cancels an in-flight hold");
	Check(hold.Progress() == 0.0f,
	      "timer resets immediately clear the visible progress");
	Check(finishes == 1, "scene-boundary presses never finish a second scene");

	const float epsilon = 0.001f;
	CutsceneSkipRingQuad quad;
	Check(!CutsceneSkipRing::GetSegment(0.0f, 0, 0, 0, 20, 4, &quad),
	      "an idle hold draws no progress segment");
	Check(!CutsceneSkipRing::GetSegment(-0.5f, 0, 0, 0, 20, 4, &quad),
	      "negative progress draws no ring segment");
	Check(CutsceneSkipRing::GetSegment(0.25f, 0, 0, 0, 20, 4, &quad),
	      "the first clockwise segment exists");
	Check(std::fabs(quad.x[0]) < epsilon &&
	      std::fabs(quad.y[0] + 20.0f) < epsilon,
	      "the progress ring starts at 12 o'clock");
	int quarterCount = 0;
	int quarterLast = -1;
	for(int i = 0; i < CutsceneSkipRing::SEGMENTS; i++)
		if(CutsceneSkipRing::GetSegment(0.25f, i, 0, 0, 20, 4, &quad)){
			quarterCount++;
			quarterLast = i;
		}
	Check(quarterCount == 16,
	      "one quarter progress fills exactly one quarter of the ring");
	Check(quarterLast == 15, "the clockwise quarter ends at segment 15");
	Check(std::fabs(quad.x[1] - 20.0f) < epsilon &&
	      std::fabs(quad.y[1]) < epsilon,
	      "clockwise progress starts at the top and reaches the right at 25 percent");
	int halfCount = 0;
	for(int i = 0; i < CutsceneSkipRing::SEGMENTS; i++)
		if(CutsceneSkipRing::GetSegment(0.5f, i, 0, 0, 20, 4, &quad))
			halfCount++;
	Check(halfCount == 32, "half progress fills half of the ring");
	Check(std::fabs(quad.x[1]) < epsilon &&
	      std::fabs(quad.y[1] - 20.0f) < epsilon,
	      "clockwise half progress reaches the bottom");
	int fullCount = 0;
	int fullLast = -1;
	for(int i = 0; i < CutsceneSkipRing::SEGMENTS; i++)
		if(CutsceneSkipRing::GetSegment(1.5f, i, 0, 0, 20, 4, &quad)){
			fullCount++;
			fullLast = i;
		}
	Check(fullCount == CutsceneSkipRing::SEGMENTS,
	      "progress above one clamps to a complete ring");
	Check(fullLast == CutsceneSkipRing::SEGMENTS - 1 &&
	      std::fabs(quad.x[1]) < epsilon &&
	      std::fabs(quad.y[1] + 20.0f) < epsilon,
	      "a complete clockwise ring closes back at the top");
	bool ringBandIsCorrect = true;
	for(int i = 0; i < CutsceneSkipRing::SEGMENTS; i++){
		if(!CutsceneSkipRing::GetSegment(1.0f, i, 7, -3, 20, 4, &quad)){
			ringBandIsCorrect = false;
			break;
		}
		for(int point = 0; point < 4; point++){
			const float dx = quad.x[point] - 7.0f;
			const float dy = quad.y[point] + 3.0f;
			const float radius = std::sqrt(dx*dx + dy*dy);
			const float expected = point < 2 ? 20.0f : 16.0f;
			if(std::fabs(radius - expected) > epsilon)
				ringBandIsCorrect = false;
		}
	}
	Check(ringBandIsCorrect,
	      "every progress segment stays within the requested ring thickness");

	CutsceneSkipRingQuad polygonArguments;
	Check(CutsceneSkipRing::GetDraw2DPolygonArguments(quad, &polygonArguments),
	      "the production ring converts to Draw2DPolygon argument order");
	const float nativeX[4] = {
		polygonArguments.x[2], polygonArguments.x[3],
		polygonArguments.x[1], polygonArguments.x[0]
	};
	const float nativeY[4] = {
		polygonArguments.y[2], polygonArguments.y[3],
		polygonArguments.y[1], polygonArguments.y[0]
	};
	bool nativeFanKeepsAnnularPerimeter = true;
	for(int i = 0; i < 4; i++)
		if(std::fabs(nativeX[i] - quad.x[i]) > epsilon ||
		   std::fabs(nativeY[i] - quad.y[i]) > epsilon)
			nativeFanKeepsAnnularPerimeter = false;
	Check(nativeFanKeepsAnnularPerimeter,
	      "Sprite2d SetVertices native fan order preserves the annular perimeter");

	std::printf("PASS: %d cutscene A hold and ring checks\n", checks);
	return 0;
}
