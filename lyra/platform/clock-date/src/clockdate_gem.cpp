/*
 * clock-date: GemSettingSetDateScene, loaded into gemstone via the modkit
 * load_module system-mod path (init export ClockDateInstall). Registers one
 * novel XUI scene class directly via xuidll!XuiRegisterClass, modeled on the
 * youtube mod and gemstone's own GemSettingSetTimeScene. Three number wheels
 * (month/day/year); OK sets the WM8350 PMU RTC (the battery-backed master the
 * bootloader seeds the OS clock from at boot) via user-mode libnvrm.dll, plus
 * SetLocalTime for immediate in-session effect. Gemstone addresses are v4.5 at
 * its preferred base 0x00010000 (static VA == live VA, verified).
 */
#include <windows.h>
#include <stdio.h>

// ── gemstone fixed addresses (base 0x10000) ─────────────────────────────────
#define ALLOCATOR            0x00084ee4   // coredll ord1095 ("operator new")
#define PARENT_VTABLE_BASE   0x0002aca0   // GemBaseScene base vtable
#define CLASS_DESTROY_SHARED 0x0008678c   // canonical OnDestroy
#define DESC_DESTRUCTOR      0x0002acf4
#define DESC_FINALIZER       0x0002a7ac
#define GEMBASESCENE_NAME    0x00011be0   // L"GemBaseScene"

typedef HRESULT (*GetChildFn)(void* scene_handle, const wchar_t* id, void** out, int zero);
#define XUI_GET_CHILD    ((GetChildFn)0x0006afec)              // XuiElementGetDescendantById
typedef void (*SetWheelFn)(void* self, int wheel_index, int value);
#define SET_WHEEL        ((SetWheelFn)0x00059f84)              // seed a wheel to `value` (coarse+fine)
typedef int  (*GetSelFn)(void* list, int* out);
#define GET_SEL          ((GetSelFn)0x0003195c)                // selected index of a wheel (returns index)
typedef void (*ListInvalidateFn)(void* list, int a, int b);
#define LIST_INVALIDATE  ((ListInvalidateFn)0x00058890)        // force a wheel to re-query its data
typedef void (*WheelSettleFn)(void* self, int wheel_index);
#define WHEEL_SETTLE     ((WheelSettleFn)0x0005a0dc)           // snap wheel to nearest item + commit selection

// WM8350 PMU RTC via user-mode libnvrm.dll (by name). Must run on a real thread:
// the RM completes the I2C in its driver context, so a kcall of this path returns 0.
typedef void* NvRmDeviceHandle;
typedef int   NvError;   // 0 == NvSuccess
typedef unsigned char NvBool;
typedef unsigned int  NvU32;
typedef NvError (*pfnNvRmOpen)(NvRmDeviceHandle*, NvU32);
typedef void    (*pfnNvRmClose)(NvRmDeviceHandle);
typedef NvBool  (*pfnNvRmPmuReadRtc)(NvRmDeviceHandle, NvU32*);
typedef NvBool  (*pfnNvRmPmuWriteRtc)(NvRmDeviceHandle, NvU32);

#define YEAR_BASE  2000
#define YEAR_COUNT 100   // 2000..2099

// Instance layout mirrors GemSettingSetTimeScene: the native SET_WHEEL / settle
// helpers read the per-wheel list at +0x08+i*4 and touch element at +0x14+i*4.
typedef struct {
    DWORD vtable;         // +0x00
    void* scene_handle;   // +0x04 (load-bearing; base reads it)
    void* list[3];        // +0x08/+0x0c/+0x10  month/day/year list  (SET_WHEEL slots)
    void* touch[3];       // +0x14/+0x18/+0x1c  month/day/year touch (SET_WHEEL slots)
    DWORD reserved[10];   // +0x20..+0x4c  headroom for base-scene message handling
} DateInstance;           // 0x50 bytes

// Cached handles live in module globals, not instance fields: the base scene
// zeroes some instance offsets after the first render pass. Only one SET DATE
// scene is ever live; OnInit refreshes these.
static DWORD g_month_list = 0, g_day_list = 0, g_year_list = 0;
static DWORD g_month_touch = 0, g_day_touch = 0, g_year_touch = 0;
static DWORD g_ok_button = 0, g_cancel_button = 0;

static void L(const char* s) {
    HANDLE f = CreateFileW(L"\\flash2\\automation\\clockdate.log", GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    SetFilePointer(f, 0, NULL, FILE_END);
    { DWORD n; WriteFile(f, s, (DWORD)strlen(s), &n, NULL); WriteFile(f, "\r\n", 2, &n, NULL); }
    CloseHandle(f);
}

static int days_in_month(int month, int year) {
    static const int d[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (month < 1 || month > 12) return 31;
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
    return d[month - 1];
}

static int st_to_unix(const SYSTEMTIME* st, NvU32* out) {
    FILETIME ft;
    ULONGLONG t;
    if (!SystemTimeToFileTime(st, &ft)) return 0;
    t = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    *out = (NvU32)((t - 116444736000000000ULL) / 10000000ULL);  // 100ns-since-1601 to unix
    return 1;
}

// Set the date (time-of-day kept). SetLocalTime is this-session only; the boot clock
// is reseeded as WM8350 + a fixed offset, so persistence means writing the WM8350.
// offset is self-calibrated (OS clock - WM8350) to avoid hardcoding the epoch quirk.
static void write_date(int year, int month, int day) {
    SYSTEMTIME cur; GetLocalTime(&cur);
    SYSTEMTIME tgt = cur;
    HMODULE m;
    tgt.wYear = (WORD)year; tgt.wMonth = (WORD)month; tgt.wDay = (WORD)day;

    SetLocalTime(&tgt);

    m = GetModuleHandleW(L"libnvrm.dll");
    if (!m) m = LoadLibraryW(L"libnvrm.dll");
    if (!m) return;
    {
        pfnNvRmOpen open = (pfnNvRmOpen)GetProcAddress(m, L"NvRmOpen");
        pfnNvRmClose close = (pfnNvRmClose)GetProcAddress(m, L"NvRmClose");
        pfnNvRmPmuReadRtc rd = (pfnNvRmPmuReadRtc)GetProcAddress(m, L"NvRmPmuReadRtc");
        pfnNvRmPmuWriteRtc wr = (pfnNvRmPmuWriteRtc)GetProcAddress(m, L"NvRmPmuWriteRtc");
        NvRmDeviceHandle h = NULL;
        NvU32 rtc = 0, os_now = 0, tgt_unix = 0;
        if (!open || !rd || !wr) return;
        if (open(&h, 0) != 0 || !h) return;
        if (rd(h, &rtc) && st_to_unix(&cur, &os_now) && st_to_unix(&tgt, &tgt_unix))
            wr(h, tgt_unix - (os_now - rtc));   // WM8350 = target - (OS - WM8350) boot-seed offset
        if (close) close(h);
    }
}

// ── data-source / message plumbing ──────────────────────────────────────────
typedef HRESULT (*MessageHandlerFn)(void* self, void* msg);
#define XUISCENE_ON_MESSAGE  ((MessageHandlerFn)0x000653f4)   // GemBaseScene::OnMessage (chain target)
typedef HRESULT (*RawRowLabelFn)(DWORD out_8, DWORD out_c, const wchar_t* text);
#define RAW_ROW_LABEL  ((RawRowLabelFn)0x00083914)            // wstring row assign (no string id)
typedef HRESULT (*SceneBackFn)(void* scene_handle);
#define SCENE_BACK  ((SceneBackFn)0x0001c2d8)                 // close this scene (OK + Cancel)

// msg 0xe data-source sub-struct at msg[4] = {sub_code, target_elem, size, output_area}.
#define SUB_SET_SEL   0x01     // button tap
#define SUB_SETTLE    0x11     // wheel touch settle
#define SUB_GET_ITEM  0x3e8
#define SUB_COUNT     0x3eb

static void fmt_2d(wchar_t* b, int v) {
    b[0] = (wchar_t)(L'0' + (v / 10) % 10);
    b[1] = (wchar_t)(L'0' + v % 10);
    b[2] = 0;
}
static void fmt_year_w(wchar_t* b, int v) {
    b[0] = (wchar_t)(L'0' + (v / 1000) % 10);
    b[1] = (wchar_t)(L'0' + (v / 100) % 10);
    b[2] = (wchar_t)(L'0' + (v / 10) % 10);
    b[3] = (wchar_t)(L'0' + v % 10);
    b[4] = 0;
}
static int wheel_count(DWORD target) {
    if (target == g_month_list) return 12;
    if (target == g_day_list)   return 31;   // month-clamped at OK
    if (target == g_year_list)  return YEAR_COUNT;
    return -1;
}
static int touch_wheel_index(DWORD target) {
    if (target == g_month_touch) return 0;
    if (target == g_day_touch)   return 1;
    if (target == g_year_touch)  return 2;
    return -1;
}
// SET_WHEEL / WHEEL_SETTLE read the list at +0x08+i*4 and touch at +0x14+i*4;
// the base scene zeroes some of these, so re-lay them out before each call.
static void setup_wheel_layout(DateInstance* self) {
    self->list[0]  = (void*)g_month_list;  self->list[1]  = (void*)g_day_list;  self->list[2]  = (void*)g_year_list;
    self->touch[0] = (void*)g_month_touch; self->touch[1] = (void*)g_day_touch; self->touch[2] = (void*)g_year_touch;
}

static void do_ok(DateInstance* self) {
    int month = 0, day = 0, year = 0, dm;
    (void)self;
    // GET_SEL returns the selected row index (its 2nd arg is not the out-param).
    if (g_month_list) month = GET_SEL((void*)g_month_list, 0);
    if (g_day_list)   day   = GET_SEL((void*)g_day_list,   0);
    if (g_year_list)  year  = GET_SEL((void*)g_year_list,  0);
    if (month < 0) month = 0;
    if (day < 0) day = 0;
    if (year < 0) year = 0;
    month += 1; day += 1; year += YEAR_BASE;
    dm = days_in_month(month, year);
    if (day > dm) day = dm;
    write_date(year, month, day);
}

// ── scene methods ───────────────────────────────────────────────────────────
static HRESULT GemSettingSetDate_OnInit(DateInstance* self) {
    void* h;
    if (!self) return (HRESULT)-1;
    h = 0; XUI_GET_CHILD(self->scene_handle, L"MonthList",    &h, 0); g_month_list    = (DWORD)h;
    h = 0; XUI_GET_CHILD(self->scene_handle, L"DayList",      &h, 0); g_day_list      = (DWORD)h;
    h = 0; XUI_GET_CHILD(self->scene_handle, L"YearList",     &h, 0); g_year_list     = (DWORD)h;
    h = 0; XUI_GET_CHILD(self->scene_handle, L"MonthTouch",   &h, 0); g_month_touch   = (DWORD)h;
    h = 0; XUI_GET_CHILD(self->scene_handle, L"DayTouch",     &h, 0); g_day_touch     = (DWORD)h;
    h = 0; XUI_GET_CHILD(self->scene_handle, L"YearTouch",    &h, 0); g_year_touch    = (DWORD)h;
    h = 0; XUI_GET_CHILD(self->scene_handle, L"OkButton",     &h, 0); g_ok_button     = (DWORD)h;
    h = 0; XUI_GET_CHILD(self->scene_handle, L"CancelButton", &h, 0); g_cancel_button = (DWORD)h;
    return 0;
}

static HRESULT GemSettingSetDate_OnMessage(DateInstance* self, DWORD* msg) {
    DWORD type = 0;
    HRESULT hr = 0;

    __try { type = msg[1]; } __except (EXCEPTION_EXECUTE_HANDLER) { type = 0; }

    // msg 0x18000007 (native SeedFromClock): invalidate each wheel to trigger its
    // data pull, then SET_WHEEL to the current date so a drag snaps to a value
    // and OK reads the choice. Wheels render empty without the invalidate.
    if (type == 0x18000007) {
        SYSTEMTIME now; GetLocalTime(&now);
        __try {
            if (g_month_list) LIST_INVALIDATE((void*)g_month_list, 0, 1);
            if (g_day_list)   LIST_INVALIDATE((void*)g_day_list,   0, 1);
            if (g_year_list)  LIST_INVALIDATE((void*)g_year_list,  0, 1);
            setup_wheel_layout(self);
            if (g_month_list) SET_WHEEL(self, 0, now.wMonth - 1);
            if (g_day_list)   SET_WHEEL(self, 1, now.wDay - 1);
            if (g_year_list)  SET_WHEEL(self, 2, (int)now.wYear - YEAR_BASE);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    if (type == 0xe) {
        DWORD sub_code = 0, target = 0, out_area = 0;
        __try {
            DWORD* p = (DWORD*)msg[4];
            if (p) { sub_code = p[0]; target = p[1]; out_area = p[3]; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        if (sub_code == SUB_COUNT) {
            int c = wheel_count(target);
            if (c >= 0) {
                __try { *(DWORD*)(out_area + 4) = (DWORD)c; msg[2] = 1; }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
                return 0;
            }
        } else if (sub_code == SUB_GET_ITEM) {
            if (wheel_count(target) >= 0) {
                int idx = -1; DWORD out_8 = 0, out_c = 0;
                __try {
                    DWORD* o = (DWORD*)out_area;
                    if (o) { idx = (int)o[0]; out_8 = o[2]; out_c = o[3]; }
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
                if (idx >= 0) {
                    wchar_t buf[8];
                    if (target == g_year_list) fmt_year_w(buf, YEAR_BASE + idx);
                    else                       fmt_2d(buf, idx + 1);
                    __try { if ((int)RAW_ROW_LABEL(out_8, out_c, buf) >= 0) msg[2] = 1; }
                    __except (EXCEPTION_EXECUTE_HANDLER) {}
                }
                return 0;
            }
        } else if (sub_code == SUB_SETTLE) {
            // wheel touch settle: snap to the nearest item and commit it (the
            // native GemSettingSetTimeScene path; without it a drag never commits).
            int idx = touch_wheel_index(target);
            if (idx >= 0) {
                __try { setup_wheel_layout(self); WHEEL_SETTLE(self, idx); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
                __try { msg[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
                return 0;
            }
        } else if (sub_code == SUB_SET_SEL) {
            if (target && (target == g_ok_button || target == g_cancel_button)) {
                if (target == g_ok_button) do_ok(self);
                __try { SCENE_BACK(self->scene_handle); } __except (EXCEPTION_EXECUTE_HANDLER) {}
                __try { msg[2] = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
                return 0;
            }
        }
    }

    __try { hr = XUISCENE_ON_MESSAGE(self, msg); } __except (EXCEPTION_EXECUTE_HANDLER) { hr = 0; }
    return hr;
}

// ── registration (youtube system-mod pattern) ───────────────────────────────
typedef void* (*AllocFn)(SIZE_T);
static DWORD g_vtable[4];
static DWORD g_desc[11];
static const wchar_t SCENE_NAME[] = L"GemSettingSetDateScene";

static HRESULT date_factory(void* ctx, void** out) {
    void* inst;
    *out = NULL;
    inst = ((AllocFn)ALLOCATOR)(sizeof(DateInstance));
    if (!inst) return (HRESULT)0x8007000E;
    memset(inst, 0, sizeof(DateInstance));
    ((DWORD*)inst)[0] = (DWORD)g_vtable;
    ((DWORD*)inst)[1] = (DWORD)ctx;
    *out = inst;
    return 0;
}

typedef HRESULT (*XuiRegisterClassFn)(void* name_field, void* descriptor);

extern "C" __declspec(dllexport) int ClockDateInstall(void) {
    HMODULE x;
    XuiRegisterClassFn reg;
    int i;
    HRESULT h;

    L("ClockDateInstall: loaded into gemstone");
    g_vtable[0] = PARENT_VTABLE_BASE;
    g_vtable[1] = (DWORD)GemSettingSetDate_OnMessage;
    g_vtable[2] = (DWORD)GemSettingSetDate_OnInit;
    g_vtable[3] = CLASS_DESTROY_SHARED;

    x = GetModuleHandleW(L"xuidll.dll");
    if (!x) x = LoadLibraryW(L"xuidll.dll");
    if (!x) { L("no xuidll"); return -1; }
    reg = (XuiRegisterClassFn)GetProcAddress(x, L"XuiRegisterClass");
    if (!reg) { L("no XuiRegisterClass"); return -1; }

    for (i = 0; i < 11; i++) g_desc[i] = 0;
    g_desc[1] = (DWORD)SCENE_NAME;
    g_desc[2] = GEMBASESCENE_NAME;
    g_desc[5] = DESC_DESTRUCTOR;
    g_desc[6] = (DWORD)date_factory;
    g_desc[7] = DESC_FINALIZER;
    h = reg(&g_desc[1], &g_desc[0]);
    { char b[64]; _snprintf(b, sizeof(b), "register GemSettingSetDateScene h=0x%08x", (unsigned)h); b[63]=0; L(b); }
    return 0;
}

extern "C" BOOL WINAPI DllMain(HANDLE h, DWORD r, LPVOID l) { (void)h; (void)r; (void)l; return TRUE; }
