// 探测 LmxxfNrGetApi 接受的 (abi_version, struct_size) 组合
#include <windows.h>
#include <cstdio>
#include <cstdint>

int main() {
    const wchar_t* dll =
        L"E:\\SteamLibrary\\steamapps\\common\\RESIDENT EVIL 2  BIOHAZARD RE2\\LmxxfNrRuntime.dll";
    HMODULE mod = LoadLibraryW(dll);
    if (!mod) { std::printf("LoadLibrary failed %lu\n", GetLastError()); return 1; }
    std::printf("loaded\n");

    using GetApiFn = int32_t(*)(uint32_t, void*);
    auto getApi = (GetApiFn)GetProcAddress(mod, "LmxxfNrGetApi");
    if (!getApi) { std::printf("export missing\n"); return 1; }

    unsigned sizes[] = { 0u, 16u, 128u, 136u, 144u, 152u, 256u };
    unsigned abis[]  = { 0u, 1u, 2u };

    // 用一块大缓冲当 out，先填 0xCC 便于观察写入位置
    alignas(16) unsigned char buf[512];

    for (unsigned a : abis) {
        for (unsigned s : sizes) {
            for (auto& b : buf) b = 0xCC;
            *(uint32_t*)buf = s;          // out->struct_size
            int32_t r = getApi(a, buf);
            std::printf("  abi=%u struct_size=%3u -> %d   (out words: %08X %08X)\n",
                        a, s, r, *(uint32_t*)(buf + 0), *(uint32_t*)(buf + 4));
        }
    }
    FreeLibrary(mod);
    return 0;
}
