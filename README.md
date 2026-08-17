# IEDSyncTogether

Compatibility and synchronization layer between Immersive Equipment Displays (IED) and Skyrim Together Reborn (STR).

## v0.4.0 — placement and Node Override synchronization

v0.4.0 keeps the validated v0.3.x LAN/form synchronization and adds restitution of the effective IED placement used by the owning player.

For every displayed IED slot, the owner synchronizes:

- stable `plugin + local FormID` identity;
- a stable skeleton anchor node;
- the composed X/Y/Z position relative to that anchor;
- the composed XYZ rotation;
- the composed scale.

### Capturing the real Node Override result

IED's public Papyrus API exposes setters for Custom Item transforms but no getters for ordinary slot transforms. IEDSyncTogether therefore reads the **already evaluated PlayerCharacter scene graph**.

Starting from the gear node for the slot (`WeaponSword`, `WeaponBack`, `ShieldBack`, `QUIVER`, etc.), v0.4.0 walks upward through IED/XPMSSE helper nodes whose names start with `MOV ` or `CME `.

Their local transforms are multiplied together using Skyrim's normal hierarchy rule:

```text
world = parent.world * local
```

The walk stops at the first stable non-IED skeleton node. The packet therefore contains the complete effective Node Override transform relative to that stable skeleton anchor, rather than merely the transform of the final `Weapon...` node.

This means a spear repositioned to the back or a shield moved by an IED/XPMSSE profile can be reproduced without requiring the observing client to recalculate the same MOV/CME hierarchy.

No IED runtime function is detoured and no private IED controller structure is accessed.

### Remote restitution

On the remote STR proxy, IEDSyncTogether:

1. resolves the synchronized FormIdentity locally;
2. creates a non-inventory IED Custom Item;
3. attaches it to the captured stable skeleton anchor;
4. applies the captured position with `IED.SetItemPositionActor`;
5. applies the captured rotation with `IED.SetItemRotationActor`;
6. applies the captured scale with `IED.SetItemScaleActor`;
7. evaluates the proxy.

If placement capture is unavailable, the FormID is still restored using the canonical gear node as a fallback.

## Required IED setting

On every STR client, IED must have the following value in:

```text
Data\SKSE\Plugins\IED\Settings.json
```

```json
"disable_npc_slots": true
```

IEDSyncTogether reads this setting for diagnostics but never writes it. With it enabled, IED skips ordinary inventory-driven slot rendering on STR proxies while continuing to render IEDSyncTogether Custom Items.

## Synchronized slot state

The slot payload still begins with the stable identity:

```text
plugin:localFormID
```

v0.4.0 appends an optional placement extension containing:

```text
anchor + position XYZ + rotation XYZ + scale
```

Old slot data without placement information remains readable.

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
6. Compare the owner `IED placement captured` lines with the observer `IED placement applied` lines.

Expected logs include:

```text
IEDSyncTogether v0.4.0 loading
IED placement chain: slot=0 gearNode="WeaponSword" helperDepth=... anchor="..."
IED placement captured: slot=0 gearNode="WeaponSword" anchor="..." pos=(...) rot=(...) scale=...
IED placement applied: proxy=XXXXXXXX slot=0 anchor="..." pos=(...) rot=(...) scale=...
IED Custom Item render queued: proxy=XXXXXXXX slots=3 placements=3 dispatchAccepted=1
```

## Safety history

- v0.2.2: private `SelectSlotItem` suppression caused IED crashes and was rejected.
- v0.2.4/v0.2.5: private runtime `ProcessSlots` experiments were rejected after side effects/save-load crashes.
- v0.2.6: returned to public IED APIs.
- v0.3.1: public `AddActorBlock` suppressed both stock slots and Custom Items, so it was rejected.
- v0.3.3: diagnostics proved `disable_npc_slots=true` plus normal NPC proxy classification provides the correct suppression architecture; favorite restitution works.
- v0.4.0: adds composed scene-graph Node Override capture and public-API transform restitution without reintroducing runtime hooks.

## License

IEDSyncTogether is MIT licensed. Immersive Equipment Displays is a separate dependency.
