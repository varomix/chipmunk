#!/usr/bin/env python3
"""Turns one song from FamiStudio's text export into a note-event script for fms_compare.

The text export reads any project version, so this covers current projects, unlike the binary
reader in fms_to_script.py. Effects can be left out, to measure what each one contributes.

Usage: fms_txt_to_script.py Project.txt song_index out.script [--without slide,vibrato,...] [--epsm-period Name=16]
Effects: slide vibrato volume fine duty arpeggio noattack release phase

The text export leaves out one setting, the fixed hardware envelope period of an EPSM instrument
that doesn't follow the note. --epsm-period supplies it. TestEPSM's instrument EPSM9 needs 16.
"""
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import fms_to_script as fms

# A quote inside a value is written twice.
ATTRIBUTE = re.compile(r'(\w+)="((?:[^"]|"")*)"')
PITCHES = {'C': 0, 'C#': 1, 'D': 2, 'D#': 3, 'E': 4, 'F': 5, 'F#': 6, 'G': 7, 'G#': 8, 'A': 9, 'A#': 10, 'B': 11}
ENVELOPE_TYPES = {'Volume': 0, 'Arpeggio': 1, 'Pitch': 2, 'DutyCycle': 3, 'FDSWave': 4, 'FDSMod': 5, 'N163Wave': 6, 'Repeat': 7, 'MixerSettings': 8, 'NoiseFreq': 9}
SAW_VOLUMES = {'Full': 0, 'Half': 1, 'Quarter': 2}
CHANNEL_TYPES = {'Square1': 0, 'Square2': 1, 'Triangle': 2, 'Noise': 3, 'DPCM': 4,
                 'VRC6Square1': 5, 'VRC6Square2': 6, 'VRC6Saw': 7,
                 'MMC5Square1': 15, 'MMC5Square2': 16,
                 'S5BSquare1': 26, 'S5BSquare2': 27, 'S5BSquare3': 28, 'FDS': 14,
                 **{'VRC7FM%d' % (i + 1): 8 + i for i in range(6)},
                 **{'N163Wave%d' % (i + 1): 18 + i for i in range(8)},
                 **{'EPSMSquare%d' % (i + 1): 29 + i for i in range(3)},
                 **{'EPSMFM%d' % (i + 1): 32 + i for i in range(6)},
                 **{'EPSMRythm%d' % (i + 1): 38 + i for i in range(6)}}
PADDING = {'Beginning': fms.PAD_BEGIN, 'Middle': fms.PAD_MIDDLE, 'End': fms.PAD_END}


def note_value(name):
    match = re.fullmatch(r'([A-G]#?)(\d)', name)
    return int(match.group(2)) * 12 + PITCHES[match.group(1)] + 1


def make_note(a, instruments, arpeggios):
    n = fms.Note()
    value = a.get('Value')
    if value == 'Stop':
        n.val, n.duration = fms.NOTE_STOP, 1
    elif value == 'Release':
        n.val = fms.NOTE_RELEASE
    elif value:
        n.val = note_value(value)
        n.duration = int(a.get('Duration', 0))
        n.release = int(a.get('Release', 0))
        n.instrument = instruments.get(a.get('Instrument'), -1)
        n.arp = arpeggios.get(a.get('Arpeggio'), -1)
        if 'SlideTarget' in a:
            n.slide = note_value(a['SlideTarget'])
            if n.slide == n.val:
                n.slide = 0
    if a.get('Attack') == 'False':
        n.flags |= 1

    def effect(key, fx, field, convert=int):
        if key in a:
            n.mask |= 1 << fx
            setattr(n, field, convert(a[key]))

    effect('Volume', fms.FX_VOLUME, 'volume')
    effect('VolumeSlideTarget', fms.FX_VOL_SLIDE, 'vol_slide')
    effect('Speed', fms.FX_SPEED, 'speed')
    effect('FinePitch', fms.FX_FINE, 'fine')
    effect('DutyCycle', fms.FX_DUTY, 'duty')
    effect('NoteDelay', fms.FX_DELAY, 'delay')
    effect('CutDelay', fms.FX_CUT, 'cut')
    effect('DeltaCounter', fms.FX_DMC, 'dmc')
    effect('PhaseReset', fms.FX_PHASE, 'phase')
    effect('EnvelopePeriod', fms.FX_ENV_PERIOD, 'env_period')
    effect('FdsModSpeed', fms.FX_FDS_SPEED, 'fds_speed')
    effect('FdsModDepth', fms.FX_FDS_DEPTH, 'fds_depth')
    if 'VibratoSpeed' in a:
        n.mask |= fms.MASK_VIB
        n.vibrato = (int(a['VibratoSpeed']) << 4) | int(a['VibratoDepth'])
    return n


def strip_effects(n, without):
    """Takes effects away from a note, as they'd be missing from plain MIDI notes."""
    if 'slide' in without:
        n.slide = 0
    if 'vibrato' in without:
        n.mask &= ~fms.MASK_VIB
    if 'volume' in without:
        n.mask &= ~((1 << fms.FX_VOLUME) | (1 << fms.FX_VOL_SLIDE))
    if 'fine' in without:
        n.mask &= ~(1 << fms.FX_FINE)
    if 'duty' in without:
        n.mask &= ~(1 << fms.FX_DUTY)
    if 'phase' in without:
        n.mask &= ~(1 << fms.FX_PHASE)
    if 'arpeggio' in without:
        n.arp = -1
    if 'noattack' in without:
        n.flags &= ~1
    if 'release' in without:
        n.release = 0
    return n


def read_envelope(a):
    length = int(a['Length'])
    values = [int(v) for v in a.get('Values', '').split(',') if v][:length]
    return dict(length=length, loop=int(a.get('Loop', -1)), release=int(a.get('Release', -1)),
                relative=a.get('Relative') == 'True', values=values + [0] * (length - len(values)))


def chip_settings(inst):
    """Script lines for what only FDS, N163, VRC7 and EPSM instruments have."""
    a, envelopes, lines = inst['chip'], inst['envelopes'], []
    join = lambda values: ' '.join(map(str, values))

    for key, size in ((4, 64), (6, int(a.get('N163WaveSize', 16)))):
        if key in envelopes:
            lines.append('W %d %d %s' % (size, envelopes[key]['length'], join(envelopes[key]['values'])))
    if 7 in envelopes:
        e = envelopes[7]
        lines.append('Y %d %d %d %s' % (e['length'], e['loop'], e['release'], join(e['values'])))
    if 5 in envelopes:
        lines.append('D ' + join(envelopes[5]['values']))
    expansion = a.get('Expansion')
    if expansion == 'FDS':
        lines.append('P ' + join([a.get('FdsMasterVolume', 0), a.get('FdsModSpeed', 0), a.get('FdsModDepth', 0), a.get('FdsModDelay', 0),
                                  int(a.get('FdsAutoMod') == 'True'), a.get('FdsAutoModNumer', 1), a.get('FdsAutoModDenom', 1)]))
    elif expansion == 'N163':
        lines.append('Q ' + a.get('N163WavePos', '0'))
    elif expansion == 'VRC7':
        lines.append('H ' + join([a.get('Vrc7Patch', 1), int(a.get('Vrc7OverrideRelease') == 'True'), int(a.get('Vrc7OverrideStop') == 'True')]
                                 + [a.get('Vrc7Reg%d' % i, 0) for i in range(8)]))
    elif expansion == 'EPSM' and a.get('EpsmPatch', '1') == '0':
        lines.append('J ' + join(a.get('EpsmReg%d' % i, 0) for i in range(31)))
    return lines


def load(path, song_index, without, epsm_periods=None):
    instruments, arps, samples, kits = [], [], {}, []
    instrument_ids, arp_ids = {}, {}
    current, song, channel, pattern = None, None, None, None
    songs_seen, default_settings = -1, None
    lines = open(path, encoding='utf-8-sig').read().splitlines()

    # Names are only known once they've been declared, and notes refer to them.
    for line in lines:
        keyword = line.strip().split(' ', 1)[0]
        a = dict(ATTRIBUTE.findall(line))
        if keyword == 'Instrument':
            instrument_ids[a['Name']] = len(instrument_ids) + 1
        elif keyword == 'Arpeggio':
            arp_ids[a['Name']] = len(arp_ids) + 1
        elif keyword == 'DPCMSample':
            samples[a['Name']] = (int(a.get('DmcInitialValue', 32)), a['Data'])

    for line in lines:
        keyword = line.strip().split(' ', 1)[0]
        a = dict(ATTRIBUTE.findall(line))

        if keyword == 'Instrument':
            settings = dict(saw=0, shape=0, auto=1, octave=3, period=1000)
            settings['saw'] = SAW_VOLUMES.get(a.get('Vrc6SawMasterVolume'), 0)
            if a.get('Expansion') == 'S5B':
                settings.update(shape=int(a['S5BEnvelopeShape']), auto=int(a['S5BEnvelopeAutoPitch'] == 'True'),
                                octave=int(a['S5BEnvelopeAutoPitchOctave']), period=int(a['S5BEnvelopePitch']))
            if a.get('Expansion') == 'EPSM':
                settings.update(shape=int(a['EPSMSquareEnvelopeShape']), auto=int(a['EPSMSquareEnvelopeAutoPitch'] == 'True'),
                                octave=int(a['EPSMSquareEnvelopeAutoPitchOctave']), period=(epsm_periods or {}).get(a['Name'], 4000))
            current = dict(id=instrument_ids[a['Name']], name=a['Name'], envelopes={}, settings=settings, chip=a)
            instruments.append(current)
        elif keyword == 'Envelope' and a.get('Type') in ENVELOPE_TYPES:
            current['envelopes'][ENVELOPE_TYPES[a['Type']]] = read_envelope(a)
        elif keyword == 'DPCMMapping':
            kits.append((current['id'], note_value(a['Note']), int(a['Pitch']), int(a['Loop'] == 'True'), *samples[a['Sample']]))
        elif keyword == 'Arpeggio':
            arps.append(dict(id=arp_ids[a['Name']], envelope=read_envelope(a)))
        elif keyword == 'Song':
            songs_seen += 1
            song = None
            if songs_seen == song_index:
                if 'NoteLength' in a:
                    note_length = int(a['NoteLength'])
                    song = dict(pattern_length=int(a['PatternLength']) * note_length, length=int(a['Length']),
                                ft_tempo=150, ft_speed=6, groove=[int(v) for v in a['Groove'].split('-')], pad=PADDING[a['GroovePaddingMode']],
                                channels=[], title=a['Name'])
                else:
                    song = dict(pattern_length=int(a['PatternLength']), length=int(a['Length']), ft_tempo=int(a['FamiTrackerTempo']),
                                ft_speed=int(a['FamiTrackerSpeed']), groove=[1], pad=fms.PAD_MIDDLE, channels=[], title=a['Name'])
                song['custom'] = [(False, 0, None, 0)] * song['length']
                default_settings = song
        elif song is None:
            continue
        elif keyword == 'PatternCustomSettings':
            if 'NoteLength' in a:
                note_length = int(a['NoteLength'])
                song['custom'][int(a['Time'])] = (True, int(a['Length']) * note_length, [int(v) for v in a['Groove'].split('-')], PADDING[a['GroovePaddingMode']])
            else:
                song['custom'][int(a['Time'])] = (True, int(a['Length']), [1], fms.PAD_MIDDLE)
        elif keyword == 'Channel':
            channel = dict(type=CHANNEL_TYPES[a['Type']], patterns={}, instances=[None] * 256)
            song['channels'].append(channel)
        elif keyword == 'Pattern':
            pattern = {}
            channel['patterns'][a['Name']] = pattern
        elif keyword == 'Note':
            note = strip_effects(make_note(a, instrument_ids, arp_ids), without)
            if note.valid() or note.mask:
                pattern[int(a['Time'])] = note
        elif keyword == 'PatternInstance':
            channel['instances'][int(a['Time'])] = channel['patterns'][a['Pattern']]

    for inst in instruments:
        inst['extra'] = chip_settings(inst)

    if default_settings is None:
        raise SystemExit('no song %d' % song_index)
    return instruments, arps, default_settings, kits, songs_seen + 1


def mixer_overrides(project_line):
    """Projects can set their own mixer, which FamiStudio's export follows."""
    a = dict(ATTRIBUTE.findall(project_line))
    lines = ['B ' + a['GlobalBassCutoffHz']] if 'GlobalBassCutoffHz' in a else []
    for index, name in enumerate(('', 'VRC6', 'VRC7', 'FDS', 'MMC5', 'N163', 'S5B', 'EPSM')):
        if name + 'VolumeDb' in a:
            lines.append('O %d %s %s %s %s' % (index, a[name + 'VolumeDb'], a.get(name + 'TrebleDb', 0), a[name + 'TrebleRolloffHz'], a.get(name + 'BassCutoffHz', 5)))
    return lines


def main():
    path, song_index, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    options = dict(zip(sys.argv[4::2], sys.argv[5::2]))
    without = set(options.get('--without', '').split(',')) - {''}
    epsm_periods = {name: int(value) for name, value in (pair.split('=') for pair in options.get('--epsm-period', '').split(',') if pair)}
    instruments, arps, song, kits, _ = load(path, song_index, without, epsm_periods)
    tempo_mode = fms.TEMPO_FAMITRACKER if 'TempoMode="FamiTracker"' in open(path, encoding='utf-8-sig').readline() else 0
    lines, frames = fms.song_to_lines(instruments, arps, song, tempo_mode)
    lines += ['K %d %d %d %d %d %s' % kit for kit in kits]
    lines += mixer_overrides(open(path, encoding='utf-8-sig').readline())
    with open(out, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print('%s: %d frames, %d channels' % (song['title'], frames, len(song['channels'])))


if __name__ == '__main__':
    main()
