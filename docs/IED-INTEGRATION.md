# IED slot-override integration

IEDSyncTogether deliberately does not modify Skyrim Together inventories. It
publishes a small C ABI that tells IED whether a given remote-proxy slot must
be empty or must use a specific form already present in that proxy inventory.

Released IED 1.7.4 exposes slot reads, but no per-actor slot override. The
companion source patch in `integration/ied-dev/ied-sync-together.patch` adds
that missing query at the selection boundary in `IEquipment::SelectSlotItem`.
IED keeps responsibility for model loading, nodes, transforms and candidate
lifetime; the synchronized owner selection intentionally takes precedence over
the remote proxy's NPC item filters.

## ABI

`IEDST_QuerySlotOverride(actorFormID, slotIndex, outFormID)` returns:

- `0`: the actor is not a synchronized remote proxy; use normal IED behavior.
- `1`: the authoritative remote slot is empty; select no item.
- `2`: select `outFormID` if it exists in IED's candidates, otherwise select
  no item.

The ABI is declared in `include/IEDSyncTogether/Interface.h`. It is intentionally
C-only at the DLL boundary, so the two plugins do not share C++ objects or
allocator ownership.

## Building the patched IED DLL

The helper `build-ied-patched.ps1` targets the official IED 1.7.4 source commit:

```text
3f014c3e8574ef0e88b2ec0b7cdf58b86c9737b0
```

It creates a temporary detached Git worktree, applies the patch, builds the
`Release MT Post 629 143|x64` configuration, copies the resulting
`ImmersiveEquipmentDisplays.dll` into `build/ied-patched/`, then removes the
temporary worktree. The original `ied-dev` checkout is not modified.

Example:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-ied-patched.ps1 `
  -IedSourceRoot "C:\path\to\ied-dev"
```

IED 1.7.4 expects its historical build dependencies beside the `ied-dev`
checkout, including `sse-build-resources`, `imgui`, `assimp`, and its vcpkg
configuration.

The standalone `IEDSyncTogetherBridge.h` next to the patch is also provided for
review.

## Vortex packaging

`build-vortex.ps1` now requires a patched IED DLL. If none has already been
produced in `build/ied-patched/`, it invokes `build-ied-patched.ps1`
automatically.

The generated archive contains:

```text
Data/SKSE/Plugins/IEDSyncTogether.dll
Data/SKSE/Plugins/IEDSyncTogether.ini
Data/SKSE/Plugins/ImmersiveEquipmentDisplays.dll
Data/IEDSyncTogether.esp
Data/IEDSyncTogether/licenses/IED-LICENSE.txt
```

The patched `ImmersiveEquipmentDisplays.dll` intentionally overrides the DLL
from the normal IED installation. In Vortex, IEDSyncTogether must win that file
conflict. All other IED assets and configuration continue to come from the
original IED mod.

## Safety properties

- No item is added, removed, equipped or unequipped.
- A form is selected only when it already exists in IED's candidate set.
- Unknown actors keep normal IED behavior.
- An unresolved cross-client form identity produces an empty slot rather than
  falling back to an arbitrary NPC inventory item.
