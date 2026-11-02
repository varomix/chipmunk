#include "ui/ComputerKeyboard.h"

namespace
{
// In semitones from the base note.
constexpr const char* playKeys = "awsedftgyhujkolp;";
constexpr int octaveDownKey = 'z', octaveUpKey = 'x';

// C1 to C7. The keys reach an octave and a bit above the base, which stays on the keyboard.
constexpr int lowestBaseNote = 24, highestBaseNote = 96;
constexpr float velocity = 0.9f;
} // namespace

ComputerKeyboard::ComputerKeyboard(juce::MidiKeyboardState& s, IsKeyDown keyState)
    : state(s),
      isKeyDown(std::move(keyState))
{
    if (! isKeyDown)
        isKeyDown = [](int keyCode) { return juce::KeyPress(keyCode).isCurrentlyDown(); };

    heldNotes.fill(-1);
}

void ComputerKeyboard::update()
{
    const auto octaveDown = isKeyDown(octaveDownKey), octaveUp = isKeyDown(octaveUpKey);

    if (octaveDown && ! octaveDownHeld)
        baseNote = juce::jmax(lowestBaseNote, baseNote - 12);

    if (octaveUp && ! octaveUpHeld)
        baseNote = juce::jmin(highestBaseNote, baseNote + 12);

    octaveDownHeld = octaveDown;
    octaveUpHeld = octaveUp;

    releaseLifted();

    for (int i = 0; i < numPlayKeys; ++i)
    {
        if (heldNotes[(size_t) i] < 0 && isKeyDown(playKeys[i]))
        {
            heldNotes[(size_t) i] = baseNote + i;
            state.noteOn(1, baseNote + i, velocity);
        }
    }
}

void ComputerKeyboard::releaseLifted()
{
    for (int i = 0; i < numPlayKeys; ++i)
    {
        if (heldNotes[(size_t) i] >= 0 && ! isKeyDown(playKeys[i]))
        {
            state.noteOff(1, heldNotes[(size_t) i], 0.0f);
            heldNotes[(size_t) i] = -1;
        }
    }
}

void ComputerKeyboard::releaseAll()
{
    for (auto& note : heldNotes)
    {
        if (note >= 0)
            state.noteOff(1, note, 0.0f);

        note = -1;
    }
}

bool ComputerKeyboard::usesKey(const juce::KeyPress& key) const
{
    // Shortcuts like Command-Z belong to the rest of the app.
    if (key.getModifiers().isCommandDown() || key.getModifiers().isCtrlDown() || key.getModifiers().isAltDown())
        return false;

    const auto code = (int) juce::CharacterFunctions::toLowerCase((juce::juce_wchar) key.getKeyCode());

    return code == octaveDownKey || code == octaveUpKey
        || (code > 0 && code < 128 && juce::String(playKeys).containsChar((juce::juce_wchar) code));
}

juce::String ComputerKeyboard::labelForNote(int midiNote) const
{
    const auto index = midiNote - baseNote;

    return juce::isPositiveAndBelow(index, numPlayKeys) ? juce::String::charToString((juce::juce_wchar) playKeys[index]).toUpperCase()
                                                        : juce::String();
}
