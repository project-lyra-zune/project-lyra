/* repo_version.h: version comparison shared by every reposd client (the Browse UI
 * now, the Manage Updates tab after consolidation). Numeric dotted compare so
 * 0.10.0 sorts above 0.9.0 (plain strcmp would rank it below). Missing trailing
 * components count as 0, so "1.2" equals "1.2.0"; non-digit suffix on a component
 * (e.g. "-beta") is ignored. Header-only static so each client compiles the same
 * logic with no link coupling. */
#ifndef REPO_VERSION_H
#define REPO_VERSION_H

/* <0 if a is older than b, 0 if equal, >0 if a is newer. */
static int version_cmp(const char* a, const char* b) {
    while (*a || *b) {
        unsigned long na = 0, nb = 0;
        while (*a >= '0' && *a <= '9') { na = na * 10 + (unsigned long)(*a - '0'); a++; }
        while (*b >= '0' && *b <= '9') { nb = nb * 10 + (unsigned long)(*b - '0'); b++; }
        if (na != nb) return na < nb ? -1 : 1;
        while (*a && *a != '.') a++;   /* drop any non-digit tail of this component */
        while (*b && *b != '.') b++;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}

#endif /* REPO_VERSION_H */
