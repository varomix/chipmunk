#pragma once

#include "plugin/PluginProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

// Maps DPCM samples to keys. Each mapped key is a row with its waveform, speed and loop.
class DpcmEditor final : public juce::Component,
                         private juce::ListBoxModel
{
public:
    explicit DpcmEditor(ChipmunkProcessor&);
    ~DpcmEditor() override;

    // Rows call the edit methods below from their own callbacks, so those methods don't rebuild
    // the rows. The list catches up on the editor's next refresh, within a frame.
    void refresh();

    // On a Drum Kit, keys can also play noise sounds, and choosing one shows it in the envelope panels.
    void setKitMode(bool shouldShowNoise);
    void addNoiseSound();
    void selectKey(int midiNote);
    void setKeyNoisePitch(int midiNote, int noisePitch);
    void renameKeySound(int midiNote, const juce::String& name);

    // Puts each file on the next free key, starting at the chosen key.
    void addFiles(const juce::StringArray& paths);

    void setStartKey(int midiNote);
    void setKeyPitch(int midiNote, int pitch);
    void setKeyLoop(int midiNote, bool loop);
    void removeKey(int midiNote);

    // Moves a sample to another key, swapping with whatever is there.
    void moveKey(int fromMidiNote, int toMidiNote);

    int getNumMappedKeys() const { return (int) keys.size(); }
    const juce::String& getStatus() const { return status; }

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    class Row;

    int getNumRows() override { return (int) keys.size(); }
    void paintListBoxItem(int, juce::Graphics&, int, int, bool) override {}
    juce::Component* refreshComponentForRow(int row, bool isSelected, juce::Component* existing) override;

    void chooseFiles();
    void setStatus(const juce::String&, bool isError);

    ChipmunkProcessor& processor;
    std::vector<int> keys; // MIDI notes with a sample, ascending.
    int startKey = 60;
    bool kitMode = false;

    juce::TextButton addButton { "Add samples" }, addNoiseButton { "Add noise" };
    juce::ComboBox startKeyBox;
    juce::Label startKeyLabel;
    juce::ListBox list;
    juce::String status;
    bool statusIsError = false;
    std::unique_ptr<juce::FileChooser> chooser;
};
