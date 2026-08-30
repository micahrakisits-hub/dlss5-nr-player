// nr_video.cpp - DLSS 5 Neural Rendering video frame pipeline.
// Feeds a sequence of PNG frames through nvngx_dlssnr.dll (the same runtime
// NR-Media-UI's CLI uses) keeping ONE NGX session alive across frames, so
// temporal history (reset=0) is preserved exactly like in games.
//
// Usage: nr_video.exe in_dir out_dir [--style natural|cinematic|default|0..6]
//                                  [--preset 0..3] [--intensity 0..2]
//                                  [--tone 0..2] [--structure 0..2]
//                                  [--skin -1..2] [--mask 0|1]
// Reads in_dir/*.png sorted, writes out_dir/frame_%05d.png
//
// Build (MSVC, Hostx64):
//   cl /nologo /EHsc /O2 /MT nr_video.cpp /link /OUT:nr_video.exe
//      d3d12.lib dxgi.lib windowscodecs.lib ole32.lib

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef NVSDK_CONV
#define NVSDK_CONV __cdecl
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// NGX interface (declared inline, mirrors NVIDIA's vtable layout)
// ---------------------------------------------------------------------------
typedef int NVSDK_NGX_Result;
static const NVSDK_NGX_Result NGX_SUCCESS = 1;

struct NVSDK_NGX_Handle { unsigned int Id; };

struct NVSDK_NGX_Parameter
{
    virtual void Set(const char *InName, unsigned long long InValue) = 0;
    virtual void Set(const char *InName, float InValue) = 0;
    virtual void Set(const char *InName, double InValue) = 0;
    virtual void Set(const char *InName, unsigned int InValue) = 0;
    virtual void Set(const char *InName, int InValue) = 0;
    virtual void Set(const char *InName, ID3D11Resource *InValue) = 0;
    virtual void Set(const char *InName, ID3D12Resource *InValue) = 0;
    virtual void Set(const char *InName, void *InValue) = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, unsigned long long *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, float *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, double *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, unsigned int *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, int *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, ID3D11Resource **OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, ID3D12Resource **OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, void **OutValue) const = 0;
    virtual void Reset() = 0;
};

typedef struct NVSDK_NGX_PathListInfo
{
    wchar_t const *const *Path;
    unsigned int Length;
} NVSDK_NGX_PathListInfo;

typedef enum NVSDK_NGX_Logging_Level
{
    NVSDK_NGX_LOGGING_LEVEL_OFF = 0,
    NVSDK_NGX_LOGGING_LEVEL_ON,
    NVSDK_NGX_LOGGING_LEVEL_VERBOSE,
} NVSDK_NGX_Logging_Level;

typedef void(NVSDK_CONV *NVSDK_NGX_AppLogCallback)(const char *message,
                                                   NVSDK_NGX_Logging_Level loggingLevel,
                                                   int sourceComponent);

typedef struct NVSDK_NGX_LoggingInfo
{
    NVSDK_NGX_Logging_Level LoggingLevel;
    NVSDK_NGX_AppLogCallback Callback;
    void *UserData;
    bool DisableOtherLoggingSinks;
} NVSDK_NGX_LoggingInfo;

typedef struct NVSDK_NGX_FeatureCommonInfo_Internal NVSDK_NGX_FeatureCommonInfo_Internal;

typedef struct NVSDK_NGX_FeatureCommonInfo
{
    NVSDK_NGX_PathListInfo PathListInfo;
    NVSDK_NGX_FeatureCommonInfo_Internal *InternalData;
    NVSDK_NGX_LoggingInfo LoggingInfo;
} NVSDK_NGX_FeatureCommonInfo;

typedef NVSDK_NGX_Result (*PFN_Init)(unsigned long long, const wchar_t *, ID3D12Device *,
                                     const void *, int);
typedef NVSDK_NGX_Result (*PFN_Init_Ext)(unsigned long long, const wchar_t *, ID3D12Device *,
                                         int, const void *);
// Snippet-build Init_Ext: (app, path, device, FeatureCommonInfo*, version)
typedef NVSDK_NGX_Result (*PFN_Init_Ext_Snippet)(unsigned long long, const wchar_t *,
                                                 ID3D12Device *, const void *, int);
// Core Init_ProjectID (arg order from bridge's disassembly):
//   (projectId, engineType, engineVersion, dataPath, device, version, FCI)
typedef NVSDK_NGX_Result (*PFN_Init_ProjectID)(const char *, int, const char *,
                                               const wchar_t *, ID3D12Device *, int,
                                               const void *);
// The caller shim (caller\nvngx.dll) wraps the snippet's entry points so the
// return address lands inside the shim -- the snippet validates its caller and
// refuses direct calls with 0xBAD00002 (PlatformError). Each wrapper takes the
// real function pointer first and forwards the remaining args.
typedef NVSDK_NGX_Result (*PFN_ShimInit)(void *, unsigned long long, const wchar_t *,
                                         ID3D12Device *, int, const void *);
typedef NVSDK_NGX_Result (*PFN_ShimCreate)(void *, ID3D12GraphicsCommandList *, int,
                                           NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
typedef NVSDK_NGX_Result (*PFN_ShimEvaluate)(void *, ID3D12GraphicsCommandList *,
                                             const NVSDK_NGX_Handle *,
                                             const NVSDK_NGX_Parameter *, void *);
typedef NVSDK_NGX_Result (*PFN_ShimRelease)(void *, NVSDK_NGX_Handle *);
typedef NVSDK_NGX_Result (*PFN_ShimShutdown)(void *);
typedef NVSDK_NGX_Result (*PFN_AllocateParameters)(NVSDK_NGX_Parameter **);
typedef NVSDK_NGX_Result (*PFN_D3D12CreateFeature)(ID3D12GraphicsCommandList *, int,
                                                   NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
typedef NVSDK_NGX_Result (*PFN_D3D12EvaluateFeature)(ID3D12GraphicsCommandList *,
                                                     const NVSDK_NGX_Handle *,
                                                     const NVSDK_NGX_Parameter *, void *);
typedef NVSDK_NGX_Result (*PFN_D3D12ReleaseFeature)(NVSDK_NGX_Handle *);
typedef NVSDK_NGX_Result (*PFN_Shutdown)(void);

// DLSS 5 Neural Rendering feature id. renodx logs "feature 18 created via the
// signed snippet" -- 18 is the NR feature id it creates after DLSS/DLAA.
static const int NR_FEATURE_ID = 18;

// ---------------------------------------------------------------------------
// globals
// ---------------------------------------------------------------------------
static PFN_Init_Ext            g_direct_init;
static PFN_Init_Ext            g_init_ext;
static PFN_Init_ProjectID      g_init_projectid;
static PFN_AllocateParameters  g_alloc;
static PFN_D3D12CreateFeature  g_create;
static PFN_D3D12EvaluateFeature g_eval;
static PFN_D3D12ReleaseFeature g_release;
static PFN_Shutdown            g_shutdown;
static PFN_D3D12CreateFeature  g_nr_create;
static PFN_D3D12EvaluateFeature g_nr_eval;
static PFN_D3D12ReleaseFeature g_nr_release;
static PFN_ShimInit            g_shim_init;
static PFN_ShimCreate          g_shim_create;
static PFN_ShimEvaluate        g_shim_eval;
static PFN_ShimRelease         g_shim_release;
static PFN_ShimShutdown        g_shim_shutdown;

static ComPtr<ID3D12Device>             g_dev;
static ComPtr<ID3D12CommandQueue>       g_queue;
static ComPtr<ID3D12CommandAllocator>   g_alloc0;
static ComPtr<ID3D12GraphicsCommandList> g_list;
static ComPtr<ID3D12Fence>              g_fence;
static UINT64                           g_fence_value = 0;

static NVSDK_NGX_Parameter *g_params = nullptr;
static NVSDK_NGX_Handle    *g_feature = nullptr;

// ---------------------------------------------------------------------------
// logging
// ---------------------------------------------------------------------------
static void Log(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap); fprintf(stderr, "\n");
    va_end(ap);
}

// ---------------------------------------------------------------------------
// D3D12 setup
// ---------------------------------------------------------------------------
static int g_gpu_index = -1;   // -1 = auto (first hardware adapter)
static int g_gpu_vendor = 0;   // 0x10DE = NVIDIA only

static ComPtr<ID3D12Device> CreateDevice()
{
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;

    // Enumerate all hardware adapters once, log them, then pick the requested one.
    struct Ad { ComPtr<IDXGIAdapter1> a; DXGI_ADAPTER_DESC1 d; };
    std::vector<Ad> hw;
    ComPtr<IDXGIAdapter1> it;
    for (UINT i = 0; factory->EnumAdapters1(i, &it) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc = {};
        it->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { it.Reset(); continue; }
        Ad e; e.a = it; e.d = desc;
        hw.push_back(e);
        it.Reset();
    }
    for (size_t k = 0; k < hw.size(); ++k)
        Log("GPU[%zu]: %ls (vendor=0x%04X vid=%u, VRAM=%llu MB)",
            k, hw[k].d.Description, (unsigned)hw[k].d.VendorId, hw[k].d.DeviceId,
            (unsigned long long)(hw[k].d.DedicatedVideoMemory >> 20));

    int sel = (g_gpu_index >= 0) ? g_gpu_index : 0;
    // If the user asked for a specific name, match by substring first.
    if (g_gpu_index < 0) sel = 0;
    for (size_t k = 0; k < hw.size(); ++k)
    {
        if ((int)k != sel) continue;
        ComPtr<ID3D12Device> dev;
        if (SUCCEEDED(D3D12CreateDevice(hw[k].a.Get(), D3D_FEATURE_LEVEL_12_0,
                                        IID_PPV_ARGS(&dev))))
        {
            Log("using adapter: %ls", hw[k].d.Description);
            return dev;
        }
        Log("FAIL: D3D12CreateDevice on GPU[%zu] %ls", k, hw[k].d.Description);
    }
    return nullptr;
}

static bool SetupD3D12()
{
    g_dev = CreateDevice();
    if (!g_dev) { Log("FAIL: no D3D12 device"); return false; }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue))))
        { Log("FAIL: CreateCommandQueue"); return false; }
    if (FAILED(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(&g_alloc0))))
        { Log("FAIL: CreateCommandAllocator"); return false; }
    if (FAILED(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        g_alloc0.Get(), nullptr,
                                        IID_PPV_ARGS(&g_list))))
        { Log("FAIL: CreateCommandList"); return false; }
    if (FAILED(g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                  IID_PPV_ARGS(&g_fence))))
        { Log("FAIL: CreateFence"); return false; }
    return true;
}

static void ExecuteAndWait()
{
    g_list->Close();
    g_queue->ExecuteCommandLists(1, (ID3D12CommandList *const *)g_list.GetAddressOf());
    ++g_fence_value;
    g_queue->Signal(g_fence.Get(), g_fence_value);
    if (g_fence->GetCompletedValue() < g_fence_value)
    {
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_fence->SetEventOnCompletion(g_fence_value, ev);
        WaitForSingleObject(ev, 20000);
        CloseHandle(ev);
    }
    g_alloc0->Reset();
    g_list->Reset(g_alloc0.Get(), nullptr);
}

// ---------------------------------------------------------------------------
// texture helpers
// ---------------------------------------------------------------------------
static ComPtr<ID3D12Resource> CreateTex2D(UINT w, UINT h, DXGI_FORMAT fmt,
                                          D3D12_RESOURCE_STATES init,
                                          D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1;
    d.MipLevels = 1; d.Format = fmt; d.SampleDesc.Count = 1;
    d.Flags = flags;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> r;
    if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, init,
                                              nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

// ---------------------------------------------------------------------------
// PNG load/save via WIC (same stack as the CLI)
// ---------------------------------------------------------------------------
static bool LoadPng(const wchar_t *path, UINT *out_w, UINT *out_h,
                    std::vector<uint8_t> *rgba)
{
    ComPtr<IWICImagingFactory> fac;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&fac)))) return false;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(fac->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
                                              WICDecodeMetadataCacheOnLoad,
                                              &dec))) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(fac->CreateFormatConverter(&conv))) return false;
    if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom))) return false;
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    rgba->resize((size_t)w * h * 4);
    if (FAILED(conv->CopyPixels(nullptr, w * 4, (UINT)rgba->size(), rgba->data())))
        return false;
    *out_w = w; *out_h = h;
    return true;
}

static bool SavePng(const wchar_t *path, UINT w, UINT h, const uint8_t *rgba)
{
    ComPtr<IWICImagingFactory> fac;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&fac)))) return false;
    ComPtr<IWICStream> stream;
    if (FAILED(fac->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE))) return false;
    ComPtr<IWICBitmapEncoder> enc;
    if (FAILED(fac->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc))) return false;
    if (FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> fe;
    ComPtr<IPropertyBag2> bag;
    if (FAILED(enc->CreateNewFrame(&fe, &bag))) return false;
    if (FAILED(fe->Initialize(bag.Get()))) return false;
    if (FAILED(fe->SetSize(w, h))) return false;
    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppRGBA;
    if (FAILED(fe->SetPixelFormat(&pf))) return false;
    if (FAILED(fe->WritePixels(h, w * 4, w * h * 4, (BYTE *)rgba))) return false;
    if (FAILED(fe->Commit())) return false;
    if (FAILED(enc->Commit())) return false;
    return true;
}

static D3D12_RESOURCE_BARRIER TransitionBarrier(ID3D12Resource *res,
                                                 D3D12_RESOURCE_STATES before,
                                                 D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

// R16G16B16A16_FLOAT stores IEEE half-floats, not UNORM. The CLI converts
// RGBA8 -> /255 -> [0,1] float -> half. These helpers do the bit conversion.
static uint16_t FloatToHalf(float f)
{
    uint32_t x; memcpy(&x, &f, 4);
    uint32_t s = (x >> 16) & 0x8000u;
    int32_t  e = (int32_t)((x >> 23) & 0xff) - 127 + 15;
    uint32_t m = x & 0x7fffffu;
    if (e <= 0) { if (e < -10) return (uint16_t)s; m = (m | 0x800000u) >> (1 - e); return (uint16_t)(s | (m >> 13)); }
    if (e >= 31) return (uint16_t)(s | 0x7c00u);
    return (uint16_t)(s | ((uint32_t)e << 10) | (m >> 13));
}
static float HalfToFloat(uint16_t h)
{
    uint32_t s = (h >> 15) & 1;
    uint32_t e = (h >> 10) & 0x1f;
    uint32_t m = h & 0x3ff;
    uint32_t x;
    if (e == 0) { if (m == 0) x = s << 31; else { e = 1; while (!(m & 0x400)) { m <<= 1; --e; } m &= 0x3ff; x = (s << 31) | ((e + 112) << 23) | (m << 13); } }
    else if (e == 0x1f) x = (s << 31) | 0x7f800000u | (m << 13);
    else x = (s << 31) | ((e + 112) << 23) | (m << 13);
    float f; memcpy(&f, &x, 4); return f;
}

// ---------------------------------------------------------------------------
// frame processing
// ---------------------------------------------------------------------------
static bool ProcessFrames(const std::vector<std::wstring> &inputs,
                          const std::wstring &out_dir,
                          const std::string &style, int preset, int intensity,
                          int tone, int structure, int skin, int mask)
{
    // Load the first frame to learn dimensions.
    UINT w = 0, h = 0;
    std::vector<uint8_t> rgba;
    if (!LoadPng(inputs[0].c_str(), &w, &h, &rgba))
        { Log("FAIL: cannot load %ls", inputs[0].c_str()); return false; }
    Log("input: %ux%u from %ls", w, h, inputs[0].c_str());

    const DXGI_FORMAT fmt = DXGI_FORMAT_R16G16B16A16_FLOAT; // as the CLI
    auto color = CreateTex2D(w, h, fmt, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    // The NR feature writes the output as a UAV: the CLI logs
    // "Color=NON_PIXEL_SHADER_RESOURCE Output=UNORDERED_ACCESS".
    auto output = CreateTex2D(w, h, fmt, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!color || !output) { Log("FAIL: create textures"); return false; }

    // Upload buffer for RGBA8 -> R16G16B16A16_FLOAT conversion is done on CPU.
    std::vector<uint8_t> rgba16(w * h * 8);
    for (size_t i = 0; i < (size_t)w * h; ++i)
    {
        float r = rgba[i*4+0] / 255.0f, g = rgba[i*4+1] / 255.0f;
        float b = rgba[i*4+2] / 255.0f, a = rgba[i*4+3] / 255.0f;
        ((uint16_t*)rgba16.data())[i*4+0] = FloatToHalf(r);
        ((uint16_t*)rgba16.data())[i*4+1] = FloatToHalf(g);
        ((uint16_t*)rgba16.data())[i*4+2] = FloatToHalf(b);
        ((uint16_t*)rgba16.data())[i*4+3] = FloatToHalf(a);
    }

    // Persistent upload heap + readback for the color texture.
    UINT row_pitch = (w * 8 + 255) & ~255u; // 256-byte aligned row pitch
    UINT64 total = (UINT64)row_pitch * h;
    D3D12_RESOURCE_DESC ubd = {};
    ubd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    ubd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    ubd.Width = total; ubd.Height = 1; ubd.DepthOrArraySize = 1;
    ubd.MipLevels = 1; ubd.SampleDesc.Count = 1;
    ubd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES uhp = {};
    uhp.Type = D3D12_HEAP_TYPE_UPLOAD;
    ComPtr<ID3D12Resource> upload;
    if (FAILED(g_dev->CreateCommittedResource(&uhp, D3D12_HEAP_FLAG_NONE, &ubd,
                                              D3D12_RESOURCE_STATE_GENERIC_READ,
                                              nullptr, IID_PPV_ARGS(&upload))))
        { Log("FAIL: upload heap"); return false; }

    D3D12_HEAP_PROPERTIES rhp = {};
    rhp.Type = D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> readback;
    if (FAILED(g_dev->CreateCommittedResource(&rhp, D3D12_HEAP_FLAG_NONE, &ubd,
                                              D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&readback))))
        { Log("FAIL: readback heap"); return false; }

    // NGX init: load the driver's NGX core (_nvngx.dll), which has the full
    // export set including AllocateParameters. nvngx_dlssnr.dll (the NR model)
    // must sit next to this exe; NGX finds it by name.
    HMODULE ngx = LoadLibraryW(L"_nvngx.dll");
    if (!ngx)
    {
        // Fall back to scanning the DriverStore like the CLI does.
        WIN32_FIND_DATAW fd;
        wchar_t pat[MAX_PATH];
        swprintf_s(pat, L"%ls\\FileRepository\\nv_dispi.inf_*\\_nvngx.dll",
                   L"C:\\Windows\\System32\\DriverStore");
        HANDLE hf = FindFirstFileW(pat, &fd);
        if (hf != INVALID_HANDLE_VALUE)
        {
            wchar_t full[MAX_PATH];
            swprintf_s(full, L"C:\\Windows\\System32\\DriverStore\\FileRepository\\%ls",
                       fd.cFileName);
            ngx = LoadLibraryW(full);
            FindClose(hf);
        }
    }
    if (!ngx) { Log("FAIL: cannot load _nvngx.dll (driver NGX core)"); return false; }
    g_init_ext = (PFN_Init_Ext)GetProcAddress(ngx, "NVSDK_NGX_D3D12_Init_Ext");
    g_init_projectid = (PFN_Init_ProjectID)GetProcAddress(ngx, "NVSDK_NGX_D3D12_Init_ProjectID");
    g_alloc    = (PFN_AllocateParameters)GetProcAddress(ngx, "NVSDK_NGX_D3D12_AllocateParameters");
    g_create   = (PFN_D3D12CreateFeature)GetProcAddress(ngx, "NVSDK_NGX_D3D12_CreateFeature");
    g_eval     = (PFN_D3D12EvaluateFeature)GetProcAddress(ngx, "NVSDK_NGX_D3D12_EvaluateFeature");
    g_release  = (PFN_D3D12ReleaseFeature)GetProcAddress(ngx, "NVSDK_NGX_D3D12_ReleaseFeature");

    // The NR snippet must be loaded so the core sees it as an NGX layer
    // (renodx LoadLibrary's it; bridge logs it as "NGX layer 0"). The CLI
    // creates the NR feature "direct" -- through the snippet's own export,
    // after the core session (and its DLSS feature) is up.
    HMODULE nr = LoadLibraryW(L"nvngx_dlssnr.dll");
    if (!nr) { Log("FAIL: cannot load nvngx_dlssnr.dll (NR runtime)"); return false; }
    g_direct_init = (PFN_Init_Ext)GetProcAddress(nr, "NVSDK_NGX_D3D12_Init_Ext");
    g_nr_create   = (PFN_D3D12CreateFeature)GetProcAddress(nr, "NVSDK_NGX_D3D12_CreateFeature");
    g_nr_eval     = (PFN_D3D12EvaluateFeature)GetProcAddress(nr, "NVSDK_NGX_D3D12_EvaluateFeature");
    g_nr_release  = (PFN_D3D12ReleaseFeature)GetProcAddress(nr, "NVSDK_NGX_D3D12_ReleaseFeature");
    g_shutdown = (PFN_Shutdown)GetProcAddress(ngx, "NVSDK_NGX_D3D12_Shutdown");
    if (!g_init_ext || !g_alloc || !g_create || !g_eval || !g_release || !g_shutdown)
    { Log("FAIL: NGX entry points missing (init=%p alloc=%p create=%p eval=%p rel=%p shut=%p)",
          g_init_ext, g_alloc, g_create, g_eval, g_release, g_shutdown); return false; }

    // The caller shim wraps the snippet's entry points so the snippet's caller
    // validation passes (return address lands inside the shim). Load it and
    // wire the wrappers to the snippet's exports.
    HMODULE shim = LoadLibraryW(L"caller\\nvngx.dll");
    if (shim)
    {
        g_shim_init     = (PFN_ShimInit)GetProcAddress(shim, "DLSSNR_CallInit");
        g_shim_create   = (PFN_ShimCreate)GetProcAddress(shim, "DLSSNR_CallCreate");
        g_shim_eval     = (PFN_ShimEvaluate)GetProcAddress(shim, "DLSSNR_CallEvaluate");
        g_shim_release  = (PFN_ShimRelease)GetProcAddress(shim, "DLSSNR_CallRelease");
        g_shim_shutdown = (PFN_ShimShutdown)GetProcAddress(shim, "DLSSNR_CallShutdown");
    }
    Log("caller shim: %s (init=%p create=%p eval=%p)", shim ? "loaded" : "MISSING",
        g_shim_init, g_shim_create, g_shim_eval);

    wchar_t data_path[MAX_PATH] = L".";
    GetCurrentDirectoryW(MAX_PATH, data_path);
    Log("NGX data path: %ls", data_path);
    // The CLI's project maps to app id 141959980 (0x0876232C); the snippet
    // init must use the same id as the core session.
    const unsigned long long APP_ID = 141959980ULL; // 0x0876232C
    int inited = 0;
    const wchar_t *path_list[1] = { data_path };
    NVSDK_NGX_PathListInfo pli = {};
    pli.Path = path_list;
    pli.Length = 1;
    NVSDK_NGX_FeatureCommonInfo fci = {};
    fci.PathListInfo = pli;
    fci.InternalData = nullptr;
    fci.LoggingInfo.LoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
    fci.LoggingInfo.Callback = nullptr;
    fci.LoggingInfo.UserData = nullptr;
    fci.LoggingInfo.DisableOtherLoggingSinks = false;

    // The DLSS 5 Neural Rendering feature only initialises on a session opened
    // with Init_ProjectID (the CLI logs "MapProjectId ... projectID 53f803cc-
    // a12f-4d69-90d5-19b7599cad19, engine: custom, engineVersion 0.1"). Plain
    // Init_Ext opens a session where CreateFeature(18) is refused.
    if (g_init_projectid)
    {
        for (int ver = 0x13; ver <= 0x20 && !inited; ++ver)
        {
            NVSDK_NGX_Result r = g_init_projectid("53f803cc-a12f-4d69-90d5-19b7599cad19",
                                                  0 /* ENGINE_TYPE_CUSTOM */, "0.1",
                                                  data_path, g_dev.Get(), ver, nullptr);
            if (r == NGX_SUCCESS) { Log("core Init_ProjectID ver=0x%02X ok", ver); inited = 1; }
            else Log("core Init_ProjectID ver=0x%02X -> 0x%08X", ver, (unsigned)r);
        }
    }
    if (!inited)
    {
        for (int ver = 0x13; ver <= 0x20 && !inited; ++ver)
        {
            NVSDK_NGX_Result r = g_init_ext(APP_ID, data_path, g_dev.Get(), ver, &fci);
            if (r == NGX_SUCCESS) { Log("core Init_Ext ver=0x%02X ok", ver); inited = 1; }
            else Log("core Init_Ext ver=0x%02X -> 0x%08X", ver, (unsigned)r);
        }
    }
    if (!inited) { Log("FAIL: NGX init"); return false; }

    // The snippet's own init (CLI logs "DLSSNR direct init"). It needs the
    // core session above, and must be called through the shim so the snippet's
    // caller validation passes. Version 0x15 (21) matches the CLI.
    if (g_direct_init && g_shim_init)
    {
        NVSDK_NGX_Result r = g_shim_init((void *)g_direct_init, APP_ID, data_path,
                                         g_dev.Get(), 0x15, &fci);
        Log("snippet Init_Ext (via shim) -> 0x%08X", (unsigned)r);
    }
    else if (g_direct_init)
    {
        NVSDK_NGX_Result r = g_direct_init(APP_ID, data_path, g_dev.Get(), 0x15, &fci);
        Log("snippet Init_Ext (direct) -> 0x%08X", (unsigned)r);
    }

    NVSDK_NGX_Result ra = g_alloc(&g_params);
    if (ra != NGX_SUCCESS || !g_params) { Log("FAIL: AllocateParameters 0x%08X", (unsigned)ra); return false; }

    // Feature parameters. The CLI (reference) passes only DLSSNR.* keys for
    // the NR feature (id 18). Its diagnostic log records: Style=1 (int, not a
    // string) Preset=3 Intensity=1 LocalToneStrength=1 LocalStructureStrength=1
    // SkinStructureStrength=-1 UseAutoMask=0 Enabled=1 UICorrection=0
    // DepthInverted=1 MVecScale=(1,1) ScalingRatio=1, and Color/Output only
    // (MVec/Depth/ControlMask are absent).
    int style_int = 1; // natural
    if (style == "default") style_int = 0;
    else if (style == "natural") style_int = 1;
    else if (style == "cinematic") style_int = 2;
    else style_int = atoi(style.c_str());
    g_params->Set("DLSSNR.Width", w);
    g_params->Set("DLSSNR.Height", h);
    g_params->Set("DLSSNR.Enabled", 1);
    g_params->Set("DLSSNR.Reset", 1);
    g_params->Set("DLSSNR.Style", style_int);
    g_params->Set("DLSSNR.Hint.Render.Preset", preset);
    g_params->Set("DLSSNR.Intensity", (float)intensity);
    g_params->Set("DLSSNR.LocalToneStrength", (float)tone);
    g_params->Set("DLSSNR.LocalStructureStrength", (float)structure);
    g_params->Set("DLSSNR.SkinStructureStrength", (float)skin);
    g_params->Set("DLSSNR.UseAutoMask", mask);
    g_params->Set("DLSSNR.UICorrection", 0);
    g_params->Set("DLSSNR.DepthInverted", 1);
    g_params->Set("DLSSNR.ScalingRatio", 1.0f);
    g_params->Set("DLSSNR.MVecScaleX", 1.0f);
    g_params->Set("DLSSNR.MVecScaleY", 1.0f);
    g_params->Set("DLSSNR.Color", color.Get());
    g_params->Set("DLSSNR.Output", output.Get());
    g_params->Set("DLSSNR.Backbuffer", output.Get());
    g_params->Set("DLSSNR.ColorSubrectBaseX", 0);
    g_params->Set("DLSSNR.ColorSubrectBaseY", 0);
    g_params->Set("DLSSNR.ColorSubrectWidth", w);
    g_params->Set("DLSSNR.ColorSubrectHeight", h);
    g_params->Set("DLSSNR.OutputSubrectBaseX", 0);
    g_params->Set("DLSSNR.OutputSubrectBaseY", 0);
    g_params->Set("DLSSNR.OutputSubrectWidth", w);
    g_params->Set("DLSSNR.OutputSubrectHeight", h);

    Log("creating NR feature %d ...", NR_FEATURE_ID);

    // The CLI creates the NR feature "direct" through the snippet (wrapped by
    // the shim), not through the core's CreateFeature. Drop the DLSS SR
    // prerequisite (photos have none) and go straight to the snippet.
    if (g_nr_create && g_shim_create)
    {
        NVSDK_NGX_Result rc = g_shim_create((void *)g_nr_create, g_list.Get(),
                                            NR_FEATURE_ID, g_params, &g_feature);
        if (rc != NGX_SUCCESS || !g_feature)
        { Log("FAIL: CreateFeature(18) via shim -> 0x%08X", (unsigned)rc); return false; }
    }
    else
    {
        NVSDK_NGX_Result rc = g_create(g_list.Get(), NR_FEATURE_ID, g_params, &g_feature);
        if (rc != NGX_SUCCESS || !g_feature)
        { Log("FAIL: CreateFeature(18) -> 0x%08X", (unsigned)rc); return false; }
    }
    Log("feature created, handle=%p", g_feature);

    // process all frames
    for (size_t n = 0; n < inputs.size(); ++n)
    {
        std::vector<uint8_t> fr;
        UINT fw = w, fh = h;
        if (n > 0)
        {
            if (!LoadPng(inputs[n].c_str(), &fw, &fh, &fr))
                { Log("FAIL: load %ls", inputs[n].c_str()); return false; }
            if (fw != w || fh != h)
                { Log("FAIL: frame %zu size %ux%u != %ux%u", n, fw, fh, w, h); return false; }
            std::vector<uint8_t> fr16(w * h * 8);
            for (size_t i = 0; i < (size_t)w * h; ++i)
            {
                float r = fr[i*4+0]/255.0f, g = fr[i*4+1]/255.0f;
                float b = fr[i*4+2]/255.0f, a = fr[i*4+3]/255.0f;
                ((uint16_t*)fr16.data())[i*4+0]=FloatToHalf(r);
                ((uint16_t*)fr16.data())[i*4+1]=FloatToHalf(g);
                ((uint16_t*)fr16.data())[i*4+2]=FloatToHalf(b);
                ((uint16_t*)fr16.data())[i*4+3]=FloatToHalf(a);
            }
            // upload
            void *mapped = nullptr;
            upload->Map(0, nullptr, &mapped);
            memcpy(mapped, fr16.data(), total);
            upload->Unmap(0, nullptr);
        }
        else
        {
            void *mapped = nullptr;
            upload->Map(0, nullptr, &mapped);
            memcpy(mapped, rgba16.data(), total);
            upload->Unmap(0, nullptr);
        }

        // copy upload -> color (color starts each frame in COPY_DEST)
        auto b1 = TransitionBarrier(color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                    D3D12_RESOURCE_STATE_COPY_DEST);
        g_list->ResourceBarrier(1, &b1);
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = color.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = 0;
        src.PlacedFootprint.Footprint.Format = fmt;
        src.PlacedFootprint.Footprint.Width = w;
        src.PlacedFootprint.Footprint.Height = h;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = row_pitch;
        g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        auto b2 = TransitionBarrier(color.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g_list->ResourceBarrier(1, &b2);

        // evaluate
        g_params->Set("DLSSNR.Reset", n == 0 ? 1 : 0);
        NVSDK_NGX_Result re;
        if (g_nr_eval && g_shim_eval)
            re = g_shim_eval((void *)g_nr_eval, g_list.Get(), g_feature, g_params, nullptr);
        else
            re = g_eval(g_list.Get(), g_feature, g_params, nullptr);
        if (re != NGX_SUCCESS)
            Log("frame %zu: Evaluate -> 0x%08X", n, (unsigned)re);

        // readback output (output ends the evaluate in UNORDERED_ACCESS)
        auto b3 = TransitionBarrier(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                    D3D12_RESOURCE_STATE_COPY_SOURCE);
        g_list->ResourceBarrier(1, &b3);
        D3D12_TEXTURE_COPY_LOCATION rdst = {};
        rdst.pResource = readback.Get();
        rdst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        rdst.PlacedFootprint.Offset = 0;
        rdst.PlacedFootprint.Footprint.Format = fmt;
        rdst.PlacedFootprint.Footprint.Width = w;
        rdst.PlacedFootprint.Footprint.Height = h;
        rdst.PlacedFootprint.Footprint.Depth = 1;
        rdst.PlacedFootprint.Footprint.RowPitch = row_pitch;
        D3D12_TEXTURE_COPY_LOCATION rsrc = {};
        rsrc.pResource = output.Get();
        rsrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        rsrc.SubresourceIndex = 0;
        g_list->CopyTextureRegion(&rdst, 0, 0, 0, &rsrc, nullptr);
        // back to UAV for the next evaluate
        auto b4 = TransitionBarrier(output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g_list->ResourceBarrier(1, &b4);

        ExecuteAndWait();

        void *rmap = nullptr;
        readback->Map(0, nullptr, &rmap);
        std::vector<uint8_t> out_rgba(w * h * 4);
        const uint16_t *p16 = (const uint16_t *)rmap;
        for (size_t i = 0; i < (size_t)w * h; ++i)
        {
            // BGRA in the texture: R from channel 2, B from channel 0.
            out_rgba[i*4+0] = (uint8_t)(HalfToFloat(p16[i*4+2]) * 255.0f + 0.5f);
            out_rgba[i*4+1] = (uint8_t)(HalfToFloat(p16[i*4+1]) * 255.0f + 0.5f);
            out_rgba[i*4+2] = (uint8_t)(HalfToFloat(p16[i*4+0]) * 255.0f + 0.5f);
            out_rgba[i*4+3] = 255;
        }
        readback->Unmap(0, nullptr);

        wchar_t out_path[MAX_PATH];
        swprintf_s(out_path, L"%ls\\frame_%05d.png", out_dir.c_str(), (int)n);
        if (!SavePng(out_path, w, h, out_rgba.data()))
            { Log("FAIL: save %ls", out_path); return false; }
        if ((n % 60) == 0 || n + 1 == inputs.size())
            Log("frame %zu/%zu done", n + 1, inputs.size());
    }

    if (g_feature)
    {
        if (g_nr_release && g_shim_release)
            g_shim_release((void *)g_nr_release, g_feature);
        else
            g_release(g_feature);
    }
    if (g_shutdown) g_shutdown();
    return true;
}

// ---------------------------------------------------------------------------
// entry
// ---------------------------------------------------------------------------
int wmain(int argc, wchar_t **argv)
{
    if (argc < 3)
    {
        Log("usage: nr_video.exe in_dir out_dir [--style S] [--preset P]");
        Log("       [--intensity I] [--tone T] [--structure S] [--skin K] [--mask M]");
        return 1;
    }
    std::wstring in_dir = argv[1], out_dir = argv[2];
    std::string style = "natural";
    int preset = 3, intensity = 1, tone = 1, structure = 1, skin = -1, mask = 0;
    for (int i = 3; i + 1 < argc; i += 2)
    {
        std::wstring k = argv[i];
        int v = _wtoi(argv[i+1]);
        if (k == L"--style")
        {
            char buf[64] = {};
            WideCharToMultiByte(CP_UTF8, 0, argv[i+1], -1, buf, sizeof(buf), nullptr, nullptr);
            style = buf;
        }
        else if (k == L"--preset") preset = v;
        else if (k == L"--intensity") intensity = v;
        else if (k == L"--tone") tone = v;
        else if (k == L"--structure") structure = v;
        else if (k == L"--skin") skin = v;
        else if (k == L"--mask") mask = v;
        else if (k == L"--gpu") g_gpu_index = v;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // enumerate input pngs
    std::vector<std::wstring> inputs;
    std::wstring pat = in_dir + L"\\*.png";
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW(pat.c_str(), &fd);
    if (hf == INVALID_HANDLE_VALUE) { Log("no *.png in %ls", in_dir.c_str()); return 1; }
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        inputs.push_back(in_dir + L"\\" + fd.cFileName); }
    while (FindNextFileW(hf, &fd));
    FindClose(hf);
    std::sort(inputs.begin(), inputs.end());
    if (inputs.empty()) { Log("no frames"); return 1; }
    Log("%zu frames", inputs.size());

    CreateDirectoryW(out_dir.c_str(), nullptr);

    if (!SetupD3D12()) return 1;

    bool ok = ProcessFrames(inputs, out_dir, style, preset, intensity,
                            tone, structure, skin, mask);
    Log(ok ? "DONE" : "FAILED");
    CoUninitialize();
    return ok ? 0 : 1;
}
