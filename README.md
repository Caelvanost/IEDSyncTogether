# IEDSyncTogether

Compatibility and synchronization layer between Immersive Equipment Displays (IED) and Skyrim Together Reborn (STR).

## Target behavior

STR represents remote players as dynamically created actor proxies. IEDSyncTogether captures the 19 forms actually displayed by IED on the owning client, exchanges stable `plugin + local FormID` identities over the LAN, binds the remote state to the matching STR proxy, and reproduces that state locally without changing STR inventory or equipment.

## v0.2.5: protect the local player and runtime-calibrate proxy identity

v0.2.5 fixes the v0.2.4 regression where connecting a remote STR proxy could make the local player's normal IED slots disappear. That regression also caused placement presets to stop applying correctly to locally equipped/favorited weapons because IED's normal `ProcessSlots` path was being skipped for the wrong actor.

The v0.2.4 assumption was incorrect: IED's private `Game::ObjectRefHandle` at the start of `ProcessParamsData` must not be treated as if it were CommonLibSSE-NG's `RE::ObjectRefHandle` native 32-bit representation. In the failing runtime log, the proxy was armed and the next local 19-slot capture immediately returned zero forms for every slot.

v0.2.5 therefore removes all handle-value matching from the suppression selector.

### Runtime-calibrated Actor* selector

The `DoObjectEvaluation -> ProcessSlots` call-site remains the interception level because this preserves IED's own control flow: skipping `ProcessSlots` still allows the subsequent `ProcessCustom` pass to execute.

Actor identification is now fail-open and calibrated at runtime:

1. the wrapper observes normal IED `ProcessSlots` evaluations;
2. it searches the readable `ProcessParams` stack region for the exact `RE::PlayerCharacter*` address;
3. the same unique offset must be observed repeatedly before it is accepted as the actor-field offset;
4. until calibration succeeds, **no `ProcessSlots` call is suppressed**;
5. after calibration, the wrapper reads only that calibrated pointer field;
6. the local `PlayerCharacter*` is explicitly excluded from suppression;
7. only an exact `Actor*` registered by the STR proxy resolver is allowed to skip `ProcessSlots`.

This avoids assuming IED's private handle ABI and prevents Kahel/the local player from being mistaken for a remote proxy.

The official `ImmersiveEquipmentDisplays.dll` 1.7.4 file is never rebuilt, replaced, modified on disk or redistributed. The call-site is still validated against the known official 1.7.4 machine-code fingerprints and x64 unwind metadata before any in-memory patch is installed.

The old v0.2.2 `SelectSlotItem` experiment remains retired. v0.2.5 never changes `SelectSlotItem` and never returns a fabricated/null slot-selection object to IED.

## Official IED API renderer

Remote synchronized objects use native Papyrus functions already shipped by official IED 1.7.4:

- `IED.GetSlottedForm` captures the 19 authoritative display slots on the owning player.
- `IED.CreateItemActor` creates proxy-only Custom Items with `abIsInventoryForm=false`.
- `IED.SetItemFormActor` and `IED.SetItemNodeActor` mirror the form/node to both sex variants.
- `IED.SetItemLeftWeaponActor` preserves left-hand weapon semantics where required.
- `IED.SetItemEnabledActor` enables the generated entry.
- `IED.DeleteAllActor` removes stale IEDSyncTogether entries before rebuilding a changed snapshot.
- `IED.Evaluate` refreshes the proxy.
- `IED.RemoveActorBlock` clears any historical IEDSyncTogether block state.

`abIsInventoryForm=false` lets IED display the synchronized form even when the STR proxy does not contain the owner's real inventory entry.

## Why an ESP is included

IED's Custom Item Papyrus API requires `asPlugin` to name a loaded Skyrim data plugin. `build-vortex.ps1` generates a minimal ESL-flagged `IEDSyncTogether.esp`. It contains no gameplay records and exists only as IEDSyncTogether's ownership namespace for Custom Item configuration.

Do not rename or disable this ESP.

## Required versions

- Skyrim Special Edition 1.6.1170
- SKSE 2.2.6
- Immersive Equipment Displays 1.7.4 official release
- Skyrim Together Reborn 1.8.0

## Runtime flow

```text
DataLoaded
    -> verify official IED 1.7.4 fingerprint and ProcessSlots call site
    -> install the narrow ProcessSlots wrapper if validation succeeds
    -> cache PlayerCharacter address
    -> start IEDSyncTogether

normal IED player evaluations
    -> locate PlayerCharacter* uniquely inside ProcessParams
    -> require repeated matching offset
    -> calibrate actor field

remote proxy resolved
    -> register exact proxy Actor*
    -> register proxy for authoritative Custom Item rendering

IED evaluates an actor
    -> calibration unavailable? run stock ProcessSlots
    -> actor == local PlayerCharacter? run stock ProcessSlots
    -> actor == exact tracked STR proxy? skip ProcessSlots
    -> otherwise run stock ProcessSlots
    -> IED continues normally into ProcessCustom

remote STATE received
    -> store 19-slot snapshot using stable form identities
    -> create/update non-inventory Custom Items

proxy disappears / disconnect
    -> unregister exact proxy Actor*
    -> delete IEDSyncTogether Custom Items
```

## Building

Set `VCPKG_ROOT` if necessary and run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1
```

The archive name follows `CMakeLists.txt`:

```text
dist/IEDSyncTogether-v0.2.5.zip
```

Expected contents include:

```text
Data/IEDSyncTogether.esp
Data/SKSE/Plugins/IEDSyncTogether.dll
Data/SKSE/Plugins/IEDSyncTogether.ini
```

The archive must **not** contain `ImmersiveEquipmentDisplays.dll`. Keep official IED 1.7.4 installed separately in Vortex.

## First v0.2.5 test

Install the same archive on both STR clients. Startup should include:

```text
IEDSyncTogether v0.2.5 loading
IED 1.7.4 proxy ProcessSlots suppression installed: ... actor identity will be runtime-calibrated from PlayerCharacter ...
```

When a remote proxy is resolved, one of these is expected:

```text
IED ProcessSlots suppression armed: proxy=FF...... actor=0x... actorOffset=0x...
```

or, if the local-player calibration has not happened yet:

```text
IED ProcessSlots suppression armed: proxy=FF...... actor=0x...; actor-field calibration pending, suppression remains fail-open until calibrated
```

The critical regression check is the local capture. It must no longer report nineteen zero forms if the local player has IED-displayed favorites/equipment. The local player's favorited equipment and placement presets must remain exactly as they behave with stock IED.

For the current Elir test, `slots=1` remains expected when Elir has no equipped gear and one favorited weapon.

### Emergency fallback

If a test build ever affects the local player's IED display, set:

```ini
[Compatibility]
SuppressRemoteNpcDisplays=0
```

and restart Skyrim. No STR proxy will then be registered in the suppression/custom-render path, restoring ordinary local IED behavior while debugging continues.

## Historical note

- v0.2.2: retired `SelectSlotItem` null-return experiment; caused deterministic IED crashes.
- v0.2.3: safe rollback to official Papyrus Custom Item rendering only.
- v0.2.4: moved interception to the complete `ProcessSlots` call, but incorrectly assumed private IED handle representation and could suppress the local player.
- v0.2.5: keeps the safer `ProcessSlots` interception level but replaces handle matching with runtime-calibrated exact `Actor*` matching and an explicit local-player exclusion.

`integration/ied-dev/` and `build-ied-patched.ps1` remain reference/development material only. Rebuilding IED is not a dependency of this project.

## License

IEDSyncTogether is MIT licensed. Immersive Equipment Displays is a separate dependency and is not redistributed or modified by IEDSyncTogether.
