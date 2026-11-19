# PS5 port upload staging (2026-11-19)

This staging branch contains the original incremental Git bundle and two ROM-free PS5 build archives. The 7.4MB HD-2D archive is stored in four binary Git blobs to bypass connector request limits; the workflow validates and joins those files into the original ZIP.

The workflow in .github/workflows/ps5-port-publish.yml restores all 17 PS5 port commits, adds both PS5 ZIPs and pushes branch ps5/port without modifying main or force-pushing. If Actions is disabled, the branch can still be published from a local Git clone using the same uploaded source files.

The ZIPs contain native eboot.bin files, libc.prx and PS5 metadata, no game ROM. Hardware functionality has not been independently verified.
