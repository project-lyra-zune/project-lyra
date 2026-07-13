/* gem_mod_detail.h - the unified mod-detail scene's cross-scene entry points.
 *
 * The detail is keyed by mod_id: a list sets the target id, then navigates to
 * ManageModDetail.xur. The detail looks the id up in both ModScan (local disk) and
 * the reposd feed and shows the actions the mod's actual state allows. */
#ifndef GEM_MOD_DETAIL_H
#define GEM_MOD_DETAIL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Set the mod the detail will show next. Called by a list on row-tap before
   navigating. Ascii for a feed row id, wide for a ModScan row id. */
void GemModDetailSetTargetA(const char* id, int from_browse);
void GemModDetailSetTargetW(const wchar_t* id, int from_browse);

/* reposd DONE handler: updates the active detail's status + buttons on install /
   uninstall progress. Registered with RepoClientSetOnDone from ZUxHookInit. */
void GemModDetailOnRepoDone(void);

#ifdef __cplusplus
}
#endif
#endif /* GEM_MOD_DETAIL_H */
