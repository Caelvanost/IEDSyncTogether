# IEDSyncTogether

Compatibility and synchronization layer between Immersive Equipment Displays (IED) and Skyrim Together Reborn (STR).

## Target behavior

STR represents remote players as dynamically created actor proxies. IEDSyncTogether captures the 19 forms actually displayed by IED on the owning client, exchanges stable `plugin + local FormID` identities over the LAN, binds the remote state to the matching STR proxy, and reproduces that display state locally without changing STR inventory or equipment.

## v0.2.1: official IED API renderer

v0.2.1 keeps the official-I​ED renderer introduced in v0.2.0 and fixes the CommonLibSSE-NG Papyrus dispatch argument packing used by the Custom Item API. Arguments are now materialized as decayed values before `RE::MakeFunctionArguments`, avoiding unsupported reference types such as `const std::string&` and `RE::TESForm*&` during compilation.

The official `ImmersiveEquipmentDisplays.dll` 1.7.4 is never patched, rebuilt, replaced or redistributed.

The renderer uses native Papyrus functions already shipped by IED 1.7.4:

- `IED.GetSlottedForm` captures the 19 authoritative display slots on the owning player.
- `IED.CreateItemActor` creates proxy-only Custom Items.
- `IED.SetItemFormActor` mirrors the form to both sex variants.
- `IED.SetItemNodeActor` mirrors the managed equipment node to both sex variants.
- `IED.SetItemLeftWeaponActor` preserves left-hand weapon semantics where required.
- `IED.SetItemEnabledActor` enables the generated entry.
- `IED.DeleteAllActor` removes stale IEDSyncTogether entries before a changed snapshot is rebuilt.
- `IED.Evaluate` asks IED to refresh the proxy.
- `IED.RemoveActorBlock` clears any historical v0.1.x block owned by IEDSyncTogether.

The critical argument to `CreateItemActor` is `abIsInventoryForm=false`. IED therefore loads the synchronized form directly instead of requiring the STR proxy to contain the owner's real inventory entry. This addresses the failure mode seen in v0.1.x where networking and proxy binding were correct but the desired form was absent from IED's proxy inventory candidates.

Custom entries are attached to IED's managed equipment nodes for the corresponding slot (`WeaponSword`, `WeaponAxe`, `WeaponBack`, `WeaponBow`, `WeaponCrossBow`, `Shield`, `Quiver`, and their left-hand variants). The remote snapshot remains authoritative: when the owner stops displaying a slot, the proxy entry disappears on the next changed snapshot.

## Why an ESP is included

IED's Custom Item Papyrus API requires the `asPlugin` key to name a loaded Skyrim data plugin. `build-vortex.ps1` therefore generates a minimal ESL-flagged `IEDSyncTogether.esp`. It contains no gameplay records and exists only as IEDSyncTogether's ownership namespace for persistent Custom Item configuration.

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
    -> remove any historical IEDSyncTogether actor block

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

The legacy INI key `SuppressRemoteNpcDisplays` remains for compatibility. With the default value `1`, it now enables proxy ownership by the Custom Item renderer; v0.2.1 does not call `IED.AddActorBlock`.

## Building

Set `VCPKG_ROOT` if necessary and run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1
```

The archive name follows the version in `CMakeLists.txt`:

```text
dist/IEDSyncTogether-v0.2.1.zip
```

Expected relevant contents:

```text
Data/IEDSyncTogether.esp
Data/SKSE/Plugins/IEDSyncTogether.dll
Data/SKSE/Plugins/IEDSyncTogether.ini
```

The archive must **not** contain `ImmersiveEquipmentDisplays.dll`. Keep the official IED 1.7.4 installation enabled separately in Vortex.

## First v0.2.1 test

Install the same v0.2.1 archive on both STR clients. On Player1, verify `Documents/My Games/Skyrim Special Edition/SKSE/IEDSyncTogether.log` contains:

```text
IEDSyncTogether v0.2.1 loading
IED integration mode: official Papyrus API; no IED runtime patch installed
```

After Player2 is bound and a remote snapshot has been received, look for:

```text
IED Custom Item render queued: proxy=XXXXXXXX slots=N dispatchAccepted=1
```

For the current diagnostic case, Elir has one IED-displayed slot because she has no equipped gear and only one favorited weapon. Receiving `slots=1` is therefore expected. If exactly one object appears on Elir's proxy from Player1's point of view, the proxy renderer is validated.

### Useful failure signals

- `dispatchAccepted=0`: at least one Papyrus call could not be dispatched; verify IED 1.7.4 and `IEDSyncTogether.esp` are loaded.
- `IED custom render skipped unresolved form`: the stable remote form identity could not be resolved on this client, normally indicating a mod/load-order mismatch.
- No `IED Custom Item render queued` line after `Remote IED state stored`: verify the peer is still bound to the same dynamic STR proxy.

## Historical code

`src/IEDRuntimeHook.*`, `integration/ied-dev/`, and `build-ied-patched.ps1` are retained only as development history/reference. They are not compiled or used by v0.2.1. The IED source tree remains useful for documenting the official 1.7.4 API, but its private/non-public build assets make rebuilding IED an unsupported dependency for this project.

## License

IEDSyncTogether is MIT licensed. Immersive Equipment Displays is a separate dependency and is not redistributed or modified by IEDSyncTogether.
