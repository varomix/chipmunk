# Changes to NesSndEmu

Vendored from FamiStudio `ThirdParty/NesSndEmu` at commit `70625dd`.
Licensed under the LGPL; see `LGPL.txt`.

Removed: prebuilt binaries, `DllWrapper.cpp`, the demo and the Windows/Android project files.

## `Simple_Apu`: chunked rendering

`end_frame()` only supports a full 1/60 s frame, which adds about 17 ms of delay in a plugin.

- `end_chunk(cycles)` holds the former body of `end_frame()` and accepts any length.
- `end_frame()` now calls `end_chunk()` with its usual alternating frame length, so its behaviour is unchanged.
- `chunk_time()` returns the cycles elapsed in the current chunk.
- `clocks_for_samples()` converts a sample count to cycles using the Blip_Buffer's rounded rate.

## `Nes_Sunsoft`: uninitialized state

`reset()` didn't set `delay` or `reg`, so a new chip's timing depended on whatever was in memory.
Two identical instances could produce different audio. `reset()` now clears both.

## `ym3438`: data race on `chip_type`

Every new EPSM chip calls `OPN2_SetChipType(0)`, which wrote the shared `chip_type` while other
instances read it on their audio threads. The value doesn't change after the first call, so
`OPN2_SetChipType` now skips the write when it's the same.
