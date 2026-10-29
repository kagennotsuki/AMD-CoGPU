// 暴力扫描 LmxxfNrGetApi 接受的 struct_size（abi 固定 1）
#include <windows.h>
#include <cstdio>
#include <cstdint>

int main() {
    const wchar_t* dll =
        L"E:\\SteamLibrary\\steamapps\\common\\RESIDENT EVIL 2  BIOHAZARD RE2\\LmxxfNrRuntime.dll";
    HMODULE mod = LoadLibraryW(dll);
    if (!mod) { std::printf("load failed\n"); return 1; }

    using GetApiFn = int32_t(*)(uint32_t, void*);
    auto getApi = (GetApiFn)GetProcAddress(mod, "LmxxfNrGetApi");
    if (!getApi) { std::printf("export missing\n"); return 1; }

    alignas(16) unsigned char buf[1024];
    int hits = 0;
    for (unsigned abi = 1; abi <= 2; ++abi) {
        for (unsigned s = 0; s <= 320; s += 8) {
            for (auto& b : buf) b = 0xCC;
            *(uint32_t*)buf = s;
            int32_t r = getApi(abi, buf);
            if (r != 2) {
                std::printf("  *** abi=%u size=%u -> %d  words=%08X %08X %08X %08X\n",
                            abi, s, r, *(uint32_t*)(buf), *(uint32_t*)(buf + 4),
                            *(uint32_t*)(buf + 8), *(uint32_t*)(buf + 12));
                hits++;
            }
        }
    }
    std::printf("non-INVALID_ARGUMENT combos: %d\n", hits);
    if (hits == 0) std::printf("(所有组合都是 INVALID_ARGUMENT)\n");
    FreeLibrary(mod);
    return 0;
}
