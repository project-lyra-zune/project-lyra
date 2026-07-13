#pragma once

#include "nativeapp_common.h"

// Append a tagged breadcrumb to \flash2\zpod-wk.log. Survives device
// blackouts (USB-active WiFi loss) for read-after-reboot diagnosis.
void zlog(const char* tag, DWORD a, DWORD b, DWORD c);
