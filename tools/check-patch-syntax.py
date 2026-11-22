"""Host check of the fix patches' C/C++ text, before the long compile.

    python3 -I tools/check-patch-syntax.py [patch ...]

For each tools/source-1.0.2/fixes/*.patch (or the patches given), the lines each hunk adds to a
C or C++ file are scanned with a small lexer (comments, string and character literals, raw
strings): a line that ends inside a string or character literal is an error, and so is a file
whose added lines change the balance of its braces, parentheses or brackets against the lines
the patch removes. Candidate 36's fix 30 put a newline inside a character literal (a shell had
turned \\n into a line break): `find('` then `', line_start)`; the compile failed minutes later
with a cascade of errors. This catches that, and an unbalanced brace, in a second.
"""
import re
import sys
from pathlib import Path

FIXES = Path(__file__).resolve().parents[1] / 'tools/source-1.0.2/fixes'
SOURCES = re.compile(r'\.(c|cc|cpp|cxx|h|hh|hpp|hxx|inc)$')
PAIRS = {'{': '}', '(': ')', '[': ']'}


class Lexer:
    """Line-by-line C/C++ lexer state: whether a block comment or raw string is open."""

    def __init__(self):
        self.block = False
        self.raw_end = None  # the )delim" that closes an open raw string
        self.open_quote = None  # a literal continued with a backslash at the end of a line

    @staticmethod
    def closing(text, start, quote):
        """Index just past the closing quote from `start` (escapes skipped), or None."""
        j, n = start, len(text)
        while j < n and text[j] != quote:
            j += 2 if text[j] == chr(92) else 1
        return j + 1 if j < n else None

    def line(self, text):
        """Returns (counts of the bracket characters outside literals, error or None)."""
        counts = {c: 0 for c in '{}()[]'}
        i, n = 0, len(text)
        if self.open_quote:
            quote, self.open_quote = self.open_quote, None
            end = self.closing(text, 0, quote)
            if end is None:
                if text.endswith(chr(92)):
                    self.open_quote = quote
                    return counts, None
                return counts, f'unterminated {"string" if quote == chr(34) else "character"} literal'
            i = end
        while i < n:
            if self.raw_end:
                end = text.find(self.raw_end, i)
                if end < 0:
                    return counts, None
                i = end + len(self.raw_end)
                self.raw_end = None
                continue
            if self.block:
                end = text.find('*/', i)
                if end < 0:
                    return counts, None
                i = end + 2
                self.block = False
                continue
            c = text[i]
            if text.startswith('//', i):
                return counts, None
            if text.startswith('/*', i):
                self.block = True
                i += 2
                continue
            raw = re.match(r'(?:u8|u|U|L)?R"([^()\\ ]{0,16})\(', text[i:])
            if raw and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == '_')):
                self.raw_end = ')' + raw.group(1) + '"'
                i += raw.end()
                continue
            if c in '"\'':
                # A digit separator (1'000'000) is not a character literal; an encoding prefix
                # (u'a', U'a', L'a', u8'a': fix 114's old keyboard header) is.
                prefix = re.search(r'(?:^|[^A-Za-z0-9_])(u8|u|U|L)$', text[:i])
                if (c == "'" and not prefix and i and text[i - 1].isalnum() and i + 1 < n and
                        text[i + 1].isalnum()):
                    i += 1
                    continue
                end = self.closing(text, i + 1, c)
                if end is None:
                    if text.endswith(chr(92)):  # continued on the next line
                        self.open_quote = c
                        return counts, None
                    return counts, f'unterminated {"string" if c == chr(34) else "character"} literal'
                i = end
                continue
            if c in counts:
                counts[c] += 1
            i += 1
        return counts, None


def check_patch(path):
    """The problems of one patch, as text lines."""
    problems = []
    current = None
    added = removed = None
    balances = {}
    lex_new = lex_old = None
    for number, raw in enumerate(path.read_bytes().decode('utf-8', 'replace').split('\n'), 1):
        line = raw[:-1] if raw.endswith('\r') else raw
        if line.startswith('diff --git '):
            current = line.split(' b/', 1)[-1]
            continue
        if line.startswith('@@'):
            lex_new, lex_old = Lexer(), Lexer()
            continue
        if not current or not SOURCES.search(current) or lex_new is None:
            continue
        if line.startswith('+++') or line.startswith('---'):
            continue
        body = line[1:]
        balance = balances.setdefault(current, {c: 0 for c in '{}()[]'})
        if line.startswith('+'):
            counts, error = lex_new.line(body)
            if error:
                problems.append(f'{path.name}:{number}: {current}: {error}: {body.strip()[:100]}')
            for c, k in counts.items():
                balance[c] += k
        elif line.startswith('-'):
            counts, _ = lex_old.line(body)
            for c, k in counts.items():
                balance[c] -= k
        elif line.startswith(' '):
            lex_new.line(body)
            lex_old.line(body)
    for name, balance in balances.items():
        for opening, closing in PAIRS.items():
            delta = balance[opening] - balance[closing]
            if delta:
                problems.append(f'{path.name}: {name}: the patch changes the balance of {opening}{closing} by {delta:+d}')
    return problems


def self_test(scratch):
    """The lexer and the balance on made-up patches: what must pass and what must fail."""
    # Built from character codes, so no shell or editor can turn an escape into a line break.
    q, bs, nl = chr(39), chr(92), chr(10)

    def run(added, removed=()):
        text = 'diff --git a/x.cc b/x.cc' + nl + '--- a/x.cc' + nl + '+++ b/x.cc' + nl + '@@ -1 +1 @@' + nl
        text += ''.join('-' + line + nl for line in removed)
        text += ''.join('+' + line + nl for line in added)
        path = scratch / 'self-test.patch'
        path.write_text(text, encoding='utf-8')
        return check_patch(path)
    good = [
        ['size_t end = text.find(' + q + bs + 'n' + q + ', start);'],  # an escaped character
        ['const char* s = "a ' + bs + '" quote, a ' + q + ' and // not a comment";'],
        ['int n = 1' + q + '000' + q + '000;  // digit separators'],
        ['if (c >= u' + q + 'a' + q + ' && c <= u' + q + 'z' + q + ') c -= 32;'],  # prefixed characters
        ['auto w = L' + q + 'x' + q + ' + U' + q + 'y' + q + ' + u8' + q + 'z' + q + ';'],
        ['auto r = R"x(a ) " ' + q + ' { unbalanced inside)x";'],
        ['auto r = R"(', 'still raw ( {', ')";'],  # a raw string over lines
        ['/* a comment with ' + q + ' and { */ int a;'],
        ['/* open', ' { ' + q + ' still a comment', '*/'],
        ['XELOGW("a ' + bs, 'continued");'],  # a string continued with a backslash
        ['  if (a) {', '  }'],
    ]
    for added in good:
        assert not run(added), (added, run(added))
    assert not run(['f(a, b);'], ['f(a);'])  # replaced lines keep the balance
    bad = [
        (['find(' + q, q + ', start);'], 'unterminated character literal'),  # candidate 36
        (['const char* s = "open;'], 'unterminated string literal'),
        (['  if (a) {'], 'balance of {} by +1'),
        (['f(a, b));'], 'balance of () by -1'),
        (['int a[3;'], 'balance of [] by +1'),
    ]
    for added, expected in bad:
        problems = run(added)
        assert any(expected in p for p in problems), (added, problems)
    assert run([], ['  }']) and 'by +1' in run([], ['  }'])[0]  # removing a brace unbalances too
    (scratch / 'self-test.patch').unlink()


def main():
    import tempfile
    with tempfile.TemporaryDirectory() as scratch:
        self_test(Path(scratch))
    patches = [Path(a) for a in sys.argv[1:]] or sorted(FIXES.glob('*.patch'))
    problems = []
    for patch in patches:
        problems += check_patch(patch)
    for problem in problems:
        print('FAIL', problem)
    if problems:
        sys.exit(1)
    print(f'PASS: the C/C++ lines of {len(patches)} fix patches have no unterminated literal and keep their '
          'braces, parentheses and brackets balanced')


if __name__ == '__main__':
    main()
