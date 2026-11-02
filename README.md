<p align="center">
  <img src="assets/logo.png" alt="The Chipmunk logo: a pixel art chipmunk in headphones playing a keyboard" width="256">
</p>

# Chipmunk

Chipmunk is a chiptune instrument for your DAW. It's built on
[FamiStudio](https://famistudio.org)'s audio engine and plays eight sound chips: 2A03, VRC6, VRC7,
MMC5, FDS, N163, S5B and EPSM.

Your DAW handles composing, arranging and mixing. Chipmunk handles sound design and chip playback.

![The Chipmunk window, with the sound browser on the left and the volume, arpeggio, pitch and duty envelopes on the right](assets/screenshot.png)

> **Early days:** Chipmunk has only been built for Apple Silicon Macs. There are no prebuilt downloads
> yet, so you need to build it yourself.

## Features

- **Eight chips, one device:** choose a channel on each track, from Pulse and Triangle to VRC6 Saw,
  VRC7 FM and EPSM Rhythm. Saved Sets keep their sounds without any external files.
- **Drawable envelopes:** draw volume, arpeggio, pitch and duty, or type them as text like
  `15 12 9 | 6 4 / 2`. Hold Shift while dragging to fine-tune a value.
- **Optional polyphony:** play up to 8 voices on one track, with each note on its own chip.
- **Drum Kit channel:** map samples and noise sounds to keys.
- **Sound browser:** 61 factory sounds, search and favorites, plus import of `.fms` and `.fti` files.
- **Play from your computer keyboard:** in the standalone app, the letter keys play notes and Z and X
  change the octave. Notes from any source light up the on-screen keyboard.
- **MIDI control:** draw FamiStudio's note effects, like slides, fine pitch and vibrato, as clip
  envelopes.

The engine matches FamiStudio's 100% at the chip register level, and its audio comes out identical or
within a rounding error.

## Build

You need macOS 11 or later, the Xcode Command Line Tools, CMake 3.22 or later, and an internet
connection for the first build, which downloads JUCE.

```bash
git clone git@github.com:varomix/chipmunk.git
```

```bash
cd chipmunk
```

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build
```

The build installs the VST3 and AU to `~/Library/Audio/Plug-Ins`. The standalone app is in
`build/Chipmunk_artefacts/Release/Standalone`. Restart your DAW to load a new build.

To run the tests:

```bash
ctest --test-dir build
```

## Getting started

- **Templates:** [`templates/`](templates) has Ableton Live Sets with tracks already set up, one for
  the 2A03 and one for all the chips.
- **Examples:** [`examples/`](examples) has songs you can open in Live and take apart.
- **Presets:** your own sounds are saved as `.chmk` files in `~/Music/Chipmunk/Presets`.

## Credits and license

Chipmunk is licensed under the [GNU AGPLv3](LICENSE). It's built on FamiStudio by BleuBleu and JUCE,
among others. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for the full list. Example songs by
other composers belong to them, as listed in the [examples README](examples/README.md).

Chipmunk isn't affiliated with or endorsed by FamiStudio.
