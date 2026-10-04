// Steam-in-place game data for the Steam Frame build. See GameData.h.
#include "GameData.h"

#if MIAMIVR_GAMEDATA || defined(GAMEDATA_HOST_TEST)

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mutex>
#include <string>
#include <vector>

#ifdef GAMEDATA_HOST_TEST
#define GD_LOGI(...) do { printf(__VA_ARGS__); printf("\n"); } while(0)
#define GD_LOGW(...) do { printf(__VA_ARGS__); printf("\n"); } while(0)
#define GD_LOGE(...) do { printf(__VA_ARGS__); printf("\n"); } while(0)
#else
#include "platform_android.h"
#define GD_LOGI LOGI
#define GD_LOGW LOGW
#define GD_LOGE LOGE
#endif

namespace gamedata {

namespace {

// Files this port adds to or replaces in the retail folders: the repo's
// gamefiles/, listed by CMake at configure time. They are read from
// Documents, where deploy.sh puts them, even when the game itself is read
// from Steam.
const char *const kOverlayFiles[] = {
#include "gamefiles_manifest.inc"
	nullptr
};

// The retail folders. A file in one of these comes from Steam unless it is an
// overlay file. Anything else (ini files, settings, caches, saves) is the
// port's own and lives in Documents.
const char *const kRetailFolders[] = {
	"data", "text", "anim", "txd", "models", "audio", "mp3", "skins", "movies",
	nullptr
};

std::mutex gMutex;
std::string gDocs;       // writable root: <storage>/gamedata
std::string gSteam;      // Steam install folder, or empty
std::string gCwd;        // virtual working directory, absolute

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

// Backslashes to slashes, doubled slashes and "." dropped, trailing blanks
// dropped (a few retail data files name "foo.dat " with one). ".." is kept
// for fold(). absolute is set when the path started with a slash.
std::vector<std::string>
split(const char *path, bool *absolute)
{
	std::string p = path;
	while(!p.empty() && (p.back() == ' ' || p.back() == '\t' ||
	                     p.back() == '\r' || p.back() == '\n'))
		p.pop_back();
	*absolute = !p.empty() && (p[0] == '/' || p[0] == '\\');
	std::vector<std::string> parts;
	std::string cur;
	for(size_t i = 0; i <= p.size(); i++){
		const char c = i < p.size() ? p[i] : '/';
		if(c == '/' || c == '\\'){
			if(!cur.empty() && cur != ".")
				parts.push_back(cur);
			cur.clear();
		}else
			cur += c;
	}
	return parts;
}

std::vector<std::string>
fold(const std::vector<std::string> &parts)
{
	std::vector<std::string> out;
	for(const std::string &p : parts){
		if(p == ".."){
			if(!out.empty())
				out.pop_back();
		}else
			out.push_back(p);
	}
	return out;
}

std::string
join(const std::vector<std::string> &parts, size_t from = 0)
{
	std::string out;
	for(size_t i = from; i < parts.size(); i++){
		if(i > from)
			out += '/';
		out += parts[i];
	}
	return out;
}

bool
exists(const std::string &path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0;
}

bool
isDir(const std::string &path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// Walks components below base without regard to case. base must exist (an
// empty base is "/"). Components are corrected as far as they exist; the rest
// is appended as written. Returns true when the whole path exists.
bool
ciWalk(const std::string &base, const std::vector<std::string> &parts, size_t from,
       std::string *out)
{
	std::string cur = base;
	bool found = true;
	for(size_t i = from; i < parts.size(); i++){
		const std::string &name = parts[i];
		if(!found){
			cur += "/" + name;
			continue;
		}
		std::string candidate = cur + "/" + name;
		if(exists(candidate)){
			cur = candidate;
			continue;
		}
		DIR *d = opendir(cur.empty() ? "/" : cur.c_str());
		bool hit = false;
		if(d){
			while(struct dirent *e = readdir(d)){
				if(strcasecmp(e->d_name, name.c_str()) == 0){
					cur += "/";
					cur += e->d_name;
					hit = true;
					break;
				}
			}
			closedir(d);
		}
		if(!hit){
			cur = candidate;
			found = false;
		}
	}
	*out = cur;
	return found;
}

bool
isRetailFolder(const std::string &first)
{
	for(int i = 0; kRetailFolders[i]; i++)
		if(strcasecmp(first.c_str(), kRetailFolders[i]) == 0)
			return true;
	return false;
}

bool
isOverlayFile(const std::string &rel)
{
	for(int i = 0; kOverlayFiles[i]; i++)
		if(strcasecmp(rel.c_str(), kOverlayFiles[i]) == 0)
			return true;
	return false;
}

void
makeParents(const std::string &path)
{
	for(size_t i = 1; i < path.size(); i++)
		if(path[i] == '/'){
			std::string dir = path.substr(0, i);
			if(!isDir(dir))
				mkdir(dir.c_str(), 0775);
		}
}

bool
copyOut(const std::string &s, char *out, size_t outSize)
{
	if(s.size() + 1 > outSize){
		if(outSize)
			out[0] = '\0';
		return false;
	}
	memcpy(out, s.c_str(), s.size() + 1);
	return true;
}

enum Mode { Read, Write, Dir };

// The core: where does this game path live?
//   Write: Documents.
//   Read:  see the order in GameData.h.
//   Dir:   Documents if it has the folder, else Steam.
// Paths outside the game data folder (the userfiles folder, for one) are not
// game data and pass through, with case-insensitive matching as casepath() did.
bool
resolveParts(const char *path, Mode mode, std::string *result)
{
	std::string docs, steam, cwd;
	{
		std::lock_guard<std::mutex> lock(gMutex);
		docs = gDocs; steam = gSteam; cwd = gCwd;
	}

	bool absolute;
	std::vector<std::string> full = split(path, &absolute);
	if(!absolute){
		bool unused;
		std::vector<std::string> base = split(cwd.empty() ? docs.c_str() : cwd.c_str(), &unused);
		base.insert(base.end(), full.begin(), full.end());
		full.swap(base);
	}
	full = fold(full);

	bool unused;
	const std::vector<std::string> docParts = split(docs.c_str(), &unused);
	bool inside = full.size() >= docParts.size();
	for(size_t i = 0; inside && i < docParts.size(); i++)
		if(full[i] != docParts[i])
			inside = false;

	if(!inside){
		std::string out;
		const bool found = ciWalk("", full, 0, &out);
		if(mode == Write)
			makeParents(out);
		*result = out;
		return mode == Write || found;
	}

	const size_t from = docParts.size();
	std::string docsPath;
	const bool inDocs = ciWalk(docs, full, from, &docsPath);
	if(mode == Write){
		makeParents(docsPath);
		*result = docsPath;
		return true;
	}

	std::string steamPath;
	bool inSteam = false;
	if(!steam.empty() && full.size() > from)
		inSteam = ciWalk(steam, full, from, &steamPath);

	if(mode == Dir){
		if(inDocs && isDir(docsPath)){ *result = docsPath; return true; }
		if(inSteam && isDir(steamPath)){ *result = steamPath; return true; }
		*result = docsPath;
		return inDocs;
	}

	const bool retail = full.size() > from && isRetailFolder(full[from]);
	if(retail && !steam.empty()){
		if(isOverlayFile(join(full, from))){
			if(inDocs){ *result = docsPath; return true; }
			if(inSteam){ *result = steamPath; return true; }
		}else{
			if(inSteam){ *result = steamPath; return true; }
			if(inDocs){ *result = docsPath; return true; }
		}
	}else{
		if(inDocs){ *result = docsPath; return true; }
		if(inSteam){ *result = steamPath; return true; }
	}
	*result = docsPath;
	return false;
}

// ---------------------------------------------------------------------------
// Valve's text format, just enough: first  "key"   "value"  after the key.
// ---------------------------------------------------------------------------

bool
readFile(const std::string &path, std::string *out)
{
	FILE *f = fopen(path.c_str(), "rb");
	if(!f)
		return false;
	char buf[4096];
	size_t n;
	out->clear();
	while((n = fread(buf, 1, sizeof(buf), f)) > 0)
		out->append(buf, n);
	fclose(f);
	return true;
}

// Reads one quoted string starting at s[i] (which must be a quote). Handles
// the \\ and \" escapes Steam writes into paths.
bool
quoted(const std::string &s, size_t *i, std::string *out)
{
	if(*i >= s.size() || s[*i] != '"')
		return false;
	out->clear();
	for(size_t k = *i + 1; k < s.size(); k++){
		if(s[k] == '\\' && k + 1 < s.size()){
			out->push_back(s[++k]);
		}else if(s[k] == '"'){
			*i = k + 1;
			return true;
		}else
			out->push_back(s[k]);
	}
	return false;
}

// All values of "key" "value" pairs in text, in order.
std::vector<std::string>
vdfValues(const std::string &text, const char *key)
{
	std::vector<std::string> values;
	size_t i = 0;
	while((i = text.find('"', i)) != std::string::npos){
		std::string k;
		if(!quoted(text, &i, &k))
			break;
		if(strcasecmp(k.c_str(), key) != 0)
			continue;
		size_t j = i;
		while(j < text.size() && (text[j] == ' ' || text[j] == '\t'))
			j++;
		std::string v;
		if(j < text.size() && text[j] == '"' && quoted(text, &j, &v)){
			values.push_back(v);
			i = j;
		}
	}
	return values;
}

} // namespace

bool
locateSteam(const char *steamappsDir, int appId, const char *defaultInstallDir,
            const char *const *keyFiles, int numKeyFiles, char *out, size_t outSize)
{
	std::vector<std::string> libraries;
	std::string vdf;
	if(readFile(std::string(steamappsDir) + "/libraryfolders.vdf", &vdf))
		libraries = vdfValues(vdf, "path");
	// The Steam folder itself is always a library.
	std::string self = steamappsDir;
	const size_t slash = self.rfind('/');
	if(slash != std::string::npos)
		self.resize(slash);
	libraries.push_back(self);

	for(const std::string &lib : libraries){
		char name[64];
		snprintf(name, sizeof(name), "/steamapps/appmanifest_%d.acf", appId);
		std::string manifest;
		if(!readFile(lib + name, &manifest))
			continue;

		const std::vector<std::string> flags = vdfValues(manifest, "StateFlags");
		// 4 is "fully installed"; an update in flight clears it. Reading a
		// half-updated install is worse than the copy.
		if(!flags.empty() && !(atoi(flags[0].c_str()) & 4)){
			GD_LOGW("[vr] game data: Steam app %d in %s is not fully installed (StateFlags %s)",
			        appId, lib.c_str(), flags[0].c_str());
			continue;
		}
		std::vector<std::string> dirs = vdfValues(manifest, "installdir");
		const std::string installDir = dirs.empty() || dirs[0].empty() ?
			std::string(defaultInstallDir) : dirs[0];
		const std::string root = lib + "/steamapps/common/" + installDir;
		if(!isDir(root))
			continue;

		bool ok = true;
		for(int i = 0; i < numKeyFiles && ok; i++){
			bool abs;
			std::string real;
			const std::vector<std::string> parts = split(keyFiles[i], &abs);
			ok = ciWalk(root, parts, 0, &real) && access(real.c_str(), R_OK) == 0;
			if(!ok)
				GD_LOGW("[vr] game data: Steam folder %s lacks %s", root.c_str(), keyFiles[i]);
		}
		if(ok)
			return copyOut(root, out, outSize);
	}
	return false;
}

void
setRoots(const char *docsRoot, const char *steamRoot)
{
	std::lock_guard<std::mutex> lock(gMutex);
	gDocs = docsRoot ? docsRoot : "";
	gSteam = steamRoot ? steamRoot : "";
	gCwd = gDocs;
}

const char *steamRoot(void) { return gSteam.c_str(); }
const char *docsRoot(void) { return gDocs.c_str(); }

void
init(const char *docs)
{
	static const int kAppId = 12110;
	static const char *const kKeyFiles[] = { "models/gta3.img", "models/gta3.dir", "data/gta_vc.dat" };
	// Inside Lepton the host's Steam library shows up at the same paths.
	static const char *const kSteamApps[] = {
		"/home/steamos/.local/share/Steam/steamapps",
		"/home/steamos/.steam/steam/steamapps",
	};

	char steam[512] = "";
	bool found = false;
	for(const char *dir : kSteamApps)
		if(locateSteam(dir, kAppId, "Grand Theft Auto Vice City", kKeyFiles, 3,
		               steam, sizeof(steam))){
			found = true;
			break;
		}
	setRoots(docs, found ? steam : nullptr);

	if(found)
		GD_LOGI("[vr] game data: steam %s", steam);
	else
		GD_LOGI("[vr] game data: Documents copy %s (no Steam install of app %d found)", docs, kAppId);
	GD_LOGI("[vr] game data overlay: %s (port files, settings, saves, caches)", docs);

	// The port's own files must be in Documents; deploy.sh puts them there.
	if(found){
		char probe[512];
		for(int i = 0; kOverlayFiles[i]; i++)
			if(!resolve(kOverlayFiles[i], false, probe, sizeof(probe)) ||
			   strncmp(probe, docs, strlen(docs)) != 0){
				GD_LOGW("[vr] game data: port file %s is not in %s; run deploy.sh --port-assets",
				        kOverlayFiles[i], docs);
				break;
			}
	}else{
		char probe[512];
		if(!resolve("models/gta3.img", false, probe, sizeof(probe)))
			GD_LOGE("[vr] game data: no game data. Install Vice City from Steam on the Frame, "
			        "or copy it with deploy.sh --from-steam / --game-dir");
	}
}

void
changeDir(const char *dir)
{
	bool abs;
	std::vector<std::string> parts = split(dir, &abs);
	std::lock_guard<std::mutex> lock(gMutex);
	if(!abs){
		bool unused;
		std::vector<std::string> base = split(gCwd.empty() ? gDocs.c_str() : gCwd.c_str(), &unused);
		base.insert(base.end(), parts.begin(), parts.end());
		parts.swap(base);
	}
	gCwd = "/" + join(fold(parts));
}

bool
resolve(const char *path, bool write, char *out, size_t outSize)
{
	std::string result;
	const bool ok = resolveParts(path, write ? Write : Read, &result);
	return copyOut(result, out, outSize) && ok;
}

bool
resolveForMode(const char *path, const char *mode, char *out, size_t outSize)
{
	return resolve(path, strpbrk(mode, "wa+") != nullptr, out, outSize);
}

bool
resolveDir(const char *path, char *out, size_t outSize)
{
	std::string result;
	const bool ok = resolveParts(path, Dir, &result);
	return copyOut(result, out, outSize) && ok;
}

} // namespace gamedata

#endif
