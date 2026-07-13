/* repo_feed.h: parse feed.json into RepoRow[] with forward-strstr on stable key
 * anchors (no JSON parser), the ce_innertube idiom. Shape is fixed by build_feed.py. */
#ifndef REPO_FEED_H
#define REPO_FEED_H

#include "repo_ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Returns rows filled; *truncated = 1 if the feed held more than max. */
int repo_parse_feed(const char *body, RepoRow *rows, int max, int *truncated);

#ifdef __cplusplus
}
#endif

#endif /* REPO_FEED_H */
