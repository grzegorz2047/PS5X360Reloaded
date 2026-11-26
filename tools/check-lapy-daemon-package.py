#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Fix 186 (issue #80): verify the bundled Lapy elevation helper is packaged with full LGPL-2.1
# compliance and the agreed safeguards, WITHOUT building it (no prospero-clang needed). Checks:
#  - manifest.json pins an exact upstream commit + repo URL, and is labelled LGPL-2.1-or-later (NOT
#    MIT), user-loaded, not linked into eboot, not auto-launched;
#  - licenses/LAPY-LGPL-2.1.txt is the LGPL-2.1 text;
#  - NOTICE.md states LGPL (not MIT), the pinned commit, relink/corresponding-source steps, and the
#    experimental / user-risk note;
#  - the ps5log.h stub is our own MIT no-op;
#  - build-lapy-daemon.sh builds the resident service title-agnostic (--service --title '*'), verifies
#    the pinned commit, and ships the licence + manifest + notice beside the ELF;
#  - the CI step is wired and non-fatal; docs/CREDITS.md names it as LGPL-2.1.
# Run from anywhere: paths are resolved from this file's location.
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
LAPY = REPO / "tools/source-1.0.2/lapy-daemon"
PIN_RE = re.compile(r"^[0-9a-f]{40}$")
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
    manifest = json.loads((LAPY / "manifest.json").read_text(encoding="utf-8"))
except Exception as exc:  # noqa: BLE001
    manifest = {}
    failures.append(f"manifest.json unreadable: {exc}")
up = manifest.get("upstream", {})
lic = manifest.get("license", {})
dep = manifest.get("deployment", {})
expect(PIN_RE.match(up.get("commit", "")) is not None, "manifest pins a 40-hex upstream commit")
expect("mpereiraesaa/PS5-Lapy-JB-Daemon" in up.get("repo", ""), "manifest records the upstream repo URL")
expect(lic.get("spdx") == "LGPL-2.1-or-later", "manifest license spdx is LGPL-2.1-or-later")
expect("NOT MIT" in lic.get("note", "").upper().replace("-", " ") or "not mit" in lic.get("note", "").lower(),
       "manifest says NOT MIT")
expect("LAPY-LGPL-2.1.txt" in lic.get("license_text", ""), "manifest points at the LGPL text")
model = dep.get("model", "").lower()
expect("user" in model and "loaded" in model, "manifest marks the helper user-loaded")
expect(("not" in model or "never" in model) and ("auto" in model or "launch" in model or "loader" in model),
       "manifest says not auto-launched / not sent to a loader")
expect("experimental" in dep.get("status", "").lower() and "12.02" in dep.get("status", ""),
       "manifest marks it experimental (FW 12.02 validated)")
PIN = up.get("commit", "")

# LGPL text
lgpl = read("licenses/LAPY-LGPL-2.1.txt")
expect("GNU LESSER GENERAL PUBLIC LICENSE" in lgpl and "Version 2.1" in lgpl,
       "licenses/LAPY-LGPL-2.1.txt is the LGPL-2.1 text")

# NOTICE.md
notice = (LAPY / "NOTICE.md").read_text(encoding="utf-8", errors="replace") if (LAPY / "NOTICE.md").exists() else ""
notice_plain = notice.replace("*", "").replace("`", "")
expect("LGPL-2.1-or-later" in notice and "not MIT" in notice.lower() or "NOT MIT" in notice,
       "NOTICE states LGPL-2.1, not MIT")
expect(PIN and PIN in notice, "NOTICE records the pinned commit")
expect("build_owned_daemon.py" in notice and "--service" in notice,
       "NOTICE documents the relink / corresponding-source build")
expect("experimental" in notice.lower() and "risk" in notice.lower(),
       "NOTICE carries the experimental / user-risk note")
expect("not linked into" in notice_plain.lower() and "eboot" in notice_plain.lower(),
       "NOTICE says the helper is not linked into eboot")

# ps5log stub (ours, MIT, no-op)
stub = (LAPY / "ps5log-stub/ps5log.h").read_text(encoding="utf-8", errors="replace") if (LAPY / "ps5log-stub/ps5log.h").exists() else ""
expect("SPDX-License-Identifier: MIT" in stub, "the ps5log stub is MIT (ours)")
expect("ps5log_init_default" in stub and "ps5log_printf" in stub and "ps5log_close" in stub,
       "the ps5log stub declares the symbols the daemon uses")
expect("PS5LOG_IMPLEMENTATION" in stub, "the ps5log stub provides the single-header implementation")

# build script
build = (LAPY / "build-lapy-daemon.sh").read_text(encoding="utf-8", errors="replace") if (LAPY / "build-lapy-daemon.sh").exists() else ""
expect("--service --title '*'" in build, "the build uses the resident, title-agnostic service mode")
expect('rev-parse HEAD' in build and 'mismatch' in build, "the build verifies the pinned commit")
expect("LAPY-LGPL-2.1.txt" in build and "manifest.json" in build and "NOTICE.md" in build,
       "the build ships the licence, manifest and notice beside the ELF")
expect("x7fELF" in build or "\\x7fELF" in build, "the build verifies the output is an ELF")

# CI wiring: non-fatal step
ci = read(".github/workflows/build-source-1.0.2.yml")
step = ci[ci.find("Build the Lapy elevation helper"):] if "Build the Lapy elevation helper" in ci else ""
expect(step != "", "the CI build step for the helper exists")
expect("continue-on-error: true" in step[:400], "the helper CI step is non-fatal (continue-on-error)")
expect("build-lapy-daemon.sh" in step[:600], "the CI step runs build-lapy-daemon.sh")

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
