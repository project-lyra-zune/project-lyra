/* Host tests for the Browse status line.

   The two failures users actually hit, no Wi-Fi and a wrong clock, must reach the
   screen as those words and not as a spinner: nobody can report what they were
   never shown. */

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "browse_status.h"

static int failures = 0;

#define CHECK(c) \
    do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* A healthy fetch that returned a full catalog, for cases to spoil one field of. */
static BrowseStatus good(void) {
    BrowseStatus s;
    s.rows_in_category = 0;
    s.ipc_mapped = 1;
    s.daemon_present = 1;
    s.timed_out = 0;
    s.fetched = 1;
    s.feed_error = REPO_FEED_OK;
    s.feed_detail = 0;
    s.feed_rows = 7;
    return s;
}

static int state_of(const BrowseStatus* s) {
    wchar_t buf[512];
    return BrowseStatusFor(s, buf, 512);
}

static int says(const BrowseStatus* s, const wchar_t* needle) {
    wchar_t buf[512];
    if (BrowseStatusFor(s, buf, 512) != BROWSE_UI_MESSAGE) return 0;
    return wcsstr(buf, needle) != NULL;
}

static void test_rows_win(void) {
    /* A tab with rows has nothing to explain, whatever the last fetch did. */
    BrowseStatus s = good();
    s.rows_in_category = 3;
    CHECK(state_of(&s) == BROWSE_UI_ROWS);
    s.feed_error = REPO_FEED_NO_NET;
    CHECK(state_of(&s) == BROWSE_UI_ROWS);
}

static void test_waiting(void) {
    /* Waiting is its own state, never a message: the animated stock visual draws it,
       so returning "Loading..." as text would silently lose the animation. */
    BrowseStatus s = good();
    wchar_t buf[512];
    s.fetched = 0;
    CHECK(BrowseStatusFor(&s, buf, 512) == BROWSE_UI_WAITING);
    CHECK(buf[0] == 0);
}

static void test_remedies_users_can_act_on(void) {
    BrowseStatus s = good();

    s = good(); s.feed_error = REPO_FEED_NO_NET;
    CHECK(says(&s, L"No connection"));
    CHECK(says(&s, L"Wi-Fi"));
    CHECK(says(&s, L"(E-NET)"));

    s = good(); s.feed_error = REPO_FEED_CLOCK;
    CHECK(says(&s, L"clock is wrong"));
    CHECK(says(&s, L"Settings, Clock, Set Date and Set Time"));
    CHECK(says(&s, L"(E-CLOCK)"));

    /* A wrong clock must never be reported as a generic certificate problem: the
       remedy is the whole point of separating them. */
    CHECK(!says(&s, L"E-CERT"));
}

static void test_service_missing(void) {
    BrowseStatus s;

    s = good(); s.ipc_mapped = 0;
    CHECK(says(&s, L"(E-NOIPC)"));

    /* No daemon outranks a stale "nothing came back": nothing was ever going to. */
    s = good(); s.daemon_present = 0; s.fetched = 0;
    CHECK(says(&s, L"(E-NODAEMON)"));
    CHECK(state_of(&s) != BROWSE_UI_WAITING);

    s = good(); s.timed_out = 1; s.fetched = 0;
    CHECK(says(&s, L"(E-TIMEOUT)"));
    CHECK(state_of(&s) != BROWSE_UI_WAITING);
}

static void test_empty_is_not_an_error(void) {
    BrowseStatus s = good();

    /* The feed carried mods, this tab just holds none of them. */
    s.feed_rows = 7;
    CHECK(says(&s, L"No mods in this category"));
    CHECK(!says(&s, L"(E-"));

    /* The feed carried nothing at all, which is a fault worth a code. */
    s.feed_rows = 0;
    CHECK(says(&s, L"(E-EMPTY)"));

    s = good(); s.feed_error = REPO_FEED_EMPTY;
    CHECK(says(&s, L"(E-EMPTY)"));
}

static void test_codes_carry_their_number(void) {
    BrowseStatus s = good();

    s.feed_error = REPO_FEED_HTTP; s.feed_detail = 503;
    CHECK(says(&s, L"(E-HTTP 503)"));

    s.feed_error = REPO_FEED_CERT; s.feed_detail = -188;
    CHECK(says(&s, L"(E-CERT -188)"));

    s.feed_error = REPO_FEED_TLS; s.feed_detail = -313;
    CHECK(says(&s, L"(E-TLS -313)"));

    s.feed_error = REPO_FEED_TRANSFER; s.feed_detail = 0;
    CHECK(says(&s, L"(E-XFER 0)"));

    /* An outcome this build has no words for still reaches the user as something
       they can read out. */
    s.feed_error = 999; s.feed_detail = 42;
    CHECK(says(&s, L"(E-FEED 42)"));
}

static void test_never_overruns(void) {
    /* A caller with a short buffer gets a truncated, terminated line, not a smash. */
    BrowseStatus s = good();
    wchar_t small[24];
    size_t i;
    s.feed_error = REPO_FEED_HTTP;
    s.feed_detail = 404;
    memset(small, 0x7f, sizeof(small));
    CHECK(BrowseStatusFor(&s, small, 24) == BROWSE_UI_MESSAGE);
    CHECK(wcslen(small) < 24);

    /* Every cap from useless to ample terminates inside its own buffer. */
    for (i = 2; i < 64; i++) {
        wchar_t probe[64];
        BrowseStatusFor(&s, probe, (int)i);
        CHECK(wcslen(probe) < i);
    }
}

int main(void) {
    test_rows_win();
    test_waiting();
    test_remedies_users_can_act_on();
    test_service_missing();
    test_empty_is_not_an_error();
    test_codes_carry_their_number();
    test_never_overruns();

    if (failures) {
        printf("%d FAILURES\n", failures);
        return 1;
    }
    printf("browse status: every outcome reaches the user in words\n");
    return 0;
}
