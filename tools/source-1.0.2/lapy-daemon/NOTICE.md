# lapy-root-daemon.elf — attribution, licence, and corresponding source

`lapy-root-daemon.elf` is an **optional, user-loaded** PS5 elevation daemon bundled with TrinityX360
for issue #80 (fix 186). It is a **separate helper ELF**: it is **not** linked into `eboot.bin`, is
**not** auto-launched by TrinityX360, and TrinityX360 never writes it to the console or sends it to a
loader. The **user loads it themselves** (like `PS5X360-Helper.elf`). When loaded, TrinityX360's fix
186 probe (enabled with `elevate-probe.txt`) cooperates with it through the `/download0/elevate_proc`
marker to run the write benchmark as UID 0.

## Licence — LGPL-2.1-or-later, NOT MIT

This binary is **LGPL-2.1-or-later**, not MIT. Although the upstream repository's `LICENSE` file is
MIT (Arksama / Team PHU) and GitHub reports the repo as NOASSERTION (mixed), the daemon source
`owned_root_daemon.c` `#include`s `source/lapy_elevation_protocol.h`, which carries
`SPDX-License-Identifier: LGPL-2.1-or-later`. The built `lapy-root-daemon.elf` therefore incorporates
LGPL-2.1-or-later code and is distributed under **LGPL-2.1-or-later**. The full licence text is in
`licenses/LAPY-LGPL-2.1.txt` (shipped beside the ELF as `LAPY-LGPL-2.1.txt`).

TrinityX360's own `eboot.bin` and the fix 186 marker probe remain **MIT**; no LGPL header is pulled
into TrinityX360 sources.

## Upstream and corresponding source

- Project: **PS5-Lapy-JB-Daemon** (owned-root fork) — <https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon>
- Pinned commit: **`b48b7d7236eca25c7b9dfd6c040c763ae05dde80`** (2026-10-06), recorded in `manifest.json`.

The complete corresponding source for the shipped binary is that repository at the pinned commit.
To relink / rebuild it, per LGPL-2.1:

```sh
git clone https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon
cd PS5-Lapy-JB-Daemon && git checkout b48b7d7236eca25c7b9dfd6c040c763ae05dde80
# PS5_PAYLOAD_SDK = a ps5-payload-sdk install (prospero-clang); logging-client = any dir with a
# ps5log.h (TrinityX360 builds with its own MIT no-op stub in tools/source-1.0.2/lapy-daemon/ps5log-stub).
python3 tools/build_owned_daemon.py --sdk "$PS5_PAYLOAD_SDK" --logging-client <ps5log.h dir> --service --title '*'
# -> build/owned_root_daemon-service/lapy-root-daemon.elf
```

The `ps5log.h` stub TrinityX360 uses for the build is its own MIT code and is not part of Lapy.

## Experimental / risk

Upstream validated the owned-root backend on PS5 **firmware 12.02 only**; other firmware is
experimental and gated by the daemon's own runtime layout check, which refuses an unvalidated layout
(then elevation simply does not happen). The development console for TrinityX360 is **FW 13.60**, so
this helper is **experimental** there. Elevation changes kernel credentials; legacy elevation
backends were associated with kernel panics after many elevations per boot. **The stability risk is
the user's to accept.**
