/* Canonical enabled.json codec. See enabled_set.h.

   The schema is fixed and trivial: {"version":1,"enabled":["id",...]}.
   The array-of-strings extractor tolerates arbitrary whitespace between
   tokens; it does not accept escapes, which mod_ids never contain. */

#include <windows.h>
#include <string.h>
#include <stdio.h>
#include "enabled_set.h"

#define ENABLED_JSON_PATH  L"\\flash2\\automation\\mods\\enabled.json"
#define ENABLED_BUF_BYTES  8192

int EnabledSetRead(char ids[][ENABLED_ID_LEN], int max) {
    char buf[ENABLED_BUF_BYTES];
    HANDLE h;
    DWORD sz, got = 0;
    char *a, *e, *p, *q;
    int n = 0;

    h = CreateFileW(ENABLED_JSON_PATH, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;   /* absent -> no mods active */
    sz = GetFileSize(h, NULL);
    if (sz == INVALID_FILE_SIZE || sz >= ENABLED_BUF_BYTES) {
        CloseHandle(h);
        return -1;
    }
    if (!ReadFile(h, buf, sz, &got, NULL)) { CloseHandle(h); return -1; }
    CloseHandle(h);
    buf[got] = 0;

    a = strstr(buf, "\"enabled\"");
    if (!a) return -1;
    a = strchr(a, '[');
    if (!a) return -1;
    e = strchr(a, ']');
    p = a;
    while (n < max) {
        int len;
        p = strchr(p + 1, '"');
        if (!p || (e && p > e)) break;
        q = strchr(p + 1, '"');
        if (!q || (e && q > e)) break;
        len = (int)(q - p - 1);
        if (len > 0 && len < ENABLED_ID_LEN) {
            memcpy(ids[n], p + 1, (size_t)len);
            ids[n][len] = 0;
            n++;
        }
        p = q;
    }
    return n;
}

int EnabledSetWrite(const char ids[][ENABLED_ID_LEN], int count) {
    char out[ENABLED_BUF_BYTES];
    int o = 0, i, r;
    HANDLE h;
    DWORD w = 0;
    BOOL ok;

    r = _snprintf(out + o, ENABLED_BUF_BYTES - o, "{\"version\":1,\"enabled\":[");
    if (r < 0) return -1;
    o += r;
    for (i = 0; i < count; i++) {
        r = _snprintf(out + o, ENABLED_BUF_BYTES - o, "%s\"%s\"", i ? "," : "", ids[i]);
        if (r < 0) return -1;
        o += r;
    }
    r = _snprintf(out + o, ENABLED_BUF_BYTES - o, "]}");
    if (r < 0) return -1;
    o += r;

    h = CreateFileW(ENABLED_JSON_PATH, GENERIC_WRITE, 0, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    ok = WriteFile(h, out, (DWORD)o, &w, NULL);
    CloseHandle(h);
    return (ok && w == (DWORD)o) ? 0 : -1;
}

int EnabledSetContains(const char ids[][ENABLED_ID_LEN], int count,
                       const char* id) {
    int i;
    for (i = 0; i < count; i++)
        if (strcmp(ids[i], id) == 0) return 1;
    return 0;
}

int EnabledSetAdd(const char* id) {
    static char ids[ENABLED_ID_MAX][ENABLED_ID_LEN];
    int n = EnabledSetRead(ids, ENABLED_ID_MAX);
    if (n < 0) n = 0;   /* malformed: rewrite a clean set rather than propagate it */
    if (EnabledSetContains(ids, n, id)) return 0;
    if (n >= ENABLED_ID_MAX) return -1;
    strncpy(ids[n], id, ENABLED_ID_LEN - 1);
    ids[n][ENABLED_ID_LEN - 1] = 0;
    n++;
    return EnabledSetWrite(ids, n);
}

int EnabledSetRemove(const char* id) {
    static char ids[ENABLED_ID_MAX][ENABLED_ID_LEN];
    int n = EnabledSetRead(ids, ENABLED_ID_MAX);
    int w = 0, i;
    if (n <= 0) return 0;
    for (i = 0; i < n; i++) {
        if (strcmp(ids[i], id) != 0) {
            if (w != i) memcpy(ids[w], ids[i], ENABLED_ID_LEN);
            w++;
        }
    }
    if (w == n) return 0;   /* not present */
    return EnabledSetWrite(ids, w);
}
