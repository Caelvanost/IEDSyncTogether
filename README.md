# IEDSyncTogether

Compatibility and synchronization layer between Immersive Equipment Displays (IED) and Skyrim Together Reborn (STR).

## Target behavior

STR represents remote players as dynamically created actor proxies. IEDSyncTogether captures the 19 forms actually displayed by IED on the owning client, exchanges stable `plugin + local FormID` identities over the LAN, binds the remote state to the matching STR proxy, and reproduces that state locally without changing STR inventory or equipment.

## v0.2.3: safe rollback to the official IED API

v0.2.3 removes the experimental v0.2.2 stock-slot runtime shim. Crash logs from both STR clients proved that IED immediately dereferences the value returned by its internal `SelectSlotItem` call. The v0.2.2 suppression path returned `nullptr`, causing a deterministic access violation at `ImmersiveEquipmentDisplays.dll+00E380B` as soon as the remote proxy was evaluated.

v0.2.3 therefore restores the known-safe v0.2.1 renderer and installs **no runtime patch in IED**. The official `ImmersiveEquipmentDisplays.dll` 1.7.4 is never rebuilt, replaced, modified on disk or redistributed.

The renderer uses native Papyrus functions already shipped by IED 1.7.4:

- `IED.GetSlottedForm` captures the 19 authoritative display slots on the owning player.
- `IED.CreateItemActor` creates proxy-only Custom Items with `abIsInventoryForm=false`.
- `IED.SetItemFormActor` and `IED.SetItemNodeActor` mirror the form/node to both sex variants.
- `IED.SetItemLeftWeaponActor` preserves left-hand weapon semantics where required.
- `IED.SetItemEnabledActor` enables the generated entry.
- `IED.DeleteAllActor` removes stale IEDSyncTogether entries before rebuilding a changed snapshot.
- `IED.Evaluate` refreshes the proxy.
- `IED.RemoveActorBlock` clears any historical IEDSyncTogether block state.

`abIsInventoryForm=false` is essential: the synchronized model can be displayed even when the STR proxy does not contain the owner's real inventory entry.

## Known limitation

IED's normal NPC slot renderer is still active on STR proxies. As a result, a remote player may temporarily appear with ordinary IED inventory displays **in addition to** the authoritative Custom Items created by IEDSyncTogether. This is the behavior seen before v0.2.2.

That visual duplication is intentionally accepted in v0.2.3 because the attempted binary suppression was unsafe. Future suppression work must preserve IED's `ProcessCustom` path and must not return an invalid slot-selection object to IED.

## Why an ESP is included

IED's Custom Item Papyrus API requires `asPlugin` to name a loaded Skyrim data plugin. `build-vortex.ps1` therefore generates a minimal ESL-flagged `IEDSyncTogether.esp`. It contains no gameplay records and exists only as IEDSyncTogether's ownership namespace for persistent Custom Item configuration.

Do not rename or disable this ESP.

## Required versions

- Skyrim Special Edition 1.6.1170
- SKSE 2.2.6
- Immersive Equipment Displays 1.7.4 official release
- Skyrim Together Reborn 1.8.0

## Runtime flow

```text
DataLoaded
    -> start IEDSyncTogether
    -> no IED binary patch is installed

PostLoadGame / NewGame
    -> wait for LAN peer + matching STR proxy

remote proxy resolved
    -> register proxy for authoritative Custom Item rendering

remote STATE received
    -> store 19-slot snapshot using stable form identities

next synchronization tick
    -> resolve remote forms locally
    -> if snapshot changed: delete old IEDSyncTogether Custom Items
    -> create non-inventory Custom Items on the correct IED nodes
    -> evaluate proxy

proxy disappears / STR disconnect
    -> unregister proxy
    -> delete IEDSyncTogether Custom Items
```

The legacy INI key `SuppressRemoteNpcDisplays` remains for compatibility. With the default value `1`, it enables proxy ownership by the Custom Item renderer. Despite the historical name, v0.2.3 does not suppress IED's ordinary NPC slots at binary level and does not call `IED.AddActorBlock`.

## Building

Set `VCPKG_ROOT` if necessary and run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1
```

The archive name follows `CMakeLists.txt`:

```text
dist/IEDSyncTogether-v0.2.3.zip
```

Expected relevant contents:

```text
Data/IEDSyncTogether.esp
Data/SKSE/Plugins/IEDSyncTogether.dll
Data/SKSE/Plugins/IEDSyncTogether.ini
```

The archive must **not** contain `ImmersiveEquipmentDisplays.dll`. Keep the official IED 1.7.4 installation enabled separately in Vortex.

## First v0.2.3 test

Install the same v0.2.3 archive on both STR clients. The log must contain:

```text
IEDSyncTogether v0.2.3 loading
IED integration mode: official Papyrus API; no IED runtime patch installed
```

After the remote player is bound and a state is received, look for:

```text
IED Custom Item render queued: proxy=XXXXXXXX slots=N dispatchAccepted=1
```

There must be no `IED 1.7.4 proxy stock-slot shim installed` or `IED stock-slot suppression armed` line in a v0.2.3 log.

## Historical code

`src/IEDRuntimeHook.*`, `integration/ied-dev/`, and `build-ied-patched.ps1` are retained only as development history/reference. They are not compiled or used by v0.2.3. The v0.2.2 `SelectSlotItem` suppression experiment is explicitly retired after the dual-client deterministic crash described above.

## License

IEDSyncTogether is MIT licensed. Immersive Equipment Displays is a separate dependency and is not redistributed or modified by IEDSyncTogether.
