# CT5 1.0 — Chrono Trigger PS5 Homebrew

Unofficial ROM-free PS5 homebrew build of CT5 with its HD-2D GPU renderer and layered 3D diorama view.

### Download
- **CT5-1.0-PS5.zip** — exact, unchanged archive supplied for this release (7,838,359 bytes; SHA-256 `709be8aaba64a2b5c4caeb27cf03000d7a4d70dc5e5fd8190d392c659ebf4b28`).

### Contents
- `PPSA28361/` — HD-2D PS5 homebrew app, including `eboot.bin`, `libc.prx`, and app metadata.
- `source/ct5-ps5-port.bundle` — Git source history for the port.
- `README.txt`, `CHANGELOG.txt`, licences and attribution.

**Packaging note:** The README mentions the standard `PPSA28360` edition, but **this ZIP contains only the HD-2D `PPSA28361` app**.

### Installation
1. Requires a compatible, homebrew-enabled PS5 and suitable app registration tools.
2. Copy `PPSA28361` to `/data/homebrew/PPSA28361`.
3. Supply your own legally obtained, headerless Chrono Trigger (USA) ROM as `rom.sfc` in the app folder, or use `/data/ct-recomp/`.
4. Register and launch the app using your PS5 homebrew setup.

For controller shortcuts, display modes, asset packs, limitations and troubleshooting, read `CT5-1.0/README.txt` inside the archive.

**No copyrighted game ROM or game art is bundled.** This is an unofficial fan project, not affiliated with Square Enix. The archive passed ZIP integrity checks; console behavior has not been independently validated in this release process.
