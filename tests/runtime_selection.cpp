#include "../nr_runtime.h"
#include <cassert>
#include <initializer_list>
#include <cstdio>
int main() {
    for (const wchar_t *name : {L"NVIDIA GeForce RTX 4060", L"NVIDIA GeForce RTX 4070 SUPER",
                               L"NVIDIA GeForce RTX 4070 Ti SUPER", L"NVIDIA GeForce RTX 4090",
                               L"NVIDIA GeForce RTX 4070 Laptop GPU"})
        assert(RuntimeForAdapter(0x10DE, name) == NRRuntime::RTX40);
    for (const wchar_t *name : {L"NVIDIA GeForce RTX 5060", L"NVIDIA GeForce RTX 5070 Ti", L"NVIDIA GeForce RTX 5090"})
        assert(RuntimeForAdapter(0x10DE, name) == NRRuntime::RTX50);
    assert(RuntimeForAdapter(0x1002, L"AMD Radeon") == NRRuntime::Unsupported);
    assert(RuntimeForAdapter(0x10DE, L"NVIDIA GeForce RTX 3090") == NRRuntime::Unsupported);
    assert(RuntimeForAdapter(0x10DE, nullptr) == NRRuntime::Unsupported);
    assert(std::wcscmp(RuntimeRelativePath(NRRuntime::RTX40), L"runtime40\\nvngx_dlssnr.dll") == 0);
    assert(std::wcscmp(RuntimeRelativePath(NRRuntime::RTX50), L"nvngx_dlssnr.dll") == 0);
    puts("PASS: GPU family selection, Ti/Super/laptop variants, runtime paths, unsupported devices");
}
