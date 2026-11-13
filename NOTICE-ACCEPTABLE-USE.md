# Installation and use notes

Dynamic VRS is experimental, unofficial software. It is provided as is, without warranty,
under the MIT license (`LICENSE` in the source checkout, `LICENSE.txt` in the local ZIP).
The project distributes source. Redistribution is governed
by the applicable licenses, including those in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

## What each integration changes

**OpenXR (recommended).** Registers an API layer under
`HKCU\Software\Khronos\OpenXR\1\ApiLayers\Implicit`, stores its files in
`%LOCALAPPDATA%\OpenVRDynamicVRS\openxr`, and starts its control panel with the
application by default. Its changes are confined to the current user's registry and local files.

**OpenVR (experimental, opt-in).** Replaces `SteamVR\bin\vrclient_x64.dll` with this
project's proxy, keeps Valve's original next to it as `real_vrclient_x64.dll`, and loads
the layer into OpenVR applications that present a scene. This modifies a file installed by
Steam. Steam file verification or a SteamVR update may restore the original DLL. Review the
applicable Steam and game terms before installing the OpenVR integration.

## Application and data access

- The layer sets shading rates through NVAPI while the application runs. The OpenVR proxy's
  legacy module-path diagnostic is disabled by default and unsupported.
- Dynamic VRS exchanges gaze messages with PaperTracker over local loopback UDP.
  Diagnostic logs can contain gaze coordinates. Review logs before sharing them.

## Anti-cheat and removal

The layer loads into the application process. Anti-cheat software may flag it as tampering,
and game terms may prohibit its use. Uninstall the integrations before launching a protected game.

Close VR applications and the control panel. Exit Steam completely if you installed OpenVR.
From a source checkout, run `.\scripts\uninstall.ps1`. From a local ZIP, run `Uninstall.cmd`.
Check that uninstalling succeeds. It unregisters OpenXR and restores the original SteamVR client
DLL if the OpenVR proxy is still installed. To remove the retained OpenXR files and settings,
delete `%LOCALAPPDATA%\OpenVRDynamicVRS\openxr` after uninstalling.
