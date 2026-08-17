# IEDSyncTogether

Compatibility and synchronization layer between Immersive Equipment Displays (IED) and Skyrim Together Reborn (STR).

## v0.4.0 — placement and Node Override synchronization

v0.4.0 keeps the validated v0.3.x LAN/form synchronization and adds restitution of the effective IED placement used by the owning player.

For every one of IED's 19 equipment slots, the owner now sends:

- stable `plugin + local FormID` identity;
- effective parent node selected by IED/XPMSSE after Node Overrides have been evaluated;
- local X/Y/Z position of the gear node;
- local XYZ rotation;
- local scale.

The placement is captured from the live PlayerCharacter scene graph. No IED function is detoured and no private IED controller structure is patched.

On the remote STR proxy, IEDSyncTogether creates a non-inventory IED Custom Item, attaches it directly to the captured effective parent node, and applies the captured transform through IED's public Papyrus API:

- `IED.SetItemNodeActor`
- `IED.SetItemPositionActor`
- `IED.SetItemRotationActor`
- `IED.SetItemScaleActor`

This is intended to reproduce placements such as a spear moved from the sword hip slot to the back and a stowed shield placed on the back.

## Required IED setting

On every STR client, IED must have the following value in `Data\SKSE\Plugins\IED\Settings.json`:

```json
"disable_npc_slots": true
```

IEDSyncTogether reads this setting for diagnostics but never writes it. With it enabled, IED skips ordinary inventory-driven slot rendering on STR proxies while continuing to render IEDSyncTogether Custom Items.

## Architecture

```text
owner IED GetSlottedForm
    -> stable FormIdentity
    -> inspect live gear node in PlayerCharacter scene graph
    -> capture effective parent + local transform
    -> encode optional placement extension in STATE packet
    -> LAN
    -> resolve FormIdentity on remote client
    -> create proxy-only IED Custom Item
    -> attach to captured parent node
    -> apply position / rotation / scale
    -> IED.Evaluate(proxy)
```

The placement extension is backwards-readable: the existing slot token still begins with `plugin:localFormID`; v0.4.0 appends placement data only when it was successfully captured.

## Slot nodes used for capture

IEDSyncTogether follows IED's `ObjectSlot` order and captures the live scene-graph state of the corresponding gear nodes, including `WeaponSword`, `WeaponBack`, `WeaponBow`, `ShieldBack` and `QUIVER`.

The important distinction from v0.3.x is that these names are now capture anchors, not necessarily the final remote attachment point. The final attachment point is the captured parent node selected by the owner's IED/Node Override state.

## Build

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1
```

Expected archive:

```text
dist/IEDSyncTogether-v0.4.0.zip
```

Expected relevant contents:

```text
Data/IEDSyncTogether.esp
Data/SKSE/Plugins/IEDSyncTogether.dll
Data/SKSE/Plugins/IEDSyncTogether.ini
```

`ImmersiveEquipmentDisplays.dll` is never redistributed or modified.

## v0.4.0 test protocol

Install the same v0.4.0 archive on both clients and keep `disable_npc_slots=true` on both PCs.

Recommended first test:

1. Kahel: keep Iron Hunter's Spear, Springsteel Bow and Iron Shield favorited/stowed.
2. Confirm locally that the spear and shield use the intended IED/Node Override placements.
3. Connect Elir.
4. On Player2, verify that Kahel's spear is restored on the same back placement and the shield is restored in its stowed placement rather than in hand.
5. Repeat in the opposite direction with an Elir favorite.
6. Compare `IED placement captured` on the owner with `IED placement applied` on the observing client.

Expected log examples:

```text
IEDSyncTogether v0.4.0 loading
IED placement captured: slot=0 gearNode="WeaponSword" parent="..." pos=(...) rot=(...) scale=...
IED placement captured: slot=16 gearNode="ShieldBack" parent="..." pos=(...) rot=(...) scale=...
IED placement applied: proxy=XXXXXXXX slot=0 node="..." pos=(...) rot=(...) scale=...
IED Custom Item render queued: proxy=XXXXXXXX slots=3 placements=3 dispatchAccepted=1
```

If a gear node cannot be found in the owner's loaded skeleton, the FormID is still synchronized and the remote renderer falls back to the standard node for that slot.

## Safety history

- v0.2.2: private `SelectSlotItem` suppression caused IED crashes and was rejected.
- v0.2.4/v0.2.5: private runtime `ProcessSlots` experiments were rejected after side effects/save-load crashes.
- v0.2.6: returned to public IED APIs.
- v0.3.1: public `AddActorBlock` suppressed both stock slots and Custom Items, so it was rejected.
- v0.3.3: diagnostics proved `disable_npc_slots=true` + normal NPC proxy classification provides the correct suppression architecture; favorite restitution works.
- v0.4.0: adds scene-graph placement capture and public-API transform restitution without reintroducing runtime hooks.

## License

IEDSyncTogether is MIT licensed. Immersive Equipment Displays is a separate dependency.
