// nr_player.cpp - DLSS 5 Neural Rendering VIDEO PLAYER.
// ffmpeg (subprocess, CPU decode) -> raw RGBA pipe -> upload -> cs1(RGBA8->RGBA16F)
// -> NGX NR feature 18 -> cs2(RGBA16F->RGBA8, R/B swap) -> RGBA8 swapchain window.
// All textures RGBA (B8G8R8A8 has NO UAV support in D3D12 — that was the color bug).
//
// Usage: nr_player.exe <video> [--gpu N] [--style ...] [--preset ...] ... [--fast]
// Press ESC in the window to exit.
//
// Build: cl /nologo /EHsc /O2 /MT nr_player.cpp /link /OUT:nr_player.exe
//        d3d12.lib dxgi.lib d3dcompiler.lib user32.lib

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef NVSDK_CONV
#define NVSDK_CONV __cdecl
#endif
#include <windows.h>
#include <mmsystem.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <d3d12.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstdarg>
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>
#include <commdlg.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <stdexcept>
#include "nr_runtime.h"

using Microsoft::WRL::ComPtr;

// forward-declared (we never touch D3D11; only the NGX vtable signature needs it)
struct ID3D11Resource;

// ---------------------------------------------------------------------------
// NGX interface (inline vtable, mirrors NVIDIA's layout)
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
static const UINT ID_FILE_OPEN = 1001;
static const UINT ID_FILE_EXIT = 1002;
static const UINT ID_HELP_SHORTCUTS = 1003;
static const UINT ID_THEME_LIGHT = 1010;
static const UINT ID_THEME_DARK = 1011;
static const wchar_t HOTKEY_HELP_TEXT[] =
    L"Ctrl+O\tOpen a video\r\n"
    L"Space\tPause or resume\r\n"
    L"Left / Right\tPrevious or next frame\r\n"
    L"S\tToggle split comparison\r\n"
    L"D\tToggle DLSS 5\r\n"
    L"M\tCycle DLSS 5 model\r\n"
    L"F11\tToggle fullscreen\r\n"
    L"Esc\tExit fullscreen or close the player";

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

static ComPtr<IDXGIFactory1>            g_factory;
static ComPtr<IDXGIFactory2>            g_factory2;
static ComPtr<ID3D12Device>             g_dev;
static ComPtr<ID3D12CommandQueue>       g_queue;
static ComPtr<ID3D12GraphicsCommandList> g_list;
static const int                        FRAMES_IN_FLIGHT = 2;
static ComPtr<ID3D12CommandAllocator>   g_cmd_alloc[FRAMES_IN_FLIGHT];
static ComPtr<ID3D12Fence>              g_fence[FRAMES_IN_FLIGHT];
static UINT64                           g_fence_value[FRAMES_IN_FLIGHT];
static UINT                             g_frame_slot = 0;
static ComPtr<ID3D12Fence>              g_sync_fence;
static UINT64                           g_sync_value = 0;

static NVSDK_NGX_Parameter *g_params = nullptr;
static NVSDK_NGX_Handle    *g_feature = nullptr;
static bool                 g_ngx_initialized = false;

static ComPtr<ID3D12Resource> g_y_tex;       // R8_UNORM Y plane (W x H)
static ComPtr<ID3D12Resource> g_uv_tex;      // R8G8_UNORM UV plane (W/2 x H/2)
static ComPtr<ID3D12Resource> g_staging;     // UPLOAD heap: Y (padded) + UV (padded)
static ComPtr<ID3D12Resource> g_nr_in;       // R16G16B16A16_FLOAT NR input
static ComPtr<ID3D12Resource> g_nr_out;      // R16G16B16A16_FLOAT NR output
static ComPtr<ID3D12Resource> g_stage_rgba;  // R8G8B8A8 NR output (cs2 UAV)
static ComPtr<ID3D12Resource> g_orig_rgba;   // R8G8B8A8 "original" (side-by-side left)
static ComPtr<ID3D12DescriptorHeap> g_cbv_heap;

static ComPtr<ID3D12RootSignature> g_rs;
static ComPtr<ID3D12PipelineState> g_pso_in;   // RGBA8 -> RGBA16F
static ComPtr<ID3D12PipelineState> g_pso_out;  // RGBA16F -> RGBA8 (R/B swap)

static ComPtr<IDXGISwapChain3> g_swap;
static HWND g_hwnd = nullptr;
static HWND g_video_hwnd = nullptr, g_pause_button = nullptr;
static HWND g_split_button = nullptr, g_dlss_button = nullptr, g_model_button = nullptr;
static HWND g_prev_frame_button = nullptr, g_next_frame_button = nullptr;
static HWND g_volume_slider = nullptr, g_volume_label = nullptr, g_mute_button = nullptr;
static HWND g_fullscreen_button = nullptr;
static bool g_gui = false, g_media_loaded = false;
static std::wstring g_open_path;
static HMODULE g_core_module = nullptr, g_nr_module = nullptr, g_caller_module = nullptr;
static NRRuntime g_runtime = NRRuntime::Unsupported;
static bool g_nr_available = true;
static std::wstring g_runtime_directory;
static bool g_paused = false;
static SRWLOCK g_audio_lock = SRWLOCK_INIT;
static HWAVEOUT g_wave_out = nullptr;
static int g_volume = 100;
static bool g_muted = false;
static bool g_fullscreen = false;
static DWORD g_windowed_style = 0;
static WINDOWPLACEMENT g_windowed_placement = {sizeof(WINDOWPLACEMENT)};
static HMENU g_windowed_menu = nullptr;
static HMENU g_theme_menu = nullptr;
static HFONT g_ui_font = nullptr;
static HBRUSH g_background_brush = nullptr;
static HBRUSH g_video_brush = nullptr;
static bool g_dark_theme = true;
static HWND g_hover_button = nullptr;

static UINT g_vid_w = 0, g_vid_h = 0;
static UINT g_row_pitch = 0;
static double g_fps = 30.0;
static int  g_gpu_index = -1;
static bool g_cuda_decode = false;
static std::string g_style = "natural";
static int  g_preset = 3, g_intensity = 1, g_tone = 1, g_structure = 1, g_skin = -1, g_mask = 0;
static bool g_fast = false;
static bool g_side = false;  // default: single DLSS 5 view
static bool g_nr_enabled = true;
static bool g_nr_reset = true;
static bool g_refresh_view = false;
static UINT64 g_frame_index = 0;
static std::wstring g_dump_path;
static std::wstring g_output;      // offline output file (empty = playback only)
static int g_crf = 18;             // x264 CRF for offline output
static HANDLE g_enc_write = nullptr, g_enc_proc = nullptr;
static UINT g_last_slot = 0;
static ComPtr<ID3D12Resource> g_readback;

static HANDLE g_audio_read = nullptr;
static HANDLE g_audio_thread = nullptr;
static volatile bool g_audio_done = false;

static void LayoutControls(HWND hwnd);

// seek / progress bar state
static double g_duration = 0.0;      // video duration (seconds)
static double g_base_time = 0.0;     // seek offset (seconds)
static double g_current_time = 0.0;  // timestamp of the displayed frame
static volatile double g_seek_to = 0.0;
static volatile bool g_seek_requested = false;
static bool g_dragging = false;
static ULONGLONG g_last_seek_tick = 0;
static HWND g_trackbar = nullptr;
static HANDLE g_ffread = nullptr, g_ffproc = nullptr, g_afproc = nullptr;

// timing instrumentation (ms)
static double g_read_ms = 0, g_wait_ms = 0, g_upload_ms = 0;

static volatile bool g_running = true;

static int StyleValue()
{
    if (g_style == "default") return 0;
    if (g_style == "natural") return 1;
    if (g_style == "cinematic") return 2;
    return atoi(g_style.c_str());
}

static const wchar_t *StyleName()
{
    switch (StyleValue())
    {
    case 0: return L"Default";
    case 1: return L"Natural";
    case 2: return L"Cinematic";
    default: return L"Custom";
    }
}

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
    char message[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(message, sizeof(message), fmt, ap); va_end(ap);
    Log("%s", message);
    throw std::runtime_error(message);
}

static D3D12_RESOURCE_BARRIER Trans(ID3D12Resource *res, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER x = {};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = res;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    return x;
}

static void WaitFence(ID3D12Fence *f, UINT64 v)
{
    if (f->GetCompletedValue() < v)
    {
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        f->SetEventOnCompletion(v, ev);
        WaitForSingleObject(ev, 20000);
        CloseHandle(ev);
    }
}

static void ExecuteAndWait()
{
    g_list->Close();
    ID3D12CommandList *cmds[] = { g_list.Get() };
    g_queue->ExecuteCommandLists(1, cmds);
    g_queue->Signal(g_sync_fence.Get(), ++g_sync_value);
    WaitFence(g_sync_fence.Get(), g_sync_value);
}

// ---------------------------------------------------------------------------
// D3D12 device (--gpu N)
// ---------------------------------------------------------------------------
static bool CreateDevice()
{
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&g_factory)))) return false;
    g_factory.As(&g_factory2);
    ComPtr<IDXGIAdapter1> adapter;
    int sel = -1;
    for (UINT i = 0; g_factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        Log("GPU[%d]: %ls (vendor=0x%04X VRAM=%u MB)", i, desc.Description, desc.VendorId, (unsigned)(desc.DedicatedVideoMemory >> 20));
        if (g_gpu_index < 0) { if (sel < 0 && desc.VendorId == 0x10DE) sel = (int)i; }
        else if (g_gpu_index == (int)i) sel = (int)i;
        adapter.Reset();
    }
    if (sel < 0) sel = 0;
    ComPtr<IDXGIAdapter1> chosen;
    g_factory->EnumAdapters1(sel, &chosen);
    if (!chosen) { Log("FAIL: selected GPU is not available"); return false; }
    DXGI_ADAPTER_DESC1 selectedDesc = {};
    chosen->GetDesc1(&selectedDesc);
    g_cuda_decode = selectedDesc.VendorId == 0x10DE;
    g_runtime = RuntimeForAdapter(selectedDesc.VendorId, selectedDesc.Description);
    g_nr_available = g_runtime != NRRuntime::Unsupported;
    if (!g_nr_available)
    {
        g_nr_enabled = false;
        g_side = false;
        Log("DLSS 5 NR is unavailable on this adapter; continuing with original video rendering");
    }
    if (FAILED(D3D12CreateDevice(chosen.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_dev))))
        { Log("FAIL: D3D12CreateDevice"); return false; }
    Log("D3D12 adapter: index %d", sel);

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue)))) return false;
    for (int i = 0; i < FRAMES_IN_FLIGHT; ++i)
    {
        if (FAILED(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_cmd_alloc[i])))) return false;
        if (FAILED(g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence[i])))) return false;
    }
    if (FAILED(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_cmd_alloc[0].Get(), nullptr, IID_PPV_ARGS(&g_list)))) return false;
    if (FAILED(g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_sync_fence)))) return false;
    return true;
}

// ---------------------------------------------------------------------------
// textures
// ---------------------------------------------------------------------------
static ComPtr<ID3D12Resource> MakeTex(UINT w, UINT h, DXGI_FORMAT fmt, D3D12_RESOURCE_STATES st, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1;
    d.MipLevels = 1; d.Format = fmt; d.SampleDesc.Count = 1;
    d.Flags = flags;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> r;
    if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

// ---------------------------------------------------------------------------
// NGX init (Init_ProjectID + shim + snippet, feature 18)
// ---------------------------------------------------------------------------
static bool SetupNGX(UINT w, UINT h)
{
    HMODULE ngx = g_core_module ? g_core_module : LoadLibraryW(L"_nvngx.dll");
    if (!ngx)
    {
        WIN32_FIND_DATAW fd;
        wchar_t pat[MAX_PATH];
        swprintf_s(pat, L"%ls\\FileRepository\\nv_dispi.inf_*\\_nvngx.dll", L"C:\\Windows\\System32\\DriverStore");
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
    g_core_module = ngx;
    g_init_ext       = (PFN_Init_Ext)GetProcAddress(ngx, "NVSDK_NGX_D3D12_Init_Ext");
    g_init_projectid = (PFN_Init_ProjectID)GetProcAddress(ngx, "NVSDK_NGX_D3D12_Init_ProjectID");
    g_alloc          = (PFN_AllocateParameters)GetProcAddress(ngx, "NVSDK_NGX_D3D12_AllocateParameters");
    g_create         = (PFN_D3D12CreateFeature)GetProcAddress(ngx, "NVSDK_NGX_D3D12_CreateFeature");
    g_eval           = (PFN_D3D12EvaluateFeature)GetProcAddress(ngx, "NVSDK_NGX_D3D12_EvaluateFeature");
    g_release        = (PFN_D3D12ReleaseFeature)GetProcAddress(ngx, "NVSDK_NGX_D3D12_ReleaseFeature");
    g_shutdown       = (PFN_Shutdown)GetProcAddress(ngx, "NVSDK_NGX_D3D12_Shutdown");

    wchar_t executable[32768];
    DWORD executableLength = GetModuleFileNameW(nullptr, executable, 32768);
    if (!executableLength || executableLength >= 32768) return false;
    std::wstring base(executable, executableLength);
    base.resize(base.find_last_of(L"\\/") + 1);
    std::wstring nrPath = base + RuntimeRelativePath(g_runtime);
    g_runtime_directory = nrPath.substr(0, nrPath.find_last_of(L"\\/"));
    Log("NR runtime: %s (%ls)", g_runtime == NRRuntime::RTX40 ? "RTX40 community patch" : "RTX50 original", nrPath.c_str());
    HMODULE nr = g_nr_module ? g_nr_module : LoadLibraryW(nrPath.c_str());
    if (!nr) { Log("FAIL: cannot load %ls (Windows error %lu)", nrPath.c_str(), GetLastError()); return false; }
    g_nr_module = nr;
    g_direct_init = (PFN_Init_Ext)GetProcAddress(nr, "NVSDK_NGX_D3D12_Init_Ext");
    g_nr_create   = (PFN_D3D12CreateFeature)GetProcAddress(nr, "NVSDK_NGX_D3D12_CreateFeature");
    g_nr_eval     = (PFN_D3D12EvaluateFeature)GetProcAddress(nr, "NVSDK_NGX_D3D12_EvaluateFeature");
    g_nr_release  = (PFN_D3D12ReleaseFeature)GetProcAddress(nr, "NVSDK_NGX_D3D12_ReleaseFeature");

    HMODULE shim = g_caller_module ? g_caller_module : LoadLibraryW(L"caller\\nvngx.dll");
    g_caller_module = shim;
    if (shim)
    {
        g_shim_init    = (PFN_ShimInit)GetProcAddress(shim, "DLSSNR_CallInit");
        g_shim_create  = (PFN_ShimCreate)GetProcAddress(shim, "DLSSNR_CallCreate");
        g_shim_eval    = (PFN_ShimEvaluate)GetProcAddress(shim, "DLSSNR_CallEvaluate");
        g_shim_release = (PFN_ShimRelease)GetProcAddress(shim, "DLSSNR_CallRelease");
    }
    if (!g_init_projectid || !g_alloc || !g_nr_create || !g_nr_eval || !g_nr_release)
        { Log("FAIL: NGX entry points missing"); return false; }

    wchar_t data_path[MAX_PATH] = L".";
    GetCurrentDirectoryW(MAX_PATH, data_path);
    const unsigned long long APP_ID = 141959980ULL;
    const wchar_t *path_list[2] = { g_runtime_directory.c_str(), data_path };
    NVSDK_NGX_PathListInfo pli = {}; pli.Path = path_list; pli.Length = 2;
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
    g_ngx_initialized = true;

    if (g_direct_init && g_shim_init)
    {
        NVSDK_NGX_Result r = g_shim_init((void *)g_direct_init, APP_ID, data_path, g_dev.Get(), 0x15, &fci);
        Log("snippet Init_Ext (via shim) -> 0x%08X", (unsigned)r);
    }

    NVSDK_NGX_Result ra = g_alloc(&g_params);
    if (ra != NGX_SUCCESS || !g_params) { Log("FAIL: AllocateParameters"); return false; }

    int style_int = StyleValue();

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
    ExecuteAndWait();
    return true;
}

static void ReleaseNGXObjects()
{
    if (g_feature && g_nr_release)
    {
        if (g_shim_release) g_shim_release((void *)g_nr_release, g_feature);
        else if (g_release) g_release(g_feature);
    }
    g_feature = nullptr;

    if (g_params && g_core_module)
    {
        auto destroy = (NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter *))GetProcAddress(
            g_core_module, "NVSDK_NGX_D3D12_DestroyParameters");
        if (destroy) destroy(g_params);
    }
    g_params = nullptr;

    if (g_ngx_initialized && g_shutdown) g_shutdown();
    g_ngx_initialized = false;
}

// ---------------------------------------------------------------------------
// compute shaders + descriptor heap
// ---------------------------------------------------------------------------
static bool SetupCompute()
{
    // root signature: 4 descriptor tables (SRV t0, SRV t1, UAV u0, UAV u1)
    D3D12_DESCRIPTOR_RANGE ranges[4] = {};
    for (int i = 0; i < 4; ++i)
    {
        ranges[i].RangeType = (i < 2) ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[i].NumDescriptors = 1;
        ranges[i].BaseShaderRegister = (i < 2) ? i : (i - 2);
    }
    D3D12_ROOT_PARAMETER rp[4] = {};
    for (int i = 0; i < 4; ++i)
    {
        rp[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[i].DescriptorTable.NumDescriptorRanges = 1;
        rp[i].DescriptorTable.pDescriptorRanges = &ranges[i];
    }

    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 4; rsd.pParameters = rp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
        { Log("FAIL: serialize root sig"); return false; }
    if (FAILED(g_dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_rs))))
        { Log("FAIL: CreateRootSignature"); return false; }

    // cs0: NV12 -> RGBA16F (NR input). BT.709 limited-range YUV -> RGB.
    const char *src1 =
        "Texture2D<float> Y : register(t0);\n"
        "Texture2D<float2> UV : register(t1);\n"
        "RWTexture2D<float4> dst : register(u0);\n"
        "[numthreads(16,16,1)]\n"
        "void CSMain(uint3 id : SV_DispatchThreadID) {\n"
        "  float y = Y[id.xy];\n"
        "  float2 uv = UV[uint2(id.x >> 1, id.y >> 1)];\n"
        "  float yv = (y - 16.0f/255.0f) * (255.0f/219.0f);\n"
        "  float u  = (uv.x - 128.0f/255.0f) * (255.0f/224.0f);\n"
        "  float v  = (uv.y - 128.0f/255.0f) * (255.0f/224.0f);\n"
        "  float r = yv + 1.5748f * v;\n"
        "  float g = yv - 0.1873f * u - 0.4681f * v;\n"
        "  float b = yv + 1.8556f * u;\n"
        "  dst[id.xy] = float4(saturate(r), saturate(g), saturate(b), 1.0f);\n"
        "}\n";
    // cs2: RGBA16F -> RGBA8 (NR output, and also "original" for side-by-side).
    const char *src2 =
        "Texture2D<float4> src : register(t0);\n"
        "RWTexture2D<unorm float4> dst : register(u0);\n"
        "[numthreads(16,16,1)]\n"
        "void CSMain(uint3 id : SV_DispatchThreadID) {\n"
        "  float4 c = src[id.xy];\n"
        "  dst[id.xy] = float4(c.rgb, 1.0f);\n"
        "}\n";

    D3D12_COMPUTE_PIPELINE_STATE_DESC ps = {};
    ps.pRootSignature = g_rs.Get();

    ComPtr<ID3DBlob> b1, e1, b2, e2;
    if (FAILED(D3DCompile(src1, strlen(src1), "cs0", nullptr, nullptr, "CSMain", "cs_5_0", 0, 0, &b1, &e1)))
        { Log("FAIL: compile cs0: %s", e1 ? (char *)e1->GetBufferPointer() : "?"); return false; }
    if (FAILED(D3DCompile(src2, strlen(src2), "cs2", nullptr, nullptr, "CSMain", "cs_5_0", 0, 0, &b2, &e2)))
        { Log("FAIL: compile cs2: %s", e2 ? (char *)e2->GetBufferPointer() : "?"); return false; }

    ps.CS = { b1->GetBufferPointer(), b1->GetBufferSize() };
    if (FAILED(g_dev->CreateComputePipelineState(&ps, IID_PPV_ARGS(&g_pso_in)))) return false;
    ps.CS = { b2->GetBufferPointer(), b2->GetBufferSize() };
    if (FAILED(g_dev->CreateComputePipelineState(&ps, IID_PPV_ARGS(&g_pso_out)))) return false;

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 7; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_cbv_heap)))) return false;
    UINT inc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE base = g_cbv_heap->GetCPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    // [0] SRV Y (R8)
    srv.Format = DXGI_FORMAT_R8_UNORM;
    g_dev->CreateShaderResourceView(g_y_tex.Get(), &srv, { base.ptr });
    // [1] SRV UV (R8G8)
    srv.Format = DXGI_FORMAT_R8G8_UNORM;
    g_dev->CreateShaderResourceView(g_uv_tex.Get(), &srv, { base.ptr + inc });
    // [2] UAV nr_in (RGBA16F)
    uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    g_dev->CreateUnorderedAccessView(g_nr_in.Get(), nullptr, &uav, { base.ptr + 2 * inc });
    // [3] SRV nr_out (RGBA16F)
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    g_dev->CreateShaderResourceView(g_nr_out.Get(), &srv, { base.ptr + 3 * inc });
    // [4] UAV stage (RGBA8)
    uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    g_dev->CreateUnorderedAccessView(g_stage_rgba.Get(), nullptr, &uav, { base.ptr + 4 * inc });
    // [5] SRV nr_in (RGBA16F) — for the "original" pass
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    g_dev->CreateShaderResourceView(g_nr_in.Get(), &srv, { base.ptr + 5 * inc });
    // [6] UAV orig (RGBA8)
    uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    g_dev->CreateUnorderedAccessView(g_orig_rgba.Get(), nullptr, &uav, { base.ptr + 6 * inc });

    return true;
}

// ---------------------------------------------------------------------------
// window + swapchain (RGBA8)
// ---------------------------------------------------------------------------
struct ThemeColors
{
    COLORREF background, surface, button, buttonHover, border;
    COLORREF text, mutedText, accent, accentHover;
};

static ThemeColors CurrentThemeColors()
{
    if (g_dark_theme) return {
        RGB(18, 20, 24), RGB(27, 30, 36), RGB(39, 43, 51), RGB(50, 55, 65),
        RGB(66, 72, 84), RGB(241, 244, 248), RGB(151, 158, 170),
        RGB(75, 113, 255), RGB(91, 127, 255)};
    return {
        RGB(242, 244, 248), RGB(255, 255, 255), RGB(255, 255, 255), RGB(244, 247, 252),
        RGB(205, 210, 220), RGB(30, 35, 44), RGB(102, 110, 124),
        RGB(51, 94, 234), RGB(67, 108, 241)};
}

static bool IsButtonActive(HWND button)
{
    return (button == g_pause_button && g_paused) ||
        (button == g_split_button && g_side) ||
        (button == g_dlss_button && g_nr_available && (g_side || g_nr_enabled)) ||
        (button == g_mute_button && g_muted);
}

static void DrawModernButton(const DRAWITEMSTRUCT *draw)
{
    ThemeColors colors = CurrentThemeColors();
    bool enabled = !(draw->itemState & ODS_DISABLED);
    bool pressed = draw->itemState & ODS_SELECTED;
    bool active = IsButtonActive(draw->hwndItem);
    bool hovered = draw->hwndItem == g_hover_button;
    COLORREF fill = active ? (hovered ? colors.accentHover : colors.accent) :
        (hovered ? colors.buttonHover : colors.button);
    if (pressed) fill = active ? colors.accentHover : colors.border;

    HBRUSH background = CreateSolidBrush(colors.background);
    FillRect(draw->hDC, &draw->rcItem, background);
    DeleteObject(background);

    RECT face = draw->rcItem;
    InflateRect(&face, -1, -1);
    HBRUSH faceBrush = CreateSolidBrush(fill);
    HPEN borderPen = CreatePen(PS_SOLID, 1, active ? colors.accent : colors.border);
    HGDIOBJ oldBrush = SelectObject(draw->hDC, faceBrush);
    HGDIOBJ oldPen = SelectObject(draw->hDC, borderPen);
    RoundRect(draw->hDC, face.left, face.top, face.right, face.bottom, 10, 10);
    SelectObject(draw->hDC, oldPen);
    SelectObject(draw->hDC, oldBrush);
    DeleteObject(borderPen);
    DeleteObject(faceBrush);

    wchar_t text[128] = {};
    GetWindowTextW(draw->hwndItem, text, 128);
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, enabled ? (active ? RGB(255, 255, 255) : colors.text) : colors.mutedText);
    HFONT font = (HFONT)SendMessageW(draw->hwndItem, WM_GETFONT, 0, 0);
    HGDIOBJ oldFont = font ? SelectObject(draw->hDC, font) : nullptr;
    RECT label = face;
    if (pressed) OffsetRect(&label, 0, 1);
    DrawTextW(draw->hDC, text, -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (oldFont) SelectObject(draw->hDC, oldFont);

    if ((draw->itemState & ODS_FOCUS) && enabled) {
        RECT focus = face;
        InflateRect(&focus, -3, -3);
        HPEN focusPen = CreatePen(PS_DOT, 1, active ? RGB(255, 255, 255) : colors.accent);
        oldPen = SelectObject(draw->hDC, focusPen);
        oldBrush = SelectObject(draw->hDC, GetStockObject(NULL_BRUSH));
        RoundRect(draw->hDC, focus.left, focus.top, focus.right, focus.bottom, 8, 8);
        SelectObject(draw->hDC, oldBrush);
        SelectObject(draw->hDC, oldPen);
        DeleteObject(focusPen);
    }
}

static void MeasureModernMenu(MEASUREITEMSTRUCT *measure)
{
    const wchar_t *text = (const wchar_t *)measure->itemData;
    if (!text) {
        measure->itemWidth = 12;
        measure->itemHeight = 9;
        return;
    }
    HDC dc = GetDC(g_hwnd);
    HGDIOBJ oldFont = g_ui_font ? SelectObject(dc, g_ui_font) : nullptr;
    SIZE size = {};
    GetTextExtentPoint32W(dc, text, (int)wcslen(text), &size);
    if (oldFont) SelectObject(dc, oldFont);
    ReleaseDC(g_hwnd, dc);
    measure->itemWidth = size.cx + (wcschr(text, L'\t') ? 54 : 24);
    measure->itemHeight = 27;
}

static void DrawModernMenu(const DRAWITEMSTRUCT *draw)
{
    ThemeColors colors = CurrentThemeColors();
    const wchar_t *text = (const wchar_t *)draw->itemData;
    bool selected = draw->itemState & ODS_SELECTED;
    HBRUSH brush = CreateSolidBrush(selected ? colors.buttonHover : colors.background);
    FillRect(draw->hDC, &draw->rcItem, brush);
    DeleteObject(brush);
    if (!text) {
        RECT line = draw->rcItem;
        int y = (line.top + line.bottom) / 2;
        HPEN pen = CreatePen(PS_SOLID, 1, colors.border);
        HGDIOBJ oldPen = SelectObject(draw->hDC, pen);
        MoveToEx(draw->hDC, line.left + 10, y, nullptr);
        LineTo(draw->hDC, line.right - 8, y);
        SelectObject(draw->hDC, oldPen);
        DeleteObject(pen);
        return;
    }

    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, (draw->itemState & ODS_DISABLED) ? colors.mutedText : colors.text);
    HGDIOBJ oldFont = g_ui_font ? SelectObject(draw->hDC, g_ui_font) : nullptr;
    RECT label = draw->rcItem;
    label.left += 12;
    label.right -= 12;
    const wchar_t *tab = wcschr(text, L'\t');
    if (tab) {
        std::wstring left(text, tab);
        UINT flags = DT_VCENTER | DT_SINGLELINE |
            ((draw->itemState & ODS_NOACCEL) ? DT_HIDEPREFIX : 0);
        DrawTextW(draw->hDC, left.c_str(), -1, &label, DT_LEFT | flags);
        DrawTextW(draw->hDC, tab + 1, -1, &label, DT_RIGHT | flags);
    } else {
        if (draw->itemState & ODS_CHECKED) {
            HBRUSH dot = CreateSolidBrush(colors.accent);
            HGDIOBJ oldBrush = SelectObject(draw->hDC, dot);
            HGDIOBJ oldPen = SelectObject(draw->hDC, GetStockObject(NULL_PEN));
            Ellipse(draw->hDC, label.left, label.top + 9, label.left + 8, label.top + 17);
            SelectObject(draw->hDC, oldPen);
            SelectObject(draw->hDC, oldBrush);
            DeleteObject(dot);
            label.left += 14;
        }
        UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE |
            ((draw->itemState & ODS_NOACCEL) ? DT_HIDEPREFIX : 0);
        DrawTextW(draw->hDC, text, -1, &label, flags);
    }
    if (oldFont) SelectObject(draw->hDC, oldFont);
}

static void SetMenuBackgrounds(HMENU menu)
{
    if (!menu || !g_background_brush) return;
    MENUINFO info = {sizeof(info), MIM_BACKGROUND};
    info.hbrBack = g_background_brush;
    SetMenuInfo(menu, &info);
    int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i)
        SetMenuBackgrounds(GetSubMenu(menu, i));
}

static LRESULT CALLBACK ModernButtonProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp,
                                         UINT_PTR id, DWORD_PTR)
{
    switch (message) {
    case WM_MOUSEMOVE:
        if (g_hover_button != hwnd) {
            HWND previous = g_hover_button;
            g_hover_button = hwnd;
            if (previous) InvalidateRect(previous, nullptr, TRUE);
            InvalidateRect(hwnd, nullptr, TRUE);
            TRACKMOUSEEVENT tracking = {sizeof(tracking), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tracking);
        }
        break;
    case WM_MOUSELEAVE:
        if (g_hover_button == hwnd) g_hover_button = nullptr;
        InvalidateRect(hwnd, nullptr, TRUE);
        break;
    case WM_ENABLE:
    case WM_SETTEXT:
        InvalidateRect(hwnd, nullptr, TRUE);
        break;
    case WM_NCDESTROY:
        if (g_hover_button == hwnd) g_hover_button = nullptr;
        RemoveWindowSubclass(hwnd, ModernButtonProc, id);
        break;
    }
    return DefSubclassProc(hwnd, message, wp, lp);
}

static void ApplyTheme(bool dark)
{
    g_dark_theme = dark;
    ThemeColors colors = CurrentThemeColors();
    if (g_background_brush) DeleteObject(g_background_brush);
    if (g_video_brush) DeleteObject(g_video_brush);
    g_background_brush = CreateSolidBrush(colors.background);
    g_video_brush = CreateSolidBrush(RGB(0, 0, 0));
    if (g_theme_menu) CheckMenuRadioItem(g_theme_menu, ID_THEME_LIGHT, ID_THEME_DARK,
        dark ? ID_THEME_DARK : ID_THEME_LIGHT, MF_BYCOMMAND);
    if (g_hwnd) {
        BOOL darkTitleBar = dark;
        if (FAILED(DwmSetWindowAttribute(g_hwnd, 20, &darkTitleBar, sizeof(darkTitleBar))))
            DwmSetWindowAttribute(g_hwnd, 19, &darkTitleBar, sizeof(darkTitleBar));
        SetWindowTheme(g_hwnd, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        SetMenuBackgrounds(GetMenu(g_hwnd));
    }
    HWND themed[] = {g_volume_slider, g_trackbar};
    for (HWND control : themed) if (control)
        SetWindowTheme(control, L"", L"");
    if (g_hwnd) {
        SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
            SWP_NOOWNERZORDER | SWP_NOACTIVATE);
        DrawMenuBar(g_hwnd);
        RedrawWindow(g_hwnd, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
        for (HWND control : themed) if (control)
            RedrawWindow(control, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    }
}

static void UpdateModeTitle()
{
    const wchar_t *mode = !g_nr_available ? L"Original (DLSS 5 unavailable)" :
                         (g_side ? L"Original | DLSS 5" :
                         (g_nr_enabled ? L"DLSS 5 ON" : L"DLSS 5 OFF - Original"));
    std::wstring title = L"DLSS 5 NR Player  —  ";
    title += mode;
    SetWindowTextW(g_hwnd, title.c_str());
    SetWindowTextW(g_split_button, !g_nr_available ? L"Split: N/A" : (g_side ? L"Split: ON" : L"Split: OFF"));
    SendMessageW(g_split_button, BM_SETCHECK, g_side ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(g_dlss_button, !g_nr_available ? L"DLSS 5: N/A" : ((g_side || g_nr_enabled) ? L"DLSS 5: ON" : L"DLSS 5: OFF"));
    SendMessageW(g_dlss_button, BM_SETCHECK, (g_side || g_nr_enabled) ? BST_CHECKED : BST_UNCHECKED, 0);
    std::wstring modelText = g_nr_available ? L"Model: " : L"Model: N/A";
    if (g_nr_available) modelText += StyleName();
    SetWindowTextW(g_model_button, modelText.c_str());
    EnableWindow(g_split_button, g_nr_available);
    EnableWindow(g_dlss_button, g_nr_available && !g_side);
    EnableWindow(g_model_button, g_nr_available);
    EnableWindow(g_pause_button, g_media_loaded);
    EnableWindow(g_prev_frame_button, g_media_loaded);
    EnableWindow(g_next_frame_button, g_media_loaded);
    EnableWindow(g_trackbar, g_media_loaded);
    EnableWindow(g_fullscreen_button, g_media_loaded);
    if (!g_media_loaded) SetWindowTextW(g_hwnd, L"DLSS 5 NR Player - Open a video");
    Log("view: %s", !g_nr_available ? "Original (DLSS 5 unavailable)" :
         (g_side ? "Original | DLSS 5" : (g_nr_enabled ? "DLSS 5 ON" : "DLSS 5 OFF - Original")));
}

static void ToggleComparison()
{
    if (!g_nr_available) return;
    if (!g_swap) { g_side = !g_side; UpdateModeTitle(); return; }
    for (UINT i = 0; i < FRAMES_IN_FLIGHT; ++i)
        WaitFence(g_fence[i].Get(), g_fence_value[i]);
    bool side = !g_side;
    HRESULT hr = g_swap->ResizeBuffers(0, side ? g_vid_w * 2 : g_vid_w,
                                       g_vid_h, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) { Log("FAIL: resize comparison buffers -> 0x%08X", (unsigned)hr); return; }
    g_side = side;
    g_nr_reset = true;
    g_refresh_view = true;
    LayoutControls(g_hwnd);
    UpdateModeTitle();
}

static void ToggleNR()
{
    if (!g_nr_available) return;
    if (g_side) return; // comparison always shows original alongside NR
    g_nr_enabled = !g_nr_enabled;
    g_nr_reset = true;
    g_refresh_view = true;
    UpdateModeTitle();
}

static void CycleModel()
{
    if (!g_nr_available) return;
    int next = StyleValue() + 1;
    if (next < 0 || next > 2) next = 0;
    static const char *styles[] = { "default", "natural", "cinematic" };
    g_style = styles[next];
    if (g_params) g_params->Set("DLSSNR.Style", next);
    g_nr_reset = true;
    g_refresh_view = true;
    UpdateModeTitle();
    Log("DLSS 5 model: %s", g_style.c_str());
}

// Caller holds g_audio_lock so changing volume cannot race device replacement.
static void ApplyVolumeLocked()
{
    if (!g_wave_out) return;
    DWORD level = g_muted ? 0 : (DWORD)(g_volume * 65535 / 100);
    waveOutSetVolume(g_wave_out, level | (level << 16));
}

static void UpdateVolumeControls()
{
    wchar_t label[32];
    swprintf_s(label, L"Volume: %d%%", g_volume);
    SetWindowTextW(g_volume_label, label);
    SetWindowTextW(g_mute_button, g_muted ? L"Unmute" : L"Mute");
    SendMessageW(g_mute_button, BM_SETCHECK, g_muted ? BST_CHECKED : BST_UNCHECKED, 0);
}

static void SetVolume(int volume)
{
    AcquireSRWLockExclusive(&g_audio_lock);
    g_volume = std::max(0, std::min(100, volume));
    ApplyVolumeLocked();
    ReleaseSRWLockExclusive(&g_audio_lock);
    UpdateVolumeControls();
}

static void ToggleMute()
{
    AcquireSRWLockExclusive(&g_audio_lock);
    g_muted = !g_muted;
    ApplyVolumeLocked();
    ReleaseSRWLockExclusive(&g_audio_lock);
    UpdateVolumeControls();
}

static void TogglePause()
{
    if (!g_media_loaded) return;
    AcquireSRWLockExclusive(&g_audio_lock);
    g_paused = !g_paused;
    if (g_wave_out) {
        if (g_paused) waveOutPause(g_wave_out);
        else waveOutRestart(g_wave_out);
    }
    ReleaseSRWLockExclusive(&g_audio_lock);
    SetWindowTextW(g_pause_button, g_paused ? L"Resume" : L"Pause");
    Log("%s at frame %llu", g_paused ? "paused" : "resumed", (unsigned long long)g_frame_index);
}

static void RequestFrameStep(int direction)
{
    if (!g_media_loaded || g_fps <= 0.0 || direction == 0) return;
    if (!g_paused) TogglePause();

    double origin = g_seek_requested ? g_seek_to : g_current_time;
    double lastFrame = g_duration > 0.0 ? std::max(0.0, g_duration - 1.0 / g_fps) : origin + 1.0 / g_fps;
    g_seek_to = std::max(0.0, std::min(lastFrame, origin + direction / g_fps));
    g_seek_requested = true;
    g_last_seek_tick = GetTickCount64();
    if (g_trackbar && g_duration > 0.0)
    {
        int pos = (int)(g_seek_to / g_duration * 1000.0);
        SendMessageW(g_trackbar, TBM_SETPOS, TRUE, pos);
    }
    Log("frame step %s -> %.3fs", direction < 0 ? "back" : "forward", (double)g_seek_to);
}

static void SeekAtMouse(HWND hwnd, LPARAM lp, WORD notification)
{
    RECT channel, thumb;
    SendMessageW(hwnd, TBM_GETCHANNELRECT, 0, (LPARAM)&channel);
    SendMessageW(hwnd, TBM_GETTHUMBRECT, 0, (LPARAM)&thumb);
    int x = (short)LOWORD(lp);
    int thumbWidth = thumb.right - thumb.left;
    int start = channel.left + thumbWidth / 2;
    int span = std::max(1L, channel.right - channel.left - thumbWidth);
    int pos = (int)(1000.0 * (x - start) / span + 0.5);
    pos = std::max(0, std::min(1000, pos));
    SendMessageW(hwnd, TBM_SETPOS, TRUE, pos);
    SendMessageW(GetParent(hwnd), WM_HSCROLL, MAKEWPARAM(notification, pos), (LPARAM)hwnd);
}

static LRESULT CALLBACK SeekBarProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp,
                                    UINT_PTR id, DWORD_PTR)
{
    switch (message)
    {
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        SetCapture(hwnd);
        SeekAtMouse(hwnd, lp, TB_THUMBTRACK);
        return 0;
    case WM_MOUSEMOVE:
        if (GetCapture() == hwnd) { SeekAtMouse(hwnd, lp, TB_THUMBTRACK); return 0; }
        break;
    case WM_LBUTTONUP:
        if (GetCapture() == hwnd) {
            SeekAtMouse(hwnd, lp, TB_ENDTRACK);
            ReleaseCapture();
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        if (g_dragging) {
            g_dragging = false;
            SendMessageW(GetParent(hwnd), WM_HSCROLL, TB_ENDTRACK, (LPARAM)hwnd);
        }
        return 0;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, SeekBarProc, id);
        break;
    }
    return DefSubclassProc(hwnd, message, wp, lp);
}

static RECT FitVideoRect(int areaWidth, int areaHeight, UINT contentWidth, UINT contentHeight)
{
    RECT result = {0, 0, std::max(1, areaWidth), std::max(1, areaHeight)};
    if (!contentWidth || !contentHeight || areaWidth <= 0 || areaHeight <= 0) return result;

    if ((long long)areaWidth * contentHeight > (long long)areaHeight * contentWidth) {
        int width = std::max(1, (int)((long long)areaHeight * contentWidth / contentHeight));
        result.left = (areaWidth - width) / 2;
        result.right = result.left + width;
    } else {
        int height = std::max(1, (int)((long long)areaWidth * contentHeight / contentWidth));
        result.top = (areaHeight - height) / 2;
        result.bottom = result.top + height;
    }
    return result;
}

static void ToggleFullscreen()
{
    if (!g_hwnd || (!g_fullscreen && !g_media_loaded)) return;
    if (!g_fullscreen) {
        MONITORINFO monitor = {sizeof(monitor)};
        if (!GetWindowPlacement(g_hwnd, &g_windowed_placement) ||
            !GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) return;
        g_windowed_style = (DWORD)GetWindowLongPtrW(g_hwnd, GWL_STYLE);
        g_windowed_menu = GetMenu(g_hwnd);
        g_fullscreen = true;
        SetMenu(g_hwnd, nullptr);
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, g_windowed_style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(g_hwnd, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
            monitor.rcMonitor.right - monitor.rcMonitor.left,
            monitor.rcMonitor.bottom - monitor.rcMonitor.top,
            SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        g_fullscreen = false;
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, g_windowed_style);
        SetMenu(g_hwnd, g_windowed_menu);
        SetWindowPlacement(g_hwnd, &g_windowed_placement);
        SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
            SWP_NOOWNERZORDER | SWP_NOACTIVATE);
        DrawMenuBar(g_hwnd);
    }
    LayoutControls(g_hwnd);
}

static void LayoutControls(HWND hwnd)
{
    const int muteWidth = 70, volumeLabelWidth = 110, volumeSliderWidth = 100;
    const int audioWidth = muteWidth + 6 + volumeLabelWidth + 6 + volumeSliderWidth;
    RECT r; GetClientRect(hwnd, &r);
    int width = r.right, height = r.bottom;
    HWND chrome[] = {g_pause_button, g_prev_frame_button, g_next_frame_button,
        g_split_button, g_dlss_button, g_model_button, g_fullscreen_button,
        g_mute_button, g_volume_label, g_volume_slider, g_trackbar};
    if (g_fullscreen) {
        for (HWND control : chrome) if (control) ShowWindow(control, SW_HIDE);
        UINT contentWidth = g_vid_w * (g_side ? 2u : 1u);
        RECT video = FitVideoRect(width, height, contentWidth, g_vid_h);
        SetWindowPos(g_video_hwnd, nullptr, video.left, video.top,
            video.right - video.left, video.bottom - video.top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        return;
    }
    for (HWND control : chrome) if (control) ShowWindow(control, SW_SHOW);
    struct Control { HWND window; int width; };
    Control buttons[] = {{g_pause_button, 72}, {g_prev_frame_button, 64},
        {g_next_frame_button, 64}, {g_split_button, 90}, {g_dlss_button, 100},
        {g_model_button, 130}, {g_fullscreen_button, 88}};
    int x = 8, row = 0;
    auto place = [&](int controlWidth) {
        if (x > 8 && x + controlWidth > width - 8) { x = 8; ++row; }
        POINT pos = {x, row * 42 + 8};
        x += controlWidth + 6;
        return pos;
    };
    POINT positions[7];
    for (int i = 0; i < 7; ++i) positions[i] = place(buttons[i].width);
    POINT audio = place(audioWidth);
    int seekY = (row + 1) * 42 + 8;
    int videoHeight = std::max(1, height - (seekY + 38));
    UINT contentWidth = g_media_loaded ? g_vid_w * (g_side ? 2u : 1u) : 0;
    UINT contentHeight = g_media_loaded ? g_vid_h : 0;
    RECT video = FitVideoRect(width, videoHeight, contentWidth, contentHeight);
    struct Placement { HWND window; int x, y, width, height; };
    Placement placements[12];
    int count = 0;
    placements[count++] = {g_video_hwnd, video.left, video.top,
        video.right - video.left, video.bottom - video.top};
    for (int i = 0; i < 7; ++i)
        placements[count++] = {buttons[i].window, positions[i].x,
            videoHeight + positions[i].y, buttons[i].width, 34};
    placements[count++] = {g_mute_button, audio.x, videoHeight + audio.y, muteWidth, 34};
    placements[count++] = {g_volume_label, audio.x + muteWidth + 6,
        videoHeight + audio.y + 6, volumeLabelWidth, 22};
    placements[count++] = {g_volume_slider, audio.x + muteWidth + 6 + volumeLabelWidth + 6,
        videoHeight + audio.y, volumeSliderWidth, 34};
    placements[count++] = {g_trackbar, 8, videoHeight + seekY, std::max(1, width - 16), 30};

    HDWP batch = BeginDeferWindowPos(count);
    for (int i = 0; batch && i < count; ++i)
        if (placements[i].window)
            batch = DeferWindowPos(batch, placements[i].window, nullptr,
                placements[i].x, placements[i].y, placements[i].width, placements[i].height,
                SWP_NOZORDER | SWP_NOACTIVATE);
    if (batch) EndDeferWindowPos(batch);
    else for (int i = 0; i < count; ++i)
        if (placements[i].window)
            SetWindowPos(placements[i].window, nullptr,
                placements[i].x, placements[i].y, placements[i].width, placements[i].height,
                SWP_NOZORDER | SWP_NOACTIVATE);

    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

static void OpenVideoDialog(HWND hwnd)
{
    bool wasPlaying = g_media_loaded && !g_paused;
    if (wasPlaying) TogglePause();
    wchar_t path[32768] = {};
    OPENFILENAMEW dialog = {sizeof(dialog)};
    dialog.hwndOwner = hwnd;
    dialog.lpstrFilter = L"Video files\0*.mp4;*.mkv;*.avi;*.mov;*.webm;*.ts;*.m2ts;*.wmv;*.m4v\0All files\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = 32768;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    dialog.lpstrTitle = L"Open video";
    if (GetOpenFileNameW(&dialog)) g_open_path = path;
    else if (wasPlaying) TogglePause();
}

static void ShowHotkeyHelp(HWND hwnd)
{
    MessageBoxW(hwnd, HOTKEY_HELP_TEXT, L"Keyboard Shortcuts", MB_OK | MB_ICONINFORMATION);
}

static LRESULT DrawModernTrackbar(NMCUSTOMDRAW *draw)
{
    ThemeColors colors = CurrentThemeColors();
    if (draw->dwDrawStage == CDDS_PREPAINT) {
        RECT client;
        GetClientRect(draw->hdr.hwndFrom, &client);
        FillRect(draw->hdc, &client, g_background_brush ? g_background_brush : GetSysColorBrush(COLOR_WINDOW));
        return CDRF_NOTIFYITEMDRAW;
    }
    if (draw->dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;
    if (draw->dwItemSpec != TBCD_CHANNEL && draw->dwItemSpec != TBCD_THUMB)
        return CDRF_SKIPDEFAULT;

    RECT item = draw->rc;
    COLORREF fill = draw->dwItemSpec == TBCD_THUMB ? colors.accent : colors.border;
    int radius = draw->dwItemSpec == TBCD_THUMB ? 12 : 6;
    if (draw->dwItemSpec == TBCD_CHANNEL) {
        int center = (item.top + item.bottom) / 2;
        item.top = center - 2;
        item.bottom = center + 3;
    }
    HBRUSH brush = CreateSolidBrush(fill);
    HGDIOBJ oldBrush = SelectObject(draw->hdc, brush);
    HGDIOBJ oldPen = SelectObject(draw->hdc, GetStockObject(NULL_PEN));
    RoundRect(draw->hdc, item.left, item.top, item.right, item.bottom, radius, radius);
    SelectObject(draw->hdc, oldPen);
    SelectObject(draw->hdc, oldBrush);
    DeleteObject(brush);
    return CDRF_SKIPDEFAULT;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m)
    {
    case WM_ERASEBKGND:
    {
        HDC dc = (HDC)wp;
        RECT client;
        GetClientRect(hwnd, &client);
        FillRect(dc, &client, g_background_brush ? g_background_brush : GetSysColorBrush(COLOR_WINDOW));
        if (g_media_loaded || g_fullscreen) {
            RECT videoArea = client;
            if (!g_fullscreen && g_pause_button) {
                RECT button;
                GetWindowRect(g_pause_button, &button);
                MapWindowPoints(nullptr, hwnd, (POINT *)&button, 2);
                videoArea.bottom = std::max(0L, button.top - 8);
            }
            FillRect(dc, &videoArea, g_video_brush ? g_video_brush : (HBRUSH)GetStockObject(BLACK_BRUSH));
        }
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    {
        HDC dc = (HDC)wp;
        HWND control = (HWND)lp;
        ThemeColors colors = CurrentThemeColors();
        SetBkMode(dc, TRANSPARENT);
        if (control == g_video_hwnd) {
            SetTextColor(dc, g_dark_theme ? RGB(176, 182, 193) : RGB(205, 210, 220));
            return (LRESULT)(g_video_brush ? g_video_brush : (HBRUSH)GetStockObject(BLACK_BRUSH));
        }
        SetTextColor(dc, colors.text);
        return (LRESULT)(g_background_brush ? g_background_brush : GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_DRAWITEM:
        if (((DRAWITEMSTRUCT *)lp)->CtlType == ODT_BUTTON) {
            DrawModernButton((DRAWITEMSTRUCT *)lp);
            return TRUE;
        }
        if (((DRAWITEMSTRUCT *)lp)->CtlType == ODT_MENU) {
            DrawModernMenu((DRAWITEMSTRUCT *)lp);
            return TRUE;
        }
        break;
    case WM_MEASUREITEM:
        if (((MEASUREITEMSTRUCT *)lp)->CtlType == ODT_MENU) {
            MeasureModernMenu((MEASUREITEMSTRUCT *)lp);
            return TRUE;
        }
        break;
    case WM_NOTIFY:
        if (((NMHDR *)lp)->code == NM_CUSTOMDRAW &&
            (((NMHDR *)lp)->hwndFrom == g_volume_slider || ((NMHDR *)lp)->hwndFrom == g_trackbar))
            return DrawModernTrackbar((NMCUSTOMDRAW *)lp);
        break;
    case WM_SIZE: LayoutControls(hwnd); return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize = {320, 300}; return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_FILE_OPEN) { OpenVideoDialog(hwnd); return 0; }
        if (LOWORD(wp) == ID_FILE_EXIT) { SendMessageW(hwnd, WM_CLOSE, 0, 0); return 0; }
        if (LOWORD(wp) == ID_HELP_SHORTCUTS) { ShowHotkeyHelp(hwnd); return 0; }
        if (LOWORD(wp) == ID_THEME_LIGHT) { ApplyTheme(false); return 0; }
        if (LOWORD(wp) == ID_THEME_DARK) { ApplyTheme(true); return 0; }
        if ((HWND)lp == g_pause_button && HIWORD(wp) == BN_CLICKED) { TogglePause(); return 0; }
        if ((HWND)lp == g_prev_frame_button && HIWORD(wp) == BN_CLICKED) { RequestFrameStep(-1); return 0; }
        if ((HWND)lp == g_next_frame_button && HIWORD(wp) == BN_CLICKED) { RequestFrameStep(1); return 0; }
        if ((HWND)lp == g_split_button && HIWORD(wp) == BN_CLICKED) { ToggleComparison(); return 0; }
        if ((HWND)lp == g_dlss_button && HIWORD(wp) == BN_CLICKED) { ToggleNR(); return 0; }
        if ((HWND)lp == g_model_button && HIWORD(wp) == BN_CLICKED) { CycleModel(); return 0; }
        if ((HWND)lp == g_mute_button && HIWORD(wp) == BN_CLICKED) { ToggleMute(); return 0; }
        if ((HWND)lp == g_video_hwnd && HIWORD(wp) == STN_CLICKED) { TogglePause(); return 0; }
        if ((HWND)lp == g_fullscreen_button && HIWORD(wp) == BN_CLICKED) { ToggleFullscreen(); return 0; }
        break;
    case WM_KEYDOWN:
        if (wp == VK_F11) { ToggleFullscreen(); return 0; }
        if (wp == VK_ESCAPE) {
            if (g_fullscreen) ToggleFullscreen(); else g_running = false;
            return 0;
        }
        break;
    case WM_DROPFILES:
    {
        HDROP drop = (HDROP)wp;
        UINT length = DragQueryFileW(drop, 0, nullptr, 0);
        std::vector<wchar_t> path(length + 1);
        if (length && DragQueryFileW(drop, 0, path.data(), length + 1)) g_open_path = path.data();
        DragFinish(drop);
        return 0;
    }
    case WM_HSCROLL:
        if ((HWND)lp == g_volume_slider) {
            SetVolume((int)SendMessageW(g_volume_slider, TBM_GETPOS, 0, 0));
            return 0;
        }
        if ((HWND)lp == g_trackbar)
        {
            int pos = (int)SendMessageW(g_trackbar, TBM_GETPOS, 0, 0);
            if (g_duration > 0) g_seek_to = (double)pos / 1000.0 * g_duration;
            WORD code = LOWORD(wp);
            bool force = (code == TB_THUMBPOSITION || code == TB_ENDTRACK);
            ULONGLONG now = GetTickCount64();
            if (force || now - g_last_seek_tick > 150)
                { g_seek_requested = true; g_last_seek_tick = now; }
            if (code == TB_THUMBTRACK) g_dragging = true;
            else if (force) g_dragging = false;
            return 0;
        }
        break;
    case WM_CLOSE: g_running = false; DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        if (g_ui_font) { DeleteObject(g_ui_font); g_ui_font = nullptr; }
        if (g_background_brush) { DeleteObject(g_background_brush); g_background_brush = nullptr; }
        if (g_video_brush) { DeleteObject(g_video_brush); g_video_brush = nullptr; }
        g_running = false;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, m, wp, lp);
}

static bool SetupWindow(UINT w, UINT h)
{
    if (!g_hwnd) {
    UINT dw = g_side ? w * 2 : w; // side-by-side doubles the width
    const UINT TBH = 76;          // controls stay outside the video surface
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"nr_player";
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    RECT r = { 0, 0, (LONG)dw, (LONG)(h + TBH) };
    AdjustWindowRect(&r, style, TRUE);
    RECT work; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    g_hwnd = CreateWindowExW(0, L"nr_player", L"DLSS5 NR player", style,
                             CW_USEDEFAULT, CW_USEDEFAULT, std::min(r.right - r.left, work.right - work.left), std::min(r.bottom - r.top, work.bottom - work.top),
                             nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) return false;
    g_ui_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HMENU menu = CreateMenu(), file = CreatePopupMenu(), view = CreatePopupMenu();
    HMENU theme = CreatePopupMenu(), help = CreatePopupMenu();
    AppendMenuW(file, MF_STRING | MF_OWNERDRAW, ID_FILE_OPEN, L"&Open...\tCtrl+O");
    AppendMenuW(file, MF_SEPARATOR | MF_OWNERDRAW, 0, nullptr);
    AppendMenuW(file, MF_STRING | MF_OWNERDRAW, ID_FILE_EXIT, L"E&xit");
    AppendMenuW(menu, MF_POPUP | MF_OWNERDRAW, (UINT_PTR)file, L"&File");
    AppendMenuW(theme, MF_STRING | MF_OWNERDRAW, ID_THEME_LIGHT, L"&Light");
    AppendMenuW(theme, MF_STRING | MF_OWNERDRAW, ID_THEME_DARK, L"&Dark");
    AppendMenuW(view, MF_POPUP | MF_OWNERDRAW, (UINT_PTR)theme, L"&Theme");
    AppendMenuW(menu, MF_POPUP | MF_OWNERDRAW, (UINT_PTR)view, L"&View");
    AppendMenuW(help, MF_STRING | MF_OWNERDRAW, ID_HELP_SHORTCUTS, L"&Keyboard Shortcuts...");
    AppendMenuW(menu, MF_POPUP | MF_OWNERDRAW, (UINT_PTR)help, L"&Help");
    g_theme_menu = theme;
    SetMenu(g_hwnd, menu);
    DragAcceptFiles(g_hwnd, TRUE);

    DWORD buttonStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW;
    g_video_hwnd = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_NOTIFY | SS_CENTER | SS_CENTERIMAGE,
                                  0, 0, dw, h, g_hwnd, nullptr, wc.hInstance, nullptr);
    g_pause_button = CreateWindowExW(0, L"BUTTON", L"Pause", buttonStyle,
                                    0, 0, 72, 28, g_hwnd, nullptr, wc.hInstance, nullptr);
    g_prev_frame_button = CreateWindowExW(0, L"BUTTON", L"\x25C0", buttonStyle,
                                         0, 0, 64, 28, g_hwnd, nullptr, wc.hInstance, nullptr);
    g_next_frame_button = CreateWindowExW(0, L"BUTTON", L"\x25B6", buttonStyle,
                                         0, 0, 64, 28, g_hwnd, nullptr, wc.hInstance, nullptr);
    DWORD toggleStyle = buttonStyle;
    g_split_button = CreateWindowExW(0, L"BUTTON", L"Split: OFF", toggleStyle,
                                    0, 0, 100, 28, g_hwnd, nullptr, wc.hInstance, nullptr);
    g_dlss_button = CreateWindowExW(0, L"BUTTON", L"DLSS 5: ON", toggleStyle,
                                   0, 0, 100, 28, g_hwnd, nullptr, wc.hInstance, nullptr);
    g_model_button = CreateWindowExW(0, L"BUTTON", L"Model: Natural", buttonStyle,
                                    0, 0, 130, 28, g_hwnd, nullptr, wc.hInstance, nullptr);
    g_fullscreen_button = CreateWindowExW(0, L"BUTTON", L"Full screen", buttonStyle,
                                         0, 0, 88, 28, g_hwnd, nullptr, wc.hInstance, nullptr);
    g_mute_button = CreateWindowExW(0, L"BUTTON", L"Mute", toggleStyle,
                                   0, 0, 70, 28, g_hwnd, nullptr, wc.hInstance, nullptr);
    g_volume_label = CreateWindowExW(0, L"STATIC", L"Volume: 100%", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                                    0, 0, 88, 22, g_hwnd, nullptr, wc.hInstance, nullptr);
    g_volume_slider = CreateWindowExW(0, TRACKBAR_CLASSW, L"Volume", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                                     0, 0, 116, 28, g_hwnd, nullptr, wc.hInstance, nullptr);
    SendMessageW(g_volume_slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(g_volume_slider, TBM_SETPAGESIZE, 0, 10);
    SendMessageW(g_volume_slider, TBM_SETPOS, TRUE, g_volume);
    UpdateVolumeControls();
    // seek bar (child trackbar at the bottom)
    g_trackbar = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                                 0, h, dw, TBH, g_hwnd, nullptr, wc.hInstance, nullptr);
    if (g_trackbar) SendMessageW(g_trackbar, TBM_SETRANGE, TRUE, MAKELPARAM(0, 1000));
    if (!g_video_hwnd || !g_pause_button || !g_prev_frame_button || !g_next_frame_button ||
        !g_split_button || !g_dlss_button || !g_model_button || !g_trackbar ||
        !g_mute_button || !g_volume_label || !g_volume_slider || !g_fullscreen_button) return false;
    HWND controls[] = {g_video_hwnd, g_pause_button, g_prev_frame_button, g_next_frame_button,
        g_split_button, g_dlss_button, g_model_button, g_fullscreen_button,
        g_mute_button, g_volume_label, g_volume_slider, g_trackbar};
    for (HWND control : controls) SendMessageW(control, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    HWND buttons[] = {g_pause_button, g_prev_frame_button, g_next_frame_button,
        g_split_button, g_dlss_button, g_model_button, g_fullscreen_button, g_mute_button};
    for (HWND button : buttons) if (!SetWindowSubclass(button, ModernButtonProc, 2, 0)) return false;
    if (!SetWindowSubclass(g_trackbar, SeekBarProc, 1, 0)) return false;
    ApplyTheme(true);
    LayoutControls(g_hwnd);
    }

    if (!g_dev) {
        SetWindowTextW(g_video_hwnd, L"Drop a video here, or choose File > Open");
        UpdateModeTitle();
        ShowWindow(g_hwnd, SW_SHOW);
        return true;
    }
    SetWindowTextW(g_video_hwnd, L"");
    LayoutControls(g_hwnd);

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = g_side ? w * 2 : w; sd.Height = h;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(g_factory2->CreateSwapChainForHwnd(g_queue.Get(), g_video_hwnd, &sd, nullptr, nullptr, &sc1)))
        { Log("FAIL: CreateSwapChainForHwnd"); return false; }
    sc1.As(&g_swap);
    UpdateModeTitle();
    ShowWindow(g_hwnd, SW_SHOW);
    return true;
}

// ---------------------------------------------------------------------------
// ffmpeg / ffprobe (subprocess helpers)
// ---------------------------------------------------------------------------
static std::string RunCapture(const std::wstring &cmdline)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return "";
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr; si.hStdError = GetStdHandle(STD_ERROR_HANDLE); si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = {};
    std::wstring cl = cmdline;
    if (!CreateProcessW(nullptr, &cl[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        { CloseHandle(rd); CloseHandle(wr); return ""; }
    CloseHandle(wr);
    std::string out; char buf[4096]; DWORD n;
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, n);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return out;
}

// spawn ffmpeg with stdout piped; returns the read handle (or NULL)
static HANDLE SpawnFfmpeg(const std::wstring &input, double seek, HANDLE *proc)
{
    std::wstring cmdline = L"ffmpeg -hide_banner -loglevel error ";
    if (g_cuda_decode) cmdline += L"-hwaccel cuda ";
    if (seek > 0) { wchar_t b[64]; swprintf_s(b, L"-ss %.3f ", seek); cmdline += b; }
    cmdline += L"-i \"" + input + L"\" -an -f rawvideo -pix_fmt nv12 -";
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return nullptr;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr; si.hStdError = GetStdHandle(STD_ERROR_HANDLE); si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &cmdline[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        { CloseHandle(rd); CloseHandle(wr); Log("FAIL: cannot spawn ffmpeg"); return nullptr; }
    CloseHandle(wr);
    CloseHandle(pi.hThread);
    if (proc) *proc = pi.hProcess; else CloseHandle(pi.hProcess);
    return rd;
}

static bool ProbeVideo(const std::wstring &input, UINT *w, UINT *h, double *fps)
{
    std::wstring cmdline = L"ffprobe -v error -select_streams v:0 -show_entries stream=width,height,r_frame_rate -of csv=p=0 \"" + input + L"\"";
    std::string out = RunCapture(cmdline);
    // format: 1920,1080,60/1\n
    int W = 0, H = 0, num = 0, den = 1;
    if (sscanf_s(out.c_str(), "%d,%d,%d/%d", &W, &H, &num, &den) < 2)
        { Log("FAIL: ffprobe parse (%s)", out.c_str()); return false; }
    *w = W; *h = H;
    *fps = (num > 0 && den > 0) ? (double)num / (double)den : 30.0;
    return true;
}

static bool ProbeDuration(const std::wstring &input, double *dur)
{
    std::wstring cmdline = L"ffprobe -v error -show_entries format=duration -of csv=p=0 \"" + input + L"\"";
    std::string out = RunCapture(cmdline);
    double d = atof(out.c_str());
    if (d <= 0) return false;
    *dur = d;
    return true;
}

// spawn ffmpeg decoding audio -> raw float32 stereo 48kHz on stdout
static HANDLE SpawnAudio(const std::wstring &input, double seek, HANDLE *proc)
{
    std::wstring cmdline = L"ffmpeg -hide_banner -loglevel error ";
    if (seek > 0) { wchar_t b[64]; swprintf_s(b, L"-ss %.3f ", seek); cmdline += b; }
    cmdline += L"-i \"" + input + L"\" -vn -f f32le -ac 2 -ar 48000 -";
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return nullptr;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr; si.hStdError = GetStdHandle(STD_ERROR_HANDLE); si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &cmdline[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        { CloseHandle(rd); CloseHandle(wr); return nullptr; }
    CloseHandle(wr);
    CloseHandle(pi.hThread);
    if (proc) *proc = pi.hProcess; else CloseHandle(pi.hProcess);
    return rd;
}

// spawn the encode ffmpeg (reads NR RGBA from stdin, encodes + muxes original audio)
static HANDLE SpawnEncoder(const std::wstring &input, const std::wstring &output, HANDLE *proc)
{
    std::wstring cmdline = L"ffmpeg -hide_banner -loglevel error -f rawvideo -pix_fmt rgba ";
    wchar_t b[64];
    swprintf_s(b, L"-s %ux%u ", g_vid_w, g_vid_h); cmdline += b;
    swprintf_s(b, L"-r %.6f ", g_fps); cmdline += b;
    cmdline += L"-i - -i \"" + input + L"\" -map 0:v:0 -map 1:a:0? ";
    swprintf_s(b, L"-c:v libx264 -pix_fmt yuv420p -crf %d ", g_crf); cmdline += b;
    cmdline += L"-preset medium -c:a copy -movflags +faststart -y \"" + output + L"\"";
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return nullptr;
    SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0); // parent keeps the write end (not inherited)
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = rd; si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE); si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &cmdline[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        { CloseHandle(rd); CloseHandle(wr); return nullptr; }
    CloseHandle(rd);
    CloseHandle(pi.hThread);
    if (proc) *proc = pi.hProcess; else CloseHandle(pi.hProcess);
    return wr;  // write end (stdin of the encoder)
}

// read exactly `want` bytes from the pipe (blocks; returns bytes read, 0 on EOF)
static size_t ReadPipe(HANDLE rd, void *buf, size_t want)
{
    size_t got = 0;
    while (got < want)
    {
        DWORD n = 0;
        if (!ReadFile(rd, (char *)buf + got, (DWORD)(want - got), &n, nullptr)) break;
        if (n == 0) break; // EOF
        got += n;
    }
    return got;
}

// audio playback thread: waveOut + continuous refill from the PCM pipe
static DWORD WINAPI AudioThread(LPVOID)
{
    WAVEFORMATEX wf = {};
    wf.wFormatTag = 0x0003; // WAVE_FORMAT_IEEE_FLOAT
    wf.nChannels = 2;
    wf.nSamplesPerSec = 48000;
    wf.wBitsPerSample = 32;
    wf.nBlockAlign = wf.nChannels * wf.wBitsPerSample / 8;   // 8
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign; // 384000
    HWAVEOUT hwo;
    if (waveOutOpen(&hwo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
        { g_audio_done = true; return 0; }
    AcquireSRWLockExclusive(&g_audio_lock);
    g_wave_out = hwo;
    ApplyVolumeLocked();
    if (g_paused) waveOutPause(hwo);
    ReleaseSRWLockExclusive(&g_audio_lock);

    const int NUM = 12;
    const DWORD CHUNK = 4800 * wf.nBlockAlign; // 100 ms
    struct Buf { std::vector<char> d; WAVEHDR h; bool active; };
    std::vector<Buf> bufs(NUM);
    for (auto &b : bufs) { b.d.resize(CHUNK); b.active = false; }

    // prefill
    for (auto &b : bufs)
    {
        size_t n = ReadPipe(g_audio_read, b.d.data(), CHUNK);
        if (n == 0) { g_audio_done = true; break; }
        b.h = {}; b.h.lpData = b.d.data(); b.h.dwBufferLength = (DWORD)n;
        waveOutPrepareHeader(hwo, &b.h, sizeof(WAVEHDR));
        waveOutWrite(hwo, &b.h, sizeof(WAVEHDR));
        b.active = true;
    }

    while (g_running && !g_audio_done)
    {
        bool refilled = false;
        for (auto &b : bufs)
        {
            if (b.active && (b.h.dwFlags & WHDR_DONE))
            {
                waveOutUnprepareHeader(hwo, &b.h, sizeof(WAVEHDR));
                size_t n = ReadPipe(g_audio_read, b.d.data(), CHUNK);
                if (n == 0) { g_audio_done = true; break; }
                b.h = {}; b.h.lpData = b.d.data(); b.h.dwBufferLength = (DWORD)n;
                waveOutPrepareHeader(hwo, &b.h, sizeof(WAVEHDR));
                waveOutWrite(hwo, &b.h, sizeof(WAVEHDR));
                refilled = true;
            }
        }
        if (!refilled) Sleep(5);
    }

    AcquireSRWLockExclusive(&g_audio_lock);
    g_wave_out = nullptr;
    waveOutReset(hwo);
    for (auto &b : bufs)
        if (b.active) waveOutUnprepareHeader(hwo, &b.h, sizeof(WAVEHDR));
    waveOutClose(hwo);
    ReleaseSRWLockExclusive(&g_audio_lock);
    return 0;
}

// ---------------------------------------------------------------------------
// seek: restart both ffmpeg processes at a new position
// ---------------------------------------------------------------------------
static void Seek(const std::wstring &input, double t)
{
    if (g_duration > 0) t = std::max(0.0, std::min(t, g_duration - 1.0 / g_fps));
    g_audio_done = true;
    // stop video ffmpeg
    if (g_ffproc) { TerminateProcess(g_ffproc, 0); CloseHandle(g_ffproc); g_ffproc = nullptr; }
    if (g_ffread) { CloseHandle(g_ffread); g_ffread = nullptr; }
    // stop audio ffmpeg, wait for the playback thread to drain (pipe EOF), then close
    if (g_afproc) { TerminateProcess(g_afproc, 0); CloseHandle(g_afproc); g_afproc = nullptr; }
    if (g_audio_thread) { WaitForSingleObject(g_audio_thread, 2000); CloseHandle(g_audio_thread); g_audio_thread = nullptr; }
    if (g_audio_read) { CloseHandle(g_audio_read); g_audio_read = nullptr; }
    g_audio_done = false;

    g_base_time = t;
    g_frame_index = 0; // reset NR temporal history (Reset=1 on next frame)

    g_ffread = SpawnFfmpeg(input, t, &g_ffproc);
    g_audio_read = SpawnAudio(input, t, &g_afproc);
    if (g_audio_read)
        g_audio_thread = CreateThread(nullptr, 0, AudioThread, nullptr, 0, nullptr);
    Log("seek -> %.2fs", t);
}

// ---------------------------------------------------------------------------
// upload + render one frame
// ---------------------------------------------------------------------------
static bool ReadFrame(HANDLE rd, std::vector<uint8_t> &buf)
{
    size_t need = buf.size(), got = 0;
    while (got < need)
    {
        DWORD n = 0;
        if (!ReadFile(rd, buf.data() + got, (DWORD)(need - got), &n, nullptr) || n == 0)
            return false; // EOF or error
        got += n;
    }
    return true;
}

static void RenderFrame(const uint8_t *nv12)
{
    UINT slot = g_frame_slot;
    ULONGLONG wt0 = GetTickCount64();
    WaitFence(g_fence[slot].Get(), g_fence_value[slot]);
    g_wait_ms += (double)(GetTickCount64() - wt0);
    g_cmd_alloc[slot]->Reset();
    g_list->Reset(g_cmd_alloc[slot].Get(), nullptr);

    // copy NV12 (Y plane + interleaved UV plane, tight) into padded staging
    ULONGLONG ut0 = GetTickCount64();
    uint8_t *stg = nullptr;
    g_staging->Map(0, nullptr, (void **)&stg);
    UINT64 y_size = (UINT64)g_row_pitch * g_vid_h;
    const uint8_t *y_src = nv12;
    const uint8_t *uv_src = nv12 + (size_t)g_vid_w * g_vid_h;
    for (UINT y = 0; y < g_vid_h; ++y)
        memcpy(stg + (size_t)y * g_row_pitch, y_src + (size_t)y * g_vid_w, g_vid_w);
    for (UINT y = 0; y < g_vid_h / 2; ++y)
        memcpy(stg + y_size + (size_t)y * g_row_pitch, uv_src + (size_t)y * g_vid_w, g_vid_w);
    g_staging->Unmap(0, nullptr);
    g_upload_ms += (double)(GetTickCount64() - ut0);

    D3D12_RESOURCE_BARRIER bars[8];
    UINT nb = 0;
    D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
    D3D12_TEXTURE_COPY_LOCATION src_loc = {};
    src_loc.pResource = g_staging.Get();
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src_loc.PlacedFootprint.Footprint.Depth = 1;
    src_loc.PlacedFootprint.Footprint.RowPitch = g_row_pitch;

    // upload Y and UV
    bars[nb++] = Trans(g_y_tex.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    bars[nb++] = Trans(g_uv_tex.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    g_list->ResourceBarrier(nb, bars);

    dst_loc.pResource = g_y_tex.Get();
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_loc.SubresourceIndex = 0;
    src_loc.PlacedFootprint.Offset = 0;
    src_loc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8_UNORM;
    src_loc.PlacedFootprint.Footprint.Width = g_vid_w;
    src_loc.PlacedFootprint.Footprint.Height = g_vid_h;
    g_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

    dst_loc.pResource = g_uv_tex.Get();
    src_loc.PlacedFootprint.Offset = y_size;
    src_loc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8_UNORM;
    src_loc.PlacedFootprint.Footprint.Width = g_vid_w / 2;
    src_loc.PlacedFootprint.Footprint.Height = g_vid_h / 2;
    g_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

    // Y/UV -> NPSR (cs0 reads); nr_in: NPSR -> UAV (cs0 writes)
    nb = 0;
    bars[nb++] = Trans(g_y_tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    bars[nb++] = Trans(g_uv_tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    bars[nb++] = Trans(g_nr_in.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_list->ResourceBarrier(nb, bars);

    ID3D12DescriptorHeap *heaps[] = { g_cbv_heap.Get() };
    g_list->SetDescriptorHeaps(1, heaps);
    UINT inc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_GPU_DESCRIPTOR_HANDLE h0 = g_cbv_heap->GetGPUDescriptorHandleForHeapStart();

    // cs0: NV12 -> RGBA16F (NR input)
    g_list->SetPipelineState(g_pso_in.Get());
    g_list->SetComputeRootSignature(g_rs.Get());
    g_list->SetComputeRootDescriptorTable(0, h0);                        // Y
    g_list->SetComputeRootDescriptorTable(1, { h0.ptr + inc });          // UV
    g_list->SetComputeRootDescriptorTable(2, { h0.ptr + 2 * inc });      // nr_in UAV
    g_list->Dispatch((g_vid_w + 15) / 16, (g_vid_h + 15) / 16, 1);

    // nr_in: UAV -> NPSR (NR reads)
    nb = 0;
    bars[nb++] = Trans(g_nr_in.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_list->ResourceBarrier(nb, bars);

    bool useNR = g_nr_available && (g_side || g_nr_enabled);
    if (useNR) {
        g_params->Set("DLSSNR.Reset", (g_frame_index == 0 || g_nr_reset) ? 1 : 0);
        NVSDK_NGX_Result re;
        if (g_nr_eval && g_shim_eval)
            re = g_shim_eval((void *)g_nr_eval, g_list.Get(), g_feature, g_params, nullptr);
        else
            re = g_eval(g_list.Get(), g_feature, g_params, nullptr);
        if (re != NGX_SUCCESS) Log("Evaluate -> 0x%08X", (unsigned)re);
        g_nr_reset = false;
    }

    // nr_out: UAV -> NPSR (cs2 reads); stage/orig: COMMON -> UAV (cs2 writes)
    nb = 0;
    bars[nb++] = Trans(g_nr_out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    bars[nb++] = Trans(g_stage_rgba.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (g_side)
        bars[nb++] = Trans(g_orig_rgba.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_list->ResourceBarrier(nb, bars);

    // cs2: NR output -> stage (right side)
    g_list->SetPipelineState(g_pso_out.Get());
    g_list->SetComputeRootDescriptorTable(0, { h0.ptr + (useNR ? 3 : 5) * inc }); // NR output or original input
    g_list->SetComputeRootDescriptorTable(2, { h0.ptr + 4 * inc });      // stage UAV
    g_list->Dispatch((g_vid_w + 15) / 16, (g_vid_h + 15) / 16, 1);

    // cs2: NR input (original) -> orig (left side), side-by-side only
    if (g_side)
    {
        g_list->SetComputeRootDescriptorTable(0, { h0.ptr + 5 * inc });  // nr_in SRV
        g_list->SetComputeRootDescriptorTable(2, { h0.ptr + 6 * inc });  // orig UAV
        g_list->Dispatch((g_vid_w + 15) / 16, (g_vid_h + 15) / 16, 1);
    }

    // copy to backbuffer
    UINT bb = g_swap->GetCurrentBackBufferIndex();
    ComPtr<ID3D12Resource> backbuffer;
    g_swap->GetBuffer(bb, IID_PPV_ARGS(&backbuffer));
    nb = 0;
    bars[nb++] = Trans(g_stage_rgba.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (g_side)
        bars[nb++] = Trans(g_orig_rgba.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    bars[nb++] = Trans(backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    g_list->ResourceBarrier(nb, bars);

    D3D12_TEXTURE_COPY_LOCATION d2 = {};
    d2.pResource = backbuffer.Get();
    d2.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    d2.SubresourceIndex = 0;
    if (g_side)
    {
        D3D12_TEXTURE_COPY_LOCATION so = {};
        so.pResource = g_orig_rgba.Get();
        so.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        so.SubresourceIndex = 0;
        g_list->CopyTextureRegion(&d2, 0, 0, 0, &so, nullptr);
        D3D12_TEXTURE_COPY_LOCATION s2 = {};
        s2.pResource = g_stage_rgba.Get();
        s2.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        s2.SubresourceIndex = 0;
        g_list->CopyTextureRegion(&d2, g_vid_w, 0, 0, &s2, nullptr);
    }
    else
    {
        D3D12_TEXTURE_COPY_LOCATION s2 = {};
        s2.pResource = g_stage_rgba.Get();
        s2.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        s2.SubresourceIndex = 0;
        g_list->CopyTextureRegion(&d2, 0, 0, 0, &s2, nullptr);
    }

    // offline: copy NR output (stage) into the readback buffer for encoding
    if (!g_output.empty())
    {
        D3D12_TEXTURE_COPY_LOCATION rd = {};
        rd.pResource = g_readback.Get();
        rd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        rd.PlacedFootprint.Offset = 0;
        rd.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.PlacedFootprint.Footprint.Width = g_vid_w;
        rd.PlacedFootprint.Footprint.Height = g_vid_h;
        rd.PlacedFootprint.Footprint.Depth = 1;
        rd.PlacedFootprint.Footprint.RowPitch = (g_vid_w * 4 + 255) & ~255u;
        D3D12_TEXTURE_COPY_LOCATION ss = {};
        ss.pResource = g_stage_rgba.Get();
        ss.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        ss.SubresourceIndex = 0;
        g_list->CopyTextureRegion(&rd, 0, 0, 0, &ss, nullptr);
    }

    // restore states
    nb = 0;
    bars[nb++] = Trans(backbuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    bars[nb++] = Trans(g_stage_rgba.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    if (g_side)
        bars[nb++] = Trans(g_orig_rgba.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    bars[nb++] = Trans(g_nr_out.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_list->ResourceBarrier(nb, bars);

    g_list->Close();
    ID3D12CommandList *cmds[] = { g_list.Get() };
    g_queue->ExecuteCommandLists(1, cmds);
    g_queue->Signal(g_fence[slot].Get(), ++g_fence_value[slot]);
    g_swap->Present((g_fast || !g_output.empty()) ? 0 : 1, 0);
    ++g_frame_index;
    g_current_time = g_base_time + (double)(g_frame_index - 1) / g_fps;
    g_last_slot = slot;
    g_frame_slot = (slot + 1) % FRAMES_IN_FLIGHT;
}

// read back the stage RGBA8 texture and write tight RGBA bytes to a file
static void DumpFirstFrame()
{
    // drain in-flight frames so an allocator can be safely reused
    for (int i = 0; i < FRAMES_IN_FLIGHT; ++i)
        WaitFence(g_fence[i].Get(), g_fence_value[i]);
    g_cmd_alloc[0]->Reset();
    g_list->Reset(g_cmd_alloc[0].Get(), nullptr);
    D3D12_RESOURCE_BARRIER b = Trans(g_stage_rgba.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g_list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION s = {};
    s.pResource = g_stage_rgba.Get(); s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; s.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION d = {};
    d.pResource = g_readback.Get(); d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    d.PlacedFootprint.Offset = 0;
    d.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.PlacedFootprint.Footprint.Width = g_vid_w;
    d.PlacedFootprint.Footprint.Height = g_vid_h;
    d.PlacedFootprint.Footprint.Depth = 1;
    d.PlacedFootprint.Footprint.RowPitch = (g_vid_w * 4 + 255) & ~255u;
    g_list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
    ExecuteAndWait();
    void *m = nullptr;
    g_readback->Map(0, nullptr, &m);
    FILE *f = _wfopen(g_dump_path.c_str(), L"wb");
    UINT rgba_pitch = (g_vid_w * 4 + 255) & ~255u;
    if (f)
    {
        const uint8_t *src = (const uint8_t *)m;
        for (UINT y = 0; y < g_vid_h; ++y)
            fwrite(src + (size_t)y * rgba_pitch, 1, (size_t)g_vid_w * 4, f);
        fclose(f);
        Log("dumped first frame -> %ls", g_dump_path.c_str());
    }
    g_readback->Unmap(0, nullptr);
}

// ---------------------------------------------------------------------------
// wmain
// ---------------------------------------------------------------------------
static int PlayVideo(const std::wstring &input)
{
    if (!ProbeVideo(input, &g_vid_w, &g_vid_h, &g_fps))
        Fatal("cannot probe video");
    Log("video %ux%u @ %.2f fps", g_vid_w, g_vid_h, g_fps);
    if (ProbeDuration(input, &g_duration))
        Log("duration %.1fs", g_duration);

    g_row_pitch = (g_vid_w + 255) & ~255u; // Y/UV row pitch (both W bytes per row)
    if (!CreateDevice()) Fatal("device failed");

    // textures
    g_y_tex   = MakeTex(g_vid_w, g_vid_h, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_FLAG_NONE);
    g_uv_tex  = MakeTex(g_vid_w / 2, g_vid_h / 2, DXGI_FORMAT_R8G8_UNORM, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_FLAG_NONE);
    g_nr_in   = MakeTex(g_vid_w, g_vid_h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    g_nr_out  = MakeTex(g_vid_w, g_vid_h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    g_stage_rgba = MakeTex(g_vid_w, g_vid_h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COMMON,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    g_orig_rgba  = MakeTex(g_vid_w, g_vid_h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COMMON,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    // staging (upload) buffer: Y plane (padded) then UV plane (padded)
    D3D12_RESOURCE_DESC sbd = {};
    sbd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    sbd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    sbd.Width = (UINT64)g_row_pitch * g_vid_h + (UINT64)g_row_pitch * (g_vid_h / 2);
    sbd.Height = 1; sbd.DepthOrArraySize = 1; sbd.MipLevels = 1;
    sbd.SampleDesc.Count = 1; sbd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES shp = {};
    shp.Type = D3D12_HEAP_TYPE_UPLOAD;
    if (FAILED(g_dev->CreateCommittedResource(&shp, D3D12_HEAP_FLAG_NONE, &sbd,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_staging))))
        Fatal("staging failed");

    if (g_nr_available && !SetupNGX(g_vid_w, g_vid_h))
    {
        // SetupNGX starts with an open command list. Close and discard it so
        // RenderFrame can reset the list normally for original-only playback.
        g_list->Close();
        ReleaseNGXObjects();
        g_nr_available = false;
        g_nr_enabled = false;
        g_side = false;
        Log("NGX setup failed; continuing with original video rendering");
    }
    if (!SetupCompute()) Fatal("compute failed");
    g_media_loaded = true;
    if (!SetupWindow(g_vid_w, g_vid_h)) Fatal("window failed");

    // readback buffer for --dump / --output
    if (!g_dump_path.empty() || !g_output.empty())
    {
        D3D12_RESOURCE_DESC rbd = {};
        rbd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rbd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        rbd.Width = (UINT64)((g_vid_w * 8 + 255) & ~255u) * g_vid_h;
        rbd.Height = 1; rbd.DepthOrArraySize = 1; rbd.MipLevels = 1;
        rbd.SampleDesc.Count = 1; rbd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES rhp = {};
        rhp.Type = D3D12_HEAP_TYPE_READBACK;
        if (FAILED(g_dev->CreateCommittedResource(&rhp, D3D12_HEAP_FLAG_NONE, &rbd,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_readback))))
            Fatal("readback failed");
    }

    g_ffread = SpawnFfmpeg(input, 0, &g_ffproc);
    if (!g_ffread) Fatal("cannot start ffmpeg");

    // audio: spawn a second ffmpeg + playback thread (skip if no audio stream)
    g_audio_read = SpawnAudio(input, 0, &g_afproc);
    if (g_audio_read)
    {
        g_audio_thread = CreateThread(nullptr, 0, AudioThread, nullptr, 0, nullptr);
        Log("audio: on");
    }
    else Log("audio: none (no stream / ffmpeg failed)");

    // offline mode: spawn the encoder (NR RGBA -> output file)
    if (!g_output.empty())
    {
        g_enc_write = SpawnEncoder(input, g_output, &g_enc_proc);
        if (!g_enc_write) Fatal("cannot start encoder");
        Log("encoding to %ls (crf %d)", g_output.c_str(), g_crf);
    }

    Log("playing (ESC to stop). %s on GPU %d.",
        g_nr_available ? "DLSS 5 NR" : "Original video", g_gpu_index < 0 ? 0 : g_gpu_index);

    std::vector<uint8_t> frame((size_t)g_vid_w * g_vid_h * 3 / 2); // NV12: Y + interleaved UV
    std::vector<uint8_t> rgba_tight;
    if (!g_output.empty()) rgba_tight.resize((size_t)g_vid_w * g_vid_h * 4);
    ULONGLONG t0 = GetTickCount64();
    double next_t = (double)t0;
    UINT64 frames = 0;
    double frame_ms = g_fast ? 0.0 : 1000.0 / g_fps;
    MSG msg;
    bool have_frame = false;
    bool pending_frame = false; // paused seek preview, not yet consumed by playback

    while (g_running)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) { g_running = false; break; }
            if (msg.message == WM_KEYDOWN && msg.wParam == 'O' && (GetKeyState(VK_CONTROL) & 0x8000)) { OpenVideoDialog(g_hwnd); continue; }
            if (msg.message == WM_KEYDOWN && (msg.wParam == 'S' || msg.wParam == 'D' || msg.wParam == 'M'))
            {
                if (!(msg.lParam & (1LL << 30))) {
                    if (msg.wParam == 'S') ToggleComparison();
                    else if (msg.wParam == 'D') ToggleNR();
                    else CycleModel();
                }
                continue;
            }
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_SPACE && msg.hwnd != g_mute_button)
            {
                if (!(msg.lParam & (1LL << 30))) TogglePause();
                continue;
            }
            if (msg.message == WM_KEYDOWN && msg.hwnd != g_volume_slider && (msg.wParam == VK_LEFT || msg.wParam == VK_RIGHT))
            {
                if (!(msg.lParam & (1LL << 30))) RequestFrameStep(msg.wParam == VK_LEFT ? -1 : 1);
                continue;
            }
            if (msg.message == WM_KEYUP && msg.wParam == VK_SPACE && msg.hwnd != g_mute_button) continue;
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_F11) {
                if (!(msg.lParam & (1LL << 30))) ToggleFullscreen();
                continue;
            }
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
                if (g_fullscreen) ToggleFullscreen(); else { g_running = false; break; }
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_running || !g_open_path.empty()) break;

        // handle a pending seek
        if (g_seek_requested)
        {
            Seek(input, g_seek_to);
            have_frame = false;
            pending_frame = false;
            g_seek_requested = false;
            if (g_paused) {
                if (ReadFrame(g_ffread, frame)) {
                    RenderFrame(frame.data());
                    --g_frame_index;
                    g_nr_reset = true;
                    have_frame = true;
                    pending_frame = true;
                    g_refresh_view = false;
                    Log("paused seek preview at %.2fs", g_base_time);
                } else {
                    Log("No preview frame at %.2fs; playback remains paused", g_base_time);
                }
            }
            next_t = (double)GetTickCount64();
            continue;
        }

        if (g_paused)
        {
            if (g_refresh_view && have_frame) {
                RenderFrame(frame.data());
                --g_frame_index; // redraw the held frame without advancing playback
                g_nr_reset = true;
                g_refresh_view = false;
            }
            next_t = (double)GetTickCount64();
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
            continue;
        }

        ULONGLONG rt0 = GetTickCount64();
        if (!pending_frame && !ReadFrame(g_ffread, frame)) { Log("EOF"); break; }
        pending_frame = false;
        g_read_ms += (double)(GetTickCount64() - rt0);
        RenderFrame(frame.data());
        have_frame = true;
        g_refresh_view = false;
        ++frames;

        // offline: read back the NR output and feed it to the encoder
        if (!g_output.empty())
        {
            WaitFence(g_fence[g_last_slot].Get(), g_fence_value[g_last_slot]);
            void *m = nullptr;
            g_readback->Map(0, nullptr, &m);
            UINT rgba_pitch = (g_vid_w * 4 + 255) & ~255u;
            const uint8_t *src = (const uint8_t *)m;
            for (UINT y = 0; y < g_vid_h; ++y)
                memcpy(rgba_tight.data() + (size_t)y * g_vid_w * 4, src + (size_t)y * rgba_pitch, g_vid_w * 4);
            g_readback->Unmap(0, nullptr);
            DWORD n = 0;
            if (!WriteFile(g_enc_write, rgba_tight.data(), (DWORD)rgba_tight.size(), &n, nullptr) || n != rgba_tight.size())
                { Log("encoder write failed"); break; }
        }

        // advance the seek bar (only when not dragging)
        if (!g_dragging && g_trackbar && g_duration > 0)
        {
            int pos = (int)(g_current_time / g_duration * 1000.0);
            SendMessageW(g_trackbar, TBM_SETPOS, TRUE, pos);
        }

        if (!g_dump_path.empty() && g_frame_index == 1)
        { DumpFirstFrame(); g_dump_path.clear(); }

        ULONGLONG t1 = GetTickCount64();
        if (t1 - t0 >= 2000)
        {
            double fps = frames * 1000.0 / (t1 - t0);
            Log("%.1f fps (%llu frames)  read=%.1fms wait=%.1fms upload=%.1fms",
                fps, (unsigned long long)frames, g_read_ms / frames, g_wait_ms / frames, g_upload_ms / frames);
            frames = 0; t0 = t1;
            g_read_ms = g_wait_ms = g_upload_ms = 0;
        }
        if (!g_fast && g_output.empty())
        {
            next_t += frame_ms;
            double now = (double)GetTickCount64();
            if (now < next_t) Sleep((DWORD)(next_t - now));
        }
    }

    // finalize the encoder (offline)
    if (g_enc_write) { CloseHandle(g_enc_write); g_enc_write = nullptr; }
    if (g_enc_proc) { WaitForSingleObject(g_enc_proc, 600000); CloseHandle(g_enc_proc); g_enc_proc = nullptr; }
    if (!g_output.empty()) Log("encoded -> %ls", g_output.c_str());

    Log("stopped");
    return 0;
}

static void CleanupPlayback()
{
    if (g_fullscreen) ToggleFullscreen();
    Log("cleanup: subprocesses");
    g_audio_done = true;
    if (g_ffproc) { TerminateProcess(g_ffproc, 0); WaitForSingleObject(g_ffproc, 2000); CloseHandle(g_ffproc); g_ffproc = nullptr; }
    if (g_afproc) { TerminateProcess(g_afproc, 0); WaitForSingleObject(g_afproc, 2000); CloseHandle(g_afproc); g_afproc = nullptr; }
    if (g_audio_thread) { WaitForSingleObject(g_audio_thread, INFINITE); CloseHandle(g_audio_thread); g_audio_thread = nullptr; }
    if (g_ffread) { CloseHandle(g_ffread); g_ffread = nullptr; }
    if (g_audio_read) { CloseHandle(g_audio_read); g_audio_read = nullptr; }
    for (UINT i = 0; i < FRAMES_IN_FLIGHT; ++i) if (g_fence[i]) WaitFence(g_fence[i].Get(), g_fence_value[i]);
    Log("cleanup: feature");
    ReleaseNGXObjects();
    Log("cleanup: resources");
    g_swap.Reset(); g_readback.Reset();
    g_pso_in.Reset(); g_pso_out.Reset(); g_rs.Reset(); g_cbv_heap.Reset();
    g_y_tex.Reset(); g_uv_tex.Reset(); g_staging.Reset(); g_nr_in.Reset();
    g_nr_out.Reset(); g_stage_rgba.Reset(); g_orig_rgba.Reset(); g_list.Reset();
    for (UINT i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        g_cmd_alloc[i].Reset(); g_fence[i].Reset(); g_fence_value[i] = 0;
    }
    g_sync_fence.Reset(); g_queue.Reset(); g_dev.Reset(); g_factory2.Reset(); g_factory.Reset();
    Log("cleanup: modules");
    // NVIDIA runtime workers must remain loaded for the process lifetime.
    g_frame_slot = g_last_slot = 0; g_sync_value = g_frame_index = 0;
    g_base_time = g_current_time = g_duration = 0; g_seek_requested = g_dragging = false;
    g_nr_reset = true; g_refresh_view = false; g_paused = false; g_audio_done = false;
    g_read_ms = g_wait_ms = g_upload_ms = 0;
    g_media_loaded = false;
    if (IsWindow(g_hwnd)) {
        SetWindowTextW(g_pause_button, L"Pause");
        SendMessageW(g_trackbar, TBM_SETPOS, TRUE, 0);
        SetWindowTextW(g_video_hwnd, L"Drop a video here, or choose File > Open");
        InvalidateRect(g_video_hwnd, nullptr, TRUE);
        UpdateModeTitle();
        LayoutControls(g_hwnd);
    }
}

int wmain(int argc, wchar_t **argv)
{
    std::wstring input;
    for (int i = 1; i < argc; ++i)
    {
        std::wstring a = argv[i];
        if (a == L"--gpu" && i + 1 < argc) g_gpu_index = _wtoi(argv[++i]);
        else if (a == L"--style" && i + 1 < argc) { char b[64]; WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, b, 64, nullptr, nullptr); g_style = b; }
        else if (a == L"--preset" && i + 1 < argc) g_preset = _wtoi(argv[++i]);
        else if (a == L"--intensity" && i + 1 < argc) g_intensity = _wtoi(argv[++i]);
        else if (a == L"--tone" && i + 1 < argc) g_tone = _wtoi(argv[++i]);
        else if (a == L"--structure" && i + 1 < argc) g_structure = _wtoi(argv[++i]);
        else if (a == L"--skin" && i + 1 < argc) g_skin = _wtoi(argv[++i]);
        else if (a == L"--mask" && i + 1 < argc) g_mask = _wtoi(argv[++i]);
        else if (a == L"--fast") g_fast = true;
        else if (a == L"--nr-only") g_side = false;
        else if (a == L"--side-by-side") g_side = true;
        else if (a == L"--dump" && i + 1 < argc) g_dump_path = argv[++i];
        else if (a == L"--output" && i + 1 < argc) g_output = argv[++i];
        else if (a == L"--crf" && i + 1 < argc) g_crf = _wtoi(argv[++i]);
        else if (a == L"--gui") g_gui = true;
        else input = a;
    }
    if (input.empty()) g_gui = true;
    if (!g_output.empty() && g_fast) { Log("note: offline mode (--output) already runs full speed"); }

    // common controls (trackbar)
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);


    if (g_gui && !SetupWindow(960, 540)) return 1;
    int result = 0;
    while (g_running) {
        if (!input.empty()) {
            try { result = PlayVideo(input); }
            catch (const std::exception &error) {
                result = 1;
                if (g_gui) MessageBoxA(g_hwnd, error.what(), "Cannot play video", MB_OK | MB_ICONERROR);
            }
            CleanupPlayback();
            input.clear();
            if (!g_gui && g_open_path.empty()) break;
        }
        if (!g_open_path.empty()) { input.swap(g_open_path); continue; }
        if (!g_running) break;
        MSG message;
        int got = GetMessageW(&message, nullptr, 0, 0);
        if (got <= 0) break;
        if (message.message == WM_KEYDOWN) {
            if (message.wParam == 'O' && (GetKeyState(VK_CONTROL) & 0x8000)) { OpenVideoDialog(g_hwnd); continue; }
            if (message.wParam == 'S') { ToggleComparison(); continue; }
            if (message.wParam == 'D') { ToggleNR(); continue; }
            if (message.wParam == 'M') { CycleModel(); continue; }
            if (message.wParam == VK_F11) { ToggleFullscreen(); continue; }
            if (message.wParam == VK_LEFT && message.hwnd != g_volume_slider) { RequestFrameStep(-1); continue; }
            if (message.wParam == VK_RIGHT && message.hwnd != g_volume_slider) { RequestFrameStep(1); continue; }
            if (message.wParam == VK_ESCAPE && g_fullscreen) { ToggleFullscreen(); continue; }
        }
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    return result;
}
