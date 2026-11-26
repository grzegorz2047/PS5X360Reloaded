#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Fix 186d (issue #80): verify the bundled Lapy elevation parts are packaged with full LGPL-2.1
# compliance, WITHOUT building them. Two LGPL parts: the owned one-shot helper lapy.elf (built in CI
# from PS5-Lapy-JB-Daemon, --elf-helper --title PPSA83600) and the ELV1/elfldr client module
# (port/vendor/lapy, adapted from prospero-win; its own SPDX/attribution is checked by
# check-elevate-probe.cpp). Here we check the manifest, licence, NOTICE, build script, stub and
# CREDITS. Run from anywhere.
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
LAPY = REPO / "tools/source-1.0.2/lapy-daemon"
SHA40 = re.compile(r"^[0-9a-f]{40}$")
failures = []


def expect(cond, what):
    if not cond:
        failures.append(what)


def read(path):
    try:
        return (REPO / path).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


# manifest.json
try:
    m = json.loads((LAPY / "manifest.json").read_text(encoding="utf-8"))
except Exception as exc:  # noqa: BLE001
    m = {}
    failures.append(f"manifest.json unreadable: {exc}")
up = m.get("upstream", {})
lic = m.get("license", {})
dep = m.get("deployment", {})
expect(SHA40.match(up.get("lapy_helper", {}).get("commit", "")) is not None,
       "manifest pins the Lapy helper commit")
expect(up.get("prospero_win_client", {}).get("commit", "") == "404cac6ebbb236d2486d2796fd28b9bd39f2caa6",
       "manifest pins the prospero-win client rev 404cac6")
expect("mpereiraesaa/prospero-win" in up.get("prospero_win_client", {}).get("repo", ""),
       "manifest records the prospero-win repo")
expect(lic.get("spdx") == "LGPL-2.1-or-later", "manifest license spdx is LGPL-2.1-or-later")
expect("not mit" in lic.get("note", "").lower(), "manifest says NOT MIT")
expect("mit" in lic.get("note", "").lower() and "eboot" in lic.get("note", "").lower(),
       "manifest says the eboot stays MIT")
expect("LAPY-LGPL-2.1.txt" in lic.get("license_text", ""), "manifest points at the LGPL text")
expect("relink" in lic.get("relink", "").lower() or "separable" in lic.get("relink", "").lower(),
       "manifest explains relinkability")
expect("--elf-helper --title PPSA83600" in json.dumps(m), "manifest records the owned-helper build")
model = dep.get("model", "").lower()
expect("elfldr" in model and "9021" in model, "manifest deployment notes the elfldr stream")
expect("off by default" in model or "elevate-probe.txt" in model, "manifest notes off-by-default")

# LGPL text
lgpl = read("licenses/LAPY-LGPL-2.1.txt")
expect("GNU LESSER GENERAL PUBLIC LICENSE" in lgpl and "Version 2.1" in lgpl,
       "licenses/LAPY-LGPL-2.1.txt is the LGPL-2.1 text")

# NOTICE.md
notice = (LAPY / "NOTICE.md").read_text(encoding="utf-8", errors="replace") if (LAPY / "NOTICE.md").exists() else ""
plain = notice.replace("*", "").replace("`", "")
expect("LGPL-2.1-or-later" in notice and "not MIT" in notice.lower() or "NOT MIT" in notice,
       "NOTICE states LGPL, not MIT")
expect("404cac6ebbb236d2486d2796fd28b9bd39f2caa6" in notice and "prospero-win" in notice,
       "NOTICE credits prospero-win at the pinned rev")
expect("b48b7d7236eca25c7b9dfd6c040c763ae05dde80" in notice, "NOTICE pins the Lapy helper commit")
expect("elfldr" in notice.lower() and "ELV1" in notice, "NOTICE describes the elfldr/ELV1 path")
expect("relink" in notice.lower() and "separable" in notice.lower(),
       "NOTICE documents relink / separable unit")
expect("experimental" in notice.lower() and "risk" in notice.lower(), "NOTICE has the risk note")
expect("not" in plain.lower() and "fused" in plain.lower(), "NOTICE says not fused into MIT source")

# ps5log stub (ours, MIT)
stub = (LAPY / "ps5log-stub/ps5log.h").read_text(encoding="utf-8", errors="replace") if (LAPY / "ps5log-stub/ps5log.h").exists() else ""
expect("SPDX-License-Identifier: MIT" in stub and "ps5log_printf" in stub,
       "the ps5log stub is our MIT no-op")

# build script
build = (LAPY / "build-lapy-daemon.sh").read_text(encoding="utf-8", errors="replace") if (LAPY / "build-lapy-daemon.sh").exists() else ""
expect("--elf-helper --title PPSA83600" in build, "the build makes the owned one-shot helper for PPSA83600")
expect('"$out/lapy.elf"' in build, "the build outputs lapy.elf to the artifact root (/app0)")
expect('rev-parse HEAD' in build and 'mismatch' in build, "the build verifies the pinned commit")

# stage copies lapy.elf to /app0
stage = read("tools/stage-reloaded-candidate.py")
expect("lapy.elf" in stage and "app / 'lapy.elf'" in stage,
       "stage-reloaded-candidate.py bundles lapy.elf at /app0")

# docs
credits = read("docs/CREDITS.md")
expect("PS5-Lapy-JB-Daemon" in credits and "LGPL-2.1" in credits,
       "docs/CREDITS.md names the helper as LGPL-2.1")

if failures:
    for f in failures:
        print("FAIL:", f)
    print(f"{len(failures)} check(s) failed")
    sys.exit(1)
print("lapy daemon package: all checks passed")
