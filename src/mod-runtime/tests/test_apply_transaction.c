/* Host tests for the per-mod apply transaction.

   Both pieces only run when a mod fails, so normal use never exercises them and
   a regression would stay invisible until the boot it matters on. That is the
   reason they were split free of windows.h. */

#include <stdio.h>
#include <string.h>

#include "mods_compose_state.h"
#include "failed_set.h"

static int failures = 0;

#define CHECK(c) \
    do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static ModsXuizEntry make_entry(const unsigned char* data, int len) {
    ModsXuizEntry e;
    memset(&e, 0, sizeof(e));
    e.data = data;
    e.data_len = len;
    return e;
}

/* A mod's edits are discarded, and edits made before it survive. */
static void test_rollback_restores_entries(void) {
    ModsArena arena;
    ComposeState st;
    ComposeCheckpoint cp;
    GemComp gem;
    GemComp* gems[1];
    ModsXuizEntry entries[3];
    static const unsigned char original[] = "original";
    static const unsigned char earlier[]  = "earlier-mod";
    static const unsigned char mine[]     = "this-mod";

    CHECK(ModsArenaInit(&arena, 64 * 1024) == 0);

    memset(&gem, 0, sizeof(gem));
    entries[0] = make_entry(original, 8);
    entries[1] = make_entry(earlier, 11);   /* an earlier mod already changed this */
    gem.xuiz.entries = entries;
    gem.xuiz.count = 2;
    gem.xuiz.cap = 3;
    gem.modified = 1;
    gems[0] = &gem;
    st.gems = gems; st.count = 1; st.cap = 1;

    CHECK(ModsComposeCheckpoint(&st, &arena, &cp) == 0);
    CHECK(cp.valid == 1);

    /* This mod substitutes one entry and appends another, then fails. */
    entries[1] = make_entry(mine, 8);
    entries[2] = make_entry(mine, 8);
    gem.xuiz.count = 3;

    ModsComposeRollback(&st, &cp);

    CHECK(gem.xuiz.count == 2);                       /* the append is gone */
    CHECK(gem.xuiz.entries[0].data == original);      /* untouched entry intact */
    CHECK(gem.xuiz.entries[1].data == earlier);       /* the EARLIER mod's edit survives */
    CHECK(gem.modified == 1);
    ModsArenaFree(&arena);
}

/* A gem the failing mod caused to load is dropped entirely. */
static void test_rollback_drops_new_gem(void) {
    ModsArena arena;
    ComposeState st;
    ComposeCheckpoint cp;
    GemComp a, b;
    GemComp* gems[2];
    ModsXuizEntry ea[1], eb[1];
    static const unsigned char d[] = "x";

    CHECK(ModsArenaInit(&arena, 64 * 1024) == 0);
    memset(&a, 0, sizeof(a)); memset(&b, 0, sizeof(b));
    ea[0] = make_entry(d, 1); a.xuiz.entries = ea; a.xuiz.count = 1; a.xuiz.cap = 1;
    eb[0] = make_entry(d, 1); b.xuiz.entries = eb; b.xuiz.count = 1; b.xuiz.cap = 1;
    gems[0] = &a; gems[1] = &b;
    st.gems = gems; st.count = 1; st.cap = 2;      /* only gem a loaded so far */

    CHECK(ModsComposeCheckpoint(&st, &arena, &cp) == 0);
    st.count = 2;                                   /* the mod touched a second gem */
    ModsComposeRollback(&st, &cp);
    CHECK(st.count == 1);
    ModsArenaFree(&arena);
}

/* An unmodified composition rolls back to unmodified, not to "modified". */
static void test_rollback_preserves_clean_flag(void) {
    ModsArena arena;
    ComposeState st;
    ComposeCheckpoint cp;
    GemComp gem;
    GemComp* gems[1];
    ModsXuizEntry entries[1];
    static const unsigned char d[] = "x";

    CHECK(ModsArenaInit(&arena, 64 * 1024) == 0);
    memset(&gem, 0, sizeof(gem));
    entries[0] = make_entry(d, 1);
    gem.xuiz.entries = entries; gem.xuiz.count = 1; gem.xuiz.cap = 1;
    gem.modified = 0;
    gems[0] = &gem;
    st.gems = gems; st.count = 1; st.cap = 1;

    CHECK(ModsComposeCheckpoint(&st, &arena, &cp) == 0);
    gem.modified = 1;
    ModsComposeRollback(&st, &cp);
    CHECK(gem.modified == 0);   /* else the gem is flushed for no reason */
    ModsArenaFree(&arena);
}

/* Too many gems to snapshot must report failure, not a partial checkpoint that
   would roll back to a half-remembered state. */
static void test_checkpoint_refuses_when_too_many_gems(void) {
    ModsArena arena;
    ComposeState st;
    ComposeCheckpoint cp;
    GemComp* gems[COMPOSE_CKPT_MAX_GEMS + 1];

    CHECK(ModsArenaInit(&arena, 16 * 1024) == 0);
    memset(gems, 0, sizeof(gems));
    st.gems = gems; st.count = COMPOSE_CKPT_MAX_GEMS + 1; st.cap = st.count;
    CHECK(ModsComposeCheckpoint(&st, &arena, &cp) != 0);
    CHECK(cp.valid == 0);
    ModsComposeRollback(&st, &cp);          /* must be a no-op, not a crash */
    CHECK(st.count == COMPOSE_CKPT_MAX_GEMS + 1);
    ModsArenaFree(&arena);
}

static void test_failed_set_matches_whole_lines(void) {
    const char* set = "night-mode\nyoutube\n";

    CHECK(FailedSetBufContains(set, "night-mode") == 1);
    CHECK(FailedSetBufContains(set, "youtube") == 1);

    /* A prefix must not match, or Phase 2 would skip the wrong mod. */
    CHECK(FailedSetBufContains(set, "night") == 0);
    CHECK(FailedSetBufContains(set, "you") == 0);
    CHECK(FailedSetBufContains(set, "night-mode-extra") == 0);

    CHECK(FailedSetBufContains("", "night-mode") == 0);
    CHECK(FailedSetBufContains(set, "") == 0);
    CHECK(FailedSetBufContains(NULL, "night-mode") == 0);
    CHECK(FailedSetBufContains(set, NULL) == 0);

    /* A final line with no trailing newline still counts. */
    CHECK(FailedSetBufContains("a\nzune-cast", "zune-cast") == 1);
}

int main(void) {
    test_rollback_restores_entries();
    test_rollback_drops_new_gem();
    test_rollback_preserves_clean_flag();
    test_checkpoint_refuses_when_too_many_gems();
    test_failed_set_matches_whole_lines();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
