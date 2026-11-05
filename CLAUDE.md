# rg251os

GBA-only Linux distro for the Anbernic RG351P. Start with README.md, then docs/hardware-research.md (hardware, decisions, current state and card layout) and docs/roadmap.md (pending hardware checks, known issues, backlog, past pitfalls). Both docs are in Chinese.

- Build with `make image` (output build/sdcard.img) and test with `make frontend-test`. Both must pass with zero warnings before committing.
- The frontend in app/frontend is the whole userland (PID 1). Each file starts with a header comment describing what it does; keep those accurate.
- There is no serial console. Hardware iterations go through the logs the frontend writes to `log/boot-NNNN/` on the GBA partition, so new features must log enough to be diagnosed from one test run, and the user may send log tarballs into the repo root (never commit them).
- Legacy ROCKNIX U-Boot is the chosen bootloader; mainline U-Boot stays buildable but is shelved until a UART is available.
- When a feature lands or a hardware result comes back, update docs/roadmap.md and the current state section of docs/hardware-research.md. Fighting game details (roster, mechanics, feel, its hardware results) go to the fighter repo's docs/roadmap.md (external/fighter, github.com/aisk/rg251fighter, private); rg251os docs only keep the integration side.
