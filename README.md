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
- Exchange stable `plugin + local FormID` identities through the STR session.
- Reproduce that authoritative selection on the matching remote proxy.
- Never alter the real inventory or equipment managed by Skyrim Together.

## Status

The v0.3.1 prototype now builds a Vortex/FOMOD package and provides:

- Skyrim Together remote-proxy detection.
- Asynchronous capture of all 19 IED equipment slots.
- STR Plugin Messaging transport on channel `strpm.iedsynctogether.slots.v1`.
- Default networking with no IEDSyncTogether port forwarding.
- STRPM diagnostics guard: the default profile requires the `StrBridge` backend.
- Legacy UDP LAN/direct-peer/relay transport, kept as an opt-in compatibility path.
- Optional shared-secret HMAC authentication.
- Stable cross-client form identities (`plugin + local FormID`).
- A versioned C ABI through which IED can request an authoritative slot.
- An opt-in fallback that hides IED-cloned gear on remote proxies.

It is built against:

- Skyrim Special Edition 1.6.1170
- SKSE 2.2.6
- Immersive Equipment Displays 1.7.4
- Skyrim Together Reborn
- STRPluginMessagingAPI interface v2 + diagnostics v2

Released IED 1.7.4 has no public per-actor slot-override API. Full visual
application therefore requires the small companion IED source patch documented
in [docs/IED-INTEGRATION.md](docs/IED-INTEGRATION.md). Without that integration,
the DLL remains useful for diagnostics and can optionally suppress the incorrect
NPC display without touching equipped items or the real STR inventory.

## Build

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-vortex.ps1
```

The generated archive is `IEDSyncTogether-v0.3.1-STRPM-FOMOD.zip`.

## Recommended remote setup

1. Install `STRPluginMessagingAPI.dll` with Skyrim Together Reborn.
2. Install IEDSyncTogether and keep the FOMOD default:
   `STR plugin messaging / no port forwarding`.
3. Connect with Skyrim Together normally.

IEDSyncTogether dynamically loads `STRPluginMessagingAPI.dll`, registers the
`strpm.iedsynctogether.slots.v1` channel, and sends the same compact slot
payloads through the messaging plugin. It does not bind or expose its own
Internet UDP port in the default profile.

The default INI also sets `RequireStrBridge=1`. That means IEDSyncTogether
queries `STR_QueryPluginMessagingDiagnostics` and refuses the STRPM transport
unless STRPM reports `activeBackend=StrBridge`. This prevents an older or dev
STRPM UDP backend from silently becoming the "no port forwarding" path.

The old direct UDP transport remains available for diagnostics:

```ini
[Network]
Transport=UDP
RequireStrBridge=0
RemotePeers=player-one.example:38471,203.0.113.8:38472
SharedSecret=
```

`Transport=Auto` tries STR Plugin Messaging first, then falls back to UDP when
STRPluginMessagingAPI is unavailable or rejected by `RequireStrBridge=1`.
`UdpFallback=1` enables the same fallback while keeping `Transport=STR`; the
packaged default leaves it disabled.

For stricter legacy UDP play, set the same `SharedSecret=` on every client and
the relay host. `AutoSharedSecretFromSTR=1` can reuse STR's saved password when
available, matching MorphSyncTogether's previous UDP behavior.

## License

MIT
