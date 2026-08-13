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

Early prototype. The repository is being built and tested against:

- Skyrim Special Edition 1.6.1170
- SKSE 2.2.6
- Immersive Equipment Displays 1.7.4
- Skyrim Together Reborn

The first milestone is a safe diagnostic build that identifies proxies and
captures IED slot state. Inventory suppression will only be enabled once it
can be scoped to those proxies without affecting normal NPCs or equipped gear.

## License

MIT

