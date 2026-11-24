"""Host check of Grand Theft Auto V's preset (545408A7) in assets/presets.txt.

    python3 -I tools/check-gta5-preset.py

The 1.0.6 console tests (2026-11-24) found that GTA V needs no resolve readback: the minimap, map,
mirrors, phone, weapon wheel, thumbnails and ocean water look the same as with Fast, and readback
Off runs at 24-26 fps driving against 13-23 with Fast. "Optimized video memory" (memory_boost) cut
write-watch arming from about 200-240 to 7 ms/s, for a steady 30 fps (the game's own cap). The
upstream compatibility list's readback=1 must not come back without a new test. It checks:
  * there is exactly one [545408A7] section, and it sets exactly readback=0 and memory_boost=1;
  * its comment keeps the reason, so nobody restores readback=1 from the upstream list;
  * both keys are options a preset may set (the same keys other sections already use).
"""
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]


def sections(text):
    """[(title id, {key: value}, comment lines above it)] in file order."""
    out, comment, current = [], [], None
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            comment = [] if current is None else comment
            current = None
            continue
        if line.startswith('#'):
            if current is None:
                comment.append(line)
            continue
        m = re.fullmatch(r'\[([0-9A-Fa-f]{8})\]', line)
        if m:
            current = (m.group(1).upper(), {}, comment)
            out.append(current)
            comment = []
            continue
        if current is not None and '=' in line:
            key, value = (part.strip() for part in line.split('=', 1))
            current[1][key] = value
    return out


def main():
    text = (REPO / 'assets/presets.txt').read_text(encoding='utf-8')
    all_sections = sections(text)
    gta5 = [s for s in all_sections if s[0] == '545408A7']
    assert len(gta5) == 1, f'{len(gta5)} [545408A7] sections'
    _, values, comment = gta5[0]
    assert values == {'readback': '0', 'memory_boost': '1'}, values
    note = ' '.join(comment)
    for words in ('1.0.6 console tests', 'ocean water', 'Do not restore readback=1'):
        assert words in note, f'the GTA V preset comment lost "{words}"'
    # The keys are ones the presets already use for other games, so the launcher reads them.
    used = {key for tid, vals, _ in all_sections if tid != '545408A7' for key in vals}
    assert {'readback', 'memory_boost'} <= used, sorted(used)
    print('PASS: Grand Theft Auto V (545408A7) presets readback=0 and memory_boost=1, with the reason kept')
    return 0


if __name__ == '__main__':
    sys.exit(main())
