#include <windows.h>
#include <string.h>
#include <stdlib.h>

#include "cast_channel.h"

#define CH_DEFAULT_PORT 8009

HANDLE cast_channel_scan_event(void)          { return mod_channel_scan_event(); }
void   cast_channel_set_sublabel(const wchar_t* text) { mod_channel_set_sublabel(text); }

void cast_channel_publish(const MdnsDevice* devs, int n)
{
    ModListChannelRow merged[MODLISTCH_MAX_ROWS];
    ModListChannelRow existing[MODLISTCH_MAX_ROWS];
    int count = 0, ecount, i, k, j;
    if (n < 0) n = 0;
    if (n > MODLISTCH_MAX_ROWS) n = MODLISTCH_MAX_ROWS;
    /* Fresh scan results first (new data wins for a device present in both). */
    for (i = 0; i < n; i++) {
        for (k = 0; k < MODLISTCH_NAME_LEN - 1 && devs[i].name[k]; k++)
            merged[count].name[k] = (wchar_t)(unsigned char)devs[i].name[k];
        merged[count].name[k] = 0;
        merged[count].sub[0] = 0;
        _snprintf(merged[count].value, MODLISTCH_VAL_LEN, "%s:%u", devs[i].ip, devs[i].port);
        merged[count].value[MODLISTCH_VAL_LEN - 1] = 0;
        count++;
    }
    /* Carry over previously-known devices this scan missed. mDNS drops packets
     * (worse while a cast saturates WiFi), so replacing the list wholesale would
     * lose a device still on the LAN. Union by "ip:port". */
    ecount = mod_channel_get_rows(existing, MODLISTCH_MAX_ROWS);
    for (i = 0; i < ecount && count < MODLISTCH_MAX_ROWS; i++) {
        int dup = 0;
        if (!existing[i].value[0]) continue;
        for (j = 0; j < count; j++)
            if (strcmp(merged[j].value, existing[i].value) == 0) { dup = 1; break; }
        if (!dup) merged[count++] = existing[i];
    }
    mod_channel_publish(merged, count);
}

/* Parse a "ip" or "ip:port" token into out_ip / *out_port (CH_DEFAULT_PORT when
 * no ":port"). Returns 1 if a non-empty ip resulted. */
static int parse_ipport(const char* token, char* out_ip, int out_ip_sz, unsigned short* out_port)
{
    char  buf[MODLISTCH_VAL_LEN];
    char* colon;
    int   i;
    if (!token || !token[0]) return 0;
    for (i = 0; i < MODLISTCH_VAL_LEN - 1 && token[i]; i++) buf[i] = token[i];
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
    char token[MODLISTCH_VAL_LEN];
    if (!mod_channel_get_selection(token, sizeof(token))) return 0;
    return parse_ipport(token, out_ip, out_ip_sz, out_port);
}

int cast_channel_name_for_target(const char* ip, unsigned short port,
                                 wchar_t* out_name, int out_name_len)
{
    ModListChannelRow rows[MODLISTCH_MAX_ROWS];
    char want[MODLISTCH_VAL_LEN];
    int  n, i, k;
    if (!out_name || out_name_len <= 0) return 0;
    _snprintf(want, sizeof(want), "%s:%u", ip, port);
    want[MODLISTCH_VAL_LEN - 1] = 0;
    n = mod_channel_get_rows(rows, MODLISTCH_MAX_ROWS);
    for (i = 0; i < n; i++) {
        if (strcmp(rows[i].value, want) == 0) {
            for (k = 0; k < out_name_len - 1 && rows[i].name[k]; k++) out_name[k] = rows[i].name[k];
            out_name[k] = 0;
            return out_name[0] ? 1 : 0;
        }
    }
    return 0;
}

void cast_channel_set_selection(const char* ip, unsigned short port)
{
    char val[MODLISTCH_VAL_LEN];
    if (!ip) return;
    _snprintf(val, sizeof(val), "%s:%u", ip, port);
    val[MODLISTCH_VAL_LEN - 1] = 0;
    mod_channel_set_selection(val);
}
