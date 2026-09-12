#pragma once
#include <cwchar>

enum class NRRuntime { Unsupported, RTX40, RTX50 };

// Use the selected DXGI adapter, not the first GPU or a system-wide guess.
inline NRRuntime RuntimeForAdapter(unsigned vendor, const wchar_t *name)
{
    if (vendor != 0x10DE || !name) return NRRuntime::Unsupported;
    if (std::wcsstr(name, L"RTX 40")) return NRRuntime::RTX40;
    if (std::wcsstr(name, L"RTX 50")) return NRRuntime::RTX50;
    return NRRuntime::Unsupported;
}

inline const wchar_t *RuntimeRelativePath(NRRuntime runtime)
{
    return runtime == NRRuntime::RTX40 ? L"runtime40\\nvngx_dlssnr.dll" : L"nvngx_dlssnr.dll";
}
