#include <windows.h>
#include <string.h>
#include <stdlib.h>

#include "cast_channel.h"
#include "cast_keys.h"   /* CAST_TOGGLE_KEY: this mod's channel */

#define CH_DEFAULT_PORT 8009

HANDLE cast_channel_scan_event(void)          { return lyra_channel_scan_event(CAST_TOGGLE_KEY); }
void   cast_channel_set_sublabel(const wchar_t* text) { lyra_channel_set_sublabel(CAST_TOGGLE_KEY, text); }

void cast_channel_publish(const MdnsDevice* devs, int n)
{
    char values[LYRA_CHANNEL_ROWS_MAX][LYRA_CHANNEL_VALUE_MAX];
    int  count = 0, published, i, k;
    if (n < 0) n = 0;
    if (n > LYRA_CHANNEL_ROWS_MAX) n = LYRA_CHANNEL_ROWS_MAX;

    /* Fresh scan results first, so new data wins for a device present in both. */
    for (i = 0; i < n; i++) {
        wchar_t name[LYRA_CHANNEL_NAME_MAX];
        for (k = 0; k < LYRA_CHANNEL_NAME_MAX - 1 && devs[i].name[k]; k++)
            name[k] = (wchar_t)(unsigned char)devs[i].name[k];
        name[k] = 0;
        _snprintf(values[count], LYRA_CHANNEL_VALUE_MAX, "%s:%u", devs[i].ip, devs[i].port);
        values[count][LYRA_CHANNEL_VALUE_MAX - 1] = 0;
        lyra_channel_stage_row(CAST_TOGGLE_KEY, count, name, L"", values[count]);
        count++;
    }

    /* Carry over previously-known devices this scan missed. mDNS drops packets,
     * worse while a cast saturates WiFi, so replacing the list wholesale would
     * lose a device still on the LAN. Union by "ip:port". Staged rows are not
     * visible until commit, so reading the published list here is safe. */
    published = lyra_channel_row_count(CAST_TOGGLE_KEY);
    for (i = 0; i < published && count < LYRA_CHANNEL_ROWS_MAX; i++) {
        wchar_t name[LYRA_CHANNEL_NAME_MAX];
        wchar_t sub[LYRA_CHANNEL_NAME_MAX];
        char    value[LYRA_CHANNEL_VALUE_MAX];
        int     dup = 0, j;
        if (!lyra_channel_get_row(CAST_TOGGLE_KEY, i, name, LYRA_CHANNEL_NAME_MAX,
                                  sub, LYRA_CHANNEL_NAME_MAX,
                                  value, LYRA_CHANNEL_VALUE_MAX)) continue;
        if (!value[0]) continue;
        for (j = 0; j < count; j++)
            if (strcmp(values[j], value) == 0) { dup = 1; break; }
        if (dup) continue;
        _snprintf(values[count], LYRA_CHANNEL_VALUE_MAX, "%s", value);
        values[count][LYRA_CHANNEL_VALUE_MAX - 1] = 0;
        lyra_channel_stage_row(CAST_TOGGLE_KEY, count, name, sub, values[count]);
        count++;
    }

    lyra_channel_commit(CAST_TOGGLE_KEY, count);
}

/* Parse a "ip" or "ip:port" token into out_ip / *out_port (CH_DEFAULT_PORT when
 * no ":port"). Returns 1 if a non-empty ip resulted. */
static int parse_ipport(const char* token, char* out_ip, int out_ip_sz, unsigned short* out_port)
{
    char  buf[LYRA_CHANNEL_VALUE_MAX];
    char* colon;
    int   i;
    if (!token || !token[0]) return 0;
    for (i = 0; i < LYRA_CHANNEL_VALUE_MAX - 1 && token[i]; i++) buf[i] = token[i];
    buf[i] = 0;
    if (out_port) *out_port = CH_DEFAULT_PORT;
    colon = strchr(buf, ':');
    if (colon) {
        int p; *colon = 0; p = atoi(colon + 1);
        if (out_port && p > 0 && p < 65536) *out_port = (unsigned short)p;
    }
    _snprintf(out_ip, out_ip_sz, "%s", buf);
    out_ip[out_ip_sz - 1] = 0;
    return out_ip[0] ? 1 : 0;
}

int cast_channel_get_selection(char* out_ip, int out_ip_sz, unsigned short* out_port)
{
    char token[LYRA_CHANNEL_VALUE_MAX];
    if (!lyra_channel_get_selection(CAST_TOGGLE_KEY, token, sizeof(token))) return 0;
    return parse_ipport(token, out_ip, out_ip_sz, out_port);
}

int cast_channel_name_for_target(const char* ip, unsigned short port,
                                 wchar_t* out_name, int out_name_len)
{
    char want[LYRA_CHANNEL_VALUE_MAX];
    int  n, i, k;
    if (!out_name || out_name_len <= 0) return 0;
    _snprintf(want, sizeof(want), "%s:%u", ip, port);
    want[LYRA_CHANNEL_VALUE_MAX - 1] = 0;
    n = lyra_channel_row_count(CAST_TOGGLE_KEY);
    for (i = 0; i < n; i++) {
        wchar_t name[LYRA_CHANNEL_NAME_MAX];
        char    value[LYRA_CHANNEL_VALUE_MAX];
        if (!lyra_channel_get_row(CAST_TOGGLE_KEY, i, name, LYRA_CHANNEL_NAME_MAX, NULL, 0,
                                  value, LYRA_CHANNEL_VALUE_MAX)) continue;
        if (strcmp(value, want) != 0) continue;
        for (k = 0; k < out_name_len - 1 && name[k]; k++) out_name[k] = name[k];
        out_name[k] = 0;
        return out_name[0] ? 1 : 0;
    }
    return 0;
}

void cast_channel_set_selection(const char* ip, unsigned short port)
{
    char val[LYRA_CHANNEL_VALUE_MAX];
    if (!ip) return;
    _snprintf(val, sizeof(val), "%s:%u", ip, port);
    val[LYRA_CHANNEL_VALUE_MAX - 1] = 0;
    lyra_channel_set_selection(CAST_TOGGLE_KEY, val);
}
