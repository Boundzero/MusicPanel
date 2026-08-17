# Firmware binaries

The web installer (`../index.html` via `../manifest.json`) serves the merged
firmware image from this folder.

Place the built image here as:

```
musicpanel-merged.bin
```

Produce it after a successful build:

```bash
idf.py merge-bin -o docs/firmware/musicpanel-merged.bin
```

The merged image contains the bootloader, partition table, and app in one file,
flashed at offset `0x0` — which is what `manifest.json` expects.

When you update the firmware, replace this file and bump the `version` field in
`../manifest.json` so the installer reflects the new release.
