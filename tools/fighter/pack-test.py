#!/usr/bin/env python3
"""Checks of pack.py's systems.json (stats, walls, slash and effect sets),
stats, class, src and sources handling and of convert-stats.py, run by
`make fighter-test`. Works on in-memory
copies of assets/fighter/systems.json and testdata/terry-unconverted.json,
Terry's manifest as it was before convert-stats.py ran (no sheet decoding):
the real files load, every malformed variant is refused with ValueError,
stats resolve from the system plus overrides, class defaults and the damage
scale give the expected numbers, follow-ups pack their parent, window and
contact flag, a half circle behind the quarter circle it completes is
refused, and the conversion keeps every loaded
value and is idempotent. It also checks that every roster manifest is
already converted."""
import copy
import importlib.util
import json
import pathlib
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = pathlib.Path(__file__).resolve().parent
ASSETS = HERE.parents[1] / 'assets' / 'fighter'
sys.path.insert(0, str(HERE))
import pack  # noqa: E402

spec = importlib.util.spec_from_file_location('convert', HERE / 'convert-stats.py')
convert = importlib.util.module_from_spec(spec)
spec.loader.exec_module(convert)

SYSTEMS = json.loads((ASSETS / 'systems.json').read_text())
UNCONVERTED = HERE / 'testdata' / 'terry-unconverted.json'
TERRY = json.loads(UNCONVERTED.read_text())


def systems(data):
    with tempfile.TemporaryDirectory() as d:
        path = pathlib.Path(d) / 'systems.json'
        path.write_text(json.dumps(data))
        return pack.Systems(path)


def character(m, name='terry', data=SYSTEMS):
    return pack.Character(m, name, list(m['frames']), systems(data))


def refused(what, build):
    try:
        build()
    except ValueError:
        return
    sys.exit(f'pack-test: {what} was accepted')


def with_system(path, value):
    data = copy.deepcopy(SYSTEMS)
    node = data['systems']
    for k in path[:-1]:
        node = node[k]
    if value is None:
        del node[path[-1]]
    else:
        node[path[-1]] = value
    return data


def with_terry(edit):
    m = copy.deepcopy(TERRY)
    edit(m['characters']['terry'], m)
    return m


def main():
    # The shipped profiles and their resolved stats.
    real = systems(SYSTEMS)
    assert real.names[:2] == ['kof2002', 'samsho5sp'] and real.assign == {'haohmaru': 'samsho5sp'}
    kof, ss = real.profile['kof2002'], real.profile['samsho5sp']
    assert kof['damage_scale'] == 1000 and ss['damage_scale'] == 800
    assert kof['stats'] == {'life': 1000, 'defense': 1000, 'weight': 1000, 'height': 1,
                            'walk': 12, 'walk_back': 10, 'jump_vy': -51, 'gravity': 3, 'vx': 8,
                            'vx_back': 8, 'prejump': 4}
    assert kof['classes']['light'] == {'hitstun': 11, 'blockstun': 9, 'hitstop': 8}
    assert kof['classes']['command'] == {'hitstun': 19, 'blockstun': 17, 'hitstop': 12}
    # Walls and weapon moves: KOF strikes, Samurai Shodown slashes LP, HP
    # and specials (bits 0, 1 and 4).
    assert kof['margin'] == ss['margin'] == 24 and kof['play_width'] == ss['play_width'] == 480
    assert kof['slash'] == 0 and ss['slash'] == 0b10011 and ss['effects']['blood_light']
    assert pack.jump_arc(-51, 3) == (108, 35) and pack.jump_arc(-42, 2) == (110, 43)
    t = character(TERRY)
    assert t.system['name'] == 'kof2002' and not t.overrides and t.stats == kof['stats']
    # Terry under samsho5sp (by assignment): damage 30 * 0.8 = 24, chip of
    # Burn Knuckle 18 -> 14.4 -> 14.
    moved = copy.deepcopy(SYSTEMS)
    moved['characters']['terry'] = 'samsho5sp'
    t8 = character(TERRY, data=moved)
    assert t8.hits[0].resolved[:2] == [24, 0] and t8.hits[len(pack.MOVES) + 1].resolved[1] == 14
    assert t8.stats['walk'] == 6 and t8.stats['prejump'] == 1
    # Overrides: walk, a partial jump (vx_back follows vx) and prejump.
    m = with_terry(lambda c, _: c.update(stats={'walk': 9, 'jump': {'vx': 5}, 'prejump': 2,
                                                'height': 'low', 'defense': 1100,
                                                'src': {'walk': 'estimate'}}))
    c = character(m)
    assert c.overrides == {'walk': 9, 'vx': 5, 'vx_back': 5, 'prejump': 2, 'height': 0,
                           'defense': 1100}
    assert c.stats['walk_back'] == 10 and c.stats['jump_vy'] == -51
    # Class defaults: stand LP without its stun values takes light 11/9/8.
    m = with_terry(lambda c, _: [c['moves']['stand_lp'].pop(k) for k in
                                 ('hitstun', 'blockstun', 'hitstop')] and
                   c['moves']['stand_lp'].update({'class': 'light'}))
    lp = character(m).hits[0]
    assert lp.resolved[2:5] == [11, 9, 8] and lp.packed[2:5] == [0, 0, 0] and lp.defaults == 7
    assert lp.cls == pack.CLASSES.index('light') + 1
    # Written values win over the class.
    m = with_terry(lambda c, _: c['moves']['stand_lp'].update({'class': 'light'}))
    assert character(m).hits[0].resolved[2:5] == [12, 10, 6]
    # Sources: ids, mugen packs and counting.
    m = with_terry(lambda c, mm: (mm.update(sources={'dc02': {'url': 'https://example.org'}}),
                                  c['moves']['stand_lp'].update(
                                      src={'damage': 'dc02', 'steps': 'mugen:zelgadis',
                                           '*': 'estimate'})))
    c = character(m)
    assert c.sources['source'] == 1 and c.sources['mugen'] == 1 and c.sources['estimate'] > 50

    # Follow-ups: Burn Knuckle as a rekka of Power Wave packs its parent
    # (move index + 1), window and contact flag into the last four fields.
    def follow(**tr):
        return with_terry(lambda c, _: c['moves']['burn_knuckle']['trigger'].update(tr))

    c = character(follow(motion='any', after='power_wave', window=[3, 9], contact=True))
    k = c.move_names.index('burn_knuckle')
    assert c.moves[k][16:] == (c.move_names.index('power_wave') + 1, 3, 9, 1)
    assert c.moves[k - 1][16:] == (0, 0, 0, 0)
    assert character(follow(after='stand_hp', window=[0, 0])).moves[k][16:19] == (2, 0, 0)
    # Half circles and 421 pack as motions 11..13; a half circle may follow
    # its quarter circle only without a shared button.
    for index, motion in enumerate(['41236', '63214', '421'], 11):
        assert character(follow(motion=motion, buttons='K')).moves[k][1] == index
    assert character(follow(motion='63214')).moves[k][1] == 12

    # Malformed systems.json.
    bad_systems = [
        ('no kof2002', lambda: {'systems': {'samsho5sp': SYSTEMS['systems']['samsho5sp']}}),
        ('a bad name', lambda: {'systems': {'KOF': SYSTEMS['systems']['kof2002']}}),
        ('life 0', lambda: with_system(['kof2002', 'life'], 0)),
        ('no damage_scale', lambda: with_system(['kof2002', 'damage_scale'], None)),
        ('damage_scale 50', lambda: with_system(['kof2002', 'damage_scale'], 50)),
        ('walk 65', lambda: with_system(['kof2002', 'walk'], 65)),
        ('a jump past MAX_HEIGHT', lambda: with_system(['kof2002', 'jump', 'vy'], -200)),
        ('gravity 0', lambda: with_system(['kof2002', 'jump', 'gravity'], 0)),
        ('a jump without prejump', lambda: with_system(['kof2002', 'jump', 'prejump'], None)),
        ('an unknown key', lambda: with_system(['kof2002', 'speed'], 3)),
        ('an unknown class', lambda: with_system(['kof2002', 'stun_table', 'grab'], [1, 1])),
        ('a stun pair of one', lambda: with_system(['kof2002', 'stun_table', 'light'], [11])),
        ('hitstop 31', lambda: with_system(['kof2002', 'hitstop_table', 'light'], 31)),
        ('quarter window 33', lambda: with_system(['kof2002', 'input', 'quarter'], 33)),
        ('no buffer', lambda: with_system(['kof2002', 'input', 'buffer'], None)),
        ('push 1', lambda: with_system(['kof2002', 'push_distance'], 1)),
        ('height tall', lambda: with_system(['kof2002', 'height'], 'tall')),
        ('a bad hop', lambda: with_system(['kof2002', 'hop'], {'vy': 3})),
        ('nine systems', lambda: {'systems': {f's{i}': SYSTEMS['systems']['kof2002']
                                              for i in range(9)} | {
                                                  'kof2002': SYSTEMS['systems']['kof2002']}}),
        ('an assignment to an unknown system', lambda: {**SYSTEMS, 'characters': {'terry': 'x'}}),
        ('wall_margin 7', lambda: with_system(['kof2002', 'wall_margin'], 7)),
        ('play_width 481', lambda: with_system(['kof2002', 'play_width'], 481)),
        ('play_width 319', lambda: with_system(['kof2002', 'play_width'], 319)),
        ('an unknown effect key', lambda: with_system(['kof2002', 'effects', 'dust'], 'kof.x')),
        ('an effect without a set', lambda: with_system(['kof2002', 'effects', 'guard'], 'x')),
        ('a slash button', lambda: with_system(['kof2002', 'slash'], ['lp', 'zz'])),
        ('a src naming an unknown source', lambda: with_system(['kof2002', 'src'],
                                                               {'walk': 'dc99'})),
    ]
    for what, data in bad_systems:
        refused(f'systems.json with {what}', lambda d=data: systems(d()))
    refused('no systems.json', lambda: pack.Systems(pathlib.Path('/nonexistent/systems.json')))

    # Malformed stats, system choice, class, src and damage.
    def stats(v):
        return with_terry(lambda c, _: c.update(stats=v))

    bad_terry = [
        ('an unknown stat', stats({'speed': 3})),
        ('walk 65', stats({'walk': 65})),
        ('defense 99', stats({'defense': 99})),
        ('weight 5000', stats({'weight': 5000})),
        ('height tall', stats({'height': 'tall'})),
        ('a jump key typo', stats({'jump': {'vz': 1}})),
        ('prejump and jump.prejump differing', stats({'prejump': 2, 'jump': {'prejump': 3}})),
        ('a jump past MAX_HEIGHT', stats({'jump': {'vy': -120, 'gravity': 1}})),
        ('a bad stats src', stats({'src': {'walk': 'guess'}})),
        ('an unknown system', with_terry(lambda c, _: c.update(system='sf2'))),
        ('an unknown class', with_terry(lambda c, _: c['moves']['stand_lp'].update(
            {'class': 'medium'}))),
        ('a class without a hitstop default', with_terry(lambda c, _: (
            c['moves']['stand_lp'].update({'class': 'throw'}),
            c['moves']['stand_lp'].pop('hitstop')))),
        ('no class and no hitstun', with_terry(lambda c, _: c['moves']['stand_lp'].pop(
            'hitstun'))),
        ('damage 1501', with_terry(lambda c, _: c['moves']['stand_lp'].update(damage=1501))),
        ('a src naming an unknown field', with_terry(lambda c, _: c['moves']['stand_lp'].update(
            src={'colour': 'estimate'}))),
        ('a src naming an unknown source', with_terry(lambda c, _: c['moves']['stand_lp'].update(
            src={'damage': 'dc02'}))),
        ('a projectile class typo', with_terry(lambda c, _: c['projectiles']['power_wave'].update(
            {'class': 'specail'}))),
        ('a source without url', with_terry(lambda c, mm: mm.update(sources={'x': {}}))),
        ('a follow-up of an unknown move', follow(after='power_wav', window=[0, 1])),
        ('a follow-up of itself', follow(after='burn_knuckle', window=[0, 1])),
        ('a follow-up without a window', follow(after='power_wave')),
        ('a window of one tick value', follow(after='power_wave', window=[3])),
        ('a window closing before it opens', follow(after='power_wave', window=[5, 4])),
        ('a window past the parent', follow(after='stand_lp', window=[0, 60])),
        ('a window without after', follow(window=[0, 1])),
        ('contact without after', follow(contact=True)),
        ('contact not a bool', follow(after='power_wave', window=[0, 1], contact=1)),
        ('a held direction on its own', follow(motion='6')),
        ('any on its own', follow(motion='any')),
        ('a 41236+P behind the 236+P Power Wave', follow(motion='41236')),
    ]
    for what, m in bad_terry:
        refused(f'terry with {what}', lambda mm=m: character(mm))
    # A character's own system may not contradict the assignment.
    refused('haohmaru as kof2002 against the assignment', lambda: real.of('haohmaru', 'kof2002'))
    assert real.of('haohmaru', None)['name'] == 'samsho5sp'
    assert real.of('haohmaru', 'samsho5sp')['name'] == 'samsho5sp'

    # Conversion: same loaded values, idempotent, formatting kept.
    text = UNCONVERTED.read_text()
    lines = []
    new = convert.convert(text, TERRY, real, False, lines.append)
    after = json.loads(new)
    assert convert.loaded(TERRY, real) == convert.loaded(after, real)
    assert convert.convert(new, after, real, False, lambda _: None) == new
    lp = after['characters']['terry']['moves']['stand_lp']
    assert lp['class'] == 'light' and lp['hitstop'] == 6 and lp['hitstun'] == 12 and \
        lp['src'] == {'*': 'estimate'} and after['characters']['terry']['system'] == 'kof2002'
    clp = after['characters']['terry']['moves']['crouch_lp']
    assert not {'hitstun', 'blockstun'} & set(clp) and clp['hitstop'] == 6
    assert len(new.splitlines()) == len(text.splitlines()) + 1
    # --adopt-class drops the normals' stun values in favour of the class.
    adopted = json.loads(convert.convert(text, TERRY, real, True, lambda _: None))
    lp = adopted['characters']['terry']['moves']['stand_lp']
    assert not {'hitstun', 'blockstun', 'hitstop'} & set(lp)
    assert character(adopted).hits[0].resolved[2:5] == [11, 9, 8]
    # The real manifests have been converted: converting again changes nothing.
    for name in json.loads((ASSETS / 'roster.json').read_text())['manifests']:
        name = name if isinstance(name, str) else name['manifest']
        path = ASSETS / name
        text = path.read_text()
        assert convert.convert(text, json.loads(text), real, False, lambda _: None) == text, name
    low_depth_boxed_sheet()
    print('pack-test: systems.json, stats, class defaults, damage scale, follow-ups, '
          'src/sources validation, conversion and 4-bit boxed sheets passed')


def low_depth_boxed_sheet():
    """A 4-bit palette sheet of two boxes whose lines are also the sprites'
    outline colour decodes through its palette, a whole box is a frame (the
    outline colour inside it stays a sprite colour) and a rectangle cutting a
    sprite is refused."""
    import hashlib
    import struct
    import zlib
    pal = [(255, 0, 255), (24, 16, 16), (200, 100, 50)]
    w, h = 13, 7
    grid = [[1] * w for _ in range(h)]
    for y in range(1, 6):
        for x in list(range(1, 6)) + list(range(7, 12)):
            grid[y][x] = 0
    grid[3][2:5] = [1, 2, 2]     # an outlined sprite in the left box
    grid[5][9] = 2               # one touching the bottom line of the right box
    raw = b''.join(b'\0' + bytes((r[x] << 4) | (r[x + 1] if x + 1 < w else 0)
                                 for x in range(0, w, 2)) for r in grid)

    def chunk(k, b):
        return struct.pack('>I', len(b)) + k + b + struct.pack('>I', zlib.crc32(k + b))
    png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 4, 3, 0, 0, 0)) +
           chunk(b'PLTE', bytes(v for c in pal for v in c)) + chunk(b'IDAT', zlib.compress(raw)) +
           chunk(b'IEND', b''))
    with tempfile.TemporaryDirectory() as d:
        sheet = pathlib.Path(d) / 'boxed.png'
        sheet.write_bytes(png)
        rw, rh, rows = pack.read_png(sheet)
        assert (rw, rh) == (w, h) and rows[3][6:15] == bytes([24, 16, 16, 200, 100, 50, 200, 100, 50])
        man = pathlib.Path(d) / 'boxed.json'
        man.write_text(json.dumps({
            'sheet': 'boxed.png', 'sha256': hashlib.sha256(png).hexdigest(), 'background': 'ff00ff',
            'boxed': '181010',
            'frames': {'a': [1, 1, 5, 5, 2, 5], 'b': [7, 1, 5, 5, 2, 5], 'cut': [1, 1, 3, 5, 1, 5]}}))
        s = pack.Sheet(man)
        pix, ax, ay = s.frame('a')
        assert pix == [[b'\x18\x10\x10', b'\xc8\x64\x32', b'\xc8\x64\x32']] and (ax, ay) == (1, 3)
        assert s.frame('b')[0] == [[b'\xc8\x64\x32']]
        refused('a rectangle cutting a sprite', lambda: s.frame('cut'))


if __name__ == '__main__':
    main()
