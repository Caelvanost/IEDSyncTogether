# IEDSyncTogether

Compatibility layer between Immersive Equipment Displays (IED) and Skyrim Together Reborn.

## v0.7.2 — evaluated IED anchor + transform replication

The `strpm` branch now reproduces the standard-slot visual result already evaluated by IED on the owning client:

```text
local IED evaluated state
        ↓
anchor + position + rotation matrix + scale
        ↓
LocalIEDState / STRPM
        ↓
STRPM ProxyResolver
        ↓
remote STR proxy
        ↓
IED Custom Item owned by IEDSyncTogether
```

### Requirements

- Skyrim Together Reborn 1.8.0
- Immersive Equipment Displays
- STRPluginMessagingAPI v0.8.2 or newer compatible build with ProxyResolver available
- the relay/server files required by that STRPM build

For renderer testing, disable IED **NPC Displays** on both clients so remote favorites visible in game are created by IEDSyncTogether rather than IED's normal NPC display path.

## Local capture

The authoritative local capture records:

- the 19 official IED equipment slots through `IED.GetSlottedForm`;
- loaded IED `OBJECT ... [FormID]` scene objects;
- stable `plugin + local FormID` identity;
- slot/custom classification;
- visibility/culling state;
- object, attachment and evaluated anchor node names;
- evaluated local XYZ position;
- raw 3x3 rotation matrix;
- scale.

The raw matrix remains the wire representation. IEDSyncTogether does not serialize Euler angles, avoiding an unnecessary convention change during transport.

Helmet Toggle 2 Custom Items are still captured and transported, but v0.7.2 continues to render only standard IED slot objects remotely.

## STRPM transport and ProxyResolver

State is sent on:

```text
strpm.iedsynctogether.state.v1
```

using reliable + ordered STRPM messages.

The resilient delivery behavior remains active:

- changed state is retained as pending while transport is unavailable;
- pending state retries every 1 second;
- the current full state is retransmitted every 10 seconds as a heartbeat;
- late-joining peers receive the current state without forcing an equipment change.

IEDSyncTogether uses STRPM's public ProxyResolver API only. It does not scan `ProcessLists`, compare actor names, or guess dynamic proxy FormIDs. Proxy mapping callbacks are forwarded to the SKSE game-task queue before actor lookup or IED rendering.

## Remote renderer

For every visible standard IED slot object, v0.7.2:

1. resolves the transmitted `plugin + local FormID` on the receiving client;
2. creates an IED Custom Item on the correct STR proxy under `IEDSyncTogether.esp`;
3. prefers the evaluated `MOV ...` / `CME ...` anchor captured on the owner;
4. falls back to the captured attachment or generic slot node only if needed;
5. reconstructs the captured raw 3x3 rotation matrix as `RE::NiMatrix3`;
6. converts that matrix to XYZ Euler angles only at the IED Papyrus boundary;
7. applies `SetItemPositionActor`, `SetItemRotationActor`, and `SetItemScaleActor` for both male and female configurations;
8. applies left-hand semantics where applicable;
9. enables the item and calls `IED.Evaluate`.

The renderer clears only Custom Items owned by `IEDSyncTogether.esp`. Heartbeats remain idempotent: unchanged remote state is not rebuilt every 10 seconds.

## Why v0.7.2 exists

v0.7.1 validated the evaluated anchor selection. For example Kahel's spear correctly arrived at Player2 on:

```text
MOV WeaponSwordOnBack
```

instead of the generic hip node `WeaponSword`.

However the weapon orientation was still wrong because only the anchor was replicated. Kahel's local evaluated spear state also contains:

```text
pos=(-11.891,1.917,6.666)
rotM=[0.069,-0.211,0.975;-0.808,-0.586,-0.070;0.586,-0.783,-0.210]
scale=1.000
```

v0.7.2 applies those values to the remote IED Custom Item. This is intended to carry the final result of IED placement rules, including keyword-based weapon placement and additional offsets used to coexist with backpacks, without reimplementing those conditions inside IEDSyncTogether.

## Expected logs

Startup should include:

```text
IEDSyncTogether v0.7.2 loading
STRPM ProxyResolver listener registered
STRPM adapter started: ... proxyResolverReady=1
```

For a rendered slot, the log now includes the exact transform sent to IED:

```text
REMOTE IED SLOT queued: ...
node="MOV WeaponSwordOnBack"
nodeSource=captured-anchor
pos=(-11.891,1.917,6.666)
rotDeg=(...,...,...)
scale=1.000
capturedAttachment="OBJECT R WeaponSword"
capturedAnchor="MOV WeaponSwordOnBack"
```

## Build

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1
```

Expected archive:

```text
dist/IEDSyncTogether-v0.7.2.zip
```

## v0.7.2 test protocol

Install the same v0.7.2 archive on Player1 and Player2 together with the current STRPM/ProxyResolver build.

1. Keep IED NPC Displays disabled on both clients.
2. Connect both players to the same STR server.
3. Leave Kahel's spear, bow and shield visible with the normal local IED presets active.
4. Compare their remote position and orientation against Kahel's local display.
5. Pay particular attention to the spear/backpack combination: the spear should use `MOV WeaponSwordOnBack` and should now reproduce the captured position/rotation rather than merely attaching to the correct node.
6. Verify Elir's visible favorite in the opposite direction.
7. Send both `IEDSyncTogether.log` files and describe any remaining visual difference.

Helmet Toggle Custom Items remain intentionally excluded from remote rendering in v0.7.2; they are still captured and transported for the next stage.

## Safety

The capture path uses the public IED slot getter and Skyrim's evaluated scene graph. Proxy identity comes exclusively from the public STRPM ProxyResolver. Remote objects are created and transformed through IED's public Papyrus Custom Item API under an IEDSyncTogether-owned key. No private IED runtime detours are used.

## License

MIT
