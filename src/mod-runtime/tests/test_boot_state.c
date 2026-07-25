/* Host tests for the boot ladder and the marker codec.

   boot_state.c and mod_fault.c are deliberately free of windows.h so this runs
   on the build machine with no device: everything here is pure logic that the
   failsafe's correctness rests on, and getting it wrong is not visible until a
   device is already in trouble. The flash I/O halves (boot_state_file.c,
   mod_fault_file.c) are not covered and still need the device. */

#include <stdio.h>
#include <string.h>

#include "boot_state.h"
#include "mod_fault.h"

static int failures = 0;

#define CHECK(c) \
    do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void test_ladder(void) {
    /* NORMAL spends two attempts, SAFE one, BARE is the floor. */
    CHECK(BootLevelDemote(BOOT_LEVEL_NORMAL, 0) == BOOT_LEVEL_NORMAL);
    CHECK(BootLevelDemote(BOOT_LEVEL_NORMAL, 1) == BOOT_LEVEL_NORMAL);
    CHECK(BootLevelDemote(BOOT_LEVEL_NORMAL, 2) == BOOT_LEVEL_SAFE);
    CHECK(BootLevelDemote(BOOT_LEVEL_NORMAL, 9) == BOOT_LEVEL_SAFE);
    CHECK(BootLevelDemote(BOOT_LEVEL_SAFE,   0) == BOOT_LEVEL_SAFE);
    CHECK(BootLevelDemote(BOOT_LEVEL_SAFE,   1) == BOOT_LEVEL_BARE);
    CHECK(BootLevelDemote(BOOT_LEVEL_BARE,   0) == BOOT_LEVEL_BARE);
    CHECK(BootLevelDemote(BOOT_LEVEL_BARE,   9) == BOOT_LEVEL_BARE);

    /* The ladder only ever descends one rung per boot. */
    CHECK(BootLevelDemote(BOOT_LEVEL_NORMAL, 100) != BOOT_LEVEL_BARE);
}

static void test_marker_round_trip(void) {
    BootState a, b;
    char buf[BOOT_STATE_BUF_BYTES];
    int n;

    memset(&a, 0, sizeof(a));
    a.level = BOOT_LEVEL_SAFE;
    a.failures = 2;
    a.shell_started = 1;

    n = BootStateFormat(&a, buf, sizeof(buf));
    CHECK(n > 0);
    BootStateParse(buf, (size_t)n, &b);
    CHECK(b.level == BOOT_LEVEL_SAFE);
    CHECK(b.failures == 2);
    CHECK(b.shell_started == 1);
}

static void test_unreadable_marker_is_safe(void) {
    BootState st;
    const char* wrong_version = "{\"version\":2,\"level\":\"normal\",\"failures\":0}";
    const char* unknown_level = "{\"version\":1,\"level\":\"turbo\",\"failures\":0}";
    const char* negative      = "{\"version\":1,\"level\":\"normal\",\"failures\":-3}";
    const char* truncated     = "{\"version\":1,\"level\":\"norm";

    /* An unknown history must never resolve to a full apply. Each of these is a
       way the marker can be wrong; all of them have to land on SAFE. */
    BootStateParse("", 0, &st);                              CHECK(st.level == BOOT_LEVEL_SAFE);
    BootStateParse("not json at all", 15, &st);              CHECK(st.level == BOOT_LEVEL_SAFE);
    BootStateParse(truncated, strlen(truncated), &st);       CHECK(st.level == BOOT_LEVEL_SAFE);
    BootStateParse(wrong_version, strlen(wrong_version), &st); CHECK(st.level == BOOT_LEVEL_SAFE);
    BootStateParse(unknown_level, strlen(unknown_level), &st); CHECK(st.level == BOOT_LEVEL_SAFE);
    BootStateParse(negative, strlen(negative), &st);         CHECK(st.level == BOOT_LEVEL_SAFE);
}

static void test_marker_defaults(void) {
    BootState st;
    const char* minimal = "{\"version\":1,\"level\":\"normal\",\"failures\":0}";

    BootStateParse(minimal, strlen(minimal), &st);
    CHECK(st.level == BOOT_LEVEL_NORMAL);
    CHECK(st.failures == 0);
    CHECK(st.shell_started == 0);   /* absent means no shell has claimed the boot */
}

static void test_fault_round_trip(void) {
    ModFault a, b;
    char buf[512];
    int n;

    memset(&a, 0, sizeof(a));
    a.phase = 2;
    a.action = 3;
    a.code = 0xC0000005UL;   /* does not fit a signed int; must survive as hex */
    a.disabled = 1;
    strcpy(a.cap, "lyra.load_module");
    strcpy(a.version, "0.1.7");

    n = ModFaultFormat(&a, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK(ModFaultParse(buf, (size_t)n, "0.1.7", &b) == 1);
    CHECK(b.phase == 2);
    CHECK(b.action == 3);
    CHECK(b.code == 0xC0000005UL);
    CHECK(b.disabled == 1);
    CHECK(b.reported == 0);
    CHECK(strcmp(b.cap, "lyra.load_module") == 0);
}

static void test_fault_is_scoped_to_its_version(void) {
    ModFault a, b;
    char buf[512];
    int n;

    memset(&a, 0, sizeof(a));
    a.phase = 1;
    a.code = 0x80000003UL;
    strcpy(a.cap, "lyra.patch_bytes");
    strcpy(a.version, "1.0.0");
    n = ModFaultFormat(&a, buf, sizeof(buf));
    CHECK(n > 0);

    /* An updated mod has not earned the old fault, so the installer needs no
       explicit clear; a version mismatch simply reads as no fault. */
    CHECK(ModFaultParse(buf, (size_t)n, "1.0.0", &b) == 1);
    CHECK(ModFaultParse(buf, (size_t)n, "1.0.1", &b) == 0);
    CHECK(ModFaultParse(buf, (size_t)n, "0.9.9", &b) == 0);
    CHECK(ModFaultParse(buf, (size_t)n, NULL,    &b) == 1);   /* caller opted out */
}

static void test_fault_junk_is_no_fault(void) {
    ModFault f;
    CHECK(ModFaultParse("", 0, "1.0.0", &f) == 0);
    CHECK(ModFaultParse("{}", 2, "1.0.0", &f) == 0);
    CHECK(ModFaultParse("{\"version\":1,\"phase\":2}", 22, "1.0.0", &f) == 0);
}

int main(void) {
    test_ladder();
    test_marker_round_trip();
    test_unreadable_marker_is_safe();
    test_marker_defaults();
    test_fault_round_trip();
    test_fault_is_scoped_to_its_version();
    test_fault_junk_is_no_fault();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
