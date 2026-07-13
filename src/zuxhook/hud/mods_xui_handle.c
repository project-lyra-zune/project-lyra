#include "mods_xui_handle.h"

void* XuiResolveObj(DWORD handle) {
    void* obj = NULL;
    __try {
        const DWORD table = 0x41873bb0u;
        DWORD cap  = *(volatile DWORD*)(table + 0x4a4);
        DWORD lo16 = handle & 0xffff;
        DWORD subtab, slot;
        if (lo16 >= cap) return NULL;
        subtab = *(volatile DWORD*)(table + ((lo16 >> 8) << 2) + 0x8c);
        slot   = subtab + ((lo16 & 0xff) << 3);
        if (*(volatile DWORD*)slot != (handle >> 16)) return NULL;
        obj = (void*)*(volatile DWORD*)(slot + 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) { obj = NULL; }
    return obj;
}

DWORD XuiResolveNode(DWORD handle) {
    DWORD node = 0;
    __try {
        DWORD rec = (DWORD)XuiResolveObj(handle);
        DWORD next, guard = 0;
        if (!rec) return 0;
        while ((next = *(volatile DWORD*)(rec + 4)) != 0 && next != rec && guard++ < 16)
            rec = next;
        node = *(volatile DWORD*)(rec + 0x18);
    } __except (EXCEPTION_EXECUTE_HANDLER) { node = 0; }
    return node;
}
