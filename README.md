# DLSS 5 Neural Rendering — Video Player & Offline Converter

Real-time playback and offline conversion of video through **NVIDIA DLSS 5 Neural Rendering** (NR, NGX feature id 18), driven directly through the NGX API. Side-by-side "original | NR" comparison, audio, seek bar, and a file-conversion mode.

> ⚠️ **Personal / experimental project.** It calls NVIDIA's NR model through the raw NGX interface. It runs **only on RTX 50-series (Blackwell) GPUs** and requires a recent NVIDIA driver.

---

## Features

- **Real-time NR playback** — decode → NR → window, with a live side-by-side *original | NR* view.
- **Offline conversion** — process a whole file and write a new video, keeping the original audio.
- **Hardware decode** — NVDEC (`-hwaccel cuda`) + YUV→RGB conversion on the GPU (no CPU color-conversion bottleneck).
- **Double-buffered** D3D12 frame loop so CPU decode overlaps GPU NR.
- Seek bar, audio (waveOut), GPU selection (`--gpu N`).

## Requirements

- Windows 10/11 64-bit
- NVIDIA **RTX 50-series** GPU (Blackwell)
- Recent NVIDIA display driver
- `ffmpeg` + `ffprobe` on `PATH`
- MSVC 2019 Build Tools (to build; `vcvars64.bat`)

## Build

Run the `.bat` in a "x64 Native Tools" context (each script calls `vcvars64.bat` itself):

| Script | Output | Links |
|---|---|---|
| `build_player.bat` | `nr_player.exe` (player + offline converter) | `d3d12 dxgi d3dcompiler user32 winmm comctl32` |
| `build.bat` | `nr_video.exe` (offline PNG→NR→PNG reference) | `d3d12 dxgi windowscodecs ole32` |
| `build_live.bat` | `nr_live.exe` (screen-capture experiment) | `d3d11 d3d12 dxgi d3dcompiler user32` |

The build scripts assume `C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat` — edit if yours differs.

## Usage

**Playback** (double-click `NR_player.bat` to pick a file, or from the command line):

```bat
nr_player.exe "video.mp4" --gpu 1
```

**Offline conversion** (processes the file, shows a live preview while it works):

```bat
nr_player.exe "video.mp4" --gpu 1 --style cinematic --preset 3 --intensity 2 --tone 1 --structure 1 --output "output.mp4"
```

### Options

| Flag | Meaning |
|---|---|
| `--gpu N` | DXGI adapter index (0 = first NVIDIA, etc.) |
| `--style natural\|cinematic` | NR style |
| `--preset N` | render preset (default 3) |
| `--intensity N` / `--tone N` / `--structure N` | NR strength sliders |
| `--skin N` / `--mask N` | skin structure / auto-mask |
| `--fast` | no frame pacing (max throughput, benchmark) |
| `--nr-only` | hide the side-by-side "original" half |
| `--output out.mp4` | offline mode: encode the NR result to a file |
| `--crf N` | x264 CRF for `--output` (default 18) |
| `--dump file.rgba` | dump the first NR frame as tight RGBA |

## How it works (short version)

`ffmpeg -hwaccel cuda -f rawvideo -pix_fmt nv12` feeds raw NV12 into the player → uploaded to the GPU → a compute shader converts NV12→RGBA16F (BT.709 limited range) → DLSS 5 NR (`nvngx_dlssnr.dll`, feature 18) → back to RGBA8 → swapchain. Offline mode adds a readback of each NR frame and pipes it into a second `ffmpeg` (libx264) that muxes the original audio.

Key detail: the NR path needs an exact NGX init (`NVSDK_NGX_D3D12_Init_ProjectID`) and a small **caller-validation shim** (`caller/nvngx.dll`) — the NR DLL rejects direct calls from unknown callers (`0xBAD00002`).

---

## ⚠️ Files you must provide yourself (NOT in this repo)

NVIDIA's binaries and the NGX SDK headers are **not redistributable** and are intentionally absent from this repository. Download/extract them yourself and place them next to the sources before building/running:

| File | What it is | Where to get it |
|---|---|---|
| `_nvngx.dll` | NGX core runtime | NVIDIA display driver — `C:\Windows\System32\DriverStore\FileRepository\nv_dispi.inf_amd64_*\` (the hash dir varies per driver version) |
| `nvngx_dlssnr.dll` | DLSS 5 Neural Rendering runtime | Ships with the driver and with DLSS-5 apps; the [NR-Media-UI](https://youtube.com/@perseval_BLR) release bundles it |
| `nvngx_dlss.dll` | DLSS Super Resolution (only needed by the DX11 bridge) | Any DLSS-enabled game's install dir, or the NVIDIA DLSS SDK |
| `nvsdk_ngx.h` / `nvsdk_ngx_defs.h` / `nvsdk_ngx_helpers.h` / `nvsdk_ngx_params.h` | NGX SDK headers | NVIDIA NGX SDK — [Streamline](https://github.com/NVIDIAGameWorks/Streamline) or the DLSS SDK at [developer.nvidia.com](https://developer.nvidia.com/rtx/dlss) |
| `caller/nvngx.dll` | caller-validation shim (thin wrapper) | Bundled with [NR-Media-UI](https://youtube.com/@perseval_BLR). Prefer committing its **source** (a few thin wrappers) if you have it, not the binary |
| `ffmpeg.exe` / `ffprobe.exe` | decode / encode / probe | [ffmpeg.org](https://ffmpeg.org/download.html), `winget install ffmpeg`, or `choco install ffmpeg` |

> The shim (`caller/nvngx.dll`) is a small helper DLL that exports thin wrappers so the NR runtime's caller-validation passes. It is **required** at runtime; keep it out of the repo unless you have redistributable source.

## Repo contents (what's actually ours)

- `nr_player.cpp` — the player + offline converter (main deliverable)
- `nr_video.cpp` — offline PNG→NR→PNG reference pipeline
- `nr_live.cpp` — screen-capture experiment
- `build.bat` / `build_live.bat` / `build_player.bat` — MSVC build scripts
- `NR_player.bat` — launcher with a file-open dialog
- `bridge.h` / `bridge.inc` / `dlss5-dx11-bridge.cpp` / `version.rc` — the **DLSS 5 DX11 Bridge** for ReShade (a separate, games-oriented sub-project)

Everything else in the working directory (`.exe`, `.obj`, `.mp4`, `.rgba`, `.png`, `bench_*`, `test_*`, `real_*`, logs) is build output or test data and is excluded via `.gitignore`.

## Legal

This project is a personal experiment. NVIDIA DLSS, NGX, and the `nvngx_*` / `_nvngx.dll` binaries are NVIDIA Corporation property and are governed by their own licenses — do not redistribute them. Use this code at your own risk.
