// lmxxf_ext_probe.cpp -- probe lmxxf extended exports (QueryContextCapabilities / SetRuntimeSettings)
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

static const wchar_t* kDll =
    L"E:\\SteamLibrary\\steamapps\\common\\RESIDENT EVIL 2  BIOHAZARD RE2\\LmxxfNrRuntime.dll";

static uint8_t g_out[1024];

// 各种参数形态的调用封装（纯 POD，避免 C2712）
static int32_t Call3(void* fn, void* a, void* b, uint32_t c, bool* crash) {
    __try { *crash = false; return ((int32_t(*)(void*, void*, uint32_t))fn)(a, b, c); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *crash = true; return -999; }
}
static int32_t Call2(void* fn, void* a, void* b, bool* crash) {
    __try { *crash = false; return ((int32_t(*)(void*, void*))fn)(a, b); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *crash = true; return -999; }
}
static int32_t Call1(void* fn, void* a, bool* crash) {
    __try { *crash = false; return ((int32_t(*)(void*))fn)(a); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *crash = true; return -999; }
}

static void Dump(const char* tag, uint32_t n) {
    printf("\n---- %s (%u bytes) ----\n", tag, n);
    for (uint32_t off = 0; off + 16 <= n; off += 16) {
        printf("%04X:", off);
        for (uint32_t k = 0; k < 16; k += 4) {
            uint32_t u; memcpy(&u, g_out + off + k, 4);
            float f;    memcpy(&f, &u, 4);
            printf(" [%08X f=%8.3f i=%d]", u, f, (int32_t)u);
        }
        printf("\n");
    }
    // ASCII 视图
    printf("  ascii: ");
    for (uint32_t i = 0; i < n && i < 256; ++i) {
        char c = (char)g_out[i];
        putchar((c >= 32 && c < 127) ? c : '.');
    }
    printf("\n");
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("=== lmxxf extended API probe ===\n");

    HMODULE m = LoadLibraryW(kDll);
    if (!m) { printf("LoadLibrary failed %lu\n", GetLastError()); return 1; }
    printf("loaded\n");

    const char* names[6] = {
        "LmxxfNrQueryContextCapabilities", "LmxxfNrSetRuntimeSettings", "LmxxfNrSetDirectConsumer",
        "LmxxfNrGetApi", "LmxxfNrSetStreamPriority", "LmxxfNrGetJobTimes"
    };
    void* fns[6] = {};
    for (int i = 0; i < 6; ++i) {
        fns[i] = (void*)GetProcAddress(m, names[i]);
        printf("  %-34s = %p\n", names[i], fns[i]);
    }

    bool crash = false;
    int32_t r;

    // QueryContextCapabilities 各种形态
    memset(g_out, 0, sizeof(g_out));
    r = Call1(fns[0], g_out, &crash);
    printf("\nQuery(out)                 -> %d%s\n", r, crash ? " CRASH" : "");
    if (r == 0) Dump("Query(out)", 128);

    memset(g_out, 0, sizeof(g_out));
    r = Call2(fns[0], g_out, (void*)(uintptr_t)sizeof(g_out), &crash);
    printf("\nQuery(out,size)            -> %d%s\n", r, crash ? " CRASH" : "");
    if (r == 0) Dump("Query(out,size)", 128);

    memset(g_out, 0, sizeof(g_out));
    r = Call3(fns[0], nullptr, g_out, (uint32_t)sizeof(g_out), &crash);
    printf("\nQuery(ctx=null,out,size)   -> %d%s\n", r, crash ? " CRASH" : "");
    if (r == 0) Dump("Query(null,out,size)", 128);

    memset(g_out, 0, sizeof(g_out));
    r = Call2(fns[0], nullptr, g_out, &crash);
    printf("\nQuery(ctx=null,out)        -> %d%s\n", r, crash ? " CRASH" : "");
    if (r == 0) Dump("Query(null,out)", 128);

    // SetRuntimeSettings：形态探测（不期望成功，只看返回码分布）
    memset(g_out, 0, sizeof(g_out));
    r = Call1(fns[1], g_out, &crash);
    printf("\nSetRuntimeSettings(out)             -> %d%s\n", r, crash ? " CRASH" : "");
    memset(g_out, 0, sizeof(g_out));
    r = Call2(fns[1], g_out, (void*)(uintptr_t)sizeof(g_out), &crash);
    printf("SetRuntimeSettings(out,size)        -> %d%s\n", r, crash ? " CRASH" : "");
    memset(g_out, 0, sizeof(g_out));
    r = Call3(fns[1], nullptr, g_out, (uint32_t)sizeof(g_out), &crash);
    printf("SetRuntimeSettings(null,out,size)   -> %d%s\n", r, crash ? " CRASH" : "");

    printf("\ndone\n");
    return 0;
}
