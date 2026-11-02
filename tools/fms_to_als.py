#!/usr/bin/env python3
"""Turns a song from a FamiStudio project into a Live Set, with a Chipmunk device on each track.

Every effect becomes something editable in Live, using the plugin's MIDI controllers:
each mix of channel, instrument and arpeggio gets its own track, volumes become velocity and
clip envelopes, and slides become overlapping notes with a slide time. Noise and DPCM share one
Drums track, with a Drum Kit device whose keys choose the sound.

    FamiStudio Project.fms famistudio-txt-export Project.txt
    python3 tools/fms_to_als.py <base.als> Project.txt <song index> <out.als>

The base Set is the one make_live_templates.py uses, which gives the shape of tracks and clips.
"""
import base64
import gzip
import math
import pathlib
import re
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import fms_to_script as fms
import fms_txt_to_script as txt
from make_live_templates import build
from make_sample_songs import make_clip

FRAMES_PER_SECOND = 60.0988
NOTE_STOP, NOTE_RELEASE, NOTE_INVALID = 0, 0x80, 0xff

# MidiEffect in src/engine/VoiceController.h, with each controller's value at rest.
CC_DEPTH, CC_FINE, CC_SLIDE, CC_VOLUME, CC_DUTY, CC_PHASE, CC_SPEED, CC_FDS_SPEED, CC_FDS_DEPTH, CC_STOP_SAMPLE, CC_STOP, CC_HOLD_RELEASE = 1, 3, 5, 11, 71, 73, 76, 77, 78, 85, 86, 87
# Live's controller slots start with pitch bend and channel pressure, so CC 0 is ControllerTargets.2.
CONTROLLER_SLOT_OF_CC_0 = 2
RESTING = {CC_DEPTH: 0, CC_FINE: 64, CC_SLIDE: 0, CC_VOLUME: 127, CC_DUTY: 0, CC_PHASE: 0, CC_SPEED: 6, CC_STOP: 0}

# Levels the plugin takes off these channels, which go back on to match FamiStudio's mix.
TRIM_DB = {'n163': -11, 'vrc7': -7, 'epsmfm': -6, 'epsmrhythm': -4}


KIND_NOISE, KIND_DPCM, KIND_DRUMS = 3, 4, -1
FIRST_KEY, LAST_KEY, FIRST_NOISE_KEY = 24, 119, 36


def channel_name(kind):
    if kind == KIND_DRUMS: return 'drumkit'
    if kind in (0, 1): return 'pulse'
    if kind == 2: return 'triangle'
    if kind == 3: return 'noise'
    if kind == 4: return 'dpcm'
    if kind in (5, 6): return 'vrc6pulse'
    if kind == 7: return 'vrc6saw'
    if 8 <= kind <= 13: return 'vrc7'
    if kind == 14: return 'fds'
    if kind in (15, 16): return 'mmc5pulse'
    if 18 <= kind <= 25: return 'n163'
    if 26 <= kind <= 28: return 's5b'
    if 29 <= kind <= 31: return 'epsmsquare'
    if 32 <= kind <= 37: return 'epsmfm'
    return 'epsmrhythm'


SHORT_NAMES = {0: 'Sq1', 1: 'Sq2', 2: 'Tri', 3: 'Noi', 4: 'DPCM', 5: 'VRC6 1', 6: 'VRC6 2', 7: 'Saw', 14: 'FDS', 15: 'MMC5 1', 16: 'MMC5 2',
               **{8 + i: 'VRC7 %d' % (i + 1) for i in range(6)}, **{18 + i: 'N163 %d' % (i + 1) for i in range(8)},
               **{26 + i: 'S5B %d' % (i + 1) for i in range(3)}, **{29 + i: 'PSG %d' % (i + 1) for i in range(3)},
               **{32 + i: 'FM %d' % (i + 1) for i in range(6)}, **{38 + i: 'Drum %d' % (i + 1) for i in range(6)}}
RHYTHM_FIRST = 38
COLOURS = {'pulse': 25, 'triangle': 17, 'noise': 19, 'dpcm': 3, 'vrc6pulse': 5, 'vrc6saw': 6, 'mmc5pulse': 9, 's5b': 13, 'fds': 21, 'n163': 29, 'vrc7': 33, 'epsmsquare': 37, 'epsmfm': 41, 'epsmrhythm': 45, 'drumkit': 19}


def format_envelope(e):
    """The text form that the plugin reads: values, with | before the loop and / before the release."""
    parts = []
    for i in range(e['length']):
        if i == e['loop']:
            parts.append('|')
        if i == e['release']:
            parts.append('/')
        parts.append(str(e['values'][i]))
    return ' '.join(parts)


def sound_text(name, kind, instrument, arpeggio, kit, n163_channels):
    """The whole sound as a .chmk file, and the values that also belong in the device's parameters."""
    envelopes = instrument['envelopes'] if instrument else {}
    settings = instrument['settings'] if instrument else dict(saw=1, shape=0, auto=1, octave=3, period=1000)
    channel = channel_name(kind)
    keys = {0: 'volume', 1: 'arpeggio', 2: 'pitch', 3: 'dutyEnvelope', 8: 's5bMixer', 9: 's5bNoise'}
    lines = ['Chipmunk preset 1', 'name=' + name, 'channel=' + channel, 'duty=0', 'velocity=1', 'legato=1', 'glide=0', 'bendRange=2', 'vibratoSpeed=6']

    for number, key in keys.items():
        envelope = arpeggio if number == 1 and arpeggio is not None else envelopes.get(number)
        if envelope is not None and envelope['length'] > 0:
            # The plugin keeps its envelopes to 256 steps and arpeggios to its own range, as FamiStudio does.
            lines.append(key + '=' + format_envelope(envelope))

    if 2 in envelopes and envelopes[2]['relative']:
        lines.append('pitchRelative=1')

    lines += ['sawVolume=%d' % settings['saw'], 's5bShape=%d' % settings['shape'], 's5bAutoPitch=%d' % settings['auto'],
              's5bOctave=%d' % settings['octave'], 's5bPeriod=%d' % settings['period']]

    a = instrument['chip'] if instrument else {}
    wave = envelopes.get(4) or envelopes.get(6)
    if wave is not None:
        lines += ['waveSize=%s' % ('64' if 4 in envelopes else a.get('N163WaveSize', '16')), 'wave=' + ' '.join(map(str, wave['values']))]
        repeat = envelopes.get(7)
        if repeat is not None:
            lines += ['waveRepeatPoints=%d %d' % (repeat['loop'], repeat['release']), 'waveRepeat=' + ' '.join(map(str, repeat['values']))]
    if 5 in envelopes:
        lines.append('fdsModulation=' + ' '.join(map(str, envelopes[5]['values'])))
    if channel == 'fds':
        lines.append('fds=%s %s %s %s %d %s %s' % (a.get('FdsMasterVolume', 0), a.get('FdsModSpeed', 0), a.get('FdsModDepth', 0), a.get('FdsModDelay', 0),
                                                   a.get('FdsAutoMod') == 'True', a.get('FdsAutoModNumer', 1), a.get('FdsAutoModDenom', 1)))
    if channel == 'n163':
        lines.append('n163Position=' + a.get('N163WavePos', '0'))
    if channel == 'vrc7':
        lines.append('vrc7=%s %d %d' % (a.get('Vrc7Patch', 1), a.get('Vrc7OverrideRelease') == 'True', a.get('Vrc7OverrideStop') == 'True'))
        lines.append('vrc7Registers=' + ' '.join(str(a.get('Vrc7Reg%d' % i, 0)) for i in range(8)))
    if channel in ('epsmfm', 'epsmrhythm') and a.get('EpsmPatch', '1') == '0':
        lines.append('epsmRegisters=' + ' '.join(str(a.get('EpsmReg%d' % i, 0)) for i in range(31)))

    written = []
    for note_value, pitch, loop, initial, data in kit:
        if data not in [w[1] for w in written]:
            written.append((initial, data))
            lines.append('sample=%d %s sample%d' % (initial, base64.b64encode(bytes.fromhex(data)).decode(), len(written)))
    for note_value, pitch, loop, initial, data in kit:
        index = [w[1] for w in written].index(data)
        lines.append('key=%d %d %d %d 0 32' % (note_value, index, pitch, loop))

    text = '\n'.join(lines) + '\n'
    values = dict(line.split('=', 1) for line in lines if '=' in line and not line.startswith(('sample=', 'key=', 'wave=')))
    values.update(gain=-TRIM_DB.get(channel, 0), n163Channels=n163_channels)
    return text, values


def drums_text(keys, by_id, arp_by_id, kit_for):
    """The Drum Kit sound: "keys" gives the key of each (channel, instrument, arpeggio, note value)."""
    lines = ['Chipmunk preset 1', 'name=Drums', 'channel=drumkit', 'duty=0', 'velocity=1', 'legato=1', 'glide=0', 'bendRange=2', 'vibratoSpeed=6']

    samples = []
    key_lines = []
    for (kind, instrument_id, _, value), key in sorted(keys.items(), key=lambda item: item[1]):
        if kind != KIND_DPCM:
            continue
        for note_value, pitch, loop, initial, data in kit_for.get(instrument_id, []):
            if note_value == value:
                if data not in [d for _, d in samples]:
                    samples.append((initial, data))
                    lines.append('sample=%d %s sample%d' % (initial, base64.b64encode(bytes.fromhex(data)).decode(), len(samples)))
                index = [d for _, d in samples].index(data)
                key_lines.append('key=%d %d %d %d 0 32' % (key - 23, index, pitch, loop))
    lines += key_lines

    sounds = []
    for (kind, instrument_id, arp_id, value), key in sorted(keys.items(), key=lambda item: item[1]):
        if kind != KIND_NOISE:
            continue
        if (instrument_id, arp_id) not in sounds:
            sounds.append((instrument_id, arp_id))
            instrument = by_id.get(instrument_id)
            envelopes = dict(instrument['envelopes']) if instrument else {}
            if arp_id in arp_by_id:
                envelopes[1] = arp_by_id[arp_id]
            name = instrument['name'] if instrument else 'Noise'
            lines.append('sound=' + name + (' arp' if arp_id in arp_by_id else ''))
            for number, envelope_key in {0: 'volume', 1: 'arpeggio', 3: 'dutyEnvelope'}.items():
                if number in envelopes and envelopes[number]['length'] > 0:
                    lines.append('soundEnvelope=%s %s' % (envelope_key, format_envelope(envelopes[number])))
        lines.append('pad=%d %d %d' % (key - 23, sounds.index((instrument_id, arp_id)), value))

    text = '\n'.join(lines) + '\n'
    values = dict(line.split('=', 1) for line in lines[:9] if '=' in line)
    values.update(gain=0, n163Channels=1)
    return text, values


def merge_drums(noise, dpcm, keys):
    """Puts the noise and DPCM tracks on one, moving each sound to a key of its own."""
    real = {}
    used = set()

    # Samples stay on the keys they have in the song, where they can.
    for key_id, temporary in sorted(keys.items(), key=lambda item: item[1]):
        if key_id[0] == KIND_DPCM:
            key = key_id[3] + 23
            while key in used:
                key += 1
            real[temporary] = key
            used.add(key)

    next_key = FIRST_NOISE_KEY
    for key_id, temporary in sorted(keys.items(), key=lambda item: item[1]):
        if key_id[0] == KIND_NOISE:
            while next_key in used:
                next_key += 1
            real[temporary] = next_key
            used.add(next_key)

    assert not used or (min(used) >= FIRST_KEY and max(used) <= LAST_KEY), 'more drum sounds than keys'

    drums = Track('Drums', KIND_DRUMS, -1, -1)
    if noise:
        drums.controllers = dict(noise.controllers)
        drums.notes = [(start, end, real[key], velocity) for start, end, key, velocity in noise.notes]
    if dpcm:
        if CC_STOP in dpcm.controllers:
            drums.controllers[CC_STOP_SAMPLE] = dpcm.controllers[CC_STOP]
        drums.notes += [(start, end, real[key], velocity) for start, end, key, velocity in dpcm.notes]

    return drums, {key_id: real[temporary] for key_id, temporary in keys.items()}


class Track:
    def __init__(self, name, kind, instrument_id, arp_id):
        self.name, self.kind, self.instrument_id, self.arp_id = name, kind, instrument_id, arp_id
        self.notes = []       # (start frame, end frame, MIDI note, velocity)
        self.controllers = {} # controller: [(frame, value)]
        self.state = dict(RESTING)
        self.open_note = None
        self.last_note_on = -1.0
        self.slide_source = None  # Index of the note a slide left, which stays held to show its length.

    def set_slide_time(self, frames, frame):
        # It's only read when a note starts, so it goes well ahead of one, in case Live sends it late.
        self.set_controller(CC_SLIDE, frames, max(frame - 1, self.last_note_on + 0.05))

    def end_slide_source(self, frame):
        """The note a slide left has to end first, or the plugin would slide back to it."""
        if self.slide_source is not None:
            start, end, key, velocity = self.notes[self.slide_source]
            self.notes[self.slide_source] = (start, min(end, frame - 0.05), key, velocity)
            self.slide_source = None

    def add_slide_source(self, start, end, key, velocity):
        self.notes.append((start, end, key, velocity))
        self.slide_source = len(self.notes) - 1

    def set_controller(self, controller, value, frame):
        if self.state.get(controller, RESTING.get(controller, 0)) != value:
            self.controllers.setdefault(controller, []).append((frame, value))
            self.state[controller] = value

    def pulse_controller(self, controller, frame):
        self.controllers.setdefault(controller, []).extend([(frame, 127), (frame + 1, 0)])

    def close_note(self, frame):
        self.end_slide_source(frame)
        if self.open_note is not None:
            start, key, velocity = self.open_note
            self.notes.append((start, max(frame, start + 0.25), key, velocity))
            self.open_note = None


def volume_to_cc(volume):
    return min(127, round(volume * 127 / 15))


def convert(path, song_index):
    """Works out the tracks and their events, one frame at a time."""
    instruments, arps, song, kits, _ = txt.load(path, song_index, set())
    tempo_mode = fms.TEMPO_FAMITRACKER if 'TempoMode="FamiTracker"' in open(path, encoding='utf-8-sig').readline() else 0
    lines, total_frames = fms.song_to_lines(instruments, arps, song, tempo_mode)

    by_id = {i['id']: i for i in instruments}
    arp_by_id = {a['id']: a['envelope'] for a in arps}
    kit_for = {}
    for instrument_id, value, pitch, loop, initial, data in kits:
        kit_for.setdefault(instrument_id, []).append((value, pitch, loop, initial, data))

    kinds, events = {}, []
    for line in lines:
        fields = line.split()
        if fields[0] == 'C':
            kinds[int(fields[1])] = int(fields[2])
        elif fields[0] == 'N':
            events.append(list(map(int, fields[1:])))
        elif fields[0] == 'V':
            events.append([int(fields[1]), int(fields[2]), None, int(fields[3])])

    # The order they play in FamiStudio is how they're listed, so a stable sort keeps it.
    events.sort(key=lambda e: e[0])

    tracks = {}
    drum_keys = {}

    def drum_key(kind, instrument_id, arp_id, value):
        # Real keys get worked out at the end, once it's known how many sounds there are.
        return drum_keys.setdefault((kind, instrument_id, arp_id, value), 1000 + len(drum_keys))

    channels = {c: dict(volume=15, volume_step=0, volume_target=15, depth=0, speed=6, fine=0, duty=0, instrument=-1, arp=-1, track=None, key=None, released=False) for c in kinds}

    def track_for(channel, instrument_id, arp_id):
        # Every noise sound is on one track, and every sample on another, until they're merged.
        is_drums = kinds[channel] in (KIND_NOISE, KIND_DPCM)
        key = (channel, -1, -1) if is_drums else (channel, instrument_id, arp_id)
        if key not in tracks:
            if is_drums:
                tracks[key] = Track('Drums', kinds[channel], -1, -1)
                tracks[key].set_controller(CC_HOLD_RELEASE, 127, 0)
                return tracks[key]
            instrument = by_id.get(instrument_id)
            name = SHORT_NAMES[kinds[channel]]
            if instrument:
                name += ' ' + instrument['name']
            if arp_id >= 0:
                name += ' arp'
            tracks[key] = Track(name, kinds[channel], instrument_id, arp_id)
            tracks[key].set_controller(CC_HOLD_RELEASE, 127, 0)
        return tracks[key]

    def outlasts_note(track):
        """Samples, release envelopes and FM notes go on after a note-off, where FamiStudio's stop cuts them."""
        instrument = by_id.get(track.instrument_id)
        has_release = instrument is not None and any(e['release'] >= 0 for e in instrument['envelopes'].values())
        return has_release or track.kind in (KIND_NOISE, KIND_DPCM) or 8 <= track.kind <= 13 or 32 <= track.kind <= 37

    def apply_state(track, c, frame):
        track.set_controller(CC_DEPTH, min(127, math.ceil(c['depth'] * 127 / 15)), frame)
        if c['depth'] > 0:
            track.set_controller(CC_SPEED, max(1, c['speed']), frame)
        track.set_controller(CC_FINE, max(0, min(127, 64 + c['fine'])), frame)
        track.set_controller(CC_DUTY, c['duty'], frame)

    next_index = 0

    for frame in range(total_frames):
        # Volume slides move a little every frame, in steps of a sixteenth of a volume.
        for channel, c in channels.items():
            if c['volume_step'] != 0:
                c['volume'] = c['volume'] + c['volume_step'] / 16
                if (c['volume_step'] > 0 and c['volume'] >= c['volume_target']) or (c['volume_step'] < 0 and c['volume'] <= c['volume_target']):
                    c['volume'], c['volume_step'] = c['volume_target'], 0
                if c['track'] is not None:
                    c['track'].set_controller(CC_VOLUME, volume_to_cc(c['volume']), frame)

        while next_index < len(events) and events[next_index][0] == frame:
            event = events[next_index]
            next_index += 1
            channel = event[1]
            c = channels[channel]

            if event[2] is None:  # A volume slide's step, sent ahead of a delayed note.
                c['volume_step'] = event[3]
                continue

            _, _, value, attack, instrument_id, arp_id, slide, has_fine, fine, has_vib, vib_speed, vib_depth, has_duty, duty, has_vol, volume, has_vol_slide, vol_target, phase, slide_pitch, slide_step, vol_step, *_rest = event
            extra = _rest[4:]

            if has_fine: c['fine'] = fine
            if has_vib: c['depth'], c['speed'] = vib_depth, vib_speed
            if has_duty: c['duty'] = duty
            if has_vol:
                c['volume'] = volume
                c['volume_step'] = 0
            if has_vol_slide:
                c['volume_target'], c['volume_step'] = vol_target, vol_step

            current = c['track']

            if value == NOTE_STOP:
                if current is not None:
                    current.close_note(frame)
                    if outlasts_note(current):
                        current.pulse_controller(CC_STOP, frame)
                c['track'], c['released'] = None, False
            elif value == NOTE_RELEASE:
                instrument = by_id.get(c['instrument'])
                has_release = instrument is not None and any(e['release'] >= 0 for e in instrument['envelopes'].values())
                is_fm = 8 <= kinds[channel] <= 13 or 32 <= kinds[channel] <= 37
                if current is not None and (has_release or is_fm):
                    current.close_note(frame)
                    c['released'] = True
            elif value != NOTE_INVALID:
                if instrument_id >= 0: c['instrument'] = instrument_id
                c['arp'] = arp_id
                track = track_for(channel, c['instrument'], c['arp'])
                midi = value + 23
                slide_key = slide + 23

                if kinds[channel] in (KIND_NOISE, KIND_DPCM):
                    midi = drum_key(kinds[channel], c['instrument'], c['arp'], value)
                    if slide:
                        slide_key = drum_key(kinds[channel], c['instrument'], c['arp'], slide)

                if RHYTHM_FIRST <= kinds[channel] <= RHYTHM_FIRST + 5:
                    midi = 60 + kinds[channel] - RHYTHM_FIRST

                # Everything else about the channel carries over, whichever track played last.
                apply_state(track, c, frame - 0.1)

                velocity = max(1, min(127, round(max(1, round(c['volume'])) * 127 / 15)))
                keeps_going = not attack and current is track and track.open_note is not None and kinds[channel] != 4

                if current is not None and not keeps_going:
                    current.close_note(frame)
                    # Another track doesn't know this note has taken over the channel.
                    if current is not track and outlasts_note(current):
                        current.pulse_controller(CC_STOP, frame)

                track.set_controller(CC_VOLUME, volume_to_cc(c['volume']), frame - 0.1)

                held_key = track.open_note[1] if keeps_going else None
                target = slide_key if slide else midi

                if target == held_key:
                    # Already on this key, and the note just carries on. A second copy of the key
                    # would end the sound when the first one's note-off arrived.
                    track.set_slide_time(0, frame)
                elif slide and target != midi:
                    # The start note then the target, overlapping, which is the plugin's slide. The
                    # start note stays for the slide's length, so it can be seen on the piano roll.
                    frames = min(127, max(1, math.ceil(abs(slide_pitch) / max(1, abs(slide_step)))))
                    track.set_slide_time(frames, frame)
                    track.end_slide_source(frame)
                    if keeps_going:
                        start, key, held_velocity = track.open_note
                        track.add_slide_source(start, frame + frames, key, held_velocity)
                    else:
                        track.add_slide_source(frame, frame + frames, midi, velocity)
                    track.open_note = (frame + 0.1, target, velocity)
                    track.last_note_on = frame + 0.1
                else:
                    track.set_slide_time(0, frame)
                    if keeps_going:
                        # A note with no attack is a legato overlap.
                        track.end_slide_source(frame)
                        track.close_note(frame + 0.2)
                    track.open_note = (frame, midi, velocity)
                    track.last_note_on = frame

                c['track'], c['released'] = track, False
            elif current is not None:
                apply_state(current, c, frame)
                if has_vol:
                    current.set_controller(CC_VOLUME, volume_to_cc(c['volume']), frame)

            if phase and c['track'] is not None:
                c['track'].pulse_controller(CC_PHASE, frame)

            if extra and c['track'] is not None:
                has_speed, speed, has_depth, depth = extra
                if has_speed: c['track'].set_controller(CC_FDS_SPEED, min(127, speed // 32), frame)
                if has_depth: c['track'].set_controller(CC_FDS_DEPTH, min(63, depth), frame)

    # The channel goes quiet with the song, even if a release tail is still holding.
    for c in channels.values():
        if c['track'] is not None and outlasts_note(c['track']):
            c['track'].pulse_controller(CC_STOP, total_frames - 2)

    for track in tracks.values():
        track.close_note(total_frames)

        # Copies of a key that overlap would cut each other off, in the plugin and in Live.
        last_end = {}
        for start, end, key, _ in sorted(track.notes):
            assert start >= last_end.get(key, 0), '%s: key %d overlaps itself at frame %s' % (track.name, key, start)
            last_end[key] = end

    n163_channels = max(1, sum(1 for k in kinds.values() if 18 <= k <= 25))
    used = [t for t in tracks.values() if t.notes and t.kind not in (KIND_NOISE, KIND_DPCM)]
    noise = next((t for t in tracks.values() if t.kind == KIND_NOISE and t.notes), None)
    dpcm = next((t for t in tracks.values() if t.kind == KIND_DPCM and t.notes), None)
    keys = {}

    if noise or dpcm:
        drums, keys = merge_drums(noise, dpcm, drum_keys)
        used.insert(0, drums)

    return song, used, by_id, arp_by_id, kit_for, n163_channels, total_frames, keys


def clip_envelopes(track, controllers, beats_per_frame):
    """Clip envelopes that hold each value until the next, as Live writes them."""
    envelopes = []
    for envelope_id, (number, events) in enumerate(sorted(track.controllers.items())):
        element = ET.Element('ClipEnvelope', Id=str(envelope_id))
        target = ET.SubElement(element, 'EnvelopeTarget')
        ET.SubElement(target, 'PointeeId', Value=controllers[number])
        automation = ET.SubElement(element, 'Automation')
        points = ET.SubElement(automation, 'Events')
        previous = RESTING.get(number, 0)
        event_id = 0

        def point(time, value):
            nonlocal event_id
            ET.SubElement(points, 'FloatEvent', Id=str(event_id), Time=time, Value=str(value))
            event_id += 1

        point('-63072000', previous)
        for frame, value in sorted(events, key=lambda e: e[0]):
            time = '%.9f' % max(0.0, frame * beats_per_frame)
            point(time, previous)
            point(time, value)
            previous = value

        view = ET.SubElement(automation, 'AutomationTransformViewState')
        ET.SubElement(view, 'IsTransformPending', Value='false')
        ET.SubElement(view, 'TimeAndValueTransforms')
        envelopes.append(element)
    return envelopes


def main():
    base_path, path, song_index, out = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
    base = ET.fromstring(gzip.open(base_path).read())
    prototype = next(base.iter('MidiClip'))

    song, used, by_id, arp_by_id, kit_for, n163_channels, total_frames, drum_keys = convert(path, song_index)

    frames_per_note = sum(song['groove']) / len(song['groove'])
    # A beat holds this many frames, which keeps Live's grid on the song's own notes.
    beat_notes = max(1, int(dict(txt.ATTRIBUTE.findall(next(l for l in open(path, encoding='utf-8-sig') if l.startswith('\tSong ')))).get('BeatLength', 4)))
    tempo = 60.0 * FRAMES_PER_SECOND / (frames_per_note * beat_notes)
    while tempo > 240: tempo /= 2
    while tempo < 60: tempo *= 2
    beats_per_frame = tempo / (60.0 * FRAMES_PER_SECOND)

    sounds = {}
    lineup = []
    for index, track in enumerate(used):
        instrument = by_id.get(track.instrument_id)
        arpeggio = arp_by_id.get(track.arp_id)
        kit = kit_for.get(track.instrument_id, []) if track.kind == 4 else []
        key = 'track%d' % index
        sounds[key] = drums_text(drum_keys, by_id, arp_by_id, kit_for) if track.kind == KIND_DRUMS else sound_text(track.name, track.kind, instrument, arpeggio, kit, n163_channels)
        lineup.append((track.name, key, COLOURS[channel_name(track.kind)]))

    text = build(base, sounds.__getitem__, lineup)
    root = ET.fromstring(text)
    live_set = root.find('LiveSet')

    # Set the tempo, in both places Live keeps it.
    main_track = live_set.find('MainTrack')
    tempo_element = main_track.find('.//Tempo')
    tempo_element.find('Manual').set('Value', '%.6f' % tempo)
    target = tempo_element.find('AutomationTarget').get('Id')
    for envelope in main_track.find('AutomationEnvelopes/Envelopes'):
        if envelope.find('EnvelopeTarget/PointeeId').get('Value') == target:
            for event in envelope.find('Automation/Events'):
                event.set('Value', '%.6f' % tempo)
    live_set.find('Transport/LoopOn').set('Value', 'false')

    length = total_frames * beats_per_frame
    midi_tracks = live_set.find('Tracks').findall('MidiTrack')

    for clip_id, (element, track) in enumerate(zip(midi_tracks, used)):
        # Each note ends just short of its length, so a note can't run into the next one on its key.
        notes = [(start * beats_per_frame, (end - start) * beats_per_frame - 1e-6, key, velocity) for start, end, key, velocity in track.notes]
        clip = make_clip(prototype, 0.0, track.name, length, notes, COLOURS[channel_name(track.kind)], clip_id + 1)
        clip.find('TakeId').set('Value', '1')
        for counter in element.iter('TakeCounter'):
            counter.set('Value', '2')

        controllers = {}
        for target in element.find('DeviceChain/MainSequencer/MidiControllers'):
            if target.tag.startswith('ControllerTargets.'):
                controllers[int(target.tag.split('.')[1]) - CONTROLLER_SLOT_OF_CC_0] = target.get('Id')

        clip_list = clip.find('Envelopes/Envelopes')
        for envelope in clip_envelopes(track, controllers, beats_per_frame):
            clip_list.append(envelope)

        element.find('DeviceChain/MainSequencer/ClipTimeable/ArrangerAutomation/Events').append(clip)

    pathlib.Path(out).parent.mkdir(parents=True, exist_ok=True)
    pathlib.Path(out).write_bytes(gzip.compress(('<?xml version="1.0" encoding="UTF-8"?>\n' + ET.tostring(root, encoding='unicode') + '\n').encode(), mtime=0))
    print('%s: %d tracks, %.0f BPM, %.1f s' % (out, len(used), tempo, total_frames / FRAMES_PER_SECOND))


if __name__ == '__main__':
    main()
