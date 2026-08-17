# IEDSyncTogether

Compatibility and synchronization layer between Immersive Equipment Displays (IED) and Skyrim Together Reborn (STR).

## Target behavior

STR represents remote players as dynamically created actor proxies. IEDSyncTogether captures the 19 forms actually displayed by IED on the owning client, exchanges stable `plugin + local FormID` identities over the LAN, binds the remote state to the matching STR proxy, and reproduces that display state locally without changing STR inventory or equipment.

## v0.2.2: authoritative remote display

v0.2.2 fixes the case where IED's ordinary NPC renderer displayed the STR proxy's whole local inventory in addition to the synchronized Custom Items.

IED internally evaluates ordinary slot items (`ProcessSlots`) and Custom Items (`ProcessCustom`) separately. IEDSyncTogether now filters the ordinary slot-selection result only while IED is evaluating a tracked STR proxy. The subsequent Custom Item pass remains active, so the remote snapshot is the only IEDSyncTogether-managed equipment display on that proxy.

The filter is deliberately narrow:

- only dynamically tracked STR remote proxies are affected;
- normal NPCs and the local player keep IED's normal behavior;
- no inventory, favorites or equipped state is changed;
- no private IED C++ object is constructed, called or dereferenced by IEDSyncTogether;
- the shim compares raw words in the already-valid IED evaluation context only against exact `Actor*` addresses registered by the STR proxy resolver;
- unmatched evaluations tail-jump to the exact official IED 1.7.4 `SelectSlotItem` implementation;
- matched proxy evaluations return `nullptr` only for the ordinary slot selector, allowing IED itself to remove those stock slot objects before running its Custom Item pass.

The official `ImmersiveEquipmentDisplays.dll` 1.7.4 file is never rebuilt, replaced, redistributed or modified on disk. v0.2.2 installs a small version-checked in-memory call-site relay at runtime. If the expected IED 1.7.4 machine-code signatures do not match, the relay refuses to install instead of patching an unknown build.

## Official IED API renderer

The synchronized objects themselves still use native Papyrus functions shipped by official IED 1.7.4:

- `IED.GetSlottedForm` captures the 19 authoritative display slots on the owning player.
- `IED.CreateItemActor` creates proxy-only Custom Items.
- `IED.SetItemFormActor` mirrors the form to both sex variants.
- `IED.SetItemNodeActor` mirrors the managed equipment node to both sex variants.
- `IED.SetItemLeftWeaponActor` preserves left-hand weapon semantics where required.
- `IED.SetItemEnabledActor` enables the generated entry.
- `IED.DeleteAllActor` removes stale IEDSyncTogether entries before a changed snapshot is rebuilt.
- `IED.Evaluate` asks IED to refresh the proxy.
- `IED.RemoveActorBlock` clears any historical v0.1.x block owned by IEDSyncTogether.

The critical argument to `CreateItemActor` is `abIsInventoryForm=false`. IED therefore loads the synchronized form directly instead of requiring the STR proxy to contain the owner's real inventory entry.

Custom entries are attached to IED's managed equipment nodes for the corresponding slot (`WeaponSword`, `WeaponAxe`, `WeaponBack`, `WeaponBow`, `WeaponCrossBow`, `Shield`, `Quiver`, and their left-hand variants).

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
    -> verify official IED 1.7.4 call-site/function signatures
    -> install narrow in-memory stock-slot relay
    -> start IEDSyncTogether

remote proxy resolved
    -> register exact proxy Actor* in the stock-slot filter
    -> register proxy for authoritative Custom Item rendering

remote STATE received
    -> store 19-slot snapshot using stable form identities

IED evaluates the tracked proxy
    -> ordinary ProcessSlots selection is filtered for that proxy only
    -> normal stock slot objects are removed by IED
    -> ProcessCustom remains active

next synchronization tick
    -> resolve remote forms locally
    -> rebuild changed IEDSyncTogether Custom Items
    -> evaluate proxy

proxy disappears / STR disconnect
    -> unregister its exact Actor* from the filter
    -> delete IEDSyncTogether Custom Items
```

The legacy INI key `SuppressRemoteNpcDisplays` remains for compatibility. Keep it at the default value `1`: it activates proxy ownership by the authoritative renderer. v0.2.2 still does **not** use `IED.AddActorBlock`, because an actor block would suppress both ordinary slots and Custom Items.

## Building

Set `VCPKG_ROOT` if necessary and run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1
```

The archive name follows the version in `CMakeLists.txt`:

```text
dist/IEDSyncTogether-v0.2.2.zip
```

Expected relevant contents:

```text
Data/IEDSyncTogether.esp
Data/SKSE/Plugins/IEDSyncTogether.dll
Data/SKSE/Plugins/IEDSyncTogether.ini
```

The archive must **not** contain `ImmersiveEquipmentDisplays.dll`. Keep the official IED 1.7.4 installation enabled separately in Vortex.

## First v0.2.2 test

Install the same v0.2.2 archive on both STR clients. At startup, the log should contain:

```text
IEDSyncTogether v0.2.2 loading
IED 1.7.4 proxy stock-slot shim installed: ...
IED integration mode: official Papyrus Custom Item API + targeted in-memory stock-slot suppression for tracked STR proxies
```

When Elir is bound, Player1 should still receive the expected authoritative state:

```text
Remote IED state stored: ... slots=1
IED stock-slot suppression armed: proxy=FF...... actor=0x...
IED Custom Item render queued: proxy=FF...... slots=1 dispatchAccepted=1
```

In the current diagnostic case Elir has exactly one IED-displayed slot because she has no equipped gear and only one favorited weapon. Player1 should therefore see that one synchronized IED object, not every item present in the STR proxy inventory.

### Useful failure signals

- `IED stock-slot shim: unsupported IED build`: the installed IED DLL does not match the validated official 1.7.4 code signatures; no unknown build is patched.
- `dispatchAccepted=0`: at least one Papyrus call could not be dispatched; verify IED 1.7.4 and `IEDSyncTogether.esp` are loaded.
- `IED custom render skipped unresolved form`: the stable remote form identity could not be resolved on this client, normally indicating a mod/load-order mismatch.
- no `IED Custom Item render queued` after `Remote IED state stored`: verify the peer is still bound to the same dynamic STR proxy.

## Historical code

`integration/ied-dev/` and `build-ied-patched.ps1` are retained only as development history/reference. Rebuilding IED remains unsupported and unnecessary. `src/IEDRuntimeHook.*` is active again in v0.2.2, but only as the narrow version-checked in-memory relay described above; it does not replace or redistribute IED.

## License

IEDSyncTogether is MIT licensed. Immersive Equipment Displays is a separate dependency and is not redistributed by IEDSyncTogether.
