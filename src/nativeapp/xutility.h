/*  ______   ______   ______   ______   ______   __  __   _______    
 * /_____/\ /_____/\ /_____/\ /_____/\ /_____/\ /_/\/_/\ /______/\   
 * \   __\/ \   _ \ \\   _ \ \\    _\/_\   _ \ \\ \ \ \ \\   __\/__ 
 *  \ \ \  __\ \ \ \ \\ \ \ \ \\ \/___/\\ (_) \ \\ \ \ \ \\ \ /____/\
 *   \ \ \/_/\\ \ \ \ \\ \ \ \ \\  ___\/_\  ___\/ \ \ \ \ \\ \\_  _\/
 *    \ \_\ \ \\ \_\ \ \\ \/  | |\ \____/\\ \ \    \ \_\ \ \\ \_\ \ \
 *     \_____\/ \_____\/ \____/_/ \_____\/ \_\/     \_____\/ \_____\/
 *  
 * (paart of) xutility.h
 * Copyright (c) 2010 CodePug
 * All rights reserved.
 */


#ifndef _UTILITY_H				// If we haven't included this file
#define _UTILITY_H	


static inline void SuppressReboot()
{
	HKEY key = NULL;
	HRESULT hr = S_OK;
	DWORD value;

	if (SUCCEEDED(hr))
		hr = HRESULT_FROM_WIN32(RegOpenKeyEx(HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Control\\Power\\State\\Reboot", 0, 0, &key));
	if (SUCCEEDED(hr))
		hr = HRESULT_FROM_WIN32(RegSetValueEx(key, L"Flags", 0, REG_DWORD, (BYTE *)&(value = 0x10000), sizeof(DWORD)));
	if (SUCCEEDED(hr))
		hr = HRESULT_FROM_WIN32(RegSetValueEx(key, L"Default", 0, REG_DWORD, (BYTE *)&(value = 0), sizeof(DWORD)));
	if (key)
		RegCloseKey(key);
}
#endif