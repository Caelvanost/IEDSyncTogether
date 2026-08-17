# IEDSyncTogether

Compatibility and synchronization layer between Immersive Equipment Displays (IED) and Skyrim Together Reborn (STR).

## Target behavior

STR represents remote players as dynamically created actor proxies. IEDSyncTogether captures the 19 forms actually displayed by IED on the owning client, exchanges stable `plugin + local FormID` identities over the LAN, binds the remote state to the matching STR proxy, and reproduces that state locally without changing STR inventory or equipment.

## v0.2.6: safe rollback after runtime-hook crashes

v0.2.6 removes the experimental IED runtime interception used by v0.2.4 and v0.2.5.

The v0.2.4 handle-based filter incorrectly affected the local player's normal IED slot processing. The v0.2.5 runtime-calibrated wrapper then crashed while loading a save: the crash stack showed `ImmersiveEquipmentDisplays.dll+00E5855 -> IEDSyncTogether.dll -> ImmersiveEquipmentDisplays.dll+00DF591`, proving that the inferred `ProcessSlots` call boundary/calling context was not safe to invoke through our wrapper.

v0.2.6 therefore restores the known-safe v0.2.3 integration path:

- no in-memory patch is installed in `ImmersiveEquipmentDisplays.dll`;
- `src/IEDRuntimeHook.*` is not compiled or called;
- local IED behavior, favorites, weapon placement and user presets are left entirely to official IED;
- remote synchronized display still uses IED's public Papyrus Custom Item API;
- the official IED 1.7.4 DLL is never rebuilt, replaced, modified on disk or redistributed.

This intentionally re-accepts the known limitation that IED's ordinary NPC renderer may display the STR proxy's local inventory in addition to IEDSyncTogether's authoritative Custom Items. That duplication is preferable to risking local-player regressions or save-load crashes.

## Official IED API renderer

The synchronized objects use native Papyrus functions already shipped by IED 1.7.4:

- `IED.GetSlottedForm` captures the 19 authoritative display slots on the owning player.
- `IED.CreateItemActor` creates proxy-only Custom Items with `abIsInventoryForm=false`.
- `IED.SetItemFormActor` and `IED.SetItemNodeActor` mirror the form/node to both sex variants.
- `IED.SetItemLeftWeaponActor` preserves left-hand weapon semantics where required.
- `IED.SetItemEnabledActor` enables the generated entry.
- `IED.DeleteAllActor` removes stale IEDSyncTogether entries before rebuilding a changed snapshot.
- `IED.Evaluate` refreshes the proxy.
- `IED.RemoveActorBlock` clears any historical IEDSyncTogether block state.

`abIsInventoryForm=false` is essential: the synchronized model can be displayed even when the STR proxy does not contain the owner's real inventory entry.

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
    -> create non-inventory Custom Items on the corresponding IED nodes
    -> evaluate proxy

proxy disappears / STR disconnect
    -> unregister proxy
    -> delete IEDSyncTogether Custom Items
```

The legacy INI key `SuppressRemoteNpcDisplays` remains for compatibility. With the default value `1`, it registers resolved STR proxies for authoritative Custom Item rendering only. v0.2.6 does not suppress IED's ordinary NPC slots at binary level and does not call `IED.AddActorBlock`.

## Building

Set `VCPKG_ROOT` if necessary and run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1
```

The archive name follows `CMakeLists.txt`:

```text
dist/IEDSyncTogether-v0.2.6.zip
```

Expected relevant contents:

```text
Data/IEDSyncTogether.esp
Data/SKSE/Plugins/IEDSyncTogether.dll
Data/SKSE/Plugins/IEDSyncTogether.ini
```

The archive must **not** contain `ImmersiveEquipmentDisplays.dll`. Keep the official IED 1.7.4 installation enabled separately in Vortex.

## First v0.2.6 test

Install the same v0.2.6 archive on both STR clients. The log must contain:

```text
IEDSyncTogether v0.2.6 loading
IED integration mode: official Papyrus API; no IED runtime patch installed
```

There must be no `IED 1.7.4 proxy ProcessSlots suppression installed`, `runtime-calibrated`, `ProcessSlots suppression armed`, or `SelectSlotItem` hook line.

Before connecting the second client, verify that the local player's IED favorites and weapon-placement presets behave normally. Then connect the second client and confirm the local display stays unchanged.

## Historical note

- v0.2.2 attempted to suppress `SelectSlotItem` by returning `nullptr`; IED immediately dereferenced the result and both clients crashed.
- v0.2.4 attempted a higher-level `ProcessSlots` filter using a misidentified handle and affected local IED behavior.
- v0.2.5 attempted runtime calibration and still crashed on save load because the inferred internal call boundary was not safe to wrap.

These runtime-hook approaches are retired. Future duplicate-suppression work must use a public/stable IED or STR-facing integration path rather than another private IED ABI patch.

## License

IEDSyncTogether is MIT licensed. Immersive Equipment Displays is a separate dependency and is not redistributed or modified by IEDSyncTogether.
