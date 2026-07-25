#include <string.h>

#include "failed_set.h"

int FailedSetBufContains(const char* buf, const char* mod_id) {
    size_t idlen;
    const char* p;

    if (!buf || !mod_id || !mod_id[0]) return 0;
    idlen = strlen(mod_id);
    for (p = buf; *p; ) {
        const char* nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len == idlen && strncmp(p, mod_id, idlen) == 0) return 1;
        if (!nl) break;
        p = nl + 1;
    }
    return 0;
}
