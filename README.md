# IEDSyncTogether

Compatibility layer between Immersive Equipment Displays (IED) and Skyrim Together Reborn.

## v0.10.0-dev — proxy NPC Display isolation

This development branch builds on v0.9.1 and removes the need to disable IED **NPC Displays** globally during multiplayer.

IED itself remains completely stock. IEDSyncTogether does not patch, replace or detour `ImmersiveEquipmentDisplays.dll`.

The intended ownership model is now:

```text
normal NPC
   ↓
IED NPC Displays works normally

STR remote proxy
   ↓
IED may evaluate the actor normally
   ↓
IEDSyncTogether receives the authoritative remote-player state
   ↓
IEDSyncTogether creates its own remote Custom Items through the public IED API
   ↓
100 ms scenegraph watchdog
   ├─ keeps exactly the expected remote objects visible
   ├─ restores exact position / 3x3 rotation / scale
   └─ hides every other IED OBJECT ... [FormID] branch on that STR proxy
```

This means NPC Displays can stay **enabled** for the whole game while STR proxies visually expose only the state synchronized by IEDSyncTogether.

### Requirements

- Skyrim Special Edition / Anniversary Edition runtime supported by the project
- Skyrim Together Reborn 1.8.0
- Immersive Equipment Displays 1.7.4 stock
- STRPluginMessagingAPI v0.8.2 or newer compatible build with ProxyResolver available

No custom IED DLL is required or distributed.

## Local capture

The authoritative local capture records:

- the 19 official IED equipment slots through `IED.GetSlottedForm`;
- loaded IED `OBJECT ... [FormID]` scene objects;
- stable `plugin + local FormID` identity;
- slot/custom classification;
- visibility/culling state;
- object, attachment and evaluated anchor node names;
- exact local XYZ position;
- raw 3x3 rotation matrix;
- scale.

The raw matrix remains the wire representation. No Euler conversion is used for the final remote orientation.

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

IEDSyncTogether uses STRPM's public ProxyResolver API. It does not scan `ProcessLists`, compare actor names, or guess dynamic proxy FormIDs.

## Remote renderer

Standard favorites and persistent Custom Items are still created through IED's public Papyrus Custom Item API:

1. resolve `plugin + local FormID` on the receiving client;
2. select the captured remote node/attachment;
3. create an actor Custom Item owned by `IEDSyncTogether.esp`;
4. preserve left-weapon semantics where required;
5. call `IED.Evaluate`;
6. inspect the resulting proxy scenegraph;
7. match the generated `OBJECT ... [FormID]` object to the authoritative remote state;
8. restore its exact local position, raw 3x3 rotation matrix and scale.

No private IED runtime hooks are used.

## Proxy NPC Display isolation

With NPC Displays enabled, IED can independently create equipment displays for the STR proxy because STR proxies look like normal NPCs to IED.

v0.10.0-dev handles that at the scenegraph level rather than changing IED.

On every 100 ms watchdog tick, IEDSyncTogether inventories all proxy nodes whose object name follows IED's loaded-object form:

```text
OBJECT ... [FormID]
```

For every object in the received remote state, the watchdog searches for candidates with the same resolved FormID and the same attachment parent. If several candidates exist, it selects the one whose current transform is closest to the authoritative captured transform.

The selected instance is kept visible and corrected. Every remaining IED object on that STR proxy is application-culled.

This handles both unwanted NPC Displays and duplicates:

```text
remote state expects sword A on WeaponSword

proxy scenegraph before isolation
   sword A from normal NPC Displays
   sword A from IEDSyncTogether
   dagger B from local NPC Display evaluation

proxy scenegraph after isolation
   one sword A kept visible and aligned to the remote player
   duplicate sword A hidden
   dagger B hidden
```

The isolation applies only to proxies already tracked by IEDSyncTogether. Ordinary NPCs are never traversed or modified by this system.

Suppressed scene objects are retained through state updates. If the STR proxy mapping is removed, the objects that IEDSyncTogether itself suppressed are restored before normal IED evaluation resumes.

Expected diagnostic log:

```text
REMOTE IED ISOLATION suppressed: ... reason=not-in-authoritative-remote-state
```

The main render summary also reports:

```text
npcDisplayIsolation=1
```

## Persistent transform watchdog

The transform watchdog remains active on the selected authoritative instance:

```text
100 ms tick
   ↓
match remote state to proxy IED objects
   ↓
keep one expected instance
   ↓
measure rotation / position / scale delta
   ↓
restore exact raw transform when required
   ↓
hide every unmatched IED display on the proxy
```

Expected logs:

```text
REMOTE IED WATCHDOG acquired: ...
REMOTE IED WATCHDOG corrected: ...
REMOTE IED WATCHDOG recorrected: ...
```

## Helmet Toggle / AnimSyncTogether ownership

The v0.9.1 ownership split is retained.

When `AnimSyncTogether.dll` is loaded, transient Custom Items attached to:

```text
OBJECT P AnimObjectR
OBJECT R AnimObjectR
OBJECT P AnimObjectL
OBJECT R AnimObjectL
```

are delegated to AnimSync/OAR instead of being recreated by IEDSyncTogether.

Persistent displays such as:

```text
OBJECT R ExtraPelvisArmorHelmet1
```

remain synchronized by IEDSyncTogether.

The scenegraph isolation only targets IED `OBJECT ... [FormID]` display objects. It does not target the GPMA/OAR animation object itself.

## Build

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1
```

Expected archive:

```text
dist/IEDSyncTogether-v0.10.0.zip
```

The package contains the IEDSyncTogether plugin only. It does **not** contain `ImmersiveEquipmentDisplays.dll`.

## v0.10.0-dev test protocol

Install this build on both clients and use the normal stock IED installation on both machines.

1. Enable **NPC Displays** in IED on both clients.
2. Before connecting to STR, verify that normal NPCs still show their normal IED equipment displays.
3. Connect both players to the same STR server.
4. Verify the remote STR proxy does not gain additional local NPC Display weapons/items that are absent from the remote player's IED state.
5. Equip or favorite several weapon types and verify exactly one synchronized copy is visible remotely.
6. Test sword, bow/quiver, shield and at least one left-hand weapon if available.
7. Test Helmet Toggle removal and re-equip with AnimSyncTogether.
8. Verify the transient hand helmet remains owned by AnimSync/OAR and is not suppressed as an IED duplicate.
9. Verify a persistent waist helmet remains synchronized once when the remote state contains it.
10. Change cell or reconnect and verify proxy isolation resumes once ProxyResolver remaps the actor.
11. Disconnect and verify ordinary NPC displays still operate normally.

Useful log markers:

```text
IEDSyncTogether v0.10.0 loading
REMOTE IED transform/isolation watchdog started: interval=100ms
REMOTE IED ISOLATION suppressed: ...
REMOTE IED RENDER queued: ... npcDisplayIsolation=1
```

## Safety

- stock IED DLL only;
- public IED Papyrus API for Custom Item creation and cleanup;
- public STRPluginMessagingAPI / ProxyResolver for player-to-proxy identity;
- scenegraph reads, local transform correction and application-culling are performed only on tracked STR proxies;
- no IED detours;
- no IED binary patch;
- no dependency on reproducing IED's build environment.

## License

MIT
