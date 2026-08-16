# IEDSyncTogether

Compatibility and synchronization layer between Immersive Equipment Displays (IED) and Skyrim Together Reborn (STR).

## Target behavior

STR represents remote players as dynamically created actor proxies. IED normally evaluates those proxies as NPCs, so the owning player's displayed/favorited equipment is not reproduced reliably. IEDSyncTogether captures the 19 IED display slots on the owning client, exchanges stable form identities over LAN, binds the remote state to the matching STR proxy, and asks IED to render that authoritative state without changing the proxy inventory or equipment.

## v0.1.14

v0.1.14 replaces the experimental binary `SelectSlotItem` hook with a source-level bridge built against the exact IED 1.7.4 source commit:

```text
3f014c3e8574ef0e88b2ec0b7cdf58b86c9737b0
```

The previous v0.1.13 transparent relay was intentionally a no-op and could not display remote slots. Earlier v0.1.9-v0.1.12 binary ABI experiments were also removed from the active path because they could crash during save loading.

The source patch now does two things for synchronized STR proxies:

1. `InventoryInfoCollector` injects each authoritative remote form into IED's collector before `GenerateSlotCandidates()` runs. This is required because an STR proxy does not necessarily contain the owning player's real inventory item.
2. `IEquipment::SelectSlotItem` selects the synchronized FormID from that generated candidate set, or leaves the slot empty when the remote state says it is empty.

The synthetic collector entry is used only by IED's display pipeline. IEDSyncTogether does not add, remove, equip or unequip any gameplay item.

## Required versions

- Skyrim Special Edition 1.6.1170
- SKSE 2.2.6
- Immersive Equipment Displays 1.7.4 source-patched by this project
- Skyrim Together Reborn 1.8.0

## Building the patched IED DLL

The upstream IED source patch is under `integration/ied-dev/`.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-ied-patched.ps1
```

The patched DLL must end up at:

```text
third-party/IED-1.7.4/ImmersiveEquipmentDisplays.dll
```

It exports `IEDST_GetSourceBridgeVersion`; IEDSyncTogether logs the detected bridge version at `DataLoaded`.

## Building the Vortex package

With the patched IED DLL present, run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1
```

The archive name follows `CMakeLists.txt`:

```text
dist/IEDSyncTogether-v0.1.14.zip
```

The archive contains:

```text
Data/IEDSyncTogether.esp
Data/SKSE/Plugins/IEDSyncTogether.dll
Data/SKSE/Plugins/IEDSyncTogether.ini
Data/SKSE/Plugins/ImmersiveEquipmentDisplays.dll
```

For this experimental build the packaged `ImmersiveEquipmentDisplays.dll` must win the Vortex conflict against the official IED 1.7.4 DLL. Keep the rest of the official IED mod (scripts, assets and configuration) installed.

## Runtime checks

Install the same v0.1.14 package on both STR clients. In `Documents/My Games/Skyrim Special Edition/SKSE/IEDSyncTogether.log`, a correct installation should contain:

```text
IEDSyncTogether v0.1.14 loading
IED source bridge v1 detected; binary SelectSlotItem shim is disabled
```

When a remote STR player is resolved, the log should still show the LAN peer/proxy binding and remote state storage. With `SuppressRemoteNpcDisplays=0`, there must no longer be a new `Suppressed IED NPC display` line for that proxy.

If only one remote object appears and the log says `Remote IED state stored ... (slots=1)`, the rendering path is working and the next issue is the owning client's capture state rather than proxy rendering.

## License

IEDSyncTogether is MIT licensed. Immersive Equipment Displays remains third-party software; the source patch is maintained only as a compatibility integration for the pinned IED 1.7.4 build.
