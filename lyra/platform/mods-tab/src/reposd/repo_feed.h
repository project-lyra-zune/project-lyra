/* repo_feed.h: parse feed.json (the shared ModsJson tokenizer) into RepoRow[]. Shape is
 * fixed by build_feed.py. */
#ifndef REPO_FEED_H
#define REPO_FEED_H

#include "repo_ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Returns rows filled; *truncated = 1 if the feed held more than max. The platform
   (`lyra`) row's `provides` capabilities are extracted into plat_prov[0..*plat_prov_count),
   the advertised set the install gate consults. */
int repo_parse_feed(const char *body, RepoRow *rows, int max, int *truncated,
                    char plat_prov[][REPO_CAP_LEN], int plat_prov_max, int *plat_prov_count);

#ifdef __cplusplus
}
#endif

#endif /* REPO_FEED_H */
