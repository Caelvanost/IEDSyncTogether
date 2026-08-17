# IEDSyncTogether

Compatibility and synchronization layer between Immersive Equipment Displays (IED) and Skyrim Together Reborn (STR).

## Target behavior

STR represents remote players as dynamically created actor proxies. IEDSyncTogether captures the 19 forms actually displayed by IED on the owning client, exchanges stable `plugin + local FormID` identities over the LAN, binds the remote state to the matching STR proxy, and reproduces that state locally without changing STR inventory or equipment.

## v0.2.4: per-proxy ProcessSlots suppression

v0.2.4 addresses the remaining duplication where IED's normal NPC renderer shows the STR proxy's local inventory in addition to IEDSyncTogether's authoritative Custom Items.

The implementation follows IED 1.7.4's own control flow. In official source, `Controller::DoObjectEvaluation(ProcessParams&)` skips `ProcessSlots(a_params)` when NPC slots are disabled, then still executes `ProcessCustom(a_params)`. v0.2.4 applies that same choice only to STR proxies currently owned by IEDSyncTogether:

- ordinary IED `ProcessSlots` is skipped for a tracked remote proxy;
- `ProcessCustom` continues normally, so synchronized Custom Items remain active;
- the local player and ordinary NPCs keep normal IED behavior;
- inventory, favorites and equipped state are never changed.

The v0.2.2 `SelectSlotItem` experiment is **not** reused. That build returned `nullptr` from an internal selector whose result IED immediately dereferenced, producing deterministic crashes at `ImmersiveEquipmentDisplays.dll+00E380B` on both clients. v0.2.4 never hooks or alters `SelectSlotItem`.

### Runtime safety checks

The per-proxy hook is deliberately fail-closed. Before modifying memory it verifies:

1. known official IED 1.7.4 machine-code fingerprints;
2. that the inferred `DoObjectEvaluation -> ProcessSlots` site is a `CALL rel32`;
3. that PE x64 unwind metadata places the decoded target in the same function observed in the dual-client crash call stacks;
4. that the call target is the beginning of that runtime function.

If any validation fails, no memory is modified. IEDSyncTogether continues using the safe Custom Item renderer and logs that ordinary NPC slots may remain visible.

The official `ImmersiveEquipmentDisplays.dll` 1.7.4 file is never rebuilt, replaced, modified on disk or redistributed.

### Proxy identification inside ProcessSlots

IED 1.7.4 source defines `ProcessParams` as beginning with `ProcessParamsData`, whose first member is the actor's `Game::ObjectRefHandle`. Skyrim reference handles are 32-bit native values. When STR resolves a proxy, IEDSyncTogether stores that proxy's native reference handle. The hook reads only the first 32 bits of the already-valid `ProcessParams` argument and compares that value against the small tracked-proxy table.

No guessed `Actor*` offset, structure scan, private object construction, or invalid return object is used.

## Official IED API renderer

The synchronized objects themselves still use native Papyrus functions shipped by official IED 1.7.4:

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

IED's Custom Item Papyrus API requires `asPlugin` to name a loaded Skyrim data plugin. `build-vortex.ps1` generates a minimal ESL-flagged `IEDSyncTogether.esp`. It contains no gameplay records and exists only as IEDSyncTogether's ownership namespace for persistent Custom Item configuration.

Do not rename or disable this ESP.

## Required versions

- Skyrim Special Edition 1.6.1170
- SKSE 2.2.6
- Immersive Equipment Displays 1.7.4 official release
- Skyrim Together Reborn 1.8.0

## Runtime flow

```text
DataLoaded
    -> verify the official IED 1.7.4 fingerprint and ProcessSlots call site
    -> install the narrow ProcessSlots call wrapper if validation succeeds
    -> start IEDSyncTogether

PostLoadGame / NewGame
    -> wait for LAN peer + matching STR proxy

remote proxy resolved
    -> store its native Skyrim reference handle in the suppression table
    -> register it for authoritative Custom Item rendering

IED evaluates the tracked proxy
    -> skip ordinary ProcessSlots for this proxy only
    -> return to IED normally
    -> IED continues into ProcessCustom

remote STATE received
    -> store 19-slot snapshot using stable form identities
    -> create/update non-inventory Custom Items on the corresponding IED nodes

proxy disappears / STR disconnect
    -> remove its native handle from the suppression table
    -> delete IEDSyncTogether Custom Items
```

The legacy INI key `SuppressRemoteNpcDisplays` remains for compatibility. Keep it at the default value `1` for this test.

## Building

Set `VCPKG_ROOT` if necessary and run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1
```

The archive name follows `CMakeLists.txt`:

```text
dist/IEDSyncTogether-v0.2.4.zip
```

Expected contents include:

```text
Data/IEDSyncTogether.esp
Data/SKSE/Plugins/IEDSyncTogether.dll
Data/SKSE/Plugins/IEDSyncTogether.ini
```

The archive must **not** contain `ImmersiveEquipmentDisplays.dll`. Keep the official IED 1.7.4 installation enabled separately in Vortex.

## First v0.2.4 test

Install the same archive on both STR clients. At startup, the success path is:

```text
IEDSyncTogether v0.2.4 loading
IED 1.7.4 proxy ProcessSlots suppression installed: ...
IED integration mode: official Papyrus Custom Item API + source-aligned per-proxy ProcessSlots suppression
```

When the remote STR proxy is resolved:

```text
IED ProcessSlots suppression armed: proxy=FF...... handle=........
Remote IED state stored: ... slots=N
IED Custom Item render queued: proxy=FF...... slots=N dispatchAccepted=1
```

For the current Elir test, `slots=1` is expected because she has no equipped gear and one favorited weapon. Player1 should therefore see that single synchronized IED object rather than every item present in Elir's STR proxy inventory.

If startup instead logs `IED ProcessSlots suppression unavailable`, do not force the hook. Send the log; the renderer will remain on the safe fallback path.

## Historical note

v0.2.2 is retired and must not be used. Its `SelectSlotItem` null-return suppression caused the dual-client crash described above. v0.2.3 was the safe rollback. v0.2.4 uses a different interception level and semantics: skip the complete void `ProcessSlots` call, then let official IED continue into `ProcessCustom`.

`integration/ied-dev/` and `build-ied-patched.ps1` remain reference/development material only. Rebuilding IED is not a dependency of this project.

## License

IEDSyncTogether is MIT licensed. Immersive Equipment Displays is a separate dependency and is not redistributed or modified by IEDSyncTogether.
