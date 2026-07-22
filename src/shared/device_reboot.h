#pragma once

// Force a device reset: arm the CE Power\State\Reboot registry state, then request
// POWER_STATE_RESET | POWER_FORCE. Does not return. Shared by nativeapp (install/uninstall
// splash) and zuxhook (mods-tab uninstall).
void RebootDevice(void);
