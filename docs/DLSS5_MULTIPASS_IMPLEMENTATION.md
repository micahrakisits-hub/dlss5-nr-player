# DLSS 5 Multipass Implementation Spec

Implement native DLSS 5 multipass support in this fork of `scegielski/dlss5-nr-player`.

## Goal

Add selectable 1x, 2x, and 3x DLSS 5 Neural Rendering passes to `nr_player.cpp`, using the existing `nvngx_dlssnr.dll` and existing caller shim.

Do **not** add Alex's Toolkit, ReShade, RenoDX, or any new NVIDIA runtime dependency.

The behavior should mimic the multipass cascade concept used by Alex's Toolkit:

- 1x: `A`
- 2x: `B -> A`
- 3x: `B -> C -> A`

Each A/B/C stage must be a **separate NGX feature-18 handle with its own temporal history**.

Do **not** implement multipass by calling Evaluate repeatedly on the same feature handle.

Before editing, inspect the existing code thoroughly, especially:

- `SetupNGX`
- `ReleaseNGXObjects`
- `RenderFrame`
- `SetupCompute`
- `PlayVideo`
- `CycleModel`
- pause/seek/reset handling
- GUI button creation/layout
- CLI argument parsing
- portable build scripts

Preserve all existing behavior.

---

## 1. Refactor the current single NGX feature into A/B/C stages

The current code has:

```cpp
static NVSDK_NGX_Parameter *g_params = nullptr;
static NVSDK_NGX_Handle    *g_feature = nullptr;
```

Replace/refactor this into three independently allocated parameter objects and feature handles, conceptually:

```cpp
g_params_a
g_params_b
g_params_c

g_feature_a
g_feature_b
g_feature_c
```

A must always exist when NR initializes successfully.

Attempt to create B and C as additional feature-18 instances using exactly the same existing NGX runtime/shim path that A uses.

Do not initialize NGX three times. There should still be one NGX initialization/session, but three feature-18 instances within that session.

Each feature must use its own `NVSDK_NGX_Parameter` object. Do not share one mutable parameter object among the three feature handles.

Add helpers where useful rather than triplicating large blocks of code, for example:

```cpp
ConfigureNRParams(...)
CreateNRFeature(...)
ReleaseNRFeature(...)
SetAllNRStyles(...)
```

Use the existing shim functions when available:

```cpp
g_shim_create
g_shim_eval
g_shim_release
```

exactly as the current single-feature path does.

---

## 2. Add intermediate FP16 textures

Keep:

```text
g_nr_in
g_nr_out
```

Add:

```text
g_nr_mid1
g_nr_mid2
```

using:

```text
DXGI_FORMAT_R16G16B16A16_FLOAT
D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
```

Allocate these at video resolution alongside `g_nr_in` and `g_nr_out`.

The data paths must be:

```text
1x:
g_nr_in
   -> A
g_nr_out

2x:
g_nr_in
   -> B
g_nr_mid1
   -> A
g_nr_out

3x:
g_nr_in
   -> B
g_nr_mid1
   -> C
g_nr_mid2
   -> A
g_nr_out
```

The final image must always end up in `g_nr_out`, so the existing output conversion, split comparison, readback, and offline encoding paths can continue to consume the same final texture.

---

## 3. Configure the three feature instances identically

Preserve all current DLSSNR parameters:

```text
DLSSNR.Width
DLSSNR.Height
DLSSNR.Enabled
DLSSNR.Reset
DLSSNR.Style
DLSSNR.Hint.Render.Preset
DLSSNR.Intensity
DLSSNR.LocalToneStrength
DLSSNR.LocalStructureStrength
DLSSNR.SkinStructureStrength
DLSSNR.UseAutoMask
DLSSNR.UICorrection
DLSSNR.DepthInverted
DLSSNR.ScalingRatio
DLSSNR.MVecScaleX
DLSSNR.MVecScaleY
subrect parameters
```

Apply the same current user-selected model/style and tuning values to A, B, and C.

A/B/C are labels for separate feature instances/cascade stages. Do **not** assume that A/B/C correspond to values of `DLSSNR.Style`.

The existing Natural/Default/Cinematic model selection remains independent of the new pass count.

Update `CycleModel()` so changing model/style updates all existing A/B/C parameter objects.

---

## 4. Set Color/Output resources per pass immediately before Evaluate

Before evaluating each stage, set its `DLSSNR.Color`, `DLSSNR.Output`, and `DLSSNR.Backbuffer` appropriately.

### 1x

```text
A.Color      = g_nr_in
A.Output     = g_nr_out
A.Backbuffer = g_nr_out
```

### 2x

```text
B.Color      = g_nr_in
B.Output     = g_nr_mid1

A.Color      = g_nr_mid1
A.Output     = g_nr_out
```

### 3x

```text
B.Color      = g_nr_in
B.Output     = g_nr_mid1

C.Color      = g_nr_mid1
C.Output     = g_nr_mid2

A.Color      = g_nr_mid2
A.Output     = g_nr_out
```

Set `Backbuffer` consistently with each pass's output as the current code does.

---

## 5. Handle D3D12 resource states correctly

The intermediate output of one NR stage becomes the input of the next stage.

After evaluating B:

```text
g_nr_mid1:
UNORDERED_ACCESS -> NON_PIXEL_SHADER_RESOURCE
```

before evaluating C or A.

In 3x mode, after evaluating C:

```text
g_nr_mid2:
UNORDERED_ACCESS -> NON_PIXEL_SHADER_RESOURCE
```

before evaluating A.

At the end of the frame, restore any intermediate textures that were transitioned to shader-resource state back to:

```text
D3D12_RESOURCE_STATE_UNORDERED_ACCESS
```

ready for the next frame.

Preserve the existing `g_nr_out` transitions used by the output compute pass.

Use transition/UAV barriers sufficient to guarantee that one NR pass has completed writing before the next reads that texture.

Do not introduce a CPU `ExecuteAndWait()` between neural passes. They should remain commands in the same D3D12 command list where possible.

---

## 6. Temporal reset behavior is critical

All active stages must receive the same reset event.

Existing conditions such as:

```cpp
g_frame_index == 0 || g_nr_reset
```

should cause:

```text
A.Reset = 1
B.Reset = 1
C.Reset = 1
```

for whichever stages are active on that frame.

Do **not** clear `g_nr_reset` after B or C. Clear it only after all active stages for that frame have evaluated.

Preserve/reset histories after:

- initial playback
- seek
- file replacement
- DLSS on/off
- model/style change
- comparison-mode changes where currently appropriate
- pass-count change

A hard seek must reset every active temporal stage.

---

## 7. Pause behavior

Preserve the current good pause behavior.

While a video is simply sitting paused, do **not** continuously evaluate A/B/C against the frozen frame.

Continue displaying the cached processed frame as the player does now.

A deliberate user change while paused — model, pass count, DLSS toggle, split mode, seeking/frame step — may request one refreshed render using the existing `g_refresh_view` mechanism.

Do not create an endless temporal feedback loop while paused.

---

## 8. Add user-selectable pass count

Add:

```cpp
static int g_nr_passes = 1;
```

Valid values:

```text
1
2
3
```

Default must remain **1**, so existing behavior and visual output are unchanged unless the user opts into multipass.

Add command-line support:

```text
--passes 1
--passes 2
--passes 3
```

Reject/clamp invalid values cleanly and log what happened.

Add a GUI button:

```text
Passes: 1x
Passes: 2x
Passes: 3x
```

Clicking it cycles:

```text
1 -> 2 -> 3 -> 1
```

Add keyboard hotkey:

```text
P
```

to cycle pass count.

Update the title/help text to include:

```text
P: passes
```

Pass-count controls should be disabled when DLSS NR is unavailable.

When pass count changes:

```cpp
g_nr_reset = true;
g_refresh_view = true;
```

so changing it while paused immediately gives one updated preview.

Do not resize/recreate the swapchain merely because pass count changes.

---

## 9. Graceful fallback if extra feature creation fails

A is mandatory for NR.

B and C are optional enhancements.

If:

- A succeeds
- B fails

continue with maximum supported pass count = 1.

If:

- A succeeds
- B succeeds
- C fails

continue with maximum supported pass count = 2.

Log this clearly.

The Passes button and `P` hotkey should cycle only through the supported count.

Example logs:

```text
NR feature A created: ...
NR feature B created: ...
NR feature C created: ...
DLSS 5 multipass available: 3 passes
```

or:

```text
NR feature C creation failed; limiting multipass to 2x
```

Do not make the whole player fail merely because B or C cannot be created.

---

## 10. Release all resources correctly

Update `ReleaseNGXObjects()` to:

1. wait/fence appropriately before releasing active NGX features
2. release C, B, A feature handles
3. destroy C, B, A parameter objects
4. then perform the existing NGX shutdown once

Avoid double releases.

Ensure reopening another video in the same GUI window still works.

---

## 11. Existing output modes must continue to work

Verify:

```text
DLSS off:
original video

DLSS on, 1x:
A

DLSS on, 2x:
B -> A

DLSS on, 3x:
B -> C -> A

Split:
original | final selected multipass result
```

Offline:

```text
--output
--dump
```

must use the final selected multipass result from `g_nr_out`.

Do not change FFmpeg/audio behavior.

---

## 12. Keep portable packaging working

This implementation must use the **existing runtime files**.

Do **not** add:

```text
alexs-toolkit.addon64
RenoDX
ReShade
another nvngx_dlssnr.dll
```

to the package.

Multipass is being implemented directly in our feature-18 D3D12 player.

`build_portable.py` should require no new runtime dependency unless the code genuinely demonstrates otherwise.

---

## 13. Update README

Document:

```text
P / Passes button:
1x = normal DLSS 5 NR
2x = B -> A cascade
3x = B -> C -> A cascade
```

Explain that each pass has an independent temporal history, so 2x/3x can increase enhancement but may also increase:

- temporal persistence
- smearing/ghosting
- settling time after cuts
- GPU load

State that multipass uses the same existing DLSS NR runtime; no additional NVIDIA DLL is required.

Add `--passes N` to the command-line table.

---

## 14. Build and test

Do not stop after editing.

Run:

```bat
build_player.bat
```

Fix all compilation errors and warnings that indicate real problems.

Then test at minimum:

```text
nr_player.exe --passes 1 <test video>
nr_player.exe --passes 2 <test video>
nr_player.exe --passes 3 <test video>
```

Also test:

- P cycling during playback
- P cycling while paused
- pause/resume
- seek while paused
- frame step
- Split mode
- D DLSS toggle
- M model cycling
- opening a second file
- closing cleanly

If you cannot visually verify playback yourself, say exactly which runtime checks need me to perform; do not claim visual success.

Inspect `%TEMP%\DLSS5-NR-Player.log` / console logs for feature creation/evaluation failures.

If possible, add diagnostic logging showing:

```text
DLSS NR passes: 1
DLSS NR passes: 2 (B -> A)
DLSS NR passes: 3 (B -> C -> A)
```

whenever the mode changes.

---

## 15. Do not commit until reviewed

Make the implementation and build it, but before committing, give me:

- concise summary of code changes
- files changed
- build result
- any runtime assumptions
- any test I need to perform manually on the RTX 5090

Do not publish a GitHub release yet.

---

## Important implementation constraint

The implementation should have **three feature handles**, not merely three calls to `g_eval()` against the same feature handle.

The latter would advance one temporal history multiple times for the same movie frame instead of creating independent cascade stages.

The intended design is:

```text
1x: A
2x: B -> A
3x: B -> C -> A
```

with independent temporal state for A, B, and C.

No additional `nvngx_dlssnr.dll` should be required for this implementation.
