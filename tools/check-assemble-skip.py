"""Host check of assemble.py's switch that leaves fixes out of a test build.

    python3 -I tools/check-assemble-skip.py [--tree <assembled tree>]

select_fixes and skip_names on the real fix list and on made-up names: a fix named by its number,
stem or file name is left out and only it; a name that matches no fix, or more than one, stops the
assembly; PS5X360_SKIP_FIXES's comma-separated list adds to --skip-fix. With --tree, the tree's
SOURCES.json lists the fixes it applied and left out, and none is in both.
"""
import argparse
import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'tools/source-1.0.2'))
import assemble  # noqa: E402


def stops(patches, skip):
    try:
        assemble.select_fixes(patches, skip)
    except SystemExit:
        return True
    return False


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--tree', type=Path)
    args = parser.parse_args()

    patches = sorted(assemble.FIXES.glob('*.patch'), key=assemble.fix_order)
    # Fixes apply by number, then letter: 100 after 99, not before 11.
    order = sorted((Path(n) for n in ('100-z.patch', '11-a.patch', '08b-b.patch', '08-c.patch', '99-d.patch')),
                   key=assemble.fix_order)
    assert [p.name for p in order] == ['08-c.patch', '08b-b.patch', '11-a.patch', '99-d.patch', '100-z.patch'], order
    names = [p.name for p in patches]
    assert '01-jit-reserved-barriers.patch' in names, names

    # Fix 01 by number, stem and file name: only it is left out.
    for skip in (['01'], ['01-jit-reserved-barriers'], ['01-jit-reserved-barriers.patch']):
        selected, left_out = assemble.select_fixes(patches, skip)
        assert [p.name for p in left_out] == ['01-jit-reserved-barriers.patch'], (skip, left_out)
        assert len(selected) == len(patches) - 1 and all(p not in selected for p in left_out)
        assert selected == sorted(selected, key=assemble.fix_order)  # order kept (by number: 102 after 99)
    # Nothing skipped: everything applied, in order.
    selected, left_out = assemble.select_fixes(patches, [])
    assert selected == patches and not left_out
    # "08" and "08b" are different fixes.
    if any(n.startswith('08b-') for n in names) and any(n.startswith('08-') for n in names):
        _, left_out = assemble.select_fixes(patches, ['08'])
        assert [p.name for p in left_out] == [n for n in names if n.startswith('08-')]
    # A typo, or a name matching several, stops the assembly.
    assert stops(patches, ['1'])
    assert stops(patches, ['99'])
    assert stops(patches, ['jit'])
    made_up = [Path('fixes/30-a.patch'), Path('fixes/30-b.patch')]
    assert stops(made_up, ['30'])
    assert not stops(made_up, ['30-a'])
    # The environment's list adds to the arguments; blanks are ignored.
    assert assemble.skip_names(['01'], ' 08b , ,24-stall-code-words ') == ['01', '08b', '24-stall-code-words']
    assert assemble.skip_names(None, None) == [] and assemble.skip_names([], '') == []

    if args.tree:
        report = json.loads((args.tree / 'SOURCES.json').read_text(encoding='utf-8'))
        applied = {fix['patch'] for fix in report.get('fixes', [])}
        skipped = set(report.get('fixes_skipped', []))
        assert applied and not (applied & skipped), (applied & skipped)
        assert len(applied) + len(skipped) == len(patches), (len(applied), len(skipped), len(patches))
        print(f'  tree: {len(applied)} fixes applied, left out: {sorted(skipped) or "none"}')
    print('PASS: assemble.py leaves out exactly the fixes asked for, and stops on a wrong name')


if __name__ == '__main__':
    main()
