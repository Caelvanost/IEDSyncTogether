# IEDSyncTogether

Compatibility layer between Immersive Equipment Displays (IED) and Skyrim Together Reborn.

## v0.6.0 — STRPM state transport

The `strpm` branch now has two active responsibilities:

1. capture the authoritative evaluated IED state of the local `PlayerCharacter`;
2. publish/receive that serialized state through STRPluginMessagingAPI (STRPM).

Remote rendering is deliberately still disabled. v0.6.0 validates transport independently before the received state is applied to Skyrim Together proxies.

### Local state

Every ~400 ms, only when the previous capture has completed, IEDSyncTogether captures:

- the 19 official IED equipment slots through `IED.GetSlottedForm`;
- loaded IED `OBJECT ... [FormID]` scene objects;
- stable `plugin + local FormID` identity;
- explicit slot/custom classification;
- visibility state;
- object, attachment and anchor node names;
- local XYZ position and scale;
- raw 3x3 rotation matrix.

Helmet Toggle 2 Custom Items are captured generically from the evaluated scene graph. No Helmet Toggle-specific preset name, Papyrus key or hard-coded FormID is required.

### STRPM transport

Changed `LocalIEDState` snapshots are encoded by `EncodeLocalIEDState()` and sent through the public STRPM v2 API on:

```text
strpm.iedsynctogether.state.v1
```

Messages are sent to `TargetKind::kAllPlayers` with reliable + ordered flags.

On receive, v0.6.0:

- ignores local loopback by `ConnectionID`;
- validates payload size;
- decodes the payload back into `LocalIEDState`;
- logs sender identity, sequence, slot count, object count and Custom Item count;
- does **not** yet render anything on the remote proxy.

Expected transport logs:

```text
STRPM adapter started: channel=strpm.iedsynctogether.state.v1 player="..." connectionID=...
STRPM IED STATE TX: bytes=... objects=... customObjects=...
STRPM IED STATE RX: sender=... name="..." sequence=... bytes=... slots=... objects=... customObjects=... (remote rendering disabled)
```

### Runtime cleanup

The v0.6.0 CMake target no longer compiles the old IEDSyncTogether networking/runtime stack:

- `Config`
- `ProxyResolver`
- `StrServerDiscovery`
- legacy `StrTransport`
- `UdpTransport`
- `SyncService`

Those source files remain in repository history for reference, but are not part of the `strpm` runtime. The package also no longer installs the obsolete IEDSyncTogether network INI.

## Architecture

```text
IED / local PlayerCharacter
        |
        v
LocalCaptureProbe
        |
        v
LocalIEDState
        |
        v
EncodeLocalIEDState()
        |
        v
STRPMAdapter
        |
        v
STRPluginMessagingAPI / Skyrim Together
        |
        v
STRPMAdapter (remote client)
        |
        v
DecodeLocalIEDState()
        |
        v
Remote LocalIEDState
        |
        +-- v0.6.0: log/validate only
        +-- next: remote renderer
```

## Build

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1
```

Expected archive:

```text
dist/IEDSyncTogether-v0.6.0.zip
```

## Test protocol

For transport validation, install the same v0.6.0 archive on both players together with the current STRPluginMessagingAPI build.

1. Launch both clients and connect through Skyrim Together.
2. Confirm both logs contain `STRPM adapter started`.
3. Change an IED favorite on Player1.
4. Trigger Helmet Toggle remove/equip on Player1.
5. Repeat one state change on Player2.
6. Send both `IEDSyncTogether.log` files.

The important validation is that every local `LOCAL IED STATE CHANGED` produces a matching `STRPM IED STATE TX` and a corresponding `STRPM IED STATE RX` on the other client with matching payload size/object counts.

## Safety

The capture path uses the public IED slot getter and Skyrim's evaluated scene graph. It does not detour private IED runtime functions and does not mutate IED controller/configuration state. The v0.6.0 receive path only decodes/logs remote state; it does not modify remote actors yet.

## License

MIT
