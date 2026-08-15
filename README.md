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
- A versioned C ABI through which IED can request an authoritative slot.
- An IED 1.7.4 source integration that queries that ABI at the slot-selection boundary.
- A build pipeline capable of packaging the patched IED DLL together with IEDSyncTogether.
- An opt-in fallback that hides IED-cloned gear on remote proxies.

It is built against:

- Skyrim Special Edition 1.6.1170
- SKSE 2.2.6
- Immersive Equipment Displays 1.7.4
- Skyrim Together Reborn

## Patched IED integration

Released IED 1.7.4 does not expose a public per-actor slot-override API.
IEDSyncTogether therefore carries a small source patch in:

```text
integration/ied-dev/ied-sync-together.patch
```

The build helper targets the official `SlavicPotato/ied-dev` 1.7.4 commit:

```text
3f014c3e8574ef0e88b2ec0b7cdf58b86c9737b0
```

The patch adds a C ABI query at `IEquipment::SelectSlotItem`. IED remains
responsible for candidate discovery, model loading, transforms and nodes;
IEDSyncTogether only decides which already-valid IED candidate is authoritative
for a synchronized remote player.

The generated Vortex archive includes the patched:

```text
Data/SKSE/Plugins/ImmersiveEquipmentDisplays.dll
```

This intentionally conflicts with the DLL from the normal IED installation.
In Vortex, IEDSyncTogether must win that file conflict / load after Immersive
Equipment Displays. All other IED assets, configuration files and scripts still
come from the original IED installation.

IED is MIT licensed. The required upstream copyright/license notice is included
in the generated package under:

```text
Data/IEDSyncTogether/licenses/IED-LICENSE.txt
```

## Building

### 1. Prepare an IED 1.7.4 source workspace

IED 1.7.4 uses a Visual Studio workspace with historical sibling dependencies,
including `sse-build-resources`, `imgui`, `assimp`, and its vcpkg setup. Prepare
a checkout of `SlavicPotato/ied-dev` that can build with those dependencies.

Point IEDSyncTogether at it with either:

```powershell
$env:IED_SOURCE_ROOT = "C:\path\to\ied-dev"
```

or pass `-IedSourceRoot` directly.

The helper never edits that checkout. It creates a temporary detached Git
worktree at the exact 1.7.4 commit, applies the IEDSyncTogether patch there,
builds the `Release MT Post 629 143|x64` configuration, copies the resulting
DLL into `build/ied-patched/`, then removes the temporary worktree.

You can build only the patched IED DLL with:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-ied-patched.ps1 `
  -IedSourceRoot "C:\path\to\ied-dev"
```

### 2. Build the complete Vortex archive

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1 `
  -IedSourceRoot "C:\path\to\ied-dev"
```

`build-vortex.ps1` builds IEDSyncTogether, builds the patched IED DLL when it is
not already available, and refuses to create a complete package if the patched
DLL is missing.

For development, an already-built patched DLL may be supplied explicitly:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1 `
  -PatchedIedDll "C:\path\to\ImmersiveEquipmentDisplays.dll"
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
Data/SKSE/Plugins/ImmersiveEquipmentDisplays.dll
Data/IEDSyncTogether/licenses/IED-LICENSE.txt
```

## Runtime integration

`IEDST_QuerySlotOverride(actorFormID, slotIndex, outFormID)` returns:

- `0`: actor is not a synchronized remote proxy; IED uses normal behavior.
- `1`: authoritative slot is empty.
- `2`: use `outFormID` if that form is present in IED's candidate set.

No item is added, removed, equipped or unequipped by this integration.

## License

IEDSyncTogether is MIT licensed. The bundled modified IED binary remains subject
to IED's MIT license and copyright notice, included in `third-party/IED-LICENSE.txt`.
