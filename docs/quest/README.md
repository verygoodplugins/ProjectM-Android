# Quest 3S / 3 Immersive Port (OpenXR)

Status: milestones 1 and 2 are implemented behind `-PprojectmQuest=true`. The default TV and
phone builds, and the published core AAR, are unchanged. Milestones 3 and 4 are not started.

## What is implemented
- `core/src/main/cpp/quest_xr.cpp`: OpenXR session (GLES, `XR_KHR_opengles_enable`) on its own render
  thread. projectM renders through an FBO into the swapchain texture (2048x2048), submitted every frame
  as an `XrCompositionLayerEquirect2KHR` dome (120x120 degrees, 10 m, straight ahead of the start pose).
  Presets come from the bundled library and advance every 30 s.
- `app/src/quest/.../QuestActivity.java`: runtime `RECORD_AUDIO` request, `AudioRecord` (48 kHz mono
  float, default input = headset mics, only while the app is in front) with a noise gate and slow AGC,
  pushed to `projectm_pcm_add_float` on the render thread.
- `app/src/quest/AndroidManifest.xml`: the manifest in `AndroidManifest.quest.draft.xml`; the TV launcher
  activity is removed from this build.

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
- arm64-v8a only (the Quest build restricts both `:app` and `:core` to it, and uses minSdk 29).
- Build (JDK 21, SDK 34, NDK 27.3.13750724, CMake 3.22.1, initialized submodules; the OpenXR loader comes
  from Maven Central as the Khronos `openxr_loader_for_android` AAR):
  `./gradlew -PprojectmQuest=true :app:assembleRelease`. The release variant is used because the debug
  variant builds projectM unoptimized. It is signed with the local debug key unless `SIGNING_*` is set, so
  it installs but is not a store build. Output: `app/build/outputs/apk/release/`.
- Enable developer mode on the headset, then `adb install -r <apk>`; launch it from Unknown Sources.

## Open items
- Confirm license implications of the upstream repo and projectM (LGPL) before any store release.
- Decide GLES vs Vulkan if the renderer moves.
