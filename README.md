# IEDSyncTogether

Compatibility and synchronization layer between Immersive Equipment Displays (IED) and Skyrim Together Reborn (STR).

## Target behavior

STR represents remote players as dynamically created actor proxies. IEDSyncTogether captures the 19 forms actually displayed by IED on the owning client, exchanges stable `plugin + local FormID` identities over the LAN, binds the remote state to the matching STR proxy, and reproduces that state locally without changing STR inventory or equipment.

## v0.3.0: native IED NPC-slot baseline

v0.3.0 establishes a safe development baseline for remote equipment restitution.

The previous private-runtime experiments are retired. IEDSyncTogether does not patch `ImmersiveEquipmentDisplays.dll`, does not wrap IED's internal `ProcessSlots`, and does not intercept `SelectSlotItem`.

Instead, while remote restitution is being developed, enable IED's own global setting:

```text
Disable NPC equipment displays
```

on **every STR client**.

IED 1.7.4 already handles this distinction internally: its ordinary equipment-slot pass is skipped for NPCs when that setting is enabled, while the PlayerCharacter continues through normal slot processing and IED Custom Items continue to be processed. This gives IEDSyncTogether the clean test environment it needs:

- the local player keeps normal IED favorites, placement rules and presets;
- ordinary NPC/STR-proxy slot displays are disabled by IED itself;
- IEDSyncTogether can render the authoritative remote state using non-inventory Custom Items;
- no private IED ABI interception is required.

This setting is intentionally **not forced or written by IEDSyncTogether**. It belongs to IED and may affect every NPC, so it must be enabled explicitly by the user during this development phase.

## Node Override presets and spear placement

The supplied `Backpack And Spears Repositionner` archive was inspected as part of the v0.3.0 work. Its current archive contains an IED **Node Overrides** profile, including `CME ...` / `MOV ...` skeleton-node transforms. It is not, by itself, a list of spear keyword-routing rules.

For the current development baseline:

- install/import the same Node Overrides profile on both clients;
- keep that IED profile as the source of truth for local node transforms;
- IEDSyncTogether continues to create remote Custom Items on IED managed equipment nodes;
- v0.3.0 does **not** hard-code guessed spear keywords, offsets or rotations from that profile.

Exact remote transform replication is the next layer to build. IED 1.7.4's public Papyrus API exposes `SetItemNodeActor`, `SetItemPositionActor`, `SetItemRotationActor` and `SetItemScaleActor`, so once the authoritative local node/transform data is identified we can reproduce it without returning to runtime hooks.

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

IED also exposes public Custom Item setters for position, rotation and scale. These are deliberately not populated with guessed values in v0.3.0.

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
    -> require native IED "Disable NPC equipment displays" development baseline

PostLoadGame / NewGame
    -> wait for LAN peer + matching STR proxy

remote proxy resolved
    -> register proxy for authoritative Custom Item rendering

remote STATE received
    -> store 19-slot snapshot using stable form identities

next synchronization tick
    -> resolve remote forms locally
    -> if snapshot changed: delete old IEDSyncTogether Custom Items
    -> create non-inventory Custom Items on the corresponding IED managed nodes
    -> evaluate proxy

proxy disappears / STR disconnect
    -> unregister proxy
    -> delete IEDSyncTogether Custom Items
```

The legacy INI key `SuppressRemoteNpcDisplays` remains for compatibility. With the default value `1`, it registers resolved STR proxies for authoritative Custom Item rendering. It does **not** alter IED's global NPC setting itself.

## Building

Set `VCPKG_ROOT` if necessary and run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1
```

The archive name follows `CMakeLists.txt`:

```text
dist/IEDSyncTogether-v0.3.0.zip
```

Expected relevant contents:

```text
Data/IEDSyncTogether.esp
Data/SKSE/Plugins/IEDSyncTogether.dll
Data/SKSE/Plugins/IEDSyncTogether.ini
```

The archive must **not** contain `ImmersiveEquipmentDisplays.dll`. Keep the official IED 1.7.4 installation enabled separately in Vortex.

## v0.3.0 test protocol

Install the same v0.3.0 archive on both STR clients.

On **both PCs**, open IED and enable:

```text
Disable NPC equipment displays
```

Also import/activate the same Node Overrides profile used for weapon placement on both PCs when testing placement-sensitive equipment such as spears.

Before connecting the second client:

1. load the same save that previously crashed with v0.2.5;
2. verify the local player's IED favorites are visible;
3. equip/stow the spear and verify the local placement preset still behaves normally.

Then connect the second STR client and verify:

1. the local player's IED display remains unchanged;
2. the remote proxy no longer shows stock IED equipment from its STR proxy inventory;
3. only IEDSyncTogether's remote Custom Items remain as the equipment-restoration layer;
4. note any remote item using the wrong node or transform for the next restitution pass.

The log must contain:

```text
IEDSyncTogether v0.3.0 loading
IED integration mode: official Papyrus Custom Item API; no IED runtime patch installed
IED v0.3.0 baseline: enable "Disable NPC equipment displays" ...
```

There must be no runtime-hook installation line.

## Historical safety note

- v0.2.2 attempted to suppress `SelectSlotItem` by returning `nullptr`; IED immediately dereferenced the result and both clients crashed.
- v0.2.4 attempted a higher-level `ProcessSlots` filter using a misidentified handle and affected local IED behavior.
- v0.2.5 attempted runtime calibration and still crashed on save load because the inferred internal call boundary was not safe to wrap.
- v0.2.6 removed those hooks and restored the official Papyrus-only renderer.
- v0.3.0 keeps that safe renderer and adopts IED's own NPC-display switch as the temporary duplicate-suppression baseline.

Future synchronization work must stay on public/stable IED interfaces or synchronize explicit data rather than patching private IED runtime internals.

## License

IEDSyncTogether is MIT licensed. Immersive Equipment Displays is a separate dependency and is not redistributed or modified by IEDSyncTogether.
