# IEDSyncTogether

Compatibility layer between Immersive Equipment Displays (IED) and Skyrim Together Reborn.

## v0.10.0 — automatic IED isolation for STR proxies

v0.10.0 removes the need to disable **IED NPC Displays** globally during multiplayer.

IED can remain fully enabled for ordinary NPCs. Only actors identified by STRPluginMessagingAPI's ProxyResolver as remote STR player proxies are isolated from IED's normal NPC display pipeline. Those proxies then display only the equipment state reproduced by IEDSyncTogether.

```text
ordinary NPC
   ↓
normal IED NPC Displays + normal IED Custom Items

remote STR proxy
   ↓
patched IED bridge detects proxy FormID
   ↓
normal IED slot display = suppressed
non-IEDSync Custom Items = suppressed / purged
   ↓
IEDSyncTogether.esp-owned Custom Items only
   ↓
remote sender state + exact transform watchdog
```

This feature builds on v0.9.1, including the AnimSyncTogether ownership split for transient `AnimObjectR` / `AnimObjectL` Helmet Toggle animation objects.

### Requirements

- Skyrim Together Reborn 1.8.0
- Immersive Equipment Displays **1.7.4**
- STRPluginMessagingAPI v0.8.2 or newer compatible build with ProxyResolver available
- the relay/server files required by that STRPM build

The v0.10.0 test archive includes an IED 1.7.4 compatibility DLL built reproducibly from the official IED 1.7.4 source plus the small IEDSyncTogether bridge patch. It replaces `ImmersiveEquipmentDisplays.dll` while preserving normal IED behavior for non-proxy actors.

IED is MIT licensed; the upstream license is included in `Data/IEDSyncTogether/licenses/IED-LICENSE.txt`.

## Proxy isolation bridge

IEDSyncTogether keeps a small thread-safe set of active remote proxy FormIDs. STRPM ProxyResolver mapping callbacks update that set as proxies are added, replaced, removed or cleared.

The patched IED 1.7.4 DLL queries two public exports from `IEDSyncTogether.dll`:

```text
IEDST_QuerySlotOverride(actorFormID, slotIndex, outFormID)
IEDST_IsRemoteProxy(actorFormID)
```

For a normal NPC, the bridge does nothing.

For a remote STR proxy:

- all 19 normal IED equipment slots return an empty selection;
- IED's actor/NPC/race/global Custom Item maps are filtered so only the plugin key `IEDSyncTogether.esp` is processed;
- any non-IEDSync Custom Items that were already created before the STR mapping became known are removed through IED's normal object cleanup path.

This avoids `AddActorBlock`, which is too broad because it removes both standard gear and the IEDSyncTogether-owned Custom Items needed by the renderer. It also avoids the old private runtime detours used by early experimental versions.

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

Delivery behavior:

- changed state is retained as pending while transport is unavailable;
- pending state retries every 1 second;
- the current full state is retransmitted every 10 seconds as a heartbeat;
- late-joining peers receive the current state without forcing an equipment change.

IEDSyncTogether uses STRPM's public ProxyResolver API. It does not scan `ProcessLists`, compare actor names, or guess dynamic proxy FormIDs.

## Remote renderer

Standard slots are recreated as actor-specific IED Custom Items owned by `IEDSyncTogether.esp`:

1. resolve `plugin + local FormID` on the receiving client;
2. select the evaluated remote node/anchor;
3. create an IED Custom Item owned by `IEDSyncTogether.esp`;
4. preserve left-weapon semantics where required;
5. call `IED.Evaluate`;
6. find the generated `OBJECT ... [FormID]` scene object;
7. restore its exact captured local position, raw 3x3 matrix and scale directly.

Persistent captured Custom Items follow the same ownership model.

## Persistent transform watchdog

Every 100 ms, the local watchdog checks the exact scenegraph transform of IEDSyncTogether-owned remote displays. If IED later re-evaluates an item and changes its transform, IEDSyncTogether restores the captured matrix, position and scale on the game thread.

Expected logs:

```text
REMOTE IED WATCHDOG acquired: ...
REMOTE IED WATCHDOG corrected: ...
REMOTE IED WATCHDOG recorrected: ...
```

The watchdog creates no network traffic.

## Helmet Toggle / AnimSyncTogether ownership

When `AnimSyncTogether.dll` is loaded, transient Custom Items attached to:

```text
OBJECT P AnimObjectR
OBJECT R AnimObjectR
OBJECT P AnimObjectL
OBJECT R AnimObjectL
```

are delegated to AnimSync/OAR and are not recreated by IEDSyncTogether.

Persistent displays such as:

```text
OBJECT R ExtraPelvisArmorHelmet1
```

remain synchronized by IEDSyncTogether.

Expected delegation log:

```text
REMOTE IED CUSTOM delegated to AnimSync/OAR: ...
```

## Diagnostic logs

Startup should include:

```text
IEDSyncTogether v0.10.0 loading
REMOTE IED transform watchdog started: interval=100ms
STRPM branch mode: ... patched-IED proxy isolation ...
```

When STRPM resolves another player:

```text
IED bridge proxy registered: connection=... proxy=... tracked=...
```

Normal renderer markers remain:

```text
REMOTE IED SLOT queued: ...
REMOTE IED CUSTOM queued: ...
REMOTE IED RENDER queued: ...
```

## Build

The core plugin still builds with:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1
```

For v0.10.0, `build-vortex.ps1` also requires the patched IED 1.7.4 DLL at:

```text
third-party/IED-1.7.4/ImmersiveEquipmentDisplays.dll
```

The `Build patched IED 1.7.4` GitHub workflow builds that DLL from the pinned official IED 1.7.4 source and commits the reproducible test binary to `feature/proxy-ied-isolation`.

Expected archive:

```text
dist/IEDSyncTogether-v0.10.0.zip
```

The archive contains:

```text
Data/SKSE/Plugins/IEDSyncTogether.dll
Data/SKSE/Plugins/ImmersiveEquipmentDisplays.dll
Data/IEDSyncTogether.esp
Data/IEDSyncTogether/licenses/IED-LICENSE.txt
```

No legacy IEDSyncTogether INI or UDP transport files are included.

## v0.10.0 test protocol

Install the same v0.10.0 archive on both clients and **leave IED NPC Displays enabled on both clients**.

1. Before connecting to STR, verify ordinary NPCs still show their normal IED displays.
2. Connect Player1 and Player2 to the same STR server.
3. Verify the remote player proxy does not retain any automatic NPC Display copy of weapons, bows, shields or other IED slots.
4. Verify the proxy shows one copy of each display reproduced by IEDSyncTogether and that its placement/orientation matches the sender.
5. Verify ordinary NPCs nearby continue to use normal IED NPC Displays while the remote player is isolated.
6. Test a persistent IED Custom Item such as the Helmet Toggle waist display and verify only the IEDSyncTogether copy remains on the proxy.
7. With AnimSyncTogether installed, test the helmet hand animation and verify `AnimObjectR/L` remains delegated with no duplicate IEDSyncTogether copy.
8. Disconnect/reconnect STR and verify proxy isolation is removed/reapplied with the proxy mapping lifecycle.
9. Change cell or reload a save and verify no stale automatic NPC displays remain on a recreated proxy.
10. Send both `IEDSyncTogether.log` files if a proxy shows an extra automatic IED object or a normal NPC loses its displays.

## Safety

The proxy registry contains only FormIDs provided by STRPM ProxyResolver. Normal NPCs are never classified by name, base FormID or heuristic.

The IED compatibility change is a small compile-time bridge against the official IED 1.7.4 source. No private IED runtime detours are installed by IEDSyncTogether. IED remains responsible for object creation and cleanup; IEDSyncTogether owns only its `IEDSyncTogether.esp` Custom Items and directly corrects their final scenegraph transforms.

## License

IEDSyncTogether: MIT.

Immersive Equipment Displays compatibility DLL: upstream IED MIT license included in the package.
