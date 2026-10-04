// Host test for src/skel/android/GameData.cpp (Steam-in-place game data).
//
//   tools/steam_frame/tests/run_gamedata_test.sh
//
// Builds a fake Steam library and a fake Documents folder in a temp dir and
// checks library discovery and the read/write resolution order.
#include "GameData.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

static int failures;
// Case-insensitive on purpose: a Mac test folder ignores case, so the path
// comes back as asked. On a Linux host the case correction is exercised too.
static bool same(const std::string &a, const std::string &b) { return strcasecmp(a.c_str(), b.c_str()) == 0; }
#define CHECK(c) do { if(!(c)){ printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while(0)

static void mk(const std::string &path)
{
	for(size_t i = 1; i < path.size(); i++)
		if(path[i] == '/')
			mkdir(path.substr(0, i).c_str(), 0775);
	mkdir(path.c_str(), 0775);
}
static void touch(const std::string &path, const char *text = "x")
{
	mk(path.substr(0, path.rfind('/')));
	FILE *f = fopen(path.c_str(), "w");
	fputs(text, f);
	fclose(f);
}
static std::string res(const char *path, bool write = false, bool *ok = nullptr)
{
	char out[1024];
	bool r = gamedata::resolve(path, write, out, sizeof(out));
	if(ok) *ok = r;
	return out;
}

int main()
{
	char tmpl[] = "/tmp/vcgdtestXXXXXX";
	std::string t = mkdtemp(tmpl);
	std::string steamHome = t + "/Steam";
	std::string lib2 = t + "/Lib2";
	std::string docs = t + "/Documents/ViceCityVR/gamedata";
	mk(docs);

	const char *keys[] = { "models/gta3.img", "models/gta3.dir", "data/gta_vc.dat" };
	char root[512];

	// Nothing installed.
	touch(steamHome + "/steamapps/libraryfolders.vdf",
	      ("\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" + steamHome + "\"\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"" + lib2 + "\"\n\t}\n}\n").c_str());
	CHECK(!gamedata::locateSteam((steamHome + "/steamapps").c_str(), 12110, "Grand Theft Auto Vice City", keys, 3, root, sizeof root));

	// Installed in the second library, odd case on the key files.
	touch(lib2 + "/steamapps/appmanifest_12110.acf",
	      "\"AppState\"\n{\n\t\"appid\"\t\t\"12110\"\n\t\"StateFlags\"\t\t\"4\"\n\t\"installdir\"\t\t\"Grand Theft Auto Vice City\"\n}\n");
	std::string steam = lib2 + "/steamapps/common/Grand Theft Auto Vice City";
	touch(steam + "/Models/GTA3.IMG", "img");
	touch(steam + "/Models/gta3.dir");
	touch(steam + "/DATA/Gta_vc.dat", "dat");
	touch(steam + "/TEXT/american.gxt", "retail gxt");
	touch(steam + "/audio/sfx.RAW");
	CHECK(gamedata::locateSteam((steamHome + "/steamapps").c_str(), 12110, "Grand Theft Auto Vice City", keys, 3, root, sizeof root));
	CHECK(steam == root);

	// Half-updated: StateFlags without the installed bit.
	touch(lib2 + "/steamapps/appmanifest_12110.acf", "\"AppState\"\n{\n\t\"StateFlags\"\t\t\"1026\"\n\t\"installdir\"\t\t\"Grand Theft Auto Vice City\"\n}\n");
	CHECK(!gamedata::locateSteam((steamHome + "/steamapps").c_str(), 12110, "Grand Theft Auto Vice City", keys, 3, root, sizeof root));
	touch(lib2 + "/steamapps/appmanifest_12110.acf", "\"AppState\"\n{\n\t\"StateFlags\"\t\t\"4\"\n\t\"installdir\"\t\t\"Grand Theft Auto Vice City\"\n}\n");

	// Missing key file.
	unlink((steam + "/Models/gta3.dir").c_str());
	CHECK(!gamedata::locateSteam((steamHome + "/steamapps").c_str(), 12110, "Grand Theft Auto Vice City", keys, 3, root, sizeof root));
	touch(steam + "/Models/gta3.dir");

	// ---- Steam active ----
	// A port overlay file and a stale full copy in Documents.
	touch(docs + "/TEXT/american.gxt", "re3 gxt");        // overlay (in the test manifest)
	touch(docs + "/data/gta_vc.dat", "stale copy");         // retail, copied earlier
	touch(docs + "/re3.ini", "ini");                       // port file
	touch(docs + "/Models/only_in_docs.txd");              // retail folder, Steam lacks it
	gamedata::setRoots(docs.c_str(), steam.c_str());
	

	CHECK(same(res("TEXT\\american.gxt"), docs + "/TEXT/american.gxt"));          // overlay wins
	CHECK(same(res("text/AMERICAN.GXT"), docs + "/TEXT/american.gxt"));           // any case
	CHECK(same(res("data\\gta_vc.dat"), steam + "/DATA/Gta_vc.dat"));                  // Steam beats copy
	CHECK(same(res("models/gta3.img"), steam + "/Models/GTA3.IMG"));
	CHECK(same(res("models/only_in_docs.txd"), docs + "/Models/only_in_docs.txd")); // fallback to copy
	bool ok;
	touch(docs + "/models/vrhands/BigHandLeft.uxrh", "hand");   // port asset: Documents wins
	touch(steam + "/models/vrhands/BigHandLeft.uxrh", "other");
	CHECK(same(res("models\\vrhands\\BigHandLeft.uxrh"), docs + "/models/vrhands/BigHandLeft.uxrh"));
	CHECK(same(res("modelsets/modern/models/gta3.img", false, &ok), docs + "/modelsets/modern/models/gta3.img") && !ok);
	CHECK(same(res("re3.ini"), docs + "/re3.ini"));                                // not retail: Documents
	CHECK(same(res("vr_settings.ini"), docs + "/vr_settings.ini"));                // missing: Documents path
	res("vr_settings.ini", false, &ok); CHECK(!ok);
	CHECK(same(res("models/nothere.txd", false, &ok), docs + "/models/nothere.txd") && !ok);
	CHECK(same(res("audio/sfx.raw"), steam + "/audio/sfx.RAW"));
	CHECK(same(res("data/gta_vc.dat "), steam + "/DATA/Gta_vc.dat"));                  // trailing blank

	// Virtual working directory.
	gamedata::changeDir((docs + "\\DATA\\").c_str());
	CHECK(same(res("gta_vc.dat"), steam + "/DATA/Gta_vc.dat"));
	gamedata::changeDir((docs + "\\TEXT\\").c_str());
	CHECK(same(res("american.gxt"), docs + "/TEXT/american.gxt"));
	gamedata::changeDir((docs + "\\").c_str());
	CHECK(same(res("american.gxt", false, &ok), docs + "/american.gxt") && !ok);
	gamedata::changeDir((t + "/Documents/ViceCityVR/userfiles").c_str());   // outside the data folder
	touch(t + "/Documents/ViceCityVR/userfiles/GTAVCsf1.b", "save");
	CHECK(same(res("gtavcsf1.b"), t + "/Documents/ViceCityVR/userfiles/GTAVCsf1.b"));
	gamedata::changeDir(docs.c_str());

	// Writes always land in Documents, even for retail names; parents appear.
	CHECK(same(res("data/gta_vc.dat", true), docs + "/data/gta_vc.dat"));
	CHECK(same(res("data/new/cache.bin", true), docs + "/data/new/cache.bin"));
	struct stat st; CHECK(stat((docs + "/data/new").c_str(), &st) == 0);
	char out[1024];
	CHECK(gamedata::resolveForMode("models/gta3.img", "rb", out, sizeof out) && same(out, steam + "/Models/GTA3.IMG"));
	CHECK(gamedata::resolveForMode("models/gta3.img", "r+b", out, sizeof out) && same(out, docs + "/models/gta3.img"));

	// Folder listing: Documents' folder if it has one, else Steam's.
	CHECK(gamedata::resolveDir("data\\", out, sizeof out) && same(out, docs + "/data"));
	CHECK(gamedata::resolveDir("audio", out, sizeof out) && same(out, steam + "/audio"));

	// ---- No Steam: the Documents copy, exactly as before ----
	gamedata::setRoots(docs.c_str(), nullptr);
	CHECK(same(res("data\\gta_vc.dat"), docs + "/data/gta_vc.dat"));
	CHECK(same(res("models/only_in_docs.txd"), docs + "/Models/only_in_docs.txd"));
	CHECK(same(res("models/gta3.img", false, &ok), docs + "/models/gta3.img") && !ok);

	printf(failures ? "gamedata_test: %d FAILED\n" : "gamedata_test: ok\n", failures);
	char cmd[600]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", t.c_str()); system(cmd);
	return failures != 0;
}
