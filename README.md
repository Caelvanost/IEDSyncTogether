# IEDSyncTogether

Compatibility and synchronization layer between
[Immersive Equipment Displays](https://www.nexusmods.com/skyrimspecialedition/mods/62001)
and Skyrim Together Reborn.

## Problem

Skyrim Together represents remote players as dynamically created `Actor`
proxies. IED therefore applies its NPC rules to them. In particular, the
player-only favorite check is not applied, which can make every compatible
item in the remote inventory appear on the proxy.

## Intended behavior

- Detect only Skyrim Together remote-player proxies.
- Capture the 19 forms actually selected by IED on the owning client.
- Exchange stable `plugin + local FormID` identities over the LAN.
- Reproduce that authoritative selection on the matching remote proxy.
- Never alter the real inventory or equipment managed by Skyrim Together.

## Status

The v0.1 prototype provides:

- Skyrim Together remote-proxy detection.
- Asynchronous capture of all 19 IED equipment slots.
- LAN peer discovery and UDP state exchange.
- Stable cross-client form identities (`plugin + local FormID`).
- An authoritative remote-slot query in IEDSyncTogether.
- A runtime hook for the official IED 1.7.4 DLL at the slot-selection boundary.
- Strict byte-signature checks so unsupported IED builds are rejected instead of patched blindly.
- An opt-in fallback that hides IED-cloned gear on remote proxies.

It is built for:

- Skyrim Special Edition 1.6.1170
- SKSE 2.2.6
- Immersive Equipment Displays 1.7.4
- Skyrim Together Reborn 1.8.0

## IED 1.7.4 runtime integration

Released IED 1.7.4 does not expose a public per-actor slot-override API.
IEDSyncTogether therefore intercepts the single call to
`IED::IEquipment::SelectSlotItem` inside `Controller::ProcessSlots`.

The supported official IED 1.7.4 binary was identified from the upstream
`SlavicPotato/ied-dev` commit:

```text
3f014c3e8574ef0e88b2ec0b7cdf58b86c9737b0
```

For that binary:

```text
SelectSlotItem call site RVA : 0xE3806
SelectSlotItem function RVA  : 0x146360
```

Before installing the hook, IEDSyncTogether validates both the original
five-byte `CALL rel32` instruction and the `SelectSlotItem` prologue. If either
signature differs, synchronization is disabled and an error is written to
`IEDSyncTogether.log`.

IED remains responsible for candidate discovery, model loading, transforms,
nodes, filters and normal selection. IEDSyncTogether only changes the selected
candidate when the actor is a synchronized Skyrim Together remote-player proxy.
If the synchronized form is not present in IED's locally generated candidate
set, the authoritative slot is left empty.

The official IED DLL is **not modified or bundled**. Keep the normal IED 1.7.4
installation enabled in Vortex.

The old source-patch files under `integration/ied-dev/` and
`build-ied-patched.ps1` are retained as development/history material; they are
not required by the runtime-hook package.

## Building

Set `VCPKG_ROOT` if needed, then run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1
```

The generated archive is:

```text
dist/IEDSyncTogether-v0.1.0.zip
```

Expected relevant contents:

```text
Data/IEDSyncTogether.esp
Data/SKSE/Plugins/IEDSyncTogether.dll
Data/SKSE/Plugins/IEDSyncTogether.ini
```

`ImmersiveEquipmentDisplays.dll` must **not** appear in this archive. Install
IED 1.7.4 separately and leave its normal scripts/assets/configuration enabled.
There is no Vortex DLL conflict to resolve between IED and IEDSyncTogether.

## Runtime integration semantics

`IEDST_QuerySlotOverride(actorFormID, slotIndex, outFormID)` and the internal
runtime hook use the same result contract:

- `0`: actor is not a synchronized remote proxy; IED uses normal behavior.
- `1`: authoritative slot is empty.
- `2`: use `outFormID` if that form is present in IED's candidate set; otherwise leave the slot empty.

No item is added, removed, equipped or unequipped by this integration.

## Installation

Install on both Skyrim Together clients:

1. Immersive Equipment Displays **1.7.4** (official release).
2. IEDSyncTogether's generated Vortex archive.
3. Skyrim Together Reborn 1.8.0 and the normal prerequisites for the mod list.

On startup, check `Documents/My Games/Skyrim Special Edition/SKSE/IEDSyncTogether.log`.
A supported installation should contain a line similar to:

```text
IED 1.7.4 runtime hook installed: call RVA=0xE3806 SelectSlotItem RVA=0x146360
```

## License

IEDSyncTogether is MIT licensed. Immersive Equipment Displays remains a
separate dependency and is not redistributed by the runtime-hook package.
