#include "platform_android.h"

#include <android/native_activity.h>
#include <dirent.h>
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#if MIAMIVR_STEAM_FRAME
#include <dlfcn.h>
#include <signal.h>
#include <sys/syscall.h>
#include <ucontext.h>
#endif

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace platform {

static char gStorageRoot[512] = "";
static char gGameDataRoot[512] = "";
static JavaVM *gJavaVm = nullptr;
static jclass gMediaPlayerClass = nullptr;

struct MediaPlayerMethods {
	jmethodID ctor = nullptr;
	jmethodID setAudioStreamType = nullptr;
	jmethodID setDataSource = nullptr;
	jmethodID prepare = nullptr;
	jmethodID start = nullptr;
	jmethodID pause = nullptr;
	jmethodID stop = nullptr;
	jmethodID release = nullptr;
	jmethodID setLooping = nullptr;
	jmethodID setVolume = nullptr;
	jmethodID seekTo = nullptr;
	jmethodID getCurrentPosition = nullptr;
	jmethodID getDuration = nullptr;
	jmethodID isPlaying = nullptr;
};
static MediaPlayerMethods gMediaMethods;

struct MediaStreamState {
	std::mutex mutex;
	std::atomic<uint32_t> generation{0};
	jobject player = nullptr;
	bool requestedPlaying = false;
	bool preparing = false;
	bool loop = false;
	float left = 1.0f;
	float right = 1.0f;
	int positionMs = 0;
	int durationMs = 0;
	std::string cachePath;
};
static MediaStreamState gMediaStreams[3];

static JNIEnv *
getEnv(bool *attached)
{
	*attached = false;
	if(gJavaVm == nullptr)
		return nullptr;
	JNIEnv *env = nullptr;
	const jint status = gJavaVm->GetEnv((void **)&env, JNI_VERSION_1_6);
	if(status == JNI_OK)
		return env;
	if(status != JNI_EDETACHED ||
	   gJavaVm->AttachCurrentThread(&env, nullptr) != JNI_OK)
		return nullptr;
	*attached = true;
	return env;
}

static void
releaseEnv(bool attached)
{
	if(attached && gJavaVm != nullptr)
		gJavaVm->DetachCurrentThread();
}

static bool
checkJavaException(JNIEnv *env, const char *operation)
{
	if(!env->ExceptionCheck())
		return false;
	env->ExceptionDescribe();
	env->ExceptionClear();
	LOGE("Java audio exception in %s", operation);
	return true;
}

static void *
stdioPump(void *)
{
	static char line[1024];
	size_t used = 0;
	ssize_t got;
	char c;
	while((got = read(STDIN_FILENO, &c, 1)) == 1){
		if(c == '\n' || used == sizeof(line) - 1){
			line[used] = '\0';
			if(used > 0)
				__android_log_write(ANDROID_LOG_INFO, "MiamiVR-game", line);
			used = 0;
		}else if(c != '\r'){
			line[used++] = c;
		}
	}
	return nullptr;
}

static std::atomic<const char *> gCheckpoint{"start"};
static std::atomic<unsigned long> gCheckpointSeq{0};
#if MIAMIVR_STEAM_FRAME
// The thread that last passed a checkpoint, so a stall can be traced there.
static std::atomic<pid_t> gCheckpointTid{0};
#endif

void
setCheckpoint(const char *label)
{
	gCheckpoint.store(label, std::memory_order_relaxed);
	gCheckpointSeq.fetch_add(1, std::memory_order_relaxed);
#if MIAMIVR_STEAM_FRAME
	gCheckpointTid.store(gettid(), std::memory_order_relaxed);
#endif
}

#if MIAMIVR_STEAM_FRAME
// Steam Frame stall traces. A stalled game thread gets SIGURG (unused by the
// game); the handler walks its frame-pointer chain (arm64 Android keeps frame
// pointers) into a static buffer, and the watchdog resolves and logs it as
// "[vr-stall] #n lib+offset symbol". Symbolise offsets in libmiamivr.so with
// llvm-addr2line on the unstripped library from the build's .cxx tree.
static const int kStallFrames = 48;
static uintptr_t gStallPcs[kStallFrames];
static std::atomic<int> gStallPcCount{-1};

static void
stallTraceHandler(int, siginfo_t *, void *context)
{
	const ucontext_t *uc = (const ucontext_t *)context;
	int count = 0;
	gStallPcs[count++] = (uintptr_t)uc->uc_mcontext.pc;
	gStallPcs[count++] = (uintptr_t)uc->uc_mcontext.regs[30];   // lr
	uintptr_t fp = (uintptr_t)uc->uc_mcontext.regs[29];
	const uintptr_t sp = (uintptr_t)uc->uc_mcontext.sp;
	// Only follow a chain that stays on this stack, ascending and aligned.
	while(count < kStallFrames && fp >= sp && fp < sp + 8*1024*1024 &&
	      (fp & 15) == 0){
		const uintptr_t *frame = (const uintptr_t *)fp;
		const uintptr_t next = frame[0];
		const uintptr_t ret = frame[1];
		if(ret == 0)
			break;
		gStallPcs[count++] = ret;
		if(next <= fp)
			break;
		fp = next;
	}
	gStallPcCount.store(count, std::memory_order_release);
}

static void
logStallTrace(pid_t tid)
{
	static bool installed;
	if(!installed){
		struct sigaction action = {};
		action.sa_sigaction = stallTraceHandler;
		action.sa_flags = SA_SIGINFO | SA_RESTART;
		sigemptyset(&action.sa_mask);
		sigaction(SIGURG, &action, nullptr);
		installed = true;
	}
	if(tid <= 0)
		return;
	gStallPcCount.store(-1, std::memory_order_relaxed);
	if(syscall(SYS_tgkill, getpid(), tid, SIGURG) != 0){
		LOGE("[vr-stall] cannot signal thread %d", (int)tid);
		return;
	}
	int count = -1;
	for(int wait = 0; wait < 50 && count < 0; wait++){
		usleep(2000);
		count = gStallPcCount.load(std::memory_order_acquire);
	}
	if(count < 0){
		LOGE("[vr-stall] thread %d did not answer (blocked in the kernel?)",
		     (int)tid);
		return;
	}
	LOGE("[vr-stall] thread %d, %d frames:", (int)tid, count);
	for(int i = 0; i < count; i++){
		Dl_info info = {};
		const uintptr_t pc = gStallPcs[i];
		if(dladdr((const void *)pc, &info) && info.dli_fname != nullptr){
			const char *lib = strrchr(info.dli_fname, '/');
			LOGE("[vr-stall] #%02d %s+0x%lx %s", i,
			     lib ? lib + 1 : info.dli_fname,
			     (unsigned long)(pc - (uintptr_t)info.dli_fbase),
			     info.dli_sname ? info.dli_sname : "");
		}else{
			LOGE("[vr-stall] #%02d 0x%lx", i, (unsigned long)pc);
		}
	}
}
#endif

static void *
stallWatchdog(void *)
{
	unsigned long lastSeq = 0;
	int stalledFor = 0;
	for(;;){
		sleep(1);
		const unsigned long seq = gCheckpointSeq.load(std::memory_order_relaxed);
		if(seq != lastSeq){
			lastSeq = seq;
			stalledFor = 0;
			continue;
		}
		stalledFor++;
		if(stalledFor == 3 || stalledFor % 10 == 0){
			LOGE("stalled %ds at checkpoint '%s'", stalledFor,
			     gCheckpoint.load(std::memory_order_relaxed));
#if MIAMIVR_STEAM_FRAME
			logStallTrace(gCheckpointTid.load(std::memory_order_relaxed));
#endif
		}
	}
	return nullptr;
}

void
startStallWatchdog(void)
{
	pthread_t thread;
	if(pthread_create(&thread, nullptr, stallWatchdog, nullptr) == 0)
		pthread_detach(thread);
}

void
redirectStdioToLog(void)
{
	setvbuf(stdout, nullptr, _IOLBF, 0);
	setvbuf(stderr, nullptr, _IONBF, 0);

	int fds[2];
	if(pipe(fds) != 0){
		LOGE("cannot create the stdio pipe");
		return;
	}
	// Read end becomes stdin for the pump thread, so it can use the plain
	// read() above without carrying the descriptor around.
	dup2(fds[0], STDIN_FILENO);
	dup2(fds[1], STDOUT_FILENO);
	dup2(fds[1], STDERR_FILENO);

	pthread_t thread;
	if(pthread_create(&thread, nullptr, stdioPump, nullptr) != 0){
		LOGE("cannot start the stdio pump");
		return;
	}
	pthread_detach(thread);
	LOGI("stdout/stderr now go to logcat as MiamiVR-game");
}

void
setStorageRoot(const char *path)
{
	if(path == nullptr || path[0] == '\0')
		return;
	snprintf(gStorageRoot, sizeof(gStorageRoot), "%s", path);
	snprintf(gGameDataRoot, sizeof(gGameDataRoot), "%s/gamedata", path);
	mkdir(gGameDataRoot, 0770);
	LOGI("storage root: %s", gStorageRoot);
	LOGI("game data root: %s", gGameDataRoot);
}

void
setNativeActivity(ANativeActivity *activity)
{
	if(activity == nullptr)
		return;
	gJavaVm = activity->vm;
	bool attached = false;
	JNIEnv *env = getEnv(&attached);
	if(env == nullptr)
		return;
	jclass localClass = env->FindClass("android/media/MediaPlayer");
	if(localClass == nullptr || checkJavaException(env, "MediaPlayer class")){
		releaseEnv(attached);
		return;
	}
	gMediaPlayerClass = (jclass)env->NewGlobalRef(localClass);
	env->DeleteLocalRef(localClass);
	gMediaMethods.ctor = env->GetMethodID(gMediaPlayerClass, "<init>", "()V");
	gMediaMethods.setAudioStreamType = env->GetMethodID(gMediaPlayerClass,
		"setAudioStreamType", "(I)V");
	gMediaMethods.setDataSource = env->GetMethodID(gMediaPlayerClass,
		"setDataSource", "(Ljava/lang/String;)V");
	gMediaMethods.prepare = env->GetMethodID(gMediaPlayerClass, "prepare", "()V");
	gMediaMethods.start = env->GetMethodID(gMediaPlayerClass, "start", "()V");
	gMediaMethods.pause = env->GetMethodID(gMediaPlayerClass, "pause", "()V");
	gMediaMethods.stop = env->GetMethodID(gMediaPlayerClass, "stop", "()V");
	gMediaMethods.release = env->GetMethodID(gMediaPlayerClass, "release", "()V");
	gMediaMethods.setLooping = env->GetMethodID(gMediaPlayerClass, "setLooping", "(Z)V");
	gMediaMethods.setVolume = env->GetMethodID(gMediaPlayerClass, "setVolume", "(FF)V");
	gMediaMethods.seekTo = env->GetMethodID(gMediaPlayerClass, "seekTo", "(I)V");
	gMediaMethods.getCurrentPosition = env->GetMethodID(gMediaPlayerClass,
		"getCurrentPosition", "()I");
	gMediaMethods.getDuration = env->GetMethodID(gMediaPlayerClass,
		"getDuration", "()I");
	gMediaMethods.isPlaying = env->GetMethodID(gMediaPlayerClass, "isPlaying", "()Z");
	checkJavaException(env, "setNativeActivity");
	releaseEnv(attached);
}

static void
releasePlayer(JNIEnv *env, jobject player)
{
	if(player == nullptr)
		return;
	env->CallVoidMethod(player, gMediaMethods.stop);
	if(env->ExceptionCheck())
		env->ExceptionClear();
	env->CallVoidMethod(player, gMediaMethods.release);
	if(env->ExceptionCheck())
		env->ExceptionClear();
	env->DeleteGlobalRef(player);
}

static void
releasePlayerAsync(jobject player, std::string cachePath)
{
	if(player == nullptr && cachePath.empty())
		return;
	std::thread([player, cachePath]() {
		bool attached = false;
		if(player != nullptr){
			JNIEnv *env = getEnv(&attached);
			if(env != nullptr)
				releasePlayer(env, player);
			else
				LOGE("could not attach MediaPlayer release worker");
		}
		if(!cachePath.empty())
			unlink(cachePath.c_str());
		releaseEnv(attached);
	}).detach();
}

static bool
isAdf(const std::string &path)
{
	return path.size() >= 4 &&
		strcasecmp(path.c_str() + path.size() - 4, ".adf") == 0;
}

static bool
decryptAdf(const std::string &source, const std::string &destination)
{
	FILE *in = fopen(source.c_str(), "rb");
	if(in == nullptr)
		return false;
	FILE *out = fopen(destination.c_str(), "wb");
	if(out == nullptr){
		fclose(in);
		return false;
	}
	uint8_t buffer[256 * 1024];
	bool ok = true;
	for(;;){
		const size_t count = fread(buffer, 1, sizeof(buffer), in);
		if(count == 0)
			break;
		for(size_t i = 0; i < count; ++i)
			buffer[i] ^= 0x22;
		if(fwrite(buffer, 1, count, out) != count){
			ok = false;
			break;
		}
	}
	if(ferror(in))
		ok = false;
	fclose(out);
	fclose(in);
	if(!ok)
		unlink(destination.c_str());
	return ok;
}

enum class ImaWavConversion {
	NotImaAdpcm,
	Converted,
	Failed
};

static uint16_t
readLe16(const uint8_t *data)
{
	return uint16_t(data[0]) | uint16_t(data[1]) << 8;
}

static uint32_t
readLe32(const uint8_t *data)
{
	return uint32_t(data[0]) | uint32_t(data[1]) << 8 |
		uint32_t(data[2]) << 16 | uint32_t(data[3]) << 24;
}

static void
writeLe16(uint8_t *data, uint16_t value)
{
	data[0] = uint8_t(value);
	data[1] = uint8_t(value >> 8);
}

static void
writeLe32(uint8_t *data, uint32_t value)
{
	data[0] = uint8_t(value);
	data[1] = uint8_t(value >> 8);
	data[2] = uint8_t(value >> 16);
	data[3] = uint8_t(value >> 24);
}

static int16_t
decodeImaNibble(uint8_t nibble, int &predictor, int &stepIndex)
{
	static const int stepTable[89] = {
		7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28,
		31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
		118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
		337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876,
		963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
		2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
		5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
		12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
		27086, 29794, 32767
	};
	static const int indexTable[16] = {
		-1, -1, -1, -1, 2, 4, 6, 8,
		-1, -1, -1, -1, 2, 4, 6, 8
	};

	const int step = stepTable[stepIndex];
	int difference = step >> 3;
	if(nibble & 1) difference += step >> 2;
	if(nibble & 2) difference += step >> 1;
	if(nibble & 4) difference += step;
	predictor += nibble & 8 ? -difference : difference;
	predictor = std::max(-32768, std::min(32767, predictor));
	stepIndex += indexTable[nibble & 0x0F];
	stepIndex = std::max(0, std::min(88, stepIndex));
	return int16_t(predictor);
}

// Android's MediaPlayer WAV extractor does not accept the IMA ADPCM (0x0011)
// mission-dialogue files shipped by Vice City. Decode them off the game thread
// into a short-lived PCM WAV which MediaPlayer can consume normally.
static ImaWavConversion
convertImaAdpcmWav(const std::string &source, const std::string &destination)
{
	if(source.size() < 4 ||
	   strcasecmp(source.c_str() + source.size() - 4, ".wav") != 0)
		return ImaWavConversion::NotImaAdpcm;

	FILE *input = fopen(source.c_str(), "rb");
	if(input == nullptr)
		return ImaWavConversion::Failed;
	if(fseek(input, 0, SEEK_END) != 0){
		fclose(input);
		return ImaWavConversion::Failed;
	}
	const long inputSize = ftell(input);
	if(inputSize < 12 || fseek(input, 0, SEEK_SET) != 0){
		fclose(input);
		return ImaWavConversion::NotImaAdpcm;
	}
	std::vector<uint8_t> fileData(static_cast<size_t>(inputSize));
	const bool readOk =
		fread(fileData.data(), 1, fileData.size(), input) == fileData.size();
	fclose(input);
	if(!readOk)
		return ImaWavConversion::Failed;
	if(memcmp(fileData.data(), "RIFF", 4) != 0 ||
	   memcmp(fileData.data() + 8, "WAVE", 4) != 0)
		return ImaWavConversion::NotImaAdpcm;

	const uint8_t *format = nullptr;
	size_t formatSize = 0;
	const uint8_t *compressed = nullptr;
	size_t compressedSize = 0;
	uint32_t factSamples = 0;
	for(size_t offset = 12; offset + 8 <= fileData.size();){
		const uint8_t *chunk = fileData.data() + offset;
		const uint32_t chunkSize = readLe32(chunk + 4);
		const size_t dataOffset = offset + 8;
		if(dataOffset > fileData.size() ||
		   size_t(chunkSize) > fileData.size() - dataOffset)
			return ImaWavConversion::Failed;
		if(memcmp(chunk, "fmt ", 4) == 0){
			format = fileData.data() + dataOffset;
			formatSize = chunkSize;
		}else if(memcmp(chunk, "data", 4) == 0){
			compressed = fileData.data() + dataOffset;
			compressedSize = chunkSize;
		}else if(memcmp(chunk, "fact", 4) == 0 && chunkSize >= 4){
			factSamples = readLe32(fileData.data() + dataOffset);
		}
		const size_t paddedSize = size_t(chunkSize) + (chunkSize & 1u);
		if(paddedSize > fileData.size() - dataOffset)
			break;
		offset = dataOffset + paddedSize;
	}

	if(format == nullptr || formatSize < 16)
		return ImaWavConversion::NotImaAdpcm;
	if(readLe16(format) != 0x0011)
		return ImaWavConversion::NotImaAdpcm;
	if(formatSize < 20 || compressed == nullptr || compressedSize < 4)
		return ImaWavConversion::Failed;

	const uint16_t channels = readLe16(format + 2);
	const uint32_t sampleRate = readLe32(format + 4);
	const uint16_t blockAlign = readLe16(format + 12);
	const uint16_t bitsPerSample = readLe16(format + 14);
	const uint16_t samplesPerBlock = readLe16(format + 18);
	if(channels != 1 || sampleRate == 0 || blockAlign < 4 ||
	   bitsPerSample != 4 || samplesPerBlock == 0)
		return ImaWavConversion::Failed;

	const size_t fullBlocks = compressedSize / blockAlign;
	const size_t trailingBytes = compressedSize % blockAlign;
	size_t estimatedSamples = fullBlocks * size_t(samplesPerBlock);
	if(trailingBytes >= 4)
		estimatedSamples += 1 + (trailingBytes - 4) * 2;
	if(factSamples != 0)
		estimatedSamples = std::min(estimatedSamples, size_t(factSamples));
	std::vector<int16_t> pcm;
	pcm.reserve(estimatedSamples);

	size_t offset = 0;
	while(offset + 4 <= compressedSize &&
	      (factSamples == 0 || pcm.size() < factSamples)){
		const size_t bytesInBlock =
			std::min(size_t(blockAlign), compressedSize - offset);
		const uint8_t *block = compressed + offset;
		int predictor = int16_t(readLe16(block));
		int stepIndex = block[2];
		if(stepIndex > 88)
			return ImaWavConversion::Failed;
		pcm.push_back(int16_t(predictor));
		size_t samplesInBlock = 1;
		for(size_t i = 4; i < bytesInBlock &&
		    samplesInBlock < samplesPerBlock; ++i){
			pcm.push_back(decodeImaNibble(block[i] & 0x0F,
				predictor, stepIndex));
			++samplesInBlock;
			if(samplesInBlock >= samplesPerBlock ||
			   (factSamples != 0 && pcm.size() >= factSamples))
				break;
			pcm.push_back(decodeImaNibble(block[i] >> 4,
				predictor, stepIndex));
			++samplesInBlock;
			if(factSamples != 0 && pcm.size() >= factSamples)
				break;
		}
		offset += bytesInBlock;
	}
	if(factSamples != 0 && pcm.size() > factSamples)
		pcm.resize(factSamples);
	if(pcm.empty() || pcm.size() > (UINT32_MAX - 36u) / 2u)
		return ImaWavConversion::Failed;

	const uint32_t pcmBytes = uint32_t(pcm.size() * sizeof(int16_t));
	uint8_t header[44] = {};
	memcpy(header, "RIFF", 4);
	writeLe32(header + 4, 36u + pcmBytes);
	memcpy(header + 8, "WAVEfmt ", 8);
	writeLe32(header + 16, 16);
	writeLe16(header + 20, 1);
	writeLe16(header + 22, 1);
	writeLe32(header + 24, sampleRate);
	writeLe32(header + 28, sampleRate * sizeof(int16_t));
	writeLe16(header + 32, sizeof(int16_t));
	writeLe16(header + 34, 16);
	memcpy(header + 36, "data", 4);
	writeLe32(header + 40, pcmBytes);

	FILE *output = fopen(destination.c_str(), "wb");
	if(output == nullptr)
		return ImaWavConversion::Failed;
	const bool writeOk =
		fwrite(header, 1, sizeof(header), output) == sizeof(header) &&
		fwrite(pcm.data(), sizeof(int16_t), pcm.size(), output) == pcm.size() &&
		fflush(output) == 0;
	fclose(output);
	if(!writeOk){
		unlink(destination.c_str());
		return ImaWavConversion::Failed;
	}
	return ImaWavConversion::Converted;
}

bool
audioLoadStream(int stream, const char *absolutePath, int positionMs,
	bool loop, bool autoStart)
{
	if(stream < 0 || stream >= 3 || absolutePath == nullptr ||
	   gMediaPlayerClass == nullptr)
		return false;
	struct stat st{};
	if(stat(absolutePath, &st) != 0 || !S_ISREG(st.st_mode))
		return false;

	MediaStreamState &state = gMediaStreams[stream];
	const uint32_t generation = state.generation.fetch_add(1) + 1;
	jobject retiredPlayer = nullptr;
	std::string retiredCachePath;
	{
		std::lock_guard<std::mutex> lock(state.mutex);
		retiredPlayer = state.player;
		state.player = nullptr;
		retiredCachePath.swap(state.cachePath);
		state.requestedPlaying = autoStart;
		state.preparing = true;
		state.loop = loop;
		state.positionMs = positionMs < 0 ? 0 : positionMs;
		state.durationMs = 0;
	}
	releasePlayerAsync(retiredPlayer, retiredCachePath);

	const std::string source = absolutePath;
	std::thread([stream, generation, source]() {
		MediaStreamState &workerState = gMediaStreams[stream];
		const auto markPreparationFailed = [&workerState, generation]() {
			std::lock_guard<std::mutex> lock(workerState.mutex);
			if(generation == workerState.generation.load())
				workerState.preparing = false;
		};
		std::string decoderPath = source;
		std::string temporary;
		if(isAdf(source)){
			char path[640];
			snprintf(path, sizeof(path), "%s/.miamivr-radio-%d-%u.mp3",
				gStorageRoot, stream, generation);
			temporary = path;
			if(!decryptAdf(source, temporary)){
				LOGE("could not decrypt radio stream %s", source.c_str());
				markPreparationFailed();
				return;
			}
			decoderPath = temporary;
		}else{
			char path[640];
			snprintf(path, sizeof(path), "%s/.miamivr-stream-%d-%u.wav",
				gStorageRoot, stream, generation);
			const ImaWavConversion conversion =
				convertImaAdpcmWav(source, path);
			if(conversion == ImaWavConversion::Failed){
				LOGE("could not decode IMA ADPCM stream %s", source.c_str());
				markPreparationFailed();
				return;
			}
			if(conversion == ImaWavConversion::Converted){
				temporary = path;
				decoderPath = temporary;
				LOGI("decoded IMA ADPCM stream %s to PCM", source.c_str());
			}
		}

		bool workerAttached = false;
		JNIEnv *workerEnv = getEnv(&workerAttached);
		if(workerEnv == nullptr){
			if(!temporary.empty()) unlink(temporary.c_str());
			markPreparationFailed();
			return;
		}

		jobject local = workerEnv->NewObject(gMediaPlayerClass, gMediaMethods.ctor);
		jobject player = local ? workerEnv->NewGlobalRef(local) : nullptr;
		if(local) workerEnv->DeleteLocalRef(local);
		bool ok = player != nullptr && !checkJavaException(workerEnv, "MediaPlayer ctor");
		if(ok){
			workerEnv->CallVoidMethod(player, gMediaMethods.setAudioStreamType, 3);
			jstring path = workerEnv->NewStringUTF(decoderPath.c_str());
			workerEnv->CallVoidMethod(player, gMediaMethods.setDataSource, path);
			workerEnv->DeleteLocalRef(path);
			workerEnv->CallVoidMethod(player, gMediaMethods.prepare);
			ok = !checkJavaException(workerEnv, "MediaPlayer prepare");
		}

		if(!ok){
			releasePlayer(workerEnv, player);
			if(!temporary.empty()) unlink(temporary.c_str());
			markPreparationFailed();
			releaseEnv(workerAttached);
			return;
		}

		bool stale = false;
		{
			std::lock_guard<std::mutex> lock(workerState.mutex);
			stale = generation != workerState.generation.load();
			if(!stale){
				workerEnv->CallVoidMethod(player, gMediaMethods.setLooping,
					(jboolean)workerState.loop);
				workerEnv->CallVoidMethod(player, gMediaMethods.setVolume,
					(jfloat)workerState.left, (jfloat)workerState.right);
				if(workerState.positionMs > 0)
					workerEnv->CallVoidMethod(player, gMediaMethods.seekTo,
						(jint)workerState.positionMs);
				workerState.durationMs = workerEnv->CallIntMethod(player,
					gMediaMethods.getDuration);
				workerState.player = player;
				workerState.cachePath = temporary;
				workerState.preparing = false;
				if(workerState.requestedPlaying)
					workerEnv->CallVoidMethod(player, gMediaMethods.start);
				checkJavaException(workerEnv, "MediaPlayer start");
			}
		}
		if(stale){
			releasePlayer(workerEnv, player);
			if(!temporary.empty())
				unlink(temporary.c_str());
		}
		releaseEnv(workerAttached);
	}).detach();
	return true;
}

void audioPlayStream(int stream)
{
	if(stream < 0 || stream >= 3) return;
	MediaStreamState &state = gMediaStreams[stream];
	bool attached = false; JNIEnv *env = getEnv(&attached);
	if(env){
		std::lock_guard<std::mutex> lock(state.mutex);
		state.requestedPlaying = true;
		if(state.player) env->CallVoidMethod(state.player, gMediaMethods.start);
		checkJavaException(env, "audioPlay");
	}
	releaseEnv(attached);
}

void audioPauseStream(int stream, bool pause)
{
	if(stream < 0 || stream >= 3) return;
	MediaStreamState &state = gMediaStreams[stream];
	bool attached = false; JNIEnv *env = getEnv(&attached);
	if(env){
		std::lock_guard<std::mutex> lock(state.mutex);
		state.requestedPlaying = !pause;
		if(state.player) env->CallVoidMethod(state.player,
			pause ? gMediaMethods.pause : gMediaMethods.start);
		checkJavaException(env, "audioPause");
	}
	releaseEnv(attached);
}

void audioStopStream(int stream)
{
	if(stream < 0 || stream >= 3) return;
	MediaStreamState &state = gMediaStreams[stream];
	state.generation.fetch_add(1);
	jobject retiredPlayer = nullptr;
	std::string retiredCachePath;
	{
		std::lock_guard<std::mutex> lock(state.mutex);
		state.requestedPlaying = false;
		state.preparing = false;
		retiredPlayer = state.player;
		state.player = nullptr;
		retiredCachePath.swap(state.cachePath);
	}
	releasePlayerAsync(retiredPlayer, retiredCachePath);
}

void audioSetStreamVolume(int stream, float left, float right)
{
	if(stream < 0 || stream >= 3) return;
	MediaStreamState &state = gMediaStreams[stream];
	bool attached = false; JNIEnv *env = getEnv(&attached);
	if(env){
		std::lock_guard<std::mutex> lock(state.mutex);
		state.left = std::max(0.0f, std::min(1.0f, left));
		state.right = std::max(0.0f, std::min(1.0f, right));
		if(state.player) env->CallVoidMethod(state.player, gMediaMethods.setVolume,
			(jfloat)state.left, (jfloat)state.right);
		checkJavaException(env, "audioSetVolume");
	}
	releaseEnv(attached);
}

void audioSetStreamLoop(int stream, bool loop)
{
	if(stream < 0 || stream >= 3) return;
	MediaStreamState &state = gMediaStreams[stream];
	bool attached = false; JNIEnv *env = getEnv(&attached);
	if(env){
		std::lock_guard<std::mutex> lock(state.mutex);
		state.loop = loop;
		if(state.player) env->CallVoidMethod(state.player,
			gMediaMethods.setLooping, (jboolean)loop);
		checkJavaException(env, "audioSetLoop");
	}
	releaseEnv(attached);
}

int audioGetStreamPosition(int stream)
{
	if(stream < 0 || stream >= 3) return 0;
	MediaStreamState &state = gMediaStreams[stream];
	bool attached = false; JNIEnv *env = getEnv(&attached); jint value = 0;
	if(env){
		std::lock_guard<std::mutex> lock(state.mutex);
		if(state.player)
			value = env->CallIntMethod(state.player, gMediaMethods.getCurrentPosition);
		else
			value = state.positionMs;
		checkJavaException(env, "audioGetPosition");
	}
	releaseEnv(attached); return value;
}

int audioGetStreamDuration(int stream)
{
	if(stream < 0 || stream >= 3) return 0;
	std::lock_guard<std::mutex> lock(gMediaStreams[stream].mutex);
	return gMediaStreams[stream].durationMs;
}

bool audioIsStreamPlaying(int stream)
{
	if(stream < 0 || stream >= 3) return false;
	MediaStreamState &state = gMediaStreams[stream];
	bool attached = false; JNIEnv *env = getEnv(&attached); jboolean value = JNI_FALSE;
	if(env){
		std::lock_guard<std::mutex> lock(state.mutex);
		if(state.preparing)
			value = state.requestedPlaying;
		else if(state.player)
			value = env->CallBooleanMethod(state.player, gMediaMethods.isPlaying);
		checkJavaException(env, "audioIsPlaying");
	}
	releaseEnv(attached); return value == JNI_TRUE;
}

const char *storageRoot(void) { return gStorageRoot; }
const char *gameDataRoot(void) { return gGameDataRoot; }

char *
resolveGamePath(char *buffer, size_t bufferSize, const char *relative)
{
	while(*relative == '\\' || *relative == '/')
		relative++;
	snprintf(buffer, bufferSize, "%s/%s", gGameDataRoot, relative);
	for(char *p = buffer; *p; p++)
		if(*p == '\\')
			*p = '/';
	return buffer;
}

// Walks the requested path one component at a time, matching each against the
// real directory entries without regard to case. Only the components that do
// not already exist verbatim cost a directory scan.
bool
resolveGamePathCaseInsensitive(char *buffer, size_t bufferSize, const char *relative)
{
	char normalised[512];
	resolveGamePath(normalised, sizeof(normalised), relative);

	struct stat st;
	if(stat(normalised, &st) == 0){
		snprintf(buffer, bufferSize, "%s", normalised);
		return true;
	}

	std::string resolved = gGameDataRoot;
	const char *cursor = normalised + strlen(gGameDataRoot);
	while(*cursor == '/')
		cursor++;

	std::string remainder = cursor;
	size_t start = 0;
	while(start <= remainder.size()){
		size_t slash = remainder.find('/', start);
		std::string component = remainder.substr(
			start, slash == std::string::npos ? std::string::npos : slash - start);
		if(component.empty() && slash == std::string::npos)
			break;

		std::string candidate = resolved + "/" + component;
		if(stat(candidate.c_str(), &st) != 0){
			DIR *dir = opendir(resolved.c_str());
			if(dir == nullptr)
				return false;
			bool found = false;
			struct dirent *entry;
			while((entry = readdir(dir)) != nullptr){
				if(strcasecmp(entry->d_name, component.c_str()) == 0){
					candidate = resolved + "/" + entry->d_name;
					found = true;
					break;
				}
			}
			closedir(dir);
			if(!found)
				return false;
		}
		resolved = candidate;

		if(slash == std::string::npos)
			break;
		start = slash + 1;
	}

	snprintf(buffer, bufferSize, "%s", resolved.c_str());
	return true;
}

} // namespace platform

// ---------------------------------------------------------------------------
// INI implementation
// ---------------------------------------------------------------------------

namespace {

std::string
profilePath(const char *fileName)
{
	std::string path = fileName != nullptr ? fileName : "";
	// Desktop call sites intentionally use Win32-style ".\\name.ini".
	// Android fopen treats backslash as a literal filename character, so
	// canonicalize separators while keeping the same relative data directory.
	for(char &c : path)
		if(c == '\\')
			c = '/';
	return path;
}

char *
trim(char *s)
{
	while(*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
		s++;
	char *end = s + strlen(s);
	while(end > s && (end[-1] == ' ' || end[-1] == '\t' ||
	                  end[-1] == '\r' || end[-1] == '\n'))
		end--;
	*end = '\0';
	return s;
}

bool
readLines(const char *fileName, std::vector<std::string> &lines)
{
	const std::string path = profilePath(fileName);
	FILE *f = fopen(path.c_str(), "rb");
	// Builds before path normalization created a literal Android filename
	// containing the Win32 backslash (".\vr_settings.ini"). Read it once as
	// a fallback; the next profile write migrates the same contents into the
	// canonical "./vr_settings.ini" without discarding user calibration.
	if(f == nullptr && fileName != nullptr && path != fileName)
		f = fopen(fileName, "rb");
	if(f == nullptr)
		return false;
	std::string current;
	int c;
	while((c = fgetc(f)) != EOF){
		if(c == '\n'){
			lines.push_back(current);
			current.clear();
		}else if(c != '\r'){
			current.push_back((char)c);
		}
	}
	if(!current.empty())
		lines.push_back(current);
	fclose(f);
	return true;
}

// Returns true when the line is "[wanted]".
bool
isSectionHeader(const std::string &line, std::string *nameOut)
{
	std::string copy = line;
	char *t = trim(&copy[0]);
	size_t len = strlen(t);
	if(len < 2 || t[0] != '[' || t[len-1] != ']')
		return false;
	if(nameOut != nullptr)
		nameOut->assign(t + 1, len - 2);
	return true;
}

// Splits "key = value"; returns false for blanks and comments.
bool
splitEntry(const std::string &line, std::string *keyOut, std::string *valueOut)
{
	std::string copy = line;
	char *t = trim(&copy[0]);
	if(t[0] == '\0' || t[0] == ';' || t[0] == '#')
		return false;
	char *eq = strchr(t, '=');
	if(eq == nullptr)
		return false;
	*eq = '\0';
	if(keyOut != nullptr)
		keyOut->assign(trim(t));
	if(valueOut != nullptr)
		valueOut->assign(trim(eq + 1));
	return true;
}

bool
lookup(const char *section, const char *key, const char *fileName,
       std::string &valueOut)
{
	std::vector<std::string> lines;
	if(!readLines(fileName, lines))
		return false;

	bool inSection = false;
	std::string name, entryKey, entryValue;
	for(const std::string &line : lines){
		if(isSectionHeader(line, &name)){
			inSection = strcasecmp(name.c_str(), section) == 0;
			continue;
		}
		if(!inSection)
			continue;
		if(!splitEntry(line, &entryKey, &entryValue))
			continue;
		if(strcasecmp(entryKey.c_str(), key) == 0){
			valueOut = entryValue;
			return true;
		}
	}
	return false;
}

} // namespace

extern "C" int32_t
GetPrivateProfileIntA(const char *section, const char *key,
                      int32_t defaultValue, const char *fileName)
{
	std::string value;
	if(!lookup(section, key, fileName, value) || value.empty())
		return defaultValue;
	// Win32 stops at the first non-numeric character and treats a leading
	// minus as valid; strtol matches that closely enough for the settings and
	// calibration values the VR layer stores.
	return (int32_t)strtol(value.c_str(), nullptr, 10);
}

extern "C" uint32_t
GetPrivateProfileStringA(const char *section, const char *key,
                         const char *defaultValue, char *out,
                         uint32_t outSize, const char *fileName)
{
	if(out == nullptr || outSize == 0)
		return 0;

	std::string value;
	if(!lookup(section, key, fileName, value))
		value = defaultValue != nullptr ? defaultValue : "";

	uint32_t copied = (uint32_t)value.size();
	if(copied > outSize - 1)
		copied = outSize - 1;
	memcpy(out, value.c_str(), copied);
	out[copied] = '\0';
	return copied;
}

extern "C" int32_t
WritePrivateProfileStringA(const char *section, const char *key,
                           const char *value, const char *fileName)
{
	std::vector<std::string> lines;
	readLines(fileName, lines);	// absent file is fine, we create it

	std::string name, entryKey;
	int sectionStart = -1;
	int sectionEnd = (int)lines.size();

	for(size_t i = 0; i < lines.size(); i++){
		if(!isSectionHeader(lines[i], &name))
			continue;
		if(sectionStart >= 0){
			sectionEnd = (int)i;
			break;
		}
		if(strcasecmp(name.c_str(), section) == 0)
			sectionStart = (int)i;
	}

	std::string entry = std::string(key) + " = " + (value ? value : "");

	if(sectionStart < 0){
		if(!lines.empty() && !lines.back().empty())
			lines.push_back("");
		lines.push_back(std::string("[") + section + "]");
		lines.push_back(entry);
	}else{
		bool replaced = false;
		for(int i = sectionStart + 1; i < sectionEnd; i++){
			if(!splitEntry(lines[i], &entryKey, nullptr))
				continue;
			if(strcasecmp(entryKey.c_str(), key) == 0){
				lines[i] = entry;
				replaced = true;
				break;
			}
		}
		if(!replaced){
			// Append at the end of the section, before any trailing blanks,
			// so repeated writes do not accumulate empty lines.
			int insertAt = sectionEnd;
			while(insertAt > sectionStart + 1 && lines[insertAt-1].empty())
				insertAt--;
			lines.insert(lines.begin() + insertAt, entry);
		}
	}

	const std::string path = profilePath(fileName);
	FILE *f = fopen(path.c_str(), "wb");
	if(f == nullptr){
		LOGE("WritePrivateProfileStringA: cannot open %s", path.c_str());
		return 0;
	}
	for(const std::string &line : lines)
		fprintf(f, "%s\n", line.c_str());
	fclose(f);
	return 1;
}
