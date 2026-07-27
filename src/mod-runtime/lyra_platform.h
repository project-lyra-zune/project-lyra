#ifndef LYRA_PLATFORM_H
#define LYRA_PLATFORM_H

/* The firmware autoloads this at boot and its presence is the installed switch, so
   an unpack must write it last and a wipe must remove it last. */
#define LYRA_LOADER_NAME    "zuxhook.dll"
#define LYRA_LOADER_NAME_W  L"zuxhook.dll"

#define LYRA_INSTALL_DEFER_LAST        { LYRA_LOADER_NAME }
#define LYRA_INSTALL_DEFER_LAST_COUNT  1

#endif /* LYRA_PLATFORM_H */
