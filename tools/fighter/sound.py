#!/usr/bin/env python3
"""Build the fighter's sound bank (<pack>.fightsnd) from
assets/fighter/sound/sound.json, and regenerate its committed sources.

The bank sits next to a content pack and holds the sound effects as IMA
ADPCM mono at their own rate and the music as MPEG-1 Layer II (48 kHz
stereo) with loop points in decoded samples, plus the tables that map the
rules' sound events (enum sound_id in app/fighter/fighter.h, read from the
header so the ids cannot drift) and the game's music cues to them.
app/fighter/audio.c is the reader; any change here changes FIGHTSND there.

Subcommands:

  build ROSTER OUTPUT     write the bank for the roster's pack; any SFX clip
                          whose WAV is missing is synthesized (and reported)
  extract                 re-cut assets/fighter/sound/sfx/*.wav from the
                          MUGEN .snd files listed under "sources" (needs the
                          downloads in README.md and ffmpeg)
  music VGM2WAV           re-render and encode assets/fighter/sound/music/
                          (VGM packs from README.md, libvgm's vgm2wav, ffmpeg)
                          and rewrite music/tracks.json with the loop points
  synth NAME OUTPUT.wav   write one synthesized fallback clip, for listening

Bank format FIGHTSND version 1, little-endian:
  header 32: "FIGHTSND", u32 version, u32 file size, u32 CRC-32 (zlib) of
             bytes [20, size), u16 clips, u16 tracks, u16 cues, u16 music cues,
             u32 offset of the data area
  clips x 32: name[16] (NUL-padded), u32 offset, u32 samples, u32 rate,
             u32 reserved 0. Data: ceil(samples / 256) blocks of 132 bytes:
             s16 predictor and u8 step index (the decoder state before the
             block's first sample), u8 0, then 256 4-bit codes, the even
             sample in the low nibble
  tracks x 48: name[16], u32 offset, u32 frames, u32 frame bytes (every
             frame the same size: 48 kHz without padding), u32 loop start,
             u32 loop end (decoded samples; the decoder's delay included),
             u32 flags (bit 0 loops; without it the track plays once),
             u32 level (per mille), u32 reserved 0
  cues x 32: scope[16] ("" any, a system name or a character's display
             name), u8 event (enum sound_id), u8 flags (bits 0-1 kind: 0
             effect, 1 voice, 2 announcer; bit 2: the event side's
             opponent picks and voices it),
             u8 move (0 any, else move index + 1), u8 variants (1..4),
             u16 clip[4] (0xffff = stay silent for that variant), u16 gain
             (/256), u16 reserved 0
  music x 32: scope[16] ("", a system name or a stage's display name; a
             stage row wins over the system's in that arena), u8 cue (0
             select, 1 fight, 2 win, 3 continue, 4 clear), u8 tracks
             (1..6), u16 track[6], u16 reserved 0
"""
import array
import gzip
import hashlib
import io
import json
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import wave
import zipfile
import zlib

sys.dont_write_bytecode = True
HERE = pathlib.Path(__file__).resolve().parent
TOP = HERE.parents[1]
sys.path.insert(0, str(HERE))
import pack  # noqa: E402

SOUND = TOP / 'assets' / 'fighter' / 'sound'
MANIFEST = SOUND / 'sound.json'
TRACKS = SOUND / 'music' / 'tracks.json'
MAGIC = b'FIGHTSND'
VERSION = 1
BLOCK = 256
BLOCK_BYTES = 4 + BLOCK // 2
MAX_RATE = 24000
MUSIC_CUES = ['select', 'fight', 'win', 'continue', 'clear']
KINDS = ['sfx', 'voice', 'announcer']
FLAG_OTHER = 4
# Limits mirrored in app/fighter/audio.h.
MAX_CLIPS, MAX_TRACKS, MAX_CUES, MAX_MUSIC = 1024, 64, 2048, 64
MAX_CLIP_SAMPLES = 48000 * 10
# MP2 frames: 1152 samples; 48 kHz, so no padding and a fixed size.
FRAME_SAMPLES = 1152
DECODER_DELAY = 481
LOOP_TAIL = 2 * FRAME_SAMPLES

STEPS = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
         45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190,
         209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724,
         796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272,
         2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132,
         7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
         20350, 22385, 24623, 27086, 29794, 32767]
INDEX = [-1, -1, -1, -1, 2, 4, 6, 8]


def sound_ids():
    """enum sound_id from fighter.h, without the SOUND_ prefix."""
    text = (TOP / 'app' / 'fighter' / 'fighter.h').read_text()
    m = re.search(r'enum sound_id\s*{([^}]*)}', text)
    if not m:
        raise ValueError('fighter.h: no enum sound_id')
    names = [n.strip() for n in m.group(1).split(',') if n.strip()]
    if not names or names[-1] != 'SOUNDS' or any('=' in n for n in names):
        raise ValueError('fighter.h: enum sound_id must count up to SOUNDS')
    return [n[len('SOUND_'):] for n in names[:-1]]


# ---- IMA ADPCM ----

def adpcm_decode_nibble(code, pred, index):
    step = STEPS[index]
    d = step >> 3
    if code & 4:
        d += step
    if code & 2:
        d += step >> 1
    if code & 1:
        d += step >> 2
    pred = pred - d if code & 8 else pred + d
    pred = max(-32768, min(32767, pred))
    index = max(0, min(88, index + INDEX[code & 7]))
    return pred, index


def adpcm_encode(samples):
    """Blocks of 256 samples; the last one is padded with silence."""
    out = bytearray()
    pred, index = 0, 0
    n = len(samples)
    for start in range(0, n, BLOCK):
        out += struct.pack('<hBB', pred, index, 0)
        codes = []
        for i in range(start, start + BLOCK):
            s = samples[i] if i < n else 0
            step = STEPS[index]
            diff = s - pred
            code = 0
            if diff < 0:
                code = 8
                diff = -diff
            if diff >= step:
                code |= 4
                diff -= step
            if diff >= step >> 1:
                code |= 2
                diff -= step >> 1
            if diff >= step >> 2:
                code |= 1
            pred, index = adpcm_decode_nibble(code, pred, index)
            codes.append(code)
        out += bytes(codes[i] | codes[i + 1] << 4 for i in range(0, BLOCK, 2))
    return bytes(out)


# ---- deterministic synthesized fallback (only + - * / on floats) ----

class Noise:
    def __init__(self, seed):
        self.x = seed & 0xffffffff

    def __call__(self):
        self.x = (self.x * 1664525 + 1013904223) & 0xffffffff
        return (self.x >> 8) / 8388608.0 - 1.0


def sine(phase):
    """sin(2 pi phase), phase in turns, by a Taylor series on [-pi/2, pi/2]."""
    p = phase - int(phase)
    if p < 0:
        p += 1.0
    sign = 1.0
    if p >= 0.5:
        p -= 0.5
        sign = -1.0
    if p > 0.25:
        p = 0.5 - p
    x = p * 6.283185307179586
    x2 = x * x
    s = x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110)))))
    return sign * s


SYNTH = {
    # name: (seconds, noise amount, tone start Hz, tone end Hz, tone amount,
    #        attack s, decay s, noise filter 0..1 (1 = bright), seed)
    'whoosh_light': (0.14, 0.7, 0, 0, 0.0, 0.05, 0.08, 0.35, 1),
    'whoosh_heavy': (0.22, 0.8, 0, 0, 0.0, 0.08, 0.12, 0.25, 2),
    'whoosh_special': (0.30, 0.8, 0, 0, 0.0, 0.10, 0.18, 0.30, 3),
    'hit_light': (0.10, 0.6, 180, 60, 0.6, 0.002, 0.05, 0.6, 4),
    'hit_heavy': (0.20, 0.7, 150, 45, 0.8, 0.002, 0.10, 0.5, 5),
    'hit_special': (0.32, 0.8, 140, 40, 0.9, 0.002, 0.16, 0.5, 6),
    'guard': (0.12, 0.2, 1200, 1900, 0.7, 0.001, 0.05, 0.9, 7),
    'slash_light': (0.12, 0.9, 0, 0, 0.0, 0.001, 0.06, 0.95, 8),
    'slash_heavy': (0.20, 0.9, 0, 0, 0.0, 0.001, 0.10, 0.95, 9),
    'jump': (0.10, 0.5, 0, 0, 0.0, 0.02, 0.05, 0.2, 10),
    'land': (0.10, 0.6, 90, 50, 0.5, 0.001, 0.05, 0.3, 11),
    'ko': (0.80, 0.5, 120, 30, 0.9, 0.002, 0.45, 0.3, 12),
    'go': (0.40, 0.0, 660, 990, 0.8, 0.01, 0.25, 0.5, 13),
}


def synth(name, rate=24000):
    sec, noise_amt, f0, f1, tone_amt, attack, decay, bright, seed = SYNTH[name]
    n = int(sec * rate)
    rnd = Noise(seed)
    lp = 0.0
    phase = 0.0
    out = []
    for i in range(n):
        t = i / rate
        env = t / attack if t < attack else 1.0 / (1.0 + (t - attack) / decay * 4)
        if t > sec * 0.8:
            env *= (sec - t) / (sec * 0.2)
        lp += (rnd() - lp) * bright
        f = f0 + (f1 - f0) * (i / n)
        phase += f / rate
        if name == 'guard':
            tone = (1.0 if sine(phase) >= 0 else -1.0) * 0.5 + \
                (1.0 if sine(phase * 1.58) >= 0 else -1.0) * 0.5
        else:
            tone = sine(phase)
        v = (lp * noise_amt + tone * tone_amt) * env
        out.append(max(-32768, min(32767, int(v * 20000))))
    return out


# ---- WAV and MUGEN SND ----

def read_wav_bytes(data):
    """(rate, mono int16 samples) of a PCM WAV (8/16 bit, mono or stereo)."""
    w = wave.open(io.BytesIO(data))
    ch, width, rate = w.getnchannels(), w.getsampwidth(), w.getframerate()
    raw = w.readframes(w.getnframes())
    if width == 1:
        vals = [(b - 128) << 8 for b in raw]
    elif width == 2:
        a = array.array('h')
        a.frombytes(raw[:len(raw) // 2 * 2])
        if sys.byteorder != 'little':
            a.byteswap()
        vals = list(a)
    else:
        raise ValueError(f'{width * 8}-bit WAV')
    if ch == 2:
        vals = [(vals[i] + vals[i + 1]) // 2 for i in range(0, len(vals) - 1, 2)]
    elif ch != 1:
        raise ValueError(f'{ch} channels')
    return rate, vals


def write_wav(path, rate, samples):
    a = array.array('h', samples)
    if sys.byteorder != 'little':
        a.byteswap()
    with wave.open(str(path), 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(a.tobytes())


def read_snd(path):
    """{(group, sample): wav bytes} of a MUGEN .snd file."""
    d = pathlib.Path(path).read_bytes()
    if d[:12] != b'ElecbyteSnd\0':
        raise ValueError(f'{path}: not a MUGEN SND file')
    n, off = struct.unpack_from('<II', d, 16)
    out = {}
    for _ in range(n):
        nxt, ln, g, s = struct.unpack_from('<IIII', d, off)
        out.setdefault((g, s), d[off + 16:off + 16 + ln])
        if not nxt:
            break
        off = nxt
    return out


def sha256(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def load_manifest():
    return json.loads(MANIFEST.read_text())


def resample(rate, samples, target):
    """ffmpeg's resampler, for extract only."""
    raw = array.array('h', samples).tobytes()
    out = subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-f', 's16le',
                          '-ar', str(rate), '-ac', '1', '-i', '-', '-ar', str(target),
                          '-f', 's16le', '-'], input=raw, capture_output=True, check=True).stdout
    a = array.array('h')
    a.frombytes(out)
    return list(a)


def trim(samples, threshold=48):
    """Drop leading and trailing near-silence (keeps a few samples)."""
    lo, hi = 0, len(samples)
    while lo < hi and abs(samples[lo]) < threshold:
        lo += 1
    while hi > lo and abs(samples[hi - 1]) < threshold:
        hi -= 1
    return samples[max(0, lo - 8):min(len(samples), hi + 8)]


def extract():
    m = load_manifest()
    sources = m['sources']
    cache = {}
    (SOUND / 'sfx').mkdir(exist_ok=True)
    for name, clip in m['clips'].items():
        parts = clip.get('from')
        if not parts:
            continue
        out = []
        rate = None
        for part in parts:
            if isinstance(part, (int, float)):
                out += [0] * int(part * (rate or MAX_RATE))
                continue
            src, g, s = part
            info = sources[src]
            path = TOP / info['file']
            if src not in cache:
                if sha256(path) != info['sha256']:
                    raise ValueError(f'{path}: SHA256 differs from sound.json')
                cache[src] = read_snd(path)
            if (g, s) not in cache[src]:
                raise ValueError(f'{name}: {src} has no sound {g},{s}')
            r, vals = read_wav_bytes(cache[src][(g, s)])
            vals = trim(vals)
            target = min(r, MAX_RATE)
            if rate is None:
                rate = target
            if r != rate:
                vals = resample(r, vals, rate)
            out += vals
        gain = clip.get('gain', 1.0)
        out = [max(-32768, min(32767, int(v * gain))) for v in out]
        write_wav(SOUND / 'sfx' / f'{name}.wav', rate, out)
        print(f'{name}: {rate} Hz {len(out) / rate:.2f} s')


# ---- music ----

def vgm_header(data):
    if data[:2] == b'\x1f\x8b':
        data = gzip.decompress(data)
    if data[:4] != b'Vgm ':
        raise ValueError('not a VGM file')
    total, _, loop = struct.unpack_from('<III', data, 0x18)
    return total, loop


def pcm(path):
    a = array.array('h')
    a.frombytes(pathlib.Path(path).read_bytes())
    if sys.byteorder != 'little':
        a.byteswap()
    return a


def ffmpeg(*args):
    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', *args], check=True)


def delay_of(master, decoded):
    """Lag of decoded behind master (left channel), in the loudest of the
    first few half-second windows."""
    frames = len(master) // 2 - 3 * FRAME_SAMPLES - 3000
    bases = range(0, max(1, min(frames, 48000 * 8)), 24000)
    base = max(bases, key=lambda b: sum(abs(master[2 * (b + k)]) for k in range(0, 3000, 3)))
    best = None
    for lag in range(0, 2 * FRAME_SAMPLES):
        e = 0
        for k in range(0, 3000, 3):
            e += abs(decoded[2 * (base + k + lag)] - master[2 * (base + k)])
        if best is None or e < best[0]:
            best = (e, lag)
    return best[1]


def music(vgm2wav):
    m = load_manifest()
    sources = m['sources']
    tracks = {}
    (SOUND / 'music').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = pathlib.Path(tmpdir)
        for name, t in m['tracks'].items():
            info = sources[t['source']]
            path = TOP / info['file']
            if sha256(path) != info['sha256']:
                raise ValueError(f'{path}: SHA256 differs from sound.json')
            vgm = zipfile.ZipFile(path).read(t['vgm'])
            total, loop = vgm_header(vgm)
            (tmp / 'in.vgz').write_bytes(vgm)
            subprocess.run([vgm2wav, '--samplerate', '48000', '--loops', '2', '--fade', '0',
                            str(tmp / 'in.vgz'), str(tmp / 'render.wav')], check=True,
                           capture_output=True)
            ffmpeg('-i', str(tmp / 'render.wav'), '-f', 's16le', '-ac', '2', '-ar', '48000',
                   str(tmp / 'render.raw'))
            r = pcm(tmp / 'render.raw')
            end = total * 48000 // 44100
            if loop:
                start = (total - loop) * 48000 // 44100
                # The render's loop start can sit a sample or two off the
                # exact ratio; take the one that best matches the loop end.
                def err(s):
                    return sum(abs(r[2 * (s + k)] - r[2 * (end + k)]) +
                               abs(r[2 * (s + k) + 1] - r[2 * (end + k) + 1])
                               for k in range(0, 24000, 2))
                start = min(range(start - 3, start + 4), key=err)
                master = r[:2 * end] + r[2 * start:2 * (start + LOOP_TAIL)]
            else:
                start = 0
                master = r[:2 * end]
            gain = t.get('gain', 1.0)
            if gain != 1.0:
                master = array.array('h', (max(-32768, min(32767, int(v * gain))) for v in master))
            (tmp / 'master.raw').write_bytes(array.array('h', master).tobytes())
            out = SOUND / 'music' / f'{name}.mp2'
            ffmpeg('-f', 's16le', '-ar', '48000', '-ac', '2', '-i', str(tmp / 'master.raw'),
                   '-c:a', 'mp2', '-b:a', t.get('bitrate', '128k'), '-f', 'mp2', str(out))
            ffmpeg('-i', str(out), '-f', 's16le', str(tmp / 'decoded.raw'))
            delay = delay_of(master, pcm(tmp / 'decoded.raw'))
            if delay != DECODER_DELAY:
                raise ValueError(f'{name}: decoder delay {delay}, expected {DECODER_DELAY}')
            frames, fb = mp2_frames(out.read_bytes(), name)
            entry = {'mp2': f'{name}.mp2', 'sha256': sha256(out), 'frames': frames,
                     'frame_bytes': fb, 'seconds': round(end / 48000, 2)}
            if loop:
                entry['loop_start'] = start + delay
                entry['loop_end'] = end + delay
                if entry['loop_end'] > frames * FRAME_SAMPLES:
                    raise ValueError(f'{name}: loop end past the encoded frames')
            tracks[name] = entry
            print(f'{name}: {end / 48000:.1f} s, {frames} frames x {fb} bytes, loop '
                  f'{entry.get("loop_start", "-")}..{entry.get("loop_end", "-")}')
    TRACKS.write_text(json.dumps(tracks, indent=1) + '\n')


MP2_BITRATES = [0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384]


def mp2_frames(data, what):
    """(frames, frame bytes): every frame MPEG-1 Layer II 48 kHz, same size."""
    pos, fb, frames = 0, None, 0
    while pos < len(data):
        if pos + 4 > len(data):
            raise ValueError(f'{what}: trailing bytes')
        h = data[pos:pos + 4]
        if h[0] != 0xff or (h[1] & 0xfe) != 0xfc or (h[2] >> 2) & 3 != 1 or h[2] & 2:
            raise ValueError(f'{what}: frame {frames} is not MPEG-1 Layer II 48 kHz unpadded')
        rate = MP2_BITRATES[h[2] >> 4]
        size = 144 * rate * 1000 // 48000
        if not rate or (fb is not None and size != fb):
            raise ValueError(f'{what}: frame {frames} changes size')
        fb = size
        pos += size
        frames += 1
    if pos != len(data) or not frames:
        raise ValueError(f'{what}: truncated frame')
    return frames, fb


# ---- bank ----

def name16(text, what):
    b = text.encode('ascii')
    if not 1 <= len(b) <= 15:
        raise ValueError(f'{what}: names are 1..15 ASCII characters')
    return b.ljust(16, b'\0')


def scope16(text, what):
    b = text.encode('ascii')
    if len(b) > 15:
        raise ValueError(f'{what}: scope too long')
    return b.ljust(16, b'\0')


def roster_info(roster):
    """{display name: [move names in pack order]}, system names, stage display
    names."""
    roster = pathlib.Path(roster)
    data = json.loads(roster.read_text())
    paths = [roster.parent / p for p in data['manifests']] if 'manifests' in data else [roster]
    chars = {}
    for p in paths:
        for key, ch in json.loads(p.read_text())['characters'].items():
            moves = list(pack.MOVES) + [k for k in ch['moves'] if k not in pack.MOVES]
            chars[pack.display_name(key)] = moves
    systems = json.loads((roster.parent / 'systems.json').read_text())['systems']
    stages = [pack.display_name(json.loads((roster.parent / p).read_text())['name'])
              for p in data.get('stages', [])]
    return chars, list(systems), stages


def build(roster, output):
    m = load_manifest()
    events = sound_ids()
    chars, systems, stages = roster_info(roster)
    tracks_meta = json.loads(TRACKS.read_text()) if TRACKS.exists() else {}
    clip_names = list(m['clips'])
    clips, synthesized = [], []
    for name in clip_names:
        clip = m['clips'][name]
        wav = SOUND / 'sfx' / f'{name}.wav'
        if wav.exists():
            rate, samples = read_wav_bytes(wav.read_bytes())
        else:
            rate, samples = 24000, synth(clip['synth'])
            synthesized.append(name)
        if not 4000 <= rate <= 48000 or not 1 <= len(samples) <= MAX_CLIP_SAMPLES:
            raise ValueError(f'{name}: rate {rate} or length {len(samples)} out of range')
        clips.append((name, rate, len(samples), adpcm_encode(samples)))
    track_names = list(m['tracks'])
    tracks, missing = [], []
    for name in track_names:
        meta = tracks_meta.get(name)
        mp2 = SOUND / 'music' / f'{name}.mp2'
        if not meta or not mp2.exists():
            missing.append(name)
            continue
        data = mp2.read_bytes()
        if hashlib.sha256(data).hexdigest() != meta['sha256']:
            raise ValueError(f'{mp2}: SHA256 differs from tracks.json')
        frames, fb = mp2_frames(data, name)
        loops = 'loop_end' in meta
        tracks.append((name, data, frames, fb, meta.get('loop_start', 0), meta.get('loop_end', 0),
                       1 if loops else 0, int(m['tracks'][name].get('level', 1000))))
    track_index = {t[0]: i for i, t in enumerate(tracks)}

    def scope_of(text, what, music=False):
        if music and text in stages:
            return scope16(text, what)
        if text and text not in systems and text not in chars:
            raise ValueError(f'{what}: scope {text!r} is neither a system nor a character' +
                             (' nor a stage' if music else ''))
        return scope16(text, what)

    cues = []
    for i, c in enumerate(m['cues']):
        what = f'cue {i} ({c.get("scope", "")} {c["event"]})'
        if c['event'] not in events:
            raise ValueError(f'{what}: unknown event')
        scope = c.get('scope', '')
        move = 0
        if 'move' in c:
            names = chars[scope] if scope in chars else list(pack.MOVES)
            if c['move'] not in names:
                raise ValueError(f'{what}: unknown move {c["move"]}')
            move = names.index(c['move']) + 1
        variants = c['clips']
        if not 1 <= len(variants) <= 4:
            raise ValueError(f'{what}: 1..4 variants')
        ids = [0xffff if v is None else clip_names.index(v) for v in variants]
        if c.get('kind', 'sfx') not in KINDS or c.get('target', 'self') not in ('self', 'other'):
            raise ValueError(f'{what}: kind is sfx, voice or announcer, target self or other')
        flags = KINDS.index(c.get('kind', 'sfx')) | (FLAG_OTHER if c.get('target') == 'other' else 0)
        gain = int(round(c.get('gain', 1.0) * 256))
        if not 0 <= gain <= 1024:
            raise ValueError(f'{what}: gain')
        cues.append(scope_of(scope, what) + struct.pack('<BBBB4HHH', events.index(c['event']),
                                                        flags, move, len(ids),
                                                        *(ids + [0xffff] * (4 - len(ids))),
                                                        gain, 0))
    music_rows = []
    for i, c in enumerate(m['music']):
        what = f'music {i} ({c.get("scope", "")} {c["cue"]})'
        ids = [track_index[t] for t in c['tracks'] if t in track_index]
        if not ids:
            continue
        if len(ids) > 6:
            raise ValueError(f'{what}: at most 6 tracks')
        music_rows.append(scope_of(c.get('scope', ''), what, music=True) +
                          struct.pack('<BB6HH', MUSIC_CUES.index(c['cue']), len(ids),
                                      *(ids + [0] * (6 - len(ids))), 0))
    if len(clips) > MAX_CLIPS or len(tracks) > MAX_TRACKS or len(cues) > MAX_CUES or \
            len(music_rows) > MAX_MUSIC:
        raise ValueError('too many clips, tracks or cues')
    table = 32 + 32 * len(clips) + 48 * len(tracks) + 32 * len(cues) + 32 * len(music_rows)
    data_off = (table + 15) & ~15
    body = bytearray()
    clip_rows = []
    for name, rate, n, data in clips:
        clip_rows.append(name16(name, name) + struct.pack('<IIII', data_off + len(body), n, rate, 0))
        body += data
        body += bytes(-len(body) % 4)
    track_rows = []
    for name, data, frames, fb, ls, le, flags, level in tracks:
        track_rows.append(name16(name, name) +
                          struct.pack('<8I', data_off + len(body), frames, fb, ls, le, flags,
                                      level, 0))
        body += data
        body += bytes(-len(body) % 4)
    out = bytearray(32)
    for row in clip_rows + track_rows + cues + music_rows:
        out += row
    out += bytes(data_off - len(out))
    out += body
    struct.pack_into('<8sII', out, 0, MAGIC, VERSION, len(out))
    struct.pack_into('<4HI', out, 20, len(clips), len(tracks), len(cues), len(music_rows),
                     data_off)
    struct.pack_into('<I', out, 16, zlib.crc32(out[20:]))
    pathlib.Path(output).write_bytes(out)
    sfx = sum(len(c[3]) for c in clips)
    mus = sum(len(t[1]) for t in tracks)
    print(f'{output}: {len(out) // 1024} KiB, {len(clips)} clips ({sfx // 1024} KiB ADPCM, '
          f'{len(synthesized)} synthesized), {len(tracks)} tracks ({mus // 1024} KiB MP2), '
          f'{len(cues)} cues, {len(music_rows)} music cues')
    for name in synthesized:
        print(f'  {name}: no WAV, synthesized {m["clips"][name]["synth"]}')
    for name in missing:
        print(f'  {name}: no MP2 or tracks.json entry, left out')


def main():
    args = sys.argv[1:]
    if len(args) == 3 and args[0] == 'build':
        build(args[1], args[2])
    elif args == ['extract']:
        extract()
    elif len(args) == 2 and args[0] == 'music':
        if not shutil.which('ffmpeg'):
            sys.exit('sound.py: needs ffmpeg')
        music(args[1])
    elif len(args) == 3 and args[0] == 'synth':
        write_wav(args[2], 24000, synth(args[1]))
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, KeyError, OSError) as e:
        sys.exit(f'sound.py: {e}')
