cmake_minimum_required(VERSION 3.22.1)
project(miamivr C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Repository root, from android/app/src/main/cpp
get_filename_component(REVC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(LIBRW_DIR "${REVC_ROOT}/vendor/librw")

# Optional OpenXR/Vulkan-only target for toolchain diagnostics.
option(MIAMIVR_BRINGUP "Build only the OpenXR/Vulkan bring-up target" OFF)

# Explicit opt-in; neither Gradle debug nor RelWithDebInfo enables tools.
# Keep C++ and every shader on the same definition because the debug scene
# uniform and draw tint do not exist in player builds.
option(MIAMIVR_DEV_TOOLS "Include developer-only diagnostics and test tools" OFF)

# Valve Steam Frame build (Gradle build type "frame"): SteamVR's OpenXR runtime
# inside Lepton, the Frame's Android 11 container. Off for every Quest build.
option(MIAMIVR_STEAM_FRAME "Build for the Valve Steam Frame (Lepton)" OFF)
if(MIAMIVR_STEAM_FRAME)
	message(STATUS "miamivr: Steam Frame build")
endif()

# Optional AddressSanitizer instrumentation for native memory diagnostics.
option(MIAMIVR_ASAN "Build with AddressSanitizer" OFF)
if(MIAMIVR_ASAN)
	add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
	add_link_options(-fsanitize=address)
	message(STATUS "miamivr: AddressSanitizer enabled")
endif()

# The AAR is published as org.khronos.openxr:openxr_loader_for_android, but the
# prefab package it exposes is named OpenXR, with targets OpenXR::openxr_loader
# and OpenXR::headers.
find_package(OpenXR REQUIRED CONFIG)

# ---------------------------------------------------------------------------
# Shaders: GLSL -> SPIR-V, embedded as C arrays.
# ---------------------------------------------------------------------------
find_program(GLSLC glslc
	HINTS "${ANDROID_NDK}/shader-tools/${ANDROID_HOST_TAG}"
	      "${ANDROID_NDK}/shader-tools/windows-x86_64"
	      "${ANDROID_NDK}/shader-tools/linux-x86_64"
	REQUIRED)

set(SPIRV_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/spirv")
file(MAKE_DIRECTORY "${SPIRV_OUT_DIR}")

# compile_shader(<source> <stage> <symbol> <outdir> <includedir>)
# Emits a C initialiser list, consumed as:  static const uint32_t x[] =
# #include "x.h"
# ;
function(compile_shader SRC STAGE SYMBOL OUTDIR INCDIR)
	set(OUT "${OUTDIR}/${SYMBOL}.h")
	# Track GLSL includes so uniform changes rebuild dependent shaders.
	add_custom_command(
		OUTPUT "${OUT}"
		COMMAND "${GLSLC}" -fshader-stage=${STAGE} --target-env=vulkan1.1 -O ${ARGN}
		        -DMIAMIVR_DEV_TOOLS=$<BOOL:${MIAMIVR_DEV_TOOLS}>
		        -I "${INCDIR}" -MD -MF "${OUT}.d" -mfmt=c -o "${OUT}" "${SRC}"
		DEPENDS "${SRC}"
		DEPFILE "${OUT}.d"
		COMMENT "glslc ${SYMBOL}"
		VERBATIM)
	set(SHADER_OUTPUTS ${SHADER_OUTPUTS} "${OUT}" PARENT_SCOPE)
endfunction()

# App bring-up shaders
set(SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(SHADER_OUTPUTS "")
compile_shader("${SHADER_DIR}/bringup.vert" vertex bringup_vert "${SPIRV_OUT_DIR}" "${SHADER_DIR}")
set(MIAMIVR_SPIRV_HEADERS ${SHADER_OUTPUTS})
compile_shader("${SHADER_DIR}/bringup.frag" fragment bringup_frag "${SPIRV_OUT_DIR}" "${SHADER_DIR}")
list(APPEND MIAMIVR_SPIRV_HEADERS ${SHADER_OUTPUTS})

add_custom_target(miamivr_shaders DEPENDS ${MIAMIVR_SPIRV_HEADERS})

# librw backend shaders. They live with the backend, and rw_common.glsl is
# pulled in by all of them, hence the include directory.
set(LIBRW_SHADER_DIR "${LIBRW_DIR}/src/vulkan/shaders")
set(LIBRW_SPIRV_DIR "${CMAKE_CURRENT_BINARY_DIR}/spirv-librw")
file(MAKE_DIRECTORY "${LIBRW_SPIRV_DIR}")

set(LIBRW_SPIRV_HEADERS "")
foreach(SHADER rw_world:vert:vertex rw_world:frag:fragment
               rw_im2d:vert:vertex  rw_im2d:frag:fragment
               rw_im3d:vert:vertex  rw_skin:vert:vertex
               rw_postfx:vert:vertex rw_postfx:frag:fragment
               rw_motion:frag:fragment rw_sgsr2_convert:frag:fragment)
	string(REPLACE ":" ";" PARTS "${SHADER}")
	list(GET PARTS 0 SHADER_NAME)
	list(GET PARTS 1 SHADER_EXT)
	list(GET PARTS 2 SHADER_STAGE)
	set(SHADER_OUTPUTS "")
	compile_shader("${LIBRW_SHADER_DIR}/${SHADER_NAME}.${SHADER_EXT}"
	               ${SHADER_STAGE} "${SHADER_NAME}_${SHADER_EXT}"
	               "${LIBRW_SPIRV_DIR}" "${LIBRW_SHADER_DIR}")
	list(APPEND LIBRW_SPIRV_HEADERS ${SHADER_OUTPUTS})
endforeach()

set(SHADER_OUTPUTS "")
compile_shader("${LIBRW_SHADER_DIR}/rw_im2d.frag" fragment
               "rw_im2d_nocoverage_frag" "${LIBRW_SPIRV_DIR}"
               "${LIBRW_SHADER_DIR}" -DRW_IM2D_COVERAGE=0)
list(APPEND LIBRW_SPIRV_HEADERS ${SHADER_OUTPUTS})
add_custom_target(librw_shaders DEPENDS ${LIBRW_SPIRV_HEADERS})

# ---------------------------------------------------------------------------
# librw with the Vulkan backend
#
# Every backend directory is compiled, not just src/vulkan. The other backends
# are disabled by their own #ifdefs and collapse to almost nothing, but librw's
# core calls into them unconditionally -- rw::gl3::initSkin, d3d8::initMatFX,
# ps2::readNativeTexture and so on -- so their stub definitions have to exist
# or the link fails.
# ---------------------------------------------------------------------------
file(GLOB LIBRW_CORE_SOURCES "${LIBRW_DIR}/src/*.cpp")
file(GLOB LIBRW_VULKAN_SOURCES "${LIBRW_DIR}/src/vulkan/*.cpp")
file(GLOB LIBRW_D3D_SOURCES "${LIBRW_DIR}/src/d3d/*.cpp")
file(GLOB LIBRW_D3D12_SOURCES "${LIBRW_DIR}/src/d3d12/*.cpp")
file(GLOB LIBRW_GL_SOURCES "${LIBRW_DIR}/src/gl/*.cpp")
file(GLOB LIBRW_PS2_SOURCES "${LIBRW_DIR}/src/ps2/*.cpp")
file(GLOB LIBRW_LODEPNG_SOURCES "${LIBRW_DIR}/src/lodepng/*.cpp")

add_library(librw STATIC
	${LIBRW_CORE_SOURCES} ${LIBRW_VULKAN_SOURCES}
	${LIBRW_D3D_SOURCES} ${LIBRW_D3D12_SOURCES}
	${LIBRW_GL_SOURCES} ${LIBRW_PS2_SOURCES} ${LIBRW_LODEPNG_SOURCES})
add_dependencies(librw librw_shaders)
target_include_directories(librw PUBLIC "${LIBRW_DIR}" "${LIBRW_DIR}/src")
target_include_directories(librw PRIVATE "${LIBRW_SPIRV_DIR}")
target_compile_definitions(librw PUBLIC RW_VULKAN MIAMIVR_DEV_TOOLS=$<BOOL:${MIAMIVR_DEV_TOOLS}>
	MIAMIVR_STEAM_FRAME=$<BOOL:${MIAMIVR_STEAM_FRAME}>)
target_link_libraries(librw vulkan log)
target_compile_options(librw PRIVATE
	-Wno-unused-parameter
	-Wno-unused-variable
	-Wno-unused-function
	# librw casts between plugin storage and concrete types throughout; the
	# warning fires thousands of times and hides real diagnostics.
	-Wno-invalid-offsetof)

# ---------------------------------------------------------------------------
# native_app_glue
# ---------------------------------------------------------------------------
add_library(native_app_glue STATIC
	"${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c")
target_include_directories(native_app_glue PUBLIC
	"${ANDROID_NDK}/sources/android/native_app_glue")
set_target_properties(native_app_glue PROPERTIES COMPILE_FLAGS "-Wno-unused-parameter")

# ---------------------------------------------------------------------------
# Main shared library loaded by NativeActivity (android.app.lib_name=miamivr)
# ---------------------------------------------------------------------------
set(MIAMIVR_SOURCES
	android_main.cpp
	xr_vulkan_session.cpp
	platform_android.cpp)

if(NOT MIAMIVR_BRINGUP)
	message(STATUS "miamivr: full game build selected")

	set(GAME_DIRS
		animation audio buildings collision control core entities extras
		fakerw math modelinfo objects peds render rw save skel skel/android
		text vehicles weapons)

	set(GAME_SOURCES "")
	foreach(d ${GAME_DIRS})
		file(GLOB _dir_sources "${REVC_ROOT}/src/${d}/*.cpp")
		list(APPEND GAME_SOURCES ${_dir_sources})
	endforeach()
	# Keep this explicit because file(GLOB) does not cause an already configured
	# Ninja graph to notice a newly added source file.
	set(QUEST_PROFILER_SOURCE
		"${REVC_ROOT}/src/skel/android/QuestProfiler.cpp")
	list(REMOVE_ITEM GAME_SOURCES "${QUEST_PROFILER_SOURCE}")
	list(APPEND GAME_SOURCES "${QUEST_PROFILER_SOURCE}")
	# Like QuestProfiler, keep the newly introduced source explicit so an
	# already-configured Ninja graph notices it immediately.
	set(PHYSICS_DIRECTOR_SOURCE
		"${REVC_ROOT}/src/vehicles/PhysicsDirector.cpp")
	list(REMOVE_ITEM GAME_SOURCES "${PHYSICS_DIRECTOR_SOURCE}")
	list(APPEND GAME_SOURCES "${PHYSICS_DIRECTOR_SOURCE}")
	set(VEHICLE_VISUAL_DIRECTOR_SOURCE
		"${REVC_ROOT}/src/vehicles/VehicleVisualDirector.cpp")
	list(REMOVE_ITEM GAME_SOURCES "${VEHICLE_VISUAL_DIRECTOR_SOURCE}")
	list(APPEND GAME_SOURCES "${VEHICLE_VISUAL_DIRECTOR_SOURCE}")
	set(VEHICLE_DEFORMATION_SOURCE
		"${REVC_ROOT}/src/vehicles/VehicleDeformation.cpp")
	list(REMOVE_ITEM GAME_SOURCES "${VEHICLE_DEFORMATION_SOURCE}")
	list(APPEND GAME_SOURCES "${VEHICLE_DEFORMATION_SOURCE}")
	# Exclude the tool translation unit entirely in player builds. The header
	# still provides constant-false/no-op entry points for defensive callers.
	set(VR_CULL_VIZ_SOURCE "${REVC_ROOT}/src/render/VrCullViz.cpp")
	list(REMOVE_ITEM GAME_SOURCES "${VR_CULL_VIZ_SOURCE}")
	if(MIAMIVR_DEV_TOOLS)
		list(APPEND GAME_SOURCES "${VR_CULL_VIZ_SOURCE}")
	endif()

	# Quest audio uses a native AAudio SFX mixer and the Android platform
	# decoder for long radio/cutscene streams.  The desktop OpenAL/Miles
	# backends carry host-only libraries and are deliberately not linked.
	list(REMOVE_ITEM GAME_SOURCES
		"${REVC_ROOT}/src/audio/sampman_oal.cpp"
		"${REVC_ROOT}/src/audio/sampman_miles.cpp")

	# GitSHA1.cpp is generated, not committed; the desktop build makes it with
	# printHash.bat as a prebuild step.
	execute_process(
		COMMAND git rev-parse HEAD
		WORKING_DIRECTORY "${REVC_ROOT}"
		OUTPUT_VARIABLE GIT_SHA1
		OUTPUT_STRIP_TRAILING_WHITESPACE
		ERROR_QUIET)
	if(NOT GIT_SHA1)
		set(GIT_SHA1 "unknown")
	endif()
	configure_file("${REVC_ROOT}/src/extras/GitSHA1.cpp.in"
	               "${CMAKE_CURRENT_BINARY_DIR}/GitSHA1.cpp" @ONLY)
	list(APPEND GAME_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/GitSHA1.cpp")
	# Native Quest hands share the authored UltimateXR mesh loader with the
	# desktop VR build. QuestWeaponVR is the Android input/interaction adapter;
	# it deliberately does not own another OpenXR session or stereo renderer.
	list(APPEND GAME_SOURCES
		"${REVC_ROOT}/src/vr/VRHandModel.cpp"
		"${REVC_ROOT}/src/vr/QuestDrivingVR.cpp"
		"${REVC_ROOT}/src/vr/QuestWeaponVR.cpp")

	list(APPEND MIAMIVR_SOURCES ${GAME_SOURCES})

	foreach(d ${GAME_DIRS} "")
		list(APPEND GAME_INCLUDES "${REVC_ROOT}/src/${d}")
	endforeach()
	list(APPEND GAME_INCLUDES "${REVC_ROOT}/src" "${REVC_ROOT}/src/audio/oal"
	                         "${REVC_ROOT}/src/vr")
	# The skeleton reaches into the app layer for the storage roots.
	list(APPEND GAME_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

# Steam Frame: the shared sfvr library (system layer, haptics, [vr-perf]),
# vendored unedited in third_party/sfvr.
if(MIAMIVR_STEAM_FRAME)
	set(SFVR_DIR "${REVC_ROOT}/third_party/sfvr")
	file(GLOB SFVR_SOURCES "${SFVR_DIR}/src/*.c")
	add_library(sfvr STATIC ${SFVR_SOURCES})
	target_include_directories(sfvr PUBLIC "${SFVR_DIR}/include")
	set_target_properties(sfvr PROPERTIES C_STANDARD 99)
	target_compile_options(sfvr PRIVATE -Wall -Wextra)

	# vr-diag.ini parsing and BMP output for the unattended-test diagnostics.
	list(APPEND MIAMIVR_SOURCES frame_diag.cpp)
endif()

add_library(miamivr SHARED ${MIAMIVR_SOURCES})
add_dependencies(miamivr miamivr_shaders)

target_include_directories(miamivr PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"${SPIRV_OUT_DIR}"
	"${LIBRW_DIR}"
	${GAME_INCLUDES})

target_compile_definitions(miamivr PRIVATE
	MIAMIVR_DEV_TOOLS=$<BOOL:${MIAMIVR_DEV_TOOLS}>
	XR_USE_PLATFORM_ANDROID
	XR_USE_GRAPHICS_API_VULKAN
	VK_USE_PLATFORM_ANDROID_KHR)

if(NOT MIAMIVR_BRINGUP)
	target_compile_definitions(miamivr PRIVATE
		LIBRW RW_VULKAN AUDIO_ANDROID GTA_VR_WEAPONS)
	# 2003-era C++ read by a 2024 clang. These are style diagnostics, not
	# defects, and at this volume they bury the errors that matter.
	target_compile_options(miamivr PRIVATE
		-Wno-invalid-offsetof
		-Wno-writable-strings
		-Wno-unused-value
		-Wno-parentheses
		-Wno-switch
		-Wno-format-security)
endif()

if(MIAMIVR_BRINGUP)
	target_compile_definitions(miamivr PRIVATE MIAMIVR_BRINGUP=1)
endif()

if(MIAMIVR_STEAM_FRAME)
	target_link_libraries(miamivr sfvr)
endif()

target_link_libraries(miamivr
	native_app_glue
	librw
	OpenXR::openxr_loader
	vulkan
	android
	aaudio
	log)

# NativeActivity resolves ANativeActivity_onCreate from the glue; without this
# the linker drops it as unreferenced and the app dies at startup.
target_link_options(miamivr PRIVATE "-u" "ANativeActivity_onCreate")
