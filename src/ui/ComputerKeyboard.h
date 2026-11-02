#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>

// Plays notes from the letter keys, laid out like a piano: A is C, W is C#, S is D and so on,
// with Z and X moving down and up an octave. A host keeps these keys for itself, so this is
// for the standalone app.
class ComputerKeyboard final
{
public:
    using IsKeyDown = std::function<bool(int keyCode)>;

    // The key state can be replaced for tests, which have no real keys to read.
    explicit ComputerKeyboard(juce::MidiKeyboardState&, IsKeyDown = {});

    // Starts and stops notes to match the keys that are down. Call when a key changes.
    void update();

    // Stops notes whose keys have come up, without starting any. This catches keys lifted
    // while something else had the keyboard focus.
    void releaseLifted();

    void releaseAll();

    bool usesKey(const juce::KeyPress&) const;

    // The key that plays a note, or nothing if none does at the current octave.
    juce::String labelForNote(int midiNote) const;

    int getBaseNote() const { return baseNote; }

private:
    static constexpr int numPlayKeys = 17;

    juce::MidiKeyboardState& state;
    IsKeyDown isKeyDown;
    int baseNote = 60;
    bool octaveDownHeld = false, octaveUpHeld = false;

    // Each key remembers its note, so it still stops the right one after an octave change.
    std::array<int, numPlayKeys> heldNotes;
};
