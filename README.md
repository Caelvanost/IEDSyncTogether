# IEDSyncTogether

Compatibility layer between Immersive Equipment Displays (IED) and Skyrim Together Reborn.

## v0.7.3 — correct IED extrinsic rotation reconstruction

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

The raw matrix remains the wire representation. IEDSyncTogether does not serialize Euler angles, avoiding an unnecessary convention change during transport.

Helmet Toggle 2 Custom Items are still captured and transported, but v0.7.3 continues to render only standard IED slot objects remotely.

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

For every visible standard IED slot object, v0.7.3:

1. resolves the transmitted `plugin + local FormID` on the receiving client;
2. creates an IED Custom Item on the correct STR proxy under `IEDSyncTogether.esp`;
3. prefers the evaluated `MOV ...` / `CME ...` anchor captured on the owner;
4. falls back to the captured attachment or generic slot node only if needed;
5. keeps the captured raw 3x3 rotation matrix as the authoritative transport representation;
6. decomposes that matrix into the **extrinsic XYZ convention used by IED** only at the Papyrus boundary;
7. sends the resulting `(Pitch, Roll, Yaw)` degrees to `SetItemRotationActor`;
8. applies `SetItemPositionActor` and `SetItemScaleActor` for both male and female configurations;
9. applies left-hand semantics where applicable;
10. enables the item and calls `IED.Evaluate`.

The renderer clears only Custom Items owned by `IEDSyncTogether.esp`. Heartbeats remain idempotent: unchanged remote state is not rebuilt every 10 seconds.

## Why v0.7.3 exists

v0.7.1 validated evaluated anchor selection. v0.7.2 then started applying the captured position, rotation matrix and scale, but weapon orientation remained wrong.

The reason was the Euler convention at the final IED API boundary. IED's Papyrus API documents `SetItemRotation*` as a three-element `(Pitch, Roll, Yaw)` vector in degrees. Internally IED converts those degrees to radians and its `configTransform_t` uses **extrinsic rotation by default**, rebuilding the matrix with `SetEulerAnglesExtrinsic(x, y, z)`.

v0.7.2 used `RE::NiMatrix3::ToEulerAnglesXYZ()`, which does not invert that same convention. v0.7.3 replaces it with an explicit decomposition for fixed-axis/extrinsic XYZ:

```text
R = Rz(yaw) * Ry(roll) * Rx(pitch)

roll  = asin(-R20)
pitch = atan2(R21, R22)
yaw   = atan2(R10, R00)
```

with a stable gimbal-lock branch. No capture or wire-format change is required.

For Kahel's spear, the previously logged matrix:

```text
rotM=[0.069,-0.211,0.975;-0.808,-0.586,-0.070;0.586,-0.783,-0.210]
```

should now produce approximately:

```text
rotExtrinsicDeg=(-105.0,-35.9,-85.1)
```

instead of the incorrect v0.7.2 result around:

```text
(-198.4,-77.2,-71.9)
```

## Expected logs

Startup should include:

```text
IEDSyncTogether v0.7.3 loading
STRPM ProxyResolver listener registered
STRPM adapter started: ... proxyResolverReady=1
```

For a rendered slot, the log includes the exact transform sent to IED:

```text
REMOTE IED SLOT queued: ...
node="MOV WeaponSwordOnBack"
nodeSource=captured-anchor
pos=(-11.891,1.917,6.666)
rotExtrinsicDeg=(...,...,...)
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
dist/IEDSyncTogether-v0.7.3.zip
```

## v0.7.3 test protocol

Install the same v0.7.3 archive on Player1 and Player2 together with the current STRPM/ProxyResolver build.

1. Keep IED NPC Displays disabled on both clients.
2. Connect both players to the same STR server.
3. Leave Kahel's spear, bow and shield visible with the normal local IED presets active.
4. Compare their remote position and orientation against Kahel's local display.
5. For the spear, verify the remote log uses `node="MOV WeaponSwordOnBack"` and `rotExtrinsicDeg` rather than the old `rotDeg` values.
6. Verify Elir's visible favorite in the opposite direction.
7. Send both `IEDSyncTogether.log` files and describe any remaining visual difference.

Helmet Toggle Custom Items remain intentionally excluded from remote rendering in v0.7.3; they are still captured and transported for the next stage.

## Safety

The capture path uses the public IED slot getter and Skyrim's evaluated scene graph. Proxy identity comes exclusively from the public STRPM ProxyResolver. Remote objects are created and transformed through IED's public Papyrus Custom Item API under an IEDSyncTogether-owned key. No private IED runtime detours are used.

## License

MIT
