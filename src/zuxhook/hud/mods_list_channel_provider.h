#ifndef MODS_LIST_CHANNEL_PROVIDER_H
#define MODS_LIST_CHANNEL_PROVIDER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Register the generic context-picker provider for a setting key. This is the
   whole "hold a quick-toggle to open a picker" capability: any setting that
   declares "context" in its manifest gets one here, with no per-mod C. The
   provider's rows come from the setting's cross-process list channel (a daemon
   publishes them); opening the picker requests a scan; selecting a row writes the
   chosen token back to the channel and wakes the daemon. Called from
   apply_register_setting when a setting declares a context. Idempotent per key. */
void ModListChannelProviderRegister(const char* setting_key);

#ifdef __cplusplus
}
#endif

#endif /* MODS_LIST_CHANNEL_PROVIDER_H */
