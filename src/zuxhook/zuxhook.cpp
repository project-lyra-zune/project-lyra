#include <windows.h>
#include <stdio.h>

#include "mods.h"
#include "mods_log.h"
#include "mods_phase2.h"
#include "mods_state_block.h"
#include "mods_icon_host.h"
#include "mods_state_event.h"
#include "mods_wifi_awake.h"
#include "mod_scanner.h"
#include "gemstone/repo_client.h"
#include "gemstone/gem_mod_detail.h"

// gemstone treats *out_handle as a kernel waitable HANDLE: it lands in
// state[0x40] and goes into slot 2 of a 9-element MsgWaitForMultipleObjectsEx
// array each music-shell tick. A non-NULL non-handle (an HMODULE, a flag)
// makes every wait return WAIT_FAILED and the UI message pump dies. An
// anonymous event we never signal keeps the wait healthy and prevents
// gemstone from ever calling ZUxHook back. gemstone does not CloseHandle
// this slot - the DLL owns the handle's lifetime.
static HANDLE g_wait_event = NULL;

// System-wide singleton. zuxhook.dll loads into more than one host
// process per boot (compositor.exe and gemstone.exe). The first
// ZUxHookInit to run wins this mutex and runs the watchdog forever;
// others see ERROR_ALREADY_EXISTS and skip.
//
// Watchdog: spawn nativeapp, hold its process handle, WaitForSingleObject
// for it to die, respawn. Without this loop, any nativeapp crash takes
// the listener out of service for the rest of the boot - host loses
// access entirely until power-cycle.
static DWORD WINAPI SpawnDaemonThread(LPVOID lpParam) {
	HANDLE hMutex = CreateMutexW(NULL, TRUE, L"zune-nativeapp-singleton");
	if (hMutex == NULL || GetLastError() == ERROR_ALREADY_EXISTS) {
		if (hMutex != NULL) CloseHandle(hMutex);
		return 0;
	}

	for (;;) {
		// Adopt-or-spawn: if a nativeapp from a prior compositor instance
		// is still alive, its self-mutex (zune-nativeapp-self) exists.
		// Wait on it instead of spawning a duplicate. Without this check,
		// each compositor restart leaks another nativeapp.exe.
		//
		// WinCE 6 has no OpenMutexW. Use CreateMutex with bInitialOwner=FALSE:
		// if the mutex existed, GetLastError() returns ERROR_ALREADY_EXISTS
		// and we get a handle to the existing mutex.
		HANDLE existing = CreateMutexW(NULL, FALSE, L"zune-nativeapp-self");
		DWORD err = GetLastError();
		if (existing != NULL && err == ERROR_ALREADY_EXISTS) {
			// Block until the holder releases (= nativeapp died).
			WaitForSingleObject(existing, INFINITE);
			// WaitForSingleObject claims ownership when it returns. Release
			// immediately so the next nativeapp can acquire it cleanly.
			ReleaseMutex(existing);
			CloseHandle(existing);
			Sleep(1000);
			continue;
		}
		// No existing nativeapp. Drop our placeholder handle (we don't want
		// to hold the mutex - nativeapp itself will recreate-with-ownership).
		if (existing != NULL) CloseHandle(existing);

		STARTUPINFOW si;
		PROCESS_INFORMATION pi;
		ZeroMemory(&si, sizeof(si));
		ZeroMemory(&pi, sizeof(pi));
		si.cb = sizeof(si);

		BOOL ok = CreateProcessW(
			L"\\flash2\\automation\\nativeapp.exe",
			NULL,
			NULL, NULL,
			FALSE, 0,
			NULL, NULL,
			&si, &pi);
		if (!ok) {
			// File missing or load failure. Back off a bit and retry -
			// avoids tight-spinning if the binary genuinely can't load.
			Sleep(5000);
			continue;
		}

		CloseHandle(pi.hThread);
		// Block until nativeapp exits (clean exit OR crash).
		WaitForSingleObject(pi.hProcess, INFINITE);
		CloseHandle(pi.hProcess);

		// nativeapp died. Brief pause, then respawn. The pause prevents
		// thrashing if nativeapp crashes immediately on launch (e.g.,
		// bind() loop never finds a free port).
		Sleep(1000);
	}
}

// In-host plugin loader. Waits on the system-named event the daemon
// signals via opcode 18; on each signal, LoadLibraryW the plugin from
// \flash2\automation\plugin.dll, GetProcAddress("Activate"), call
// it under SEH, FreeLibrary. Result + diagnostics get appended to
// \flash2\automation\plugin-result-<pid>.log so the host can read
// back via opcode 10.
//
// The fault-domain isolation we get: a plugin that bricks gemstone
// requires removing plugin.dll from disk before next boot (opcode 17
// MoveFileW or opcode 14 DeleteFileW). zuxhook itself does NOT load
// plugin.dll automatically at startup - only on trigger - so a bad
// plugin doesn't auto-brick subsequent boots.
static void PluginAppendLog(const wchar_t* fmt, ...) {
	wchar_t path[MAX_PATH];
	va_list ap;
	_snwprintf(path, MAX_PATH - 1, L"\\flash2\\automation\\plugin-result-%lu.log",
	           GetCurrentProcessId());
	path[MAX_PATH - 1] = 0;

	va_start(ap, fmt);
	mods_vflashlog(path, fmt, ap);
	va_end(ap);
}

static void LoadAndRunPlugin(void) {
	HMODULE h = LoadLibraryW(L"\\flash2\\automation\\plugin.dll");
	if (h == NULL) {
		PluginAppendLog(L"LoadLibrary failed err=%lu", GetLastError());
		return;
	}

	typedef int (*ActivateFn)(void);
	ActivateFn fn = (ActivateFn)GetProcAddress(h, L"Activate");
	if (fn == NULL) {
		PluginAppendLog(L"GetProcAddress(Activate) failed err=%lu", GetLastError());
		FreeLibrary(h);
		return;
	}

	int rc = -1;
	__try {
		rc = fn();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		PluginAppendLog(L"Activate threw exception code=0x%lx", GetExceptionCode());
		FreeLibrary(h);
		return;
	}

	PluginAppendLog(L"Activate returned rc=%d", rc);
	FreeLibrary(h);
}

static DWORD WINAPI PluginWaiterThread(LPVOID lpParam) {
	HANDLE hEvent = CreateEventW(NULL, FALSE, FALSE, L"zune-zuxhook-trigger");
	if (hEvent == NULL) return 0;

	for (;;) {
		DWORD r = WaitForSingleObject(hEvent, INFINITE);
		if (r != WAIT_OBJECT_0) break;
		LoadAndRunPlugin();
	}

	CloseHandle(hEvent);
	return 0;
}


extern "C" __declspec(dllexport) HRESULT ZUxHookInit(void* arg0, HANDLE* out_handle) {
	if (out_handle == NULL) return E_POINTER;
	if (g_wait_event == NULL) {
		g_wait_event = CreateEventW(NULL, FALSE, FALSE, NULL);
		if (g_wait_event == NULL) return E_FAIL;
	}
	*out_handle = g_wait_event;

	// Uninstall gate. A trigger wrote \flash2\automation\uninstall.pending and rebooted.
	// Consume it here, before the daemon spawns or any mod applies: at this instant only
	// zuxhook.dll itself is loaded, so nativeapp.exe / reposd.exe / the feature DLLs all
	// delete as plain files. zuxhook.dll loads into several hosts per boot; the first wins
	// the singleton and wipes, the rest wait on the done-event, and all return inert.
	if (ModScanUninstallArmed()) {
		HANDLE hDone  = CreateEventW(NULL, TRUE, FALSE, L"zune-zuxhook-uninstall-done");
		HANDLE hMutex = CreateMutexW(NULL, TRUE, L"zune-zuxhook-uninstall-singleton");
		if (hMutex != NULL && GetLastError() != ERROR_ALREADY_EXISTS) {
			ModScanUninstall();
			if (hDone != NULL) SetEvent(hDone);
		} else {
			if (hMutex != NULL) CloseHandle(hMutex);
			if (hDone != NULL) WaitForSingleObject(hDone, 30000);
		}
		return S_OK;
	}

	// Spawn the nativeapp watchdog FIRST so the daemon's startup runs in
	// parallel with the rest of ZUxHookInit. The thread self-gates via
	// the `zune-nativeapp-singleton` mutex so only the compositor
	// instance actually spawns (gemstone/zie ZUxHookInits see
	// ERROR_ALREADY_EXISTS and the thread exits immediately).
	HANDLE hSpawn = CreateThread(NULL, 0, SpawnDaemonThread, NULL, 0, NULL);
	if (hSpawn != NULL) CloseHandle(hSpawn);

	// Manifest-driven mod composition. zuxhook.dll loads into more than
	// one host process per boot (servicesd, compositor, gemstone); a
	// system-named singleton mutex gates Phase 1 to the first ZUxHookInit
	// that wins it.
	//
	// Non-runners MUST WAIT for Phase 1, not just skip it. Phase 1 composes
	// \Windows\*.gem; gemstone renders the Start menu right after ZUxHookInit
	// returns, and that menu's injected Mods tile reads strings.gem (the "mods"
	// label) + scenes_standard.gem (Mods.xur) at render time. A process that
	// skipped Phase 1 and proceeded while the runner was still composing would
	// read a half-written gem and hang the shell (the flaky early-boot black
	// screen). The runner SetEvents a manual-reset done-event; everyone else
	// blocks on it before proceeding. 30s is a safety cap, not an expected wait.
	{
		HANDLE hDone  = CreateEventW(NULL, TRUE, FALSE,
			L"zune-zuxhook-mods-phase1-done");
		HANDLE hMutex = CreateMutexW(NULL, TRUE,
			L"zune-zuxhook-mods-phase1-singleton");
		if (hMutex != NULL && GetLastError() != ERROR_ALREADY_EXISTS) {
			ModsApplyPhase1();
			if (hDone != NULL) SetEvent(hDone);
		} else {
			if (hMutex != NULL) CloseHandle(hMutex);
			if (hDone != NULL) WaitForSingleObject(hDone, 30000);
		}
	}

	// Host-branch by process name. zuxhook.dll runs in several processes:
	// gemstone and servicesd get deferred Phase 2 mod application; zie.exe
	// gets a synchronous inline apply (it has no xuidll registry to wait on);
	// servicesd.exe also hosts zhud_serv's HUD scene classes.
	{
		wchar_t exe_path[MAX_PATH];
		DWORD len = GetModuleFileNameW(NULL, exe_path, MAX_PATH);
		if (len > 0 && len < MAX_PATH) {
			wchar_t* base = exe_path + len;
			while (base > exe_path && *(base - 1) != L'\\') base--;

			if (_wcsicmp(base, L"gemstone.exe") == 0) {
				ModsApplyPhase2("gemstone");
				// gemstone maps the same shared block the servicesd-hosted
				// toggle action writes; the NowPlaying status icon pull-reads
				// its slot on each refresh tick. Snapshot here confirms the
				// section maps cross-process with the value servicesd seeded.
				ModStateLogSnapshot(L"gemstone");

				// Runtime status-icon injection host: hook XuiSceneCreateEx
				// and, on each created scene that carries an iconGrid, inject
				// each mod's add_status_icon fragment + AddChild it into the
				// grid. ModsIconTick (the UI-loop hook) lays it out and shows it
				// from its slot - no per-mod icon class.
				ModsIconHostInstall(ICON_HOST_IAT_GEMSTONE);

				// Cross-process state-change consumer: redirect gemstone's single
				// MsgWait call (its UI main loop) so our notification queue joins
				// the loop's wait set; a slot write in any process wakes this UI
				// thread and re-renders the NowPlaying icon.
				ModStateEventInstallConsumer(STATE_EVENT_MSGWAIT_IAT_GEMSTONE,
				                             L"zune-mod-state-q-gem");

				// Mod-repository UI client: map reposd's shared section and chain
				// the pump's MsgWait so a reposd DONE (feed answered / install
				// progressed) is delivered on this UI thread. The Browse/Manage
				// scenes (registered via class blobs) drive it; browse.dll is retired.
				RepoClientInstall();
				RepoClientSetOnDone(GemModDetailOnRepoDone);
			} else if (_wcsicmp(base, L"zie.exe") == 0) {
				// zie has no xuidll class registry; its mods (a browser
				// load_module that hooks CoCreateInstance) must install
				// synchronously before zie creates the WebBrowser control,
				// so apply target_proc:"zie" actions inline here rather than
				// via the deferred Phase2Worker.
				ModsApplyHostInline("zie");
			} else if (_wcsicmp(base, L"servicesd.exe") == 0) {
				// servicesd hosts the quick-toggle menu + the setting actions.
				// Boot defaults are seeded declaratively by each mod's
				// register_setting action (Phase 2 below), not here.
				ModStateLogSnapshot(L"servicesd");

				// HUD scene host (zhud_serv): apply servicesd-targeted Phase 2
				// caps (mod scene classes, visuals, settings), then hook zhud's
				// XuiSceneCreateEx - injects status icons into the HUD iconGrid.
				ModsApplyPhase2("servicesd");
				ModsIconHostInstall(ICON_HOST_IAT_SERVICESD);

				// Quick-toggle context pickers are registered declaratively from
				// each mod's manifest ("context") during ModsApplyPhase2 above -
				// no per-mod wiring here (see ModListChannelProviderRegister).

				// Cross-process state-change consumer for the HUD (zhud) UI loop -
				// same mechanism as gemstone, its own per-process queue.
				ModStateEventInstallConsumer(STATE_EVENT_MSGWAIT_IAT_SERVICESD,
				                             L"zune-mod-state-q-svc");

				// The on-screen volume publisher (ZAM-writer detour to the
				// zune-volume-state section) and the WiFi Awake authority
				// activate on demand, not at boot: each starts when a mod
				// declares `require_subsystem` (volume_state / wifi_awake,
				// servicesd Phase 2), so a device with no cast mod never
				// patches zam_serv and one with no WiFi mod never patches
				// znet_serv.
			}
		}
	}

	HANDLE hPlugin = CreateThread(NULL, 0, PluginWaiterThread, NULL, 0, NULL);
	if (hPlugin != NULL) CloseHandle(hPlugin);

	return S_OK;
}

extern "C" __declspec(dllexport) HRESULT ZUxHookShutdown(void) {
	if (g_wait_event != NULL) {
		CloseHandle(g_wait_event);
		g_wait_event = NULL;
	}
	return S_OK;
}

extern "C" __declspec(dllexport) HRESULT ZUxHook(void) {
	return S_OK;
}

// Named hook entry points zie.exe resolves via GetProcAddress at startup
// (PreInput/PostInput/SendEvent in its hook struct at 0x00040b90). Static
// disasm shows zero callsites - the symbols are resolved but the call paths
// weren't shipped. Exported inert so the resolution doesn't return NULL.
extern "C" __declspec(dllexport) HRESULT ZUxHookPreInput(void* a, void* b, void* c, void* d) {
	(void)a; (void)b; (void)c; (void)d;
	return S_OK;
}

extern "C" __declspec(dllexport) HRESULT ZUxHookPostInput(void* a, void* b, void* c, void* d) {
	(void)a; (void)b; (void)c; (void)d;
	return S_OK;
}

extern "C" __declspec(dllexport) HRESULT ZUxHookSendEvent(void* a, void* b, void* c, void* d) {
	(void)a; (void)b; (void)c; (void)d;
	return S_OK;
}

extern "C" BOOL WINAPI DllMain(HANDLE hinstDLL, DWORD dwReason, LPVOID lpvReserved) {
	return TRUE;
}
