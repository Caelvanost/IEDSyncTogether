# IEDSyncTogether

Compatibility layer between Immersive Equipment Displays (IED) and Skyrim Together Reborn.

## v0.7.4 — reproduce evaluated slot transforms in parent attachment mode

The `strpm` branch reproduces the standard-slot visual result already evaluated by IED on the owning client:

```text
local IED evaluated state
        ↓
anchor + position + raw rotation matrix + scale
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

The raw matrix remains the wire representation. Helmet Toggle 2 Custom Items are still captured and transported, but v0.7.4 continues to render only standard IED slot objects remotely.

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

## Why v0.7.4 exists

v0.7.1 selected the correct evaluated `MOV ...` anchor, while v0.7.2/v0.7.3 applied the captured position/rotation/scale. The remote weapons still had incorrect orientation.

The remaining problem was IED's **attachment mode**.

IED Custom Items default to **reference mode**. In reference mode, IED evaluates an object transform as:

```text
item.local = referenceNode.local * configuredTransform
```

For standard IED equipment, the scene graph captured by IEDSyncTogether already contains the evaluated `OBJECT WEAPON` / `OBJECT SHIELD` local transform in the frame of its `MOV` parent. For example:

```text
MOV WeaponSwordOnBack
  └─ OBJECT R WeaponSword
       └─ OBJECT WEAPON [...]
```

The captured `OBJECT WEAPON.local` is therefore already the transform to reproduce **under the MOV frame**. v0.7.1-v0.7.3 targeted `MOV WeaponSwordOnBack` while leaving the Custom Item in IED's default reference mode, causing IED to pre-multiply `MOV.local` again.

v0.7.4 changes evaluated `MOV ...` / `CME ...` targets to IED **parent attachment mode**:

```text
SetItemAttachmentModeActor(..., 1, false)
```

IED then creates the Custom Item under the captured anchor and applies the transmitted transform directly:

```text
item.local = configuredTransform
```

This matches the coordinate space of the captured standard-slot object and avoids double-applying the anchor transform.

The IED NodeMap contains managed nodes such as `WeaponSword`, `WeaponBow`, etc., but not the scene helper names prefixed with `MOV ` or `CME `. Therefore these captured anchors can legally use parent attachment mode. Generic managed-node fallbacks continue to use reference mode.

## Remote renderer

For every visible standard IED slot object, v0.7.4:

1. resolves the transmitted `plugin + local FormID` on the receiving client;
2. creates an IED Custom Item on the correct STR proxy under `IEDSyncTogether.esp`;
3. prefers the evaluated `MOV ...` / `CME ...` anchor captured on the owner;
4. switches those captured anchors to **parent attachment mode**;
5. applies the captured local position, raw rotation reconstructed at the IED Papyrus boundary, and scale directly in the anchor frame;
6. falls back to captured attachment/generic slot nodes in reference mode only when no evaluated anchor is available;
7. applies left-hand semantics where applicable;
8. enables the item and calls `IED.Evaluate`.

The renderer clears only Custom Items owned by `IEDSyncTogether.esp`. Heartbeats remain idempotent.

## Expected logs

Startup should include:

```text
IEDSyncTogether v0.7.4 loading
STRPM ProxyResolver listener registered
STRPM adapter started: ... proxyResolverReady=1
```

For Kahel's spear on Player2, the important renderer line should now include:

```text
REMOTE IED SLOT queued: ...
node="MOV WeaponSwordOnBack"
nodeSource=captured-anchor
attachmentMode=parent
pos=(-11.891,1.917,6.666)
rotExtrinsicDeg=(...,...,...)
scale=1.000
```

The bow and shield should likewise use `attachmentMode=parent` when their captured anchors are `MOV ...` nodes.

## Build

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1
```

Expected archive:

```text
dist/IEDSyncTogether-v0.7.4.zip
```

## v0.7.4 test protocol

Install the same v0.7.4 archive on Player1 and Player2 together with the current STRPM/ProxyResolver build.

1. Keep IED NPC Displays disabled on both clients.
2. Connect both players to the same STR server.
3. Leave Kahel's spear, bow and shield visible with the normal local IED presets active.
4. Verify Player2 sees the correct anchor **and** orientation.
5. Confirm the remote log shows `attachmentMode=parent` for the captured `MOV ...` anchors.
6. Pay attention to the spear/backpack combination: both orientation and offset relative to the backpack should now be much closer to the owner's evaluated display.
7. Verify Elir's visible favorite in the opposite direction.
8. Send both `IEDSyncTogether.log` files if any orientation or offset still differs.

Helmet Toggle Custom Items remain intentionally excluded from remote rendering in v0.7.4; they are still captured and transported for the next stage.

## Safety

The capture path uses the public IED slot getter and Skyrim's evaluated scene graph. Proxy identity comes exclusively from the public STRPM ProxyResolver. Remote objects are created, attached and transformed through IED's public Papyrus Custom Item API under an IEDSyncTogether-owned key. No private IED runtime detours are used.

## License

MIT
