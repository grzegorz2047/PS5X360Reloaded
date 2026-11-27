#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# TrinityX360 fix 186i (issue #80): patch the upstream Lapy owned-root daemon
# (PS5-Lapy-JB-Daemon source/owned_root_daemon.c, which is MIT) so its cooperative
# root APPLY accepts OUR OWN multi-threaded target process, while still refusing a
# credential shared with ANOTHER process.
#
# Why this is safe (see docs/CREDITS.md / the fix-186 notes):
#   - The daemon elevates by editing the target's EXISTING ucred IN PLACE
#     (kernel_set_ucred_uid/... on the process credential), not by swapping the
#     pointer. So every thread whose credential slot points at that one ucred
#     becomes root at once.
#   - A ucred referenced exactly (count + 1) times == the process reference plus
#     one per thread => EVERY thread shares it and NO other process does. If
#     another process shared it, cred_refs would exceed count + 1 and we refuse.
#   - All target threads are ptrace-stopped during the apply, so nothing races.
#
# The original gate `count == 1 && cred_refs == 2` is the count+1 rule for a
# single thread (1 + 1 == 2), so single-threaded behaviour is byte-for-byte
# unchanged; this only additionally admits a self-owned multi-threaded target.
#
# Two in-line edits, whitespace-tolerant, each asserted to match exactly once:
#   1) private_process_credential(): the count/refs condition (used by the
#      pre-stop preflight AND the post-stop gate).
#   2) await_target_stop(): its own `first.cred_refs == 2` guard.
import re
import sys

path = sys.argv[1]
src = open(path, encoding="utf-8").read()

edits = [
    # private_process_credential: count==1 && cred_refs==2  ->  cred_refs == count+1
    (r"state->count\s*==\s*1\s*&&\s*state->cred_refs\s*==\s*2",
     "state->cred_refs == state->count + 1"),
    # await_target_stop: first.cred_refs == 2  ->  first.cred_refs == first.count + 1
    (r"first\.cred_refs\s*==\s*2",
     "first.cred_refs == first.count + 1"),
]

for pattern, replacement in edits:
    src, n = re.subn(pattern, replacement, src)
    if n != 1:
        sys.stderr.write(
            "relax-single-thread: FAIL anchor /%s/ matched %d times (expected 1)\n"
            % (pattern, n))
        sys.exit(1)

open(path, "w", encoding="utf-8", newline="").write(src)
print("relax-single-thread: applied 2 edits (self-owned multi-threaded target allowed)")
