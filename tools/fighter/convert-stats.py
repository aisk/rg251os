#!/usr/bin/env python3
"""One-time conversion of fighter manifests to the stats schema (step 1 of
docs/fighter-stats.md). Idempotent: a second run changes nothing.

For every character of every manifest (default: those in
assets/fighter/roster.json) it
  - adds "system" with the profile the character already uses (the one
    systems.json assigns to its name, else kof2002), so the assignment is
    written down in the manifest itself;
and for every move and projectile it
  - adds "class": stand/crouch lp and lk are light, hp and hk heavy, other
    moves (specials) and projectiles special;
  - removes hitstun, blockstun and hitstop where the value equals the class
    default of the character's system (so the loaded numbers do not change);
  - adds "src": {"*": "estimate"} where there is no src yet (every number in
    today's manifests is an estimate).
With --adopt-class it instead removes hitstun/blockstun/hitstop of the
normals whenever the class gives a default, as the design asks ("普通技删掉
hitstun/blockstun 改用 class"); that changes frame data and every change is
printed. Without it the script checks, through pack.py's own loader, that
every character's stats and resolved hit values are identical before and
after, and refuses to write otherwise.

Edits are made in the text, keeping each manifest's hand formatting.

  python3 tools/fighter/convert-stats.py [--check] [--adopt-class] [MANIFEST ...]

--check only reports what would change and exits 1 if anything would.
"""
import argparse
import json
import json.decoder
import pathlib
import re
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import pack  # noqa: E402

ROSTER = pathlib.Path(__file__).resolve().parents[2] / 'assets' / 'fighter' / 'roster.json'
STUN_FIELDS = ['hitstun', 'blockstun', 'hitstop']
NUMBER = re.compile(r'-?(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?|true|false|null')


class Node:
    """A JSON value with its text span; objects keep (key, key start, value)
    members in order."""

    def __init__(self, kind, start):
        self.kind, self.start, self.end, self.members, self.items = kind, start, start, [], []

    def get(self, key):
        return next((v for k, _, v in self.members if k == key), None)


def skip(t, i):
    while i < len(t) and t[i] in ' \t\r\n':
        i += 1
    return i


def parse(t, i=0):
    i = skip(t, i)
    if t[i] == '{':
        n = Node('object', i)
        i = skip(t, i + 1)
        while t[i] != '}':
            kstart = i
            key, i = json.decoder.scanstring(t, i + 1)
            i = skip(t, i)
            assert t[i] == ':'
            value = parse(t, i + 1)
            n.members.append((key, kstart, value))
            i = skip(t, value.end)
            if t[i] == ',':
                i = skip(t, i + 1)
        n.end = i + 1
        return n
    if t[i] == '[':
        n = Node('array', i)
        i = skip(t, i + 1)
        while t[i] != ']':
            item = parse(t, i)
            n.items.append(item)
            i = skip(t, item.end)
            if t[i] == ',':
                i = skip(t, i + 1)
        n.end = i + 1
        return n
    n = Node('value', i)
    if t[i] == '"':
        _, n.end = json.decoder.scanstring(t, i + 1)
    else:
        m = NUMBER.match(t, i)
        assert m, f'bad JSON at {i}'
        n.end = m.end()
    return n


class Edits:
    def __init__(self, text):
        self.text, self.list = text, []

    def insert_first(self, obj, member):
        """Insert a member before the object's first one, matching its layout."""
        first = obj.members[0][1]
        ws = self.text[obj.start + 1:first]
        self.list.append((first, first, member + (',' + ws if '\n' in ws else ', ')))

    def insert_before(self, kstart, member):
        self.list.append((kstart, kstart, member + ', '))

    def remove(self, obj, keys):
        """Remove members (and their separators) from an object."""
        ms = obj.members
        gone = [k in keys for k, _, _ in ms]
        if all(gone):
            self.list.append((ms[0][1], ms[-1][2].end, ''))
            return
        tail = len(ms)
        while gone[tail - 1]:
            tail -= 1
        for i in range(tail):
            if gone[i]:
                self.list.append((ms[i][1], ms[i + 1][1], ''))
        if tail < len(ms):
            self.list.append((ms[tail - 1][2].end, ms[-1][2].end, ''))

    def apply(self):
        out = self.text
        # From the end; at one position a removal goes before an insertion.
        for start, end, new in sorted(self.list, key=lambda e: (e[0], e[0] != e[1]),
                                      reverse=True):
            out = out[:start] + new + out[end:]
        return out


def default_class(move):
    if move in pack.MOVES:
        return 'light' if move.endswith(('_lp', '_lk')) else 'heavy'
    return 'special'


def loaded(m, systems):
    """What the core would get: stats and resolved hits per character. A
    move without hitboxes never connects, so its hit values are left out
    (its omitted fields now come from the class instead of 1/1/0)."""
    names = list(m['frames'])
    out = {}
    for cname in m['characters']:
        c = pack.Character(m, cname, names, systems)
        hits = [h.resolved if c.frame_data(k)[4] else None for k, h in enumerate(c.hits)]
        out[cname] = (c.system['name'], c.stats, hits,
                      [h.resolved for h in c.projectile_hits], c.move_names,
                      c.projectile_names)
    return out


def convert(text, m, systems, adopt, log):
    tree = parse(text)
    edits = Edits(text)
    chars = tree.get('characters')
    for cname, _, cnode in chars.members:
        ch = m['characters'][cname]
        system = systems.of(cname, ch.get('system'))
        if 'system' not in ch:
            edits.insert_first(cnode, f'"system": "{system["name"]}"')
            log(f'{cname}: system {system["name"]}')
        groups = [('moves', False), ('projectiles', True)]
        for group, projectile in groups:
            gnode = cnode.get(group)
            if gnode is None:
                continue
            for name, _, node in gnode.members:
                d = ch[group][name]
                cls = d.get('class') or ('special' if projectile else default_class(name))
                row = system['classes'].get(cls, {})
                drop = [k for k in STUN_FIELDS if k in d and k in row and (
                    d[k] == row[k] or (adopt and not projectile and name in pack.MOVES))]
                if 'class' not in d:
                    edits.insert_first(node, f'"class": "{cls}"')
                if drop:
                    edits.remove(node, set(drop))
                if 'src' not in d:
                    steps = next(k for k, _, _ in node.members if k == 'steps')
                    edits.insert_before(next(s for k, s, _ in node.members if k == steps),
                                        '"src": {"*": "estimate"}')
                changes = ([] if 'class' in d else [f'class {cls}']) + \
                    [f'-{k} {d[k]}' + ('' if d[k] == row[k] else f' (class gives {row[k]})')
                     for k in drop] + ([] if 'src' in d else ['src estimate'])
                if changes:
                    log(f'{cname} {name}: ' + ', '.join(changes))
    return edits.apply() if edits.list else text


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('manifests', nargs='*', type=pathlib.Path)
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--adopt-class', action='store_true')
    args = ap.parse_args()
    paths = args.manifests or [ROSTER.parent / p for p in
                               json.loads(ROSTER.read_text())['manifests']]
    changed = 0
    for path in paths:
        text = path.read_text()
        m = json.loads(text)
        systems = pack.load_systems(path)
        lines = []
        new = convert(text, m, systems, args.adopt_class, lines.append)
        if new == text:
            print(f'{path}: up to date')
            continue
        changed += 1
        converted = json.loads(new)
        before, after = loaded(m, systems), loaded(converted, systems)
        for line in lines:
            print(f'{path.name}: {line}')
        if before != after:
            if not args.adopt_class:
                sys.exit(f'{path}: the conversion would change loaded values; not written')
            for cname in before:
                b, a = before[cname], after[cname]
                for k, move in enumerate(b[4]):
                    if b[2][k] != a[2][k]:
                        print(f'{path.name}: {cname} {move} damage/chip/hitstun/blockstun/'
                              f'hitstop/push/block push {b[2][k]} -> {a[2][k]}')
        if convert(new, converted, systems, args.adopt_class, lambda _: None) != new:
            sys.exit(f'{path}: a second conversion would change it again; not written')
        if not args.check:
            path.write_text(new)
            print(f'{path}: written')
    if args.check and changed:
        sys.exit(1)


if __name__ == '__main__':
    main()
