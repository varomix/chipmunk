#!/usr/bin/env python3
"""Builds original sample songs as Live Sets, one Chipmunk device per track.

Uses the same base Set as make_live_templates.py, which also supplies the shape of a clip.

    dump_factory_presets <presets folder>
    python3 tools/make_sample_songs.py <base.als> <presets folder> examples
"""

import copy
import gzip
import pathlib
import re
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from make_live_templates import build

PITCH_CLASSES = {"C": 0, "C#": 1, "Db": 1, "D": 2, "D#": 3, "Eb": 3, "E": 4, "F": 5, "F#": 6, "Gb": 6, "G": 7, "G#": 8, "Ab": 8, "A": 9, "A#": 10, "Bb": 10, "B": 11}

# Written with C4 as MIDI 60. Live shows the same keys an octave lower, as C3.
KICK, SNARE, LOW_TOM, HIGH_TOM = 36, 38, 45, 50

CHORDS = {
    "C": "C E G", "Dm": "D F A", "Em": "E G B", "F": "F A C", "G": "G B D", "Am": "A C E",
    "E": "E G# B", "Bb": "Bb D F", "Gm": "G Bb D", "A": "A C# E",
}


def midi(name):
    match = re.fullmatch(r"([A-G][#b]?)(-?\d)", name)
    return (int(match.group(2)) + 1) * 12 + PITCH_CLASSES[match.group(1)]


def line(text, velocity=110, gate=0.9):
    """Notes like "E5:1 G5:.5 r:1", where the number is the length in beats."""
    notes, beat = [], 0.0
    for item in text.split():
        name, length = item.split(":")
        length = float(length)
        if name != "r":
            notes.append((beat, length * gate, midi(name), velocity))
        beat += length
    return notes, beat


def chord_notes(name, octave):
    """The chord's notes rising from its root, starting in the given octave."""
    result, previous = [], -1
    for pitch in CHORDS[name].split():
        value = (octave + 1) * 12 + PITCH_CLASSES[pitch]
        while value <= previous:
            value += 12
        result.append(value)
        previous = value
    return result


def parse_chords(text):
    """Chords like "C Am F:2 G:2", four beats each unless a length is given."""
    result = []
    for item in text.split():
        name, _, length = item.partition(":")
        result.append((name, float(length or 4)))
    return result


def arpeggio(chords, octave, pattern, step=0.5, velocity=95, gate=0.8):
    """Repeats chord tones. In the pattern 0, 1 and 2 are the notes and 3 is the root an octave up."""
    notes, beat = [], 0.0
    for name, length in parse_chords(chords):
        tones = chord_notes(name, octave)
        tones.append(tones[0] + 12)
        position = 0.0
        index = 0
        while position < length - 1e-6:
            notes.append((beat + position, step * gate, tones[pattern[index % len(pattern)]], velocity))
            position += step
            index += 1
        beat += length
    return notes, beat


def bass(chords, octave, pattern, step=0.5, velocity=120, gate=0.8):
    """Follows chord roots. The pattern holds semitones above the root, with None for a rest."""
    notes, beat = [], 0.0
    for name, length in parse_chords(chords):
        root = chord_notes(name, octave)[0]
        position = 0.0
        index = 0
        while position < length - 1e-6:
            offset = pattern[index % len(pattern)]
            if offset is not None:
                notes.append((beat + position, step * gate, root + offset, velocity))
            position += step
            index += 1
        beat += length
    return notes, beat


def held(chords, tone, octave, velocity=100, gate=0.97):
    """One chord tone held for each chord, for pads."""
    notes, beat = [], 0.0
    for name, length in parse_chords(chords):
        notes.append((beat, length * gate, chord_notes(name, octave)[tone], velocity))
        beat += length
    return notes, beat


def steps(bars, sounds):
    """16th-note patterns, one string per bar. Each letter in "sounds" gives a (key, velocity)."""
    notes = []
    for bar, pattern in enumerate(bars):
        for index, letter in enumerate(pattern):
            if letter in sounds:
                key, velocity = sounds[letter]
                notes.append((bar * 4 + index * 0.25, 0.2, key, velocity))
    return notes, len(bars) * 4.0


def transposed(part, semitones, velocity=None):
    notes, beats = part
    return [(b, d, n + semitones, velocity or v) for b, d, n, v in notes], beats


def harmony_below(part, scale, degrees=2, velocity=95):
    """Moves each note down the scale, for parallel thirds."""
    notes, beats = part
    classes = [PITCH_CLASSES[p] for p in scale.split()]
    moved = []
    for b, d, n, v in notes:
        target = n
        for _ in range(degrees):
            target -= 1
            while target % 12 not in classes:
                target -= 1
        moved.append((b, d, target, velocity))
    return moved, beats


def rest(beats):
    return [], float(beats)


def join(*parts):
    notes, total = [], 0.0
    for part_notes, beats in parts:
        notes += [(b + total, d, n, v) for b, d, n, v in part_notes]
        total += beats
    return notes, total


DRUMS = {"k": (KICK, 127), "s": (SNARE, 127), "t": (LOW_TOM, 127), "h": (HIGH_TOM, 127)}
HATS = {"x": (71, 80), "X": (71, 112), "o": (67, 118)}


def first_stage():
    chords_a = "C Am F G C Am F:2 G:2 C"
    chords_b = "F G Em Am F G Dm G"

    melody_a = line("""E5:1 G5:.5 E5:.5 C5:1 G4:1   A4:.5 C5:.5 E5:1 D5:.5 C5:.5 A4:1
                       F4:.5 A4:.5 C5:1 F5:1 E5:.5 D5:.5   D5:1.5 E5:.5 D5:1 G4:1
                       E5:1 G5:.5 E5:.5 C6:1 G5:1   A5:.5 G5:.5 E5:1 D5:.5 E5:.5 C5:1
                       F5:1 A5:1 G5:.5 F5:.5 D5:1   C5:2 r:1 G4:.5 B4:.5""", 120)
    melody_b = line("""A4:1 C5:1 F5:1.5 E5:.5   D5:1 G5:1 B5:1 G5:1
                       E5:1 G5:.5 E5:.5 B4:1 E5:1   C5:1.5 E5:.5 A5:2
                       A5:1 G5:.5 F5:.5 C5:1 F5:1   G5:1 F5:.5 E5:.5 D5:2
                       F5:1 E5:.5 D5:.5 A4:1 D5:1   G4:.5 B4:.5 D5:.5 G5:.5 B5:1 r:1""", 120)

    hats = ["x.x.X.x.x.x.X.x."] * 7 + ["x.x.X.x.x.x.X.xX"]
    beat = ["k...s...k.k.s..."] * 7 + ["k...s...k.k.s.ss"]
    bass_line = [0, None, 12, 0, 7, None, 12, 7]

    return {
        "title": "First Stage",
        "tempo": 152,
        "scale": "C D E F G A B",
        "tracks": [
            ("Lead", "Hero Lead", 25, 0.68, -0.1),
            ("Arps", "Pluck Lead", 26, 0.48, 0.3),
            ("Bass", "Triangle Bass", 17, 0.55, 0.0),
            ("Hats", "Closed Hat", 19, 0.62, -0.2),
            ("Drums", "Synth Drum Kit", 3, 0.42, 0.0),
        ],
        "sections": [
            ("Intro", 16, {
                "Arps": arpeggio("C Am F G", 4, [0, 1, 2, 3, 2, 1, 2, 1]),
                "Hats": steps(["x.x.X.x.x.x.X.x."] * 4, HATS),
                "Bass": join(rest(8), bass("F G", 3, [0, None, None, None], step=1.0)),
                "Drums": steps(["................", "................", "k.......k.......", "k...k...k.k.s.ss"], DRUMS),
            }),
            ("Verse", 32, {
                "Lead": melody_a,
                "Arps": arpeggio("C Am F G C Am F:2 G:2 C", 4, [0, 1, 2, 1]),
                "Bass": bass(chords_a, 3, bass_line),
                "Hats": steps(hats, HATS),
                "Drums": steps(beat, DRUMS),
            }),
            ("Verse 2", 32, {
                "Lead": melody_a,
                "Arps": arpeggio(chords_a, 4, [0, 2, 3, 2, 1, 2, 3, 2], step=0.25, velocity=85),
                "Bass": bass(chords_a, 3, bass_line),
                "Hats": steps(hats, HATS),
                "Drums": steps(beat, DRUMS),
            }),
            ("Bridge", 32, {
                "Lead": melody_b,
                "Arps": arpeggio(chords_b, 4, [0, 1, 2, 3], step=0.25, velocity=85),
                "Bass": bass(chords_b, 3, [0, 12, 0, 12, 7, 12, 7, 12]),
                "Hats": steps(["x.x.x.x.x.x.x.x."] * 7 + ["X.X.X.X.XXXXXXXX"], HATS),
                "Drums": steps(["k.......s.......", "k.k.....s.......", "k.......s.......", "k.k.....s...s.s."] + ["k...s...k.k.s..."] * 3 + ["k...s...t.t.hhss"], DRUMS),
            }),
            ("Last Verse", 32, {
                "Lead": melody_a,
                "Arps": arpeggio(chords_a, 4, [0, 2, 3, 2, 1, 2, 3, 2], step=0.25, velocity=85),
                "Bass": bass(chords_a, 3, bass_line),
                "Hats": steps(hats, HATS),
                "Drums": steps(beat, DRUMS),
            }),
            ("Ending", 8, {
                "Lead": line("C5:.5 E5:.5 G5:.5 C6:.5 r:.5 C6:1.5 r:4", 124),
                "Arps": line("E4:.5 G4:.5 C5:.5 E5:.5 r:.5 E5:1.5 r:4", 90),
                "Bass": line("C3:.5 C3:.5 C3:.5 C3:.5 r:.5 C3:1.5 r:4", 120),
                "Hats": steps(["x.x.x.x.o.......", "................"], HATS),
                "Drums": steps(["k.k.k.k.k...s...", "................"], DRUMS),
            }),
        ],
    }


def neon_causeway():
    chords_a = "Am F C G Am F Dm E"
    chords_b = "F G Em Am F G Am E"
    scale = "A B C D E F G"

    melody_a = line("""A5:1.5 G5:.5 E5:1 C5:1   F5:1.5 E5:.5 C5:1 A4:1
                       G4:.5 C5:.5 E5:.5 G5:.5 C6:1 G5:1   B5:1.5 A5:.5 G5:1 D5:1
                       E5:.5 A5:.5 C6:1 B5:.5 A5:.5 E5:1   F5:.5 A5:.5 C6:1 A5:.5 F5:.5 C5:1
                       D5:1 F5:1 A5:1 D6:1   B5:1 G#5:1 E5:2""", 120)
    melody_b = line("""C6:2 A5:1 F5:1   D6:2 B5:1 G5:1
                       E6:1.5 D6:.5 B5:1 G5:1   C6:1 B5:.5 A5:.5 E5:2
                       F5:1 A5:1 C6:1 F6:1   G6:1 F6:.5 E6:.5 D6:2
                       C6:1 A5:1 E5:1 A5:1   G#5:1 B5:1 E6:2""", 118)

    saw = [0, 0, 12, 0, 0, 12, 0, 12]
    hats = ["x.x.X.x.x.x.X.xx"] * 7 + ["x.x.X.x.X.X.XXXX"]
    beat = ["k.....k.s.......", "k.....k.s..k...."] * 3 + ["k.....k.s.......", "k.....k.s...t.hh"]

    return {
        "title": "Neon Causeway",
        "tempo": 138,
        "trim": 0.82,
        "scale": "A B C D E F G G#",
        "tracks": [
            ("Pulse 1", "Hero Lead", 25, 0.5, -0.1),
            ("Pulse 2", "Pluck Lead", 26, 0.34, 0.35),
            ("Triangle", "Triangle Bass", 17, 0.35, 0.0),
            ("Noise", "Closed Hat", 19, 0.55, -0.2),
            ("DPCM", "Synth Drum Kit", 3, 0.42, 0.0),
            ("VRC6 Pulse 1", "VRC6 Soft Lead", 5, 0.42, 0.1),
            ("VRC6 Pulse 2", "VRC6 Bell", 6, 0.36, -0.35),
            ("VRC6 Saw", "Saw Bass", 7, 0.32, 0.0),
            ("MMC5 Pulse 1", "MMC5 Lead", 9, 0.3, 0.25),
            ("MMC5 Pulse 2", "MMC5 Harmony Pad", 10, 0.42, -0.25),
            ("S5B 1", "S5B Bell", 13, 0.55, 0.4),
            ("S5B 2", "S5B Lead", 14, 0.35, -0.4),
            ("S5B 3", "S5B Growl Bass", 15, 0.18, 0.0),
        ],
        "sections": [
            ("Intro", 16, {
                "S5B 1": arpeggio("Am F C G", 5, [0, 1, 2, 3, 2, 1, 2, 1], velocity=100),
                "Noise": steps(["x.x.X.x.x.x.X.x."] * 4, HATS),
                "DPCM": steps(["................", "................", "k.......k.......", "k.....k.s.t.t.hh"], DRUMS),
                "VRC6 Saw": join(rest(8), bass("C G", 2, [0, None, None, None], step=1.0, velocity=105)),
            }),
            ("Main Theme", 32, {
                "Pulse 1": melody_a,
                "VRC6 Saw": bass(chords_a, 2, saw),
                "Noise": steps(hats, HATS),
                "DPCM": steps(beat, DRUMS),
                "S5B 1": arpeggio(chords_a, 5, [0, 1, 2, 1], velocity=90),
            }),
            ("Main Theme 2", 32, {
                "Pulse 1": melody_a,
                "Pulse 2": arpeggio(chords_a, 4, [0, 2, 3, 2], step=0.25, velocity=85),
                "MMC5 Pulse 1": harmony_below(melody_a, scale),
                "VRC6 Saw": bass(chords_a, 2, saw),
                "Noise": steps(hats, HATS),
                "DPCM": steps(beat, DRUMS),
                "MMC5 Pulse 2": held(chords_a, 2, 4),
            }),
            ("Bridge", 32, {
                "VRC6 Pulse 1": melody_b,
                "MMC5 Pulse 1": harmony_below(melody_b, scale),
                "Triangle": bass(chords_b, 3, [0, None, 12, None], step=0.5),
                "VRC6 Pulse 2": arpeggio(chords_b, 5, [0, 2, 1, 3], step=0.5, velocity=95, gate=0.9),
                "S5B 2": held(chords_b, 1, 5, velocity=100),
                "S5B 3": held(chords_b, 0, 3, velocity=110),
                "Noise": steps(["x.x.x.x.x.x.x.x."] * 8, HATS),
                "DPCM": steps(["k.......s......."] * 7 + ["k.......s...t.hh"], DRUMS),
            }),
            ("Finale", 32, {
                "Pulse 1": melody_a,
                "VRC6 Pulse 1": transposed(melody_a, -12, 105),
                "Pulse 2": arpeggio(chords_a, 4, [0, 2, 3, 2], step=0.25, velocity=85),
                "MMC5 Pulse 1": harmony_below(melody_a, scale),
                "VRC6 Pulse 2": arpeggio(chords_a, 5, [0, 2, 1, 3], velocity=90, gate=0.9),
                "VRC6 Saw": bass(chords_a, 2, saw),
                "Triangle": bass(chords_a, 3, [0, None, 12, None]),
                "MMC5 Pulse 2": held(chords_a, 2, 4),
                "S5B 2": held(chords_a, 1, 5, velocity=100),
                "S5B 3": held(chords_a, 0, 3, velocity=110),
                "Noise": steps(hats, HATS),
                "DPCM": steps(beat, DRUMS),
            }),
            ("Ending", 8, {
                "Pulse 1": line("A5:1 r:1 E5:.5 A5:.5 C6:1 r:4", 124),
                "VRC6 Saw": line("A2:1 r:1 A2:.5 A2:.5 A2:1 r:4", 120),
                "Triangle": line("A3:1 r:1 A3:.5 A3:.5 A3:1 r:4", 120),
                "Noise": steps(["x.......x.x.o...", "................"], HATS),
                "DPCM": steps(["k.......k.k.k...", "................"], DRUMS),
            }),
        ],
    }


def crystal_cavern():
    chords_a = "Dm Bb F C Dm Gm A Dm"
    chords_b = "Gm Dm Bb A Gm Dm A Dm"

    melody_a = line("""A5:2 F5:1 D5:1   D5:1 F5:1 Bb5:2
                       A5:1.5 G5:.5 F5:1 C5:1   E5:1 G5:1 C6:2
                       D6:1.5 C6:.5 A5:2   Bb5:1 A5:.5 G5:.5 D5:2
                       C#5:1 E5:1 A5:1 G5:1   F5:1 E5:1 D5:2""", 112, gate=0.96)
    melody_b = line("""G5:1 Bb5:1 D6:2   A5:1 F5:1 D6:2
                       F6:1.5 D6:.5 Bb5:2   A5:1 C#6:1 E6:2
                       D6:1 Bb5:.5 G5:.5 D5:2   F5:1 A5:.5 F5:.5 D5:2
                       E5:1 C#5:1 A4:2   D5:4""", 118, gate=0.96)

    answers = line("r:3 A5:1  r:3 F5:1  r:3 C6:1  r:3 G5:1  r:3 A5:1  r:3 D6:1  r:3 E5:1  r:3 A5:1", 100, gate=0.5)
    wind = line("C4:14 r:18", 90, gate=1.0)

    return {
        "title": "Crystal Cavern",
        "tempo": 92,
        "scale": "D E F G A Bb C C#",
        "tracks": [
            ("Melody", "VRC6 Soft Lead", 5, 0.68, 0.0),
            ("Glass Arps", "S5B Bell", 13, 0.75, 0.3),
            ("Pad", "MMC5 Harmony Pad", 9, 0.42, -0.3),
            ("Echoes", "Echo Blip", 25, 0.42, 0.45),
            ("Bass", "Triangle Bass", 17, 0.6, 0.0),
            ("Wind", "Wind", 19, 0.45, -0.45),
            ("Drums", "Synth Drum Kit", 3, 0.5, 0.0),
        ],
        "sections": [
            ("Intro", 16, {
                "Glass Arps": arpeggio("Dm Bb F C", 5, [0, 1, 2, 3, 2, 1], step=0.25, velocity=95),
                "Wind": line("C4:14 r:2", 80, gate=1.0),
                "Bass": join(rest(8), held("F C", 0, 3, velocity=115)),
            }),
            ("Theme", 32, {
                "Melody": melody_a,
                "Glass Arps": arpeggio(chords_a, 5, [0, 1, 2, 3, 2, 1], step=0.25, velocity=85),
                "Pad": held(chords_a, 1, 4),
                "Bass": bass(chords_a, 3, [0, None, None, 7], step=1.0, gate=0.9),
                "Wind": wind,
            }),
            ("Rising", 32, {
                "Melody": melody_b,
                "Glass Arps": arpeggio(chords_b, 5, [0, 1, 2, 3, 2, 1], step=0.25, velocity=90),
                "Pad": held(chords_b, 1, 4),
                "Echoes": answers,
                "Bass": bass(chords_b, 3, [0, None, 7, None], step=1.0, gate=0.9),
                "Drums": steps(["t.......h.......", "t.....t.h.......", "t.......h.......", "t.....t.h...h.h."] * 2, DRUMS),
                "Wind": wind,
            }),
            ("Theme Returns", 32, {
                "Melody": melody_a,
                "Glass Arps": arpeggio(chords_a, 5, [0, 1, 2, 3, 2, 1], step=0.25, velocity=85),
                "Pad": held(chords_a, 1, 4),
                "Echoes": answers,
                "Bass": bass(chords_a, 3, [0, None, 7, None], step=1.0, gate=0.9),
                "Drums": steps(["t...............", "t...........h..."] * 4, DRUMS),
                "Wind": wind,
            }),
            ("Fade", 16, {
                "Glass Arps": join(arpeggio("Dm Bb", 5, [0, 1, 2, 3, 2, 1], step=0.25, velocity=70), arpeggio("Gm", 5, [0, 1, 2, 3, 2, 1], step=0.25, velocity=50), line("A5:1 F5:1 D5:2", 40)),
                "Pad": held("Dm Bb Gm Dm", 1, 4, velocity=70),
                "Bass": held("Dm Bb Gm Dm", 0, 3, velocity=115),
                "Wind": line("C4:12 r:4", 70, gate=1.0),
            }),
        ],
    }


AUTHOR = "varo"


def check_song(song):
    """Stops the build on bars that don't add up, wrong notes, or notes that would overlap."""
    scale = {PITCH_CLASSES[p] for p in song["scale"].split()}
    names = {t[0] for t in song["tracks"]}
    used = set()

    for section, beats, parts in song["sections"]:
        for track, (notes, length) in parts.items():
            where = f"{song['title']}, {section}, {track}"
            assert track in names, where
            assert abs(length - beats) < 1e-6, f"{where}: {length} beats, expected {beats}"
            used.add(track)
            ordered = sorted(notes)
            for (a, a_length, a_note, _), (b, _, _, _) in zip(ordered, ordered[1:]):
                assert a + a_length <= b + 1e-6, f"{where}: overlap at beat {a}"
            for start, _, note, velocity in notes:
                assert 24 <= note <= 108, f"{where}: note {note} out of range"
                assert 1 <= velocity <= 127, where
                is_drum = track in ("Drums", "DPCM", "Hats", "Noise", "Wind")
                assert is_drum or note % 12 in scale, f"{where}: note {note} at beat {start} outside the scale"

    assert used == names, names - used


def make_clip(prototype, time, section, length, notes, colour, clip_id):
    clip = copy.deepcopy(prototype)
    clip.set("Id", str(clip_id))
    clip.set("Time", f"{time:g}")
    clip.find("CurrentStart").set("Value", f"{time:g}")
    clip.find("CurrentEnd").set("Value", f"{time + length:g}")
    clip.find("Name").set("Value", section)
    clip.find("Color").set("Value", str(colour))

    loop = clip.find("Loop")
    for key, value in (("LoopStart", 0), ("LoopEnd", length), ("StartRelative", 0), ("OutMarker", length), ("HiddenLoopStart", 0), ("HiddenLoopEnd", length)):
        loop.find(key).set("Value", f"{value:g}")
    loop.find("LoopOn").set("Value", "false")

    clip.find("GrooveSettings/GrooveId").set("Value", "-1")
    clip.find("ScrollerTimePreserver/LeftTime").set("Value", f"{time:g}")
    clip.find("ScrollerTimePreserver/RightTime").set("Value", f"{time + length:g}")

    key_tracks = clip.find("Notes/KeyTracks")
    for child in list(key_tracks):
        key_tracks.remove(child)

    next_note = 1
    for track_id, key in enumerate(sorted({n[2] for n in notes})):
        key_track = ET.SubElement(key_tracks, "KeyTrack", Id=str(track_id))
        events = ET.SubElement(key_track, "Notes")
        for start, duration, note, velocity in sorted(notes):
            if note == key:
                ET.SubElement(events, "MidiNoteEvent", Time=f"{start:.12g}", Duration=f"{duration:.12g}", Velocity=str(velocity), OffVelocity="64", NoteId=str(next_note))
                next_note += 1
        ET.SubElement(key_track, "MidiKey", Value=str(key))

    clip.find("Notes/NoteIdGenerator/NextId").set("Value", str(next_note))
    return clip


def set_tempo(live_set, tempo):
    main = live_set.find("MainTrack")
    tempo_element = main.find(".//Tempo")
    tempo_element.find("Manual").set("Value", str(tempo))

    # The tempo's automation lane holds a starting value too, and that's the one Live plays.
    target = tempo_element.find("AutomationTarget").get("Id")
    found = False
    for envelope in main.find("AutomationEnvelopes/Envelopes"):
        if envelope.find("EnvelopeTarget/PointeeId").get("Value") == target:
            for event in envelope.find("Automation/Events"):
                event.set("Value", str(tempo))
                found = True
    assert found


def build_song(base, presets, prototype, song):
    check_song(song)

    lineup = [(name, preset, colour) for name, preset, colour, _, _ in song["tracks"]]
    root = ET.fromstring(build(base, presets, lineup))
    live_set = root.find("LiveSet")
    set_tempo(live_set, song["tempo"])
    live_set.find("Transport/LoopOn").set("Value", "false")

    tracks = {t.find("Name/EffectiveName").get("Value"): t for t in live_set.find("Tracks").findall("MidiTrack")}
    clip_id = 1
    takes = {}

    for name, _, colour, volume, pan in song["tracks"]:
        mixer = tracks[name].find("DeviceChain/Mixer")
        mixer.find("Volume/Manual").set("Value", f"{volume * song.get('trim', 1.0):.3f}")
        mixer.find("Pan/Manual").set("Value", str(pan))

    time = 0.0
    for section, beats, parts in song["sections"]:
        for name, _, colour, _, _ in song["tracks"]:
            if name in parts:
                events = tracks[name].find("DeviceChain/MainSequencer/ClipTimeable/ArrangerAutomation/Events")
                clip = make_clip(prototype, time, section, beats, parts[name][0], colour, clip_id)

                # Takes count per track, and the counter has to stay ahead of the clips.
                takes[name] = takes.get(name, 0) + 1
                clip.find("TakeId").set("Value", str(takes[name]))
                for counter in tracks[name].iter("TakeCounter"):
                    counter.set("Value", str(takes[name] + 1))

                events.append(clip)
                clip_id += 1
        time += beats

    # The Set's own info, and the main track's info, which Live's Info View shows.
    credit = f"{song['title']} by {AUTHOR}"
    live_set = root.find("LiveSet")
    live_set.find("Annotation").set("Value", credit)
    live_set.find("MainTrack/Name/Annotation").set("Value", credit)

    return '<?xml version="1.0" encoding="UTF-8"?>\n' + ET.tostring(root, encoding="unicode") + "\n", time


def main():
    base_path, presets, output = sys.argv[1:4]
    base = ET.fromstring(gzip.open(base_path).read())
    output = pathlib.Path(output)
    output.mkdir(exist_ok=True)

    prototype = next(base.iter("MidiClip"))
    # The base Set's notes never reach a song: every clip's key tracks are replaced.
    for song in (first_stage(), neon_causeway(), crystal_cavern()):
        text, beats = build_song(base, presets, prototype, song)
        path = output / f"{AUTHOR} - {song['title']}.als"
        path.write_bytes(gzip.compress(text.encode(), mtime=0))
        print(f"{path}: {len(song['tracks'])} tracks, {beats / 4:g} bars at {song['tempo']} BPM, {beats * 60 / song['tempo']:.0f} s")


if __name__ == "__main__":
    main()
