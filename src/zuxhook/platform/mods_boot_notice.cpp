/* mods_boot_notice.c - tell the user, once, when Lyra changed something on its
   own: the boot ladder demoted, or a mod was disabled because a capability of
   its faulted while being applied. Both are otherwise silent until someone
   thinks to open the Mods tab.

   Runs in gemstone, hosted by the first scene created once the apply pass is
   over. ZDKSystem_ShowMessageBox was tried first and does not display from
   inside servicesd (device-checked: the notice ran and marked its record, no
   box appeared); it works in nativeapp's installer because that is a
   foreground process with its own surface. The HUD dialog at 0x72db8 draws and
   takes taps, and needs a scene only to route a result we do not use, so any
   live scene serves. Hosting here also puts the notice in the same process
   that writes the fault records, so nothing has to be coordinated.

   A demotion is a standing condition, so it is announced on every boot it
   applies to. A disable is an event, so the fault record carries a `reported`
   flag and each one is named once. */

extern "C" {
#include "boot_state.h"
#include "mod_fault.h"
#include "mods_log.h"
}

#include <windows.h>
#include "mods_boot_notice.h"

typedef int (*HudConfirmShowFn)(void* host, const wchar_t* message, int cfg, int flag,
                                int reserved, DWORD* out_handle);
#define HUD_CONFIRM_SHOW    ((HudConfirmShowFn)0x00072db8)
#define HUD_CONFIRM_CFG_OK  3
#define HUD_CONFIRM_FLAG    1

#define MODS_ROOT     L"\\flash2\\automation\\mods"
#define NOTICE_CHARS  512

static int process_is_gemstone(void) {
    wchar_t  path[MAX_PATH];
    wchar_t* base;
    DWORD    len = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return 0;
    base = path + len;
    while (base > path && *(base - 1) != L'\\') base--;
    return _wcsicmp(base, L"gemstone.exe") == 0;
}

static void append_w(wchar_t* dst, int* p, const wchar_t* s) {
    int i;
    for (i = 0; s[i] && *p < NOTICE_CHARS - 1; i++) dst[(*p)++] = s[i];
    dst[*p] = 0;
}

/* Name each mod disabled but not yet announced, marking them as it goes.
   Returns how many were named. */
static int append_unreported_disables(wchar_t* buf, int* p) {
    WIN32_FIND_DATAW fd;
    HANDLE h;
    wchar_t pattern[MAX_PATH];
    int found = 0;

    _snwprintf(pattern, MAX_PATH - 1, L"%s\\*", MODS_ROOT);
    pattern[MAX_PATH - 1] = 0;
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        wchar_t dir[MAX_PATH];
        ModFault f;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        _snwprintf(dir, MAX_PATH - 1, L"%s\\%s", MODS_ROOT, fd.cFileName);
        dir[MAX_PATH - 1] = 0;
        /* No installed version to match against here, so read whatever is
           recorded; the reported flag is what stops it repeating. */
        if (!ModFaultRead(dir, NULL, &f)) continue;
        if (!f.disabled || f.reported) continue;
        append_w(buf, p, found ? L", " : L"");
        append_w(buf, p, L"\"");
        append_w(buf, p, fd.cFileName);
        append_w(buf, p, L"\"");
        ModFaultMarkReported(dir);
        found++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

void ModBootNoticeShow(void* host_scene) {
    static volatile LONG shown = 0;
    wchar_t buf[NOTICE_CHARS];
    BootState bs;
    DWORD handle = 0;
    int p = 0, named;

    if (!host_scene) return;
    /* The proxy is installed in servicesd too, and the records this reads are
       written by gemstone's apply pass, so wait for it and ignore other hosts. */
    if (!BootStateApplyIsComplete()) return;
    if (!process_is_gemstone()) return;
    if (InterlockedExchange(&shown, 1) != 0) return;

    BootStateRead(&bs);
    buf[0] = 0;

    if (bs.level != BOOT_LEVEL_NORMAL) {
        append_w(buf, &p,
            L"Safe mode was enabled due to repeated failures at startup.\n\n"
            L"Your mods were not applied. Review them, or disable safe mode "
            L"from the mod manager.");
        ModsLogf("  boot notice: safe mode (%s)", BootLevelName(bs.level));
    } else {
        named = append_unreported_disables(buf, &p);
        if (named == 0) { InterlockedExchange(&shown, 0); return; }
        append_w(buf, &p,
            named == 1
              ? L" was disabled due to a failure at startup.\n\n"
                L"Review the failure or enable again from the mod manager."
              : L" were disabled due to failures at startup.\n\n"
                L"Review the failures or enable them again from the mod manager.");
        ModsLogf("  boot notice: %d mod(s) disabled after a fault", named);
    }

    __try {
        HUD_CONFIRM_SHOW(host_scene, buf, HUD_CONFIRM_CFG_OK, HUD_CONFIRM_FLAG,
                         0, &handle);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
