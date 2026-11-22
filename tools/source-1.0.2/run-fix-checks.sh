#!/usr/bin/env bash
# Host checks of the fixes in tools/source-1.0.2/fixes, against an assembled tree:
#   bash tools/source-1.0.2/run-fix-checks.sh <tree>
# Each tools/check-*.cpp listed below is copied into <tree>/tools and built from the tree's root
# with the host's clang++-18 and the same flags as tools/source-checks/run.py (Zen 2, the
# tree's include/ and reconstructed/ and the core's include roots), then run with its arguments.
# check-wait-wakeups.cpp (fix 31) is built four ways (see below).
# Then tools/check-assemble-skip.py (the switch that leaves fixes out) and tools/check-english-source.py
# (fix 25, English keys) and tools/check-kinect-fake-relay.py (fix 44's test payload, built with the
# host's C compiler) run with python3, and tools/check-gdfx-names.sh (fix 26: GDFX names and
# guest paths, linked with the tree's xenia/base and disc image device) with bash.
# tools/check-kinect-page.mjs (fix 56: the phone camera page and its pairing state) runs with node.
# check-game-info.cpp reads a committed excerpt of the official package's assets/gameinfo.txt, and
# the whole file too when PS5X360_GAMEINFO names it (fix 43). check-guest-crash.cpp (fix 45) runs
# threads that time XmaDecoder's pause handshake (100 ms as a rule and 1 s at most with the fix; 1.0.2's
# still waiting at 300 ms) and suspends one of its own threads with SIGUSR1 while it holds a lock.
# Fix 46 (PS5X360Reloaded's own title ID, folder and name): check-app-identity.cpp (the identity
# header, the version label, the web settings' keys), check-official-import.cpp (the first start's
# copy from the official PS5X360 on a temporary folder) and tools/check-title-id.py (no official
# title ID or /data/homebrew literal left in the title and the core, with python3).
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
tree=$(realpath "$1")
cxx=${CXX_HOST:-clang++-18}
python=${PYTHON_HOST:-python3}
checks=(
  check-stale-dispatch-header.cpp  # fix 20
  check-forward.cpp                # fix 21
  check-guide-network.cpp          # fix 22
  check-english-fallback.cpp       # fixes 23 and 25
  check-stall-dump.cpp             # fix 24
  check-label-owner.cpp            # fix 24, candidate 21
  check-xtl-locale.cpp             # fix 26
  check-system-ui.cpp              # fix 26
  check-fix26-tree.cpp             # fix 26
  check-save-devices.cpp           # fix 27
  check-notify-first-poll.cpp      # fix 28
  check-net-conf.cpp               # fix 29
  check-memexport-merge.cpp        # fix 30
  check-media-voice.cpp            # fix 32
  check-switch-title.cpp           # fix 33
  check-pipeline-claim.cpp         # fix 34
  check-readback-slots.cpp         # fix 35
  check-unwatched-policy.cpp       # fix 36
  check-exact-unwatch.cpp          # fix 37
  check-jit-backports.cpp          # fix 38
  check-net-log-drain.cpp          # fix 39
  check-home-hints.cpp             # fix 40 (reconstructed files)
  check-cover-fallback.cpp         # fix 41
  check-network-self-test.cpp      # fix 42
  check-ui-lang-overlay.cpp        # fix 43
  check-game-info.cpp              # fix 43
  check-kinect-usb.cpp             # fix 44
  check-review-fix30.cpp           # fixes 35-37 with fix 30
  check-guest-crash.cpp            # fix 45
  check-app-identity.cpp           # fix 46
  check-official-import.cpp        # fix 46
  check-hang-watch.cpp             # fix 47
  check-memexport-loss.cpp         # fix 48
  check-frame-cost.cpp             # fix 49
  check-save-folder.cpp            # fix 50
  check-multidisc-source.cpp       # fix 51
  check-library-disc.cpp           # fix 52
  check-measure-10s.cpp            # fix 54
  check-global-lock-spin.cpp       # fix 55
  check-kinect-pairing.cpp         # fix 56
  check-nui-image-scan.cpp         # fix 57
  check-motion-replay.cpp          # fix 57 (1.0.5)
  check-gpu-wait-lines.cpp         # fix 58
  check-fast-readback.cpp          # fix 59
  check-library-folders.cpp        # fix 60
  check-fault-stack.cpp            # fix 61
  check-upload-cost.cpp            # fix 62
  check-phone-qr-panel.cpp         # fix 63
  check-native-mutant.cpp          # fix 64
  check-fault-path.cpp             # fix 65
  check-phone-latency.cpp          # fix 66
  check-xma-unwatch.cpp            # fix 67
  check-suspend-trace.cpp          # fix 69
  check-pending-resume.cpp         # fix 71
  check-page-version.cpp           # fix 72
  check-draw-cost.cpp              # fix 73
  check-watch-arming.cpp           # fix 74
  check-packet-cost.cpp            # fix 75
  check-alloc-failure-lines.cpp    # fix 76
  check-wait-reg.cpp               # fix 77
  check-hot-memory.cpp             # fix 78
  check-system-heap.cpp            # fix 79
  check-enumerator-release.cpp     # fix 80
  check-enum-trace.cpp             # fix 81
  check-enumerator-buffers.cpp     # fix 82
  check-wait-reg-wakeup.cpp        # fix 83
  check-arming-one-lock.cpp        # fix 84
  check-default-app-relaunch.cpp   # fix 85
  check-draw-lock-deferred.cpp     # fix 86
  check-unwatched-chunk.cpp        # fix 87
  check-global-lock-callers.cpp    # fix 88
  check-multidisc-library.cpp      # fix 89
  check-xctd-query.cpp             # fix 90 (1.0.5)
  check-readback-repeat.cpp        # fix 91
  check-readback-keys.cpp          # fix 91 (diagnostic)
  check-bug-report.cpp             # fix 93
  check-leaving-notice.cpp         # fix 93b
  check-boot-notice.cpp            # fix 93c
  check-bug-report-entries.cpp     # fix 93d
  check-recent-calls.cpp           # fix 93e
  check-report-session.cpp         # fix 93f
  check-flush-before-restart.cpp   # fix 93g
  check-leaving-steps.cpp          # fix 93h
  check-bounded-leave.cpp          # fix 93i
  check-boost-presets.cpp          # fix 95
  check-event-writes.cpp           # fix 96
  check-timer-apc.cpp              # fix 97
  check-dark-default.cpp           # fix 98
  check-avatar-assets.cpp          # fix 99 (1.0.5)
  check-kinect-identity.cpp        # fix 99b (1.0.5)
  check-content-folders.cpp        # fix 101
  check-index-unwatched.cpp        # fix 102
  check-register-writes.cpp        # fix 103
  check-texture-set-reuse.cpp      # fix 104
  check-fetch-unchanged.cpp        # fix 105
  check-kinect-tracker.cpp         # fix 106
  check-kinect-emergency.cpp       # fix 107
  check-kinect-diag.cpp            # fix 108
  check-dlc-remove.cpp             # fix 109
  check-extent-count.cpp           # fix 110
  check-unwatched-lock-free.cpp    # fix 111
  check-osk-keyboard.cpp           # fix 114
  check-osk-modes.cpp              # fix 114b
  check-guide-quick.cpp            # fix 115
  check-guide-header.cpp           # fix 115b
  check-guide-tabs.cpp             # fix 115c
  check-guide-counter.cpp          # fix 115d
  check-guide-measure.cpp          # fix 115e
  check-swap-pacing.cpp            # fix 116
  check-nui-hud-check.cpp          # fix 118
  check-kinect-camera-picture.cpp  # fix 119
  check-dynamic-constants.cpp      # fix 120
  check-file-sector.cpp            # fix 121
  check-content-checked.cpp        # fix 125
  check-sample-ticket.cpp          # fix 126
  check-vfs-host-ops.cpp           # fix 129
  check-game-language.cpp          # fix 132
)
declare -A arguments=(
  [check-english-fallback.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-save-devices.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-cover-fallback.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-ui-lang-overlay.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-game-info.cpp]="$repo/tools/source-1.0.2/fixtures/gameinfo-excerpt.txt"
  [check-guest-crash.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-phone-qr-panel.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-measure-10s.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-library-disc.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-kinect-emergency.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-multidisc-library.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-bug-report.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-leaving-notice.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-bug-report-entries.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-report-session.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-leaving-steps.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-guide-quick.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-guide-measure.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-osk-keyboard.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-dlc-remove.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-kinect-camera-picture.cpp]="$repo/tools/source-1.0.2/english/lang"
)
# The fix each of these checks belongs to: when a test build leaves that fix out
# (assemble.py --skip-fix, SOURCES.json fixes_skipped) its check is not run.
declare -A fix_of=(
  [check-media-voice.cpp]=32 [check-switch-title.cpp]=33 [check-pipeline-claim.cpp]=34
  [check-readback-slots.cpp]=35 [check-unwatched-policy.cpp]=36 [check-exact-unwatch.cpp]=37
  [check-jit-backports.cpp]=38 [check-net-log-drain.cpp]=39 [check-review-fix30.cpp]="35 36 37"
  [check-app-identity.cpp]=46 [check-official-import.cpp]=46
  [check-hang-watch.cpp]=47
  [check-memexport-loss.cpp]=48
  [check-frame-cost.cpp]=49
  [check-save-folder.cpp]=50
  [check-multidisc-source.cpp]=51
  [check-library-disc.cpp]=52
  [check-measure-10s.cpp]=54
  [check-global-lock-spin.cpp]=55
  [check-kinect-pairing.cpp]=56
  [check-nui-image-scan.cpp]=57
  [check-motion-replay.cpp]=57
  [check-gpu-wait-lines.cpp]=58
  [check-fast-readback.cpp]=59
  [check-library-folders.cpp]=60
  [check-fault-stack.cpp]=61
  [check-upload-cost.cpp]=62
  [check-phone-qr-panel.cpp]=63
  [check-native-mutant.cpp]=64
  [check-fault-path.cpp]=65
  [check-phone-latency.cpp]=66
  [check-xma-unwatch.cpp]=67
  [check-suspend-trace.cpp]=69
  [check-pending-resume.cpp]=71
  [check-page-version.cpp]=72
  [check-draw-cost.cpp]=73
  [check-watch-arming.cpp]=74
  [check-packet-cost.cpp]=75
  [check-alloc-failure-lines.cpp]=76
  [check-wait-reg.cpp]=77
  [check-hot-memory.cpp]=78
  [check-system-heap.cpp]=79
  [check-enumerator-release.cpp]=80
  [check-enum-trace.cpp]=81
  [check-enumerator-buffers.cpp]=82
  [check-wait-reg-wakeup.cpp]=83
  [check-arming-one-lock.cpp]=84
  [check-default-app-relaunch.cpp]=85
  [check-draw-lock-deferred.cpp]=86
  [check-unwatched-chunk.cpp]=87
  [check-global-lock-callers.cpp]="88 128"
  [check-multidisc-library.cpp]=89
  [check-xctd-query.cpp]="90 90b"
  [check-readback-repeat.cpp]=91
  [check-readback-keys.cpp]=91
  [check-bug-report.cpp]=93
  [check-leaving-notice.cpp]=93b
  [check-boot-notice.cpp]=93c
  [check-bug-report-entries.cpp]=93d
  [check-recent-calls.cpp]=93e
  [check-report-session.cpp]=93f
  [check-flush-before-restart.cpp]=93g
  [check-leaving-steps.cpp]=93h
  [check-bounded-leave.cpp]=93i
  [check-boost-presets.cpp]="95 122 123"
  [check-event-writes.cpp]=96
  [check-timer-apc.cpp]=97
  [check-dark-default.cpp]=98
  [check-avatar-assets.cpp]=99
  [check-kinect-identity.cpp]=99b
  [check-content-folders.cpp]=101
  [check-index-unwatched.cpp]=102
  [check-register-writes.cpp]=103
  [check-texture-set-reuse.cpp]=104
  [check-fetch-unchanged.cpp]=105
  [check-kinect-tracker.cpp]=106
  [check-kinect-emergency.cpp]=107
  [check-kinect-diag.cpp]=108
  [check-extent-count.cpp]=110
  [check-unwatched-lock-free.cpp]=111
  [check-osk-keyboard.cpp]=114
  [check-guide-quick.cpp]=115
  [check-swap-pacing.cpp]=116
  [check-nui-hud-check.cpp]=118
  [check-osk-modes.cpp]="114 114b"
  [check-dlc-remove.cpp]=109
  [check-guide-header.cpp]=115b
  [check-guide-tabs.cpp]=115c
  [check-guide-counter.cpp]=115d
  [check-guide-measure.cpp]=115e
  [check-kinect-camera-picture.cpp]=119
  [check-dynamic-constants.cpp]=120
  [check-file-sector.cpp]=121
  [check-content-checked.cpp]=125
  [check-sample-ticket.cpp]=126
  [check-vfs-host-ops.cpp]=129
  [check-game-language.cpp]=132
)
skipped_fixes=$("$python" -I -c 'import json, sys, os
report = json.load(open(sys.argv[1], encoding="utf-8"))
print(" ".join(os.path.basename(p).split("-", 1)[0] for p in report.get("fixes_skipped", [])))' "$tree/SOURCES.json")
mkdir -p "$tree/tools" "$tree/build/fix-checks"
status=0
for name in "${checks[@]}"; do
  left_out=
  for fix in ${fix_of[$name]:-}; do
    if [[ " $skipped_fixes " == *" $fix "* ]]; then left_out=$fix; fi
  done
  if [[ -n $left_out ]]; then
    echo "=== $name"
    echo "skip $name (fix $left_out left out of this build)"
    continue
  fi
  cp "$repo/tools/$name" "$tree/tools/$name"
  exe="$tree/build/fix-checks/${name%.cpp}"
  args=()
  if [[ -n "${arguments[$name]:-}" ]]; then args=("${arguments[$name]}"); fi
  echo "=== $name"
  if (cd "$tree" && "$cxx" -std=c++20 -O1 -g -pthread -march=znver2 -Wall -Wno-unused-function \
        -Iinclude -Ireconstructed -I.deps/xenia-canary/src -I.deps/xenia-canary \
        "tools/$name" -o "$exe" && "$exe" "${args[@]}"); then
    echo "pass $name"
  else
    echo "FAIL $name"
    status=1
  fi
done
echo "=== check-patch-syntax.py"
if "$python" -I "$repo/tools/check-patch-syntax.py"; then
  echo "pass check-patch-syntax.py"
else
  echo "FAIL check-patch-syntax.py"
  status=1
fi
echo "=== check-bf3-preset.py"
if "$python" -I "$repo/tools/check-bf3-preset.py" --tree "$tree"; then
  echo "pass check-bf3-preset.py"
else
  echo "FAIL check-bf3-preset.py"
  status=1
fi
# Fix 31 (Minecraft at ~1 FPS): the core's wait objects and critical sections, built
# four ways: fix 31 and 1.0.2's wake-ups (XBOX360PS5_LEGACY_WAKES) with the multi-object
# poll bound raised to 60 s (a lost wake-up then hangs and the check's watchdog fails it),
# fix 31 under ThreadSanitizer, and with the real 4 ms bound under Address- and
# UndefinedBehaviorSanitizer. Sanitized runs go without address randomization (setarch -R):
# TSan in clang 18 does not start under the runners' 32-bit mmap randomization.
cp "$repo/tools/check-wait-wakeups.cpp" "$tree/tools/check-wait-wakeups.cpp"
wait_variants=(
  "fix31|-O1 -DXBOX360PS5_MULTI_WAIT_BOUND_MS=60000"
  "legacy|-O1 -DXBOX360PS5_LEGACY_WAKES -DXBOX360PS5_MULTI_WAIT_BOUND_MS=60000"
  "tsan|-O1 -fsanitize=thread -DXBOX360PS5_MULTI_WAIT_BOUND_MS=60000"
  "asan|-O1 -fsanitize=address,undefined -fno-sanitize-recover=all"
)
for variant in "${wait_variants[@]}"; do
  label=${variant%%|*}
  read -r -a flags <<< "${variant#*|}"
  exe="$tree/build/fix-checks/check-wait-wakeups-$label"
  run=("$exe")
  if [[ $label == tsan || $label == asan ]]; then run=(setarch "$(uname -m)" -R "$exe"); fi
  echo "=== check-wait-wakeups.cpp ($label)"
  if (cd "$tree" && "$cxx" -std=c++20 -g -pthread -march=znver2 -Wall -Wno-unused-function \
        "${flags[@]}" -Iinclude -Ireconstructed -I.deps/xenia-canary/src -I.deps/xenia-canary \
        tools/check-wait-wakeups.cpp -o "$exe" && "${run[@]}"); then
    echo "pass check-wait-wakeups.cpp ($label)"
  else
    echo "FAIL check-wait-wakeups.cpp ($label)"
    status=1
  fi
done
echo "=== check-assemble-skip.py"
if "$python" -I "$repo/tools/check-assemble-skip.py" --tree "$tree"; then
  echo "pass check-assemble-skip.py"
else
  echo "FAIL check-assemble-skip.py"
  status=1
fi
echo "=== check-english-source.py"
if "$python" -I "$repo/tools/check-english-source.py" --tree "$tree"; then
  echo "pass check-english-source.py"
else
  echo "FAIL check-english-source.py"
  status=1
fi
echo "=== check-title-id.py"
if [[ " $skipped_fixes " == *" 46 "* ]]; then
  echo "skip check-title-id.py (fix 46 left out of this build)"
elif "$python" -I "$repo/tools/check-title-id.py" --tree "$tree"; then
  echo "pass check-title-id.py"
else
  echo "FAIL check-title-id.py"
  status=1
fi
echo "=== check-kinect-fake-relay.py"
if "$python" -I "$repo/tools/check-kinect-fake-relay.py"; then
  echo "pass check-kinect-fake-relay.py"
else
  echo "FAIL check-kinect-fake-relay.py"
  status=1
fi
# Fix 56: the phone camera page and its pairing state (port/phone_camera_page.hpp), with node.
echo "=== check-kinect-page.mjs"
if [[ " $skipped_fixes " == *" 56 "* ]]; then
  echo "skip check-kinect-page.mjs (fix 56 left out of this build)"
elif node "$repo/tools/check-kinect-page.mjs" "$tree"; then
  echo "pass check-kinect-page.mjs"
else
  echo "FAIL check-kinect-page.mjs"
  status=1
fi
# Fix 66: the phone page's delay by stage (latency.mjs in port/phone_camera_page.hpp), with node.
echo "=== check-phone-latency.mjs"
if [[ " $skipped_fixes " == *" 66 "* ]]; then
  echo "skip check-phone-latency.mjs (fix 66 left out of this build)"
elif node "$repo/tools/check-phone-latency.mjs" "$tree"; then
  echo "pass check-phone-latency.mjs"
else
  echo "FAIL check-phone-latency.mjs"
  status=1
fi
# Fix 72: the phone page's version and its one reload, with node.
echo "=== check-page-version.mjs"
if [[ " $skipped_fixes " == *" 72 "* ]]; then
  echo "skip check-page-version.mjs (fix 72 left out of this build)"
elif node "$repo/tools/check-page-version.mjs" "$tree"; then
  echo "pass check-page-version.mjs"
else
  echo "FAIL check-page-version.mjs"
  status=1
fi
# Fix 106: the phone's Kinect tracker (port/phone_camera_tracker.hpp) and the page's reasons, with node.
echo "=== check-kinect-tracker.mjs"
if [[ " $skipped_fixes " == *" 106 "* ]]; then
  echo "skip check-kinect-tracker.mjs (fix 106 left out of this build)"
elif node "$repo/tools/check-kinect-tracker.mjs" "$tree"; then
  echo "pass check-kinect-tracker.mjs"
else
  echo "FAIL check-kinect-tracker.mjs"
  status=1
fi
# Fix 107: re-centring the phone camera from the pad (the page's side), with node.
echo "=== check-kinect-recentre.mjs"
if [[ " $skipped_fixes " == *" 107 "* ]]; then
  echo "skip check-kinect-recentre.mjs (fix 107 left out of this build)"
elif node "$repo/tools/check-kinect-recentre.mjs" "$tree"; then
  echo "pass check-kinect-recentre.mjs"
else
  echo "FAIL check-kinect-recentre.mjs"
  status=1
fi
# Fix 119: the phone page's camera picture for the game, with node.
echo "=== check-kinect-camera-picture.mjs"
if [[ " $skipped_fixes " == *" 119 "* ]]; then
  echo "skip check-kinect-camera-picture.mjs (fix 119 left out of this build)"
elif node "$repo/tools/check-kinect-camera-picture.mjs" "$tree"; then
  echo "pass check-kinect-camera-picture.mjs"
else
  echo "FAIL check-kinect-camera-picture.mjs"
  status=1
fi
echo "=== check-gdfx-names.sh"
if bash "$repo/tools/check-gdfx-names.sh" "$tree"; then
  echo "pass check-gdfx-names.sh"
else
  echo "FAIL check-gdfx-names.sh"
  status=1
fi
# Fix 101: a file created through a package folder mounted at \Device\... (left out with the fix).
echo "=== check-content-create.sh"
if ! grep -q 'fix 101' "$tree/.deps/xenia-canary/src/xenia/vfs/virtual_file_system.cc"; then
  echo "skip check-content-create.sh (fix 101 left out)"
elif bash "$repo/tools/check-content-create.sh" "$tree"; then
  echo "pass check-content-create.sh"
else
  echo "FAIL check-content-create.sh"
  status=1
fi
exit $status
