"""English as the source language of the 1.0.2 source rebuild's interface (fix 25).

    python3 -I tools/source-1.0.2/english/rekey.py --tree build/src102 \
        [--zip PPSA50011-v1.0.2.zip --overrides <PS5X360>/assets/lang-overrides] [--check]

The official 1.0.2 title keys its interface text by Brazilian Portuguese: Tr("Configurações"),
FramegenText(pt, en, es), CoreUiText's {en, pt, es} rows, and /app0/assets/lang/<code>.txt files
of "<Portuguese> = <translation>" lines. This script makes English the key instead and keeps
Portuguese (Brazil) as an ordinary translation (pt-BR.txt):

  * the map Portuguese key -> English key. The English comes from, in this order: the language
    files' en.txt (the official package's merged with the fork's overrides, as the binary
    release ships it: an override replaces the package's line, so this is the English an English
    user sees), the built-in {pt, en, es} table of Tr (i18n.hpp), FramegenText's English
    argument, CoreUiText's English column, then english/extra.txt (texts with no English
    anywhere). extra.txt also gives the context keys ("On||feminine") of Portuguese keys whose
    English is the same but whose translations differ (a collision), and translations of keys
    that are in no language file, and ([core]) the key of a CoreUiText row whose English differs
    from its Portuguese text's key. Any key without English, any unresolved collision, and any
    FramegenText call or CoreUiText row that would change its English or Spanish stops the script.
  * fixes/25-english-source-text.patch: the code of the tree with each Portuguese key literal in
    translation context replaced by its English key (a C/C++ lexer finds them: Tr("..."), the
    tables Tr is given where they are used (CONTEXTS), FramegenText calls become Tr), Tr's
    lookup rewritten for English keys (i18n.hpp, i18n_ui.hpp), CoreUiText keyed by English,
    comments that quote Portuguese interface text changed to the English (TEXT_EDITS).
  * english/lang/<code>.txt for every interface language: the language files keyed by English.
    The eboot reads them from /app0/assets/lang/ (fixes/README.md). Keys that a later fix renames
    (RENAMES: fix 46, PS5X360Reloaded's own title ID and name; fix 54, the measurement's 10 s) are written renamed, each
    language's text with the same replacement; fix 25's code keeps the keys it was made with.

Inputs: --tree, an assembled tree (assemble.py) with fixes up to 24; when fix 25 is already
applied it is reversed in a copy first, after the fixes that follow it (those are written against
English keys; extra.txt's [added <code>] sections translate the keys they add). english/official-lang/<code>.txt is the snapshot of the
Portuguese-keyed language files (the official package's assets/lang merged with the fork's
assets/lang-overrides exactly as tools/binary-release/build_package.py merges them); --zip and
--overrides rebuild it (SOURCES.json records the inputs' and the snapshot's SHA-256; --check
verifies the snapshot against it, and against the inputs with --zip). --check regenerates everything in a temporary folder and fails if a
committed output differs. Nothing here runs code from the tree or the package.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
S102 = HERE.parent
REPO = S102.parents[1]
FIX_NAME = '25-english-source-text.patch'
FIX = S102 / 'fixes' / FIX_NAME
LANG_DIR = HERE / 'lang'
OFFICIAL_DIR = HERE / 'official-lang'
EXTRA = HERE / 'extra.txt'

# Interface language id (launcher.cpp kLanguages, i18n_ui.hpp UiLanguageCode) -> file code.
LANGUAGES = {1: 'en', 9: 'pt-BR', 19: 'pt', 5: 'es', 18: 'es-419', 3: 'de', 4: 'fr', 6: 'it',
             2: 'ja', 7: 'ko', 16: 'nl', 11: 'pl', 12: 'ru', 14: 'tr', 17: 'zh-Hans', 8: 'zh-Hant'}
CODE_OF = LANGUAGES
# The official package's files (no pt-BR.txt: Brazilian Portuguese was the source).
OFFICIAL_CODES = sorted(code for code in LANGUAGES.values() if code != 'pt-BR')
# The 13 files with every key (en.txt and es.txt of the package have only the 1.0 block).
FULL_CODES = sorted(code for code in OFFICIAL_CODES if code not in ('en', 'es'))
CONTEXT_MARK = '||'

TITLE_DIRS = ('include', 'platform', 'src', 'port', 'reconstructed')
SKIP_PREFIXES = ('port/vendor/',)
CODE_SUFFIXES = ('.cpp', '.cc', '.c', '.h', '.hpp', '.inc')

# Tables and functions whose string literals are translation keys although they are not
# written inside Tr(...): Tr is given them where they are used. (file, anchor, extent): the
# region starts at the anchor (which must occur once) and ends at the end of the statement
# ('statement': the next ';' outside brackets) or of the first block ('block': the '}' that
# closes the first '{'). 'english' regions are not translated at run time at all (a file that
# is not built); their Portuguese is replaced by the English all the same.
CONTEXTS = [
    ('src/launcher.cpp', 'const std::vector<Option>& Options() {', 'block',
     'settings options: labels, about texts, choices, off_on (Tr where drawn)'),
    ('include/xbox360ps5/launcher.hpp', 'inline constexpr const char* kOptionCategories[] =', 'statement',
     'settings categories (Tr where drawn)'),
    ('src/launcher.cpp', 'const char* const kFilters[] =', 'statement', 'library filters (Tr where drawn)'),
    ('src/launcher.cpp', 'constexpr Language kLanguages[] =', 'statement',
     'language picker: Tr(entry.name); the native names are not keys (NATIVE_NAMES)'),
    ('src/launcher.cpp', 'const char* const tabs[] = {"Geral", "Por jogo"};', 'statement',
     'settings tabs (Tr where drawn)'),
    ('src/launcher.cpp', 'void DrawGameInfo(Launcher::Canvas& c, const GameEntry& game, float x) {', 'block',
     'game sheet rows: row("...") calls Tr(label)'),
    ('src/launcher.cpp', 'const char* DlcMessage(const std::string& message) {', 'block',
     'DLC installer messages -> keys (Tr(entry.second))'),
    ('src/canary_game_main.cpp', 'constexpr const char* kGuidePages[] =', 'statement', 'guide pages (Tr where drawn)'),
    ('port/kinect_state.inc', 'const char* const kKinectChannels[] =', 'statement', 'home channels (Tr where drawn)'),
    ('port/guide_network.hpp', 'inline const char* const kModeLabels[] =', 'statement',
     'fix 22 network modes (Tr where drawn)'),
    ('port/network_switch.hpp', 'inline const char* OutcomeLabel(Outcome outcome) {', 'block',
     'fix 22 guide row after a switch (Tr where drawn)'),
    ('reconstructed/xbox360ps5/game_info.hpp', 'inline const char* CompatStateText(const std::string& state) {',
     'block', 'compatibility states (Tr at the caller)'),
    ('reconstructed/dark_theme.hpp', 'inline const char* ToggleMessage(bool done) {', 'block',
     'theme toggle messages (Tr at the caller)'),
    ('src/native_game_main.cpp', 'std::snprintf(text, sizeof(text), "N', 'english',
     'not built (canary/CMakeLists.txt); its message is written in English'),
]
# kLanguages' entries in their own language: shown as they are (Tr may translate them).
NATIVE_NAMES = {'English', 'Español (España)', 'Español (Latinoamérica)', 'Français', 'Deutsch', 'Italiano',
                'Nederlands', 'Polski', 'Português (Portugal)', 'Português (Brasil)', 'Türkçe', 'Русский',
                '日本語', '한국어', '简体中文', '繁體中文'}
# Portuguese key literals that are deliberately not translation keys (file, value, reason).
NOT_KEYS = []
# Keys of fix 25 that a later fix renames in the code, with the text replaced in the key and in every
# language's translation: (English key of fix 25, old text, new text, the fix). Fix 25 keeps the keys
# it was made with (the tree before the later fix); the language files carry the renamed keys only,
# so the old ones show nothing but their English while that fix is left out of a build. Each old
# text must be in the key and in each language's translation (a translation that dropped it stops
# the script), and the renamed key must be no other key.
# Fix 46 (fixes/46-ps5x360reloaded-identity.patch): PS5X360Reloaded has a title ID and a folder of
# its own (PPSA83600) and its own name; "PS5X360/saves" on a drive (fix 27) stays as it is.
RENAMES = [
    ('Copy your dash.xex to /data/homebrew/PPSA50011/dashboard.', 'PPSA50011', 'PPSA83600', 46),
    ('On: the game finds the DLC. Off: kept, but the game does not see it. Copy new packages to '
     "PPSA50011/assets/dlc or to the game's dlc folder: they install by themselves when you press Play "
     '(or here, with X).', 'PPSA50011', 'PPSA83600', 46),
    ("One update is active at a time. Copy the file to PPSA50011/assets/updates or to the game's "
     'updates folder: the newest installs by itself when you press Play (or here, with X). Patches made '
     'for the game without an update do not apply while one is active.', 'PPSA50011', 'PPSA83600', 46),
    ('Place .patch.toml files (Xenia Canary format) in /data/homebrew/PPSA50011/assets/patches/.',
     'PPSA50011', 'PPSA83600', 46),
    ('PS5X360 Guide', 'PS5X360', 'PS5X360Reloaded', 46),
    ('PS5X360: experimental Xbox 360 emulator for PlayStation 5.', 'PS5X360', 'PS5X360Reloaded', 46),
    # Fix 54: the guide's measurement samples for 10 s (every translation has its one "5").
    ('Measure performance (5 s)', '5', '10', 54),
]
RENAMED = {key: (old, new) for key, old, new, _ in RENAMES}


def renamed_key(key):
    """A key as the language files have it (RENAMES applied)."""
    if key not in RENAMED:
        return key
    old, new = RENAMED[key]
    return key.replace(old, new)


def renamed_text(key, text):
    """A translation of a key as the language files have it (RENAMES applied)."""
    if key not in RENAMED or text is None:
        return text
    old, new = RENAMED[key]
    return text.replace(old, new)

I18N = 'include/xbox360ps5/i18n.hpp'
I18N_UI = 'reconstructed/xbox360ps5/i18n_ui.hpp'
LAUNCHER = 'src/launcher.cpp'
GAME_MAIN = 'src/canary_game_main.cpp'


# --- language files (reconstructed/xbox360ps5/i18n_ui.hpp's format) -------------------------
def unescape(text):
    out, i = [], 0
    while i < len(text):
        ch = text[i]
        if ch == '\\' and i + 1 < len(text):
            i += 1
            ch = text[i]
            out.append({'s': ' ', 'n': '\n', 't': '\t'}.get(ch, ch))
        else:
            out.append(ch)
        i += 1
    return ''.join(out)


def escape(text):
    out = []
    for ch in text:
        out.append({'\\': '\\\\', ' ': '\\s', '\n': '\\n', '\t': '\\t'}.get(ch, ch))
    escaped = ''.join(out)
    return '\\' + escaped if escaped.startswith('#') else escaped


def parse_lang(text):
    """Key -> value, as i18n_ui::Read: first ' = ' splits, '#' comments, a later key wins."""
    table = {}
    for line in text.split('\n'):
        if line.endswith('\r'):
            line = line[:-1]
        if not line or line.startswith('#'):
            continue
        split = line.find(' = ')
        if split < 0:
            continue
        table[unescape(line[:split])] = unescape(line[split + 3:])
    return table


def format_lang(header, table):
    lines = [f'# {line}' if line else '#' for line in header]
    lines += [f'{escape(key)} = {escape(value)}' for key, value in sorted(table.items())]
    return '\n'.join(lines) + '\n'


def merge_lang(base, overrides):
    """tools/binary-release/build_package.py merge_lang, unchanged: the binary release's files."""
    wanted = {}
    for line in overrides.decode('utf-8').splitlines():
        if line and not line.startswith('#'):
            key, sep, value = line.partition(' = ')
            if not sep:
                raise SystemExit(f"Language override line without ' = ': {line!r}")
            wanted[key] = value
    lines = base.decode('utf-8').split('\n')
    for i, line in enumerate(lines):
        key, sep, _ = line.partition(' = ')
        if sep and key in wanted:
            lines[i] = f'{key} = {wanted.pop(key)}'
    if lines and lines[-1] == '':
        lines.pop()
    lines += [f'{key} = {value}' for key, value in wanted.items()]
    return ('\n'.join(lines) + '\n').encode('utf-8')


def official_snapshot(zip_path, overrides):
    """The Portuguese-keyed files the binary release ships: package files + overrides."""
    files = {}
    sources = {'package': 'PPSA50011.zip of PS5X360 v1.0.2 (assets/lang/<code>.txt)',
               'package_sha256': sha256(zip_path.read_bytes()),
               'overrides': "the fork's assets/lang-overrides/<code>.txt, merged as build_package.py merge_lang",
               'overrides_sha256': {}}
    with zipfile.ZipFile(zip_path) as z:
        for code in OFFICIAL_CODES:
            data = z.read(f'PPSA50011/assets/lang/{code}.txt')
            extra = overrides / f'{code}.txt'
            if extra.exists():
                sources['overrides_sha256'][extra.name] = sha256(extra.read_bytes())
                data = merge_lang(data, extra.read_bytes())
            files[f'{code}.txt'] = data
    # The snapshot's own files, so that --check notices one edited by hand without the inputs.
    sources['files_sha256'] = {name: sha256(data) for name, data in sorted(files.items())}
    files['SOURCES.json'] = (json.dumps(sources, indent=1, sort_keys=True) + '\n').encode()
    return files


def check_snapshot(folder, problems):
    """official-lang/ against the hashes SOURCES.json records for it (no --zip needed)."""
    try:
        recorded = json.loads((folder / 'SOURCES.json').read_text(encoding='utf-8'))['files_sha256']
    except (OSError, ValueError, KeyError) as error:
        problems.append(f'official-lang: SOURCES.json has no files_sha256 ({error})')
        return
    names = sorted(p.name for p in folder.iterdir() if p.name != 'SOURCES.json')
    if names != sorted(recorded):
        problems.append(f'official-lang: files {names} are not the ones SOURCES.json lists {sorted(recorded)}')
    for name in names:
        if name in recorded and sha256((folder / name).read_bytes()) != recorded[name]:
            problems.append(f'official-lang: {name} is not the file SOURCES.json records (rebuild it with '
                            '--zip and --overrides; it is not edited by hand)')


def load_official(folder=OFFICIAL_DIR):
    return {code: parse_lang((folder / f'{code}.txt').read_text(encoding='utf-8')) for code in OFFICIAL_CODES}


def sha256(data):
    return hashlib.sha256(data).hexdigest()


# --- C/C++ lexer -------------------------------------------------------------------------------
class Tok:
    __slots__ = ('kind', 'start', 'end', 'value', 'raw')

    def __init__(self, kind, start, end, value, raw=False):
        self.kind, self.start, self.end, self.value, self.raw = kind, start, end, value, raw

    def __repr__(self):
        return f'Tok({self.kind},{self.start},{self.value!r})'


IDENT = re.compile(r'[A-Za-z_][A-Za-z0-9_]*')
STRING_PREFIXES = {'u8', 'u', 'U', 'L'}
SIMPLE_ESCAPES = {'n': '\n', 't': '\t', 'r': '\r', 'a': '\a', 'b': '\b', 'f': '\f', 'v': '\v',
                  '\\': '\\', '"': '"', "'": "'", '?': '?'}


def c_unescape(body, where):
    """A narrow string literal's bytes (the source is UTF-8; hex and octal escapes are bytes),
    decoded as UTF-8."""
    out, i = bytearray(), 0
    while i < len(body):
        ch = body[i]
        if ch != '\\':
            out += ch.encode('utf-8')
            i += 1
            continue
        e = body[i + 1]
        if e in SIMPLE_ESCAPES:
            out += SIMPLE_ESCAPES[e].encode()
            i += 2
        elif e == 'x':
            m = re.match(r'[0-9A-Fa-f]+', body[i + 2:])
            out.append(int(m.group(0), 16) & 0xFF)
            i += 2 + len(m.group(0))
        elif e in '01234567':
            m = re.match(r'[0-7]{1,3}', body[i + 1:])
            out.append(int(m.group(0), 8) & 0xFF)
            i += 1 + len(m.group(0))
        elif e in 'uU':
            n = 4 if e == 'u' else 8
            out += chr(int(body[i + 2:i + 2 + n], 16)).encode('utf-8')
            i += 2 + n
        elif e == '\n':
            i += 2
        else:
            raise SystemExit(f'{where}: unknown escape \\{e}')
    return out.decode('utf-8', errors='surrogateescape')


def lex(text, where=''):
    """Tokens of C/C++ source: 'str' (value unescaped; raw strings flagged), 'chr', 'num', 'id',
    'op' (one character), 'comment'."""
    toks, i, n = [], 0, len(text)
    line_start = True
    while i < n:
        c = text[i]
        if c == '\n':
            line_start = True
            i += 1
            continue
        if c in ' \t\r\f\v':
            i += 1
            continue
        if text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            toks.append(Tok('comment', i, j, text[i:j]))
            i = j
            continue
        if text.startswith('/*', i):
            j = text.find('*/', i + 2)
            if j < 0:
                raise SystemExit(f'{where}: unterminated comment')
            toks.append(Tok('comment', i, j + 2, text[i:j + 2]))
            i = j + 2
            continue
        at_line_start, line_start = line_start, False
        if c == '#' and at_line_start:
            toks.append(Tok('op', i, i + 1, '#'))
            i += 1
            continue
        m = IDENT.match(text, i)
        if m:
            word, j = m.group(0), m.end()
            if j < n and text[j] == '"' and (word in STRING_PREFIXES or word in ('R', 'u8R', 'uR', 'UR', 'LR')):
                if word.endswith('R'):
                    open_ = text.find('(', j)
                    delim = text[j + 1:open_]
                    close = text.find(')' + delim + '"', open_)
                    if close < 0:
                        raise SystemExit(f'{where}: unterminated raw string')
                    end = close + len(delim) + 2
                    toks.append(Tok('str', i, end, text[open_ + 1:close], raw=True))
                    i = end
                    continue
                i = j  # the prefix; the string follows
                start = m.start()
                tok = lex_string(text, j, where)
                tok.start = start
                toks.append(tok)
                i = tok.end
                continue
            if j < n and text[j] == "'" and word in STRING_PREFIXES:
                end = skip_char(text, j, where)
                toks.append(Tok('chr', i, end, text[i:end]))
                i = end
                continue
            toks.append(Tok('id', i, j, word))
            i = j
            continue
        if c.isdigit() or (c == '.' and i + 1 < n and text[i + 1].isdigit()):
            j = i + 1
            while j < n:
                d = text[j]
                if d.isalnum() or d in '_.':
                    j += 1
                elif d == "'" and j + 1 < n and text[j + 1].isalnum():
                    j += 1
                elif d in '+-' and text[j - 1] in 'eEpP' and not text[i:j].lower().startswith('0x') or \
                        d in '+-' and text[j - 1] in 'pP':
                    j += 1
                else:
                    break
            toks.append(Tok('num', i, j, text[i:j]))
            i = j
            continue
        if c == '"':
            tok = lex_string(text, i, where)
            toks.append(tok)
            i = tok.end
            continue
        if c == "'":
            end = skip_char(text, i, where)
            toks.append(Tok('chr', i, end, text[i:end]))
            i = end
            continue
        toks.append(Tok('op', i, i + 1, c))
        i += 1
    return toks


def lex_string(text, i, where):
    j = i + 1
    while True:
        if j >= len(text) or text[j] == '\n':
            raise SystemExit(f'{where}: unterminated string at {i}')
        if text[j] == '\\':
            j += 2
            continue
        if text[j] == '"':
            break
        j += 1
    return Tok('str', i, j + 1, c_unescape(text[i + 1:j], where))


def skip_char(text, i, where):
    j = i + 1
    while j < len(text) and text[j] != "'":
        if text[j] == '\n':
            raise SystemExit(f'{where}: unterminated character literal at {i}')
        j += 2 if text[j] == '\\' else 1
    return j + 1


class Literal:
    """Adjacent string literals (concatenated by the compiler) as one value."""
    __slots__ = ('start', 'end', 'value', 'raw', 'index', 'last', 'call', 'context')

    def __init__(self, toks, index, last):
        self.start, self.end = toks[index].start, toks[last].end
        self.value = ''.join(toks[k].value for k in range(index, last + 1) if toks[k].kind == 'str')
        self.raw = any(toks[k].raw for k in range(index, last + 1))
        self.index, self.last = index, last
        self.call = None
        self.context = None


def code_tokens(toks):
    return [t for t in toks if t.kind != 'comment']


def literals(text, where=''):
    """(tokens without comments, literals) with each literal's innermost enclosing call."""
    toks = code_tokens(lex(text, where))
    lits, stack, k = [], [], 0
    while k < len(toks):
        t = toks[k]
        if t.kind == 'op' and t.value in '([{':
            callee = toks[k - 1].value if t.value == '(' and k and toks[k - 1].kind == 'id' else None
            stack.append((t.value, callee, k))
        elif t.kind == 'op' and t.value in ')]}':
            if stack:
                stack.pop()
        elif t.kind == 'str':
            last = k
            while last + 1 < len(toks) and toks[last + 1].kind == 'str':
                last += 1
            lit = Literal(toks, k, last)
            if stack and stack[-1][0] == '(':
                lit.call = (stack[-1][1], stack[-1][2])
            lits.append(lit)
            k = last
        k += 1
    return toks, lits


def region_end(toks, text, start, extent, where):
    first = next(k for k, t in enumerate(toks) if t.start >= start)
    depth = 0
    for k in range(first, len(toks)):
        t = toks[k]
        if t.kind != 'op':
            continue
        if t.value in '([{':
            depth += 1
        elif t.value in ')]}':
            depth -= 1
            if extent == 'block' and depth == 0 and t.value == '}':
                return t.end
        elif t.value == ';' and depth == 0 and extent in ('statement', 'english'):
            return t.end
    raise SystemExit(f'{where}: region from {start} has no end')


def find_once(text, anchor, where):
    return find_span(text, anchor, where)[0]


def find_span(text, anchor, where):
    """Where an anchor occurs, once; its lines may end in CRLF in the file (some drop files)."""
    for variant in dict.fromkeys((anchor, anchor.replace('\n', '\r\n'))):
        count = text.count(variant)
        if count == 1:
            start = text.index(variant)
            return start, start + len(variant)
        if count > 1:
            break
    raise SystemExit(f'{where}: {anchor!r} occurs {text.count(anchor)} times, expected once')


def call_span(toks, open_index):
    """The tokens of a call's arguments: indexes of '(' and of its ')'."""
    depth = 0
    for k in range(open_index, len(toks)):
        t = toks[k]
        if t.kind == 'op' and t.value in '([{':
            depth += 1
        elif t.kind == 'op' and t.value in ')]}':
            depth -= 1
            if depth == 0:
                return open_index, k
    raise SystemExit('unbalanced call')


def c_literal(value):
    out = ['"']
    for ch in value:
        if ch == '\\':
            out.append('\\\\')
        elif ch == '"':
            out.append('\\"')
        elif ch == '\n':
            out.append('\\n')
        elif ch == '\t':
            out.append('\\t')
        elif ord(ch) < 0x20 or ch == '\x7f':
            raise SystemExit(f'control character in {value!r}')
        else:
            out.append(ch)
    out.append('"')
    return ''.join(out)


# --- the tree --------------------------------------------------------------------------------
def code_files(tree):
    for top in TITLE_DIRS:
        root = tree / top
        if not root.exists():
            continue
        for path in sorted(root.rglob('*')):
            rel = path.relative_to(tree).as_posix()
            if path.is_file() and path.suffix in CODE_SUFFIXES and not rel.startswith(SKIP_PREFIXES):
                yield rel, path


def git_env(home):
    env = dict(os.environ, GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL=str(home / 'gitconfig'),
               GIT_CEILING_DIRECTORIES=str(home.parent), HOME=str(home), LC_ALL='C')
    (home / 'gitconfig').write_text('')
    return env


GIT = ['git', '-c', 'core.autocrlf=false', '-c', 'core.safecrlf=false', '-c', 'core.filemode=false',
       '-c', 'core.quotepath=true', '-c', 'diff.noprefix=false', '-c', 'diff.mnemonicPrefix=false',
       '-c', 'color.ui=false']


def fix_order(patch):
    """A fix's place in the order the fixes apply: by number, then letter (08, 08b, 09, ..., 99,
    100). Name order alone would put 100-... before 11-...; names without a number go last."""
    match = re.match(r'(\d+)([a-z]*)-', patch.name)
    return (int(match.group(1)), match.group(2), patch.name) if match else (1 << 30, '', patch.name)


def later_fixes():
    """The fixes assemble.py applies after fix 25 (in number order, as its fix_order)."""
    return [path for path in sorted(FIX.parent.glob('*.patch'), key=fix_order) if fix_order(path) > fix_order(FIX)]


def before_tree(tree, work):
    """A copy of the tree's code files with fix 25 reversed when it is applied."""
    before = work / 'before'
    for rel, path in code_files(tree):
        dest = before / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, dest)
    env = git_env(work)
    # Fixes after fix 25 (assemble.py applies fixes/*.patch in name order) are written against
    # English keys; they are reversed first, newest first, in the title files copied here (their
    # core and other paths are left out), so that fix 25 is regenerated from the tree it was
    # made for and later fixes do not end up in it.
    copied = {rel for rel, _ in code_files(before)}
    included = [f'--include={rel}' for rel in sorted(copied)]
    for later in reversed(later_fixes()):
        touched = set(re.findall(r'^diff --git a/(\S+) b/', later.read_text(encoding='utf-8', errors='replace'), re.M))
        if not touched & copied:
            continue
        def apply(*options):
            return subprocess.run(GIT + ['apply', *options, '--whitespace=nowarn', *included, str(later)],
                                  cwd=before, env=env, capture_output=True, text=True)
        if apply('-R', '--check').returncode == 0:
            apply('-R').check_returncode()
        elif apply('--check').returncode:
            raise SystemExit(f'{tree}: fix {later.name} (after fix 25) neither reverses nor applies: '
                             f'{apply("-R", "--check").stderr}assemble the tree again with every fix')
    if FIX.exists():
        reverse = subprocess.run(GIT + ['apply', '-R', '--check', str(FIX)], cwd=before, env=env,
                                 capture_output=True)
        if reverse.returncode == 0:
            subprocess.run(GIT + ['apply', '-R', '--whitespace=nowarn', str(FIX)], cwd=before, env=env, check=True)
            return before, True
    if 'struct Entry { const char* pt; const char* en; const char* es; };' not in (before / I18N).read_text(encoding='utf-8'):
        raise SystemExit(f'{tree}: Tr has no Portuguese table, but fix 25 as committed does not reverse: the tree '
                         'has another fix 25 (assemble it again, without fix 25 if it no longer applies)')
    return before, False


def read_tree(root):
    return {rel: path.read_bytes().decode('utf-8') for rel, path in code_files(root)}


# --- what the before tree translates ---------------------------------------------------------
class Before:
    """Translation data of a tree with fixes up to 24: built-in tables and every key literal."""

    def __init__(self, files):
        self.files = files
        self.lexed = {rel: literals(text, rel) for rel, text in files.items()}
        self.builtin = self._builtin()
        self.framegen = self._framegen()
        self.core = self._core()

    def _table(self, rel, anchor, width):
        text = self.files[rel]
        start = find_once(text, anchor, rel)
        toks, lits = self.lexed[rel]
        end = region_end(toks, text, start, 'block', rel)
        rows = []
        inside = {lit.index: lit for lit in lits if start <= lit.start < end}
        # Rows: '{' literal ',' literal ',' literal '}' (each literal may be several, adjacent).
        idx = [k for k, t in enumerate(toks) if start <= t.start < end]
        k = 0
        while k < len(idx):
            t = toks[idx[k]]
            if t.kind == 'op' and t.value == '{':
                row, j = [], idx[k] + 1
                while True:
                    if toks[j].kind == 'str':
                        lit = inside[j]
                        row.append(lit.value)
                        j = lit.last + 1
                    elif toks[j].kind == 'op' and toks[j].value == ',':
                        j += 1
                    else:
                        break
                if len(row) == width and toks[j].value == '}':
                    rows.append(tuple(row))
            k += 1
        return rows, (start, end)

    def _builtin(self):
        rows, self.builtin_span = self._table(I18N, 'inline const char* Tr(const char* key) {', 3)
        table = {}
        for pt, en, es in rows:
            table.setdefault(pt, (en, es))  # Tr's linear search: the first row wins
        self.builtin_rows = rows
        return table

    def _core(self):
        rows, self.core_span = self._table(GAME_MAIN, 'static const char* CoreUiText(const char* english) {', 3)
        return [{'core': en, 'pt': pt, 'en': en, 'es': es} for en, pt, es in rows]

    def _framegen(self):
        sites = []
        for rel, (toks, lits) in self.lexed.items():
            for k, t in enumerate(toks):
                if not (t.kind == 'id' and t.value == 'FramegenText' and toks[k + 1].value == '('):
                    continue
                open_, close = call_span(toks, k + 1)
                args = [lit for lit in lits if open_ < lit.index < close]
                if toks[k - 1].kind == 'op' and toks[k - 1].value == '*' or \
                        toks[k - 1].kind == 'id' and toks[k - 1].value == 'char':
                    continue  # the declaration and the definition
                shape = [toks[j].kind if toks[j].kind != 'op' else toks[j].value for j in range(open_ + 1, close)]
                values = []
                j = open_ + 1
                while j < close:
                    lit = next((l for l in args if l.index == j), None)
                    if lit is None:
                        raise SystemExit(f'{rel}: FramegenText with an argument that is not a literal: {shape}')
                    values.append(lit.value)
                    j = lit.last + 1
                    if j < close:
                        assert toks[j].value == ',', (rel, shape)
                        j += 1
                assert len(values) == 3, (rel, values)
                sites.append({'file': rel, 'start': toks[k].start, 'end': toks[close].end,
                              'pt': values[0], 'en': values[1], 'es': values[2]})
        return sites

    def regions(self, rel):
        out = []
        for file, anchor, extent, why in CONTEXTS:
            if file != rel:
                continue
            text = self.files[rel]
            start = find_once(text, anchor, rel)
            out.append((start, region_end(self.lexed[rel][0], text, start, extent, rel), extent, why))
        return out


def classify(before, keys):
    """Every literal whose value is a Portuguese key: ('tr' | 'region' | 'english' | 'framegen' |
    'builtin' | 'core' | None) by where it is."""
    found = []
    for rel, (toks, lits) in before.lexed.items():
        regions = before.regions(rel)
        for lit in lits:
            if lit.raw or lit.value not in keys:
                continue
            context = None
            if rel == I18N and before.builtin_span[0] <= lit.start < before.builtin_span[1]:
                context = 'builtin'
            elif rel == GAME_MAIN and before.core_span[0] <= lit.start < before.core_span[1]:
                context = 'core'
            elif lit.call and lit.call[0] == 'FramegenText':
                context = 'framegen'
            elif lit.call and lit.call[0] == 'Tr':
                context = 'tr'
            else:
                for start, end, extent, why in regions:
                    if start <= lit.start < end:
                        context = 'english' if extent == 'english' else 'region'
            if context == 'region' and lit.value in NATIVE_NAMES:
                context = 'native'
            lit.context = context
            found.append((rel, lit))
    return found


# --- extra.txt ---------------------------------------------------------------------------------
def load_extra(path=EXTRA):
    """Sections: [english] Portuguese = English; [keys] Portuguese = English key (context keys);
    [<code>] Portuguese = translation."""
    sections, current = {}, None
    for number, line in enumerate(path.read_text(encoding='utf-8').split('\n'), 1):
        if not line or line.startswith('#'):
            continue
        m = re.fullmatch(r'\[((?:added )?[A-Za-z0-9-]+)\]', line)
        if m:
            current = m.group(1)
            sections.setdefault(current, {})
            continue
        split = line.find(' = ')
        if current is None or split < 0:
            raise SystemExit(f'{path.name}:{number}: not "key = value" in a section')
        key = unescape(line[:split])
        if key in sections[current]:
            raise SystemExit(f'{path.name}:{number}: {key!r} twice in [{current}]')
        sections[current][key] = unescape(line[split + 3:])
    return sections


# --- the map Portuguese -> English -------------------------------------------------------------
def strip_context(key):
    return key.split(CONTEXT_MARK, 1)[0]


class Rekey:
    def __init__(self, before, official, extra):
        # [added <code>]: translations of English keys that fixes after fix 25 add (no Portuguese).
        extra = dict(extra)
        self.added = {name.split(' ', 1)[1]: extra.pop(name) for name in list(extra) if name.startswith('added ')}
        self.before, self.official, self.extra = before, official, extra
        keys = set()
        for table in official.values():
            keys |= set(table)
        keys |= set(before.builtin)
        keys |= {site['pt'] for site in before.framegen}
        keys |= {row['pt'] for row in before.core}
        for section in extra.values():
            keys |= set(section)
        self.candidates = keys
        self.found = classify(before, keys)
        unknown = [(rel, lit) for rel, lit in self.found if lit.context is None
                   and (rel, lit.value) not in {(f, v) for f, v, _ in NOT_KEYS}]
        if unknown:
            for rel, lit in unknown:
                line = before.files[rel].count('\n', 0, lit.start) + 1
                print(f'  {rel}:{line}: {lit.value!r}', file=sys.stderr)
            raise SystemExit(f'{len(unknown)} Portuguese key literals outside any translation context '
                             '(add a CONTEXTS region or a NOT_KEYS entry)')
        # The keys the code uses, by how they are looked up.
        self.used = {}
        for rel, lit in self.found:
            if lit.context in ('tr', 'region', 'english', 'native'):
                self.used.setdefault(lit.value, set()).add('tr')
        for site in before.framegen:
            self.used.setdefault(site['pt'], set()).add('framegen')
        for row in before.core:
            self.used.setdefault(row['pt'], set()).add('core')
        self.english = {}
        self.source_of = {}
        missing = []
        for pt in sorted(self.used):
            text, source = self.english_text(pt)
            if text is None:
                missing.append(pt)
            self.english[pt], self.source_of[pt] = text, source
        if missing:
            raise SystemExit('Portuguese keys with no English (add them to [english] in extra.txt):\n' +
                             '\n'.join(f'  {escape(pt)} = ' for pt in missing))
        self.key = {}
        for pt in self.used:
            if pt in extra.get('keys', {}):
                self.key[pt] = extra['keys'][pt]
                if strip_context(self.key[pt]) != self.english[pt]:
                    raise SystemExit(f'[keys] {pt!r}: {self.key[pt]!r} is not the English {self.english[pt]!r}')
            elif pt in NATIVE_NAMES:
                self.key[pt] = pt
            else:
                self.key[pt] = self.english[pt]
            if CONTEXT_MARK in self.key[pt] and pt not in extra.get('keys', {}):
                raise SystemExit(f'{pt!r}: English with {CONTEXT_MARK!r}')
        # CoreUiText rows whose English (en.txt, else the row's) is not their key's English: the
        # row gets its own key ([core] of extra.txt: "<the core's English> = <key>").
        self.core_key = {}
        for row in before.core:
            shown = official['en'].get(row['pt'], row['en'])
            key = extra.get('core', {}).get(row['core'], self.key[row['pt']])
            if strip_context(key) != shown:
                raise SystemExit(f'CoreUiText {row["core"]!r} shows {shown!r} in English but its key '
                                 f'{key!r} would show {strip_context(key)!r}: give it a key in [core]')
            self.core_key[row['core']] = key
        for site in before.framegen:
            shown = official['en'].get(site['pt'], site['en'])
            if shown != self.english[site['pt']]:
                raise SystemExit(f'FramegenText {site["pt"]!r} shows {shown!r} in English, its key '
                                 f'{self.key[site["pt"]]!r}: the call needs a key of its own')
        unused = [pt for name, section in extra.items() if name != 'core' for pt in section if pt not in self.used]
        unused += [core for core in extra.get('core', {}) if core not in {row['core'] for row in before.core}]
        if unused:
            raise SystemExit(f'extra.txt keys the code does not use: {sorted(set(unused))}')
        # Every (Portuguese key, English key, English text, Spanish) the language files hold.
        self.entries = [(pt, key, self.english[pt], self.spanish(pt)) for pt, key in self.key.items()]
        for row in before.core:
            if self.core_key[row['core']] != self.key[row['pt']]:
                self.entries.append((row['pt'], self.core_key[row['core']],
                                     official['en'].get(row['pt'], row['en']),
                                     official['es'].get(row['pt'], row['es'])))
        self.collisions = self._collisions()
        self.tables = self._tables()
        # (A key RENAMES renames: its renamed text under its renamed key.)
        for site in before.framegen:
            key = self.key[site['pt']]
            shown = renamed_text(key, official['es'].get(site['pt'], site['es']))
            if shown != self.tables['es'].get(renamed_key(key)):
                raise SystemExit(f'FramegenText {site["pt"]!r} shows {shown!r} in Spanish, its key '
                                 f'{self.tables["es"].get(renamed_key(key))!r}')
        for row in before.core:
            key = self.core_key[row['core']]
            shown = renamed_text(key, official['es'].get(row['pt'], row['es']))
            if shown != self.tables['es'].get(renamed_key(key)):
                raise SystemExit(f'CoreUiText {row["core"]!r} shows {shown!r} in Spanish, its key '
                                 f'{self.tables["es"].get(self.core_key[row["core"]])!r}')

    def english_text(self, pt):
        if pt in self.official['en']:
            return self.official['en'][pt], 'en.txt'
        if pt in self.before.builtin:
            return self.before.builtin[pt][0], 'Tr table'
        sites = [site for site in self.before.framegen if site['pt'] == pt]
        if sites:
            return sites[0]['en'], 'FramegenText'
        rows = [row for row in self.before.core if row['pt'] == pt]
        if rows:
            return rows[0]['en'], 'CoreUiText'
        if pt in self.extra.get('english', {}):
            return self.extra['english'][pt], 'extra.txt'
        if pt in NATIVE_NAMES:
            return pt, 'native name'
        return None, None

    def spanish(self, pt):
        """Spanish (Spain) of a key: es.txt, then the built-in Spanish of Tr, FramegenText,
        CoreUiText, then extra.txt."""
        if pt in self.official['es']:
            return self.official['es'][pt]
        if pt in self.before.builtin:
            return self.before.builtin[pt][1]
        for site in self.before.framegen:
            if site['pt'] == pt:
                return site['es']
        for row in self.before.core:
            if row['pt'] == pt:
                return row['es']
        return self.extra.get('es', {}).get(pt)

    def _collisions(self):
        """Portuguese keys that would share an English key. Every one is a collision: their
        Brazilian Portuguese (the keys themselves) differs, and so may the other languages'."""
        by_key = {}
        for pt, key, _, _ in self.entries:
            by_key.setdefault(key, set()).add(pt)
        clashes = {key: sorted(pts) for key, pts in by_key.items() if len(pts) > 1}
        if clashes:
            lines = []
            for key, pts in sorted(clashes.items()):
                differ = [code for code in OFFICIAL_CODES if len({self.official[code].get(p) for p in pts}) > 1]
                lines.append(f'  {key!r} <- {pts}; translations differ in pt-BR {" ".join(differ)}')
            raise SystemExit('collisions (give each Portuguese key its own context key in [keys] of '
                             'extra.txt):\n' + '\n'.join(lines))
        remaining = {pt for pt, key in self.key.items() if key != pt}
        overlap = sorted({key for _, key, _, _ in self.entries} & remaining)
        if overlap:
            raise SystemExit(f'English keys equal to other Portuguese keys: {overlap}')
        # Context keys in use, for the report.
        groups = {}
        for pt, key in self.key.items():
            if CONTEXT_MARK in key or any(other != pt and strip_context(k) == strip_context(key)
                                          for other, k in self.key.items()):
                groups.setdefault(strip_context(key), []).append((pt, key))
        return {text: sorted(rows, key=lambda r: r[1]) for text, rows in sorted(groups.items())}

    def _tables(self):
        """The English-keyed language files."""
        tables = {code: {} for code in LANGUAGES.values()}
        extra = self.extra
        for code in extra:
            if code in ('en', 'pt-BR'):
                raise SystemExit(f'extra.txt [{code}]: English is [english], Brazilian Portuguese the key')
        for pt, key, english, spanish in self.entries:
            tables['en'][key] = english
            tables['pt-BR'][key] = pt
            if spanish is not None:
                tables['es'][key] = spanish
            for code in FULL_CODES:
                text = self.official[code].get(pt, extra.get(code, {}).get(pt))
                if text is not None:
                    tables[code][key] = text
        # Keys a later fix renames (RENAMES): the key and every language's text.
        keys = set(self.key.values()) | set(self.core_key.values())
        for key, old, new, fix in RENAMES:
            if key not in keys:
                raise SystemExit(f'RENAMES: {key!r} (fix {fix}) is no key of fix 25')
            if old not in key:
                raise SystemExit(f'RENAMES: {key!r} does not contain {old!r}')
            if renamed_key(key) in keys:
                raise SystemExit(f'RENAMES: {renamed_key(key)!r} (fix {fix}) is a key of fix 25 already')
            for code, table in tables.items():
                if key not in table:
                    continue
                text = table.pop(key)
                if old not in text:
                    raise SystemExit(f'RENAMES: the {code} text of {key!r} lacks {old!r}: {text!r}')
                table[renamed_key(key)] = renamed_text(key, text)
        for code, section in sorted(self.added.items()):
            if code not in tables or code == 'en':
                raise SystemExit(f'extra.txt [added {code}]: no such language file (English is the key)')
            for key, text in section.items():
                if key in self.key.values() or key in self.core_key.values():
                    raise SystemExit(f'extra.txt [added {code}]: {key!r} is a key of fix 25 (translate it there)')
                tables[code][key] = text
                tables['en'][key] = strip_context(key)
        return tables


# --- the rewrite (fix 25) ----------------------------------------------------------------------
NEW_TR = '''// The interface text of an English key in the chosen language (Brazilian Portuguese is a
// translation like the others, pt-BR.txt): the language's file (/app0/assets/lang/<code>.txt,
// LoadUiTranslations), then what it lacks from a related file: Portuguese (Portugal) from
// pt-BR.txt, Spanish (Latin America) from es.txt; then English (en.txt, else the key itself
// without its "||context"). A text that is no key (a game's name) is shown as it is.
inline const char* Tr(const char* key) {
  const int language = ui_language.load(std::memory_order_relaxed);
  if (const char* loaded = LoadedUiText(key)) return loaded;
  if (language == 19) {
    if (const char* brazilian = FallbackUiText(9, key)) return brazilian;
  } else if (language == 18) {
    if (const char* spanish = FallbackUiText(5, key)) return spanish;
  }
  if (language != 1) {
    if (const char* english = FallbackUiText(1, key)) return english;
  }
  return SourceUiText(key);
}'''

I18N_UI_EDITS = [
    ('// and makes Tr() look in the loaded file first. File format, as in the official package:\n'
     '// "<Portuguese text> = <translation>" per line, "\\s" for a space, "\\n" a new line, "#" comments.\n',
     '// and makes Tr() look in the loaded file first. File format, as in the official package:\n'
     '// "<key> = <translation>" per line, "\\s" for a space, "\\n" a new line, "#" comments. Fix 25\n'
     '// (tools/source-1.0.2/fixes) keys them by English: the official package\'s files are keyed by\n'
     '// Brazilian Portuguese, so this title needs the files of tools/source-1.0.2/english/lang.\n'),
    ('#include <map>\n#include <memory>\n#include <vector>\n#include <mutex>\n#include <string>\n',
     '#include <map>\n#include <memory>\n#include <vector>\n#include <mutex>\n#include <set>\n#include <string>\n'),
    ('''// Loads /app0/assets/lang/<code>.txt for the language (Brazilian Portuguese is the source).
inline void LoadUiTranslations(int id) {
  auto table = std::make_shared<i18n_ui::Table>();
  table->language = id;
  if (id != 9) i18n_ui::Read(id, *table);
  std::lock_guard<std::mutex> guard(i18n_ui::Lock());
  i18n_ui::Current() = std::move(table);
}
// Fix 23 (tools/source-1.0.2/fixes): the English translation of a Portuguese text from
// /app0/assets/lang/en.txt (read once), or nullptr. A text the chosen language's file lacks is
// shown in English, not in the Portuguese source.
inline const char* EnglishUiText(const char* pt) {
  static const i18n_ui::Table english = [] {
    i18n_ui::Table table;
    table.language = 1;
    i18n_ui::Read(1, table);
    return table;
  }();
  const auto found = english.text.find(pt);
  return found == english.text.end() ? nullptr : found->second.c_str();
}
// The loaded translation of a Portuguese text, or nullptr. Tables stay alive for the
// process, so the returned text remains valid.
inline const char* LoadedUiText(const char* pt) {''',
     '''// Loads /app0/assets/lang/<code>.txt for the language (every one, English and Brazilian
// Portuguese included: the keys are English, fix 25). Settings::Apply calls this on every
// settings change; the file is read again only when the language (or the folder) changed, since
// LoadedUiText keeps each table it gave text from for the process.
inline void LoadUiTranslations(int id) {
  {
    std::lock_guard<std::mutex> guard(i18n_ui::Lock());
    const auto& current = i18n_ui::Current();
    if (current->language == id && current->folder == i18n_ui::Folder()) return;
  }
  auto table = std::make_shared<i18n_ui::Table>();
  table->language = id;
  table->folder = i18n_ui::Folder();
  i18n_ui::Read(id, *table);
  std::lock_guard<std::mutex> guard(i18n_ui::Lock());
  i18n_ui::Current() = std::move(table);
}
// The text of a key in a fallback language's file, or nullptr when the file lacks it: English
// (en.txt; fix 23 for every language), Brazilian Portuguese (pt-BR.txt, for Portugal's) or
// Spanish (es.txt, for Latin America's). Each file is read once, when first needed.
inline const char* FallbackUiText(int id, const char* key) {
  const auto read_file = [](int language) {
    i18n_ui::Table table;
    table.language = language;
    i18n_ui::Read(language, table);
    return table;
  };
  const i18n_ui::Table* table;
  if (id == 9) {
    static const i18n_ui::Table brazilian = read_file(9);
    table = &brazilian;
  } else if (id == 5) {
    static const i18n_ui::Table spanish = read_file(5);
    table = &spanish;
  } else {
    static const i18n_ui::Table english = read_file(1);
    table = &english;
  }
  const auto found = table->text.find(key);
  return found == table->text.end() ? nullptr : found->second.c_str();
}
// Fix 23's name for the English file's text of a key.
inline const char* EnglishUiText(const char* key) { return FallbackUiText(1, key); }
// The English of a key no file has: the key, without the "||context" that tells apart keys
// whose English is the same but whose translations are not ("On||feminine"). The text is kept
// for the process.
inline const char* SourceUiText(const char* key) {
  const char* mark = std::strstr(key, "||");
  if (!mark) return key;
  static std::mutex lock;
  static std::set<std::string> texts;
  std::lock_guard<std::mutex> guard(lock);
  return texts.emplace(key, size_t(mark - key)).first->c_str();
}
// The loaded translation of a key, or nullptr. Tables stay alive for the process, so the
// returned text remains valid.
inline const char* LoadedUiText(const char* key) {'''),
    ('  const auto found = table->text.find(pt);\n  if (found == table->text.end()) return nullptr;\n',
     '  const auto found = table->text.find(key);\n  if (found == table->text.end()) return nullptr;\n'),
    ('#include <atomic>\n#include <fstream>\n', '#include <atomic>\n#include <cstring>\n#include <fstream>\n'),
    ('struct Table {\n  int language = -1;\n  std::map<std::string, std::string> text;\n};\n',
     'struct Table {\n  int language = -1;\n  std::string folder;  // Folder() when it was read (LoadUiTranslations)\n'
     '  std::map<std::string, std::string> text;\n};\n'),
]

I18N_EDITS = [
    ('// Xbox language IDs. UI translations currently cover English, Portuguese and Spanish.\n',
     '// Xbox language IDs. The interface is keyed by English (fix 25) and translated by the language\n'
     '// files (reconstructed/xbox360ps5/i18n_ui.hpp).\n'),
]

# Comments that quote Portuguese interface text, and Portuguese that is not a key: (file, old,
# new). "{{<Portuguese key>}}" in the new text is that key's English (so comments follow the map).
TEXT_EDITS = [
    ('src/launcher.cpp', '// "Continuar", and choosing another game', '// "{{Continuar}}", and choosing another game'),
    ('src/launcher.cpp', '// How far the game sheet\'s "Sobre" page', '// How far the game sheet\'s "{{Sobre}}" page'),
    ('src/launcher.cpp', '// The game sheet\'s "Sobre" page.', '// The game sheet\'s "{{Sobre}}" page.'),
    ('src/launcher.cpp', 'FSI per game. Padrão keeps', 'FSI per game. {{Padrão}} keeps'),
    ('src/canary_game_main.cpp', 'option (1, Padrão: its preset,', 'option (1, {{Padrão}}: its preset,'),
    ('src/canary_game_main.cpp', '("Ver a biblioteca" in the guide)',
     '("{{Ver a biblioteca (o jogo continua aberto)}}" in the guide)'),
    ('src/canary_game_main.cpp', '// Its case says "Continuar";', '// Its case says "{{Continuar}}";'),
    ('src/canary_game_main.cpp', '// "Fechar o emulador": the system', '// "{{Fechar o emulador}}": the system'),
    ('port/game_art.hpp', 'the home\'s "Jogar ..." slot', 'the home\'s "{{Jogar}} ..." slot'),
    ('port/kinect_home.inc', 'and "Jogar ..." at the bottom', 'and "{{Jogar}} ..." at the bottom'),
    ('port/kinect_state.inc', 'the disc mark, "Jogar ...")', 'the disc mark, "{{Jogar}} ...")'),
    ('port/guide_network.hpp', '// The guide\'s "Rede" row', '// The guide\'s "{{Rede}}" row'),
    ('reconstructed/dark_theme.hpp',
     '//    "Tema" slot (after System Link) says "Tema claro/escuro ligado..." or "Não foi possível\n'
     '//    trocar o tema."; the Settings sheet\'s "Tema" row (before "Reiniciar o emulador") saves the\n',
     '//    "{{Tema}}" slot (after System Link) says "{{Tema claro ligado. O emulador vai reiniciar.}}" (or\n'
     '//    dark) or "{{Não foi possível trocar o tema.}}"; the Settings sheet\'s "{{Tema}}" row (before\n'
     '//    "{{Reiniciar o emulador}}") saves the\n'),
    ('reconstructed/dark_theme.hpp', '// The texts are the official ones, translated by the package\'s assets/lang files.',
     '// The texts are the official ones, keyed by English (fix 25) and translated by the language files.'),
    ('reconstructed/dark_theme.hpp', '(Portuguese source text, for Tr)', '(English keys, for Tr)'),
    ('include/xbox360ps5/launcher.hpp', '// Portuguese; Tr() gives the other languages.',
     '// English key; Tr() gives the language.'),
    ('reconstructed/xbox360ps5/game_info.hpp',
     '// The Portuguese text (translated by Tr) of a Xenia Canary compatibility state; the official\n'
     '// package\'s assets/lang files translate exactly these.',
     '// The English key (translated by Tr) of a Xenia Canary compatibility state; the official\n'
     '// package\'s assets/lang files translate exactly these (keyed by Portuguese there).'),
    ('include/xbox360ps5/frame_watch.hpp',
     '(its text gains "compiling shaders or loading" through\n'
     '  // the package\'s assets/lang files).',
     '(its English key says "compiling shaders or\n'
     '  // loading", and so do the language files).'),
    ('src/native_game_main.cpp', '"Desempenho: media {:.1f} quadros/s, minimo {:.0f} nos ultimos 30 s"',
     '"Performance: average {:.1f} frames/s, lowest {:.0f} in the last 30 s"'),
]


def fill(text, rekey):
    return re.sub(r'\{\{(.+?)\}\}', lambda m: strip_context(rekey.key[m.group(1)]), text)


def line_of(text, pos):
    return text.count('\n', 0, pos) + 1


def splice(text, edits, where):
    edits = sorted(edits)
    for (s1, e1, _), (s2, e2, _) in zip(edits, edits[1:]):
        if s2 < e1:
            raise SystemExit(f'{where}: overlapping edits at lines {line_of(text, s1)} and {line_of(text, s2)}')
    for start, end, new in reversed(edits):
        # New lines end as the line the edit starts in does (some drop files have CRLF).
        eol = text.find('\n', start)
        if eol > 0 and text[eol - 1] == '\r':
            new = new.replace('\r\n', '\n').replace('\n', '\r\n')
        text = text[:start] + new + text[end:]
    return text


def comment_edits(text, rel, old, new, newline):
    # TEXT_EDITS may be comments whose quoted words also appear elsewhere: they are matched in
    # the comments only when the plain text is not unique. Lines end as the file's do (CRLF in
    # some drop files).
    old = old.replace('\n', newline)
    if text.count(old) == 1:
        start = text.index(old)
        return [(start, start + len(old), new.replace('\n', newline))]
    toks = lex(text, rel)
    hits = [(t.start + t.value.index(old)) for t in toks if t.kind == 'comment' and old in t.value]
    if len(hits) != 1:
        raise SystemExit(f'{rel}: {old!r} occurs {text.count(old)} times ({len(hits)} in comments)')
    return [(hits[0], hits[0] + len(old), new.replace('\n', newline))]


def rewrite(rekey):
    """The after tree's changed files {rel: text} and the replacement counts per file."""
    before = rekey.before
    edits = {rel: [] for rel in before.files}
    counts = {}

    def count(rel, kind):
        counts.setdefault(rel, {}).setdefault(kind, 0)
        counts[rel][kind] += 1

    for rel, lit in rekey.found:
        if lit.context in ('tr', 'region', 'english'):
            key = rekey.key[lit.value]
            if key != lit.value:
                edits[rel].append((lit.start, lit.end, c_literal(key)))
                count(rel, f'{lit.context} literal')
    for site in before.framegen:
        edits[site['file']].append((site['start'], site['end'], 'Tr(' + c_literal(rekey.key[site['pt']]) + ')'))
        count(site['file'], 'FramegenText -> Tr')
    # FramegenText's declarations and definition.
    text = before.files[LAUNCHER]
    decl = 'const char* FramegenText(const char* pt, const char* en, const char* es);\n'
    if text.replace('\r\n', '\n').count(decl) != 2:
        raise SystemExit(f'{LAUNCHER}: FramegenText declared {text.count(decl)} times, expected 2')
    for old in ('// Kept outside Settings to preserve the shipped title and launcher ABI.\n'
                '// The presenter reads the same file once, before creating its swapchain.\n' + decl,
                '// the DLCs tab\'s list, defined with RefreshDlcs\n' + decl):
        s, e = find_span(text, old, LAUNCHER)
        edits[LAUNCHER].append((s + text[s:e].index('const char* FramegenText') if old.startswith('// the DLCs') else s,
                                e, ''))
    start = find_once(text, 'const char* FramegenText(const char* pt, const char* en, const char* es) {', LAUNCHER)
    end = region_end(before.lexed[LAUNCHER][0], text, start, 'block', LAUNCHER)
    edits[LAUNCHER].append((start, text.index('\n', end) + 1, ''))
    count(LAUNCHER, 'FramegenText removed')
    # CoreUiText: the core's English -> the interface's English key.
    text = before.files[GAME_MAIN]
    start = find_once(text, 'static const char* CoreUiText(const char* english) {', GAME_MAIN)
    end = region_end(before.lexed[GAME_MAIN][0], text, start, 'block', GAME_MAIN)
    rows = []
    for row in before.core:
        rows.append(f'    {{{c_literal(row["core"])}, {c_literal(rekey.core_key[row["core"]])}}},')
    core = ('static const char* CoreUiText(const char* english) {\n'
            '  // The core\'s own English (xam_ui.cc) and the interface\'s key for it.\n'
            '  struct Entry { const char* core; const char* key; };\n'
            '  static constexpr Entry entries[] = {\n' + '\n'.join(rows) + '\n  };\n'
            '  for (const auto& entry : entries) {\n'
            '    if (!std::strcmp(entry.core, english)) return xbox360ps5::Tr(entry.key);\n'
            '  }\n'
            '  return english;\n'
            '}')
    edits[GAME_MAIN].append((start, end, core))
    count(GAME_MAIN, 'CoreUiText table keyed by English')
    # Tr: the built-in table goes (its Portuguese and Spanish are in the language files).
    text = before.files[I18N]
    start, end = before.builtin_span
    edits[I18N].append((start, end, NEW_TR))
    count(I18N, 'built-in table removed')
    for old, new in I18N_EDITS:
        edits[I18N].append((*find_span(text, old, I18N), new))
    text = before.files[I18N_UI]
    for old, new in I18N_UI_EDITS:
        edits[I18N_UI].append((*find_span(text, old, I18N_UI), new))
    for rel, old, new in TEXT_EDITS:
        text = before.files[rel]
        newline = '\r\n' if '\r\n' in text else '\n'
        edits[rel] += comment_edits(text, rel, old, fill(new, rekey), newline)
        count(rel, 'comment or log text')
    after = {}
    for rel, items in edits.items():
        if items:
            after[rel] = splice(before.files[rel], items, rel)
    return after, counts


def make_patch(before_files, after, work):
    repo = work / 'diff'
    env = git_env(work)
    for rel in after:
        path = repo / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(before_files[rel].encode('utf-8'))
    subprocess.run(GIT + ['init', '-q'], cwd=repo, env=env, check=True)
    subprocess.run(GIT + ['add', '-A'], cwd=repo, env=env, check=True)
    for rel, text in after.items():
        (repo / rel).write_bytes(text.encode('utf-8'))
    diff = subprocess.run(GIT + ['diff', '--no-ext-diff', '--no-textconv', '--diff-algorithm=myers', '-U3',
                                 '--src-prefix=a/', '--dst-prefix=b/', '--no-renames'],
                          cwd=repo, env=env, check=True, capture_output=True).stdout
    return diff


LANG_HEADER = {
    'en': ['English, the source: each key is its English text ("||" and what follows tell apart',
           'keys whose English is the same but whose translations are not).'],
    'pt-BR': ['Portuguese (Brazil): the text of the official 1.0.2 title, whose keys it was.'],
}


def lang_files(rekey):
    files = {}
    for code in sorted(LANGUAGES.values()):
        header = [f'PS5X360 interface text: {code}. Keys are English (fix 25); generated by',
                  'tools/source-1.0.2/english/rekey.py from the official 1.0.2 package\'s files (keyed by',
                  'Brazilian Portuguese), the fork\'s overrides and english/extra.txt. Do not edit by hand.']
        if code in LANG_HEADER:
            header += LANG_HEADER[code]
        files[f'{code}.txt'] = format_lang(header, rekey.tables[code]).encode('utf-8')
    return files


def generate(tree, work, official_dir=OFFICIAL_DIR):
    before_dir, reversed_fix = before_tree(tree, work)
    before = Before(read_tree(before_dir))
    rekey = Rekey(before, load_official(official_dir), load_extra())
    after, counts = rewrite(rekey)
    patch = make_patch(before.files, after, work)
    return rekey, after, counts, patch, lang_files(rekey), reversed_fix


def report(rekey, counts, out=sys.stdout):
    print('replacements per file:', file=out)
    for rel in sorted(counts):
        print(f'  {rel}: ' + ', '.join(f'{kind} {n}' for kind, n in sorted(counts[rel].items())), file=out)
    print('context keys (collisions resolved):', file=out)
    for text, rows in rekey.collisions.items():
        print(f'  {text!r}: ' + ', '.join(f'{pt} -> {key!r}' for pt, key in rows), file=out)
    sources = {}
    for pt, source in rekey.source_of.items():
        sources[source] = sources.get(source, 0) + 1
    print(f'{len(rekey.key)} keys; English from: ' +
          ', '.join(f'{source} {n}' for source, n in sorted(sources.items())), file=out)
    print('language files: ' + ', '.join(f'{code} {len(t)}' for code, t in sorted(rekey.tables.items())), file=out)


def compare(label, want, path, problems):
    have = path.read_bytes() if path.exists() else None
    if have != want:
        problems.append(f'{label}: {path.relative_to(REPO).as_posix()} is not what rekey.py makes')


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--tree', type=Path, required=True, help='assembled 1.0.2 tree (fixes up to 24, or 25)')
    parser.add_argument('--zip', type=Path, help='official PPSA50011 v1.0.2 zip: rebuild official-lang/')
    parser.add_argument('--overrides', type=Path, help='the fork\'s assets/lang-overrides (with --zip)')
    parser.add_argument('--check', action='store_true', help='fail if a committed output is out of date')
    args = parser.parse_args()
    sys.stdout.reconfigure(encoding='utf-8')
    sys.stderr.reconfigure(encoding='utf-8')
    if bool(args.zip) != bool(args.overrides):
        parser.error('--zip and --overrides go together')
    with tempfile.TemporaryDirectory(prefix='rekey-') as temp:
        work = Path(temp)
        problems = []
        official_dir = OFFICIAL_DIR
        if args.zip:
            snapshot = official_snapshot(args.zip, args.overrides)
            official_dir = work / 'official-lang'
            official_dir.mkdir()
            for name, data in snapshot.items():
                (official_dir / name).write_bytes(data)
                if args.check:
                    compare('official-lang', data, OFFICIAL_DIR / name, problems)
        rekey, after, counts, patch, langs, reversed_fix = generate(args.tree.resolve(), work, official_dir)
        print(f'tree: {args.tree} (fix 25 {"reversed in a copy" if reversed_fix else "not applied"})')
        report(rekey, counts)
        if args.check:
            check_snapshot(OFFICIAL_DIR, problems)
            compare('patch', patch, FIX, problems)
            for name, data in langs.items():
                compare('lang', data, LANG_DIR / name, problems)
            stale = sorted(p.name for p in LANG_DIR.glob('*.txt') if p.name not in langs)
            problems += [f'lang: english/lang/{name} is not made by rekey.py' for name in stale]
            if problems:
                print('\n'.join(problems), file=sys.stderr)
                raise SystemExit('rekey.py --check: outputs out of date (run rekey.py without --check)')
            print('rekey.py --check: fix 25 and english/lang are up to date; official-lang is ' +
                  ('what the package and the overrides make' if args.zip else
                   'the snapshot SOURCES.json records (the package itself not read: no --zip)'))
            return
        if args.zip:
            OFFICIAL_DIR.mkdir(exist_ok=True)
            for name in os.listdir(official_dir):
                shutil.copyfile(official_dir / name, OFFICIAL_DIR / name)
        FIX.write_bytes(patch)
        LANG_DIR.mkdir(exist_ok=True)
        for old in LANG_DIR.glob('*.txt'):
            if old.name not in langs:
                old.unlink()
        for name, data in langs.items():
            (LANG_DIR / name).write_bytes(data)
        print(f'wrote {FIX.relative_to(REPO).as_posix()} ({len(patch)} bytes) and english/lang ({len(langs)} files)')


if __name__ == '__main__':
    main()
