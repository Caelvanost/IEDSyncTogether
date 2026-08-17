# IEDSyncTogether

Compatibility and synchronization layer between Immersive Equipment Displays (IED) and Skyrim Together Reborn (STR).

## Target behavior

STR represents remote players as dynamically created actor proxies. IEDSyncTogether captures the 19 forms actually displayed by IED on the owning client, exchanges stable `plugin + local FormID` identities over the LAN, binds the remote state to the matching STR proxy, and reproduces that state locally without changing STR inventory or equipment.

## v0.3.2: native IED diagnostics

v0.3.2 removes the v0.3.1 `AddActorBlock` experiment. The dual-client test proved that ActorBlock is too broad: it suppresses ordinary IED gear but also suppresses IED Custom Items, leaving only equipment genuinely equipped through Skyrim Together visible.

The validated LAN synchronization and public Papyrus Custom Item renderer remain unchanged.

No private IED runtime hook is installed. IEDSyncTogether does not patch `ImmersiveEquipmentDisplays.dll`, does not intercept `ProcessSlots`, and does not intercept `SelectSlotItem`.

### What v0.3.2 verifies

IED 1.7.4's own source implements the desired native separation:

```text
if disable_npc_slots is false OR actor holder is Player
    ProcessSlots

ProcessCustom
```

With `disable_npc_slots=true`, an NPC should therefore skip ordinary inventory-driven IED slots while still processing Custom Items.

IED also marks an `ActorObjectHolder` as the player only when the actor pointer is exactly equal to the local `PlayerCharacter*` pointer.

v0.3.2 adds read-only diagnostics for both assumptions:

1. it reads `Data\SKSE\Plugins\IED\Settings.json`, the settings file used by IED, and logs the stored `disable_npc_slots` value;
2. for every resolved STR proxy it logs whether the proxy's actual `Actor*` pointer equals the local `PlayerCharacter*` pointer.

IEDSyncTogether never writes the IED settings file.

Expected diagnostic lines include:

```text
IED settings diagnostic: path="...\Data\SKSE\Plugins\IED\Settings.json" disable_npc_slots=true source=IED Settings.json (read-only)
IED proxy classification diagnostic: proxy=FFxxxxxx player=00000014 proxyIsPlayerPointer=0 proxyIsPlayerFormID=0
```

`proxyIsPlayerPointer=0` is the key classification result because it mirrors IED's own player test.

## Official IED API renderer

The synchronized objects use native Papyrus functions already shipped by IED 1.7.4:

- `IED.GetSlottedForm` captures the 19 authoritative display slots on the owning player.
- `IED.CreateItemActor` creates proxy-only Custom Items with `abIsInventoryForm=false`.
- `IED.SetItemFormActor` and `IED.SetItemNodeActor` mirror the form/node to both sex variants.
- `IED.SetItemLeftWeaponActor` preserves left-hand weapon semantics where required.
- `IED.SetItemEnabledActor` enables the generated entry.
- `IED.DeleteAllActor` removes stale IEDSyncTogether entries before rebuilding a changed snapshot.
- `IED.Evaluate` refreshes the proxy.
- `IED.RemoveActorBlock` clears historical IEDSyncTogether/v0.3.1 block state before rebuilding.

`abIsInventoryForm=false` is essential: the synchronized model can be displayed even when the STR proxy does not contain the owner's real inventory entry.

IED also exposes public Custom Item setters for position, rotation and scale. Exact transform replication is a later restitution layer and is deliberately not populated with guessed values here.

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
    -> read IED Settings.json read-only and log disable_npc_slots
    -> start read-only proxy classification diagnostics

PostLoadGame / NewGame
    -> wait for LAN peer + matching STR proxy

remote proxy resolved
    -> register proxy for authoritative Custom Item rendering
    -> remove any stale v0.3.1 ActorBlock owned by IEDSyncTogether
    -> log proxy Actor* vs PlayerCharacter* classification

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

The legacy INI key `SuppressRemoteNpcDisplays` remains for compatibility. With the default value `1`, it registers resolved STR proxies for authoritative Custom Item rendering. It does not alter IED's global NPC setting itself.

Note: a legacy log line may still say `Suppressed IED NPC display` when this registration occurs. In the current Papyrus-only renderer this historical message means the proxy was registered for authoritative Custom Item rendering; v0.3.2 does not install an ActorBlock or private suppression hook.

## Building

Set `VCPKG_ROOT` if necessary and run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1
```

The archive name follows `CMakeLists.txt`:

```text
dist/IEDSyncTogether-v0.3.2.zip
```

Expected relevant contents:

```text
Data/IEDSyncTogether.esp
Data/SKSE/Plugins/IEDSyncTogether.dll
Data/SKSE/Plugins/IEDSyncTogether.ini
```

The archive must **not** contain `ImmersiveEquipmentDisplays.dll`. Keep official IED 1.7.4 installed separately in Vortex.

## v0.3.2 test protocol

Install the same v0.3.2 archive on both STR clients.

On both PCs, open IED and enable:

```text
Disable NPC equipment displays
```

Use the same clear equipment pattern as the v0.3.1 test:

1. keep one or more weapons/shields favorited but stowed;
2. keep at least one different item genuinely equipped;
3. verify the owner still sees the normal local IED favorites;
4. connect both STR clients;
5. record which favorited/stowed items are visible on the remote proxy;
6. record which genuinely equipped items are visible;
7. send both `IEDSyncTogether.log` files after the test.

The important startup lines are:

```text
IEDSyncTogether v0.3.2 loading
IED integration mode: official Papyrus Custom Item API + read-only diagnostics; no ActorBlock probe and no IED runtime patch installed
IED settings diagnostic: ... disable_npc_slots=true ...
```

For every resolved remote actor there should also be:

```text
IED proxy classification diagnostic: proxy=FFxxxxxx player=00000014 proxyIsPlayerPointer=0 proxyIsPlayerFormID=0
```

### Interpretation

- `disable_npc_slots=false`: the IED setting is not persisted in the file IED loads; fix that before investigating anything else.
- `disable_npc_slots=true` + `proxyIsPlayerPointer=1`: the proxy is being classified as the PlayerCharacter and IED's normal player slot path explains the extra display.
- `disable_npc_slots=true` + `proxyIsPlayerPointer=0`: the proxy has the expected NPC classification. If inventory-derived gear is still visible, that layer must be isolated separately rather than suppressing all of IED.
- only equipped STR gear + synchronized favorites visible: the native IED setting is sufficient and no suppression hook is required.

See `docs/v0.3.2-ied-diagnostics.md` for the focused test notes.

## Historical safety note

- v0.2.2 attempted to suppress `SelectSlotItem` by returning `nullptr`; IED immediately dereferenced the result and both clients crashed.
- v0.2.4 attempted a higher-level `ProcessSlots` filter using a misidentified handle and affected local IED behavior.
- v0.2.5 attempted runtime calibration and still crashed on save load because the inferred internal call boundary was not safe to wrap.
- v0.2.6 removed those hooks and restored the official Papyrus-only renderer.
- v0.3.0 adopted IED's native NPC-slot switch as the safe baseline.
- v0.3.1 tested public `AddActorBlock`; it correctly hid ordinary IED gear but also hid Custom Items, so the probe was rejected.
- v0.3.2 removes ActorBlock and diagnoses the native `disable_npc_slots` + proxy-classification path directly.

Future synchronization work must stay on public/stable IED interfaces or synchronize explicit data rather than patching private IED runtime internals.

## License

IEDSyncTogether is MIT licensed. Immersive Equipment Displays is a separate dependency and is not redistributed or modified by IEDSyncTogether.
