# Architecture

Dynamic VRS gives a Direct3D 11 VR application NVIDIA variable rate shading around the user's gaze. It does this from inside the application's process, through either the OpenVR or the OpenXR API, and it is controlled from a separate desktop panel.

## Binaries

| Binary | Source | Role |
| --- | --- | --- |
| `vrclient_x64.dll` | `src/proxy` | Replaces SteamVR's client library. Forwards every export to the real client (installed beside it as `real_vrclient_x64.dll`) and loads the layer into OpenVR applications. |
| `DynamicVRSLayer64.dll` | `src/layer` | The layer. Loaded by the proxy (OpenVR) or by the OpenXR loader as an implicit API layer. Contains all the rendering logic. |
| `DynamicVRSPanel64.exe` | `src/control_panel` | The control panel. Started by the layer on the first completed frame; talks to it through shared memory. |
| `dynamic_vrs_proxy_probe.exe` | `tools/diagnostics` | The installer runs it to check that the SteamVR client initializes and unloads through the installed proxy. |

`src/common` is a static library and a few headers that every binary shares: logging and module paths (`log`, `paths`), the `dynamic-vrs.ini` reader (`ini_file`), the OpenVR ABI types, the control protocol (`vrs_control_protocol.hpp`) and the gaze packet format (`gaze_packet.hpp`, `gaze_udp_receiver.hpp`).

## The layer

```
src/layer/
  config/       LayerSettings (g_settings): everything read from dynamic-vrs.ini, and the control protocol's view of it
  control/      control_server (shared memory to the panel), frame_control (per-frame apply and publish), panel_launcher
  vrs/          the shading core: gaze_tracking, eye_projection, rate_pattern, rate_image, vrs_binding, render_target_hook
  diagnostics/  debug blur and radius rings, the target alignment probe, GPU frame timing
  openvr/       IVRCompositor::Submit hooks and the DLL entry points
  openxr/       the OpenXR API layer; view_geometry holds its pure math
```

A frame flows like this:

1. **Entry.** OpenVR: the proxy calls `DynamicVRSLayer_InitializeWithConfig`, and the layer patches `Submit` in every `IVRCompositor` version the game's `openvr_api.dll` can return. OpenXR: the loader negotiates the layer, which intercepts session, swapchain and frame calls.
2. **Gaze.** `gaze_tracking` latches one gaze sample per frame from the UDP receiver, smooths it and decides how far to widen the rings (saccades, lost tracking). Without a sample the center is head-forward.
3. **Pattern.** `eye_projection` turns gaze angles into each eye's viewport position; `rate_pattern` fills a shading-rate tile map from the ring radii.
4. **Binding.** `render_target_hook` watches `OMSetRenderTargets` and viewport changes. `vrs_binding` decides which targets are eye targets, `rate_image` uploads the pattern to a per-target NVAPI rate image and binds it.
5. **Control.** After each completed frame `frame_control` applies requests from the panel and publishes metrics.

Shared render state lives in `vrs/vrs_state.hpp` behind `g_vrsMutex`. A hook that can run while the game holds its D3D multithread lock must take that lock first (`ContextThreadGuard`) and only then `g_vrsMutex`.

## The panel

`panel.cpp` owns the window and message loop. The rest of the class is split by concern: `panel_layout` (positions and hit testing), `panel_paint` (drawing through `panel_renderer`), `panel_input` (what a click or drag does), `panel_control` (polling the layer, sending edits, the A/B measurement, saving defaults). `panel_model` holds the tables and arithmetic behind the controls and has no Win32 dependency, `panel_theme` the palettes.

## Conventions

- Includes are rooted at `src/` (`"common/log.hpp"`, `"layer/vrs/vrs_state.hpp"`); a header in the same folder is included by bare name.
- A new setting: add a member to `LayerSettings`, read it in `loadLayerSettings`, document it in `config/dynamic-vrs.ini.example`, and add it to `control::Settings` only if the panel edits it.
- Every log line is `stage=<name> key=value ...` through `dvr::logMessage`; the files are in `%LOCALAPPDATA%\OpenVRDynamicVRS\logs`. That folder name predates the project's current name and is kept so existing installs keep working.
- NVAPI, OpenVR and OpenXR support are optional at build time (`cmake/DvrSdks.cmake`); code that needs one is behind `DVR_HAS_NVAPI`, `DVR_HAS_OPENVR` or `DVR_HAS_OPENXR`.

## Scripts and packaging

`scripts/install.ps1` and `scripts/uninstall.ps1` are the entry points behind `Install.cmd` and `Uninstall.cmd`. The OpenVR and OpenXR scripts run independently after the installer validates the build output. Shared helpers are in `scripts/lib`. `tools/release/package.ps1` creates a local ZIP from the build output, and `test_package.ps1` exercises OpenVR installation and removal against a temporary SteamVR tree.
