#pragma once

// Where the Steam Frame build reads the retail game data from (Vice City).
//
// The Steam PC install of the game is visible, read-only, inside Lepton. The
// port prefers it over the copy in ~/Documents, so the data does not have to
// be copied. Reads resolve in this order:
//
//   1. files this port adds or replaces (gamefiles/): the Documents copy;
//   2. everything else in the retail folders: the Steam install, if the game
//      is installed there;
//   3. the Documents copy (the old behaviour, and the fallback).
//
// Writes always land in Documents (the working directory stays there). The
// game "changes directory" a lot; here that is a virtual working directory so
// the read side can still pick Steam or Documents for the relative paths that
// follow.
//
// Everything is off outside the Steam Frame Android build.
#if defined(RW_VULKAN) && defined(__ANDROID__) && MIAMIVR_STEAM_FRAME && !defined(MIAMIVR_BRINGUP)
#define MIAMIVR_GAMEDATA 1
#else
#define MIAMIVR_GAMEDATA 0
#endif

#if MIAMIVR_GAMEDATA || defined(GAMEDATA_HOST_TEST)

#include <stddef.h>

namespace gamedata {

// Finds the Steam install of an app. Parses libraryfolders.vdf for library
// paths, appmanifest_<appid>.acf for the install folder, and checks that every
// key file is readable (case-insensitively). Returns false when not found.
bool locateSteam(const char *steamappsDir, int appId, const char *defaultInstallDir,
                 const char *const *keyFiles, int numKeyFiles,
                 char *out, size_t outSize);

// Sets the writable root (the Documents gamedata folder) and the Steam root
// (nullptr or "" for none), and resets the virtual working directory.
void setRoots(const char *docsRoot, const char *steamRoot);

// App start-up: locate Steam for GTA III, set the roots, log what was chosen.
void init(const char *docsRoot);

const char *steamRoot(void);   // "" when the game is not read from Steam
const char *docsRoot(void);

// The game's chdir. Nothing really changes directory; the path is remembered.
void changeDir(const char *dir);

// Resolve a game path (relative to the virtual working directory, or
// absolute; either slash; any case) to a real path in out. For a read, the
// result is the Steam, overlay or Documents file per the order above, and the
// return value says whether it exists. For a write the result is always under
// Documents (parent folders are created) and the return value is true.
bool resolve(const char *path, bool write, char *out, size_t outSize);

// The same for an fopen/open mode string ("w", "a", "r+", "wb" ...).
bool resolveForMode(const char *path, const char *mode, char *out, size_t outSize);

// A folder to list: the Documents folder if there is one, else Steam's.
bool resolveDir(const char *path, char *out, size_t outSize);

} // namespace gamedata

#endif
