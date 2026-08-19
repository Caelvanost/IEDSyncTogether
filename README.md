# IEDSyncTogether

Compatibility layer between Immersive Equipment Displays (IED) and Skyrim Together Reborn.

## v0.7.1 — evaluated IED anchor replication

The `strpm` branch implements the end-to-end standard-slot visual path:

```text
local IED evaluated state
        ↓
LocalIEDState
        ↓
STRPM transport
        ↓
remote ConnectionID
        ↓
STRPM ProxyResolver
        ↓
local STR proxy FormID
        ↓
RemoteIEDRenderer
        ↓
IED Custom Items owned by IEDSyncTogether
```

### Requirements

- Skyrim Together Reborn 1.8.0
- Immersive Equipment Displays
- STRPluginMessagingAPI v0.8.2 or newer compatible build with ProxyResolver available
- the relay/server files required by that STRPM build

For renderer testing, disable IED **NPC Displays** on both clients. This ensures remote favorites visible in game are created by IEDSyncTogether rather than IED's normal NPC display path.

## Local capture

The authoritative local capture records:

- the 19 official IED equipment slots through `IED.GetSlottedForm`;
- loaded IED `OBJECT ... [FormID]` scene objects;
- stable `plugin + local FormID` identity;
- slot/custom classification;
- visibility/culling state;
- object, attachment and evaluated anchor node names;
- local XYZ position and scale;
- raw 3x3 rotation matrix.

Helmet Toggle 2 Custom Items are captured and transported, but v0.7.1 still does not render arbitrary `IEDObjectKind::kCustom` objects remotely.

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

The renderer handles standard IED slot objects that were actually visible in the owner's evaluated scene graph.

For each visible slot object it:

1. resolves the transmitted `plugin + local FormID` on the receiving client;
2. creates an IED Custom Item on the STR proxy under the private key `IEDSyncTogether.esp`;
3. selects the remote attachment node from the **evaluated anchor captured on the owner** when available;
4. falls back to the captured `OBJECT R/P ...` attachment name, then to the generic slot node only if no evaluated anchor is available;
5. applies left-hand semantics for left weapon slots;
6. enables the item for both male/female IED configurations;
7. calls `IED.Evaluate`.

The renderer clears only Custom Items owned by `IEDSyncTogether.esp`; it does not delete another mod's IED Custom Items. Heartbeats are idempotent: an unchanged state is not rebuilt every 10 seconds.

## v0.7.1 placement fix

v0.7.0 proved that standard favorites can be rendered on the correct STR proxy, but it attached each item to the generic slot node. That discarded the result of IED placement rules such as keyword-based presets.

Example from Kahel's evaluated local scene graph:

```text
slot=0
attachment="OBJECT R WeaponSword"
anchor="MOV WeaponSwordOnBack"
```

v0.7.0 recreated that item on:

```text
WeaponSword
```

which put the spear at the hip on the remote client.

v0.7.1 instead prefers:

```text
MOV WeaponSwordOnBack
```

so keyword/profile decisions already evaluated by IED on the owning client are carried across directly. The same mechanism applies generically to anchors such as `MOV WeaponBowDefault`, `MOV ShieldBackDefault`, and other `MOV`/`CME` nodes.

This version still does not apply the captured raw position/rotation matrix explicitly. The current test isolates whether selecting the evaluated anchor is sufficient to reproduce the owner's preset placement. Exact local transform replication remains the next step if required.

## Expected logs

Startup should include:

```text
IEDSyncTogether v0.7.1 loading
STRPM ProxyResolver listener registered
STRPM adapter started: ... proxyResolverReady=1
```

For Kahel's spear on Player2, the important renderer line should now contain:

```text
REMOTE IED SLOT queued: ... slot=0 ... node="MOV WeaponSwordOnBack" nodeSource=captured-anchor ... capturedAnchor="MOV WeaponSwordOnBack"
```

rather than:

```text
node="WeaponSword"
```

## Build

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1
```

Expected archive:

```text
dist/IEDSyncTogether-v0.7.1.zip
```

## v0.7.1 test protocol

Install the same v0.7.1 archive on Player1 and Player2 together with the current STRPM/ProxyResolver build.

1. Keep IED NPC Displays disabled on both clients.
2. Connect both players to the same STR server.
3. Leave Kahel's spear favorite visible with the local IED keyword preset placing it on the back.
4. Verify that Player2 now sees the spear on the back rather than at the hip.
5. Check the log for `node="MOV WeaponSwordOnBack" nodeSource=captured-anchor`.
6. Verify the bow, shield, and Elir's visible favorites still render.
7. Send both `IEDSyncTogether.log` files if any item remains incorrectly positioned.

Helmet Toggle Custom Items are still captured/transported but remain intentionally excluded from remote rendering in v0.7.1.

## Safety

The capture path uses the public IED slot getter and Skyrim's evaluated scene graph. Proxy identity comes exclusively from the public STRPM ProxyResolver. Remote objects are created through IED's public Papyrus Custom Item API under an IEDSyncTogether-owned key. No private IED runtime detours are used.

## License

MIT
