// mochi_controls.cpp —— 探测 mochizuki 运行时的"控制块"（风格预设等）
// 目的：找到 NRStyle / NRIntensity / NRLocalTone / NRLocalStructure / NRSkinStructure / NRAutoMask 的偏移
// 已知默认值可用于定位：NRStyle=0, NRIntensity=1.0, NRLocalTone=1.0, NRLocalStructure=1.0, NRSkinStructure=-1.0, NRAutoMask=0
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

static const wchar_t* kDll = L"E:\\AI\\mochi_bare\\MochizukiNrRuntime.dll";

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("=== mochizuki 控制块探测 ===\n");

    HMODULE m = LoadLibraryW(kDll);
    if (!m) { printf("LoadLibrary 失败 %lu\n", GetLastError()); return 1; }
    printf("DLL 已载入\n");

    using GetDefaultsFn = int32_t(*)(void*, uint32_t);
    using GetDefaultsFn1 = int32_t(*)(void*);
    using GetFeaturesFn = int32_t(*)(void*, uint32_t);
    using GetInfoFn = int32_t(*)(void*, uint32_t);

    auto getDef2 = (GetDefaultsFn)GetProcAddress(m, "MochizukiNrGetControlDefaults");
    auto getFeat = (GetFeaturesFn)GetProcAddress(m, "MochizukiNrGetFeatures");
    auto getInfo = (GetInfoFn)GetProcAddress(m, "MochizukiNrGetInfo");
    auto setCtl = GetProcAddress(m, "MochizukiNrSetControls");
    printf("GetControlDefaults=%p GetFeatures=%p GetInfo=%p SetControls=%p\n",
        (void*)getDef2, (void*)getFeat, (void*)getInfo, (void*)setCtl);

    static uint8_t buf[4096];
    auto dump = [&](const char* tag, size_t n) {
        printf("\n---- %s 返回块（前 %zu 字节）----\n", tag, n);
        for (size_t off = 0; off + 4 <= n; off += 16) {
            printf("%04zX:", off);
            for (size_t k = 0; k < 16 && off + k + 4 <= n; k += 4) {
                uint32_t u; memcpy(&u, buf + off + k, 4);
                float f;    memcpy(&f, &u, 4);
                int32_t i = (int32_t)u;
                printf("  [%08X f=%9.4f i=%d]", u, f, i);
            }
            printf("\n");
        }
    };

    // 形态 A：GetControlDefaults(out, size)
    memset(buf, 0, sizeof(buf));
    int32_t r = -999; bool crash = false;
    __try { r = getDef2 ? getDef2(buf, (uint32_t)sizeof(buf)) : -1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { crash = true; }
    printf("\nGetControlDefaults(out,size=4096) -> %d%s\n", r, crash ? " [崩溃]" : "");
    bool any = false;
    for (size_t i = 0; i < 256; ++i) if (buf[i]) { any = true; break; }
    if (r == 0 && any) { dump("A(out,size)", 256); return 0; }

    // 形态 B：GetControlDefaults(out)
    memset(buf, 0, sizeof(buf));
    r = -999; crash = false;
    __try { r = ((GetDefaultsFn1)getDef2) ? ((GetDefaultsFn1)getDef2)(buf) : -1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { crash = true; }
    printf("\nGetControlDefaults(out) -> %d%s\n", r, crash ? " [崩溃]" : "");
    any = false;
    for (size_t i = 0; i < 256; ++i) if (buf[i]) { any = true; break; }
    if (r == 0 && any) { dump("B(out)", 256); return 0; }

    // 附带：GetFeatures / GetInfo 也探一下，可能有结构体大小线索
    memset(buf, 0, sizeof(buf));
    r = -999; crash = false;
    __try { r = getFeat ? getFeat(buf, (uint32_t)sizeof(buf)) : -1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { crash = true; }
    printf("\nGetFeatures(out,size) -> %d%s\n", r, crash ? " [崩溃]" : "");
    if (r == 0) dump("Features", 128);

    memset(buf, 0, sizeof(buf));
    r = -999; crash = false;
    __try { r = getInfo ? getInfo(buf, (uint32_t)sizeof(buf)) : -1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { crash = true; }
    printf("\nGetInfo(out,size) -> %d%s\n", r, crash ? " [崩溃]" : "");
    if (r == 0) dump("Info", 128);

    printf("\n两种形态都没拿到控制块：说明它需要会话上下文（ctx），必须先 Create 之后才能取\n");
    return 0;
}
