// mochi_abi_scan.cpp -- scan ABI versions and table sizes (no device needed)
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

static const wchar_t* kDll = L"E:\\AI\\mochi_bare\\MochizukiNrRuntime.dll";

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("=== mochizuki ABI version/size scan ===\n");

    HMODULE m = LoadLibraryW(kDll);
    if (!m) { printf("LoadLibrary failed %lu\n", GetLastError()); return 1; }
    auto getApi = (int32_t(*)(uint32_t, void*))GetProcAddress(m, "LmxxfNrGetApi");
    if (!getApi) { printf("no LmxxfNrGetApi export\n"); return 1; }
    printf("LmxxfNrGetApi = %p\n", (void*)getApi);

    static uint8_t tbl[512];
    for (uint32_t ver = 0; ver <= 4; ++ver) {
        printf("\n--- version %u ---\n", ver);
        for (uint32_t sz = 120; sz <= 256; sz += 8) {
            memset(tbl, 0, sizeof(tbl));
            memcpy(tbl, &sz, 4);          // struct_size 预填
            int32_t r = 0;
            __try { r = getApi(ver, tbl); }
            __except (EXCEPTION_EXECUTE_HANDLER) { r = -999; }
            if (r == 0) {
                uint32_t got = 0; memcpy(&got, tbl, 4);
                uint32_t abi = 0; memcpy(&abi, tbl + 4, 4);
                printf("  size=%3u -> OK  (reported struct_size=%u, abi_version=%u)\n", sz, got, abi);
            } else if (sz % 32 == 0) {
                printf("  size=%3u -> %d\n", sz, r);
            }
        }
    }

    printf("\n=== other exports ===\n");
    const char* names[5] = { "MochizukiNrGetInfo", "MochizukiNrGetFeatures",
                             "MochizukiNrGetControlDefaults", "MochizukiNrSetControls",
                             "MochizukiNrSetExposure" };
    for (int i = 0; i < 5; ++i)
        printf("  %-32s = %p\n", names[i], (void*)GetProcAddress(m, names[i]));
    printf("\ndone\n");
    return 0;
}
