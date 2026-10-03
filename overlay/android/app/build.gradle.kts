import java.util.Properties
import java.io.File
import org.gradle.api.DefaultTask
import org.gradle.api.file.DirectoryProperty
import org.gradle.api.file.RegularFileProperty
import org.gradle.api.tasks.InputFile
import org.gradle.api.tasks.OutputDirectory
import org.gradle.api.tasks.PathSensitive
import org.gradle.api.tasks.PathSensitivity
import org.gradle.api.tasks.TaskAction

abstract class StageNdkSharedStl : DefaultTask() {
	@get:InputFile
	@get:PathSensitive(PathSensitivity.NONE)
	abstract val sharedStl: RegularFileProperty

	@get:OutputDirectory
	abstract val outputDirectory: DirectoryProperty

	@TaskAction
	fun stage() {
		project.copy {
			from(sharedStl)
			into(outputDirectory.dir("arm64-v8a"))
		}
	}
}

plugins {
	id("com.android.application")
}

val questNdkVersion = "27.2.12479018"

// Developer tools require an explicit invocation: -PmiamivrDevTools=true.
// Debug APK packaging and native debug symbols are independent of this flag.
val questDeveloperTools = providers.gradleProperty("miamivrDevTools").orNull == "true"
// Build an unsigned release artifact explicitly; signing can happen outside the source tree.
val questUnsignedRelease = providers.gradleProperty("miamivrUnsignedRelease").orNull == "true"

// Optional external build directory; defaults to app/build.
System.getenv("MIAMIVR_ANDROID_BUILD_DIR")
	?.takeIf { it.isNotBlank() }
	?.let { layout.buildDirectory.set(file(it)) }

val releaseSigningPropertiesFile = System.getenv("MIAMIVR_RELEASE_SIGNING_PROPERTIES")
	?.takeIf { it.isNotBlank() }
	?.let { file(it) }
	?: rootProject.file("release-signing.properties")
val releaseSigningProperties = Properties()
if (!questUnsignedRelease && releaseSigningPropertiesFile.isFile) {
	releaseSigningPropertiesFile.inputStream().use {
		releaseSigningProperties.load(it)
	}
}

val releaseSigningKeys = listOf(
	"storeFile",
	"storePassword",
	"keyAlias",
	"keyPassword"
)
val hasReleaseSigning = releaseSigningKeys.all {
	!releaseSigningProperties.getProperty(it).isNullOrBlank()
}
val releaseBuildRequested = gradle.startParameter.taskNames.any {
	it.contains("Release", ignoreCase = true)
}
// Valve Steam Frame build type (assembleFrame); see docs/STEAM_FRAME.md.
val steamFrameBuildRequested = gradle.startParameter.taskNames.any {
	it.contains("Frame", ignoreCase = true)
}
if (releaseBuildRequested && !questUnsignedRelease && !hasReleaseSigning) {
	throw GradleException(
		"Release signing is not configured. Copy " +
			"release-signing.properties.example to release-signing.properties " +
			"and provide the private Quest release keystore values, set " +
			"MIAMIVR_RELEASE_SIGNING_PROPERTIES to an external config, or use " +
			"-PmiamivrUnsignedRelease=true for an unsigned artifact."
	)
}

android {
	namespace = "com.miamivr.quest"
	compileSdk = 35
	ndkVersion = questNdkVersion

	defaultConfig {
		applicationId = "com.miamivr.quest"
		// Quest 3 / 3S ship Android 12L or newer. Nothing below that is a target.
		// The Steam Frame's Android container (Lepton) is Android 11 / API 30,
		// and minSdk cannot vary per build type, so Frame invocations lower it.
		minSdk = if (steamFrameBuildRequested) 30 else 32
		targetSdk = 35
		versionCode = 522
		versionName = "0.5.6"

		ndk {
			abiFilters += "arm64-v8a"
		}

		externalNativeBuild {
			cmake {
				arguments += listOf(
					// Passed explicitly: CMake's option() caches its value, so
					// editing the default in CMakeLists.txt does not change an
					// already-configured build tree.
					"-DMIAMIVR_BRINGUP=OFF",
					"-DMIAMIVR_DEV_TOOLS=${if (questDeveloperTools) "ON" else "OFF"}",
					// Native optimization is independent of APK debuggability.
					"-DCMAKE_BUILD_TYPE=RelWithDebInfo",
					// Sanitizer runtimes are excluded from normal Quest packages.
					"-DMIAMIVR_ASAN=OFF",
					"-DANDROID_STL=c++_shared",
					// Legacy engine code requires both C++ runtime features.
					"-DANDROID_CPP_FEATURES=exceptions rtti"
				)
				cppFlags += listOf("-std=c++17")
			}
		}
	}

	buildFeatures {
		prefab = true
	}

	externalNativeBuild {
		cmake {
			path = file("src/main/cpp/CMakeLists.txt")
			version = "3.22.1"
		}
	}

	packaging {
		jniLibs {
			// Preserve extracted native libraries for update compatibility.
			useLegacyPackaging = true
			// Prefab also contributes this library. Keep exactly one copy even
			// when a clean/incremental build resolves both inputs.
			pickFirsts += "**/libopenxr_loader.so"
			// A normal externalNativeBuild also contributes this STL. The
			// generated jniLibs source below deliberately contributes a second
			// copy so cached/reused libmiamivr.so builds cannot produce an APK
			// without its runtime dependency.
			pickFirsts += "**/libc++_shared.so"
			// Diagnostic ASan libraries must not enter player packages.
			excludes += "**/libclang_rt.asan-*-android.so"
		}
		resources {
			excludes += "lib/*/wrap.sh"
		}
	}

	buildTypes {
		debug {
			isJniDebuggable = true
			isMinifyEnabled = false
		}
		release {
			isDebuggable = false
			isJniDebuggable = false
			isMinifyEnabled = false
			if (!questUnsignedRelease && hasReleaseSigning) {
				signingConfig = signingConfigs.create("questRelease") {
					storeFile = File(releaseSigningProperties.getProperty("storeFile")).let {
						if (it.isAbsolute) it else File(releaseSigningPropertiesFile.parentFile, it.path)
					}
					storePassword = releaseSigningProperties.getProperty(
						"storePassword"
					)
					keyAlias = releaseSigningProperties.getProperty("keyAlias")
					keyPassword = releaseSigningProperties.getProperty(
						"keyPassword"
					)
				}
			}
		}
		// Valve Steam Frame (Lepton, Android 11 on SteamVR): ./gradlew assembleFrame.
		// Everything Frame-specific hangs off this build type so the Quest
		// debug/release builds are untouched: CMake MIAMIVR_STEAM_FRAME, the
		// manifest in src/frame, a separate application id, and game data in
		// /sdcard/Documents/ViceCityVR, which survives Lepton resets.
		create("frame") {
			initWith(getByName("release"))
			matchingFallbacks += listOf("release")
			applicationIdSuffix = ".frame"
			versionNameSuffix = "-frame"
			// Sideloaded through adb and a devkit title; the debug key is enough.
			signingConfig = signingConfigs.getByName("debug")
			externalNativeBuild {
				cmake {
					arguments += listOf("-DMIAMIVR_STEAM_FRAME=ON")
				}
			}
		}
	}

	compileOptions {
		sourceCompatibility = JavaVersion.VERSION_17
		targetCompatibility = JavaVersion.VERSION_17
	}
}

// Do not rely on externalNativeBuild to package the shared C++ runtime. AGP can
// legitimately reuse a fresh libmiamivr.so while skipping buildCMake, and its
// implicit libc++_shared.so contribution then disappears. Registering the NDK
// runtime as a generated jniLibs source makes it an explicit input of every APK
// variant, including a release assembled entirely from Gradle's build cache.
val ndkPrebuiltHost = when {
	System.getProperty("os.name").startsWith("Windows", ignoreCase = true) ->
		"windows-x86_64"
	System.getProperty("os.name").startsWith("Mac", ignoreCase = true) ->
		"darwin-x86_64"
	else -> "linux-x86_64"
}

androidComponents {
	onVariants(selector().all()) { variant ->
		val taskSuffix = variant.name.replaceFirstChar {
			if (it.isLowerCase()) it.titlecase() else it.toString()
		}
		val stageSharedStl = tasks.register<StageNdkSharedStl>(
			"stage${taskSuffix}NdkSharedStl"
		) {
			sharedStl.set(
				sdkComponents.sdkDirectory.map { sdkDirectory ->
					sdkDirectory.file(
						"ndk/$questNdkVersion/toolchains/llvm/prebuilt/$ndkPrebuiltHost/" +
							"sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"
					)
				}
			)
			outputDirectory.set(
				layout.buildDirectory.dir("generated/miamivr-jniLibs/${variant.name}")
			)
		}
		variant.sources.jniLibs?.addGeneratedSourceDirectory(
			stageSharedStl,
			StageNdkSharedStl::outputDirectory
		)
	}
}

dependencies {
	// Khronos OpenXR loader for Android. Ships the arm64 loader plus a prefab
	// package, so the runtime on the headset is discovered through the standard
	// Android broker rather than a vendor SDK.
	implementation("org.khronos.openxr:openxr_loader_for_android:1.1.43")
}

// Keep javac out of the Gradle daemon.  On Windows/JDK 21 the in-process
// compiler can leave AGP's generated R.jar mounted through ZipFS and then fail
// the otherwise successful task with AccessDeniedException while closing it.
// A short-lived compiler process releases the archive deterministically.
tasks.withType<org.gradle.api.tasks.compile.JavaCompile>().configureEach {
	options.isFork = true
}
