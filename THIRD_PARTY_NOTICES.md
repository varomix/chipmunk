# Third-party notices

Chipmunk is built on the work below. Each part stays under its own license.

| Part | Where | Author | License |
|---|---|---|---|
| FamiStudio (ported engine code, test projects and traces) | `src/engine`, `tests/golden` | BleuBleu | MIT, below |
| Nes_Snd_Emu and FamiStudio's chip additions | `third_party/NesSndEmu` | Shay Green (blargg), BleuBleu | LGPL 2.1, see `third_party/NesSndEmu/LGPL.txt` |
| Nuked-OPN2 (`ym3438`) | `third_party/NesSndEmu/nes_apu` | Alexey Khokholov (Nuke.YKT) | LGPL 2.1 or later |
| emu2413 | `third_party/NesSndEmu/nes_apu` | Mitsutaka Okazaki | MIT, below |
| emu2149 | `third_party/NesSndEmu/nes_apu` | Mitsutaka Okazaki | MIT, below |
| JUCE | Downloaded when building | Raw Material Software | Used here under the AGPLv3 |
| VST3 SDK, inside JUCE | Downloaded when building | Steinberg Media Technologies | MIT |

JUCE contains other third-party software, which `JUCE.spdx.json` lists, inside JUCE's source and each
binary download. VST is a registered trademark of Steinberg Media Technologies GmbH.

Changes to the emulator are listed in `third_party/NesSndEmu/PATCHES.md`. Example songs by other
composers are covered in `examples/README.md`.

## FamiStudio

```
MIT License

Copyright (c) 2019 BleuBleu

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## emu2413

```
The MIT License (MIT)

Copyright (c) 2001-2019 Mitsutaka Okazaki

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## emu2149

```
The MIT License (MIT)

Copyright (c) 2014 Mitsutaka Okazaki

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
