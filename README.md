# IEDSyncTogether

Compatibility layer between Immersive Equipment Displays (IED) and Skyrim Together Reborn.

## v0.8.0 — raw scenegraph transform replication

The `strpm` branch now reproduces standard IED slot displays without converting the captured rotation matrix through Euler angles.

```text
local evaluated IED object
        ↓
anchor + exact local position + raw 3x3 matrix + scale
        ↓
LocalIEDState / STRPM
        ↓
STRPM ProxyResolver
        ↓
remote STR proxy
        ↓
IED creates/attaches the Custom Item
        ↓
IEDSyncTogether restores the exact NiAVObject::local transform
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
- exact local XYZ position;
- raw 3x3 rotation matrix;
- scale.

The raw matrix remains the wire representation. Helmet Toggle 2 Custom Items are still captured and transported, but v0.8.0 continues to render only standard IED slot objects remotely.

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

## Why v0.8.0 exists

v0.7.1 through v0.7.4 established several useful facts:

- the correct remote STR proxy is resolved;
- the correct standard favorites are reproduced;
- the evaluated `MOV ...` anchors are captured correctly;
- parent attachment mode is accepted for those evaluated anchors;
- but weapon orientation still differs remotely.

The remaining weak point was the conversion path:

```text
captured NiMatrix3
    ↓
Euler decomposition
    ↓
IED.SetItemRotationActor(Pitch, Roll, Yaw)
    ↓
IED rebuilds a matrix
```

Even when the intended Euler convention was matched, the result still did not reproduce the owner's visual orientation exactly.

v0.8.0 removes that conversion entirely.

## Remote renderer in v0.8.0

IED is still used through its public Papyrus API to own the remote display lifecycle:

1. create the Custom Item under `IEDSyncTogether.esp`;
2. select the evaluated anchor;
3. use parent attachment mode for captured `MOV ...` / `CME ...` anchors;
4. apply left-weapon semantics when required;
5. enable the entry;
6. call `IED.Evaluate`.

IEDSyncTogether deliberately does **not** send position/rotation/scale through the Papyrus transform setters anymore.

After `IED.Evaluate`, IEDSyncTogether searches the remote proxy scene graph for the newly created IED object using:

- the receiving client's resolved full FormID;
- the expected IED attachment node (`OBJECT P ...` or `OBJECT R ...`).

Once found, it writes the captured transform directly:

```text
remoteObject.local.translate = captured position
remoteObject.local.rotate    = captured raw 3x3 matrix
remoteObject.local.scale     = captured scale
```

It then updates world-space data/bounds from that local transform.

This means no Euler convention exists in the final orientation path.

## Raw patch retries

IED evaluation/model loading can complete after the Papyrus call returns, so v0.8.0 does not assume the object exists immediately.

Each expected standard display is searched for over multiple game-task passes. Once found, the raw transform is re-applied several times during the short settling period so asynchronous IED replacement/rebuild work cannot leave an unpatched first object behind.

Unchanged heartbeats also queue a lightweight raw-transform refresh instead of rebuilding the whole IED entry. This provides a recovery path if another later IED evaluation resets the custom display transform.

## Diagnostic logs

Startup should include:

```text
IEDSyncTogether v0.8.0 loading
STRPM branch mode: ... raw-scenegraph standard-slot renderer ...
```

A standard display is first queued through IED:

```text
REMOTE IED SLOT queued: ...
node="MOV WeaponSwordOnBack"
attachmentMode=parent
rawTransform=deferred
expectedParent="OBJECT P MOV WeaponSwordOnBack"
```

When the exact remote scene object appears, v0.8.0 logs the measured difference before and after the direct patch:

```text
REMOTE IED RAW XFORM applied: ...
beforeDelta(rot=...,pos=...,scale=...)
afterDelta(rot=0.000000,pos=0.000000,scale=0.000000)
expectedRot=[...]
beforeRot=[...]
afterRot=[...]
```

After repeated successful settling passes:

```text
REMOTE IED RAW XFORM verified: ...
matrixDelta=0.000000
positionDelta=0.000000
scaleDelta=0.000000
```

These values are the decisive v0.8.0 diagnostic. They tell us whether the exact local transform reached the actual remote IED object, independently of visual interpretation.

## Build

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1
```

Expected archive:

```text
dist/IEDSyncTogether-v0.8.0.zip
```

## v0.8.0 test protocol

Install the same v0.8.0 archive on Player1 and Player2 together with the current STRPM/ProxyResolver build.

1. Keep IED NPC Displays disabled on both clients.
2. Connect both players to the same STR server.
3. Leave Kahel's spear, bow and shield visible with the normal local IED presets active.
4. Compare orientation and placement on Player2.
5. Verify the log contains `REMOTE IED RAW XFORM applied` for every visible standard slot.
6. Verify `afterDelta` and the later `REMOTE IED RAW XFORM verified` deltas are zero or extremely close to zero.
7. Check the spear/backpack combination specifically.
8. Verify Elir's visible favorites in the opposite direction.
9. Send both `IEDSyncTogether.log` files if anything still differs visually.

If the raw local matrices match exactly but the world-space visuals still differ, the next diagnostic target is no longer the item transform itself: it is the corresponding owner/remote anchor world transform and skeleton/node-override chain.

## Safety

The plugin still uses IED's public Papyrus API for Custom Item creation, attachment, enable/disable and cleanup. v0.8.0 additionally updates the local transform of the specific IEDSyncTogether-created scene object after it exists. It does not detour or patch private IED functions, and it does not modify unrelated IED entries.

## License

MIT
