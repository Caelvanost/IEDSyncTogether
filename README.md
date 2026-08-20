# IEDSyncTogether

Compatibility layer between Immersive Equipment Displays (IED) and Skyrim Together Reborn.

## v0.9.1 — AnimSync ownership split for Helmet Toggle animation objects

IEDSyncTogether reproduces standard IED slot displays and persistent captured IED Custom Items on the correct remote STR proxy while preserving exact scenegraph transforms.

v0.9.1 fixes an interaction with AnimSyncTogether's Helmet Toggle / GPMA replay: when `AnimSyncTogether.dll` is loaded, transient Custom Items attached to `AnimObjectR` or `AnimObjectL` are no longer recreated by IEDSyncTogether. Those animation objects are owned by AnimSync/OAR instead. Persistent displays such as a helmet at `ExtraPelvisArmorHelmet1` remain synchronized by IEDSyncTogether.

```text
local evaluated IED state
        ↓
slots + persistent custom objects
        ↓
plugin/local FormID + attachment mode + exact local transform
        ↓
LocalIEDState / STRPM
        ↓
STRPM ProxyResolver
        ↓
remote STR proxy
        ↓
IED creates/attaches IEDSyncTogether-owned Custom Items
        ↓
IEDSyncTogether watchdog maintains exact NiAVObject::local transforms
```

### Requirements

- Skyrim Together Reborn 1.8.0
- Immersive Equipment Displays
- STRPluginMessagingAPI v0.8.2 or newer compatible build with ProxyResolver available
- the relay/server files required by that STRPM build

For renderer testing, disabling IED **NPC Displays** on both clients makes it easier to distinguish normal IED NPC displays from IEDSyncTogether-owned remote displays.

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

The raw matrix remains the wire representation. No Euler angles are serialized or used for final remote orientation.

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

## Standard slot renderer

Standard favorites continue to use the raw-scenegraph renderer:

1. resolve `plugin + local FormID` on the receiving client;
2. select the evaluated remote node/anchor;
3. create an IED Custom Item owned by `IEDSyncTogether.esp`;
4. preserve left-weapon semantics where required;
5. call `IED.Evaluate`;
6. find the generated `OBJECT ... [FormID]` scene object;
7. restore its exact captured local position, raw 3x3 matrix and scale directly.

This is the path used for synchronized weapons, bows, shields and other standard favorites.

## Persistent transform watchdog

IED may re-evaluate an entry after it has been created and temporarily restore its own default transform. IEDSyncTogether therefore maintains tracked remote displays with a local watchdog:

```text
100 ms tick
   ↓
for every tracked remote IEDSyncTogether display
   ↓
measure rotation / position / scale delta
   ↓
if already correct: do nothing
if changed by IED: restore exact raw transform immediately
```

The watchdog generates no network traffic. It only reads and, when needed, repairs IEDSyncTogether-owned remote scene objects on the Skyrim game thread.

Expected correction logs:

```text
REMOTE IED WATCHDOG acquired: ...
REMOTE IED WATCHDOG corrected: ...
REMOTE IED WATCHDOG recorrected: ...
```

## Custom Item renderer

Persistent visible `IEDObjectKind::kCustom` objects are rendered remotely. The captured IED attachment mode is preserved directly from the scene graph:

```text
OBJECT P <node>  → parent attachment mode
OBJECT R <node>  → reference attachment mode
```

The transmitted form is resolved locally and the same raw scenegraph transform is maintained through the watchdog.

### Helmet Toggle / AnimSyncTogether ownership

AnimSyncTogether v0.11.x replays Helmet Toggle's GPMA graph state and `OffsetGPMA` / `OffsetGPMAStop` events on the remote STR proxy. OAR can therefore create the transient animation object itself.

Without an ownership split, both systems could render the same helmet during the animation:

```text
AnimSync + OAR → animation helmet on AnimObjectR
IEDSyncTogether → duplicate IED Custom Item on AnimObjectR
```

v0.9.1 prevents that duplication. When `AnimSyncTogether.dll` is loaded, remote decode filters transient Custom Items attached to:

```text
OBJECT P AnimObjectR
OBJECT R AnimObjectR
OBJECT P AnimObjectL
OBJECT R AnimObjectL
```

These objects are delegated to AnimSync/OAR.

Persistent IED displays are not filtered. For example, a helmet at the waist remains synchronized normally:

```text
OBJECT R ExtraPelvisArmorHelmet1
```

This gives the intended ownership model:

```text
helmet animation in hand → AnimSync/OAR
persistent helmet at waist → IEDSyncTogether
helmet back on head / absent custom → no remote IEDSync custom helmet
```

If AnimSyncTogether is not loaded, IEDSyncTogether keeps its previous behavior and can still reproduce `AnimObjectR/L` Custom Items itself.

A delegated object is visible in the log as:

```text
REMOTE IED CUSTOM delegated to AnimSync/OAR: ... attachment="OBJECT P AnimObjectR" ...
```

## Diagnostic logs

Startup should include:

```text
IEDSyncTogether v0.9.1 loading
REMOTE IED transform watchdog started: interval=100ms
```

For a standard slot:

```text
REMOTE IED SLOT queued: ... rawTransform=watchdog
```

For a persistent Custom Item:

```text
REMOTE IED CUSTOM queued: ... attachmentMode=parent|reference ... rawTransform=watchdog
```

For a transient animation object delegated to AnimSync:

```text
REMOTE IED CUSTOM delegated to AnimSync/OAR: ...
```

## Build

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1
```

Expected archive:

```text
dist/IEDSyncTogether-v0.9.1.zip
```

## v0.9.1 test protocol

Install IEDSyncTogether v0.9.1 and AnimSyncTogether v0.11.1 on both clients.

1. Keep the same Helmet Toggle / DAV configuration on both clients.
2. Connect both players to the same STR server.
3. Trigger the helmet removal animation.
4. Verify there is only one helmet in the hand during the animation.
5. Verify the log contains `REMOTE IED CUSTOM delegated to AnimSync/OAR` for `AnimObjectR`/`AnimObjectL`.
6. Verify the helmet appears once at `ExtraPelvisArmorHelmet1` after the animation when the local state places it at the waist.
7. Trigger the helmet equip animation again.
8. Verify the waist display disappears and no helmet remains at the waist once the helmet is back on the head.
9. Verify normal weapon/favorite synchronization and watchdog corrections still behave as before.
10. Send both IEDSyncTogether and AnimSyncTogether logs if any duplicate or stale helmet remains.

## Safety

IED remains responsible for creation, attachment, enable/disable and cleanup through its public Papyrus Custom Item API. IEDSyncTogether only updates the local transform of the specific scene objects it created for remote synchronization. No private IED detours are used.

## License

MIT
