# HUICRSync Unreal Project (UE5.6.1-v85)

HUICRSync enables cross-reality (CR) applications by supporting the integration and spatial calibration of a Meta Quest 3 headset with any number of PC-driven 2D displays, regardless of their physical size or spatial arrangement. It keeps application state and runtime data synchronized between the HMD and PC, while its payload system supports commonly used data across common Unreal Engine gameplay actor types, including user-defined synchronized actors.

HUICRSync Unreal Project is a configured Unreal Engine project containing the HUICRSync runtime plugin. The plugin provides a reusable PC/HMD synchronization layer for mixed-reality workflows using UDP state packets, UDP commands, calibration state, nDisplay screen binding, and synchronized runtime actor spawning.

This repository is published as a full Unreal project instead of a plugin-only package so users can open a prepared project, inspect the PC/HMD target setup, and test the plugin with the included maps and plugin content.

Documentation: https://drive.google.com/file/d/1jxLM1swn0d_kw_9Zuo3iSvRMKuPGa8j7/view?usp=sharing

Tutorial Video: https://youtu.be/dEcZ0Hj-dEo

Full Experience Video (implemented with HUICRSync): https://youtu.be/v97zrFEHjj0

TVCG Paper DOI: `10.1109/TVCG.2026.3747530`

> [!IMPORTANT]
> The current HMD implementation supports Meta Quest 3 only. Other HMDs and Meta Quest models are not currently supported.

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

1. Open `HUICRPluginProject.uproject` with a compatible Unreal Engine version (UE5.6.1-v85 In the Oculus-VR fork of Unreal Engine on GitHub⁠：https://github.com/Oculus-VR/UnrealEngine/tree/oculus-5.6) after cloning this project.
2. Enable required engine plugins for your workflow, especially nDisplay/Switchboard on PC and Meta/Oculus plugins on HMD.
3. In the HMD map, place the HMD sync manager and configure the HMD screen/calibrator workflow.
4. In the PC map, place the PC sync manager and bind the nDisplay screen components.
5. Configure PC IP and sync port from the HMD widget or manager.
6. Run connection, calibration, initial sync spawn, and game start in that order.

### One PC avatar per headset

The sample pairs `BP_HUICRPawn_HMD` with `BP_HUICRPawnSyncActor_PC`. When a headset connects with its player pawn, the PC spawns its avatar and assigns both the same participant ID. Disconnecting removes that avatar. A Guest promoted to Host keeps its existing avatar and ID.

To make your own pair, derive the headset Blueprint from `HUICRSyncHMDPawn` and set it as the local player's pawn. Derive the PC Blueprint from `HUICRSyncActor` and select **HUI Pawn** under **Type Code**. In Class Defaults on each Blueprint, set **Remote Counterpart Class** to the other class and choose the same **Shared Payload Struct**. Include the PC avatar class in your PC build. The sample already cooks the folder containing both classes.

Tracking fields follow their owning headset by default during gameplay. On the HMD, connect **On Prepare Sync Fields** to **Write Sync Payload** with **Current Local Values**. Convert tracked world transforms with **Convert HMD Transform to PC**. On the PC avatar, connect **On Sync Fields Received** to **Read Sync Payload** with **Received Values**, then use each field's **Apply** pin to update its component. A field set on the PC avatar with **Set Sync Field Source** goes to that avatar's paired headset. The sample's existing byte-based graphs also work with this pairing.

### Spawn a Sync Actor with initial values

Add **Spawn Sync Actor** and choose the actor Blueprint in **Spawn Class**. The node shows the fields from that class's **Shared Payload Struct**. Its PC and HMD counterparts should use the same struct. Set or connect a field, such as **Color**, and the spawned actors can read the values during **BeginPlay** with **Read Sync Payload → Initial Values**.

Leaving every field untouched lets the actor provide its own initial values. Setting any field sends the whole struct, with its declared defaults for the other fields. Enable **Send Struct Defaults** to explicitly send the defaults, including zero or black. **Spawn Transform** uses PC coordinates by default. On a headset, enable **Use Local Space** to pass a transform in that headset's world.

## PC and HMD Targets

The project can use separate build targets for PC and HMD packaging:

- PC target: intended for nDisplay/Switchboard builds.
- HMD target: intended for Meta Quest 3 using the Meta/Oculus interaction and Android workflow.

This separation is important because Meta/Oculus runtime plugins can conflict with headless or nDisplay PC launches if they are packaged into the PC target.

## Citation

If you use HUICRSync as a system or as part of a research methodology, please cite the associated TVCG paper:

> Xiangpeng Fu and Mads Haahr. "Exploring Cross-Reality Transitions between Projections and Head-Mounted Displays for Immersive Digital Art." *IEEE Transactions on Visualization and Computer Graphics*, 2026. DOI: `10.1109/TVCG.2026.3747530`.

Machine-readable citation metadata is provided in [`CITATION.cff`](CITATION.cff). The TVCG DOI placeholder in that file will be replaced after the article DOI is assigned.


## License

Original HUICRSync source code, plugin logic, project glue code, and documentation are released under the MIT License.

Unreal Engine, Epic template content, Meta/Oculus plugins, nDisplay/Switchboard components, Marketplace/Fab assets, and other third-party assets remain governed by their own license terms. See `THIRD_PARTY_NOTICES.md`.

