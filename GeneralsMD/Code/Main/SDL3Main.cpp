/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
** SDL3Main.cpp
**
** Entry point for Linux builds using SDL3 windowing and DXVK graphics.
**
** TheSuperHackers @feature CnC_Generals_Linux 07/02/2026
** Entry point replaces WinMain() for Linux builds.
** Instantiates SDL3GameEngine and calls GameMain().
*/

#ifndef _WIN32

// SYSTEM INCLUDES
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#if defined(__ANDROID__)
#include <android/log.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#endif
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
// GeneralsX @feature android-port 06/07/2026
// Touch input, app-lifecycle render-gating, high-DPI drawables, and the
// SDL_main bootstrap are shared across mobile targets: iOS (UIKit via SDL's
// UIApplicationMain bridge) and Android (SDLActivity JNI -> SDL_main). Gate
// the platform-agnostic mobile code on SAGE_MOBILE instead of repeating
// __ANDROID__ in every iOS guard. iOS-only mechanisms (MoltenVK, funopen,
// bundle-relative paths) stay on the narrower TARGET_OS_IPHONE guard below.
#if (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE) || defined(__ANDROID__)
#define SAGE_MOBILE 1
#endif
#ifdef SAGE_MOBILE
// On iOS/Android, SDL renames main() to SDL_main and provides the platform
// bootstrap (iOS: UIApplicationMain; Android: SDLActivity JNI nativeRunMain).
// The app lifecycle (suspend/resume, window) is owned by SDL.
#include <SDL3/SDL_main.h>
#include <cerrno>
#include <sys/stat.h>
#include <fcntl.h>
#include <filesystem>
#include <string>
#endif
#include <cstdlib>

#if defined(__ANDROID__)
// GeneralsX @bugfix android-port 07/07/2026 Force OpenAL Soft to use the Oboe
// backend on Android. By default, OpenAL's init gives the "null" backend
// (a no-output sink) priority, producing no audio. Setting ALSOFT_DRIVERS=oboe
// before alcOpenDevice forces it to use Oboe (AAudio/OpenSL ES).
// This runs in main() before any OpenAL call — safe and reliable.
static void gx_force_oboe_audio_backend()
{
	setenv("ALSOFT_DRIVERS", "oboe", 1);
}
#endif
#include <cctype>
#include <cstring>
#include <cstdio>
#include <unistd.h>   // _exit()
#include <glob.h>     // glob() for Vulkan ICD discovery

// USER INCLUDES (match WinMain.cpp pattern)
#include "Lib/BaseType.h"
#include "Common/CommandLine.h"
#include "Common/CriticalSection.h"
#include "Common/GlobalData.h"
#include "Common/GameEngine.h"
#include "Common/GameMemory.h"
#include "Common/Debug.h"
#include "Common/version.h"  // GeneralsX @bugfix BenderAI 14/02/2026 Version class + TheVersion extern
#include "SDL3GameEngine.h"

// DXVK WSI
#define DXVK_WSI_SDL3 1
#include <wsi/native_wsi.h>

// CRITICAL SECTIONS (Linux needs these too)
static CriticalSection critSec1;
static CriticalSection critSec2;
static CriticalSection critSec3;
static CriticalSection critSec4;
static CriticalSection critSec5;

// GLOBAL COMMAND LINE ARGUMENTS
// TheSuperHackers @build felipebraz 13/02/2026
// Store argc/argv from main() for use by CommandLine.cpp parseCommandLine() on Linux
// Windows provides these automatically; Linux needs explicit globals
int __argc = 0;          ///< global argument count
char** __argv = nullptr; ///< global argument vector

// GLOBAL WINDOW HANDLE
// TheSuperHackers @build felipebraz 13/02/2026
// ApplicationHWnd is declared extern in GeneralsMD/Code/Main/WinMain.h
// On Linux, we cast SDL_Window* to HWND type for compatibility
HWND ApplicationHWnd = nullptr;  ///< our application window handle

// GLOBAL SDL3 WINDOW
// GeneralsX @feature felipebraz 16/02/2026
// SDL3 window created in main() before GameMain(), stored globally for engine access
SDL_Window* TheSDL3Window = nullptr;

// GAME TEXT FILE PATHS
// TheSuperHackers @build felipebraz 13/02/2026
// GameText.cpp uses these paths to load CSF and STR files (game localization)
// Format %s is replaced with language code in GameTextManager::init()
// GeneralsX @bugfix BenderAI 13/02/2026 - Fix case-sensitivity on Linux (generals.csf vs Generals.csf)
const Char *g_csfFile = "data/%s/generals.csf";  ///< CSF file path (lowercase for Linux compatibility)
const Char *g_strFile = "data/Generals.str";     ///< STR file path

// Extern declarations (from GameMain.cpp)
extern Int GameMain();

/**
 * FilterSoftwareVulkanICDs
 *
 * Sets VK_DRIVER_FILES to only hardware Vulkan ICDs, excluding LLVMpipe/lavapipe.
 *
 * Workaround for Mesa/LLVM 20.x bug: libvulkan_lvp.so (LLVMpipe Vulkan ICD) crashes
 * during dlopen() static initialization with a null-ptr deref in llvm::Regex::Regex().
 * The Vulkan loader loads ALL ICDs found in the ICD directories when
 * vkEnumerateInstanceExtensionProperties() is called, which triggers the crash.
 * Filtering hardware-only ICDs via VK_DRIVER_FILES prevents loading libvulkan_lvp.so.
 *
 * Only applied when neither VK_DRIVER_FILES nor VK_ICD_FILENAMES is already set,
 * so the user can always override by setting those variables externally.
 *
 * GeneralsX @bugfix BenderAI 06/03/2026
 */
static void FilterSoftwareVulkanICDs()
{
#if defined(__ANDROID__)
	// GeneralsX @feature android-port 06/07/2026 Android has no /usr/share/vulkan
	// ICD directory and no software Vulkan ICDs to filter; the system driver is
	// always hardware. Also, bionic's glob() requires API 28+ (we target 24).
	return;
#else
	if (getenv("VK_DRIVER_FILES") || getenv("VK_ICD_FILENAMES")) {
		return;
	}

	auto icd_is_software = [](const char *name) -> bool {
		char low[256] = "";
		for (int i = 0; name[i] && i < 255; ++i) {
			low[i] = (char)tolower((unsigned char)name[i]);
		}
		return strstr(low, "lvp") || strstr(low, "lavapipe") || strstr(low, "softpipe") || strstr(low, "llvmpipe");
	};

	static char hw_icds[4096] = "";
	const char *patterns[] = {
		"/usr/share/vulkan/icd.d/*.json",
		"/etc/vulkan/icd.d/*.json",
		nullptr
	};

	glob_t gl = {};
	int gflags = 0;
	for (int i = 0; patterns[i]; ++i) {
		if (glob(patterns[i], gflags, nullptr, &gl) == 0) {
			gflags = GLOB_APPEND;
		}
	}

	bool found_hw = false;
	for (size_t i = 0; i < gl.gl_pathc; ++i) {
		const char *path = gl.gl_pathv[i];
		const char *base = strrchr(path, '/');
		base = base ? base + 1 : path;
		if (icd_is_software(base)) {
			fprintf(stderr, "INFO: Vulkan ICD filter: skipping software ICD '%s'\n", base);
			continue;
		}
		if (found_hw) {
			strncat(hw_icds, ":", sizeof(hw_icds) - strlen(hw_icds) - 1);
		}
		strncat(hw_icds, path, sizeof(hw_icds) - strlen(hw_icds) - 1);
		found_hw = true;
	}
	globfree(&gl);

	if (found_hw) {
		setenv("VK_DRIVER_FILES", hw_icds, 1);
		fprintf(stderr, "INFO: Vulkan ICD filter: VK_DRIVER_FILES=%s\n", hw_icds);
	} else {
		fprintf(stderr, "WARNING: Vulkan ICD filter: no hardware ICDs found, LLVMpipe exclusion skipped\n");
		fprintf(stderr, "WARNING: If startup crashes in libvulkan_lvp.so, set VK_DRIVER_FILES manually\n");
	}
#endif // __ANDROID__
}

/**
 * FilterPipeWireOpenAL
 *
 * Sets ALSOFT_DRIVERS to skip PipeWire, falling back to pulse/alsa.
 *
 * Workaround for openal-soft PipeWire backend crash: alcOpenDevice() segfaults
 * inside the PipeWire backend while opening the default playback device.
 * The crash occurs in PipeWire's stream/context internals and is unrecoverable
 * from userspace. Excluding PipeWire via ALSOFT_DRIVERS causes openal-soft to
 * fall back to the PulseAudio backend, which works correctly on PipeWire systems
 * via the PulseAudio compatibility layer.
 *
 * NOTE: openal-soft reads ALSOFT_DRIVERS from a static global constructor when
 * libopenal.so is loaded by the dynamic linker, which is before main() runs.
 * This function is therefore only effective for builds that use lazy
 * initialization. The authoritative fix is in the launch scripts (run-linux-zh.sh
 * etc.), which set ALSOFT_DRIVERS before the binary starts.
 *
 * Only applied when ALSOFT_DRIVERS is not already set by the user.
 *
 * GeneralsX @bugfix 09/03/2026
 */
static void FilterPipeWireOpenAL()
{
	// GeneralsX @bugfix Copilot 24/03/2026 PipeWire/OpenAL workaround is Linux-only; keep macOS CoreAudio backend selection untouched.
	#if defined(__linux__)
	// Crash: alcOpenDevice() hits 'movaps %xmm1,0x26260(%rbx)' — SSE movaps requires
	// 16-byte alignment; a misaligned ALCdevice struct faults regardless of backend.
	// Disabling CPU extensions forces openal-soft to use scalar code that has no
	// alignment requirements. Also exclude pipewire which has its own crash at
	// device-open time on PipeWire 1.4.x.
	// NOTE: these env vars are authoritative only when set before the binary loads
	// (openal-soft reads them from a static constructor). The launch scripts set them
	// first; this is a best-effort fallback for lazy-init builds.
	if (!getenv("ALSOFT_DISABLE_CPU_EXTS")) {
		setenv("ALSOFT_DISABLE_CPU_EXTS", "all", 1);
		fprintf(stderr, "INFO: OpenAL: ALSOFT_DISABLE_CPU_EXTS=all (movaps alignment crash workaround)\n");
	}
	if (!getenv("ALSOFT_DRIVERS")) {
		setenv("ALSOFT_DRIVERS", "pulse,alsa,oss,jack,null,wave", 1);
		fprintf(stderr, "INFO: OpenAL: ALSOFT_DRIVERS=pulse,alsa,oss,jack,null,wave (pipewire excluded)\n");
	}
	#else
	fprintf(stderr, "INFO: OpenAL: keeping default driver selection on non-Linux platform\n");
	#endif
}

/**
 * CreateGameEngine
 *
 * Factory function for SDL3GameEngine on Linux.
 * Called by GameMain() to instantiate platform-specific engine.
 *
 * @return SDL3GameEngine instance
 */
GameEngine *CreateGameEngine(void)
{
	fprintf(stderr, "INFO: CreateGameEngine() - Creating SDL3GameEngine for Linux\n");
	SDL3GameEngine *engine = NEW SDL3GameEngine();
	return engine;
}

/**
 * main
 *
 * Linux entry point (replaces WinMain on Windows).
 * Initializes subsystems and calls GameMain().
 *
 * @param argc Command line argument count
 * @param argv Command line arguments
 * @return Exit code (0 = success)
 */
int main(int argc, char* argv[])
{
	int exitcode = 1;

#if defined(__ANDROID__)
	// GeneralsX @bugfix android-port 07/07/2026 Force Oboe backend before
	// anything else runs. Must be the very first call in main() so OpenAL
	// picks up ALSOFT_DRIVERS when it initializes its backends.
	gx_force_oboe_audio_backend();
	__android_log_print(ANDROID_LOG_INFO, "GeneralsX", "=== SDL_main entered ===");
#endif

	// TheSuperHackers @build felipebraz 13/02/2026
	// Store command line arguments in globals for CommandLine.cpp parser
	__argc = argc;
	__argv = argv;

#if defined(__ANDROID__)
	// GeneralsX @feature android-port 06/07/2026 Android working directory + diagnostics.
	//
	// Android does not expose APK assets via the filesystem: a native fopen() on
	// "GameData/*.big" cannot reach them. The packaging step stages GameData into
	// the app's internal storage (<files>/GameData), which IS a real filesystem
	// path the engine can chdir() into and read with stdio — mirroring the iOS
	// "GameData beside the binary" model. SDL3 surfaces that path via
	// SDL_GetAndroidInternalStoragePath(). User data (saves, replays, logs) goes
	// under the same root; DXVK's shader cache lives under the cache dir.
	//
	// Diagnostics: native stderr/stdout already flow to `adb logcat`, but a
	// memory-killed process leaves no tombstone, so we ALSO keep a capped,
	// filtered file log (the iOS port's hard-won lesson). bionic has no funopen(),
	// so a simple ring-buffered write() sink replaces it.
	setenv("DXVK_LOG_LEVEL", "none", 0);
	// The engine's StdBIGFileSystem::init() reads "InstallPath" from the registry
	// to locate the Data/*.big archives. On Android there's no registry — the
	// env-var fallback (CNC_ZH_INSTALLPATH) provides it. Point it at "." so the
	// engine finds Data/ relative to the CWD (set below).
	setenv("CNC_ZH_INSTALLPATH", ".", 0);
	setenv("CNC_GENERALS_INSTALLPATH", ".", 0);
	{
		// GeneralsX @feature android-port 06/07/2026
		// Try EXTERNAL storage first (adb-pushable, no root): the app's external
		// files dir at /sdcard/Android/data/<pkg>/files/GameData. Fall back to
		// internal storage (set by the packaging script).
		const char *extFiles = SDL_GetAndroidExternalStoragePath();
		const char *files = SDL_GetAndroidInternalStoragePath();
		__android_log_print(ANDROID_LOG_INFO, "GeneralsX",
			"storage: external=%s internal=%s",
			extFiles ? extFiles : "(null)", files ? files : "(null)");

		bool chdirOk = false;
		if (extFiles != nullptr) {
			char gameData[1024];
			snprintf(gameData, sizeof(gameData), "%s/GameData", extFiles);
			if (access(gameData, R_OK) == 0 && chdir(gameData) == 0) {
				__android_log_print(ANDROID_LOG_INFO, "GeneralsX", "CWD -> %s (external)", gameData);
				chdirOk = true;
			}
			SDL_free((void*)extFiles);
		}
		if (!chdirOk && files != nullptr) {
			char gameData[1024];
			snprintf(gameData, sizeof(gameData), "%s/GameData", files);
			if (access(gameData, R_OK) == 0 && chdir(gameData) == 0) {
				__android_log_print(ANDROID_LOG_INFO, "GeneralsX", "CWD -> %s (internal)", gameData);
				chdirOk = true;
			}
		}
		if (!chdirOk) {
			__android_log_print(ANDROID_LOG_WARN, "GeneralsX", "no GameData dir found, CWD unchanged");
		}

		// GeneralsX @feature android-port 07/07/2026 Extract bundled fonts from
		// APK assets to the GameData filesystem. Android APK assets are invisible
		// to fopen()/access(), but the engine's FreeType font locator
		// (Locate_Font_FontConfig) probes <CWD>/fonts/<name>.ttf via access().
		// The packaging step bundles Liberation fonts (renamed to Windows names)
		// into assets/fonts/. Extract them once on first launch so the engine can
		// read them via standard stdio.
		{
			char fontsDir[1024];
			const char *extractBase = nullptr;
			const char *extFiles2 = SDL_GetAndroidExternalStoragePath();
			if (extFiles2 != nullptr) {
				snprintf(fontsDir, sizeof(fontsDir), "%s/GameData/fonts", extFiles2);
				extractBase = fontsDir;
				SDL_free((void*)extFiles2);
			}
			if (extractBase == nullptr) {
				const char *intFiles2 = SDL_GetAndroidInternalStoragePath();
				if (intFiles2 != nullptr) {
					snprintf(fontsDir, sizeof(fontsDir), "%s/GameData/fonts", intFiles2);
					extractBase = fontsDir;
					SDL_free((void*)intFiles2);
				}
			}

			if (extractBase != nullptr) {
				mkdir(extractBase, 0755);

				// Check if fonts already extracted (skip if arial.ttf exists)
				char checkPath[1100];
				snprintf(checkPath, sizeof(checkPath), "%s/arial.ttf", extractBase);
				if (access(checkPath, R_OK) != 0) {
					// Obtain the AAssetManager via JNI (SDL3 doesn't expose it directly)
					AAssetManager *mgr = nullptr;
					JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
					jobject activity = (jobject)SDL_GetAndroidActivity();
					if (env != nullptr && activity != nullptr) {
						jclass cls = env->GetObjectClass(activity);
						jmethodID mid = env->GetMethodID(cls, "getAssets", "()Landroid/content/res/AssetManager;");
						if (mid != nullptr) {
							jobject javaAssetMgr = env->CallObjectMethod(activity, mid);
							if (javaAssetMgr != nullptr) {
								mgr = AAssetManager_fromJava(env, javaAssetMgr);
								env->DeleteLocalRef(javaAssetMgr);
							}
						}
						env->DeleteLocalRef(cls);
					}

					if (mgr != nullptr) {
						static const char * const fontFiles[] = {
							"arial.ttf", "arialbold.ttf",
							"couriernew.ttf", "timesnewroman.ttf"
						};
						__android_log_print(ANDROID_LOG_INFO, "GeneralsX",
							"fonts: extracting from APK assets to %s", extractBase);
						for (int i = 0; i < (int)(sizeof(fontFiles)/sizeof(fontFiles[0])); ++i) {
							char assetPath[256];
							snprintf(assetPath, sizeof(assetPath), "fonts/%s", fontFiles[i]);
							AAsset *asset = AAssetManager_open(mgr, assetPath, AASSET_MODE_STREAMING);
							if (asset == nullptr) {
								__android_log_print(ANDROID_LOG_WARN, "GeneralsX",
									"fonts: asset '%s' not found in APK", assetPath);
								continue;
							}
							char outPath[1100];
							snprintf(outPath, sizeof(outPath), "%s/%s", extractBase, fontFiles[i]);
							int outFd = open(outPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
							if (outFd < 0) {
								AAsset_close(asset);
								continue;
							}
							char buf[8192];
							int bytesRead;
							while ((bytesRead = AAsset_read(asset, buf, sizeof(buf))) > 0) {
								write(outFd, buf, bytesRead);
							}
							close(outFd);
							AAsset_close(asset);
							__android_log_print(ANDROID_LOG_INFO, "GeneralsX",
								"fonts: extracted %s", fontFiles[i]);
						}
					} else {
						__android_log_print(ANDROID_LOG_WARN, "GeneralsX",
							"fonts: cannot get AAssetManager via JNI");
					}
				} else {
					__android_log_print(ANDROID_LOG_INFO, "GeneralsX",
						"fonts: already present at %s", extractBase);
				}
			}
		}

			if (files != nullptr) {
			// DXVK shader cache in the app cache dir (purgeable under storage pressure).
			const char *cache = SDL_GetAndroidCachePath();
			if (cache != nullptr) {
				setenv("DXVK_STATE_CACHE_PATH", cache, 0);
				SDL_free((void*)cache);
			}
			// Capped, filtered stderr file sink (post-mortem evidence after a kill).
			char logPath[1100], prevPath[1100];
			snprintf(logPath, sizeof(logPath), "%s/generals-stderr.log", files);
			snprintf(prevPath, sizeof(prevPath), "%s/generals-stderr-prev.log", files);
			rename(logPath, prevPath);
			static int s_logFd = open(logPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (s_logFd >= 0) {
				// Redirect the C stderr FILE onto our fd via dup2: portable across
				// bionic/libc, unlike Darwin's funopen(). Line-buffered so a crash
				// still flushes recent lines. (Per-frame spam filtering is left to
				// dxvk.conf + DXVK_LOG_LEVEL=none; a full filter callback would need
				// a custom FILE backend that bionic does not provide.)
				fflush(stderr);
				dup2(s_logFd, STDERR_FILENO);
				setvbuf(stderr, nullptr, _IOLBF, 0);
			}
			SDL_free((void*)files);
		}
	}
#elif defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	// Diagnostic capture: an icon-launched app's stderr goes nowhere we can read,
	// so mirror it to a file in Library/Caches (purgeable, not user-visible). This
	// lets us pull a full engine log after an on-device session — essential for
	// debugging mode-specific issues (e.g. Generals Challenge radar/scripts) that
	// only the user can reproduce. Pull with: devicectl ... copy from
	// Library/Caches/generals-stderr.log. Remove once the relevant bugs are fixed.
	{
		// Quiet DXVK at the source: the d3d8 layer's per-call warns (e.g. an
		// unimplemented render state set every frame) wrote hundreds of MB per
		// long session. The shipped dxvk.conf also sets logLevel=none; the env
		// covers modules that read it before the config.
		setenv("DXVK_LOG_LEVEL", "none", 0);
		const char *diagHome = getenv("HOME");
		if (diagHome != nullptr) {
			char diagPath[1024];
			char prevPath[1024];
			// Documents, not Library/Caches: Caches is purgeable (a device restart or
			// storage pressure can empty it), and Documents is user-reachable via the
			// Files app since the bundle enables UIFileSharingEnabled.
			snprintf(diagPath, sizeof(diagPath), "%s/Documents/generals-stderr.log", diagHome);
			// Keep the previous session's log: a session that ends in a memory kill
			// leaves no OS crash report, so the prior log is often the only evidence.
			snprintf(prevPath, sizeof(prevPath), "%s/Documents/generals-stderr-prev.log", diagHome);
			rename(diagPath, prevPath);
			// Filtered + capped sink instead of a raw freopen: per-frame debug spam
			// (upstream [GX-ISSUE144] font traces, [INI] loader traces, residual DXVK
			// warns) is dropped, and the file stops growing at 8 MB so a marathon
			// session cannot eat device storage. funopen() is fine here: this is
			// Darwin-only code.
			static int s_logFd = -1;
			s_logFd = open(diagPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (s_logFd >= 0) {
				static size_t s_logWritten = 0;
				FILE *sink = funopen(nullptr,
					nullptr,
					[](void *, const char *buf, int len) -> int {
						static const size_t kLogCap = 8u * 1024u * 1024u;
						if (s_logFd < 0) return len;
						if (len > 13 &&
						    (memcmp(buf, "[GX-ISSUE144]", 13) == 0 ||
						     memcmp(buf, "[INI] ", 6) == 0 ||
						     memcmp(buf, "warn:  D3D8De", 13) == 0)) {
							return len;  // drop known per-frame spam, report consumed
						}
						if (s_logWritten >= kLogCap) {
							// past the cap, still record errors — the tail of a dying
							// session is this log's whole reason to exist
							static bool s_capMarked = false;
							if (!s_capMarked) {
								s_capMarked = true;
								const char *mark = "[log capped: non-error lines dropped from here]\n";
								write(s_logFd, mark, strlen(mark));
							}
							if (len > 4 && (memcmp(buf, "err:", 4) == 0 ||
							                memcmp(buf, "ERROR", 5) == 0 ||
							                memcmp(buf, "FATAL", 5) == 0)) {
								write(s_logFd, buf, (size_t)len);
							}
							return len;
						}
						ssize_t w = write(s_logFd, buf, (size_t)len);
						if (w > 0) s_logWritten += (size_t)w;
						return len;
					},
					nullptr, nullptr);
				if (sink != nullptr) {
					*stderr = *sink;  // classic Darwin stderr swap; stderr is a FILE, not a macro here
					setvbuf(stderr, nullptr, _IOLBF, 0);  // line-buffered so a crash still flushes recent lines
				}
			}
		}
	}

	// The engine resolves all game data relative to the working directory.
	// Preferred layout: assets ship read-only INSIDE the signed app bundle
	// (<bundle>/GameData), the iOS-sanctioned home for app resources — the
	// install is then fully self-contained. Dev builds packaged without
	// assets fall back to the Documents folder (Files-app accessible).
	// User data (saves, Options.ini) always lives in Library/Application
	// Support via the engine's user-data path; never in the bundle.
	{
		const char *home = getenv("HOME");

		// <bundle>/GameData, derived from the executable path (argv[0])
		char bundleData[1024] = {0};
		if (argc > 0 && argv[0] != nullptr) {
			const char *slash = strrchr(argv[0], '/');
			if (slash != nullptr) {
				const size_t dirLen = (size_t)(slash - argv[0]);
				if (dirLen < sizeof(bundleData) - 16) {
					memcpy(bundleData, argv[0], dirLen);
					snprintf(bundleData + dirLen, sizeof(bundleData) - dirLen, "/GameData");
				}
			}
		}

		bool usingBundleData = false;
		if (bundleData[0] != '\0' && access(bundleData, R_OK) == 0) {
			if (chdir(bundleData) == 0) {
				usingBundleData = true;
				fprintf(stderr, "INFO: iOS working directory (bundle): %s\n", bundleData);
			}
		}
		if (!usingBundleData && home != nullptr) {
			char docs[1024];
			snprintf(docs, sizeof(docs), "%s/Documents", home);
			if (chdir(docs) != 0) {
				fprintf(stderr, "WARNING: chdir(%s) failed: %s\n", docs, strerror(errno));
			} else {
				fprintf(stderr, "INFO: iOS working directory (Documents): %s\n", docs);
			}
		}

		if (home != nullptr) {
			// Keep DXVK's shader cache in Library/Caches: purgeable under
			// storage pressure, excluded from iCloud backup, invisible in the
			// Files app. Must be set before the d3d8 dylib loads.
			char cacheDir[1024];
			snprintf(cacheDir, sizeof(cacheDir), "%s/Library/Caches", home);
			mkdir(cacheDir, 0755);
			setenv("DXVK_STATE_CACHE_PATH", cacheDir, 0);

			if (usingBundleData) {
				// Seed default settings on first run (full detail instead of the
				// 2003 auto-detect, which drops unknown GPUs to Low).
				char userDataDir[1024], optionsPath[1024];
				snprintf(userDataDir, sizeof(userDataDir),
				         "%s/Library/Application Support/GeneralsX/GeneralsZH", home);
				snprintf(optionsPath, sizeof(optionsPath), "%s/Options.ini", userDataDir);
				if (access(optionsPath, F_OK) != 0 && access("DefaultOptions.ini", R_OK) == 0) {
					std::error_code fsError;
					std::filesystem::create_directories(userDataDir, fsError);
					std::filesystem::copy_file("DefaultOptions.ini", optionsPath, fsError);
					if (!fsError) {
						fprintf(stderr, "INFO: Seeded default Options.ini\n");
					}
				}

				// One-time tidy-up: remove asset copies from Documents now that
				// the bundle carries them. Guarded by a sentinel so it truly runs
				// once — Documents is exposed via the Files app, and anything the
				// user places there later (mods, custom maps) must never be touched.
				// "Maps" is deliberately NOT in the list: it is where user maps live.
				char docs[1024];
				snprintf(docs, sizeof(docs), "%s/Documents", home);
				char sentinel[1024];
				snprintf(sentinel, sizeof(sentinel), "%s/.bundle-assets-tidied", docs);
				if (access(sentinel, F_OK) != 0) {
					std::error_code fsError;
					for (const auto &entry : std::filesystem::directory_iterator(docs, fsError)) {
						const std::string name = entry.path().filename().string();
						const bool isShippedAsset =
							(name.size() > 4 && name.compare(name.size() - 4, 4, ".big") == 0) ||
							name == "Data" || name == "Window" || name == "ZH_Generals" ||
							name == "fonts" || name == "_CommonRedist" ||
							name == "dxvk.conf" || name == "GeneralsXZH.dxvk-cache" ||
							name == "GeneralsXZH_d3d9.log";
						if (isShippedAsset) {
							fprintf(stderr, "INFO: tidy-up removing shipped asset copy: %s\n", name.c_str());
							std::error_code removeError;
							std::filesystem::remove_all(entry.path(), removeError);
						}
					}
					if (!fsError) {  // a failed scan must retry next launch, not fail closed forever
						FILE *s = fopen(sentinel, "w");
						if (s) fclose(s);
					}
				}
			}
		}
	}
#endif

	fprintf(stderr, "=================================================\n");
	fprintf(stderr, " Command & Conquer Generals: Zero Hour (Linux)\n");
	fprintf(stderr, " SDL3 + DXVK Build\n");
	fprintf(stderr, "=================================================\n\n");

	try {
		// Initialize critical sections (required by game engine)
		TheAsciiStringCriticalSection = &critSec1;
		TheUnicodeStringCriticalSection = &critSec2;
		TheDmaCriticalSection = &critSec3;
		TheMemoryPoolCriticalSection = &critSec4;
		TheDebugLogCriticalSection = &critSec5;
#if defined(__ANDROID__)
		__android_log_print(ANDROID_LOG_INFO, "GeneralsX", "init: critical sections OK");
#endif

		// Initialize memory manager early (required by NEW operator)
		initMemoryManager();
#if defined(__ANDROID__)
		__android_log_print(ANDROID_LOG_INFO, "GeneralsX", "init: memory manager OK");
#endif

		// GeneralsX @bugfix BenderAI 14/02/2026 Initialize Version singleton
		// GameEngine::init() calls updateWindowTitle() which uses TheVersion
		// Must be created before GameMain() to avoid nullptr dereference
		TheVersion = NEW Version;
#if defined(__ANDROID__)
		__android_log_print(ANDROID_LOG_INFO, "GeneralsX", "init: Version OK");
#endif

		// Parse command line (CommandLine class handles argc/argv internally)
		// TheSuperHackers @build felipebraz 10/02/2026 Phase 1.5
		// Store argc/argv for CommandLine parser to access via _NSGetArgc/_NSGetArgv or /proc/self/cmdline
		// For now, let CommandLine::parseCommandLineForStartup() handle this
		CommandLine::parseCommandLineForStartup();
#if defined(__ANDROID__)
		__android_log_print(ANDROID_LOG_INFO, "GeneralsX", "init: CommandLine OK (TheGlobalData=%p)", (void*)TheGlobalData);
#endif

		// GeneralsX @bugfix Copilot 17/05/2026 Skip SDL3 window bootstrap for CLI/headless replay execution.
		const bool isHeadlessMode = (TheGlobalData != nullptr && TheGlobalData->m_headless);
		if (isHeadlessMode) {
			fprintf(stderr, "INFO: Headless mode detected, skipping SDL3 video/Vulkan window initialization\n");
		} else {

		// GeneralsX @bugfix felipebraz 16/02/2026
		// Initialize SDL3 and Vulkan BEFORE creating GameEngine (fighter19 pattern)
		// This prevents LLVM SIGSEGV crash during Vulkan driver enumeration
		// Must be done here, not in SDL3GameEngine::init() which is too late
		fprintf(stderr, "INFO: Initializing SDL3 video subsystem...\n");
#ifdef SAGE_MOBILE
		// All mouse events are synthesized by the gesture translator in
		// SDL3GameEngine.cpp; SDL's automatic touch->mouse synthesis would
		// double-deliver finger 1 and fight the two-finger pan logic.
		SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#endif
		if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
			fprintf(stderr, "FATAL: Failed to initialize SDL3: %s\n", SDL_GetError());
			return 1;
		}

		// Set DXVK WSI driver before loading Vulkan
		setenv("DXVK_WSI_DRIVER", "SDL3", 1);

		// GeneralsX @bugfix BenderAI 06/03/2026 - Exclude LLVMpipe Vulkan ICD before loading Vulkan.
		// libvulkan_lvp.so crashes during static initialization with LLVM 20.x when the Vulkan
		// loader enumerates all ICDs. Restrict to hardware ICDs first.
		FilterSoftwareVulkanICDs();
		FilterPipeWireOpenAL();

		// Load Vulkan library for DXVK DirectX8→Vulkan translation
		fprintf(stderr, "INFO: Loading Vulkan library...\n");
		if (!SDL_Vulkan_LoadLibrary(nullptr)) {
			fprintf(stderr, "WARNING: Failed to load Vulkan: %s\n", SDL_GetError());
			fprintf(stderr, "WARNING: Continuing without Vulkan (may use software rendering)\n");
		}

		// Create SDL3 window with Vulkan support
		fprintf(stderr, "INFO: Creating SDL3 Vulkan window...\n");
		// GeneralsX @bugfix android-port 08/07/2026 On Android, the window MUST
		// NOT start hidden. A hidden SDL window has no ANativeWindow attached
		// (Android only creates the Surface for visible windows), which causes
		// CreateAndroidSurfaceKHR to dereference null (fault addr 0x98) when
		// DXVK tries to create the Vulkan swapchain surface. On desktop the
		// hidden flag avoids a flash before D3D init; on mobile it's fatal.
#if defined(__ANDROID__)
		Uint32 windowFlags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
#else
		Uint32 windowFlags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN;  // Start hidden, show after D3D init
#endif
#ifdef SAGE_MOBILE
		// Request a native-resolution drawable (e.g. 2868x1320 instead of the
		// 956x440 point size). Without this the swapchain renders at point size and
		// the display upscales 3x, visibly blurring textures and terrain.
		windowFlags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
#endif
		TheSDL3Window = SDL_CreateWindow(
			"Command & Conquer Generals: Zero Hour",
			1024, 768,  // Default resolution
			windowFlags
		);

		if (!TheSDL3Window) {
			fprintf(stderr, "FATAL: Failed to create SDL3 window: %s\n", SDL_GetError());
			SDL_Quit();
			return 1;
		}

		// Store window handle globally (cast SDL_Window* to HWND for compatibility)
		ApplicationHWnd = (HWND)TheSDL3Window;
		fprintf(stderr, "INFO: SDL3 window created successfully\n");

#if defined(__ANDROID__)
		// GeneralsX @bugfix android-port 08/07/2026 Wait for the Android Surface
		// to be attached to the SDL window before proceeding. On Android, the
		// ANativeWindow is delivered asynchronously from the Java side via
		// surfaceChanged(). If the engine reaches CreateDevice (which calls
		// SDL_Vulkan_CreateSurface → CreateAndroidSurfaceKHR) before the
		// ANativeWindow is ready, the Vulkan driver dereferences null and crashes
		// (fault addr 0x98). Poll SDL events until the window has a valid surface
		// or timeout after 5 seconds.
		{
			SDL_PropertiesID props = SDL_GetWindowProperties(TheSDL3Window);
			void *nativeWin = nullptr;
			int waitMs = 0;
			while (waitMs < 5000) {
				nativeWin = SDL_GetPointerProperty(props,
					SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
				if (nativeWin) break;
				SDL_Event ev;
				while (SDL_PollEvent(&ev)) { /* drain */ }
				SDL_Delay(50);
				waitMs += 50;
			}
			if (nativeWin) {
				__android_log_print(ANDROID_LOG_INFO, "GeneralsX",
					"ANativeWindow ready after %dms", waitMs);
			} else {
				__android_log_print(ANDROID_LOG_WARN, "GeneralsX",
					"ANativeWindow NOT ready after 5000ms — CreateDevice may crash");
			}
		}
#endif

#ifdef SAGE_MOBILE
		// Match the game's internal resolution to the screen's aspect ratio.
		// Without this the engine runs its 4:3 default inside a wide mobile display
		// (e.g. 19.5:9 phone, 16:10 tablet): pillarboxed picture and a skewed
		// window->game coordinate mapping. Height stays at the engine's 600px
		// design baseline (UI layouts assume >= 600); width follows the real
		// aspect. Injected as -xres/-yres argv entries so the normal command-line
		// path applies them (user-passed flags still win because the parser lets
		// later arguments override earlier ones... ours go last, so only add them
		// if the user didn't pass explicit -xres/-yres).
		{
			bool userSetRes = false;
			for (int i = 1; i < __argc; ++i) {
				if (strcmp(__argv[i], "-xres") == 0 || strcmp(__argv[i], "-yres") == 0) {
					userSetRes = true;
					break;
				}
			}
			// Use the pixel size of the high-density drawable: the game renders
			// 1:1 into the native-resolution swapchain, and fonts/UI rescale via
			// the engine's resolution-aware font scaling (GlobalLanguage).
			int winW = 0, winH = 0;
			SDL_GetWindowSizeInPixels(TheSDL3Window, &winW, &winH);
			if (!userSetRes && winW > 0 && winH > 0 && winW > winH) {
				static char xresVal[16], yresVal[16];
				static char xresFlag[] = "-xres";
				static char yresFlag[] = "-yres";
				const int yres = winH;
				int xres = winW;
				xres &= ~1;  // keep it even
				snprintf(xresVal, sizeof(xresVal), "%d", xres);
				snprintf(yresVal, sizeof(yresVal), "%d", yres);

				static char* newArgv[64];
				int n = 0;
				for (int i = 0; i < __argc && n < 59; ++i) {
					newArgv[n++] = __argv[i];
				}
				newArgv[n++] = xresFlag;
				newArgv[n++] = xresVal;
				newArgv[n++] = yresFlag;
				newArgv[n++] = yresVal;
				newArgv[n] = nullptr;
				__argv = newArgv;
				__argc = n;
				fprintf(stderr, "INFO: Mobile internal resolution set to %sx%s (window %dx%d)\n",
				        xresVal, yresVal, winW, winH);
			}
		}
#endif
		}

		// Call cross-platform game entry point
#if defined(__ANDROID__)
		__android_log_print(ANDROID_LOG_INFO, "GeneralsX", "calling GameMain()...");
#endif
		exitcode = GameMain();

		fprintf(stderr, "INFO: GameMain() returned with code %d\n", exitcode);

	} catch (const std::exception& e) {
		fprintf(stderr, "FATAL: Unhandled exception in main(): %s\n", e.what());
#if defined(__ANDROID__)
		__android_log_print(ANDROID_LOG_FATAL, "GeneralsX", "Unhandled exception: %s", e.what());
#endif
		exitcode = 1;
	} catch (...) {
		fprintf(stderr, "FATAL: Unknown exception in main()\n");
#if defined(__ANDROID__)
		__android_log_print(ANDROID_LOG_FATAL, "GeneralsX", "Unknown exception in main()");
#endif
		exitcode = 1;
	}

	// Cleanup SDL3 resources
	if (TheSDL3Window) {
		SDL_DestroyWindow(TheSDL3Window);
		TheSDL3Window = nullptr;
		ApplicationHWnd = nullptr;
	}
	SDL_Quit();

	// GeneralsX @bugfix BenderAI 14/02/2026 Cleanup Version singleton
	if (TheVersion) {
		delete TheVersion;
		TheVersion = nullptr;
	}

	// GeneralsX @bugfix BenderAI 19/02/2026 Shutdown memory manager BEFORE nulling critical
	// sections. Without this, global pool destructors (ObjectPoolClass) crash during atexit()
	// because they call ::operator delete after the memory manager is already gone (SIGSEGV).
	// Matches WinMain.cpp cleanup order: TheVersion -> shutdownMemoryManager -> null critSecs.
	shutdownMemoryManager();

	// Cleanup critical sections (after memory manager, which may use them during shutdown)
	TheAsciiStringCriticalSection = nullptr;
	TheUnicodeStringCriticalSection = nullptr;
	TheDmaCriticalSection = nullptr;
	TheMemoryPoolCriticalSection = nullptr;
	TheDebugLogCriticalSection = nullptr;

	fprintf(stderr, "\nExiting with code %d\n", exitcode);

	// GeneralsX @bugfix BenderAI 25/02/2026 — use _exit() to skip C++ global destructors.
	// On macOS, __cxa_finalize_ranges runs ObjectPoolClass<X,256> global dtors after main() returns.
	// Those dtors crash with a corrupted BlockListHead (SIGSEGV at 0x4ade32ec4ade0018) because
	// pool block memory was already reused/overwritten during game shutdown.
	// Windows never had this problem — ExitProcess() terminates without running C++ global dtors.
	// _exit() matches that behavior. Explicit cleanup already done above (SDL_Quit, shutdownMemoryManager).
	_exit(exitcode);
}

#endif // !_WIN32
