#!/usr/bin/env python3
"""public_scan.py: fail on anything in a git repository that must not be public.

What it looks for (each finding has a rule id; `--list-rules` prints them all):
  disc.*      files from or made from the discs: images and executables, game assets, dumps, generated code,
              pictures, binaries, address tables made from the ELFs;
  personal.*  home folders, e-mail addresses, private network addresses, ssh host names;
  secret.*    private keys, tokens, passwords;
  workshop.*  the workshop's own files (agent rules, lane logs, briefs, merge and remote scripts, status pages);
  private.*   whatever a --rules file adds: the machine, host, domain and device names, private-use work. Those
              names are themselves private, so they are never written here: the workshop keeps its rules file.

Modes:
  --tree [REV]      the files of REV (default HEAD; gitlinks skipped), read from git, so CI sees what a push sends;
  --history         every blob reachable from any ref, plus every commit's message and author and committer;
  --added BASE      the lines added since BASE in the working tree (and new files): the home, scratch, machine and
                    secret rules only, a workshop file the path rules only (lanecheck's paths step).

An allow file (--allow, default public_scan_allow.tsv beside this script) lists decisions by path glob:
  GLOB <TAB> RULE (an id, a prefix such as disc.*, or *) <TAB> ok|open <TAB> why
`ok` drops the finding; `open` keeps it as an open question (reported, not failing unless --strict).

Exit: 0 clean, 1 findings, 2 usage. Stdlib only (python3 -I), no network.
"""
import argparse
import fnmatch
import json
import os
import re
import subprocess
import sys
import threading
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))

# (id, scope, regex, why). scope: path (the file's path), content (its text), message (commit text and idents).
# Content and message rules share their regexes; a content rule applies to messages too unless its id says .content.
RULES = [
    # Discs and what is made from them.
    ('disc.image', 'path', r'\.(iso|cue|img|mdf|nrg|gcm|rvz|cso|chd|xiso|wbfs)$', 'a disc image'),
    ('disc.executable', 'path', r'(\.(elf|irx|xbe|dol|rel|erx)$|(^|/)S[CL][UEP][SMA][_-]\d{3}\.?\d{2}(\.\d+)?$)',
     'a game executable or module'),
    ('disc.asset', 'path',
     r'\.(pss|vag|vgs|ads|vb|vh|bd|hd|sq|zit|geo|zgui|tex|tm2|tim2?|pak|ngc|xbx|xpr|xwb|xsb|wmv|xmv|bik|sfd|adx|'
     r'ahx|afs|lev|thp|ps2|mc2|psu|max|sav|xsv|gci|p2s)$',
     'a game asset or save format'),
    ('disc.dump', 'path',
     r'(\.(raw|dump|gsdump|gs|vif|vifhex|rdram|ramdump|state)$|\.vif\.hex$|(^|/)(dumps?|generated(-[\w]+)?|traces|'
     r'extracted|disc-root|disc|irx-dis|reccorpus|baseline|reference)/)',
     'a dump, trace or generated output'),
    ('disc.picture', 'path', r'\.(png|jpe?g|bmp|tga|ppm|pgm|gif|webp|dds|tiff?|heic|mp4|mov|webm|mkv|avi)$',
     'a picture or video: from a game, a capture or another work unless the allow file says where it is from'),
    ('disc.recording', 'path', r'\.(pad|padrec|zrec|rec|replay|lanes\.txt)$', 'a recorded session'),
    ('disc.binary', 'content', None, 'a binary file (an ELF, or NUL bytes) not in the allow file'),
    ('disc.addresses', 'content', None,
     'an address table made from a game ELF (most lines name an EE address): build-time data, or generated'),
    ('disc.readable', 'path', r'(^|/)(engine/(zge|overrides)/|games/(aggressive|dm2|bmx)/(?!tools/|docs/|workspace/))|'
     r'(^|/)(ai/decomp|games/aggressive/workspace/decomp)/(src|include|asm)/',
     'code written from the games\' own code (the readable games, the matching decomp): a legal question, not a leak'),
    ('generated.python', 'path', r'(^|/)__pycache__/|\.py[co]$', 'compiled Python'),
    # Personal and machine facts (generic forms; the specific names come from --rules).
    ('personal.home', 'content', r'(/Users/(?!Shared/|<)[A-Za-z][\w.-]*/|/home/(?!runner/|<)[a-z][\w.-]*/|'
     r'[A-Za-z]:\\Users\\(?!Public\\|<)[\w.-]+)', 'a home folder path'),
    ('personal.drive', 'content', r'(?<![\w%$])[E-Z]:(\\{1,2}|/)[\w.$-]',
     'a folder on one machine\'s own drive (a drive E to Z, then a folder after a backslash or slash; C is every '
     'Windows machine\'s, D the docs\' example drive): a script takes the folder as an argument or from paths.env'),
    ('personal.email', 'content',
     r'(?<![\w.+-])[A-Za-z0-9._%+-]+@(?![\w.-]*\bexample\.|users\.noreply\.github\.com)[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}\b',
     'an e-mail or ssh-style address'),
    ('personal.lan', 'content',
     r'(?<![\d.])(192\.168\.\d{1,3}\.\d{1,3}|10\.\d{1,3}\.\d{1,3}\.\d{1,3}|172\.(1[6-9]|2\d|3[01])\.\d{1,3}\.\d{1,3}|'
     r'100\.(6[4-9]|[7-9]\d|1[01]\d|12[0-7])\.\d{1,3}\.\d{1,3})(?![\d.])', 'a private network address'),
    ('personal.ssh', 'content', r'\b(ssh|scp|rsync -e ssh)\s+(-\S+\s+)*(?!(tunnel|session|sessions|or|and|to|the|key|keys|agent|'
     r'connection|config|into|command|commands|access|login|port|host|hosts|server|is|was|in|on|from|for|with|as|a|an|it|'
     r'that|which|works|job|jobs|call|calls|run|runs|clone|remote|worker|workers|client|user|failed|fails|error|errors|timeout|timed|-)\b)[a-z][\w-]*(@[\w.-]+)?:?(\s|$)',
     'an ssh command naming a host'),
    ('personal.adb', 'content', r'\badb\s+-s\s+\w+', 'an adb device serial'),
    # Secrets.
    ('secret.key', 'content', r'-----BEGIN ([A-Z]+ )*PRIVATE KEY-----|\bssh-(rsa|ed25519|dss) AAAA[\w+/]{20,}',
     'a private or ssh key'),
    ('secret.token', 'content',
     r'\b(gh[pousr]_[A-Za-z0-9]{30,}|github_pat_\w{30,}|xox[abprs]-[\w-]{10,}|sk-[A-Za-z0-9_-]{20,}|AKIA[0-9A-Z]{16}|'
     r'AIza[\w-]{35}|glpat-[\w-]{20})\b', 'an access token'),
    ('secret.password', 'content',
     r'(--password[ =](?![\"\']?(\$|<|\{))\S+|\b(password|passwd|api[_-]?key|secret|token)\s*[:=]\s*["\'][^"\'\s$<{]{3,}["\'])',
     'a password or key in the clear'),
    # The workshop's own files.
    ('workshop.agents', 'path', r'(^|/)(CLAUDE|AGENTS)\.md$|(^|/)\.claude/|(^|/)(brief|progress)\.md$',
     'agent rules or a lane\'s log'),
    ('workshop.ref', 'content',
     r'(?<![\w.-])(workshop[/\\](merge|remote|status|log|CLAUDE\.md|README\.md)\b|tooling[/\\](merge|remote|status|log|dm2[/\\]log)[/\\]|'
     r'(?<![\w.-])(CLAUDE|NATIVEDRAW|NATIVE-PLAN|MIXING|CARRYOVER|QOL-IDEAS|XBOX-PLAN|WORKERS|DM2PCSX2)\.md\b|(?<![\w.-])log[/\\][\w.-]+\.md\b|(tooling[/\\])?(merge|remote|status)[/\\][\w.-]+\.(sh|py|ps1|env|tsv)\b)',
     'a reference into the workshop (workshop/, the agents\' CLAUDE.md, a round log, the tooling\'s old merge, remote or '
     'status): the public repository has none of it, so a public file states the fact itself or names a work database note'),
    ('workshop.ops', 'path', r'^(tooling/)?(merge|remote|status|log)/|^workshop/|(^|/)dm2/log/', 'the workshop\'s merge, '
     'remote, status or log files (workshop/ since the tooling was folded in)'),
]

TEXT_LIMIT = 8 << 20          # a text file larger than this is scanned in its first 8 MB only
ADDRESS = re.compile(rb'\b0x0{0,2}[1-3][0-9A-Fa-f]{5}\b')


def load_rules(paths):
    rules = list(RULES)
    for path in paths:
        with open(path, encoding='utf-8') as f:
            for n, line in enumerate(f, 1):
                line = line.rstrip('\n')
                if not line or line.startswith('#'):
                    continue
                cols = line.split('\t')
                if len(cols) < 4 or cols[1] not in ('path', 'content'):
                    sys.exit(f'{path}:{n}: want ID<TAB>path|content<TAB>REGEX<TAB>WHY')
                rules.append((cols[0], cols[1], cols[2], cols[3]))
    return rules


def load_allow(path):
    allow = []
    if path and os.path.exists(path):
        with open(path, encoding='utf-8') as f:
            for n, line in enumerate(f, 1):
                line = line.rstrip('\n')
                if not line or line.startswith('#'):
                    continue
                cols = line.split('\t')
                if len(cols) < 4 or cols[2] not in ('ok', 'open'):
                    sys.exit(f'{path}:{n}: want GLOB<TAB>RULE<TAB>ok|open<TAB>WHY')
                allow.append((cols[0], cols[1], cols[2], cols[3]))
    return allow


def allowed(allow, path, rule):
    """The allow file's verdict for (path, rule): 'ok', 'open' or None. The first matching line wins."""
    for glob, rid, status, _ in allow:
        if rid != '*' and rid != rule and not (rid.endswith('*') and rule.startswith(rid[:-1])):
            continue
        if fnmatch.fnmatchcase(path, glob) or (path.startswith('tooling/') and fnmatch.fnmatchcase(path[8:], glob)):
            return status
    return None


class Scanner:
    def __init__(self, rules, allow):
        self.allow = allow
        self.path_rules = [(rid, re.compile(rx, re.I), why) for rid, scope, rx, why in rules if scope == 'path' and rx]
        content = [(rid, rx) for rid, scope, rx, why in rules if scope == 'content' and rx]
        # One search per rule (each keeps its own literal-prefix speed-up; a joined alternation loses it), first hit only.
        self.content = [(rid, re.compile(rx.encode(), re.M)) for rid, rx in content]
        self.why = {rid: why for rid, _, _, why in rules}
        self.findings = defaultdict(list)   # rule -> [(where, snippet, status)]

    def add(self, rule, where, path, snippet=''):
        status = allowed(self.allow, path, rule) if path is not None else None
        if status == 'ok':
            return
        self.findings[rule].append((where, snippet, status or 'fail'))

    def scan_path(self, path, where):
        for rid, rx, _ in self.path_rules:
            if rx.search(path):
                self.add(rid, where, path)

    def scan_text(self, data, path, where):
        """Content rules over data (bytes); path None for a commit message (no allow lines apply)."""
        if path is not None:
            if data.startswith(b'\x7fELF') or b'\0' in data[:65536]:
                self.add('disc.binary', where, path)
                return
            lines = data.count(b'\n') + 1
            if lines >= 20:
                hits = sum(1 for line in data[:TEXT_LIMIT].split(b'\n') if ADDRESS.search(line))
                if hits * 2 >= lines:
                    self.add('disc.addresses', where, path, f'{hits} of {lines} lines name an address')
        head = data[:TEXT_LIMIT]
        for rid, rx in self.content:
            m = rx.search(head)
            if not m:
                continue
            line = data.count(b'\n', 0, m.start()) + 1
            text = m.group(0).decode('latin-1').strip()
            self.add(rid, f'{where}:{line}', path, text[:60])


# --added: the rules a changed line is held to (lanecheck's paths step). A workshop file (workshop.*) is held to the
# path rules alone: its scripts name the workshop's machines and options by design (rcheck's worker options), never a home
# folder or a scratch path. The files that define the workshop's names, and the old round logs, are not held.
ADDED_RULES = ('personal.home', 'personal.drive', 'personal.email', 'personal.lan', 'personal.adb', 'secret.', 'private.', 'workshop.ref')
ADDED_WORKSHOP_RULES = ('personal.home', 'secret.', 'private.scratch', 'private.user', 'private.device')
ADDED_EXEMPT = re.compile(r'^(tooling/|workshop/)?(merge/paths\.env|merge/public_scan_private\.tsv|log/)|(^|/)dm2/log/')


def scan_added(scanner, repo, base, prefix=''):
    """The lines added since BASE (the working tree against BASE, and new files not ignored) held to ADDED_RULES;
    PREFIX goes before each path (tooling/ for the tooling's repository). Returns the number of files read."""
    workshop = [rx for rid, rx, _ in scanner.path_rules if rid.startswith('workshop.')]
    added = defaultdict(list)   # path -> [(line, text)]
    path = None
    lineno = 0
    link = False
    for line in git(repo, 'diff', '-U0', '--no-color', '--no-ext-diff', '--ignore-submodules=all', base, '--',
                    text=False).split(b'\n'):
        if line.startswith(b'diff --git '):
            link = False
        elif line.startswith((b'new file mode 120000', b'new mode 120000')):
            link = True   # (a symbolic link's target, such as a data link setup.sh made where a file was: no text)
        elif line.startswith(b'+++ '):
            name = line[4:].decode('utf-8', 'replace')
            path = None if name == '/dev/null' or link else name[2:] if name.startswith('b/') else name
        elif line.startswith(b'@@ '):
            m = re.match(rb'@@ -\S+ \+(\d+)', line)
            lineno = int(m.group(1)) if m else 0
        elif line.startswith(b'+') and path is not None:
            added[path].append((lineno, line[1:]))
            lineno += 1
    for name in git(repo, 'ls-files', '-o', '--exclude-standard', '-z').split('\0'):
        full = os.path.join(repo, name)
        if name and os.path.isfile(full) and not os.path.islink(full):
            with open(full, 'rb') as f:
                data = f.read(TEXT_LIMIT)
            if b'\0' not in data[:65536]:
                added[name] = [(n, text) for n, text in enumerate(data.split(b'\n'), 1)]
    rules = [(rid, rx) for rid, rx in scanner.content if rid.startswith(ADDED_RULES)]
    for name, lines in added.items():
        path = prefix + name
        if ADDED_EXEMPT.search(path):
            continue
        held = ADDED_WORKSHOP_RULES if any(rx.search(path) for rx in workshop) else ADDED_RULES
        for n, text in lines:
            for rid, rx in rules:
                if rid.startswith(held):
                    m = rx.search(text)
                    if m:
                        scanner.add(rid, f'{path}:{n}', path, m.group(0).decode('latin-1').strip()[:60])
    return len(added)


def git(repo, *args, text=True):
    return subprocess.run(['git', '-C', repo, *args], check=True, capture_output=True, text=text).stdout


def blobs(repo, entries):
    """Yield (path, oid, data) for (path, oid) entries, read through one cat-file --batch: a thread writes every
    request while this reads the answers in order, so the pipe never waits on a round trip."""
    proc = subprocess.Popen(['git', '-C', repo, 'cat-file', '--batch'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            bufsize=1 << 20)

    def feed():
        try:
            for _, oid in entries:
                proc.stdin.write(oid.encode() + b'\n')
        finally:
            proc.stdin.close()

    writer = threading.Thread(target=feed, daemon=True)
    writer.start()
    try:
        for path, oid in entries:
            header = proc.stdout.readline().split()
            size = int(header[2])
            data = proc.stdout.read(size)
            proc.stdout.read(1)
            yield path, oid, data
    finally:
        writer.join()
        proc.stdout.close()
        proc.wait()


def scan_tree(scanner, repo, rev):
    out = git(repo, 'ls-tree', '-r', '-z', '--full-tree', rev)
    entries = []
    for rec in out.split('\0'):
        if not rec:
            continue
        meta, path = rec.split('\t', 1)
        mode, kind, oid = meta.split()
        if kind != 'blob' or mode == '120000':
            continue
        entries.append((path, oid))
    for path, oid, data in blobs(repo, entries):
        scanner.scan_path(path, path)
        scanner.scan_text(data, path, path)
    return len(entries)


def scan_history(scanner, repo):
    out = git(repo, 'rev-list', '--objects', '--all')
    oids = [line.split(' ', 1) for line in out.splitlines() if ' ' in line]
    check = subprocess.run(['git', '-C', repo, 'cat-file', '--batch-check=%(objectname) %(objecttype)'],
                           input=''.join(oid + '\n' for oid, _ in oids), capture_output=True, text=True, check=True)
    blob_ids = {line.split()[0] for line in check.stdout.splitlines() if line.endswith(' blob')}
    entries = [(path, oid) for oid, path in oids if oid in blob_ids]
    for path, oid, data in blobs(repo, entries):
        where = f'{path}@{oid[:9]}'
        scanner.scan_path(path, where)
        scanner.scan_text(data, path, where)
    log = git(repo, 'log', '--all', '--format=%x00%H%n%an <%ae>%n%cn <%ce>%n%B', text=False)
    commits = 0
    for rec in log.split(b'\0'):
        if rec.strip():
            commits += 1
            scanner.scan_text(rec, None, f'commit {rec[:9].decode()}')
    return len(entries), commits


SELF_TESTS = [
    ('disc.image', 'path', 'discs/game.iso', True), ('disc.executable', 'path', 'disc/SLUS_203.27', True),
    ('disc.executable', 'path', 'runtime/src/elf_loader.cpp', False), ('disc.dump', 'path', 'x/vu1.vif.hex', True),
    ('disc.picture', 'path', 'docs/shot.jpg', True), ('generated.python', 'path', 'a/__pycache__/x.pyc', True),
    ('workshop.agents', 'path', 'CLAUDE.md', True), ('workshop.ops', 'path', 'tooling/merge/x.sh', True),
    ('workshop.ops', 'path', 'workshop/log/dm2/x.md', True), ('workshop.ops', 'path', 'tools/checks/x.py', False),
    ('workshop.ref', 'content', 'see tooling/log/2026-10-26-backlog.md', True),
    ('workshop.ref', 'content', 'workshop/merge/lanecheck.sh runs it', True),
    ('workshop.ref', 'content', '(tooling/dm2/log/x.md 14.1)', True), ('workshop.ref', 'content', 'merge/release.sh', True),
    ('workshop.ref', 'content', 'tools/checks/public_scan.py and tools/workdb/workdb.py', False),
    ('workshop.ref', 'content', 'the status of the merge', False),
    ('workshop.ref', 'content', 'the rules (CLAUDE.md "Checks")', True), ('workshop.ref', 'content', 'see log/2026-10-24-bringup.md', True),
    ('workshop.ref', 'content', 'recomp/logs/run.log', False),
    ('workshop.ref', 'content', '(docs/NATIVEDRAW.md 1.1)', True), ('workshop.ref', 'content', 'docs/NATIVE.md', False),
    ('personal.home', 'content', 'see /Us' + 'ers/someone/Developer/x', True),
    ('personal.home', 'content', 'see $HOME/Developer/x', False),
    ('personal.drive', 'content', '$out = "G:' + '\\dave-mirra-bmx2\\scratch"', True),
    ('personal.drive', 'content', 'cd G:' + '/bmx-xxx/disc', True),
    ('personal.drive', 'content', 'path = "H:' + '\\\\data"', True),
    ('personal.drive', 'content', '-Iso "D:\\Aggressive Inline (USA).iso" -Msys C:\\msys64', False),
    ('personal.drive', 'content', "printf('%S:\\n')", False),
    ('personal.drive', 'content', 'https://example.org/x:/y', False),
    ('personal.email', 'content', 'mail me@host.example.org', False),
    ('personal.email', 'content', 'git' + '@git.server.net:a/b.git', True),
    ('personal.lan', 'content', 'connect 192.' + '168.1.20', True), ('personal.lan', 'content', 'version 10.0.1', False),
    ('personal.ssh', 'content', 'ss' + 'h buildbox uname', True), ('personal.adb', 'content', 'ad' + 'b -s 1234abcd shell', True),
    ('secret.key', 'content', '-----BEGIN OPENSSH ' + 'PRIVATE KEY-----', True),
    ('secret.token', 'content', 'ghp_' + 'a' * 36, True),
    ('secret.password', 'content', 'push --pass' + 'word hunter2', True),
    ('secret.password', 'content', 'push --password "$PW"', False),
    ('secret.password', 'content', 'api_key = ' + '"abcd1234"', True),
    ('secret.password', 'content', 'secret = static_cast<int>(x)', False),
]


def self_test(rules):
    bad = 0
    for rid, scope, sample, want in SELF_TESTS:
        s = Scanner(rules, [])
        if scope == 'path':
            s.scan_path(sample, sample)
        else:
            s.scan_text(sample.encode(), 'x.txt', 'x.txt')
        got = rid in s.findings
        if got != want:
            bad += 1
            print(f'self-test: {rid} on {sample!r}: got {got}, want {want}')
    s = Scanner(rules, [])
    s.scan_text(b'\x7fELF\x01', 'a.out', 'a.out')
    bad += 'disc.binary' not in s.findings
    s = Scanner(rules, [])
    s.scan_text(b''.join(b'f%d\t0x0010%04X\n' % (i, i) for i in range(30)), 't.tsv', 't.tsv')
    bad += 'disc.addresses' not in s.findings
    print('self-test:', 'ok' if not bad else f'{bad} failed')
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--repo', default='.', help='the repository (default: the working folder)')
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument('--tree', nargs='?', const='HEAD', metavar='REV', help='scan REV\'s files (default HEAD)')
    mode.add_argument('--history', action='store_true', help='scan every blob, message and ident in the history')
    mode.add_argument('--added', metavar='BASE', help='scan only the lines added since BASE (the working tree and new '
                      'files): the home, scratch, machine and secret rules (lanecheck\'s paths step)')
    ap.add_argument('--prefix', default='', help='--added: put this before each path (tooling/ for the tooling)')
    ap.add_argument('--rules', action='append', default=[], help='extra rules: ID<TAB>path|content<TAB>REGEX<TAB>WHY')
    ap.add_argument('--allow', default=os.path.join(HERE, 'public_scan_allow.tsv'), help='the allow file ("" for none)')
    ap.add_argument('--strict', action='store_true', help='open questions fail too')
    ap.add_argument('--examples', type=int, default=3, help='examples per rule (default 3)')
    ap.add_argument('--redact', action='store_true', help='print no matched text (for CI logs)')
    ap.add_argument('--json', help='write every finding to this file')
    ap.add_argument('--list-rules', action='store_true')
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args()
    rules = load_rules(a.rules)
    if a.list_rules:
        for rid, scope, _, why in rules:
            print(f'{rid}\t{scope}\t{why}')
        return 0
    if a.self_test:
        return self_test(rules)
    scanner = Scanner(rules, load_allow(a.allow))
    repo = os.path.abspath(a.repo)
    if a.history:
        nblobs, ncommits = scan_history(scanner, repo)
        scope = f'{nblobs} blobs and {ncommits} commits'
    elif a.added:
        scope = f'{scan_added(scanner, repo, a.added, a.prefix)} files changed since {a.added}'
    else:
        rev = a.tree or 'HEAD'
        scope = f'{scan_tree(scanner, repo, rev)} files at {rev}'
    fails = opens = 0
    print(f'public_scan: {repo}: {scope}')
    for rid in sorted(scanner.findings):
        found = scanner.findings[rid]
        nfail = sum(1 for f in found if f[2] == 'fail')
        nopen = len(found) - nfail
        fails += nfail
        opens += nopen
        print(f'  {rid:20} {nfail:6} fail {nopen:6} open  ({scanner.why.get(rid, "")})')
        shown = sorted(found, key=lambda f: f[2] != 'fail')[:a.examples]
        for where, snippet, status in shown:
            text = '' if a.redact or not snippet else f'  [{snippet}]'
            print(f'      {status:4} {where}{text}')
    print(f'public_scan: {fails} failing, {opens} open' + (' (--strict: open fails)' if a.strict else ''))
    if a.json:
        with open(a.json, 'w', encoding='utf-8') as f:
            json.dump({rid: [{'where': w, 'match': '' if a.redact else s, 'status': st} for w, s, st in v]
                       for rid, v in scanner.findings.items()}, f, indent=1)
    return 1 if fails or (a.strict and opens) else 0


if __name__ == '__main__':
    sys.exit(main())
