/* repo_ipc.h shared-section IPC between the Browse UI (gemstone DLL) and
 * reposd.exe. Named CreateFileMappingW section + wake events, mirroring zune-yt's
 * search daemon. The UI writes a request and bumps req_seq; the daemon answers and
 * echoes done_seq. CE6 has no OpenFileMapping, so both sides CreateFileMappingW by
 * the same version-tagged name. */
#ifndef REPO_IPC_H
#define REPO_IPC_H

#include <stddef.h>   /* wchar_t */

#define REPO_SECTION_NAME  L"lyra-repo-v12"
#define REPO_WAKE_EVENT    L"lyra-repo-wake-v12"   /* UI -> daemon */
#define REPO_DONE_EVENT    L"lyra-repo-done-v12"   /* daemon -> UI */

/* The one platform is the reserved catalog id `lyra`; reposd routes it to the
   platform apply path. Everything else is a feature mod. */
#define LYRA_PLATFORM_ID   "lyra"

#define REPO_MAX_ROWS      64
#define REPO_MAX_INSTALL_SET 16   /* a target plus its transitive mod-dependency closure */
#define REPO_ID_LEN        64
#define REPO_VERSION_LEN   16
#define REPO_URL_LEN       256
#define REPO_SHA_LEN       65    /* sha256 hex (64) + NUL */
#define REPO_NAME_LEN      64
#define REPO_AUTHOR_LEN    48
#define REPO_CATEGORY_LEN  32
#define REPO_DESC_LEN      256   /* >= MAX_DESCRIPTION_LEN (255) + NUL */
#define REPO_CHANGELOG_LEN 512   /* >= MAX_CHANGELOG_LEN (511) + NUL */
#define REPO_MAX_USES      8     /* capabilities in a mod's footprint, per feed entry */
#define REPO_MAX_PROVIDES  8     /* capabilities a mod provides to other mods, per feed entry */
#define REPO_CAP_LEN       48    /* one capability string ("name", "name@r", "name@lo:hi") + NUL */
#define REPO_PLAT_PROV_MAX 48    /* platform advertised-capability ceiling (block-wide) */

enum {
    REPO_REQ_NONE      = 0,
    REPO_REQ_FEED      = 1,
    REPO_REQ_INSTALL   = 2,   /* installs install_set[0..count-1] in order; count 1 is a plain install */
    REPO_REQ_UNINSTALL = 3
};

/* Install progress; DONE/ERROR are terminal (done_seq set). */
enum {
    REPO_INSTALL_IDLE      = 0,
    REPO_INSTALL_FETCHING  = 1,
    REPO_INSTALL_VERIFYING = 2,
    REPO_INSTALL_UNPACKING = 3,
    REPO_INSTALL_ENABLING  = 4,
    REPO_INSTALL_DONE      = 5,
    REPO_INSTALL_ERROR     = 6
};

/* A pure catalog row: what the repo offers. Whether it is installed (and at what
   version) is disk truth the UI reads from the scanner by id, not carried here. */
typedef struct {
    char    id[REPO_ID_LEN];
    char    version[REPO_VERSION_LEN];
    char    url[REPO_URL_LEN];
    char    sha256[REPO_SHA_LEN];
    unsigned long size;
    long    experimental;                          /* author-declared: not finished */
    wchar_t name[REPO_NAME_LEN];
    wchar_t author[REPO_AUTHOR_LEN];
    wchar_t category[REPO_CATEGORY_LEN];
    wchar_t description[REPO_DESC_LEN];
    wchar_t changelog[REPO_CHANGELOG_LEN];         /* "what's new", shown on update */
    /* A feature mod's capability footprint (uses, points) and the capabilities it provides
       to other mods (ranges). The install gate checks the footprint against what this
       platform provides; provides lets the device resolve mod-to-mod dependencies from the
       feed. */
    char    uses_caps[REPO_MAX_USES][REPO_CAP_LEN];         /* each a point: name or name@r */
    long    uses_count;
    char    provides_caps[REPO_MAX_PROVIDES][REPO_CAP_LEN]; /* each a range: name or name@lo:hi */
    long    provides_count;
} RepoRow;

typedef struct {
    unsigned long version;
    volatile long req_seq;                /* UI bumps per request; daemon echoes into done_seq */
    volatile long done_seq;
    long          request;                /* REPO_REQ_* */
    long          status;                 /* feed: 0 or ce_https_result; install: 0 or error */
    long          count;
    long          truncated;              /* feed had more than REPO_MAX_ROWS */
    char          install_id[REPO_ID_LEN]; /* the member reposd is installing now (a set entry) */
    long          install_status;         /* REPO_INSTALL_* */
    unsigned long install_done;
    unsigned long install_total;
    /* The install plan for REPO_REQ_INSTALL: an ordered set (dependencies first, target last)
       the daemon installs one member at a time. install_set_index is the member in flight, so
       the UI can show "N of M"; a member's failure aborts the rest, leaving the device with
       whatever fully installed before it. */
    char          install_set[REPO_MAX_INSTALL_SET][REPO_ID_LEN];
    long          install_set_count;
    long          install_set_index;
    long          reboot_required;         /* set by a platform apply; UI prompts to restart */
    /* The platform authority: the `lyra` version and advertised capabilities from the pinned
       official channel (reposd REPO_HOST), the source of truth for the update remedy. Kept in
       dedicated fields, not looked up from `rows`, so the remedy stays correct when `rows` come
       from a feed a user subscribed to that carries no `lyra` entry. */
    char          plat_version[REPO_VERSION_LEN];
    char          plat_provides[REPO_PLAT_PROV_MAX][REPO_CAP_LEN];
    long          plat_provides_count;
    RepoRow       rows[REPO_MAX_ROWS];
} RepoBlock;

#define REPO_VERSION  12u

#endif /* REPO_IPC_H */
