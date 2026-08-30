// nr_live.cpp - LIVE DLSS 5 Neural Rendering pipeline.
// Captures a monitor via DXGI Desktop Duplication, runs each frame through
// nvngx_dlssnr.dll (feature 18) keeping ONE NGX session alive (temporal
// history reset=0), and shows the result in a preview window. All GPU-side;
// the only CPU work is the desktop-capture copy, no per-frame readback.
//
// Usage: nr_live.exe [--gpu N] [--style natural|cinematic|default|0..6]
//                    [--preset 0..3] [--intensity 0..2] [--tone 0..2]
//                    [--structure 0..2] [--skin -1..2] [--mask 0|1]
//                    [--monitor N] [--scale 0.25..2]
// Press ESC in the preview window to exit.
//
// Build: cl /nologo /EHsc /O2 /MT nr_live.cpp /link /OUT:nr_live.exe
//        d3d11.lib d3d12.lib dxgi.lib d3dcompiler.lib user32.lib

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef NVSDK_CONV
#define NVSDK_CONV __cdecl
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstdarg>
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

typedef void(NVSDK_CONV *NVSDK_NGX_AppLogCallback)(const char *, NVSDK_NGX_Logging_Level, int);

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

typedef NVSDK_NGX_Result (*PFN_Init_Ext)(unsigned long long, const wchar_t *, ID3D12Device *, int, const void *);
typedef NVSDK_NGX_Result (*PFN_Init_ProjectID)(const char *, int, const char *, const wchar_t *, ID3D12Device *, int, const void *);
typedef NVSDK_NGX_Result (*PFN_ShimInit)(void *, unsigned long long, const wchar_t *, ID3D12Device *, int, const void *);
typedef NVSDK_NGX_Result (*PFN_ShimCreate)(void *, ID3D12GraphicsCommandList *, int, NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
typedef NVSDK_NGX_Result (*PFN_ShimEvaluate)(void *, ID3D12GraphicsCommandList *, const NVSDK_NGX_Handle *, const NVSDK_NGX_Parameter *, void *);
typedef NVSDK_NGX_Result (*PFN_ShimRelease)(void *, NVSDK_NGX_Handle *);
typedef NVSDK_NGX_Result (*PFN_ShimShutdown)(void *);
typedef NVSDK_NGX_Result (*PFN_AllocateParameters)(NVSDK_NGX_Parameter **);
typedef NVSDK_NGX_Result (*PFN_D3D12CreateFeature)(ID3D12GraphicsCommandList *, int, NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
typedef NVSDK_NGX_Result (*PFN_D3D12EvaluateFeature)(ID3D12GraphicsCommandList *, const NVSDK_NGX_Handle *, const NVSDK_NGX_Parameter *, void *);
typedef NVSDK_NGX_Result (*PFN_D3D12ReleaseFeature)(NVSDK_NGX_Handle *);
typedef NVSDK_NGX_Result (*PFN_Shutdown)(void);

static const int NR_FEATURE_ID = 18;

// ---------------------------------------------------------------------------
// globals
// ---------------------------------------------------------------------------
static PFN_Init_Ext            g_init_ext;
static PFN_Init_ProjectID      g_init_projectid;
static PFN_AllocateParameters  g_alloc;
static PFN_D3D12CreateFeature  g_create;
static PFN_D3D12EvaluateFeature g_eval;
static PFN_D3D12ReleaseFeature g_release;
static PFN_Shutdown            g_shutdown;
static PFN_Init_Ext            g_direct_init;
static PFN_D3D12CreateFeature  g_nr_create;
static PFN_D3D12EvaluateFeature g_nr_eval;
static PFN_D3D12ReleaseFeature g_nr_release;
static PFN_ShimInit            g_shim_init;
static PFN_ShimCreate          g_shim_create;
static PFN_ShimEvaluate        g_shim_eval;
static PFN_ShimRelease         g_shim_release;

static ComPtr<ID3D12Device>             g_dev;
static ComPtr<ID3D12CommandQueue>       g_queue;
static ComPtr<ID3D12CommandAllocator>   g_alloc0;
static ComPtr<ID3D12GraphicsCommandList> g_list;
static ComPtr<ID3D12Fence>              g_fence;
static UINT64                           g_fence_value = 0;

static ComPtr<ID3D11Device>             g_d11dev;
static ComPtr<ID3D11DeviceContext>      g_d11ctx;
static ComPtr<IDXGIOutputDuplication>   g_dup;

static ComPtr<ID3D11Device5>            g_d11dev5;
static ComPtr<ID3D11DeviceContext4>     g_d11ctx4;
static ComPtr<ID3D11Fence>              g_d11fence;
static ComPtr<ID3D12Fence>              g_d12fence_shared;
static UINT64                           g_capture_value = 1;

static NVSDK_NGX_Parameter *g_params = nullptr;
static NVSDK_NGX_Handle    *g_feature = nullptr;

static ComPtr<ID3D12Resource> g_shared_bgra;   // D3D11 writes (capture), D3D12 reads
static ComPtr<ID3D11Texture2D> g_d11shared;    // D3D11 side of the shared texture
static ComPtr<ID3D12Resource> g_nr_in;         // RGBA16F NR input
static ComPtr<ID3D12Resource> g_nr_out;        // RGBA16F NR output
static ComPtr<ID3D12Resource> g_stage_bgra;    // BGRA8 stage (shader2 -> backbuffer)
static ComPtr<ID3D12DescriptorHeap> g_cbv_heap;

static ComPtr<ID3D12RootSignature> g_rs;
static ComPtr<ID3D12PipelineState> g_pso_in;   // BGRA8 -> RGBA16F (swizzle)
static ComPtr<ID3D12PipelineState> g_pso_out;  // RGBA16F -> BGRA8

static ComPtr<IDXGISwapChain3>    g_swap;
static HWND                       g_hwnd = nullptr;
static UINT                       g_bb_index = 0;

static UINT g_cap_w = 0, g_cap_h = 0;
static int  g_gpu_index = -1;
static int  g_monitor_index = 0;
static float g_scale = 1.0f;
static std::string g_style = "natural";
static int  g_preset = 3, g_intensity = 1, g_tone = 1, g_structure = 1, g_skin = -1, g_mask = 0;

static volatile bool g_running = true;

// ---------------------------------------------------------------------------
// logging
// ---------------------------------------------------------------------------
static void Log(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap); fprintf(stderr, "\n");
    va_end(ap);
}

static void Fatal(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap); fprintf(stderr, "\n");
    va_end(ap);
    MessageBoxA(nullptr, "nr_live fatal (see console)", "nr_live", MB_ICONERROR);
    ExitProcess(1);
}

// ---------------------------------------------------------------------------
// D3D12 barrier helper
// ---------------------------------------------------------------------------
static D3D12_RESOURCE_BARRIER Trans(ID3D12Resource *res, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER x = {};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = res;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    return x;
}

// submit the open command list and wait for the GPU (leaves the list CLOSED;
// the caller reopens it with Reset)
static void ExecuteAndWait()
{
    g_list->Close();
    ID3D12CommandList *cmds[] = { g_list.Get() };
    g_queue->ExecuteCommandLists(1, cmds);
    g_queue->Signal(g_fence.Get(), ++g_fence_value);
    if (g_fence->GetCompletedValue() < g_fence_value)
    {
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_fence->SetEventOnCompletion(g_fence_value, ev);
        WaitForSingleObject(ev, 20000);
        CloseHandle(ev);
    }
}

// ---------------------------------------------------------------------------
// device creation (D3D12 on selected GPU, D3D11 on the same adapter)
// ---------------------------------------------------------------------------
struct AdapterInfo { ComPtr<IDXGIAdapter1> a; DXGI_ADAPTER_DESC1 d; };

static bool SetupDevices()
{
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;

    std::vector<AdapterInfo> hw;
    ComPtr<IDXGIAdapter1> it;
    for (UINT i = 0; factory->EnumAdapters1(i, &it) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc = {};
        it->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { it.Reset(); continue; }
        AdapterInfo e; e.a = it; e.d = desc;
        hw.push_back(e);
        it.Reset();
    }
    for (size_t k = 0; k < hw.size(); ++k)
        Log("GPU[%zu]: %ls (vendor=0x%04X VRAM=%llu MB)", k, hw[k].d.Description,
            (unsigned)hw[k].d.VendorId, (unsigned long long)(hw[k].d.DedicatedVideoMemory >> 20));

    int sel = (g_gpu_index >= 0) ? g_gpu_index : 0;
    if (sel < 0 || sel >= (int)hw.size()) { Log("FAIL: GPU index %d out of range", sel); return false; }

    if (FAILED(D3D12CreateDevice(hw[sel].a.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_dev))))
        { Log("FAIL: D3D12CreateDevice on %ls", hw[sel].d.Description); return false; }
    Log("D3D12 adapter: %ls", hw[sel].d.Description);

    // D3D11 on the same adapter (Desktop Duplication needs D3D11).
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(hw[sel].a.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                   nullptr, 0, D3D11_SDK_VERSION,
                                   &g_d11dev, &fl, &g_d11ctx);
    if (FAILED(hr)) { Log("FAIL: D3D11CreateDevice 0x%08X", (unsigned)hr); return false; }
    Log("D3D11 adapter: %ls (FL %d.%d)", hw[sel].d.Description, fl >> 12, (fl >> 8) & 0xf);

    g_d11dev.As(&g_d11dev5);
    g_d11ctx.As(&g_d11ctx4);

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue)))) return false;
    if (FAILED(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc0)))) return false;
    if (FAILED(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc0.Get(), nullptr, IID_PPV_ARGS(&g_list)))) return false;
    if (FAILED(g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) return false;

    // Shared fence for D3D11 -> D3D12 sync (D3D11 signals after the capture copy).
    if (g_d11dev5)
    {
        if (SUCCEEDED(g_d11dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_d11fence))))
        {
            HANDLE fh = nullptr;
            if (SUCCEEDED(g_d11fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fh)))
            {
                if (FAILED(g_dev->OpenSharedHandle(fh, IID_PPV_ARGS(&g_d12fence_shared))))
                    Log("WARN: OpenSharedHandle(fence) failed");
                CloseHandle(fh);
            }
        }
    }
    if (!g_d11fence || !g_d12fence_shared)
        Log("WARN: no shared fence; D3D11/D3D12 sync degraded");
    return true;
}

// ---------------------------------------------------------------------------
// NGX setup (same working sequence as nr_video.cpp)
// ---------------------------------------------------------------------------
static bool SetupNGX(UINT w, UINT h)
{
    HMODULE ngx = LoadLibraryW(L"_nvngx.dll");
    if (!ngx)
    {
        WIN32_FIND_DATAW fd;
        wchar_t pat[MAX_PATH];
        swprintf_s(pat, L"%ls\\FileRepository\\nv_dispi.inf_*\\_nvngx.dll",
                   L"C:\\Windows\\System32\\DriverStore");
        HANDLE hf = FindFirstFileW(pat, &fd);
        if (hf != INVALID_HANDLE_VALUE)
        {
            wchar_t full[MAX_PATH];
            swprintf_s(full, L"C:\\Windows\\System32\\DriverStore\\FileRepository\\%ls", fd.cFileName);
            ngx = LoadLibraryW(full);
            FindClose(hf);
        }
    }
    if (!ngx) { Log("FAIL: cannot load _nvngx.dll"); return false; }
    g_init_ext       = (PFN_Init_Ext)GetProcAddress(ngx, "NVSDK_NGX_D3D12_Init_Ext");
    g_init_projectid = (PFN_Init_ProjectID)GetProcAddress(ngx, "NVSDK_NGX_D3D12_Init_ProjectID");
    g_alloc          = (PFN_AllocateParameters)GetProcAddress(ngx, "NVSDK_NGX_D3D12_AllocateParameters");
    g_create         = (PFN_D3D12CreateFeature)GetProcAddress(ngx, "NVSDK_NGX_D3D12_CreateFeature");
    g_eval           = (PFN_D3D12EvaluateFeature)GetProcAddress(ngx, "NVSDK_NGX_D3D12_EvaluateFeature");
    g_release        = (PFN_D3D12ReleaseFeature)GetProcAddress(ngx, "NVSDK_NGX_D3D12_ReleaseFeature");
    g_shutdown       = (PFN_Shutdown)GetProcAddress(ngx, "NVSDK_NGX_D3D12_Shutdown");

    HMODULE nr = LoadLibraryW(L"nvngx_dlssnr.dll");
    if (!nr) { Log("FAIL: cannot load nvngx_dlssnr.dll"); return false; }
    g_direct_init = (PFN_Init_Ext)GetProcAddress(nr, "NVSDK_NGX_D3D12_Init_Ext");
    g_nr_create   = (PFN_D3D12CreateFeature)GetProcAddress(nr, "NVSDK_NGX_D3D12_CreateFeature");
    g_nr_eval     = (PFN_D3D12EvaluateFeature)GetProcAddress(nr, "NVSDK_NGX_D3D12_EvaluateFeature");
    g_nr_release  = (PFN_D3D12ReleaseFeature)GetProcAddress(nr, "NVSDK_NGX_D3D12_ReleaseFeature");

    HMODULE shim = LoadLibraryW(L"caller\\nvngx.dll");
    if (shim)
    {
        g_shim_init     = (PFN_ShimInit)GetProcAddress(shim, "DLSSNR_CallInit");
        g_shim_create   = (PFN_ShimCreate)GetProcAddress(shim, "DLSSNR_CallCreate");
        g_shim_eval     = (PFN_ShimEvaluate)GetProcAddress(shim, "DLSSNR_CallEvaluate");
        g_shim_release  = (PFN_ShimRelease)GetProcAddress(shim, "DLSSNR_CallRelease");
    }
    if (!g_init_projectid || !g_alloc || !g_nr_create || !g_nr_eval || !g_nr_release)
        { Log("FAIL: NGX entry points missing"); return false; }

    wchar_t data_path[MAX_PATH] = L".";
    GetCurrentDirectoryW(MAX_PATH, data_path);
    const unsigned long long APP_ID = 141959980ULL;
    const wchar_t *path_list[1] = { data_path };
    NVSDK_NGX_PathListInfo pli = {}; pli.Path = path_list; pli.Length = 1;
    NVSDK_NGX_FeatureCommonInfo fci = {};
    fci.PathListInfo = pli;
    fci.LoggingInfo.LoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;

    int inited = 0;
    for (int ver = 0x13; ver <= 0x20 && !inited; ++ver)
    {
        NVSDK_NGX_Result r = g_init_projectid("53f803cc-a12f-4d69-90d5-19b7599cad19",
                                              0, "0.1", data_path, g_dev.Get(), ver, nullptr);
        if (r == NGX_SUCCESS) { Log("core Init_ProjectID ver=0x%02X ok", ver); inited = 1; }
    }
    if (!inited) { Log("FAIL: Init_ProjectID"); return false; }

    if (g_direct_init && g_shim_init)
    {
        NVSDK_NGX_Result r = g_shim_init((void *)g_direct_init, APP_ID, data_path, g_dev.Get(), 0x15, &fci);
        Log("snippet Init_Ext (via shim) -> 0x%08X", (unsigned)r);
    }

    NVSDK_NGX_Result ra = g_alloc(&g_params);
    if (ra != NGX_SUCCESS || !g_params) { Log("FAIL: AllocateParameters"); return false; }

    int style_int = 1;
    if (g_style == "default") style_int = 0;
    else if (g_style == "natural") style_int = 1;
    else if (g_style == "cinematic") style_int = 2;
    else style_int = atoi(g_style.c_str());

    g_params->Set("DLSSNR.Width", w);
    g_params->Set("DLSSNR.Height", h);
    g_params->Set("DLSSNR.Enabled", 1);
    g_params->Set("DLSSNR.Reset", 1);
    g_params->Set("DLSSNR.Style", style_int);
    g_params->Set("DLSSNR.Hint.Render.Preset", g_preset);
    g_params->Set("DLSSNR.Intensity", (float)g_intensity);
    g_params->Set("DLSSNR.LocalToneStrength", (float)g_tone);
    g_params->Set("DLSSNR.LocalStructureStrength", (float)g_structure);
    g_params->Set("DLSSNR.SkinStructureStrength", (float)g_skin);
    g_params->Set("DLSSNR.UseAutoMask", g_mask);
    g_params->Set("DLSSNR.UICorrection", 0);
    g_params->Set("DLSSNR.DepthInverted", 1);
    g_params->Set("DLSSNR.ScalingRatio", 1.0f);
    g_params->Set("DLSSNR.MVecScaleX", 1.0f);
    g_params->Set("DLSSNR.MVecScaleY", 1.0f);
    g_params->Set("DLSSNR.Color", g_nr_in.Get());
    g_params->Set("DLSSNR.Output", g_nr_out.Get());
    g_params->Set("DLSSNR.Backbuffer", g_nr_out.Get());
    g_params->Set("DLSSNR.ColorSubrectBaseX", 0);
    g_params->Set("DLSSNR.ColorSubrectBaseY", 0);
    g_params->Set("DLSSNR.ColorSubrectWidth", w);
    g_params->Set("DLSSNR.ColorSubrectHeight", h);
    g_params->Set("DLSSNR.OutputSubrectBaseX", 0);
    g_params->Set("DLSSNR.OutputSubrectBaseY", 0);
    g_params->Set("DLSSNR.OutputSubrectWidth", w);
    g_params->Set("DLSSNR.OutputSubrectHeight", h);

    if (g_nr_create && g_shim_create)
    {
        NVSDK_NGX_Result rc = g_shim_create((void *)g_nr_create, g_list.Get(), NR_FEATURE_ID, g_params, &g_feature);
        if (rc != NGX_SUCCESS || !g_feature)
            { Log("FAIL: CreateFeature(18) via shim -> 0x%08X", (unsigned)rc); return false; }
    }
    else
    {
        NVSDK_NGX_Result rc = g_create(g_list.Get(), NR_FEATURE_ID, g_params, &g_feature);
        if (rc != NGX_SUCCESS || !g_feature)
            { Log("FAIL: CreateFeature(18) -> 0x%08X", (unsigned)rc); return false; }
    }
    Log("NR feature created, handle=%p", g_feature);
    // Commit the CreateFeature work (it records GPU commands into the list).
    ExecuteAndWait();
    return true;
}

// ---------------------------------------------------------------------------
// textures
// ---------------------------------------------------------------------------
static ComPtr<ID3D12Resource> MakeTex(UINT w, UINT h, DXGI_FORMAT fmt, D3D12_RESOURCE_STATES st,
                                      D3D12_RESOURCE_FLAGS flags, bool shared = false)
{
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1;
    d.MipLevels = 1; d.Format = fmt; d.SampleDesc.Count = 1;
    d.Flags = flags;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> r;
    if (FAILED(g_dev->CreateCommittedResource(&hp,
                                              shared ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE,
                                              &d, st, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

// Create a BGRA8 texture in D3D11 (shared) and open it in D3D12. Desktop
// Duplication copies into the D3D11 side; the D3D12 side reads it.
static bool SetupSharedTexture(UINT w, UINT h)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    if (FAILED(g_d11dev->CreateTexture2D(&td, nullptr, &g_d11shared)))
        { Log("FAIL: CreateTexture2D (shared)"); return false; }

    ComPtr<IDXGIResource> dxgi;
    HANDLE sh = nullptr;
    if (FAILED(g_d11shared.As(&dxgi)) || FAILED(dxgi->GetSharedHandle(&sh)))
        { Log("FAIL: GetSharedHandle"); return false; }
    if (FAILED(g_dev->OpenSharedHandle(sh, IID_PPV_ARGS(&g_shared_bgra))))
        { Log("FAIL: OpenSharedHandle (D3D12)"); CloseHandle(sh); return false; }
    CloseHandle(sh);
    Log("shared texture created (%ux%u)", w, h);
    return true;
}

// ---------------------------------------------------------------------------
// compute shaders
// ---------------------------------------------------------------------------
static bool SetupCompute()
{
    // root signature: [0] SRV table (t0), [1] UAV table (u0)
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1; ranges[0].BaseShaderRegister = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1; ranges[1].BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 1;
    rp[0].DescriptorTable.pDescriptorRanges = &ranges[0];
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1;
    rp[1].DescriptorTable.pDescriptorRanges = &ranges[1];

    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 2; rsd.pParameters = rp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
        { Log("FAIL: serialize root sig"); return false; }
    if (FAILED(g_dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                          IID_PPV_ARGS(&g_rs))))
        { Log("FAIL: CreateRootSignature"); return false; }

    // shader 1: BGRA8 (capture) -> RGBA16F (NR input), R/B swizzle
    const char *src1 =
        "Texture2D<float4> src : register(t0);\n"
        "RWTexture2D<float4> dst : register(u0);\n"
        "[numthreads(16,16,1)]\n"
        "void CSMain(uint3 id : SV_DispatchThreadID) {\n"
        "  float4 c = src[id.xy];\n"
        "  dst[id.xy] = float4(c.b, c.g, c.r, 1.0f);\n"
        "}\n";
    // shader 2: RGBA16F (NR output, BGR order) -> BGRA8 (stage)
    const char *src2 =
        "Texture2D<float4> src : register(t0);\n"
        "RWTexture2D<unorm float4> dst : register(u0);\n"
        "[numthreads(16,16,1)]\n"
        "void CSMain(uint3 id : SV_DispatchThreadID) {\n"
        "  dst[id.xy] = src[id.xy];\n"
        "}\n";

    D3D12_COMPUTE_PIPELINE_STATE_DESC ps = {};
    ps.pRootSignature = g_rs.Get();

    ComPtr<ID3DBlob> b1, e1, b2, e2;
    if (FAILED(D3DCompile(src1, strlen(src1), "cs1", nullptr, nullptr, "CSMain", "cs_5_0", 0, 0, &b1, &e1)))
        { Log("FAIL: compile cs1: %s", e1 ? (char *)e1->GetBufferPointer() : "?"); return false; }
    if (FAILED(D3DCompile(src2, strlen(src2), "cs2", nullptr, nullptr, "CSMain", "cs_5_0", 0, 0, &b2, &e2)))
        { Log("FAIL: compile cs2: %s", e2 ? (char *)e2->GetBufferPointer() : "?"); return false; }

    ps.CS = { b1->GetBufferPointer(), b1->GetBufferSize() };
    if (FAILED(g_dev->CreateComputePipelineState(&ps, IID_PPV_ARGS(&g_pso_in)))) return false;
    ps.CS = { b2->GetBufferPointer(), b2->GetBufferSize() };
    if (FAILED(g_dev->CreateComputePipelineState(&ps, IID_PPV_ARGS(&g_pso_out)))) return false;

    // descriptor heap: SRV(BGRA) UAV(NRin) SRV(NRout) UAV(stage)
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 4; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_cbv_heap)))) return false;
    UINT inc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE base = g_cbv_heap->GetCPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    // [0] SRV on shared BGRA8
    srv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    g_dev->CreateShaderResourceView(g_shared_bgra.Get(), &srv, { base.ptr });
    // [1] UAV on NR input RGBA16F
    uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    g_dev->CreateUnorderedAccessView(g_nr_in.Get(), nullptr, &uav, { base.ptr + inc });
    // [2] SRV on NR output RGBA16F
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    g_dev->CreateShaderResourceView(g_nr_out.Get(), &srv, { base.ptr + 2 * inc });
    // [3] UAV on stage BGRA8
    uav.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    g_dev->CreateUnorderedAccessView(g_stage_bgra.Get(), nullptr, &uav, { base.ptr + 3 * inc });

    return true;
}

// ---------------------------------------------------------------------------
// desktop duplication
// ---------------------------------------------------------------------------
static bool SetupCapture()
{
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;

    // Find the adapter matching our D3D12 device's LUID, then its output[monitor].
    LUID want = g_dev->GetAdapterLuid();
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 d = {};
        adapter->GetDesc1(&d);
        if (d.AdapterLuid.HighPart == want.HighPart && d.AdapterLuid.LowPart == want.LowPart)
            break;
        adapter.Reset();
    }
    if (!adapter) { Log("FAIL: matching adapter for capture"); return false; }

    ComPtr<IDXGIOutput> output;
    if (FAILED(adapter->EnumOutputs(g_monitor_index, &output)))
        { Log("FAIL: EnumOutputs(%d)", g_monitor_index); return false; }

    ComPtr<IDXGIOutput1> out1;
    if (FAILED(output.As(&out1))) return false;

    // Describe the output to learn its resolution.
    DXGI_OUTPUT_DESC od = {};
    output->GetDesc(&od);
    g_cap_w = (UINT)(od.DesktopCoordinates.right - od.DesktopCoordinates.left);
    g_cap_h = (UINT)(od.DesktopCoordinates.bottom - od.DesktopCoordinates.top);
    Log("capture output: %ux%u (%ls)", g_cap_w, g_cap_h, od.DeviceName);

    HRESULT hr = out1->DuplicateOutput(g_d11dev.Get(), &g_dup);
    if (FAILED(hr)) { Log("FAIL: DuplicateOutput 0x%08X", (unsigned)hr); return false; }
    return true;
}

// ---------------------------------------------------------------------------
// window + swapchain
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) g_running = false;
        return 0;
    case WM_DESTROY:
        g_running = false;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static bool SetupWindow(UINT w, UINT h)
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"nr_live_preview";
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    RegisterClassExW(&wc);

    RECT r = { 0, 0, (LONG)w, (LONG)h };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExW(0, L"nr_live_preview", L"DLSS 5 Neural Rendering (live)",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top,
                             nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) { Log("FAIL: CreateWindow"); return false; }

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = w; sd.Height = h;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(factory->CreateSwapChainForHwnd(g_queue.Get(), g_hwnd, &sd, nullptr, nullptr, &sc1)))
        { Log("FAIL: CreateSwapChainForHwnd"); return false; }
    sc1.As(&g_swap);
    ShowWindow(g_hwnd, SW_SHOW);
    return true;
}

// ---------------------------------------------------------------------------
// per-frame render
// ---------------------------------------------------------------------------
static bool RenderFrame()
{
    // 1. Capture into the shared BGRA8 texture (D3D11 side).
    DXGI_OUTDUPL_FRAME_INFO fi = {};
    ComPtr<IDXGIResource> desktop;
    HRESULT hr = g_dup->AcquireNextFrame(16, &fi, &desktop);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false; // no new frame, skip
    if (hr == DXGI_ERROR_ACCESS_LOST)
    {
        Log("capture access lost; re-acquiring");
        g_dup.Reset();
        return false;
    }
    if (FAILED(hr)) return false;

    ComPtr<ID3D11Texture2D> dtex;
    desktop.As(&dtex);
    if (dtex && g_d11shared)
    {
        g_d11ctx->CopyResource(g_d11shared.Get(), dtex.Get());
    }

    g_dup->ReleaseFrame();
    desktop.Reset();

    if (g_d11ctx4 && g_d11fence)
    {
        g_d11ctx->Flush();
        g_d11ctx4->Signal(g_d11fence.Get(), g_capture_value);
        ++g_capture_value;
    }

    if (g_d12fence_shared)
        g_queue->Wait(g_d12fence_shared.Get(), g_capture_value - 1);

    // Wait for the GPU to finish the previous frame before reusing the single
    // command allocator/list (otherwise Reset races the in-flight work).
    if (g_fence->GetCompletedValue() < g_fence_value)
    {
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_fence->SetEventOnCompletion(g_fence_value, ev);
        WaitForSingleObject(ev, 20000);
        CloseHandle(ev);
    }

    g_alloc0->Reset();
    HRESULT rr = g_list->Reset(g_alloc0.Get(), nullptr);

    // Bind the shader-visible descriptor heap before using descriptor tables.
    ID3D12DescriptorHeap *heaps[] = { g_cbv_heap.Get() };
    g_list->SetDescriptorHeaps(1, heaps);

    UINT inc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_GPU_DESCRIPTOR_HANDLE h0 = g_cbv_heap->GetGPUDescriptorHandleForHeapStart();

    D3D12_RESOURCE_BARRIER bars[8];
    UINT nb = 0;

    // shared BGRA8: COMMON -> NPSR (read by cs1)
    bars[nb++] = Trans(g_shared_bgra.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // nr_in: NPSR -> UAV (write by cs1)
    bars[nb++] = Trans(g_nr_in.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_list->ResourceBarrier(nb, bars);

    g_list->SetPipelineState(g_pso_in.Get());
    g_list->SetComputeRootSignature(g_rs.Get());
    g_list->SetComputeRootDescriptorTable(0, { h0.ptr });
    g_list->SetComputeRootDescriptorTable(1, { h0.ptr + inc });
    g_list->Dispatch((g_cap_w + 15) / 16, (g_cap_h + 15) / 16, 1);

    // nr_in: UAV -> NPSR (read by NR)
    nb = 0;
    bars[nb++] = Trans(g_nr_in.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_list->ResourceBarrier(nb, bars);

    // NR evaluate (writes nr_out as UAV)
    g_params->Set("DLSSNR.Reset", g_fence_value == 1 ? 1 : 0);
    NVSDK_NGX_Result re;
    if (g_nr_eval && g_shim_eval)
        re = g_shim_eval((void *)g_nr_eval, g_list.Get(), g_feature, g_params, nullptr);
    else
        re = g_eval(g_list.Get(), g_feature, g_params, nullptr);
    if (re != NGX_SUCCESS) Log("Evaluate -> 0x%08X", (unsigned)re);

    // nr_out: UAV -> NPSR (read by cs2); stage: COMMON -> UAV (write by cs2)
    nb = 0;
    bars[nb++] = Trans(g_nr_out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    bars[nb++] = Trans(g_stage_bgra.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_list->ResourceBarrier(nb, bars);

    g_list->SetPipelineState(g_pso_out.Get());
    g_list->SetComputeRootDescriptorTable(0, { h0.ptr + 2 * inc });
    g_list->SetComputeRootDescriptorTable(1, { h0.ptr + 3 * inc });
    g_list->Dispatch((g_cap_w + 15) / 16, (g_cap_h + 15) / 16, 1);

    // stage: UAV -> COPY_SOURCE; get backbuffer and copy into it
    nb = 0;
    bars[nb++] = Trans(g_stage_bgra.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g_list->ResourceBarrier(nb, bars);

    g_bb_index = g_swap->GetCurrentBackBufferIndex();
    ComPtr<ID3D12Resource> bb;
    g_swap->GetBuffer(g_bb_index, IID_PPV_ARGS(&bb));

    nb = 0;
    bars[nb++] = Trans(bb.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    g_list->ResourceBarrier(nb, bars);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = bb.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = g_stage_bgra.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    // back to present; also reset shared/stage/nr states for next frame
    nb = 0;
    bars[nb++] = Trans(bb.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    bars[nb++] = Trans(g_shared_bgra.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    bars[nb++] = Trans(g_nr_out.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    bars[nb++] = Trans(g_stage_bgra.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    g_list->ResourceBarrier(nb, bars);

    g_list->Close();
    ID3D12CommandList *cmds[] = { g_list.Get() };
    g_queue->ExecuteCommandLists(1, cmds);
    g_queue->Signal(g_fence.Get(), ++g_fence_value);

    g_swap->Present(1, 0);
    return true;
}

// ---------------------------------------------------------------------------
// entry
// ---------------------------------------------------------------------------
int wmain(int argc, wchar_t **argv)
{
    for (int i = 1; i + 1 < argc; i += 2)
    {
        std::wstring k = argv[i];
        int v = _wtoi(argv[i + 1]);
        if (k == L"--gpu") g_gpu_index = v;
        else if (k == L"--monitor") g_monitor_index = v;
        else if (k == L"--scale") { g_scale = (float)_wtof(argv[i + 1]); }
        else if (k == L"--preset") g_preset = v;
        else if (k == L"--intensity") g_intensity = v;
        else if (k == L"--tone") g_tone = v;
        else if (k == L"--structure") g_structure = v;
        else if (k == L"--skin") g_skin = v;
        else if (k == L"--mask") g_mask = v;
        else if (k == L"--style")
        {
            char buf[64] = {};
            WideCharToMultiByte(CP_UTF8, 0, argv[i + 1], -1, buf, sizeof(buf), nullptr, nullptr);
            g_style = buf;
        }
    }

    if (!SetupDevices()) Fatal("device setup failed");
    if (!SetupCapture()) Fatal("capture setup failed");

    // process at the capture resolution (CopyResource needs matching sizes)
    UINT pw = g_cap_w;
    UINT ph = g_cap_h;

    // create textures at process resolution
    if (!SetupSharedTexture(pw, ph)) Fatal("shared texture failed");
    g_nr_in  = MakeTex(pw, ph, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    g_nr_out = MakeTex(pw, ph, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    g_stage_bgra = MakeTex(pw, ph, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_STATE_COMMON,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!g_shared_bgra || !g_nr_in || !g_nr_out || !g_stage_bgra) Fatal("texture creation failed");

    if (!SetupNGX(pw, ph)) Fatal("NGX setup failed");
    if (!SetupCompute()) Fatal("compute setup failed");

    // window at process resolution
    if (!SetupWindow(pw, ph)) Fatal("window setup failed");

    Log("LIVE pipeline running (%ux%u). Press ESC in the preview window to stop.", pw, ph);

    // warm up: run one frame so reset=1 fires, then steady state
    ULONGLONG t0 = GetTickCount64();
    UINT frames = 0;
    MSG msg;
    while (g_running)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) { g_running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_running) break;

        if (RenderFrame()) ++frames;

        ULONGLONG t1 = GetTickCount64();
        if (t1 - t0 >= 2000)
        {
            double fps = frames * 1000.0 / (t1 - t0);
            Log("%.1f fps (%u frames)", fps, frames);
            frames = 0; t0 = t1;
        }
    }

    if (g_feature && g_nr_release) { if (g_shim_release) g_shim_release((void *)g_nr_release, g_feature); else g_release(g_feature); }
    if (g_shutdown) g_shutdown();
    Log("stopped");
    return 0;
}
