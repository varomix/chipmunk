# Golden test data

Derived from FamiStudio's `FamiStudio/UnitTests` at commit `70625dd` (MIT license).

- `*.trace.gz` is FamiStudio's own `*_FamiStudioRef.txt`, compressed.
- `*.script` holds the song's instruments and each channel's per-frame note events.

`tools/fms_to_script.py` and `tools/fms_txt_to_script.py` generate these. This is how they were run, from a FamiStudio checkout's `FamiStudio/UnitTests` folder:

```bash
python3 tools/fms_to_script.py TestBase.fms TestBase_FamiStudioRef.txt tests/golden
```

`projects/` holds FamiStudio's unmodified test projects, used to test `.fms` instrument import.

FDS, N163, VRC7 and EPSM come from FamiStudio's text export instead, which any project version can produce:

```bash
FamiStudio TestEPSM.fms famistudio-txt-export TestEPSM.txt
python3 tools/fms_txt_to_script.py TestEPSM.txt 0 tests/golden/TestEPSM.script --epsm-period EPSM9=16
```
