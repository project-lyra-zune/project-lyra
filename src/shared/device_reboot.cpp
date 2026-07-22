#include <windows.h>
#include "device_reboot.h"

// SetSystemPowerState lives in pm.h; redeclared so both build environments compile this
// shared source without the PM SDK header on their include path.
extern "C" DWORD SetSystemPowerState(LPCWSTR pwsSystemState, DWORD StateFlags, DWORD Options);

#ifndef POWER_STATE_RESET
#define POWER_STATE_RESET 0x00800000u
#endif
#ifndef POWER_FORCE
#define POWER_FORCE       0x00001000u
#endif

void RebootDevice(void) {
	HKEY key = NULL;
	if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Control\\Power\\State\\Reboot",
	                 0, 0, &key) == ERROR_SUCCESS) {
		DWORD flags = 0x800000, def = 4;
		RegSetValueEx(key, L"Flags", 0, REG_DWORD, (BYTE*)&flags, sizeof(DWORD));
		RegSetValueEx(key, L"Default", 0, REG_DWORD, (BYTE*)&def, sizeof(DWORD));
		RegFlushKey(key);
		RegCloseKey(key);
	}
	SetSystemPowerState(NULL, POWER_STATE_RESET, POWER_FORCE);
}
