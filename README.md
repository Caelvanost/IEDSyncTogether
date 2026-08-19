# IEDSyncTogether

Compatibility layer between Immersive Equipment Displays (IED) and Skyrim Together Reborn.

## v0.7.0 — STRPM ProxyResolver + remote standard-slot renderer

The `strpm` branch now implements the first complete end-to-end visual path:

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
- STRPluginMessagingAPI **v0.8.2 or newer compatible build** with ProxyResolver available
- the STRPM relay/server files required by that STRPM build

For v0.7.0 testing, disable IED **NPC Displays** on both clients. This prevents IED's native NPC display system from masking whether the remote equipment is actually being created by IEDSyncTogether.

## Local capture

The authoritative local capture remains the validated v0.5.x/v0.6.x path:

- 19 official IED equipment slots through `IED.GetSlottedForm`;
- loaded IED `OBJECT ... [FormID]` scene objects;
- stable `plugin + local FormID` identity;
- slot/custom classification;
- visibility/culling state;
- object, attachment and anchor node names;
- local XYZ position and scale;
- raw 3x3 rotation matrix.

Helmet Toggle 2 Custom Items are still captured and transported, but v0.7.0 deliberately does **not** render custom objects remotely yet.

## STRPM transport

State is sent on:

```text
strpm.iedsynctogether.state.v1
```

using reliable + ordered STRPM messages.

The v0.6.1 delivery safeguards remain active:

- changed state is retained as pending if transport is unavailable;
- pending state retries every 1 second;
- the current full state is retransmitted every 10 seconds as a heartbeat;
- a late-joining peer can therefore receive the current state without forcing an equipment change.

## ProxyResolver

v0.7.0 uses STRPM's public ProxyResolver API only. IEDSyncTogether does not scan `ProcessLists`, compare actor names, or guess dynamic proxy FormIDs.

A received sender `ConnectionID` is resolved to the local STR proxy FormID. Both ordering cases are supported:

```text
state arrives first   -> keep state -> mapping event -> render
mapping exists first  -> state RX   -> resolve immediately -> render
```

Proxy mapping callbacks are never used to mutate Skyrim directly. They are forwarded to the SKSE game-task queue before actor lookup or IED rendering.

Mapping lifecycle events also clean up IEDSyncTogether-owned displays when proxies are updated, removed, or cleared.

## Remote renderer in v0.7.0

This first renderer handles only **standard IED slot objects that were actually visible in the owner's evaluated scene graph**.

For each visible slot object it:

1. resolves the transmitted `plugin + local FormID` on the receiving client;
2. creates an IED Custom Item on the STR proxy under the private key `IEDSyncTogether.esp`;
3. attaches it to the corresponding standard IED weapon/shield/ammo node;
4. applies left-hand semantics for left weapon slots;
5. enables the item for both male/female IED configurations;
6. calls `IED.Evaluate`.

The renderer clears only Custom Items owned by `IEDSyncTogether.esp`; it does not delete another mod's IED Custom Items.

Heartbeats are idempotent: an unchanged state is not rebuilt every 10 seconds.

### Deliberately deferred

v0.7.0 does not yet apply the captured placement matrix/offset to the proxy. The initial goal is to validate that the correct standard favorites appear on the correct remote STR actor with NPC Displays disabled.

It also ignores `IEDObjectKind::kCustom` during rendering. Helmet Toggle's belt/hand helmet object remains captured and transported and will be enabled after the standard-slot renderer is validated.

## Expected logs

Startup should include:

```text
IEDSyncTogether v0.7.0 loading
STRPM ProxyResolver listener registered
STRPM adapter started: ... proxyResolverReady=1
STRPM branch mode: ... standard-slot remote renderer ...
```

When the other player is mapped:

```text
STRPM proxy mapping added: connection=... proxy=FF......
```

On state reception:

```text
STRPM IED STATE RX: sender=... name="Kahel" ... renderQueued=1
REMOTE IED mapping resolved: connection=... name="Kahel" proxy=FF......
REMOTE IED SLOT queued: ... slot=... plugin="..." localForm=... node="..."
REMOTE IED RENDER queued: ... visibleSlots=... dispatchAccepted=1 customObjectsIgnored=...
```

The equivalent sequence should occur for Elir on the other client.

## Build

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1
```

Expected archive:

```text
dist/IEDSyncTogether-v0.7.0.zip
```

## v0.7.0 test protocol

Install the same v0.7.0 archive on Player1 and Player2 together with STRPM v0.8.2-compatible files.

1. Disable IED NPC Displays on both clients before connecting.
2. Start both games and connect them to the same STR server.
3. Keep at least one normal IED favorite visible on each local character.
4. Check whether those standard favorites appear on the other player's STR proxy.
5. Equip/unequip or change one normal favorite on each player and verify the remote display updates.
6. Helmet Toggle may be exercised for logging, but its custom helmet object is not expected to render remotely in v0.7.0.
7. Send both `IEDSyncTogether.log` files from the same session.

The decisive v0.7.0 result is: **with NPC Displays disabled, standard IED favorites are created by IEDSyncTogether on the correct proxy actor in both directions.**

## Safety

The capture path uses the public IED slot getter and Skyrim's evaluated scene graph. Proxy identity comes exclusively from the public STRPM ProxyResolver. Remote objects are created through IED's public Papyrus Custom Item API under an IEDSyncTogether-owned key. No private IED runtime detours are used.

## License

MIT
