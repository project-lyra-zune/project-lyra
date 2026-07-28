/* browse_status.c - see browse_status.h.

   One flowing sentence per outcome: the label wraps on width and honours no line
   breaks. Composed by hand because C89 has no wide printf. */

#include "browse_status.h"

static void put(wchar_t* buf, int cap, int* n, const wchar_t* s) {
    while (*s && *n < cap - 1) buf[(*n)++] = *s++;
    buf[*n] = 0;
}

static void put_long(wchar_t* buf, int cap, int* n, long v) {
    wchar_t digits[16];
    int k = 0;
    unsigned long u;
    if (v < 0) {
        put(buf, cap, n, L"-");
        u = (unsigned long)(-(v + 1)) + 1UL;
    } else {
        u = (unsigned long)v;
    }
    do {
        digits[k++] = (wchar_t)(L'0' + (int)(u % 10UL));
        u /= 10UL;
    } while (u != 0UL && k < (int)(sizeof(digits) / sizeof(digits[0])));
    while (k > 0 && *n < cap - 1) buf[(*n)++] = digits[--k];
    buf[*n] = 0;
}

static int message(wchar_t* buf, int cap, const wchar_t* text) {
    int n = 0;
    put(buf, cap, &n, text);
    return BROWSE_UI_MESSAGE;
}

int BrowseStatusFor(const BrowseStatus* s, wchar_t* buf, int cap) {
    const wchar_t* reason;
    int n = 0;

    if (!s || !buf || cap < 2) return BROWSE_UI_ROWS;
    buf[0] = 0;
    if (s->rows_in_category > 0) return BROWSE_UI_ROWS;

    if (!s->ipc_mapped)
        return message(buf, cap,
            L"The mod service cannot be reached from here. Restart the Zune. (E-NOIPC)");

    if (!s->daemon_present)
        return message(buf, cap,
            L"The mod service is not running, so the repo cannot be reached. Restart "
            L"the Zune. If this keeps happening, install Lyra again from "
            L"install.zune.moe. (E-NODAEMON)");

    if (s->timed_out)
        return message(buf, cap,
            L"The mod service stopped responding. Restart the Zune, then open Browse "
            L"again. (E-TIMEOUT)");

    if (!s->fetched)
        return BROWSE_UI_WAITING;

    switch (s->feed_error) {
    case REPO_FEED_OK:
        return message(buf, cap, s->feed_rows > 0
            ? L"No mods in this category."
            : L"The repo listed no mods. (E-EMPTY)");
    case REPO_FEED_EMPTY:
        return message(buf, cap,
            L"The repo answered but listed no mods. (E-EMPTY)");
    case REPO_FEED_NO_NET:
        return message(buf, cap,
            L"No connection. Check the Zune is on Wi-Fi, then open Browse again. "
            L"(E-NET)");
    case REPO_FEED_CLOCK:
        return message(buf, cap,
            L"The Zune's clock is wrong, so the repo refused a secure connection. Set "
            L"the date and time under Settings, Clock, Set Date and Set Time, then "
            L"open Browse again. (E-CLOCK)");
    case REPO_FEED_CERT:
        reason = L"The repo's certificate was refused. (E-CERT "; break;
    case REPO_FEED_TLS:
        reason = L"The secure connection failed. (E-TLS "; break;
    case REPO_FEED_TRANSFER:
        reason = L"The connection broke before the list arrived. (E-XFER "; break;
    case REPO_FEED_HTTP:
        reason = L"The repo answered with an error. (E-HTTP "; break;
    default:
        reason = L"The repo could not be read. (E-FEED "; break;
    }

    put(buf, cap, &n, L"Could not load the mod list. ");
    put(buf, cap, &n, reason);
    put_long(buf, cap, &n, s->feed_detail);
    put(buf, cap, &n, L") Check the Zune's Wi-Fi and its date and time in Settings, "
                      L"then open Browse again.");
    return BROWSE_UI_MESSAGE;
}
