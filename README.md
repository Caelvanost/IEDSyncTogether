# IEDSyncTogether

Compatibility layer between Immersive Equipment Displays (IED) and Skyrim Together Reborn.

## v0.6.1 — resilient STRPM state transport

The `strpm` branch has two active responsibilities:

1. capture the authoritative evaluated IED state of the local `PlayerCharacter`;
2. publish/receive that serialized state through STRPluginMessagingAPI (STRPM).

Remote rendering is deliberately still disabled. v0.6.1 hardens state delivery before received state is applied to Skyrim Together proxies.

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

On receive, v0.6.1:

- ignores local loopback by `ConnectionID`;
- validates payload size;
- decodes the payload back into `LocalIEDState`;
- logs sender identity, sequence, slot count, object count and Custom Item count;
- does **not** yet render anything on the remote proxy.

### v0.6.1 delivery behavior

The adapter now keeps the latest complete local snapshot independently from immediate transport availability.

If a state changes before the Skyrim Together transport is ready:

```text
capture
  -> immediate send fails
  -> latest state remains pending
  -> retry every 1 second
  -> connection becomes usable
  -> latest state is delivered automatically
```

Only a change in the failure result is logged, so being offline does not generate one warning every second.

After the first successful send, the current complete state is also retransmitted every 10 seconds as a heartbeat. This lets a player who joins the STR session later receive the current IED state without requiring the owner to equip/unequip something artificially.

`kPreLoadGame` clears the pending snapshot so state from a previous save cannot be replayed into a newly loaded game.

The adapter also refreshes the STRPM local `ConnectionID` and display name before sends, allowing identity established after initial game startup to replace early values such as `Prisoner`.

Expected transport logs include:

```text
STRPM adapter started: channel=strpm.iedsynctogether.state.v1 player="..." connectionID=... retry=1s heartbeat=10s
STRPM IED STATE TX deferred: reason=change result=transport error bytes=...
STRPM IED transport resumed; latest pending state delivered
STRPM IED STATE TX: reason=pending bytes=... objects=... customObjects=...
STRPM IED STATE TX: reason=change bytes=... objects=... customObjects=...
STRPM IED STATE TX heartbeat: bytes=... objects=... customObjects=...
STRPM IED STATE RX: sender=... name="..." sequence=... bytes=... slots=... objects=... customObjects=... (remote rendering disabled)
```

### Runtime cleanup

The `strpm` CMake target does not compile the old IEDSyncTogether networking/runtime stack:

- `Config`
- `ProxyResolver`
- `StrServerDiscovery`
- legacy `StrTransport`
- `UdpTransport`
- `SyncService`

Those source files remain in repository history for reference, but are not part of the `strpm` runtime. The package does not install the obsolete IEDSyncTogether network INI.

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
   |         |
   |         +-- pending retry (1 s)
   +------------ current-state heartbeat (10 s)
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
        +-- v0.6.1: log/validate only
        +-- next: proxy association + remote renderer
```

## Build

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1
```

Expected archive:

```text
dist/IEDSyncTogether-v0.6.1.zip
```

## Test protocol

Install the same v0.6.1 archive on both players together with the current STRPluginMessagingAPI build.

1. Launch both Skyrim clients but leave at least one client disconnected from the STR server long enough for its first local IED state to be captured.
2. Confirm that client logs one `STRPM IED STATE TX deferred`.
3. Connect both players to the same STR server without changing equipment.
4. Confirm the previously pending state is automatically sent with `reason=pending` and received by the other player.
5. Wait at least 10 seconds without changing equipment and confirm a heartbeat is sent/received.
6. Change a normal IED favorite and trigger Helmet Toggle remove/equip on each player.
7. Send both `IEDSyncTogether.log` files from the same session.

With NPC Displays disabled, no remote gear is expected to appear yet: v0.6.1 validates delivery only. The next stage is associating each received state with the STR proxy actor and applying the renderer.

## Safety

The capture path uses the public IED slot getter and Skyrim's evaluated scene graph. It does not detour private IED runtime functions and does not mutate IED controller/configuration state. The v0.6.1 receive path only decodes/logs remote state; it does not modify remote actors yet.

## License

MIT
