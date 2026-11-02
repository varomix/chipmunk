#!/usr/bin/env python3
"""Turns a FamiStudio .fms test song into a note-event script for the golden test.

Ports just enough of FamiStudio's project loading, tempo and BasePlayer/ChannelState.Advance
logic to know what each channel is told to play on each frame. The channel behaviour itself
is left to the C++ engine, whose register writes are then compared with FamiStudio's trace.

Usage: fms_to_script.py Song.fms Song_FamiStudioRef.txt out_dir
"""
import gzip
import math
import os
import re
import struct
import sys
import zlib

NOTE_STOP, NOTE_RELEASE, NOTE_INVALID = 0x00, 0x80, 0xff
FX_VOLUME, FX_VIB_SPEED, FX_VIB_DEPTH, FX_FINE, FX_SPEED, FX_FDS_DEPTH, FX_FDS_SPEED = 0, 1, 2, 3, 4, 5, 6
FX_DUTY, FX_DELAY, FX_CUT, FX_VOL_SLIDE, FX_DMC, FX_PHASE, FX_ENV_PERIOD = 7, 8, 9, 10, 11, 12, 13
MASK_VIB = (1 << FX_VIB_SPEED) | (1 << FX_VIB_DEPTH)
TEMPO_FAMITRACKER = 1
PAD_BEGIN, PAD_MIDDLE, PAD_END = 0, 1, 2


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


class Reader:
    def __init__(self, data, pos=0):
        self.d, self.p = data, pos

    def take(self, fmt):
        v = struct.unpack_from('<' + fmt, self.d, self.p)[0]
        self.p += struct.calcsize('<' + fmt)
        return v

    def i32(self): return self.take('i')
    def u16(self): return self.take('H')
    def i16(self): return self.take('h')
    def u8(self): return self.take('B')
    def i8(self): return self.take('b')
    def boolean(self): return self.u8() != 0

    def string(self):
        n = self.i32()
        if n < 0:
            return ''
        if n > 400 or n % 2:
            raise ValueError('bad string')
        s = self.d[self.p:self.p + n].decode('utf-16le')
        self.p += n
        return s

    def int_array(self):
        n = self.i32()
        if n < 0:
            return None
        if n > 256:
            raise ValueError('bad array')
        return [self.i32() for _ in range(n)]

    def sbyte_array(self):
        n = self.i32()
        if n < 0 or n > 1024:
            raise ValueError('bad array')
        return [self.i8() for _ in range(n)]


class Note:
    def __init__(self, value=NOTE_INVALID):
        self.val, self.flags, self.slide, self.instrument = value, 0, 0, -1
        self.duration, self.release, self.mask, self.arp = 0, 0, 0, -1
        self.volume = self.vibrato = self.speed = self.fine = self.duty = 0
        self.delay = self.cut = self.vol_slide = self.phase = self.dmc = self.env_period = 0
        self.fds_speed = self.fds_depth = 0

    def clone(self):
        n = Note()
        n.__dict__.update(self.__dict__)
        return n

    def has(self, fx): return (self.mask >> fx) & 1 != 0
    def valid(self): return self.val != NOTE_INVALID
    def musical(self): return self.valid() and self.val not in (NOTE_STOP, NOTE_RELEASE)
    def musical_or_stop(self): return self.musical() or self.val == NOTE_STOP
    def cut_filter(self): return self.musical_or_stop() or self.has(FX_CUT)


def read_note(r):
    n = Note()
    n.val, n.flags = r.u8(), r.u8()
    if n.musical():
        n.slide, n.instrument = r.u8(), r.i32()
        n.duration, n.release = r.u16(), r.u16()
    elif n.val == NOTE_STOP:
        n.duration = 1
    n.mask = r.u16()
    if n.has(FX_VOLUME): n.volume = r.u8()
    if n.mask & MASK_VIB: n.vibrato = r.u8()
    if n.has(FX_SPEED): n.speed = r.u8()
    if n.has(FX_FINE): n.fine = r.i8()
    if n.has(FX_FDS_SPEED): r.u16()
    if n.has(FX_FDS_DEPTH): r.u8()
    if n.has(FX_DUTY): n.duty = r.u8()
    if n.has(FX_DELAY): n.delay = r.u8()
    if n.has(FX_CUT): n.cut = r.u8()
    if n.has(FX_VOLUME) and n.has(FX_VOL_SLIDE): n.vol_slide = r.u8()
    if n.has(FX_DMC): n.dmc = r.u8()
    if n.has(FX_PHASE): n.phase = r.u8()
    if n.has(FX_ENV_PERIOD): n.env_period = r.u16()
    n.arp = r.i32()
    if n.slide == n.val:
        n.slide = 0
    return n


def read_envelope(r):
    return dict(length=r.i32(), loop=r.i32(), release=r.i32(), relative=r.boolean(), values=r.sbyte_array())


def read_instrument(r):
    inst_id, name = r.i32(), r.string()
    r.i32()  # color
    expansion = r.i32()
    settings = dict(saw=0, shape=0, auto=1, octave=3, period=1000)
    if expansion == 1:  # VRC6 saw master volume
        settings['saw'] = r.u8()
    elif expansion == 6:  # Sunsoft 5B envelope
        settings['auto'], settings['octave'], settings['shape'], settings['period'] = r.u8(), r.i8(), r.u8(), r.u16()
    elif expansion != 0:
        raise ValueError('expansion %d not handled' % expansion)
    mask = r.u16()
    envelopes = {i: read_envelope(r) for i in range(10) if mask & (1 << i)}
    r.string()  # folder
    if expansion == 0:
        count = r.i32()
        if count < 0 or count > 96:
            raise ValueError('bad mapping count')
        for _ in range(count):
            r.i32()
        for _ in range(count):
            r.i32(); r.boolean(); r.i32(); r.boolean(); r.i32()  # DPCM mappings
    return dict(id=inst_id, name=name, envelopes=envelopes, settings=settings)


def read_project_tail(data, start, num_channels, version):
    r = Reader(data, start)
    count = r.i32()
    if not 1 <= count <= 64:
        raise ValueError('bad instrument count')
    instruments = [read_instrument(r) for _ in range(count)]

    arps = []
    count = r.i32()
    if not 0 <= count <= 64:
        raise ValueError('bad arpeggio count')
    for _ in range(count):
        arp_id, _name = r.i32(), r.string()
        r.i32(); r.string()
        arps.append(dict(id=arp_id, envelope=read_envelope(r)))

    if r.i32() < 1:
        raise ValueError('no songs')

    s = {}
    r.i32()
    s['pattern_length'], s['length'], _beat = r.i32(), r.i32(), r.i32()
    if not 0 < s['length'] <= 256:
        raise ValueError('bad song')
    r.string()
    s['ft_tempo'], s['ft_speed'] = r.i32(), r.i32()
    r.i32()
    s['loop_point'], _note_length = r.i32(), r.i32()
    s['groove'] = r.int_array()
    s['pad'] = r.i32() if version >= 19 else PAD_MIDDLE
    s['custom'] = []
    for _ in range(s['length']):
        use, plen, _nl, _bl = r.boolean(), r.i32(), r.i32(), r.i32()
        groove, pad = r.int_array(), r.i32()
        s['custom'].append((use, plen, groove, pad))
    r.string()

    s['channels'] = []
    for _ in range(num_channels):
        r.i32()
        pattern_count, ch_type = r.i32(), r.i32()
        patterns = {}
        for _ in range(pattern_count):
            pid = r.i32()
            r.string(); r.i32(); r.i32(); r.i32()
            notes = {}
            for _ in range(r.i32()):
                t = r.i16()
                notes[t] = read_note(r)
            patterns[pid] = notes
        inst = [r.i32() for _ in range(256)]
        s['channels'].append(dict(type=ch_type, instances=[patterns.get(i) for i in inst]))

    if r.p != len(data):
        raise ValueError('did not consume the file')
    return instruments, arps, s


def load(path):
    raw = open(path, 'rb').read()
    version = struct.unpack_from('<i', raw, 4)[0]
    if raw[:4] != b'FMS!' or not 16 <= version <= 19:
        raise SystemExit('expected a version 16 to 19 .fms file')
    data = zlib.decompress(raw[12:], -15)

    r = Reader(data)
    r.i32()
    for _ in range(4): r.u8()
    r.string(); r.string(); r.string()
    expansion_mask, _n163, tempo_mode, pal = r.i32(), r.i32(), r.i32(), r.boolean()
    if pal:
        raise SystemExit('PAL songs are not handled')

    # The export settings and DPCM data ahead of the instruments don't matter here,
    # so find the instruments by locating the offset where the rest parses exactly.
    # Bits are VRC6, VRC7, FDS, MMC5, N163, S5B, EPSM.
    num_channels = 5 + (3 if expansion_mask & 1 else 0) + (2 if expansion_mask & 8 else 0) + (3 if expansion_mask & 32 else 0)
    if expansion_mask & ~(1 | 8 | 32):
        raise SystemExit('only 2A03, VRC6, MMC5 and S5B songs are handled')
    for start in range(r.p, len(data) - 12):
        try:
            return (*read_project_tail(data, start, num_channels, version), tempo_mode)
        except (ValueError, struct.error, UnicodeDecodeError, IndexError):
            continue
    raise SystemExit('could not locate the instruments')


class Song:
    def __init__(self, song, tempo_mode):
        self.__dict__.update(song)
        self.ft = tempo_mode == TEMPO_FAMITRACKER

    def plen(self, p):
        c = self.custom[p] if p < self.length else None
        return c[1] if c and c[0] else self.pattern_length

    def groove_of(self, p):
        c = self.custom[p]
        return (c[2], c[3]) if c[0] else (self.groove, self.pad)

    def advance(self, loc, count):
        p, n = loc
        if count > 0:
            n += count
            while True:
                if p >= self.length:
                    return (self.length, 0)
                if n < self.plen(p):
                    return (p, n)
                n -= self.plen(p)
                p += 1
                if p >= self.length:
                    return (self.length, 0)
        return (p, n)

    def abs_index(self, loc):
        return sum(self.plen(p) for p in range(loc[0])) + loc[1]

    def speed_at(self, loc, current):
        for ch in self.channels:
            pat = ch['instances'][loc[0]]
            if pat:
                n = pat.get(loc[1])
                if n and n.has(FX_SPEED):
                    current = n.speed
        return current

    def frames_between(self, l0, l1, speed):
        if self.ft:
            total = 0.0
            p, n = l0
            while (p, n) != l1 and p < self.length:
                speed = self.speed_at((p, n), speed)
                total = f32(total + f32(speed * f32(150 / f32(self.ft_tempo))))
                n += 1
                if n >= self.plen(p):
                    n, p = 0, p + 1
            return total

        def gl(length, p):
            groove, pad = self.groove_of(p)
            return groove_frames(length, groove, pad)

        p0, n0 = l0
        p1, n1 = l1
        if p0 == p1:
            return gl(n1, p1) - gl(n0, p0)
        total = 0
        if n0 != 0:
            total = gl(self.plen(p0), p0) - gl(n0, p0)
            p0 += 1
        while p0 != p1:
            total += gl(self.plen(p0), p0)
            p0 += 1
        if n1 != 0:
            total += gl(n1, p0)
        return total


class Groove:
    def __init__(self, groove, pad):
        self.groove, self.pad, self.note_length = groove, pad, min(groove)
        self.frame = self.gframe = self.gidx = 0

    def advance(self):
        self.frame += 1
        self.gframe += 1
        if self.gframe == self.groove[self.gidx]:
            self.gframe = 0
            self.gidx += 1
            if self.gidx == len(self.groove):
                self.gidx = 0

    def pad_frame(self):
        if self.groove[self.gidx] == self.note_length:
            return False
        return ((self.pad == PAD_BEGIN and self.gframe == 0) or
                (self.pad == PAD_MIDDLE and self.gframe == self.note_length // 2) or
                (self.pad == PAD_END and self.gframe == self.note_length))


def groove_frames(length, groove, pad):
    it = Groove(groove, pad)
    value = 0
    for _ in range(length + 1):
        if it.pad_frame():
            it.advance()
        value = it.frame
        it.advance()
    return value


def note_table(kind, num_n163=1):
    """The channel's periods, from NesApu.GetOrCreateNoteTableSet. "kind" is the channel type."""
    ratio = 1.0 / 13.454340859610068739450573644169
    clock, c1 = 1789773 / 16.0, 440 * ratio
    table = [0] * 97
    for i in range(1, 97):
        octave = (i - 1) // 12
        freq = c1 * 2.0 ** ((i - 1) / 12.0)
        if kind == 7:
            table[i] = int((clock * 16.0) / (freq * 14.0) - 0.5)
        elif kind == 14:
            table[i] = int((freq * 65536.0) / clock + 0.5)
        elif 18 <= kind <= 25:
            table[i] = int(min(0xffff, ((freq * num_n163 * 983040.0) / clock) / 4))
        elif 8 <= kind <= 13:
            table[i] = int(freq * 262144.0 / 49715.0 + 0.5) if octave == 0 else (table[(i - 1) % 12 + 1] << octave) & 0xffff
        elif 29 <= kind <= 31:
            table[i] = round((8000000 / 32.0) / freq)
        elif 32 <= kind <= 37:
            table[i] = int((144 * freq * 1048576 / 8000000) / 4) if octave == 0 else (table[(i - 1) % 12 + 1] << octave) & 0xffff
        else:
            table[i] = int(clock / freq - 0.5)
    return table


def slide_shift(kind, num_n163=1):
    """From Channel.GetShiftsForType."""
    if 8 <= kind <= 13 or 32 <= kind <= 37:
        return 3
    if 18 <= kind <= 25:
        return 2 if num_n163 == 1 else 3 if num_n163 == 2 else 4 if num_n163 <= 4 else 5
    return -4 if kind == 3 else -1


class Channel:
    def __init__(self, index, song, data, emit):
        self.index, self.song, self.data, self.emit = index, song, data, emit
        self.kind = data['type']
        self.release = self.duration = self.delay_counter = self.cut_counter = 0
        self.delayed = None
        self.delayed_slide = (0, 0)
        num_n163 = sum(1 for c in song.channels if 18 <= c['type'] <= 25)
        self.table = note_table(self.kind, num_n163)
        self.shift = slide_shift(self.kind, num_n163)

    def note_at(self, loc):
        pat = self.data['instances'][loc[0]] if loc[0] < 256 else None
        return pat.get(loc[1]) if pat else None

    def distance_to_next(self, loc):
        s = self.song
        start = loc
        loc = s.advance(loc, 1)
        if loc[0] < s.length:
            pat = self.data['instances'][loc[0]]
            if pat:
                for t in sorted(pat):
                    if t >= loc[1] and t < s.plen(loc[0]) and pat[t].cut_filter():
                        return s.abs_index((loc[0], t)) - s.abs_index(start)
            for p in range(loc[0] + 1, s.length):
                pat = self.data['instances'][p]
                if pat:
                    for t in sorted(pat):
                        if t < s.plen(p) and pat[t].cut_filter():
                            return s.abs_index((p, t)) - s.abs_index(start)
        return s.abs_index((s.length, 0)) - s.abs_index(start)

    def slide_params(self, n, loc, speed):
        s = self.song
        if self.kind == 3:
            delta = n.val - n.slide
        else:
            delta = self.table[n.val] - self.table[n.slide]
        if delta == 0:
            return 0, 0
        delta = delta << -self.shift if self.shift < 0 else delta >> self.shift

        next_loc = loc
        if not n.has(FX_CUT):
            distance = min(self.distance_to_next(loc), 256)
            next_loc = s.advance(loc, min(n.duration, distance))

        frames = 0.0
        if loc != next_loc:
            delay = -(n.delay if n.has(FX_DELAY) else 0)
            if s.ft:
                nn = self.note_at(next_loc)
                if nn:
                    if nn.has(FX_DELAY):
                        delay += min(nn.delay, nn.cut) if nn.has(FX_CUT) else nn.delay
                    elif nn.has(FX_CUT):
                        delay += nn.cut
            frames = s.frames_between(loc, next_loc, speed) + delay
        else:
            frames = n.cut if n.has(FX_CUT) else 0

        step = f32(abs(delta) / f32(max(1, frames)))
        sign = (delta > 0) - (delta < 0)
        return delta, max(-128, min(127, int(math.ceil(step)) * -sign))

    def volume_slide_step(self, n, loc, speed):
        s = self.song
        delta = n.volume - n.vol_slide
        if delta == 0:
            return 0
        max_loc = s.advance(loc, 256)
        cur = s.advance(loc, 1)
        next_loc = None
        pat = self.data['instances'][loc[0]]
        if cur[0] == loc[0]:
            for t in sorted(pat):
                if t >= cur[1] and pat[t].has(FX_VOLUME):
                    next_loc = (loc[0], t)
                    break
        if next_loc is None:
            for p in range(loc[0] + 1, s.length):
                pat = self.data['instances'][p]
                if pat:
                    hits = [t for t in sorted(pat) if pat[t].has(FX_VOLUME) and t < s.plen(p)]
                    if hits:
                        next_loc = (p, hits[0])
                        break
        if next_loc is None or next_loc > max_loc:
            next_loc = max_loc

        delay = -(n.delay if n.has(FX_DELAY) else 0)
        if s.ft:
            nn = self.note_at(next_loc)
            if nn and nn.has(FX_DELAY):
                delay += nn.delay
        frames = s.frames_between(loc, next_loc, speed) + delay
        delta <<= 4
        step = f32(abs(delta) / f32(max(1, frames)))
        sign = (delta > 0) - (delta < 0)
        return max(-128, min(127, int(math.ceil(step)) * -sign))

    def play(self, n, slide_pitch=0, slide_step=0, vol_step=0):
        if n.valid():
            self.cut_counter = 0
        if n.has(FX_CUT):
            self.cut_counter = n.cut + 1
        self.emit(self.index, n, slide_pitch, slide_step, vol_step)

    def advance_channel(self, loc, ft_speed):
        s = self.song
        if self.delayed is not None:
            self.play(self.delayed, *self.delayed_slide)
            self.delayed = None
            self.delay_counter = 0

        n = self.note_at(loc)
        if n is not None and loc[1] >= s.plen(loc[0]):
            n = None

        if self.release > 0:
            self.release -= 1
            if self.release == 0 and (n is None or not n.musical_or_stop()):
                n = Note() if n is None else n.clone()
                n.val = NOTE_RELEASE
        if self.duration > 0:
            self.duration -= 1
            if self.duration == 0 and (n is None or not n.musical_or_stop()):
                n = Note() if n is None else n.clone()
                n.val = NOTE_STOP

        if n is None:
            return ft_speed

        if n.has(FX_SPEED):
            ft_speed = n.speed
        if n.musical_or_stop():
            self.release = 0

        slide = (0, 0)
        if n.musical():
            if n.slide:
                slide = self.slide_params(n, loc, ft_speed)
            if n.release:
                if s.advance(loc, n.release)[0] < s.length:
                    self.release = n.release
            self.duration = n.duration if s.advance(loc, n.duration)[0] < s.length else 0

        vol_step = None
        if n.has(FX_VOL_SLIDE) and n.has(FX_VOLUME):
            vol_step = self.volume_slide_step(n, loc, ft_speed)

        if n.has(FX_DELAY):
            if vol_step is not None:
                self.emit(self.index, None, 0, 0, vol_step)
            self.delayed = n
            self.delay_counter = n.delay + 1
            self.delayed_slide = slide
            return ft_speed

        self.play(n, slide[0], slide[1], vol_step or 0)
        return ft_speed

    def before_update(self):
        if self.delayed is not None:
            self.delay_counter -= 1
            if self.delay_counter == 0:
                self.play(self.delayed, *self.delayed_slide)
                self.delayed = None
        if self.cut_counter > 0:
            self.cut_counter -= 1
            if self.cut_counter == 0:
                self.play(Note(NOTE_STOP))


def song_to_lines(instruments, arps, song_data, tempo_mode):
    """The script's instruments, arpeggios and per-frame note events. Returns (lines, frame count)."""
    song = Song(song_data, tempo_mode)

    lines = []
    for inst in instruments:
        lines.append('I %d' % inst['id'])
        c = inst['settings']
        lines.append('X %d %d %d %d %d' % (c['saw'], c['shape'], c['auto'], c['octave'], c['period']))
        for t, e in inst['envelopes'].items():
            # Files number the S5B envelopes 8 and 9. The engine has them right after the regular four.
            ours = t if t < 4 else {8: 4, 9: 5}.get(t)
            if ours is not None:
                lines.append('E %d %d %d %d %d %s' % (ours, e['length'], e['loop'], e['release'], e['relative'],
                                                      ' '.join(map(str, e['values'][:e['length']]))))
        for extra in inst.get('extra', []):
            lines.append(extra)
    for arp in arps:
        e = arp['envelope']
        lines.append('A %d %d %d %d %s' % (arp['id'], e['length'], e['loop'], e['release'],
                                           ' '.join(map(str, e['values'][:e['length']]))))

    frame = [0]

    def emit(index, n, slide_pitch, slide_step, vol_step):
        if n is None:
            lines.append('V %d %d %d' % (frame[0], index, vol_step))
            return
        has_attack = 0 if n.flags & 1 else 1
        vib = n.mask & MASK_VIB != 0
        vs = n.has(FX_VOLUME) and n.has(FX_VOL_SLIDE)
        fields = [
            frame[0], index, n.val, has_attack, n.instrument, n.arp, n.slide,
            int(n.has(FX_FINE)), n.fine, int(vib), n.vibrato >> 4, n.vibrato & 15,
            int(n.has(FX_DUTY)), n.duty, int(n.has(FX_VOLUME)), n.volume,
            int(vs), n.vol_slide, int(n.has(FX_PHASE)), slide_pitch, slide_step, vol_step,
            int(n.has(FX_DMC)), n.dmc, int(n.has(FX_ENV_PERIOD)), n.env_period]
        if n.mask & ((1 << FX_FDS_SPEED) | (1 << FX_FDS_DEPTH)):
            fields += [int(n.has(FX_FDS_SPEED)), n.fds_speed, int(n.has(FX_FDS_DEPTH)), n.fds_depth]
        lines.append('N ' + ' '.join(map(str, fields)))

    channels = [Channel(i, song, c, emit) for i, c in enumerate(song.channels)]
    for c in channels:
        lines.append('C %d %d' % (c.index, c.kind))
    loc = None
    ft_speed, ft_counter = song.ft_speed, 0
    groove = None

    while True:
        if loc is None or (ft_counter <= 0 if song.ft else not groove.pad_frame()):
            new_pattern = False
            if loc is None:
                loc, new_pattern = (0, 0), True
            else:
                p, n = loc[0], loc[1] + 1
                if n >= song.plen(p):
                    p, n, new_pattern = p + 1, 0, True
                if p >= song.length:
                    break
                loc = (p, n)
            if new_pattern and not song.ft:
                groove = Groove(*song.groove_of(loc[0]))
            for c in channels:
                ft_speed = c.advance_channel(loc, ft_speed)

        for c in channels:
            c.before_update()

        if song.ft:
            dec, rem = (song.ft_tempo * 24) // ft_speed, (song.ft_tempo * 24) % ft_speed
            if ft_counter <= 0:
                ft_counter += 3600 - rem
            ft_counter -= dec
        else:
            groove.advance()
        frame[0] += 1

    lines.append('T %d' % frame[0])
    return lines, frame[0]


def main():
    fms, ref, out_dir = sys.argv[1:4]
    name = re.sub(r'\.fms$', '', os.path.basename(fms))
    instruments, arps, song_data, tempo_mode = load(fms)
    lines, total_frames = song_to_lines(instruments, arps, song_data, tempo_mode)

    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, name + '.script'), 'w') as f:
        f.write('\n'.join(lines) + '\n')

    # Every channel in these songs is in scope now, so this is FamiStudio's whole trace.
    with open(ref, 'rb') as src:
        trace = src.read().replace(b'\r\n', b'\n')
    with open(os.path.join(out_dir, name + '.trace.gz'), 'wb') as raw_out:
        with gzip.GzipFile(filename='', mode='wb', fileobj=raw_out, mtime=0) as dst:
            dst.write(trace)
    count = trace.count(b'\n')
    print('%s: %d frames, %d note lines, %d reference writes' % (name, total_frames, len(lines), count))


if __name__ == '__main__':
    main()
