// nr_worker.exe — standalone DLSS-5 NR worker process.
// Opens cross-process shared textures (inherited NT handles), runs the NGX NR feature,
// and loops on sync events:  wait(frame_ready) -> NGX eval -> signal(frame_done).
//
// argv:
//   nr_worker <in_handle> <out_handle> <w> <h> <frame_ready> <frame_done> <shutdown> <style> <preset> <intensity> <tone> <structure>
// handles are decimal uint64 inherited NT handles; floats are plain decimal.

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_5.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <cwchar>

#define NVSDK_CONV __cdecl
typedef int NVSDK_NGX_Result;
static const NVSDK_NGX_Result NGX_SUCCESS = 1;

struct NVSDK_NGX_Handle { unsigned int Id; };
struct NVSDK_NGX_Parameter
{
    virtual void Set(const char*, unsigned long long) = 0;
    virtual void Set(const char*, float) = 0;
    virtual void Set(const char*, double) = 0;
    virtual void Set(const char*, unsigned int) = 0;
    virtual void Set(const char*, int) = 0;
    virtual void Set(const char*, ID3D11Resource*) = 0;
    virtual void Set(const char*, ID3D12Resource*) = 0;
    virtual void Set(const char*, void*) = 0;
    virtual NVSDK_NGX_Result Get(const char*, unsigned long long*) const = 0;
    virtual NVSDK_NGX_Result Get(const char*, float*) const = 0;
    virtual NVSDK_NGX_Result Get(const char*, double*) const = 0;
    virtual NVSDK_NGX_Result Get(const char*, unsigned int*) const = 0;
    virtual NVSDK_NGX_Result Get(const char*, int*) const = 0;
    virtual NVSDK_NGX_Result Get(const char*, ID3D11Resource**) const = 0;
    virtual NVSDK_NGX_Result Get(const char*, ID3D12Resource**) const = 0;
    virtual NVSDK_NGX_Result Get(const char*, void**) const = 0;
    virtual void Reset() = 0;
};
typedef struct NVSDK_NGX_PathListInfo { wchar_t const* const* Path; unsigned int Length; } NVSDK_NGX_PathListInfo;
typedef enum NVSDK_NGX_Logging_Level { NVSDK_NGX_LOGGING_LEVEL_OFF = 0, NVSDK_NGX_LOGGING_LEVEL_ON, NVSDK_NGX_LOGGING_LEVEL_VERBOSE } NVSDK_NGX_Logging_Level;
typedef void(NVSDK_CONV* NVSDK_NGX_AppLogCallback)(const char*, NVSDK_NGX_Logging_Level, int);
typedef struct NVSDK_NGX_LoggingInfo { NVSDK_NGX_Logging_Level LoggingLevel; NVSDK_NGX_AppLogCallback Callback; void* UserData; bool DisableOtherLoggingSinks; } NVSDK_NGX_LoggingInfo;
typedef struct NVSDK_NGX_FeatureCommonInfo_Internal NVSDK_NGX_FeatureCommonInfo_Internal;
typedef struct NVSDK_NGX_FeatureCommonInfo { NVSDK_NGX_PathListInfo PathListInfo; NVSDK_NGX_FeatureCommonInfo_Internal* InternalData; NVSDK_NGX_LoggingInfo LoggingInfo; } NVSDK_NGX_FeatureCommonInfo;

typedef NVSDK_NGX_Result (*PFN_Init_Ext)(unsigned long long, const wchar_t*, ID3D12Device*, int, const void*);
typedef NVSDK_NGX_Result (*PFN_Init_ProjectID)(const char*, int, const char*, const wchar_t*, ID3D12Device*, int, const void*);
typedef NVSDK_NGX_Result (*PFN_ShimInit)(void*, unsigned long long, const wchar_t*, ID3D12Device*, int, const void*);
typedef NVSDK_NGX_Result (*PFN_AllocateParameters)(NVSDK_NGX_Parameter**);
typedef NVSDK_NGX_Result (*PFN_D3D12CreateFeature)(ID3D12GraphicsCommandList*, int, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
typedef NVSDK_NGX_Result (*PFN_D3D12EvaluateFeature)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, void*);
typedef NVSDK_NGX_Result (*PFN_D3D12ReleaseFeature)(NVSDK_NGX_Handle*);
typedef NVSDK_NGX_Result (*PFN_Shutdown)(void);
typedef NVSDK_NGX_Result (*PFN_ShimCreate)(void*, ID3D12GraphicsCommandList*, int, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
typedef NVSDK_NGX_Result (*PFN_ShimEvaluate)(void*, ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, void*);
typedef NVSDK_NGX_Result (*PFN_ShimRelease)(void*, NVSDK_NGX_Handle*);

static const unsigned long long APP_ID = 141959980ULL;
static const char* PROJECT_ID = "53f803cc-a12f-4d69-90d5-19b7599cad19";
static const int FEATURE_NR = 18;
static const wchar_t* DATA_PATH = L"E:\\Work\\nr-video\\";

static HANDLE g_hFrameReady = nullptr, g_hFrameDone = nullptr, g_hShutdown = nullptr;

static void Log(const char* fmt, ...)
{
    FILE* f = nullptr;
    if (fopen_s(&f, "C:\\Program Files\\MPC-BE\\nr_worker_log.txt", "a") == 0 && f) { va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); fputc('\n', f); fclose(f); }
}

static D3D12_RESOURCE_BARRIER Trans(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER t = {}; t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    t.Transition.pResource = r; t.Transition.StateBefore = a; t.Transition.StateAfter = b; t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return t;
}

int main(int argc, char** argv)
{
    Log("[worker] start argc=%d", argc);
    if (argc < 13) { Log("[worker] bad argc"); return 1; }

    HANDLE hIn  = (HANDLE)(uintptr_t)_strtoui64(argv[1], nullptr, 10);
    HANDLE hOut = (HANDLE)(uintptr_t)_strtoui64(argv[2], nullptr, 10);
    UINT w = (UINT)strtoul(argv[3], nullptr, 10);
    UINT h = (UINT)strtoul(argv[4], nullptr, 10);
    g_hFrameReady = (HANDLE)(uintptr_t)_strtoui64(argv[5], nullptr, 10);
    g_hFrameDone  = (HANDLE)(uintptr_t)_strtoui64(argv[6], nullptr, 10);
    g_hShutdown   = (HANDLE)(uintptr_t)_strtoui64(argv[7], nullptr, 10);
    int style = atoi(argv[8]); int preset = atoi(argv[9]);
    float intensity = (float)atof(argv[10]);
    float tone = (float)atof(argv[11]);
    float structure = (float)atof(argv[12]);
    Log("[worker] in=%p out=%p %ux%u ev=%p/%p/%p style=%d preset=%d", hIn, hOut, w, h, g_hFrameReady, g_hFrameDone, g_hShutdown, style, preset);

    // D3D12 device on the RTX 5090
    IDXGIFactory1* f = nullptr; if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) { Log("[worker] no factory"); return 2; }
    ID3D12Device* dev = nullptr; IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d);
        if (wcsstr(d.Description, L"5090")) { D3D12CreateDevice(a, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)); a->Release(); break; }
        a->Release();
    }
    f->Release();
    if (!dev) { Log("[worker] no 5090 device"); return 3; }
    Log("[worker] D3D12 device ready");

    ID3D12Resource* in12 = nullptr, * out12 = nullptr;
    HRESULT hr = dev->OpenSharedHandle(hIn, IID_PPV_ARGS(&in12));
    Log("[worker] OpenSharedHandle(in) hr=0x%08X", (unsigned)hr);
    hr = dev->OpenSharedHandle(hOut, IID_PPV_ARGS(&out12));
    Log("[worker] OpenSharedHandle(out) hr=0x%08X", (unsigned)hr);
    if (!in12 || !out12) { Log("[worker] open failed"); return 4; }

    HMODULE ngx = LoadLibraryW(L"C:\\Program Files\\MPC-BE\\_nvngx.dll");
    HMODULE nr  = LoadLibraryW(L"C:\\Program Files\\MPC-BE\\nvngx_dlssnr.dll");
    HMODULE shim = LoadLibraryW(L"C:\\Program Files\\MPC-BE\\caller\\nvngx.dll");
    if (!ngx || !nr || !shim) { Log("[worker] dll load failed"); return 5; }

    auto init_projectid = (PFN_Init_ProjectID)GetProcAddress(ngx, "NVSDK_NGX_D3D12_Init_ProjectID");
    auto direct_init = (PFN_Init_Ext)GetProcAddress(nr, "NVSDK_NGX_D3D12_Init_Ext");
    auto shim_init = (PFN_ShimInit)GetProcAddress(shim, "DLSSNR_CallInit");
    auto s_alloc = (PFN_AllocateParameters)GetProcAddress(ngx, "NVSDK_NGX_D3D12_AllocateParameters");
    auto nr_create = (PFN_D3D12CreateFeature)GetProcAddress(nr, "NVSDK_NGX_D3D12_CreateFeature");
    auto nr_eval = (PFN_D3D12EvaluateFeature)GetProcAddress(nr, "NVSDK_NGX_D3D12_EvaluateFeature");
    auto nr_release = (PFN_D3D12ReleaseFeature)GetProcAddress(nr, "NVSDK_NGX_D3D12_ReleaseFeature");
    auto shim_create = (PFN_ShimCreate)GetProcAddress(shim, "DLSSNR_CallCreate");
    auto shim_eval = (PFN_ShimEvaluate)GetProcAddress(shim, "DLSSNR_CallEvaluate");
    auto shim_release = (PFN_ShimRelease)GetProcAddress(shim, "DLSSNR_CallRelease");
    if (!init_projectid || !direct_init || !shim_init || !s_alloc || !nr_create || !nr_eval || !shim_create || !shim_eval)
        { Log("[worker] missing exports"); return 6; }

    int inited = 0;
    for (int ver = 0x13; ver <= 0x20 && !inited; ++ver)
        if (init_projectid(PROJECT_ID, 0, "0.1", DATA_PATH, dev, ver, nullptr) == NGX_SUCCESS) { Log("[worker] Init_ProjectID ver=0x%02X ok", ver); inited = 1; }
    if (!inited) { Log("[worker] Init_ProjectID failed"); return 7; }

    const wchar_t* pl[1] = { DATA_PATH };
    NVSDK_NGX_PathListInfo pli{}; pli.Path = pl; pli.Length = 1;
    NVSDK_NGX_FeatureCommonInfo fci{}; fci.PathListInfo = pli; fci.LoggingInfo.LoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
    NVSDK_NGX_Result r = shim_init((void*)direct_init, APP_ID, DATA_PATH, dev, 0x15, &fci);
    Log("[worker] shim Init_Ext -> 0x%08X", (unsigned)r);
    if (r != NGX_SUCCESS) return 8;

    // command infra
    ID3D12CommandQueue* q = nullptr; ID3D12CommandAllocator* ca = nullptr; ID3D12GraphicsCommandList* cl = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca));
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, nullptr, IID_PPV_ARGS(&cl));
    ID3D12Fence* fence = nullptr; dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE fenceEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fenceVal = 0;

    // CreateFeature (list must be OPEN)
    NVSDK_NGX_Parameter* params = nullptr;
    if (s_alloc(&params) != NGX_SUCCESS || !params) { Log("[worker] AllocateParameters failed"); return 9; }
    params->Set("DLSSNR.Width", w); params->Set("DLSSNR.Height", h);
    params->Set("DLSSNR.Enabled", 1u); params->Set("DLSSNR.Reset", 1u);
    params->Set("DLSSNR.Style", style); params->Set("DLSSNR.Hint.Render.Preset", preset);
    params->Set("DLSSNR.Intensity", intensity);
    params->Set("DLSSNR.LocalToneStrength", tone);
    params->Set("DLSSNR.LocalStructureStrength", structure);
    params->Set("DLSSNR.SkinStructureStrength", -1.0f);
    params->Set("DLSSNR.UseAutoMask", 0u);
    params->Set("DLSSNR.DepthInverted", 1);
    params->Set("DLSSNR.ScalingRatio", 1.0f);
    params->Set("DLSSNR.Color", in12);
    params->Set("DLSSNR.Output", out12);
    params->Set("DLSSNR.Backbuffer", out12);
    params->Set("DLSSNR.ColorSubrectBaseX", 0); params->Set("DLSSNR.ColorSubrectBaseY", 0);
    params->Set("DLSSNR.ColorSubrectWidth", (int)w); params->Set("DLSSNR.ColorSubrectHeight", (int)h);
    params->Set("DLSSNR.OutputSubrectBaseX", 0); params->Set("DLSSNR.OutputSubrectBaseY", 0);
    params->Set("DLSSNR.OutputSubrectWidth", (int)w); params->Set("DLSSNR.OutputSubrectHeight", (int)h);

    NVSDK_NGX_Handle* feature = nullptr;
    cl->Reset(ca, nullptr);
    NVSDK_NGX_Result rc = shim_create((void*)nr_create, cl, FEATURE_NR, params, &feature);
    Log("[worker] CreateFeature(18) -> 0x%08X", (unsigned)rc);
    if (rc != NGX_SUCCESS || !feature) { cl->Close(); return 10; }
    cl->Close();
    ID3D12CommandList* cmds[] = { cl };
    q->ExecuteCommandLists(1, cmds);
    q->Signal(fence, ++fenceVal);
    fence->SetEventOnCompletion(fenceVal, fenceEv);
    WaitForSingleObject(fenceEv, 20000);
    Log("[worker] feature created, entering loop");

    // eval loop
    bool first = true;
    unsigned long long frameCount = 0;
    for (;;)
    {
        HANDLE ws[2] = { g_hFrameReady, g_hShutdown };
        DWORD sig = WaitForMultipleObjects(2, ws, FALSE, INFINITE);
        if (sig == WAIT_OBJECT_0 + 1 || sig == WAIT_FAILED) { Log("[worker] shutdown after %llu frames", frameCount); break; }
        if (sig != WAIT_OBJECT_0) continue;

        params->Set("DLSSNR.Reset", first ? 1u : 0u);
        first = false;

        ca->Reset();
        cl->Reset(ca, nullptr);
        D3D12_RESOURCE_BARRIER bars[2];
        bars[0] = Trans(in12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        bars[1] = Trans(out12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cl->ResourceBarrier(2, bars);
        NVSDK_NGX_Result re = shim_eval((void*)nr_eval, cl, feature, params, nullptr);
        if (re != NGX_SUCCESS) Log("[worker] eval -> 0x%08X (frame %llu)", (unsigned)re, frameCount);
        bars[0] = Trans(in12, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        bars[1] = Trans(out12, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        cl->ResourceBarrier(2, bars);
        cl->Close();
        q->ExecuteCommandLists(1, cmds);
        q->Signal(fence, ++fenceVal);
        fence->SetEventOnCompletion(fenceVal, fenceEv);
        WaitForSingleObject(fenceEv, 20000);

        SetEvent(g_hFrameDone);
        ++frameCount;
        if ((frameCount % 100) == 0) Log("[worker] processed %llu frames", frameCount);
    }

    if (feature) { if (shim_release) shim_release((void*)nr_release, feature); else if (nr_release) nr_release(feature); }
    Log("[worker] exit");
    return 0;
}
