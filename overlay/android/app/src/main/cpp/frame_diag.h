#pragma once

// Steam Frame unattended-test diagnostics (standard.md section 8).
//
// The headset on a desk sits in SYNCHRONIZED with shouldRender=false, so
// nothing draws and the game, which is stepped inside the frame callback,
// never advances. vr-diag.ini, read once from the storage root, lets a test
// run force frames, pose a synthetic head and dump the swapchain to files.
// This file holds only the file-format side (ini parsing, BMP and text
// output); the Vulkan readback lives in xr_vulkan_session.cpp where the
// swapchain image and command objects are.
//
// Everything here is compiled only for the Frame build.

#include <stddef.h>
#include <stdint.h>

namespace frame_diag {

struct Config
{
	// Render and step the game even when the runtime says shouldRender is
	// false and before the session is FOCUSED.
	bool forceRender = false;
	// Yaw of the synthetic head in degrees, positive = turn left.
	float yawDeg = 0.0f;
	// Dump the swapchain on this rendered frame (1-based); 0 = never.
	int dumpFrame = 0;
	// Recognised for the cross-port standard; [vr-perf] is always on.
	bool timing = false;
};

// Reads <storageRoot>/vr-diag.ini. Call once after the storage root is set;
// later calls do nothing.
void init(void);

// The parsed config (defaults before init, or when the file is absent). Calls
// init() itself if nobody has.
const Config &config(void);

// <storageRoot>/vr-dumps, created if needed. False when it cannot be made.
bool dumpDirectory(char *out, size_t size);

// 24-bit bottom-up BMP from tightly packed top-down 4-byte pixels. Bytes are
// copied as stored, so an sRGB-encoded image stays sRGB-encoded; only the
// channel order is fixed up for BMP's BGR. Alpha is dropped.
bool writeBmp24(const char *path, const uint8_t *pixels, int width, int height,
                bool sourceIsBgra);

bool writeTextFile(const char *path, const char *text);

}  // namespace frame_diag
