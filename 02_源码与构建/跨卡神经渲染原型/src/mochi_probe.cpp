// mochi_probe.cpp —— 验证 MochizukiNrRuntime.dll 是否与我们现有 lmxxf ABI 兼容
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

struct ApiTable { uint32_t struct_size; /* 其余字段我们不关心，只测 struct_size 校验 */ unsigned char pad[512]; };

static const char* kDll = "E:\\AI\\tmp\\mochi\\MochizukiNrRuntime.dll";

int main() {
    SetConsoleOutputCP(65001);
    setvbuf(stdout, nullptr, _IONBF, 0);   // 立即刷新，避免崩溃时丢输出
    printf("=== MochizukiNrRuntime ABI 探测 ===\n");

    printf("即将 LoadLibraryA...\n");
    HMODULE m = LoadLibraryA(kDll);
    if (!m) { printf("!! LoadLibrary 失败 %lu\n", GetLastError()); return 1; }
    printf("[ ok ] 已加载\n");

    // 1) 列导出
    for (const char* n : { "LmxxfNrGetApi", "MochizukiNrGetInfo", "MochizukiNrGetFeatures",
                           "MochizukiNrGetControlDefaults", "MochizukiNrSetControls", "MochizukiNrSetExposure" }) {
        printf("  导出 %-28s = %p\n", n, (void*)GetProcAddress(m, n));
    }

    // 2) 用不同 struct_size 调 LmxxfNrGetApi
    using GetApiFn = int32_t(*)(uint32_t, void*);
    auto getApi = (GetApiFn)GetProcAddress(m, "LmxxfNrGetApi");
    if (!getApi) { printf("!! 没有 LmxxfNrGetApi\n"); return 1; }

    for (uint32_t sz : { 136u, 144u, 152u, 160u, 168u, 192u }) {
        std::vector<unsigned char> buf(sz, 0);
        *(uint32_t*)buf.data() = sz;
        int32_t r = getApi(1, buf.data());
        printf("  GetApi(ver=1, struct_size=%3u) -> %d%s\n", sz, r, r == 0 ? "   ★ 接受" : "");
        if (r == 0) {
            uint32_t abi = 0, maxw = 0, maxh = 0, hist = 0;
            memcpy(&abi,  buf.data() + 4,  4);
            printf("      abi_version=%u\n", abi);
        }
    }

    // 3) GetInfo / GetFeatures（如果存在）
    using InfoFn = int32_t(*)(void*, uint32_t);
    auto info = (InfoFn)GetProcAddress(m, "MochizukiNrGetInfo");
    if (info) {
        char b[512] = { 0 };
        int32_t r = info(b, 512);
        printf("  GetInfo -> %d : %s\n", r, b[0] ? b : "(无文本)");
    }
    using FeatFn = const char*(*)();
    auto feat = (FeatFn)GetProcAddress(m, "MochizukiNrGetFeatures");
    if (feat) {
        const char* s = feat();
        printf("  GetFeatures -> %s\n", s ? s : "(null)");
    }
    return 0;
}
