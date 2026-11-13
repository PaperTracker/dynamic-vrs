Dynamic VRS @VERSION@
============================

Experimental foveated rendering for Direct3D 11 VR applications on Windows.
https://github.com/PaperTracker/dynamic-vrs

You built this ZIP locally with tools\release\package.ps1. The project
distributes source code.

Requirements
------------

- Windows 10 or 11, x64.
- NVIDIA GPU and driver with NVAPI D3D11 variable pixel shading, such as
  an RTX 20-series or newer.
- A Direct3D 11 VR application using OpenVR or OpenXR. SteamVR is the
  tested OpenXR runtime.
- For eye-tracked mode, enable Gaze data export in PaperTracker and match
  its port to gazeExportPort (default 9945). Head-direction mode needs
  no eye tracker.

Install
-------

Extract the whole ZIP and read NOTICE-ACCEPTABLE-USE.md. Close VR applications
and the control panel. If you use a game with anti-cheat, uninstall the
integrations before launching it.

OpenXR (recommended)
1. Run Install-OpenXrOnly.cmd. It registers the OpenXR API layer for the
   current Windows user and leaves SteamVR files intact.
2. Start the application. The control panel opens by default after the
   first frame submitted with a composition layer. If a Steam-launched
   application does not load the layer, exit Steam completely and relaunch
   it before starting the application again.

OpenVR (experimental, opt-in)
1. Exit Steam completely (Steam > Exit). Review the applicable Steam and game
   terms before installing.
2. Run Install.cmd -SkipOpenXr from a terminal to install OpenVR only, or
   run Install.cmd to install both integrations. The OpenVR installer
   replaces SteamVR's bin\vrclient_x64.dll and backs up the original.
   Windows may ask for administrator access.

Update and uninstall
--------------------

Build a new ZIP, extract it and repeat the install command for the integration
you use. Existing settings are kept. Steam file verification or a SteamVR
update may restore the original DLL. Repeat the OpenVR installation if you
still want that integration.

To uninstall, close VR applications and the control panel. Exit Steam
completely if you installed OpenVR. Run Uninstall.cmd and check that it
reports success. It unregisters OpenXR and restores SteamVR's client DLL if
the OpenVR proxy is still installed. To remove retained OpenXR files and
settings, delete %LOCALAPPDATA%\OpenVRDynamicVRS\openxr after uninstalling.

Settings
--------

OpenVR: <SteamVR>\bin\dynamic-vrs.ini
OpenXR: %LOCALAPPDATA%\OpenVRDynamicVRS\openxr\dynamic-vrs.ini

See config\dynamic-vrs.ini.example. New installs use gaze mode. The full-rate
region starts centered in the direction you're facing and follows the eyes
when valid gaze data arrives. Use Save as defaults to keep panel changes
for the next launch. Scene GPU timing and A/B measurements are available
in OpenVR when SteamVR provides timing samples.

Package contents
----------------

Install-OpenXrOnly.cmd     Installs OpenXR only
Install.cmd, Uninstall.cmd Install and uninstall launchers
bin\                       Proxy, rendering layer, panel and diagnostic probe
scripts\                   Installation and restoration scripts
config\                    Annotated settings and OpenXR layer manifest
NOTICE-ACCEPTABLE-USE.md    Installation, privacy and use notes
LICENSE.txt                MIT license
THIRD-PARTY-NOTICES.md     SDK licenses and notices
