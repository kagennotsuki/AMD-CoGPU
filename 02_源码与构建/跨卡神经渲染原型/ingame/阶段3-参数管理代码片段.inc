// 阶段3 参数管理（已验证：单独插入时编译通过、无花括号问题）
// 用法：整段插入到 static void ProxyImGuiDraw( 之前
// 需要已存在：#include <cstdio> <cstring> <string> <vector>，以及 g_self
// 待办：三个按钮的插入位置 = 现有"恢复默认设置"按钮【之前】，
//       撤销触发建议做成函数 UpdateUndo()，在 ImGui::Render() 之前调用一次

// ===== 阶段3：参数快照 / 撤销栈 / ini 保存 =====
struct ParamSnap {
    float gain, limit, edge, sat, smooth, temporal, mvScale, mvConf, gateK;
    float transfer, colour, uiScale, varGain;
    int   debugView, mvScaleIdx, mvConfIdx, gateIdx;
    int   bypass, nrOn, useGpuResid;
};
static std::vector<ParamSnap> g_undo;
static ParamSnap g_prev;
static const size_t kUndoMax = 64;

static ParamSnap CaptureParams() {
    ParamSnap s{};
    s.gain = g_gain; s.limit = g_limit; s.edge = g_edge; s.sat = g_sat; s.smooth = g_smooth;
    s.temporal = g_temporalStatic; s.mvScale = g_mvScale; s.mvConf = g_mvConf; s.gateK = g_gateK;
    s.transfer = g_transfer; s.colour = g_colorStr; s.uiScale = g_uiScale; s.varGain = g_varGain;
    s.debugView = g_debugView; s.mvScaleIdx = g_mvScaleIdx; s.mvConfIdx = g_mvConfIdx; s.gateIdx = g_gateIdx;
    s.bypass = g_bypass ? 1 : 0; s.nrOn = g_nrOn ? 1 : 0; s.useGpuResid = g_useGpuResid ? 1 : 0;
    return s;
}
static void ApplyParams(const ParamSnap& s) {
    g_gain = s.gain; g_limit = s.limit; g_edge = s.edge; g_sat = s.sat; g_smooth = s.smooth;
    g_temporalStatic = s.temporal; g_mvScale = s.mvScale; g_mvConf = s.mvConf; g_gateK = s.gateK;
    g_transfer = s.transfer; g_colorStr = s.colour; g_uiScale = s.uiScale; g_varGain = s.varGain;
    g_debugView = s.debugView; g_mvScaleIdx = s.mvScaleIdx; g_mvConfIdx = s.mvConfIdx; g_gateIdx = s.gateIdx;
    g_bypass = s.bypass != 0; g_nrOn = s.nrOn != 0; g_useGpuResid = s.useGpuResid != 0;
    g_stateWarmup = 4;
}
static ParamSnap FactoryParams() {
    ParamSnap s{};
    s.gain = 0.66f; s.limit = 0.20f; s.edge = 0.6f; s.sat = 0.9f; s.smooth = 0.0f;
    s.temporal = 0.02f; s.mvScale = -0.00045f; s.mvConf = 1.0f; s.gateK = 40.0f;
    s.transfer = 1.0f; s.colour = 1.0f; s.uiScale = 1.0f; s.varGain = 0.0f;
    s.debugView = 0; s.mvScaleIdx = 5; s.mvConfIdx = 1; s.gateIdx = 4;
    s.bypass = 0; s.nrOn = 1; s.useGpuResid = 1;
    return s;
}
static void PushUndo(const ParamSnap& before) {
    if (!g_undo.empty() && memcmp(&g_undo.back(), &before, sizeof(ParamSnap)) == 0) return;
    g_undo.push_back(before);
    if (g_undo.size() > kUndoMax) g_undo.erase(g_undo.begin());
}
static std::string IniPath() {
    wchar_t m[MAX_PATH]{};
    GetModuleFileNameW((HMODULE)g_self, m, MAX_PATH);
    std::wstring w(m);
    size_t sl = w.find_last_of(L"\\/");
    if (sl != std::wstring::npos) w = w.substr(0, sl + 1);
    w += L"xess_proxy.ini";
    return std::string(w.begin(), w.end());   // 仅用于 fopen（ASCII 路径）
}
static void SaveIni() {
    ParamSnap s = CaptureParams();
    FILE* fp = nullptr;
    if (fopen_s(&fp, IniPath().c_str(), "w") != 0 || !fp) { Log("保存: 无法写 ini"); return; }
    fprintf(fp, "[version]\nformat=1\n[params]\n");
    fprintf(fp, "gain=%.6f\nlimit=%.6f\nedge=%.6f\nsat=%.6f\nsmooth=%.6f\n", s.gain, s.limit, s.edge, s.sat, s.smooth);
    fprintf(fp, "temporal=%.6f\nmvScale=%.6f\nmvConf=%.6f\ngateK=%.6f\n", s.temporal, s.mvScale, s.mvConf, s.gateK);
    fprintf(fp, "transfer=%.6f\ncolour=%.6f\nuiScale=%.6f\nvarGain=%.6f\n", s.transfer, s.colour, s.uiScale, s.varGain);
    fprintf(fp, "debugView=%d\nmvScaleIdx=%d\nmvConfIdx=%d\ngateIdx=%d\n", s.debugView, s.mvScaleIdx, s.mvConfIdx, s.gateIdx);
    fprintf(fp, "bypass=%d\nnrOn=%d\nuseGpuResid=%d\n", s.bypass, s.nrOn, s.useGpuResid);
    fclose(fp);
    Log("保存: 已写入 xess_proxy.ini");
}
static void LoadIni() {
    FILE* fp = nullptr;
    if (fopen_s(&fp, IniPath().c_str(), "r") != 0 || !fp) { Log("载入: 无 ini，使用默认"); g_prev = CaptureParams(); return; }
    ParamSnap s = FactoryParams();
    char k[64] = { 0 };
    float fv = 0; int iv = 0;
    while (fscanf_s(fp, "%63[^=]=%d\n", k, (unsigned)sizeof k, &iv) == 2) {
        if      (strcmp(k, "debugView") == 0) s.debugView = iv;
        else if (strcmp(k, "mvScaleIdx") == 0) s.mvScaleIdx = iv;
        else if (strcmp(k, "mvConfIdx") == 0) s.mvConfIdx = iv;
        else if (strcmp(k, "gateIdx") == 0) s.gateIdx = iv;
        else if (strcmp(k, "bypass") == 0) s.bypass = iv;
        else if (strcmp(k, "nrOn") == 0) s.nrOn = iv;
        else if (strcmp(k, "useGpuResid") == 0) s.useGpuResid = iv;
        else if (strcmp(k, "format") == 0) { /* 版本 */ }
    }
    rewind(fp);
    while (fscanf_s(fp, "%63[^=]=%f\n", k, (unsigned)sizeof k, &fv) == 2) {
        if      (strcmp(k, "gain") == 0) s.gain = fv;
        else if (strcmp(k, "limit") == 0) s.limit = fv;
        else if (strcmp(k, "edge") == 0) s.edge = fv;
        else if (strcmp(k, "sat") == 0) s.sat = fv;
        else if (strcmp(k, "smooth") == 0) s.smooth = fv;
        else if (strcmp(k, "temporal") == 0) s.temporal = fv;
        else if (strcmp(k, "mvScale") == 0) s.mvScale = fv;
        else if (strcmp(k, "mvConf") == 0) s.mvConf = fv;
        else if (strcmp(k, "gateK") == 0) s.gateK = fv;
        else if (strcmp(k, "transfer") == 0) s.transfer = fv;
        else if (strcmp(k, "colour") == 0) s.colour = fv;
        else if (strcmp(k, "uiScale") == 0) s.uiScale = fv;
        else if (strcmp(k, "varGain") == 0) s.varGain = fv;
    }
    fclose(fp);
    ApplyParams(s);
    g_prev = CaptureParams();
    Log("载入: 已读取 xess_proxy.ini");
}

