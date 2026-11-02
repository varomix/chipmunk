Chipmunk, a chiptune instrument for your DAW.
Copyright (C) 2026 varomix

This program is free software: you can redistribute it and/or modify it under the terms of the
GNU Affero General Public License, version 3, as published by the Free Software Foundation. It's
in the LICENSE file.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the license
for more details.

SOURCE CODE

The complete source for this version, including the JUCE version it was built with, is attached to
its release at https://github.com/varomix/chipmunk/releases. The repository is at
https://github.com/varomix/chipmunk.

THIRD-PARTY SOFTWARE

THIRD_PARTY_NOTICES.md lists what Chipmunk is built on and their licenses. JUCE.spdx.json lists what
JUCE itself contains.

INSTALLING

macOS: Copy Chipmunk.vst3 to ~/Library/Audio/Plug-Ins/VST3 and Chipmunk.component to
~/Library/Audio/Plug-Ins/Components. The builds aren't notarized, so macOS blocks them until you
clear the download's quarantine, in Terminal, from this folder:

    xattr -cr VST3/Chipmunk.vst3 AU/Chipmunk.component Standalone/Chipmunk.app

Windows: Copy Chipmunk.vst3 to C:\Program Files\Common Files\VST3. Windows may warn about the
standalone app, since it isn't signed.

Linux: Copy Chipmunk.vst3 to ~/.vst3.

The standalone app in the Standalone folder runs without a DAW.
