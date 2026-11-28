#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# TrinityX360 fix 186i/186j (issue #80): patch the upstream Lapy owned-root daemon
# (PS5-Lapy-JB-Daemon source/owned_root_daemon.c, which is MIT) with two adaptations:
#   186i: the cooperative root APPLY accepts OUR OWN multi-threaded target process,
#         while still refusing a credential shared with ANOTHER process.
#   186j: CREDENTIAL-ONLY elevation -- set UID 0 but SKIP the sandbox escape (the
#         root/jail donor transaction). The PS5 write throttle is UID-gated (UID 1
#         throttled, UID 0 not), so UID 0 alone lifts it; keeping the process's
#         root/jail refs leaves its filesystem view (its /app0 mount, app skins and
#         game data) intact, which the full escape otherwise broke (default theme).
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
    # 186j: after the credential set succeeds, skip the donor/root-move block and jump to the
    # detach+resume path (done:). cred_changed is 1 here, so this always skips the escape; the guard
    # keeps the (now dead) donor code reachable to the compiler. done: cleanly detaches the stopped
    # target and resumes it as UID 0; first.pid/second.pid stay -1 so cleanup_child is a no-op, and
    # run_one returns 0 -> RESPONSE status OK. moved=0 -> request_result logs roots_moved=0.
    (r'stage = "first_donor";',
     '/* fix 186j: credential-only -- UID 0 lifts the UID-gated write throttle without the sandbox\n'
     '       escape, so the process keeps its filesystem view (/app0 mount, skins, game data). Skip\n'
     '       the donor/root-move; detach and resume as UID 0 through done:. */\n'
     '    if (cred_changed) { stopped_target = 1; stage = "complete"; goto done; }\n'
     '    stage = "first_donor";'),
]

for pattern, replacement in edits:
    src, n = re.subn(pattern, replacement, src)
    if n != 1:
        sys.stderr.write(
            "relax-single-thread: FAIL anchor /%s/ matched %d times (expected 1)\n"
            % (pattern, n))
        sys.exit(1)

open(path, "w", encoding="utf-8", newline="").write(src)
print("relax-single-thread: applied %d edits (multi-threaded target + credential-only, no sandbox escape)"
      % len(edits))
