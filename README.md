# IEDSyncTogether

Compatibility and synchronization layer between Immersive Equipment Displays (IED) and Skyrim Together Reborn (STR).

## v0.4.1 — Node Override orientation fix

v0.4.1 keeps the v0.4.0 scene-graph placement capture and fixes the remaining orientation mismatch on remote IED Custom Items.

v0.4.0 already synchronized the correct stable skeleton anchor, position and scale. Logs from both clients confirmed that those values were transported unchanged, while the rendered weapon orientation was still wrong. The cause was the Euler convention used on the wire: CommonLib's scene-graph `ToEulerAnglesXYZ` representation was being passed directly to IED, while IED Custom Item transforms use their default **extrinsic XYZ** rotation convention.

v0.4.1 reconstructs the captured rotation matrix and serializes an equivalent extrinsic XYZ triplet before sending the state over LAN. The scene-graph capture, anchor selection, position, scale and FormIdentity protocol remain otherwise unchanged.

For every displayed IED slot, the owner synchronizes:

- stable `plugin + local FormID` identity;
- a stable skeleton anchor node;
- the composed X/Y/Z position relative to that anchor;
- the composed rotation converted to IED extrinsic XYZ for remote restitution;
- the composed scale.

### Capturing the real Node Override result

IED's public Papyrus API exposes setters for Custom Item transforms but no getters for ordinary slot transforms. IEDSyncTogether therefore reads the **already evaluated PlayerCharacter scene graph**.

Starting from the gear node for the slot (`WeaponSword`, `WeaponBack`, `ShieldBack`, `QUIVER`, etc.), the plugin walks upward through IED/XPMSSE helper nodes whose names start with `MOV ` or `CME `.

Their local transforms are multiplied together using Skyrim's normal hierarchy rule:

```text
world = parent.world * local
```

The walk stops at the first stable non-IED skeleton node. The packet therefore contains the complete effective Node Override transform relative to that stable skeleton anchor, rather than merely the transform of the final `Weapon...` node.

No IED runtime function is detoured and no private IED controller structure is accessed.

### Remote restitution

On the remote STR proxy, IEDSyncTogether:

1. resolves the synchronized FormIdentity locally;
2. creates a non-inventory IED Custom Item;
3. attaches it to the captured stable skeleton anchor;
4. applies the captured position with `IED.SetItemPositionActor`;
5. applies the v0.4.1 extrinsic rotation with `IED.SetItemRotationActor`;
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

The slot payload begins with the stable identity:

```text
plugin:localFormID
```

An optional placement extension contains:

```text
anchor + position XYZ + IED-extrinsic rotation XYZ + scale
```

Old slot data without placement information remains readable.

## Build

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1
```

Expected archive:

```text
dist/IEDSyncTogether-v0.4.1.zip
```

Expected relevant contents:

```text
Data/IEDSyncTogether.esp
Data/SKSE/Plugins/IEDSyncTogether.dll
Data/SKSE/Plugins/IEDSyncTogether.ini
```

`ImmersiveEquipmentDisplays.dll` is never redistributed or modified.

## v0.4.1 test protocol

Install the same v0.4.1 archive on both clients and keep `disable_npc_slots=true` on both PCs.

Recommended first test:

1. Kahel: keep the Iron Hunter's Spear favorited/stowed on the intended back placement.
2. Connect Elir.
3. On Player2, verify both the spear position **and orientation**.
4. Then repeat with the Iron Shield explicitly favorited/stowed so slot 16 is present in the log.
5. Compare owner `IED placement captured` / `IED rotation wire conversion` lines with observer `IED placement applied` lines.

For the previously observed Kahel spear capture, v0.4.0 logged approximately:

```text
scene XYZ = (88.45, 3.20, 28.52)
```

v0.4.1 should serialize an equivalent IED extrinsic rotation close to:

```text
IED extrinsic XYZ = (88.24, 28.60, -2.36)
```

Expected logs include:

```text
IEDSyncTogether v0.4.1 loading
IED rotation wire conversion: slot=0 sceneXYZ=(...) iedExtrinsicXYZ=(...)
IED placement applied: proxy=XXXXXXXX slot=0 anchor="..." pos=(...) rot=(...) scale=...
```

## Helmet Toggle 2 / arbitrary Custom Items

The current IEDSyncTogether slot protocol synchronizes IED's 19 ordinary equipment display slots. It does **not yet** synchronize arbitrary external IED Custom Items, Dynamic Armor Variants visibility state, or third-party animation events. Helmet Toggle 2 therefore requires a separate synchronization path and is intentionally outside the v0.4.1 rotation patch.

## Safety history

- v0.2.2: private `SelectSlotItem` suppression caused IED crashes and was rejected.
- v0.2.4/v0.2.5: private runtime `ProcessSlots` experiments were rejected after side effects/save-load crashes.
- v0.2.6: returned to public IED APIs.
- v0.3.1: public `AddActorBlock` suppressed both stock slots and Custom Items, so it was rejected.
- v0.3.3: diagnostics proved `disable_npc_slots=true` plus normal NPC proxy classification provides the correct suppression architecture; favorite restitution works.
- v0.4.0: added composed scene-graph Node Override capture and public-API transform restitution.
- v0.4.1: converts captured scene-graph rotations to IED's extrinsic XYZ convention before LAN serialization.

## License

IEDSyncTogether is MIT licensed. Immersive Equipment Displays is a separate dependency.
