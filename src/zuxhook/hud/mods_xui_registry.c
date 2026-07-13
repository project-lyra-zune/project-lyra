#include "mods_xui_registry.h"

#include <windows.h>
#include <string.h>

static int walk_registry_raw(const wchar_t* find, int* out_total) {
    int bucket;
    int total = 0;
    int found = 0;
    for (bucket = 0; bucket < XUIDLL_REGISTRY_BUCKETS; bucket++) {
        DWORD head = *(volatile DWORD*)(XUIDLL_REGISTRY_BASE + bucket * 4);
        DWORD cur = head;
        int safety = 200;
        while (cur && safety-- > 0) {
            RegistryEntry* e = (RegistryEntry*)cur;
            total++;
            if (!found && find && e->name_ptr) {
                if (_wcsicmp((const wchar_t*)e->name_ptr, find) == 0)
                    found = 1;
            }
            cur = e->next;
        }
    }
    if (out_total) *out_total = total;
    return found;
}

int walk_registry(const wchar_t* find, int* out_total) {
    int result = -1;
    if (out_total) *out_total = 0;
    __try {
        result = walk_registry_raw(find, out_total);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return result;
}
