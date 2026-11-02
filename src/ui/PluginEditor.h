#pragma once

#include "plugin/PluginProcessor.h"
#include "ui/ActivityDisplay.h"
#include "ui/ComputerKeyboard.h"
#include "ui/DpcmEditor.h"
#include "ui/EnvelopeEditor.h"
#include "ui/PresetBrowser.h"
#include "ui/Theme.h"
#include "ui/WaveEditor.h"

#include <juce_audio_utils/juce_audio_utils.h>

// The handle between a Drum Kit's key list and its envelopes. Double-click to reset.
class KitDivider final : public juce::Component,
                         public juce::SettableTooltipClient
{
public:
    KitDivider();

    void paint(juce::Graphics&) override;
    void mouseEnter(const juce::MouseEvent&) override { repaint(); }
    void mouseExit(const juce::MouseEvent&) override { repaint(); }
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;

    std::function<void(int top)> onMoved;
    std::function<void()> onReset;

private:
    int grabOffset = 0;
};

// The name of the current sound, on a little screen in the header.
class PresetDisplay final : public juce::Component
{
public:
    void setSound(const juce::String& name, const juce::String& channel);
    void paint(juce::Graphics&) override;

private:
    juce::String soundName, channelName;
};

// Dark keys, so the keyboard belongs to the rest of the panel.
class ChipKeyboard final : public juce::MidiKeyboardComponent,
                           public juce::SettableTooltipClient
{
public:
    using juce::MidiKeyboardComponent::MidiKeyboardComponent;

    // Notes from elsewhere to show as pressed, along with those played on these keys.
    void setExternalNotes(const std::bitset<128>&);
    const std::bitset<128>& getExternalNotes() const { return externalNotes; }

    // What to print on each key, like the letter that plays it.
    void setKeyLabels(std::function<juce::String(int note)>);

private:
    std::bitset<128> externalNotes;
    std::function<juce::String(int)> keyLabels;

    void drawWhiteNote(int note, juce::Graphics&, juce::Rectangle<float> area, bool isDown, bool isOver, juce::Colour lineColour, juce::Colour textColour) override;
    void drawBlackNote(int note, juce::Graphics&, juce::Rectangle<float> area, bool isDown, bool isOver, juce::Colour noteFillColour) override;
};

// One window with everything visible: the current sound and scope along the top, sounds on the
// left, envelopes in the middle with playing controls above them, and a keyboard along the bottom.
class ChipmunkEditor final : public juce::AudioProcessorEditor,
                            public juce::FileDragAndDropTarget,
                            private juce::Timer
{
public:
    explicit ChipmunkEditor(ChipmunkProcessor&);
    ~ChipmunkEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;
    bool keyStateChanged(bool isKeyDown) override;

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    // Runs what the timer does. Public so it can be driven without a message loop.
    void refreshFromProcessor();

    EnvelopePanel& getPanel(int type) { return *panels[(size_t) type]; }
    PresetBrowser& getBrowser() { return browser; }
    juce::ComboBox& getS5bShapeBox() { return s5bShapeBox; }
    juce::ComboBox& getSawVolumeBox() { return sawVolumeBox; }
    juce::ComboBox& getDutyBox() { return dutyBox; }
    juce::ComboBox& getMachineBox() { return machineBox; }
    ActivityDisplay& getActivityDisplay() { return activity; }
    WavePanel& getWavePanel() { return wavePanel; }
    juce::ComboBox& getVrc7PatchBox() { return vrc7PatchBox; }
    juce::ComboBox& getN163SizeBox() { return n163SizeBox; }
    juce::TextEditor& getPatchField() { return patchField; }
    void commitPatchField();
    DpcmEditor& getDpcmEditor() { return dpcmEditor; }
    KitDivider& getKitDivider() { return kitDivider; }
    ChipKeyboard& getKeyboard() { return keyboard; }

    // On in the standalone app, where nothing else wants the letter keys.
    void setComputerKeyboardEnabled(bool);

private:
    void timerCallback() override { refreshFromProcessor(); }
    juce::Rectangle<int> envelopeArea, hintArea;
    juce::Rectangle<int> chipModule, knobModule, playModule, chipRowModule;
    void setupKnob(juce::Slider&, bool valueBelow, int valueWidth);
    void layoutControl(juce::Rectangle<int>& row, juce::Component& control, juce::Label& label, int width);
    void layoutKnob(juce::Rectangle<int>& row, juce::Slider& knob, juce::Label& label, int width);
    juce::String chipRowTitle() const;
    void layoutPanels();
    void updateForChannel(int channel);
    void refreshChipControls();
    void updateKitPanels();
    void updateVoiceControls();
    void pollComputerKeyboard();
    int kitPanelsMinHeight() const;
    void editChipSettings(const std::function<void(Instrument&)>& change);

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    ChipmunkProcessor& processor;
    theme::LookAndFeel lookAndFeel;
    juce::TooltipWindow tooltips { this, 500 };

    PresetBrowser browser;
    std::array<std::unique_ptr<EnvelopePanel>, numEnvelopeTypes> panels;
    DpcmEditor dpcmEditor;
    KitDivider kitDivider;

    juce::ComboBox channelBox, dutyBox, machineBox, voicesBox;
    juce::Slider outputSlider, glideSlider, bendSlider, vibratoSlider;
    juce::ToggleButton velocityButton { "Velocity" }, legatoButton { "Legato" }, gridButton { "Authentic grid" };
    juce::Label channelLabel, dutyLabel, voicesLabel, machineLabel, outputLabel, glideLabel, bendLabel, vibratoLabel;
    // Settings of the current sound that only some chips have.
    juce::ComboBox sawVolumeBox, s5bShapeBox;
    juce::ToggleButton s5bAutoPitchButton { "Follow note" };
    juce::Slider s5bOctaveSlider, s5bPeriodSlider;
    juce::Label sawVolumeLabel, s5bShapeLabel, s5bOctaveLabel, s5bPeriodLabel;

    WavePanel wavePanel;
    juce::ComboBox fdsMasterBox, n163SizeBox, vrc7PatchBox;
    juce::Slider fdsSpeedSlider, fdsDepthSlider, fdsDelaySlider, n163PositionSlider, n163ChannelsSlider;
    juce::ToggleButton fdsAutoButton { "Auto mod" }, vrc7ReleaseButton { "Keep playing on release" }, vrc7StopButton { "Fade on stop" };
    juce::TextEditor patchField;
    juce::Label fdsMasterLabel, fdsSpeedLabel, fdsDepthLabel, fdsDelayLabel, n163SizeLabel, n163PositionLabel, n163ChannelsLabel, vrc7PatchLabel, patchLabel;
    std::unique_ptr<SliderAttachment> n163ChannelsAttachment;

    juce::TextButton undoButton { "Undo" }, redoButton { "Redo" }, previousButton { "<" }, nextButton { ">" };
    PresetDisplay presetDisplay;
    juce::Image logo;

    std::unique_ptr<ComboAttachment> channelAttachment, dutyAttachment, machineAttachment, voicesAttachment;
    std::unique_ptr<SliderAttachment> outputAttachment, glideAttachment, bendAttachment, vibratoAttachment;
    std::unique_ptr<ButtonAttachment> velocityAttachment, legatoAttachment, gridAttachment;

    ChipKeyboard keyboard;
    ComputerKeyboard computerKeyboard;
    bool computerKeyboardEnabled = false;
    int shownBaseNote = -1;
    ActivityDisplay activity;

    int lastVersion = -1;
    int lastChannel = -1;
    bool refreshingChipControls = false;
};
