#ifndef MODS_BOOT_NOTICE_H
#define MODS_BOOT_NOTICE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Announce anything Lyra decided on its own this boot: a demotion to safe mode,
   or mods disabled after a capability faulted. Shows once, hosted by any live
   gemstone scene; a no-op when there is nothing to say. */
void ModBootNoticeShow(void* host_scene);

#ifdef __cplusplus
}
#endif

#endif
