# Patched Immersive Equipment Displays 1.7.4

This directory is reserved for the reproducible prebuilt IED 1.7.4 DLL used by
IEDSyncTogether.

Expected file:

```text
ImmersiveEquipmentDisplays.dll
```

The binary is built from the official `SlavicPotato/ied-dev` commit:

```text
3f014c3e8574ef0e88b2ec0b7cdf58b86c9737b0
```

with `integration/ied-dev/ied-sync-together.patch` applied.

The GitHub Actions workflow `.github/workflows/build-patched-ied.yml` is the
canonical way to reproduce and refresh the binary. The upstream IED MIT license
is retained in `third-party/IED-LICENSE.txt`.
