/* repo_client.h - the reposd shared-section client, hosted in zuxhook (gemstone).
 *
 * Absorbed from browse.dll so one binary owns all mod-management UI. reposd.exe
 * stays the separate backend daemon; this is only the UI-side IPC: map the section,
 * fire requests, and deliver DONE on the gemstone UI/pump thread via the MsgWait IAT
 * hook. Scene-agnostic: the scene layer registers a DONE callback rather than this
 * module reaching into any scene. */
#ifndef REPO_CLIENT_H
#define REPO_CLIENT_H

#include "repo_ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Map the section, create the wake/done events, and patch the gemstone MsgWait IAT.
   Call once from the gemstone branch of ZUxHookInit. */
void RepoClientInstall(void);

/* The shared block (daemon writes rows/status, UI reads). NULL if the map failed. */
RepoBlock* RepoClientBlock(void);

/* Fire-and-forget requests: bump req_seq and wake the daemon. */
void RepoClientRequestFeed(void);
void RepoClientRequestInstall(const char* id);
/* Install an ordered set (dependencies first, target last) in one request. */
void RepoClientRequestInstallSet(const char (*ids)[REPO_ID_LEN], int n);
void RepoClientRequestUninstall(const char* id);

/* The scene layer's DONE handler; MsgWait_proxy calls it on the UI thread when
   reposd signals completion (feed answered / install progressed). */
void RepoClientSetOnDone(void (*cb)(void));

#ifdef __cplusplus
}
#endif
#endif /* REPO_CLIENT_H */
