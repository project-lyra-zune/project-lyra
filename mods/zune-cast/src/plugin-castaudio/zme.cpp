#include "castaudio.h"
#include "zme.h"

// ZME0: live-session broker. Buffer layout per zpod-iap1-svc/svc_zdk.cpp's
// zme_get_ms: method@+0x00 (u16), HRESULT out@+0x04, kind@+0x0c, value out@
// +0x10, sentinel@+0x14 = 1.
#define ZME_IOCTL       0x201
#define ZME_KIND_MUSIC  1

static HANDLE g_zme = INVALID_HANDLE_VALUE;

// Length-parametric scalar getter. The ZME0 0x201 scalar getters validate the
// out-buffer length: the duration/position family wants 0x18 (with the +0x14
// sentinel), a second family wants 0x14 (no sentinel). method@+0x00 (u16),
// kind@+0x0c, HRESULT@+0x04, value written by the getter @+0x10.
static int zme_io(unsigned short method, unsigned int kind, unsigned int buflen,
                  unsigned int* out_hr, unsigned int* out_val)
{
    if (g_zme == INVALID_HANDLE_VALUE) {
        g_zme = CreateFileW(L"ZME0:", GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, 0, NULL);
        if (g_zme == INVALID_HANDLE_VALUE) return 0;
    }
    unsigned char buf[0x18];
    memset(buf, 0, sizeof(buf));
    *(unsigned short*)(buf + 0x00) = method;
    *(unsigned int*)  (buf + 0x0c) = kind;
    if (buflen >= 0x18) *(unsigned int*)(buf + 0x14) = 1;   // sentinel only on the 0x18 family
    DWORD ret = 0;
    if (!DeviceIoControl(g_zme, ZME_IOCTL, NULL, 0, buf, buflen, &ret, NULL)) {
        // The device node can go stale after a driver/WiFi reset; drop the handle so
        // the next read re-opens a fresh one instead of wedging every read (and every
        // duration/rate it feeds) to 0.
        CloseHandle(g_zme);
        g_zme = INVALID_HANDLE_VALUE;
        return 0;
    }
    *out_hr  = *(unsigned int*)(buf + 0x04);
    *out_val = *(unsigned int*)(buf + 0x10);
    return 1;
}

int zme_read(unsigned short method, unsigned int* out_hr, unsigned int* out_val)
{
    return zme_io(method, ZME_KIND_MUSIC, 0x18, out_hr, out_val);
}

int zme_read_kind(unsigned short method, unsigned int kind,
                  unsigned int* out_hr, unsigned int* out_val)
{
    return zme_io(method, kind, 0x18, out_hr, out_val);
}

// ZHD0: (volume/lock/battery HUD service, IOCTL 0x208): the broker that holds
// the on-screen volume slider value, separate from ZME0's media-stream volume.
static HANDLE g_zhd = INVALID_HANDLE_VALUE;
// ZHD0:/0x208 scalar getter: gemstone's volume read (method 0x0d) uses the
// sub_6fe2c shape: 0x14-byte buffer, method@+0 (u16), value in/out @+0xc; read
// = in NULL / out buf. (Method 0x0f is the WRITER; do not call it to read.)
int zhd_read(unsigned short method, unsigned int buflen, unsigned int* out_val)
{
    if (g_zhd == INVALID_HANDLE_VALUE) {
        g_zhd = CreateFileW(L"ZHD0:", GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, 0, NULL);
        if (g_zhd == INVALID_HANDLE_VALUE) return 0;
    }
    // ZHD_IOControl(0x208) reads the u16 method at buf+0 of the OUT buffer (in=NULL),
    // gates on an exact out-length per method, and writes the value to buf+4.
    unsigned char buf[0x40];
    memset(buf, 0, sizeof(buf));
    *(unsigned short*)(buf + 0x00) = method;
    DWORD ret = 0;
    if (!DeviceIoControl(g_zhd, 0x208, NULL, 0, buf, buflen, &ret, NULL)) return 0;
    *out_val = *(unsigned int*)(buf + 0x04);
    return 1;
}

// ZAM0:/0x204 method 0x12 = the live on-screen volume. The HUD VolumeUp/Down
// touch controls call method 0x13 (±delta); absolute set is 0x14. Plain read
// getter: value at buf+0xc, max at buf+0x14. No callback registration, so no
// input hook / touch interference.
static HANDLE g_zam = INVALID_HANDLE_VALUE;
int zam_volume(unsigned int* out_vol, unsigned int* out_max)
{
    if (g_zam == INVALID_HANDLE_VALUE) {
        g_zam = CreateFileW(L"ZAM0:", GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, 0, NULL);
        if (g_zam == INVALID_HANDLE_VALUE) return 0;
    }
    // sub_4189a47c gates length==0x1c, then writes the volume to buf+0xc and max
    // to buf+0x14 ONLY when buf+0x10 / buf+0x18 are non-zero (the getter's output
    // enables). Status (HRESULT) is written to buf+4.
    unsigned char buf[0x1c];
    memset(buf, 0, sizeof(buf));
    *(unsigned short*)(buf + 0x00) = 0x12;
    *(unsigned int*)  (buf + 0x10) = 1;   // enable volume write @ buf+0xc
    *(unsigned int*)  (buf + 0x18) = 1;   // enable max write   @ buf+0x14
    DWORD ret = 0;
    if (!DeviceIoControl(g_zam, 0x204, NULL, 0, buf, 0x1c, &ret, NULL)) {
        CloseHandle(g_zam); g_zam = INVALID_HANDLE_VALUE; return 0;
    }
    if (*(unsigned int*)(buf + 0x04) != 0) return 0;   // getter HRESULT
    *out_vol = *(unsigned int*)(buf + 0x0c);
    *out_max = *(unsigned int*)(buf + 0x14);
    return 1;
}

int zam_set_volume(unsigned int vol, unsigned int max)
{
    if (g_zam == INVALID_HANDLE_VALUE) {
        g_zam = CreateFileW(L"ZAM0:", GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, 0, NULL);
        if (g_zam == INVALID_HANDLE_VALUE) return 0;
    }
    // sub_4189a3e0 gates length==0x18; the real fn reads new_vol@buf+0xc,
    // max@buf+0x10, flag@buf+0x14 and forwards to the volume writer
    // sub_0x41893c18 (the same choke point the HUD Vol± buttons reach). flag=1
    // makes the writer publish the change event (as a button press does), so the
    // on-screen bar and the zune-volume-state push section stay in sync.
    unsigned char buf[0x18];
    memset(buf, 0, sizeof(buf));
    *(unsigned short*)(buf + 0x00) = 0x11;
    *(unsigned int*)  (buf + 0x0c) = vol;
    *(unsigned int*)  (buf + 0x10) = max;
    *(unsigned int*)  (buf + 0x14) = 1;
    DWORD ret = 0;
    if (!DeviceIoControl(g_zam, 0x204, NULL, 0, buf, 0x18, &ret, NULL)) {
        CloseHandle(g_zam); g_zam = INVALID_HANDLE_VALUE; return 0;
    }
    return 1;
}

int zme_play_state(void)
{
    unsigned int hr = 0xFFFFFFFFu, val = 0;
    if (!zme_read(0x29, &hr, &val) || hr != 0) return ZME_PS_UNKNOWN;
    if (val == 3) return ZME_PS_PLAYING;
    if (val == 4) return ZME_PS_PAUSED;
    return ZME_PS_UNKNOWN;   // other raw values (e.g. stopped): map when observed
}
