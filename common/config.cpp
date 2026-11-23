#include "config.h"

#include <cwchar>
#include <vector>

namespace lsfgi {

namespace {

std::wstring ReadString(const std::wstring& ini, const wchar_t* key, const wchar_t* def) {
    std::vector<wchar_t> buf(4096);
    GetPrivateProfileStringW(kConfigSection, key, def, buf.data(),
                             static_cast<DWORD>(buf.size()), ini.c_str());
    return buf.data();
}

int ReadInt(const std::wstring& ini, const wchar_t* key, int def) {
    const std::wstring s = ReadString(ini, key, L"");
    if (s.empty()) return def;
    return static_cast<int>(std::wcstol(s.c_str(), nullptr, 0));
}

float ReadFloat(const std::wstring& ini, const wchar_t* key, float def) {
    const std::wstring s = ReadString(ini, key, L"");
    if (s.empty()) return def;
    return std::wcstof(s.c_str(), nullptr);
}

bool Write(const std::wstring& ini, const wchar_t* key, const std::wstring& value) {
    return WritePrivateProfileStringW(kConfigSection, key, value.c_str(), ini.c_str()) != FALSE;
}

} // namespace

int ClampMultiplier(int m) {
    return m < 2 ? 2 : (m > 4 ? 4 : m);
}

float ClampFlowScale(float f) {
    if (!(f >= 0.25f)) return 0.25f; // also catches NaN
    return f > 1.0f ? 1.0f : f;
}

void LoadConfig(const std::wstring& ini, Config& cfg) {
    cfg.shaders = ReadString(ini, L"Shaders", cfg.shaders.c_str());
    cfg.losslessDll = ReadString(ini, L"LosslessDll", cfg.losslessDll.c_str());
    cfg.multiplier = ClampMultiplier(ReadInt(ini, L"Multiplier", cfg.multiplier));
    cfg.flowScale = ClampFlowScale(ReadFloat(ini, L"FlowScale", cfg.flowScale));
    cfg.performanceMode = ReadInt(ini, L"PerformanceMode", cfg.performanceMode) != 0;
    cfg.dynamic = ReadInt(ini, L"Dynamic", cfg.dynamic) != 0;
    cfg.hdr = ReadInt(ini, L"HDR", cfg.hdr);
    cfg.vsyncPacing = ReadInt(ini, L"VSyncPacing", cfg.vsyncPacing) != 0;
    cfg.enabled = ReadInt(ini, L"EnabledAtStart", cfg.enabled) != 0;
    cfg.toggleKey = ReadInt(ini, L"ToggleKey", cfg.toggleKey);
    cfg.cycleKey = ReadInt(ini, L"CycleKey", cfg.cycleKey);
    cfg.menuKey = ReadInt(ini, L"MenuKey", cfg.menuKey);
    cfg.shaderOffset = ReadInt(ini, L"ShaderOffset", cfg.shaderOffset);
    cfg.probeD3D12 = ReadInt(ini, L"ProbeD3D12", cfg.probeD3D12) != 0;
    cfg.api = ReadString(ini, L"Api", cfg.api.c_str());
}

bool SaveConfig(const std::wstring& ini, const Config& cfg) {
    wchar_t flow[32];
    swprintf(flow, 32, L"%.2f", cfg.flowScale);
    wchar_t key1[16], key2[16], key3[16];
    swprintf(key1, 16, L"0x%02X", cfg.toggleKey);
    swprintf(key2, 16, L"0x%02X", cfg.cycleKey);
    swprintf(key3, 16, L"0x%02X", cfg.menuKey);

    bool ok = true;
    ok &= Write(ini, L"Shaders", cfg.shaders);
    ok &= Write(ini, L"LosslessDll", cfg.losslessDll);
    ok &= Write(ini, L"Multiplier", std::to_wstring(ClampMultiplier(cfg.multiplier)));
    ok &= Write(ini, L"FlowScale", flow);
    ok &= Write(ini, L"PerformanceMode", cfg.performanceMode ? L"1" : L"0");
    ok &= Write(ini, L"Dynamic", cfg.dynamic ? L"1" : L"0");
    ok &= Write(ini, L"HDR", std::to_wstring(cfg.hdr));
    ok &= Write(ini, L"VSyncPacing", cfg.vsyncPacing ? L"1" : L"0");
    ok &= Write(ini, L"EnabledAtStart", cfg.enabled ? L"1" : L"0");
    ok &= Write(ini, L"ToggleKey", key1);
    ok &= Write(ini, L"CycleKey", key2);
    ok &= Write(ini, L"MenuKey", key3);
    ok &= Write(ini, L"ShaderOffset", std::to_wstring(cfg.shaderOffset));
    ok &= Write(ini, L"ProbeD3D12", cfg.probeD3D12 ? L"1" : L"0");
    ok &= Write(ini, L"Api", cfg.api);
    return ok;
}

std::wstring ModuleDirectory(HMODULE module) {
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        const DWORD n = GetModuleFileNameW(module, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return L".";
        if (n < buf.size()) break;
        buf.resize(buf.size() * 2);
    }
    std::wstring path(buf.data());
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

} // namespace lsfgi
