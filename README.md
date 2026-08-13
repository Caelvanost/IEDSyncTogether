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
- Exchange stable `plugin + local FormID` identities over LAN or Internet.
- Reproduce that authoritative selection on the matching remote proxy.
- Never alter the real inventory or equipment managed by Skyrim Together.

## Status

The v0.2 prototype now builds a Vortex/FOMOD package and provides:

- Skyrim Together remote-proxy detection.
- Asynchronous capture of all 19 IED equipment slots.
- LAN peer discovery and UDP state exchange.
- Internet direct peers, STR direct-connect auto-detection, and optional relay host mode.
- Optional shared-secret HMAC authentication.
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

The generated archive is `IEDSyncTogether-v0.2.0-FOMOD.zip`.

## Recommended Internet setup

On Player1 / the STR host:

1. Forward UDP port `38471` from the router to the host PC.
2. Allow UDP `38471` through Windows Firewall.
3. In the FOMOD, choose `Player1 host / Internet relay`.

On each remote client:

1. Install the default `Client / LAN / no Internet relay` profile.
2. Connect to Player1 with STR direct connect as usual.
3. No IEDSyncTogether INI edit is normally required.

Clients reuse STR's saved direct-connect host automatically. If STR saved
`82.65.51.103:10578`, IEDSyncTogether sends its UDP traffic to
`82.65.51.103:38471`.

Manual peers are still supported:

```ini
[Network]
RemotePeers=player-one.example:38471,203.0.113.8:38472
SharedSecret=
```

For stricter Internet play, set the same `SharedSecret=` on every client and
the relay host. `AutoSharedSecretFromSTR=1` can reuse STR's saved password when
available, matching MorphSyncTogether's behavior.

## License

MIT
