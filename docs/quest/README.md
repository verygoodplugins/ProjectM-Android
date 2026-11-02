# Quest 3S / 3 Immersive Port (OpenXR)

Status: scaffolding. Nothing here is wired into the existing app build yet.

## Goals
- Immersive (not 2D panel) visualizer on Meta Quest 3S and Quest 3.
- Audio input from the headset microphones only.
- Reuse the existing projectM 4.2 Android engine (arm64-v8a).

## Architecture
- Native OpenXR app (Khronos loader + Meta Android loader), `XR_KHR_opengles_enable`.
- projectM renders into an offscreen FBO texture (start ~2048x2048, adaptive scale).
- The texture is submitted as a compositor layer:
  - `XrCompositionLayerEquirect2KHR` (dome / wraparound), or
  - `XrCompositionLayerCylinderKHR` (curved wall).
- Optional `XR_FB_passthrough` backdrop (milestone 4).
- Frame cap 60-72 fps; reuse existing adaptive-quality logic for resolution scaling.

## Audio
- `AudioRecord` + `RECORD_AUDIO` (runtime permission) -> headset mic by default.
- PCM fed to `projectm_pcm_add_float`.
- Add input gain / AGC and a noise gate (headset mics pick up ambient noise).
- System/app audio capture is not available on Horizon OS; mic-only by design.

## Input
- Controller ray + trigger: next / previous / favorite preset.
- Thumbstick: brightness / speed.
- `XR_EXT_hand_tracking` pinch fallback.
- Menu is a small floating quad layer (Android views do not carry over).

## Milestones
1. OpenXR session + projectM texture on an equirect layer.
2. Headset mic -> projectM.
3. Controller menu + preset switching.
4. Passthrough toggle, comfort and perf tuning.

## Build / sideload
- arm64-v8a only.
- Enable developer mode on the headset, then `adb install -r app-quest-debug.apk`.

## Open items
- Confirm license implications of the upstream repo and projectM (LGPL) before any store release.
- Decide GLES vs Vulkan if the renderer moves.
