#!/usr/bin/env bash
# Host checks of the fixes in tools/source-1.0.2/fixes, against an assembled tree:
#   bash tools/source-1.0.2/run-fix-checks.sh <tree>
# Each tools/check-*.cpp listed below is copied into <tree>/tools and built from the tree's root
# with the host's clang++-18 and the same flags as tools/source-checks/run.py (Zen 2, the
# tree's include/ and reconstructed/ and the core's include roots), then run with its arguments.
# check-wait-wakeups.cpp (fix 31) is built four ways (see below).
# Then tools/check-assemble-skip.py (the switch that leaves fixes out) and tools/check-english-source.py
# (fix 25, English keys), tools/check-game-patch-files.py (the shipped assets/patches files) and tools/check-kinect-fake-relay.py (fix 44's test payload, built with the
# host's C compiler) run with python3, and tools/check-gdfx-names.sh (fix 26: GDFX names and
# guest paths, linked with the tree's xenia/base and disc image device) with bash.
# tools/check-kinect-page.mjs (fix 56: the phone camera page and its pairing state) runs with node,
# and so does tools/check-phone-pose-steady.mjs (fix 169: its pose frames and its link).
# check-game-info.cpp reads a committed excerpt of the official package's assets/gameinfo.txt, and
# the whole file too when PS5X360_GAMEINFO names it (fix 43). check-guest-crash.cpp (fix 45) runs
# threads that time XmaDecoder's pause handshake (100 ms as a rule and 1 s at most with the fix; 1.0.2's
# still waiting at 300 ms) and suspends one of its own threads with SIGUSR1 while it holds a lock.
# Fix 46 (PS5X360Reloaded's own title ID, folder and name): check-app-identity.cpp (the identity
# header, the version label, the web settings' keys), check-official-import.cpp (the first start's
# copy from the official PS5X360 on a temporary folder) and tools/check-title-id.py (no official
# title ID or /data/homebrew literal left in the title and the core, with python3). The rename to
# TrinityX360 (fixes/199-trinityx360-name.patch, 1.0.6; "199" when left out) is checked by
# check-app-identity.cpp, check-save-folder.cpp (the drives' folder keeps its name),
# check-guide-header.cpp and check-title-id.py.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
tree=$(realpath "$1")
cxx=${CXX_HOST:-clang++-18}
python=${PYTHON_HOST:-python3}
jobs=${JOBS:-$(nproc)}
launcher=()
if command -v ccache > /dev/null && [[ ${PS5X360_NO_CCACHE:-0} != 1 ]]; then
  launcher=(ccache)
  export CCACHE_SLOPPINESS="${CCACHE_SLOPPINESS:-time_macros}"
fi
ldflag=()
probe_bin=$(mktemp)
if "$cxx" -fuse-ld=lld -x c++ - -o "$probe_bin" <<< 'int main() { return 0; }' 2>/dev/null; then
  ldflag=(-fuse-ld=lld)
fi
rm -f "$probe_bin"
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
  check-avatar-skeleton.cpp        # fix 113
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
  check-submit-parts.cpp           # fix 130
  check-game-language.cpp          # fix 132
  check-readback-batching.cpp      # fix 133
  check-async-submit.cpp           # fix 134
  check-resolution-scale.cpp       # fixes 117 to 117k
  check-library-groups.cpp         # fixes 112 to 112f
  check-guide-per-game.cpp         # fix 143
  check-vulkan-fxaa.cpp            # fixes 137 and 137b
  check-swap-effects.cpp           # fixes 141 and 141b
  check-swap-smaa.cpp              # fix 145
  check-low-half-size.cpp          # fix 144
  check-video-mode.cpp             # fixes 140 to 140c
  check-present-limit.cpp          # fixes 146 to 146e
  check-exception-loop-entry.cpp   # fix 147
  check-disc-content.cpp           # fix 148
  check-smartglass-app.cpp         # fix 147b
  check-title-keys.cpp             # fix 147b
  check-disc-name-tags.cpp         # fix 148b
  check-package-io.cpp             # fix 148c
  check-file-io.cpp                # fix 148d
  check-harmonix-keys.cpp          # fix 147c
  check-option-groups.cpp          # fix 149
  check-settings-tree.cpp          # fix 149b
  check-readability.cpp            # fix 149c
  check-stfs-cut-short.cpp         # fix 148e
  check-dlc-incomplete.cpp         # fix 148f
  check-write-trace.cpp            # fix 148g
  check-install-notice.cpp         # fix 148h
  check-help-card.cpp              # fix 149d
  check-ui-loop-io.cpp             # fix 152
  check-hang-watch-sync.cpp        # fix 153
  check-log-ring.cpp               # fix 154
  check-utility-cache-drive.cpp    # fixes 150c, 150d and 150f
  check-two-x-unfit.cpp            # fix 150g
  check-kinect-camera-classes.cpp  # fix 162a
  check-scale-pressure.cpp         # fixes 162c, 162h and 163
  check-kinect-title-rule.cpp      # fixes 162f and 163
  check-auto-scale.cpp             # fix 163
  check-write-bench.cpp            # fixes 148i, 148j, 148k
  check-xma-trace.cpp              # fixes 155, 155b
  check-xma-progress.cpp           # fix 155b
  check-gta5-audio-patch.cpp       # fix 155c
  check-single-game-folder.cpp     # fix 159
  check-nui-device-status.cpp      # fix 156
  check-kernel-timer.cpp           # fix 156b
  check-nui-elevation.cpp          # fix 156c
  check-pscam-state.cpp            # fix 156d
  check-camera-worker.cpp          # fix 156e
  check-mic-array.cpp              # fix 156f
  check-request-pages.cpp          # fix 156g
  check-page-watch.cpp             # fix 156h
  check-skeleton-frame-number.cpp  # fix 156i
  check-xex-dll-heap.cpp           # fix 156j
  check-diag-r31.cpp               # fixes 156k and 156m
  check-nui-identity.cpp           # fixes 156l, 156n, 156q, 156t, 156u and 156v
  check-diag-stuck-dump.cpp        # fixes 156o and 156r
  check-avatar-assets-success.cpp  # fix 156p
  check-player-choice.cpp          # fix 156s
  check-nui-identity-retrack.cpp   # fix 156w
  check-user-storage.cpp           # fix 165
  check-game-configs.cpp           # fix 166
  check-resolve-trace.cpp          # fix 167
  check-half-pixel-host.cpp        # fix 168
  check-gta4-core-config.cpp       # fix 168b (assets/config/545407F2.config.toml)
  check-phone-link.cpp             # fix 169
  check-nui-speech.cpp             # fix 170
  check-xbox-fitness.cpp           # fix 171
  check-cache-churn.cpp            # fix 172
  check-exit-cache-timing.cpp      # fix 173
  check-pipeline-cache-skip.cpp    # fixes 173b and 173c
  check-guest-object-close.cpp     # fix 174
  check-virtual-mic.cpp            # fix 178
  check-recent-list.cpp            # fix 181
  check-disc-swap-content.cpp      # fix 182
  check-camera-figure.cpp          # fix 185
  check-elevate-probe.cpp          # fix 186
  check-unlimited-write.cpp        # fix 186
  check-hp-voice-skip.cpp          # fix 187 (assets/patches)
  check-title-memory-stats.cpp     # fix 188
  check-focus-gate.cpp             # fix 190
  check-xna-usermode.cpp           # fix 191
  check-play-space-map.cpp         # fix 192
)
declare -A arguments=(
  [check-english-fallback.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-play-space-map.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-harmonix-keys.cpp]="$repo"
  [check-gta4-core-config.cpp]="$repo"
  [check-gta5-audio-patch.cpp]="$repo"
  [check-hp-voice-skip.cpp]="$repo"
  [check-dlc-incomplete.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-install-notice.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-two-x-unfit.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-auto-scale.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-save-devices.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-cover-fallback.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-ui-lang-overlay.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-game-info.cpp]="$repo/tools/source-1.0.2/fixtures/gameinfo-excerpt.txt"
  [check-resolution-scale.cpp]="$repo/assets/presets.txt"
  [check-guest-crash.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-phone-qr-panel.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-measure-10s.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-library-disc.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-kinect-emergency.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-multidisc-library.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-library-groups.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-bug-report.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-leaving-notice.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-bug-report-entries.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-report-session.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-leaving-steps.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-guide-quick.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-guide-measure.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-option-groups.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-settings-tree.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-readability.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-help-card.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-osk-keyboard.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-dlc-remove.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-kinect-camera-picture.cpp]="$repo/tools/source-1.0.2/english/lang"
  [check-avatar-skeleton.cpp]="$repo/tools/source-1.0.2/english/lang"
)
# The fix each of these checks belongs to: when a test build leaves that fix out
# (assemble.py --skip-fix, SOURCES.json fixes_skipped) its check is not run.
declare -A fix_of=(
  [check-media-voice.cpp]=32 [check-switch-title.cpp]=33 [check-pipeline-claim.cpp]=34
  [check-readback-slots.cpp]=35 [check-unwatched-policy.cpp]=36 [check-exact-unwatch.cpp]=37
  [check-jit-backports.cpp]=38 [check-net-log-drain.cpp]=39 [check-review-fix30.cpp]="35 36 37"
  [check-app-identity.cpp]="46 199" [check-official-import.cpp]=46
  [check-hang-watch.cpp]=47
  [check-memexport-loss.cpp]=48
  [check-frame-cost.cpp]=49
  [check-save-folder.cpp]="50 199"
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
  [check-pending-resume.cpp]="71 151"
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
  [check-bug-report.cpp]="93 199"
  [check-leaving-notice.cpp]=93b
  [check-boot-notice.cpp]=93c
  [check-bug-report-entries.cpp]=93d
  [check-recent-calls.cpp]=93e
  [check-report-session.cpp]=93f
  [check-flush-before-restart.cpp]=93g
  [check-leaving-steps.cpp]=93h
  [check-bounded-leave.cpp]=93i
  [check-boost-presets.cpp]="95 122 123 131 135"
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
  [check-guide-quick.cpp]="115 143"
  [check-swap-pacing.cpp]=116
  [check-nui-hud-check.cpp]=118
  [check-osk-modes.cpp]="114 114b"
  [check-dlc-remove.cpp]=109
  [check-avatar-skeleton.cpp]=113
  [check-guide-header.cpp]="115b 199"
  [check-guide-tabs.cpp]=115c
  [check-guide-counter.cpp]=115d
  [check-guide-measure.cpp]=115e
  [check-kinect-camera-picture.cpp]=119
  [check-dynamic-constants.cpp]=120
  [check-file-sector.cpp]=121
  [check-content-checked.cpp]=125
  [check-sample-ticket.cpp]=126
  [check-vfs-host-ops.cpp]=129
  [check-submit-parts.cpp]=130
  [check-game-language.cpp]=132
  [check-readback-batching.cpp]=133
  [check-resolution-scale.cpp]="117 117b 117c 117d 117e 117f 117g 117h 117i 117j 117k"
  [check-library-groups.cpp]="112 112b 112c 112d 112e 112f"
  [check-async-submit.cpp]="134 134b 134c 134e"
  [check-guide-per-game.cpp]=143
  [check-vulkan-fxaa.cpp]="137 137b"
  [check-swap-effects.cpp]="141 141b"
  [check-swap-smaa.cpp]=145
  [check-low-half-size.cpp]=144
  [check-video-mode.cpp]="140 140b 140c"
  [check-present-limit.cpp]="146 146b 146c 146d 146e"
  [check-exception-loop-entry.cpp]=147
  [check-disc-content.cpp]=148
  [check-smartglass-app.cpp]=147b
  [check-title-keys.cpp]=147b
  [check-disc-name-tags.cpp]=148b
  [check-package-io.cpp]=148c
  [check-file-io.cpp]=148d
  [check-harmonix-keys.cpp]=147c
  [check-option-groups.cpp]=149
  [check-settings-tree.cpp]=149b
  [check-readability.cpp]=149c
  [check-stfs-cut-short.cpp]=148e
  [check-dlc-incomplete.cpp]=148f
  [check-write-trace.cpp]=148g
  [check-install-notice.cpp]=148h
  [check-write-bench.cpp]="148i 148j 148k"
  [check-help-card.cpp]=149d
  [check-ui-loop-io.cpp]=152
  [check-hang-watch-sync.cpp]=153
  [check-log-ring.cpp]=154
  [check-utility-cache-drive.cpp]="150c 150d"
  [check-two-x-unfit.cpp]=150g
  [check-kinect-camera-classes.cpp]=162a
  [check-scale-pressure.cpp]="162c 163"
  [check-kinect-title-rule.cpp]="162f 163"
  [check-auto-scale.cpp]=163
  [check-xma-trace.cpp]="155 155b"
  [check-xma-progress.cpp]=155b
  [check-gta5-audio-patch.cpp]=155c
  [check-single-game-folder.cpp]=159
  [check-nui-device-status.cpp]=156
  [check-kernel-timer.cpp]=156b
  [check-nui-elevation.cpp]=156c
  [check-pscam-state.cpp]=156d
  [check-camera-worker.cpp]=156e
  [check-mic-array.cpp]=156f
  [check-request-pages.cpp]=156g
  [check-page-watch.cpp]=156h
  [check-skeleton-frame-number.cpp]=156i
  [check-xex-dll-heap.cpp]=156j
  [check-diag-r31.cpp]="156k 156m"
  [check-nui-identity.cpp]="156l 156n 156q 156t 156u 156v"
  [check-diag-stuck-dump.cpp]="156o 156r"
  [check-avatar-assets-success.cpp]=156p
  [check-player-choice.cpp]=156s
  [check-nui-identity-retrack.cpp]=156w
  [check-user-storage.cpp]=165
  [check-game-configs.cpp]=166
  [check-resolve-trace.cpp]=167
  [check-half-pixel-host.cpp]=168
  [check-gta4-core-config.cpp]=168
  [check-phone-link.cpp]=169
  [check-nui-speech.cpp]=170
  [check-xbox-fitness.cpp]=171
  [check-cache-churn.cpp]=172
  [check-exit-cache-timing.cpp]=173
  [check-pipeline-cache-skip.cpp]=173b
  [check-guest-object-close.cpp]=174
  [check-virtual-mic.cpp]=178
  [check-recent-list.cpp]=181
  [check-disc-swap-content.cpp]=182
  [check-camera-figure.cpp]=185
  [check-elevate-probe.cpp]=186
  [check-unlimited-write.cpp]=186
  [check-title-memory-stats.cpp]=188
  [check-focus-gate.cpp]=190
  [check-xna-usermode.cpp]=191
  [check-play-space-map.cpp]=192
)
skipped_fixes=$("$python" -I -c 'import json, sys, os
report = json.load(open(sys.argv[1], encoding="utf-8"))
print(" ".join(os.path.basename(p).split("-", 1)[0] for p in report.get("fixes_skipped", [])))' "$tree/SOURCES.json")
mkdir -p "$tree/tools" "$tree/build/fix-checks" "$tree/build/fix-checks/build-logs"
status=0

# Stage sources and compile/link all active C++ checks in parallel (-c for ccache + fast lld link),
# then run the compiled binaries in deterministic order without background compiler contention.
compile_script="$tree/build/fix-checks/compile-one.sh"
cat > "$compile_script" <<'COMPILE_EOF'
#!/usr/bin/env bash
set -euo pipefail
name=$1
exe="$FIX_CHECKS_TREE/build/fix-checks/${name%.cpp}"
log="$FIX_CHECKS_TREE/build/fix-checks/build-logs/${name%.cpp}.log"
rc_file="$FIX_CHECKS_TREE/build/fix-checks/build-logs/${name%.cpp}.rc"
read -r -a launcher_arr <<< "${FIX_CHECKS_LAUNCHER:-}"
read -r -a ldflag_arr <<< "${FIX_CHECKS_LDFLAG:-}"
if (cd "$FIX_CHECKS_TREE" &&
    ${launcher_arr[@]+"${launcher_arr[@]}"} "$FIX_CHECKS_CXX" -std=c++20 -O1 -g -pthread -march=znver2 \
      -Wall -Wno-unused-function -Iinclude -Ireconstructed \
      -I.deps/xenia-canary/src -I.deps/xenia-canary \
      -c "tools/$name" -o "$exe.o" &&
    "$FIX_CHECKS_CXX" -O1 -g -pthread ${ldflag_arr[@]+"${ldflag_arr[@]}"} "$exe.o" -o "$exe" &&
    rm -f "$exe.o") > "$log" 2>&1; then
  echo 0 > "$rc_file"
else
  echo 1 > "$rc_file"
fi
COMPILE_EOF
chmod +x "$compile_script"

compile_queue="$tree/build/fix-checks/build-logs/queue.txt"
: > "$compile_queue"
declare -A left_out_map=()
for name in "${checks[@]}"; do
  left_out=
  for fix in ${fix_of[$name]:-}; do
    if [[ " $skipped_fixes " == *" $fix "* ]]; then left_out=$fix; fi
  done
  if [[ -n $left_out ]]; then
    left_out_map[$name]=$left_out
    continue
  fi
  cp "$repo/tools/$name" "$tree/tools/$name"
  printf '%s\n' "$name" >> "$compile_queue"
done

if [[ -s $compile_queue ]]; then
  FIX_CHECKS_TREE="$tree" FIX_CHECKS_CXX="$cxx" \
    FIX_CHECKS_LAUNCHER="${launcher[*]}" FIX_CHECKS_LDFLAG="${ldflag[*]}" \
    xargs -P "$jobs" -n 1 bash "$compile_script" < "$compile_queue"
fi

for name in "${checks[@]}"; do
  if [[ -n ${left_out_map[$name]:-} ]]; then
    echo "=== $name"
    echo "skip $name (fix ${left_out_map[$name]} left out of this build)"
    continue
  fi
  exe="$tree/build/fix-checks/${name%.cpp}"
  build_log="$tree/build/fix-checks/build-logs/${name%.cpp}.log"
  rc_file="$tree/build/fix-checks/build-logs/${name%.cpp}.rc"
  args=()
  if [[ -n "${arguments[$name]:-}" ]]; then args=("${arguments[$name]}"); fi
  echo "=== $name"
  [[ -s $build_log ]] && cat "$build_log"
  if [[ -f $rc_file && $(<"$rc_file") == 0 ]] && (cd "$tree" && "$exe" "${args[@]}"); then
    echo "pass $name"
  else
    echo "FAIL $name"
    status=1
  fi
done
# Fix 137: the FXAA pixel shaders through the core's own SPIR-V step (tools/build/compile_shader_spirv.py,
# as XeniaHelpers.cmake runs it at the compile), where glslangValidator, spirv-opt and spirv-dis are on
# the PATH (the source build installs the Vulkan SDK's before these checks); a shader error then shows
# here instead of in the compile.
echo "=== fix 137, 141 and 145 shaders (glslangValidator)"
if [[ " $skipped_fixes " == *" 137 "* ]]; then
  echo "skip fix 137 shaders (fix 137 left out of this build)"
elif ! command -v glslangValidator > /dev/null || ! command -v spirv-opt > /dev/null || ! command -v spirv-dis > /dev/null; then
  echo "skip fix 137 shaders (no glslangValidator, spirv-opt and spirv-dis on the PATH)"
else
  shader_status=0
  mkdir -p "$tree/build/fix-checks/spirv"
  shader_list="fxaa.ps.glsl fxaa_extreme.ps.glsl"
  # Fix 141: the effect path's pass without AA (and its colour preset in swap_post.glsli).
  if [[ -f "$tree/.deps/xenia-canary/src/xenia/gpu/shaders/swap_post.ps.glsl" ]]; then
    shader_list+=" swap_post.ps.glsl"
  fi
  # Fix 145: SMAA 1x's three passes (with the pinned third_party/smaa/SMAA.hlsl).
  if [[ -f "$tree/.deps/xenia-canary/src/xenia/gpu/shaders/smaa_edge.ps.glsl" ]]; then
    shader_list+=" smaa_edge.ps.glsl smaa_weight.ps.glsl smaa_blend.ps.glsl"
  fi
  for shader in $shader_list; do
    # fxaa.ps.glsl -> fxaa_ps.h, as XeniaHelpers.cmake names it.
    header="$tree/build/fix-checks/spirv/$(echo "${shader%.glsl}" | tr . _).h"
    if "$python" -I "$tree/.deps/xenia-canary/tools/build/compile_shader_spirv.py" \
          "$tree/.deps/xenia-canary/src/xenia/gpu/shaders/$shader" "$header" &&
       grep -q 'OpEntryPoint Fragment' "$header"; then
      echo "compiled $shader"
    else
      echo "FAIL: $shader"
      shader_status=1
    fi
  done
  if [[ $shader_status == 0 ]]; then
    echo "pass fix 137 shaders"
  else
    echo "FAIL fix 137 shaders"
    status=1
  fi
fi
echo "=== check-patch-syntax.py"
if "$python" -I "$repo/tools/check-patch-syntax.py"; then
  echo "pass check-patch-syntax.py"
else
  echo "FAIL check-patch-syntax.py"
  status=1
fi
echo "=== check-gta5-preset.py"
if "$python" -I "$repo/tools/check-gta5-preset.py"; then
  echo "pass check-gta5-preset.py"
else
  echo "FAIL check-gta5-preset.py"
  status=1
fi
# The shipped game patch files (assets/patches): TOML, the title's reader, GTA IV's draw distance.
echo "=== check-game-patch-files.py"
if "$python" -I "$repo/tools/check-game-patch-files.py"; then
  echo "pass check-game-patch-files.py"
else
  echo "FAIL check-game-patch-files.py"
  status=1
fi
# Fix 186 (issue #80): the optional, user-loaded Lapy elevation helper is packaged with full LGPL-2.1
# compliance (pinned commit, licence, NOTICE, MIT stub, non-fatal CI step) -- no build needed here.
echo "=== check-lapy-daemon-package.py"
if "$python" -I "$repo/tools/check-lapy-daemon-package.py"; then
  echo "pass check-lapy-daemon-package.py"
else
  echo "FAIL check-lapy-daemon-package.py"
  status=1
fi
# Fix 164: the home screen's icons (assets/skin, assets/skin-dark) and their SVG sources.
echo "=== check-skin-icons.py"
if "$python" -I "$repo/tools/check-skin-icons.py"; then
  echo "pass check-skin-icons.py"
else
  echo "FAIL check-skin-icons.py"
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
# Fix 180 adds the atomic pulse's cases, left out (-DCHECK_NO_FIX180) when a build leaves fix 180 out.
cp "$repo/tools/check-wait-wakeups.cpp" "$tree/tools/check-wait-wakeups.cpp"
wait_variants=(
  "fix31|-O1 -DXBOX360PS5_MULTI_WAIT_BOUND_MS=60000"
  "legacy|-O1 -DXBOX360PS5_LEGACY_WAKES -DXBOX360PS5_MULTI_WAIT_BOUND_MS=60000"
  "tsan|-O1 -fsanitize=thread -DXBOX360PS5_MULTI_WAIT_BOUND_MS=60000"
  "asan|-O1 -fsanitize=address,undefined -fno-sanitize-recover=all"
)
wait_pids=()
for variant in "${wait_variants[@]}"; do
  label=${variant%%|*}
  read -r -a flags <<< "${variant#*|}"
  # Fix 180's cases (the atomic pulse) only when the build has fix 180.
  if [[ " $skipped_fixes " == *" 180 "* ]]; then flags+=(-DCHECK_NO_FIX180); fi
  # Fix 180b (atomic by default): its default only when the build has it.
  if [[ " $skipped_fixes " == *" 180b "* ]]; then flags+=(-DCHECK_NO_FIX180B); fi
  exe="$tree/build/fix-checks/check-wait-wakeups-$label"
  wlog="$tree/build/fix-checks/build-logs/wait-$label.log"
  wrc="$tree/build/fix-checks/build-logs/wait-$label.rc"
  (
    if (cd "$tree" &&
        ${launcher[@]+"${launcher[@]}"} "$cxx" -std=c++20 -g -pthread -march=znver2 -Wall -Wno-unused-function \
          "${flags[@]}" -Iinclude -Ireconstructed -I.deps/xenia-canary/src -I.deps/xenia-canary \
          -c tools/check-wait-wakeups.cpp -o "$exe.o" &&
        "$cxx" -g -pthread "${flags[@]}" "$exe.o" -o "$exe" &&
        rm -f "$exe.o") > "$wlog" 2>&1; then
      echo 0 > "$wrc"
    else
      echo 1 > "$wrc"
    fi
  ) &
  wait_pids+=($!)
done
for pid in "${wait_pids[@]}"; do wait "$pid" || true; done

for variant in "${wait_variants[@]}"; do
  label=${variant%%|*}
  exe="$tree/build/fix-checks/check-wait-wakeups-$label"
  wlog="$tree/build/fix-checks/build-logs/wait-$label.log"
  wrc="$tree/build/fix-checks/build-logs/wait-$label.rc"
  run=("$exe")
  if [[ $label == tsan || $label == asan ]]; then run=(setarch "$(uname -m)" -R "$exe"); fi
  echo "=== check-wait-wakeups.cpp ($label)"
  [[ -s $wlog ]] && cat "$wlog"
  if [[ -f $wrc && $(<"$wrc") == 0 ]] && (cd "$tree" && "${run[@]}"); then
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
if [[ " $skipped_fixes " == *" 46 "* || " $skipped_fixes " == *" 199 "* ]]; then
  echo "skip check-title-id.py (fix 46 or the TrinityX360 rename left out of this build)"
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
# Fix 169: the phone page's steadier pose frames and its tolerance of one failed request, with node.
echo "=== check-phone-pose-steady.mjs"
if [[ " $skipped_fixes " == *" 169 "* ]]; then
  echo "skip check-phone-pose-steady.mjs (fix 169 left out of this build)"
elif node "$repo/tools/check-phone-pose-steady.mjs" "$tree"; then
  echo "pass check-phone-pose-steady.mjs"
else
  echo "FAIL check-phone-pose-steady.mjs"
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
# Fix 139: a file system entry outlives its files and deletes (left out with the fix).
echo "=== check-vfs-entry-lifetime.sh"
if ! grep -q 'fix 139' "$tree/.deps/xenia-canary/src/xenia/vfs/entry.cc"; then
  echo "skip check-vfs-entry-lifetime.sh (fix 139 left out)"
elif bash "$repo/tools/check-vfs-entry-lifetime.sh" "$tree"; then
  echo "pass check-vfs-entry-lifetime.sh"
else
  echo "FAIL check-vfs-entry-lifetime.sh"
  status=1
fi
exit $status
