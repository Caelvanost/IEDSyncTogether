# IEDSyncTogether

Compatibility layer between Immersive Equipment Displays (IED) and Skyrim Together Reborn.

## v0.5.1 — STRPM local-capture branch

The `strpm` branch is intentionally focused on **authoritative local capture**. Network transport and remote rendering remain disabled while the capture model is validated. The final transport is expected to be provided by STRPluginMessagingAPI (STRPM).

### What is captured

Every ~400 ms, only when the previous capture has completed, IEDSyncTogether snapshots the local `PlayerCharacter`:

- the 19 official IED equipment slots through `IED.GetSlottedForm`;
- every loaded IED scene object whose node follows IED's `OBJECT ... [FormID]` naming convention;
- stable `plugin + local FormID` identity;
- slot/custom object classification;
- visibility/culling state;
- object, attachment and anchor node names;
- local XYZ position and scale;
- the raw 3x3 rotation matrix.

Rotation remains a matrix at the capture boundary so no Euler convention is imposed before rendering.

### v0.5.1 changes

- Adds a dedicated `LocalIEDState` model independent of the capture probe.
- Adds deterministic `EncodeLocalIEDState` / `DecodeLocalIEDState` serialization for the future STRPM adapter.
- Deduplicates repeated scene-graph visits by `NiAVObject*` identity.
- Performs a second logical deduplication pass for equivalent IED objects.
- Stores the last complete local state and serialized payload for later publication through STRPM.
- Keeps Helmet Toggle 2 Custom Items generic: no Helmet Toggle-specific preset name, Papyrus key or hard-coded FormID is required.

### Custom Items / Helmet Toggle 2

Loaded display objects are discovered through IED node names such as:

```text
OBJECT WEAPON [XXXXXXXX]
OBJECT ARMOR [XXXXXXXX]
OBJECT SHIELD [XXXXXXXX/XXXXXXXX]
OBJECT MISC [XXXXXXXX]
```

Objects whose FormID is not one of the 19 currently captured equipment-slot forms are represented as `IEDObjectKind::kCustom`. This captures the evaluated Helmet Toggle visual result, including transitions between pelvis/waist attachment, `AnimObjectR`, and absence from the scene graph.

## Log format

When the serialized local visual state changes:

```text
LOCAL IED STATE CHANGED: slots=... sceneObjects=... customObjects=... payloadBytes=...
LOCAL SLOT: slot=... plugin="..." localForm=...
LOCAL IED OBJECT: kind=slot|custom slot=... visible=... plugin="..." localForm=... object="..." attachment="..." anchor="..." pos=(...) scale=... rotM=[...]
```

For a single Helmet Toggle Custom Item, `customObjects` should now normally be `1`, not `2`.

## Architecture

```text
IED / local PlayerCharacter
        |
        v
LocalCaptureProbe
        |
        v
LocalIEDState
  |          |
  |          +-- objects: slots + Custom Items + raw transforms
  +-- slots: 19 GetSlottedForm results
        |
        v
EncodeLocalIEDState()
        |
        v
future STRPM adapter
```

No UDP peer discovery, LAN snapshot exchange or remote rendering is started by v0.5.1 on this branch.

## Build

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1
```

Expected archive:

```text
dist/IEDSyncTogether-v0.5.1.zip
```

## Test protocol

1. Build and install v0.5.1 on Player1 only.
2. Load Kahel with IED and Helmet Toggle 2 enabled.
3. Leave normal IED favorites visible.
4. Remove and equip the helmet several times with Helmet Toggle.
5. Send `IEDSyncTogether.log`.

Validate that normal slot objects are unique and that each Helmet Toggle visual state produces one logical Custom Item entry when present.

## Safety

This branch uses the public IED slot getter and Skyrim's evaluated scene graph. It does not detour private IED runtime functions and does not mutate IED controller/configuration state.

## License

MIT
