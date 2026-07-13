#ifndef MODS_SETTINGS_H
#define MODS_SETTINGS_H

/* Persistence for setting values + HUD curation. ModStateBlock is pagefile-
   backed and resets every boot; this saves the live slot values to
   \flash2\automation\mods\mod-settings.json and restores them at boot, so a
   user's choices survive a reboot.

   Store shape (the only shape the reader understands, no migration):
       { "version": 3,
         "values":        { "owner/id": 0|1, ... },   // every setting's value
         "quick_toggles": [ "owner/id", ... ] }        // curated HUD list, ordered
   `values` is restored into ModStateBlock at boot. `quick_toggles` is the
   user's curated quick-menu set, seeded from the quick_toggle:"default"
   settings. A file lacking a `values` object is treated as absent → declared
   defaults stand. */

#ifdef __cplusplus
extern "C" {
#endif

/* Apply each saved slot from mod-settings.json to ModStateBlock. Call at
   Phase 2 AFTER register_setting has seeded the manifest defaults, so a saved
   value overrides the default. No-op if the file is absent (first boot,
   defaults stand). */
void ModSettingsLoad(void);

/* Write the current ModStateBlock value of every registered setting to
   mod-settings.json. Call after a menu flip persists the change. */
void ModSettingsSave(void);

#ifdef __cplusplus
}
#endif

#endif /* MODS_SETTINGS_H */
