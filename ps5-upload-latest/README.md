# CT5 PS5 latest source and standard build staging

This staging branch triggers a GitHub Actions publishing workflow. It verifies the incremental Git bundle at source commit `c69ef4c2434c729c4d152eaf369663ba684f2c95` and the ROM-free PPSA28360 standard build ZIP. It merges the source into `ps5/port` without force and publishes the verified standard ZIP. The newer HD-2D GPU ZIP is not included in this staging step. `main` is not modified.
