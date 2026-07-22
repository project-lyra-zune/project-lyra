#pragma once

// Set the XNA title's Apps-list display name via the media service set-string path, then
// flush so it persists a forced reset. Resolves the live title handle at runtime; runs at
// install and uninstall to flip the tile between "Install Project Lyra" and "Uninstall
// Project Lyra". Plain user-mode, best-effort. Shared by nativeapp (XNA path) and zuxhook
// (mods-tab path).
void SetTitleName(const wchar_t* name);
