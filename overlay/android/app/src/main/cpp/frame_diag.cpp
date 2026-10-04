#include "frame_diag.h"
#include "platform_android.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <vector>

namespace frame_diag {
namespace {

Config gConfig;
bool gInitialised = false;

// Trims leading and trailing whitespace in place and returns the start.
char *
trim(char *text)
{
	while(*text != '\0' && isspace((unsigned char)*text))
		text++;
	char *end = text + strlen(text);
	while(end > text && isspace((unsigned char)end[-1]))
		*--end = '\0';
	return text;
}

bool
parseInt(const char *value, long *out)
{
	char *end = nullptr;
	errno = 0;
	const long parsed = strtol(value, &end, 10);
	if(end == value || *end != '\0' || errno != 0)
		return false;
	*out = parsed;
	return true;
}

bool
parseFloat(const char *value, float *out)
{
	char *end = nullptr;
	errno = 0;
	const float parsed = strtof(value, &end);
	if(end == value || *end != '\0' || errno != 0)
		return false;
	*out = parsed;
	return true;
}

void
applyKey(const char *key, const char *value)
{
	long number = 0;
	float real = 0.0f;
	if(strcmp(key, "force_render") == 0){
		if(!parseInt(value, &number)){
			LOGW("[vr] diag bad value for %s: '%s' (ignored)", key, value);
			return;
		}
		gConfig.forceRender = number != 0;
		LOGI("[vr] diag force_render=%d", gConfig.forceRender ? 1 : 0);
	}else if(strcmp(key, "diag_yaw") == 0){
		if(!parseFloat(value, &real)){
			LOGW("[vr] diag bad value for %s: '%s' (ignored)", key, value);
			return;
		}
		gConfig.yawDeg = real;
		LOGI("[vr] diag diag_yaw=%g", (double)gConfig.yawDeg);
	}else if(strcmp(key, "dump_frame") == 0){
		if(!parseInt(value, &number) || number < 0 || number > 100000000L){
			LOGW("[vr] diag bad value for %s: '%s' (ignored)", key, value);
			return;
		}
		gConfig.dumpFrame = (int)number;
		LOGI("[vr] diag dump_frame=%d", gConfig.dumpFrame);
	}else if(strcmp(key, "timing") == 0){
		if(!parseInt(value, &number)){
			LOGW("[vr] diag bad value for %s: '%s' (ignored)", key, value);
			return;
		}
		gConfig.timing = number != 0;
		LOGI("[vr] diag timing=%d", gConfig.timing ? 1 : 0);
	}else if(strcmp(key, "diag_new_game") == 0){
		if(!parseInt(value, &number)){
			LOGW("[vr] diag bad value for %s: '%s' (ignored)", key, value);
			return;
		}
		gConfig.newGame = number != 0;
		LOGI("[vr] diag diag_new_game=%d", gConfig.newGame ? 1 : 0);
	}else if(strcmp(key, "diag_pointer") == 0){
		if(strcmp(value, "frontend") == 0 || strcmp(value, "1") == 0)
			gConfig.pointerTarget = 1;
		else if(strcmp(value, "vrmenu") == 0 || strcmp(value, "2") == 0)
			gConfig.pointerTarget = 2;
		else if(strcmp(value, "none") == 0 || strcmp(value, "0") == 0)
			gConfig.pointerTarget = 0;
		else{
			LOGW("[vr] diag bad value for %s: '%s' (ignored)", key, value);
			return;
		}
		LOGI("[vr] diag diag_pointer=%d", gConfig.pointerTarget);
	}else if(strcmp(key, "diag_pointer_u") == 0 ||
	         strcmp(key, "diag_pointer_v") == 0){
		if(!parseFloat(value, &real) || real < 0.0f || real > 1.0f){
			LOGW("[vr] diag bad value for %s: '%s' (ignored)", key, value);
			return;
		}
		if(key[strlen(key) - 1] == 'u')
			gConfig.pointerU = real;
		else
			gConfig.pointerV = real;
		LOGI("[vr] diag %s=%g", key, (double)real);
	}else if(strcmp(key, "diag_vr_menu") == 0){
		if(!parseInt(value, &number) || number < 0 || number > 19){
			LOGW("[vr] diag bad value for %s: '%s' (ignored)", key, value);
			return;
		}
		gConfig.openVrMenu = (int)number;
		LOGI("[vr] diag diag_vr_menu=%d", gConfig.openVrMenu);
	}else
		LOGW("[vr] diag unknown key '%s' (ignored)", key);
}

void
writeLe32(uint8_t *out, uint32_t value)
{
	out[0] = (uint8_t)(value & 0xff);
	out[1] = (uint8_t)((value >> 8) & 0xff);
	out[2] = (uint8_t)((value >> 16) & 0xff);
	out[3] = (uint8_t)((value >> 24) & 0xff);
}

}  // namespace

void
init(void)
{
	if(gInitialised)
		return;
	gInitialised = true;

	const char *root = platform::storageRoot();
	if(root == nullptr || root[0] == '\0')
		return;
	char path[512];
	if(snprintf(path, sizeof(path), "%s/vr-diag.ini", root) >= (int)sizeof(path))
		return;
	FILE *file = fopen(path, "r");
	if(file == nullptr){
		LOGI("[vr] diag: no %s", path);
		return;
	}
	LOGI("[vr] diag: reading %s", path);

	char line[256];
	while(fgets(line, sizeof(line), file) != nullptr){
		// A line longer than the buffer would otherwise resume mid-way and be
		// parsed as a second, bogus line.
		if(strchr(line, '\n') == nullptr && !feof(file)){
			int c;
			while((c = fgetc(file)) != EOF && c != '\n')
				;
		}
		char *text = trim(line);
		if(*text == '\0' || *text == '#')
			continue;
		char *equals = strchr(text, '=');
		if(equals == nullptr){
			LOGW("[vr] diag line without '=' ignored: '%s'", text);
			continue;
		}
		*equals = '\0';
		applyKey(trim(text), trim(equals + 1));
	}
	fclose(file);
}

const Config &
config(void)
{
	if(!gInitialised)
		init();
	return gConfig;
}

bool
dumpDirectory(char *out, size_t size)
{
	const char *root = platform::storageRoot();
	if(root == nullptr || root[0] == '\0')
		return false;
	if(snprintf(out, size, "%s/vr-dumps", root) >= (int)size)
		return false;
	if(mkdir(out, 0775) != 0 && errno != EEXIST){
		LOGE("[vr-dump] cannot create %s: errno %d", out, errno);
		return false;
	}
	return true;
}

bool
writeBmp24(const char *path, const uint8_t *pixels, int width, int height,
           bool sourceIsBgra)
{
	if(width <= 0 || height <= 0 || pixels == nullptr)
		return false;
	FILE *file = fopen(path, "wb");
	if(file == nullptr){
		LOGE("[vr-dump] cannot open %s: errno %d", path, errno);
		return false;
	}

	// Rows are padded to a multiple of four bytes.
	const uint32_t rowBytes = ((uint32_t)width*3u + 3u) & ~3u;
	const uint32_t imageBytes = rowBytes*(uint32_t)height;
	uint8_t header[54] = {};
	header[0] = 'B';
	header[1] = 'M';
	writeLe32(header + 2, 54u + imageBytes);
	writeLe32(header + 10, 54u);
	writeLe32(header + 14, 40u);
	writeLe32(header + 18, (uint32_t)width);
	writeLe32(header + 22, (uint32_t)height);  // positive: rows run bottom-up
	header[26] = 1;
	header[28] = 24;
	writeLe32(header + 34, imageBytes);
	bool ok = fwrite(header, sizeof(header), 1, file) == 1;

	std::vector<uint8_t> row(rowBytes, 0);
	const int redOffset = sourceIsBgra ? 2 : 0;
	const int blueOffset = sourceIsBgra ? 0 : 2;
	for(int y = height - 1; ok && y >= 0; y--){
		const uint8_t *source = pixels + (size_t)y*(size_t)width*4u;
		uint8_t *dest = row.data();
		for(int x = 0; x < width; x++){
			dest[0] = source[blueOffset];
			dest[1] = source[1];
			dest[2] = source[redOffset];
			dest += 3;
			source += 4;
		}
		ok = fwrite(row.data(), rowBytes, 1, file) == 1;
	}
	if(fclose(file) != 0)
		ok = false;
	if(!ok)
		LOGE("[vr-dump] write failed for %s", path);
	return ok;
}

bool
writeTextFile(const char *path, const char *text)
{
	FILE *file = fopen(path, "w");
	if(file == nullptr){
		LOGE("[vr-dump] cannot open %s: errno %d", path, errno);
		return false;
	}
	bool ok = fputs(text, file) >= 0;
	if(fclose(file) != 0)
		ok = false;
	return ok;
}

}  // namespace frame_diag
