# Dynamic VRS

Dynamic VRS adds experimental foveated rendering to Direct3D 11 VR applications on Windows with a compatible NVIDIA GPU. It uses NVIDIA NVAPI to keep a full-rate region around each eye's gaze point and reduce pixel shading toward the periphery. The full-rate region can follow your PaperTracker gaze or stay centered in the direction you're facing.

[Start with OpenXR](#openxr-recommended). Build from source to install.

## Requirements

- Windows 10 or 11, x64.
- An NVIDIA GPU and driver with NVAPI D3D11 variable pixel shading, such as an RTX 20-series or newer.
- A Direct3D 11 VR application using OpenVR, or OpenXR through SteamVR (the tested OpenXR runtime).
- For gaze mode: the PaperTracker Eye Tracker with **Gaze data export** enabled. Fixed mode needs no tracker.

## Install

Follow [Building](#building), then run the installer for your chosen integration from the checkout. Read the [installation and use notes](NOTICE-ACCEPTABLE-USE.md) before installing.

**Games with anti-cheat.** Dynamic VRS loads into the application process. Anti-cheat software may flag it as tampering, and game terms may prohibit its use. Close VR applications and [uninstall the integrations](#update-and-uninstall) before launching a protected game.

### OpenXR (recommended)

This integration registers a per-user OpenXR API layer and leaves SteamVR files intact.

1. Close VR games and their panels.
2. Run `Install-OpenXrOnly.cmd` or `.\scripts\install.ps1 -SkipOpenVr` in PowerShell. The installation runs with your current user permissions.
3. Start a Direct3D 11 OpenXR application. The control panel opens by default after the application submits its first frame with a composition layer. If a Steam-launched application does not load the layer, exit Steam completely and relaunch it before starting the application again.

### OpenVR (experimental, opt-in)

This integration replaces `SteamVR\bin\vrclient_x64.dll` with the Dynamic VRS proxy and backs up the original. Steam file verification or a SteamVR update may restore the original DLL. Review the applicable Steam and game terms before installing.

1. Exit Steam completely (**Steam > Exit**) and close VR games and their panels.
2. Run `.\scripts\install.ps1 -SkipOpenXr` and confirm. Windows may ask for administrator access if the SteamVR folder requires it.
3. Start a Direct3D 11 OpenVR application.

### Update and uninstall

To update, pull the source, rebuild, then repeat the install command for the integration you use. Existing settings are kept. If a SteamVR update restores the original DLL, repeat the OpenVR installation if you still want that integration.

To uninstall, close VR applications and the control panel. Exit Steam completely if you installed OpenVR. Run `.\scripts\uninstall.ps1` and check that it reports success. It unregisters the OpenXR layer and restores SteamVR's client DLL if the OpenVR proxy is still installed. To delete the retained OpenXR files and settings, remove `%LOCALAPPDATA%\OpenVRDynamicVRS\openxr` after uninstalling.

## Settings

| Integration | Settings file |
| --- | --- |
| OpenVR | `<SteamVR>\bin\dynamic-vrs.ini` |
| OpenXR | `%LOCALAPPDATA%\OpenVRDynamicVRS\openxr\dynamic-vrs.ini` |

The [configuration template](config/dynamic-vrs.ini.example) documents every setting. The most useful ones:

- `nativeVrsMode`: `off`, `fixed` or `gaze` (new installs use `gaze`).
- `vrsRing1x1Degrees` through `vrsRing4x2Degrees`: outer radius of each shading tier in degrees.
- `gazeExportPort`: loopback port of PaperTracker's gaze export, default `9945`; `0` disables gaze input.

The control panel changes mode, ring sizes, dithering and visual aids while a game runs. **Save as defaults** keeps them for the next launch.

## Eye tracking

Enable **Gaze data export** in PaperTracker and match its port to `gazeExportPort`. The full-rate region starts in the direction you're facing and returns there when tracking is lost. See the [protocol reference](docs/provider-layout.md) for the local UDP message format and the [installation and use notes](NOTICE-ACCEPTABLE-USE.md#application-and-data-access) for what diagnostic logs contain.

## Compatibility and limits

Testing covered a small number of locally installed applications on one machine. Compatibility and performance gains vary by application version, runtime and graphics settings. Foveated rendering can cause visible artifacts, flicker or discomfort, especially with inaccurate gaze tracking. Stop using it if you feel unwell.

- Only Direct3D 11 is supported. Side-by-side, two-slice array and separate-eye textures work. OpenXR needs projection subimages that fill their eye image or half of a side-by-side image; quad views and other graphics bindings are forwarded without VRS.
- Radius rings and exterior blur need single-sample RGBA8 or BGRA8 eye textures. VRS also works on eligible MSAA targets, which limits the available coarse rates.
- Scene GPU timing and A/B measurements are available in OpenVR when SteamVR provides timing samples. The shading work estimate is based on screen area and shading rates.
- VDXR is not supported. PaperTracker exports calibrated gaze by SteamVR. With VDXR selected and SteamVR closed, Dynamic VRS can load but receives no valid gaze data.
- In Beat Saber, the area you look at can still appear blurry. When foveated rendering is enabled in Beat Saber, fixed and dynamic foveated rendering appear to be active at the same time. As a result, areas near the edges of the view remain blurry even when you look directly at them. Further testing is needed to clarify the details.

## Building

For the full build, install Visual Studio 2022 with the Desktop development with C++ workload and the [pinned MSVC toolset](cmake/pinned_msvc_toolset.txt), plus CMake 3.25 or newer. Get the [OpenVR](https://github.com/ValveSoftware/openvr), [NVAPI](https://github.com/NVIDIA/nvapi) and [OpenXR](https://github.com/KhronosGroup/OpenXR-SDK) SDKs from their upstream repositories. See the [CI build](.github/workflows/package.yml) for pinned SDK revisions.

```powershell
.\build.ps1 -OpenVrSdkRoot C:\sdk\openvr -NvApiRoot C:\sdk\nvapi -OpenXrSdkRoot C:\sdk\openxr
.\scripts\install.ps1 -SkipOpenVr # Install OpenXR only
```

To create a ZIP from your own build, run `.\tools\release\package.ps1 -Version 0.0.6`. The project distributes source only. CI checks packaging without publishing the resulting ZIP.

Source layout: `src/proxy` (OpenVR client proxy), `src/layer` (the OpenVR/OpenXR layer), `src/control_panel` (the control panel), `src/common` (shared code), `scripts` (install and restore). [docs/architecture.md](docs/architecture.md) explains how the pieces fit together.

## License

Dynamic VRS is an unofficial project provided as is under the [MIT license](LICENSE). See [third-party notices](THIRD-PARTY-NOTICES.md) for SDK licenses.
