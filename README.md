# DLSS 5 NR Player — desktop player and portable build

A Windows video player and offline converter based on [Zonnery/dlss5-nr-player](https://github.com/Zonnery/dlss5-nr-player). This fork adds a desktop interface, playback controls, and automatic selection of locally supplied RTX 40/50 neural-rendering runtimes.

This repository contains **source code only**. NVIDIA DLLs, the caller helper, FFmpeg, SDK headers, videos, and portable executables are not committed. Obtain runtime dependencies separately under their applicable terms.

## Desktop features

- Start with an empty player, then use **File → Open**, **Ctrl+O**, or drop a video onto the window.
- Open another video in the same window. After playback ends, the window stays available for another file.
- Pause/resume video and audio using the button or **Space**.
- Set playback volume from **0–100%**, or use **Mute / Unmute** without losing the chosen level. Volume and mute carry across seeks and video changes until the app closes. These controls do not change exported audio.
- Step one frame backward or forward with the **Previous Frame** / **Next Frame** buttons or the **Left** / **Right Arrow** keys. Stepping pauses playback and displays the selected frame.
- Click or drag the seek bar to jump to the pointer position. Seeking while paused displays a preview without resuming playback.
- Resize the window: the video keeps its original aspect ratio with black letterbox or pillarbox bars, while controls remain visible below it and wrap when needed.
- **Split** button / **S**: toggle original (left) versus DLSS 5 (right). Split view defaults off.
- **DLSS 5** button / **D**: toggle neural processing in single view. DLSS defaults on. Comparison always includes DLSS, so the DLSS toggle is disabled there; returning to single view restores the previous setting.
- **Model** button / **M**: cycle between the Default, Natural, and Cinematic DLSS 5 models. The new model is applied immediately and also works while paused.
- View controls also work while paused. The title shows the current mode. **Esc** closes the player.
- Build a single-file portable executable containing your locally supplied dependencies. It opens without a batch-file launcher.

## Hardware and validation

Windows 10/11 x64 and a Direct3D 12 GPU are required. The player selects a runtime from the **actual DXGI adapter used for rendering**; the `--gpu N` option selects a DXGI adapter index. On non-RTX 40/50 adapters, it skips NGX initialization and plays the original video with DLSS 5 and comparison controls disabled.

If an RTX 40/50 adapter is detected but its NGX or DLSS NR runtime is missing or cannot initialize, the player also falls back to original-only playback instead of stopping with an `NGX failed` error.

| GPU | Runtime | Validation |
|---|---|---|
| RTX 50 series | Original Blackwell NR DLL | Playback tested on RTX 5090 |
| RTX 40 series, including Ti/Super | Community Ada patch from NR-Media-UI | Selection and packaging tested; actual RTX 40 playback/performance not verified here |
| Other Direct3D 12 GPUs | Original video only | DLSS 5 is disabled; no NVIDIA runtime is loaded |

The RTX40 release README specifies NVIDIA driver **616.56 or newer** and describes its runtime as experimental. Compatibility and real-time performance must be checked on the target card. On supported RTX cards, DLSS-off mode skips neural evaluation after the NGX feature is initialized. Other Direct3D 12 GPUs use the original-only fallback and do not initialize NGX.

## Build the native player

Install Visual Studio Build Tools with **Desktop development with C++** and a Windows SDK. The script discovers MSVC with `vswhere.exe` instead of hardcoding Visual Studio 2019. A custom installation can be supplied through the `VCVARS64` environment variable.

```bat
build_player.bat
```

This produces `nr_player.exe`. The player declares its NGX interface inline, so **NVIDIA SDK headers are not required to compile this target**. The compiler links Windows D3D12, DXGI, D3DCompiler, User32, GDI32, WinMM, Common Controls, Common Dialogs, and Shell32 libraries.

Other build scripts and the DX11 bridge are retained from upstream. They are separate experiments and may have different prerequisites or paths.

## Runtime files to supply locally

Place these files relative to `nr_player.exe`. Original-only playback requires only FFmpeg and FFprobe; the NVIDIA files are needed when DLSS 5 is available:

| Path | Source / purpose |
|---|---|
| `_nvngx.dll` | NGX core from an appropriate installed NVIDIA driver; typically under `C:\Windows\System32\DriverStore\FileRepository\nv_dispi.inf_amd64_*` |
| `nvngx_dlssnr.dll` | Original RTX50 NR runtime from a legitimate DLSS5 application or the original NR-Media-UI RTX50 release |
| `runtime40/nvngx_dlssnr.dll` | Community Ada runtime from the author's [NR-Media-UI v1.1.0 RTX40 release](https://github.com/perseval-BLR/NR-Media-UI/releases/tag/v1.1.0) |
| `caller/nvngx.dll` | Caller wrapper from NR-Media-UI; its PyInstaller executable embeds this helper. It must export `DLSSNR_CallInit`, `DLSSNR_CallCreate`, `DLSSNR_CallEvaluate`, and `DLSSNR_CallRelease`. The driver's ordinary `nvngx.dll` is not a substitute. |
| `ffmpeg.exe`, `ffprobe.exe` | FFmpeg, available through [FFmpeg's download page](https://ffmpeg.org/download.html). CUDA decoding is used on NVIDIA adapters when supported; other adapters use normal software decoding. Native use can also resolve these from PATH. |

The two NR runtimes stay separate; the original RTX50 file is not overwritten with the patch. The selected DLL path is logged. `nvngx_dlss.dll` (Super Resolution) is not used by this player, although the upstream DX11 bridge uses it.

For reference, the RTX40 archive used during integration was `NR-Media-UI-v1.1.0-RTX40.zip`, SHA-256 `90cc641f987a5c6302d6c05d09c0de235b7567c95c03114a2a407dbeffc9c233` (verified against release metadata). Its NR DLL hash was `28bdc080d28686decdb63f6f4246b022274916b80aafdab266fe0fb63b2b9265`. This records provenance; it does not establish redistribution rights or guarantee compatibility.

## Run

Start the empty desktop player:

```bat
nr_player.exe
```

Play a file and keep the GUI available afterward:

```bat
nr_player.exe --gui "video.mp4"
```

Without `--gui`, a command-line input exits at EOF. The legacy `NR_player.bat` file picker remains available.

### Options

| Option | Meaning |
|---|---|
| `--gpu N` | DXGI adapter index |
| `--nr-only` | Single view (default) |
| `--side-by-side` | Start in comparison view |
| `--style natural\|cinematic` | NR style |
| `--preset N` | Render preset (default 3) |
| `--intensity N`, `--tone N`, `--structure N` | NR tuning |
| `--skin N`, `--mask N` | Skin structure / automatic mask |
| `--fast` | Disable frame pacing |
| `--output out.mp4` | Offline conversion with original audio |
| `--crf N` | Output H.264 CRF (default 18) |
| `--dump frame.rgba` | Dump the first processed frame |

## Build your local portable executable

After building the native player and supplying all files in the runtime table (including both NR runtimes), install Python and PyInstaller in a build environment:

```bat
python -m pip install pyinstaller
python build_portable.py
```

The result is `dist/DLSS 5 NR Player.exe`. `--dist-dir PATH` selects another output directory. Paths resolve relative to the packaging script, so it can be invoked from another working directory. The script reports missing dependencies and does not download them automatically.

The portable launcher extracts its payload to a temporary directory, launches the native GUI without a console, and waits until it closes before cleanup. No installed Python, FFmpeg, or source checkout is needed on the target computer. A Direct3D 12-compatible GPU and driver are still necessary. Diagnostics are written to `%TEMP%\DLSS5-NR-Player.log`.

The packaging recipe is for your local dependencies. Check the applicable licenses before sharing a package containing NVIDIA or other third-party binaries. This fork does not publish those binaries.

## Validation

From an x64 Native Tools command prompt:

```bat
cl /nologo /EHsc /std:c++17 /Fe:runtime_selection_test.exe tests\runtime_selection.cpp
runtime_selection_test.exe
```

The test checks runtime selection for RTX40, RTX50, Ti/Super/laptop variants, and unsupported adapters. Integration checks performed on RTX5090 covered playback, buttons and hotkeys, paused seeking, resize, file-open/replacement, file-drop handling, EOF/reopen, and the portable package. RTX40 hardware playback remains untested.

## Credits and scope

- Original player, conversion pipeline, NGX integration and experiments: [Zonnery/dlss5-nr-player](https://github.com/Zonnery/dlss5-nr-player).
- Runtime/helper release source: [perseval-BLR/NR-Media-UI](https://github.com/perseval-BLR/NR-Media-UI). Its RTX40 README credits the community patch to Uncle Burrito / dev-camo.
- NVIDIA DLSS/NGX names and binaries belong to NVIDIA and remain subject to their own terms.

This is an experimental community project. Upstream history and attribution are preserved; this fork does not add a new license grant for upstream or third-party material.
