# Lapy elevation (fix 186d) — attribution, licence, and corresponding source

TrinityX360 elevates its own process to UID 0 before the write bench (issue #80) by **PATH B**: the
MIT eboot streams a bundled one-shot helper ELF (`/app0/lapy.elf`) to the local elfldr
(`127.0.0.1:9021`), and that helper elevates the process **cooperatively** through the ELV1 handshake
(the title calls `seteuid(geteuid())` itself; no disruptive ptrace hijack). This is the method proven
on this etaHEN console by TrinityXSX. It runs **only** when `/data/homebrew/PPSA83600/elevate-probe.txt`
says `on` (off by default); the user accepted this auto-invocation on their own console.

## Two LGPL components (NOT MIT)

1. **`lapy.elf`** — the owned one-shot helper, built in CI from **PS5-Lapy-JB-Daemon**
   (<https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon>, pinned
   `b48b7d7236eca25c7b9dfd6c040c763ae05dde80`), `build_owned_daemon.py --elf-helper --title PPSA83600`.
   Its `owned_root_daemon.c` / `donor_transaction.c` / `vnode_ref_probe.c` are MIT but it `#include`s
   and uses `lapy_elevation_protocol.h`, which is **LGPL-2.1-or-later**, so the built ELF is
   **LGPL-2.1-or-later**. Bundled at `/app0/lapy.elf`; **not** linked into `eboot.bin`.
2. **`port/vendor/lapy/`** — the ELV1/elfldr **client**, adapted from **prospero-win**
   (<https://github.com/mpereiraesaa/prospero-win>, tag **v0.3.0**, rev
   **`404cac6ebbb236d2486d2796fd28b9bd39f2caa6`**) `native/pw_lapy_elevation.c` +
   `native/ps5log/ps5log_ps5_net.c`, and the LGPL ELV1 header. These files are
   **LGPL-2.1-or-later**. The MIT eboot calls this client only through `lapy_elevation_client.h`
   (LGPL clause 5 — using the library via its header keeps the caller under its own licence).

Full licence text: `licenses/LAPY-LGPL-2.1.txt` (shipped beside the helper as `LAPY-LGPL-2.1.txt`).
TrinityX360's own `eboot.bin` and the fix-186 orchestration (`elevate_probe.hpp`, the launcher,
`canary_game_main.cpp`) stay **MIT**; no LGPL code is fused into those MIT source files.

## Corresponding source and relink (LGPL-2.1)

- `lapy.elf` source: PS5-Lapy-JB-Daemon at the pinned commit above (`--elf-helper --title PPSA83600`).
- Client source: prospero-win at the pinned rev above, plus `port/vendor/lapy/` in this repository.
- The eboot links the client (`port/vendor/lapy/lapy_elevation_client.c`) as a **separable, relinkable**
  unit; the whole project source is public, so anyone can modify the client and rebuild the eboot.

```sh
# helper
git clone https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon && cd PS5-Lapy-JB-Daemon
git checkout b48b7d7236eca25c7b9dfd6c040c763ae05dde80
python3 tools/build_owned_daemon.py --sdk "$PS5_PAYLOAD_SDK" --logging-client <ps5log.h dir> --elf-helper --title PPSA83600
# -> build/owned_root_helper-PPSA83600/lapy.elf
# client: prospero-win v0.3.0 @ 404cac6ebbb236d2486d2796fd28b9bd39f2caa6, native/pw_lapy_elevation.c (the basis of port/vendor/lapy)
```

## Experimental / risk

Upstream validated the owned-root backend on PS5 **FW 12.02**; the dev console here is **FW 13.60**, so
this is **experimental**. Elevation changes kernel credentials; legacy backends were linked to kernel
panics after many elevations per boot. **The stability risk is the user's to accept.** On any failure
the probe logs it and the app runs at UID 1 — it is never closed.
