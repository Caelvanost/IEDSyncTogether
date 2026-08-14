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

The v0.1 prototype now builds a Vortex package and provides:

- Skyrim Together remote-proxy detection.
- Asynchronous capture of all 19 IED equipment slots.
- LAN peer discovery and UDP state exchange.
- Stable cross-client form identities (`plugin + local FormID`).
- A versioned C ABI through which IED can request an authoritative slot.
- An opt-in fallback that hides IED-cloned gear on remote proxies.

It is built against:

- Skyrim Special Edition 1.6.1170
- SKSE 2.2.6
- Immersive Equipment Displays 1.7.4
- Skyrim Together Reborn

Released IED 1.7.4 has no public per-actor slot-override API. Full visual
application therefore requires the small companion IED source patch documented
in [docs/IED-INTEGRATION.md](docs/IED-INTEGRATION.md). Without that integration,
the DLL remains useful for diagnostics and can optionally suppress the incorrect
NPC display without touching equipped items or the real STR inventory.

## Build

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1
```

The generated archive is `IEDSyncTogether-v0.1.0-Vortex.zip`.

## License

MIT
