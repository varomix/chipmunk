# Chipmunk: FamiStudio's sound as an Ableton Live instrument

A native instrument plugin that gives Live FamiStudio's NES sound. Live handles composing,
arranging and mixing. The plugin handles sound design and chip playback.

## Decisions

| Topic | Decision | Consequence |
|---|---|---|
| Workflow | One device per MIDI track, each playing one channel type | Natural in Live. Triangle, noise and DPCM don't interact through the shared mixer. Channel limits aren't enforced. |
| NES output | Sound first, export later | Instrument data stays FamiStudio-shaped, so a later converter is possible. No hardware limits in v1. |
| Chips | 2A03 with DPCM, then VRC6, MMC5, Sunsoft 5B | FDS, N163, VRC7 and EPSM are out of scope for now. |
| Distribution | Personal use for now | JUCE is fine as is. Keep the LGPL emulator in its own folder so a release stays possible. |
| Priorities | Replace arranging with Live; make instrument work fast | Build no sequencer. The instrument interface is the main product. |
| Sound shaping | Drawable per-frame envelopes in v1 | The largest interface job, and the data maps directly to later export. |

## Stack

- **C++20, JUCE 9 and CMake**, with JUCE pinned to 9.0.4 through FetchContent.
- **Formats:** VST3 and AU for Live, plus a Standalone build for debugging. macOS arm64 first.
- **Emulator:** `ThirdParty/NesSndEmu` from FamiStudio, vendored and compiled into the plugin.
- **Baseline:** FamiStudio commit `70625dd` (2026-08-10), recorded so upstream changes can be diffed later.

## Architecture

```
Live MIDI + automation
        |
        v
PluginProcessor      splits each audio block at MIDI events
        |
        +--> VoiceController   mono note logic: last-note priority, legato, bend, mod wheel
        |            |
        |            v
        |     ChannelEngine    C++ port of ChannelState*.cs: envelopes, slides, periods
        |            |  register writes, timed in CPU cycles
        |            v
        |     Simple_Apu       one per plugin instance
        |            |
        +<-- 16-bit samples, converted to float mono/stereo

Editor (message thread) --> immutable instrument snapshot --> audio thread (lock-free swap)
```

### Rules for the audio thread

- No allocation, locks or file access. Instrument edits arrive as a swapped snapshot.
- Each plugin instance owns its own `Simple_Apu`.
- Don't use `DllWrapper.cpp`. It keeps a fixed static array of 10 APUs, which breaks with
  many plugin instances, and the plugin needs a C++ interface anyway.
- DPCM reads go through `dmc_reader`'s `user_data` pointer, so samples stay per-instance.

### Timing

FamiStudio's `Simple_Apu` only flushes audio at the end of a full 1/60 s frame. Inside a plugin
that would add about 17 ms of delay and make notes late, so:

1. **Patch `Simple_Apu`** with a call that ends a short chunk at an arbitrary cycle count (about
   64 samples). The frame-length code in `end_frame` is the part to change. Keep this patch small and documented.
2. **Keep a separate instrument tick.** Envelopes still step at 60.0988 Hz (NTSC) or 50.007 Hz (PAL),
   counted independently of the audio chunks.
3. **Restart the tick on note-on by default**, so notes land on the sample Live asked for.
   An "Authentic grid" toggle keeps the tick free-running, and later export will quantize to it.

### What gets ported from FamiStudio

| Source (`FamiStudio/Source/`) | Port to | Notes |
|---|---|---|
| `ChannelStates/ChannelState.cs` | `ChannelEngine` | Keep `Update`, envelopes, slides, `GetPeriod`, `GetVolume`. Drop the `Song` and `Advance` sequencing. |
| `ChannelStateSquare/Triangle/Noise/Dpcm.cs` | Channel classes | Keep the register write order and the smooth-vibrato sweep trick. |
| Note tables in `Player/NesApu.cs` | `NoteTables` | Generated from tuning and machine type. Include the saw table for VRC6. |
| `Project/Envelope.cs` | `Envelope` | Loop, release, relative mode, type limits. |
| `Project/Instrument.cs` | `Instrument` | Only the chips in scope. |
| `Project/DPCMSample.cs` | `DpcmSample` | WAV-to-DPCM conversion, off the audio thread. |
| `IO/FamitrackerInstrumentFile.cs` | `.fti` import | Gives access to existing instruments. |

Replace the `Settings.*` references with per-instance options, such as period clamping and smooth vibrato.

## Device design

- **Channel selector:** 2A03 Pulse, Triangle, Noise, DPCM; VRC6 Pulse, Saw; MMC5 Pulse; S5B Square.
- **Global settings:** NTSC/PAL, tuning, output gain, authentic grid.
- **Envelopes:** volume, arpeggio, pitch, duty, plus S5B mixer and noise frequency for that chip.

### MIDI and automation

| Input | Effect |
|---|---|
| Voices | 1 to 8, default 1. Above 1, each note takes its own chip, so legato and glide no longer apply. Not automatable, as changing it rebuilds the chips. |
| Note | Monophonic, last-note priority. On noise, selects the noise period. On DPCM, triggers the mapped sample. |
| Velocity | Scales channel volume (0 to 15), with an off switch for fixed volume. |
| Overlapping notes | Legato: changes pitch without restarting envelopes. Optional glide uses the FamiStudio slide logic. |
| Pitch bend | Fine pitch, with a settable range. |
| Mod wheel | Vibrato depth. |
| Live automation | Volume, duty override, vibrato speed and depth, glide time, envelope on/off switches. |

### Instrument workflow

This is the part FamiStudio makes slow for you, so it gets the most attention:

- One window with no dialogs: the instrument browser, channel selector and envelope editors are all visible.
- Envelope editor: draw with the mouse, set loop and release by dragging, change length directly.
- Type or paste envelopes as text, such as `15 12 9 | 6 4 / 2`, for fast entry and reuse.
- Browser: arrow keys step through presets and audition them. Favorites and search.
- Factory presets: classic leads, basses, arps and drums for each channel type.
- Presets are portable files. `.fti` import brings in existing instruments.
- Set state is saved inside the Live Set, so a project keeps its sounds without external files.
- Live Set template: tracks already named, colored and assigned as a NES lineup.

## Milestones

Each milestone ends with something you can play in Live.

### M0: Feasibility spike (small)

- CMake project; `Simple_Apu` compiled in; a square wave on note-on in VST3 and AU.
- Prove chunked rendering works with the patch.
- Run 16 instances at once and check independence and CPU use.
- **Exit:** clean tone at 44.1 and 48 kHz, sample-accurate onset, block-size changes don't glitch.

**Status (2026-10-30):** done, confirmed by ear in Live. `pluginval` on the VST3 is still open.

- Chunked output is bit-identical to whole-frame output over 440,274 samples, with the frame sequencer active.
- Onset position varies by under half a sample at 44.1, 48, 96 and 192 kHz.
- Output is bit-identical across block sizes from 1 to 4096 samples.
- A4 pitch is within 0.01 cents of the chip's own pitch (440.397 Hz; period rounding puts that about 1.6 cents above 440 Hz).
- One instance uses about 0.09% of one core at 48 kHz with 64-sample blocks. 16 instances on separate threads match their solo renders.
- Output delay is about 8 samples at every rate and is reported to the host.
- Thread Sanitizer found a race in chip setup (`ym3438.cpp`). A mutex in `NesEngine::prepare` fixes it, and the rerun is clean.
- A full-volume pulse peaks at about 0.14 once settled, so levels are low. M1 should add an output gain.

### M1: Faithful 2A03 pulse, triangle, noise (medium)

- Port the channel engine, envelopes and note tables, using a default instrument.
- Add MIDI handling: mono logic, legato, bend, velocity.
- Build the golden test described below.
- **Exit:** register traces match FamiStudio for the test set.

**Status (2026-10-30):** the trace exit is met. Still open: listening test in Live, and `pluginval` on the VST3.

- All 25,305 register writes from FamiStudio's `TestBase` and `TestFamiTrackerTempo` traces match, in order, for pulse 1, pulse 2, triangle and noise.
- Those songs exercise slides, vibrato, fine pitch, duty, volume slides, arpeggios, phase resets, release and no-attack notes.
- Five deliberate engine bugs were each caught by the trace test.
- Added beyond FamiStudio, so tested on their own rather than against a trace: last-note priority, legato, velocity, pitch bend, mod wheel vibrato, glide.
- Bent pitch measured from the emulator's audio is within about 2 cents of target.
- Device parameters: channel, duty, output, velocity, legato, glide, bend range, vibrato speed, authentic grid. Output defaults to +6 dB.
- The engine supports PAL, but the device doesn't expose it yet. That comes with the PAL checks in M5.
- The instrument is still a flat default, because envelopes can't be edited until M2.

### M2: Envelope editor and instrument workflow (large)

- Editor, text entry, browser, presets, state save and load, `.fti` import.
- **Exit:** you can design a sound faster than in FamiStudio, and a saved Live Set reopens identically.

**Status (2026-10-30):** built and tested outside a host. Both exit conditions still need checking in Live, and `pluginval` on the VST3 is still open.

- Editor: four drawable envelopes with length, loop and release markers, a text field for each, live playhead, undo and redo, and an on-screen keyboard.
- Browser: search, favorites, step and audition, save to `~/Music/Chipmunk/Presets`, `.fti` and `.chmk` import by button or drag and drop, and 18 starter sounds.
- Edits reach the sounding note through a lock-free hand-off. On-screen keyboard notes use a lock-free queue, because JUCE's own keyboard processing takes a lock on the audio thread.
- `tests/plugin_tests.cpp` runs the real processor and editor without a host, using real mouse handlers and writing screenshots.
- A state round trip restores the sound, settings and level, with bit-identical audio and identical saved bytes. Sets saved by earlier builds still load.
- `.fms` import lists a project's 2A03 instruments at the top of the browser until saved or replaced, each on the channel its songs use most. It reads format versions 10 to 19, tested on FamiStudio's 11 test projects. Other chips come with M4.
- Only 2A03 instruments are accepted until M4. No real `.fti` file was available, so tests use files built to the format in FamiStudio's importer.
- Thread Sanitizer covers the engine and hand-off tests, not the editor.
- Undo covers envelopes only, not channel, duty or the other device parameters.
- On-screen keyboard notes start at the beginning of the audio block, not sample-accurately. Clips and MIDI input stay sample-accurate.

### M3: DPCM (medium)

- Key-mapped sample zones, WAV import and conversion, pitch and loop per key.
- **Exit:** a drum kit plays on one track from imported WAVs.

**Status (2026-10-30):** built and tested outside a host. The exit still needs checking in Live with real drum samples.

- All 30,115 register writes in FamiStudio's two 2A03 traces now match, including the DPCM channel. Samples go through the plugin's own `.fms` reader and sample processing to get there.
- Audio files (WAV, AIFF, FLAC, Ogg) and raw `.dmc` files map to the next free key, by drop or button. Each key has its sample, speed, loop, and can move to another key.
- Audio is converted by a port of FamiStudio's encoder. Unlike FamiStudio, import also normalizes, trims silence and picks the fastest rate that fits. The original audio isn't kept, so changing quality means importing again.
- Hits ring to the end of the sample whatever the note length, and only looping keys stop on note-off. FamiStudio cuts on note end, so imported songs' timing assumptions don't apply here.
- `.fms` and `.fti` imports bring their kits. Projects before format version 15 skip sample kits with a message.
- Kits reach the audio thread by pointer and are released only after it moves on, tested under the address sanitizer with samples freed during playback.

### M4: Expansion chips (medium)

- VRC6 first, then MMC5, then Sunsoft 5B with its extra envelopes.
- Match relative levels through `set_expansion_volume`, and golden-test each chip.
- **Exit:** all v1 channel types work and match FamiStudio.

**Status (2026-10-31):** built and tested outside a host. Still needs listening in Live, and `pluginval` on the VST3.

- All 230,188 register writes in FamiStudio's `TestBase`, `TestFamiTrackerTempo`, `TestVRC6`, `TestMMC5` and `TestS5B` traces match. Those are the complete traces, with every channel in those songs running.
- The device adds VRC6 Pulse, VRC6 Saw, MMC5 Pulse and S5B Square. The other VRC6, MMC5 and S5B channels exist only in the trace test, since one device plays one channel and each type sounds the same.
- S5B channels share envelope, noise and mixer registers, so S5B tracks here can't interact as they do on real hardware. That's the per-track tradeoff. Each device has all those registers to itself.
- Levels use FamiStudio's default mixer. With equal settings and 50% duty, measured against 2A03 pulse: MMC5 +0.1 dB, VRC6 pulse +1.1 dB, saw at full volume +2.8 dB, S5B +4.0 dB. They aren't compared with FamiStudio's audio.
- S5B sounds an octave below the played note, as triangle does, because FamiStudio writes the same period. Audition plays it an octave up.
- `.fti` import still takes only 2A03 instruments. FamiStudio's importer doesn't read VRC6 or S5B envelopes either, so there's no reference to check against.
- Upstream bug fixed: the S5B's `reset()` left timing state uninitialized, so identical instances could differ. See `third_party/NesSndEmu/PATCHES.md`.

### M5: Polish (medium)

- Factory preset set, Live template, oscilloscope and activity display, PAL checks, x86_64 build if wanted.
- Optional approximation of the shared mixer's volume effect on triangle and noise.
- **Exit:** a full track made in Live with no FamiStudio involved.

**Status (2026-10-31):** the templates are confirmed working in Live. The exit still needs a full track made in Live, and `pluginval` on the VST3 is still open.

- Factory sounds went from 28 to 61, with at least 4 for every playable channel. Each is tested to make sound and not clip at the default output (loudest is Saw Bass, peak 0.94). None have been judged by ear.
- `Synth Drum Kit` is generated in code from a fixed seed and uses General MIDI keys, so a kit works with no sample files.
- Machine setting: NTSC or PAL per device, saved with the Set. Older Sets open on NTSC. It can't be automated, because changing it rebuilds the chip and cuts the sound.
- PAL checks: every melodic channel is within 2.1 cents of its NTSC pitch, and envelopes step at 50 Hz. FamiStudio has no PAL reference traces, so these are measurements of the audio and not a comparison with FamiStudio. DPCM rates differ slightly on PAL by design, and PAL tuning of DPCM wasn't measured.
- I didn't confirm whether FamiStudio supports VRC6, MMC5 and S5B on PAL, so treat those combinations as this plugin's own behaviour.
- Activity display: triggered oscilloscope of the output, current note, and channel volume. Samples reach the editor through a lock-free buffer, and a silent device doesn't repaint.
- Templates: `templates/Chipmunk 2A03.als` (5 tracks) and `Chipmunk All Chips.als` (13 tracks), also copied to Live's User Library. `tools/make_live_templates.py` builds them from a real Set with a Chipmunk AU device, removing its clips and automation, because Live's format isn't documented. Tests restore every track's state and play it. They use the AU, not the VST3.
- Sample songs: `examples/varo - First Stage.als` (2A03), `varo - Neon Causeway.als` (all chips) and `varo - Crystal Cavern.als` (2A03, VRC6, MMC5, S5B) are original songs built by `tools/make_sample_songs.py`, with arrangement clips named by section. It checks bar lengths, scale and overlapping notes. `tools/render_als.cpp` plays a Set's arrangement through the real processor and writes a WAV, and `ctest` fails if a song clips or has a silent track. Previews go in the untracked `songs/previews/`. They leave out return tracks and only approximate Live's pan law.
- Left out as optional: x86_64 (this machine is arm64 and use is personal), and the shared mixer approximation (tracks can't reach each other's chips, and any level trim would be a guess without FamiStudio's audio to compare).

### M6: Every FamiStudio chip, and FamiStudio songs as Live Sets

Scope changed on 2026-10-31: VRC7, FDS, N163 and EPSM are now in, so that every demo song can be tested.

**Status (2026-10-31):** built and tested outside a host. Opening the converted Sets in Live, and listening to the new chips, are still open.

- All 748,466 register writes in FamiStudio's nine test traces match, now including `TestFDS`, `TestN163`, `TestVRC7` and `TestEPSM`.
- `tools/fms_compare.cpp` plays a song through the engine and compares with FamiStudio's own WAV export. All 68 songs in its 30 demo projects match: 54 sample for sample, and the 14 that use EPSM to within +/-1 of a sample value, about -105 dB, on a small share of samples (202 of 838,618 where it was counted). That difference wasn't traced. Most likely it's float rounding between builds.
- `SFX.fms` holds sound effects and isn't included. FamiStudio's `unit-test` command gives register logs for a project's first song, and `wav-export` gives audio for any song.
- Two projects, `Evil Clockwork` and `Pressure Burst`, set their own mixer. The comparison honors that, but a device can't, so their Sets differ more in treble.
- The device has six new channels: FDS Wave, N163 Wave, VRC7 FM, EPSM Square, EPSM FM and EPSM Rhythm. Their chips switch on only when used, by rebuilding the chip, which cuts the sound briefly.
- N163 and FM are much louder than the rest on their own. The processor takes 4 to 11 dB off them, and the converter gives that back so song mixes stay faithful.
- FDS sounds two octaves below its notes, and EPSM squares one, as in FamiStudio. N163 pitch depends on wave size, also as in FamiStudio.
- FM patches are typed as register lists rather than edited with knobs. That's enough to use imported patches, and it's a rough way to design new ones.
- Note effects are MIDI controllers, listed in `MidiEffect` in `VoiceController.h`: slide time, volume, fine pitch, duty, vibrato depth and speed, phase reset, FDS modulation and stop. They can be drawn as clip envelopes.
- `tools/fms_to_als.py` converts a song to a Live Set, with one track per channel, instrument and arpeggio. The Sets are in `songs/famistudio/`, which git ignores, as many are covers of commercial game music.
- Effects a Set can't carry yet: S5B and EPSM envelope period effects and DPCM delta counter effects are dropped. Slides are limited to 127 frames. Notes land on the song's exact frames whatever its tempo changes, so Live's grid can drift from the notes in songs that change speed.
- Live's clip envelope slots start with pitch bend and channel pressure, so CC N is `ControllerTargets.(N+2)`. This comes from Live's own files, where sustain pedal data sits in slot 66.
- A Set's notes must never overlap on one key, because the first note's note-off would end the second. `fms_to_als.py` checks this, and notes are written with 12 significant digits and end just short of their length. Spectrum scores on the full mix can't catch a dropped part, so compare channels alone as well.
- CC 87 holds a released note at its last volume until a new note or a stop, as FamiStudio does, while the host is playing. Sets use it, and they send a stop wherever FamiStudio cuts a note, since a note-off alone would play the release.
- The Drum Kit channel runs noise and DPCM on one chip. Each key plays a sample, or a noise sound at a set pitch, and CC 85 stops samples while CC 86 stops noise. `songs/famistudio-drums/` holds the same 68 songs with noise and DPCM on one Drums track. `songs/famistudio/` keeps them split.
- The DAC rests at 0 unless samples are in use, as its level makes noise and triangle quieter. Moving it pops, so the engine runs the chip past that before any audio goes out.
- The comparison spectrum is the average gap in dB between spectra in third-octave bands over time. It ignores phase, which separate chips can't match. Over the 68 songs rendered offline from their Sets, the median is 1.91 dB and the worst 4.79 dB (`Seashell Searching`). With FamiStudio's exact events on one chip per channel, the median is 1.53 dB, so most of the gap comes from channels not sharing a chip. In `Nice`'s `Cutscene`, removing all effects gives 6.47 dB.
- Six songs' mixes peak at or above 0 dB when summed, and levels run from -0.7 to +2.2 dB against FamiStudio, because separate chips lose the shared mixer's compression.

### Later: NES export (separate project)

Convert a Live Set into FamiStudio's text format (`IO/FamistudioTextFile.cs`), then use
FamiStudio's existing tools for NSF and ROM output. The converter would read clips and each
device's saved state, check the channel lineup, and quantize notes to the frame grid.
Do not design this in detail until M4 is done, but keep instrument data compatible until then.

## Verifying accuracy

- **Register traces:** FamiStudio ships reference traces in `FamiStudio/UnitTests`, written by its
  `UnitTestPlayer`. `tools/fms_to_script.py` decodes each test song into note events, and
  `tests/golden_tests.cpp` replays them through the C++ port and compares every write.
  Expansion chips in M4 can use `TestVRC6`, `TestMMC5` and `TestS5B` the same way.
- **Audio comparison:** FamiStudio's command-line WAV export (`Docs/docs/cmdline.md`) gives reference audio to compare by ear and by null test.
- **Real-time safety:** run under Thread Sanitizer and an allocation check on the audio thread.
- **Plugin validity:** `pluginval` for VST3 and `auval` for AU in CI or before each milestone sign-off.
- **In Live:** AbletonMCP can help script test sets. Final listening checks are done by you.

## Risks

| Risk | Impact | Response |
|---|---|---|
| Chunk patch disturbs the emulator's frame sequencer or Blip_Buffer | Timing or sound errors | M0 proves this first. Fallback: render full frames and report 1 frame of latency to Live. |
| C# logic depends on `Song` and `Project` more than it appears | M1 grows | Port against the trace test, one channel type at a time. |
| Hidden global state across instances | Instances interfere | Multi-instance tests in `engine_tests`, also run with `CHIPMUNK_TSAN`. `ym3438.cpp` and `emu2413.c` have static state, so `NesEngine::prepare` creates chips under a lock, and the race on `chip_type` is patched (`PATCHES.md`). |
| Level differences without the shared mixer | Mix differs from FamiStudio | Accept for v1, calibrate per-channel levels, and revisit in M5. |
| `assert(time <= frame_length)` in `end_frame` | Crash in debug builds | Handled by the M0 patch. |
| JUCE and Live AU/VST3 differences | Parameter or state bugs | Test both formats at every milestone, and treat VST3 as primary. |

## Licensing

- FamiStudio's own code is MIT. Keep its notice with the ported code.
- `NesSndEmu` is LGPL. Keep it in `third_party/`, with its license and the patch documented.
- Fine for personal use. Any public release needs a fresh look at JUCE's terms and how the LGPL part is linked.

## Proposed layout

```
Chipmunk/
  CMakeLists.txt
  third_party/NesSndEmu/      vendored emulator + LGPL text + PATCHES.md
  src/engine/                 ChannelEngine, VoiceController, Instrument, EnvelopeEdit, Preset, NoteTables
  src/plugin/                 PluginProcessor, PresetLibrary, state save/load
  src/ui/                     editor, envelope view, browser
  templates/                  Live Set templates
  tests/golden/               reference traces and test instruments
```
