# HUICRSync Unreal Project

HUICRSync Unreal Project is a configured Unreal Engine project containing the HUICRSync runtime plugin. The plugin provides a reusable PC/HMD synchronization layer for mixed-reality workflows using UDP state packets, reliable UDP commands, calibration state, nDisplay screen binding, and synchronized runtime actor spawning.

This repository is published as a full Unreal project instead of a plugin-only package so users can open a prepared project, inspect the PC/HMD target setup, and test the plugin with the included maps and plugin content.

Tutorial Video: https://youtu.be/dEcZ0Hj-dEo
Document: https://drive.google.com/file/d/1cVm5pPxdXcxf4NFeOXp_Y73wFbAegLrr/view?usp=drive_link

## Repository Layout

- `Plugins/HUICRSync/` - the HUICRSync runtime plugin.
- `Source/` - project target files, including PC/HMD target variants.
- `Config/` - project settings required by the prepared workflow.
- `Content/` - project-level Unreal content. Some folders may come from Unreal template content.

## Main Plugin Features

- PC/HMD role-based runtime synchronization through `UHUICRSyncSubsystem`.
- HMD-to-PC connection handshake with saved PC IP/port support.
- Calibration state workflow with HMD screens, HMD calibrators, PC screen binding, and PC calibrators.
- nDisplay-oriented PC screen initialization and Default View Point control.
- Motion parallax state propagation from HMD to PC.
- Reliable command channel over UDP.
- Synchronized spawn and destroy workflow for `AHUICRSyncActor` counterparts.
- Runtime actor payload send/receive controls for sync actors and HMD pawn actors.

## Basic Usage

1. Open `HUICRPluginProject.uproject` with a compatible Unreal Engine version (UE5.6.1-v85 In the Oculus-VR fork of Unreal Engine on GitHub⁠：https://github.com/Oculus-VR/UnrealEngine/tree/oculus-5.6).
2. Enable required engine plugins for your workflow, especially nDisplay/Switchboard on PC and Meta/Oculus plugins on HMD.
3. In the HMD map, place the HMD sync manager and configure the HMD screen/calibrator workflow.
4. In the PC map, place the PC sync manager and bind the nDisplay screen components.
5. Configure PC IP and sync port from the HMD widget or manager.
6. Run connection, calibration, initial sync spawn, and game start in that order.

For detailed setup, API names, state definitions, and blueprint workflow notes, see `HUICRSync_UserGuide_Combined.docx`.

## PC and HMD Targets

The project can use separate build targets for PC and HMD packaging:

- PC target: intended for nDisplay/Switchboard builds.
- HMD target: intended for Meta/Oculus interaction and Android/Quest workflows.

This separation is important because Meta/Oculus runtime plugins can conflict with headless or nDisplay PC launches if they are packaged into the PC target.


## License

Original HUICRSync source code, plugin logic, project glue code, and documentation are released under the MIT License.

Unreal Engine, Epic template content, Meta/Oculus plugins, nDisplay/Switchboard components, Marketplace/Fab assets, and other third-party assets remain governed by their own license terms. See `THIRD_PARTY_NOTICES.md`.
