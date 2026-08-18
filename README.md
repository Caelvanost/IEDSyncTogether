# IEDSyncTogether

Compatibility layer between Immersive Equipment Displays (IED) and Skyrim Together Reborn.

## v0.5.0 — STRPM local-capture branch

The `strpm` branch is now intentionally focused on **authoritative local capture**. Network transport and remote rendering are disabled in this build while the capture model is validated. The final transport is expected to be provided by STRPluginMessagingAPI (STRPM).

### What is captured

Every ~400 ms, only when the previous capture has completed, IEDSyncTogether snapshots the local `PlayerCharacter`:

- the 19 official IED equipment slots through `IED.GetSlottedForm`;
- every loaded IED scene object whose node follows IED's own `OBJECT ... [FormID]` naming convention;
- stable `plugin + local FormID` identity;
- whether the scene object is currently culled/visible;
- the IED object node name;
- its attachment node;
- its parent anchor node;
- local XYZ position;
- local scale;
- the **raw 3x3 rotation matrix**.

Rotation is intentionally kept as a matrix at the capture boundary. This avoids losing information or choosing an Euler convention before the future renderer/STRPM adapter actually needs one.

### Custom Items / Helmet Toggle 2

IED internally names loaded display objects using forms such as:

```text
OBJECT WEAPON [XXXXXXXX]
OBJECT ARMOR [XXXXXXXX]
OBJECT SHIELD [XXXXXXXX/XXXXXXXX]
OBJECT MISC [XXXXXXXX]
```

The probe traverses the local player's scene graph and parses these objects directly. Objects whose FormID is not one of the 19 currently captured equipment-slot forms are reported as `kind=custom`.

That makes the capture path suitable for IED Custom Items used by Helmet Toggle 2: when the helmet Custom Item is loaded, its form identity, visibility state, attachment/anchor and transform are part of the local snapshot. The capture does not depend on knowing Helmet Toggle's preset name or Papyrus key.

This version deliberately captures the **evaluated visual result**, not the preset configuration that produced it.

## Log format

When the local visual state changes, the log contains:

```text
LOCAL IED STATE CHANGED: slots=... sceneObjects=... customCandidates=...
LOCAL SLOT: slot=... plugin="..." localForm=...
LOCAL IED OBJECT: kind=slot|custom slot=... visible=... plugin="..." localForm=... object="..." attachment="..." anchor="..." pos=(...) scale=... rotM=[...]
```

For Helmet Toggle testing, compare the log before, during and after the remove/equip animation. The helmet should appear as a `kind=custom` object and its `visible`, attachment and/or transform should change with the local Helmet Toggle state.

## Current architecture

```text
IED / local PlayerCharacter
        |
        v
LocalCaptureProbe
        |
        +-- 19 IED equipment slots
        +-- loaded IED OBJECT nodes
        +-- Custom Item candidates
        +-- raw transforms
        |
        v
LocalIEDState
        |
        v
future STRPM adapter
```

No UDP peer discovery, LAN snapshot exchange or remote Custom Item rendering is started by v0.5.0 on this branch.

## Build

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1
```

Expected archive:

```text
dist/IEDSyncTogether-v0.5.0.zip
```

## First test protocol

1. Build and install v0.5.0 on Player1 only; Player2 is not needed for capture validation.
2. Load Kahel with IED and Helmet Toggle 2 enabled.
3. Leave the spear/shield favorites in their intended local positions.
4. Trigger Helmet Toggle remove/equip several times.
5. Send `IEDSyncTogether.log`.

The important validation is that the log independently records the favorite slot forms and the Helmet Toggle Custom Item visual state.

## Safety

This branch uses the public IED slot getter and Skyrim's evaluated scene graph. It does not detour private IED runtime functions and does not mutate IED's controller/configuration state.

## License

MIT
