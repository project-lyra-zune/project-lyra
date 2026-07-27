/* The browser install's bootstrap: fetch the pinned bundle, verify it, unpack it
 * into the automation root, reboot. */

#include <winsock2.h>
#include <windows.h>

#include "ce_log.h"
#include "lyra_platform.h"
#include "zmod_extract.h"
#include "zmod_sha256.h"
#include "boot_http.h"
#include "boot_pin.h"
#include "device_reboot.h"

#define AUTOMATION_DIR  L"\\flash2\\automation"
#define TMP_ZMOD        L"\\flash2\\automation\\_lyraboot_dl.zmod"
#define MAX_ZMOD_BYTES  (8u * 1024u * 1024u)
#define SHA_HEX_LEN     65

CE_LOGGER(L, L"\\flash2\\automation\\lyraboot.log")

enum {
    RC_OK = 0,
    RC_DOWNLOAD = 1,
    RC_DIGEST   = 2,
    RC_UNPACK   = 3
};

static void on_progress(unsigned long got, unsigned long total, void* ctx) {
    int* last_quarter = (int*)ctx;
    int q = (total > 0) ? (int)((got * 4) / total) : 0;
    if (q > *last_quarter) {
        *last_quarter = q;
        L("download: %lu/%lu", got, total);
    }
}

static int install(void) {
    static const char* const defer_last[] = LYRA_INSTALL_DEFER_LAST;
    char hex[SHA_HEX_LEN];
    zmod_error ze;
    int http_status = 0, quarter = 0, hr;
    unsigned long got = 0;

    L("fetch %s", LYRA_PIN_URL);
    hr = boot_http_download(LYRA_PIN_URL, TMP_ZMOD, MAX_ZMOD_BYTES,
                            on_progress, &quarter, &http_status, &got);
    if (hr != BOOT_HTTP_OK) {
        L("fetch failed: %s http=%d got=%lu", boot_http_result_str(hr), http_status, got);
        return RC_DOWNLOAD;
    }
    L("fetched %lu bytes", got);

    if (!zmod_sha256_file(TMP_ZMOD, hex)) {
        DeleteFileW(TMP_ZMOD);
        L("digest: cannot read download");
        return RC_DIGEST;
    }
    if (!zmod_sha256_hex_equal(hex, LYRA_PIN_SHA256)) {
        DeleteFileW(TMP_ZMOD);
        L("digest mismatch: got %s want %s", hex, LYRA_PIN_SHA256);
        return RC_DIGEST;
    }

    if (!zmod_extract(TMP_ZMOD, AUTOMATION_DIR, defer_last,
                      LYRA_INSTALL_DEFER_LAST_COUNT, &ze)) {
        DeleteFileW(TMP_ZMOD);
        L("unpack failed stage=%d rc=%ld entry=%s", ze.stage, ze.rc, ze.entry);
        return RC_UNPACK;
    }
    DeleteFileW(TMP_ZMOD);
    return RC_OK;
}

int WINAPI wWinMain(HINSTANCE a, HINSTANCE b, LPWSTR c, int d) {
    WSADATA wsa;
    int rc;

    (void)a; (void)b; (void)c; (void)d;

    CreateDirectoryW(AUTOMATION_DIR, NULL);
    L("=== lyraboot start, platform %s ===", LYRA_PIN_VERSION);

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        L("WSAStartup failed");
        return RC_DOWNLOAD;
    }

    rc = install();
    WSACleanup();

    if (rc != RC_OK) {
        L("install failed rc=%d, device untouched by reboot", rc);
        return rc;
    }

    L("install done, rebooting");
    RebootDevice();
    return RC_OK;
}
