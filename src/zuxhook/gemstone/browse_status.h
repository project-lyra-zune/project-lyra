/* browse_status.h - the sentence an empty Browse list shows.

   Free of windows.h and of engine addresses, so it stays host-testable
   (tests/test_browse_status.c). The scene gathers the facts; this decides. */
#ifndef BROWSE_STATUS_H
#define BROWSE_STATUS_H

#include <stddef.h>

#include "repo_ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Everything the status line is decided from, as the scene observed it. */
typedef struct {
    int  rows_in_category;   /* rows the list would draw for the open tab */
    int  ipc_mapped;         /* the shared section resolved */
    int  daemon_present;     /* reposd has attached to it at least once */
    int  timed_out;          /* a feed request went unanswered past its deadline */
    int  fetched;            /* a feed answer has landed since Browse was opened */
    long feed_error;         /* REPO_FEED_*, from that answer */
    long feed_detail;        /* the number worth quoting alongside it */
    long feed_rows;          /* rows the whole feed carried, across every tab */
} BrowseStatus;

/* Waiting is its own state, not a message: it is drawn by the stock LibraryLoadingLabel
   element, which animates its own ellipsis. */
enum {
    BROWSE_UI_ROWS = 0,   /* the list has rows of its own; show it */
    BROWSE_UI_WAITING,    /* the fetch is still out; show the animated indicator */
    BROWSE_UI_MESSAGE     /* buf holds the line to show instead */
};

/* Returns one of BROWSE_UI_*, filling buf for BROWSE_UI_MESSAGE and emptying it
   otherwise. cap is in wchar_t; the result is always terminated. */
int BrowseStatusFor(const BrowseStatus* s, wchar_t* buf, int cap);

#ifdef __cplusplus
}
#endif
#endif /* BROWSE_STATUS_H */
