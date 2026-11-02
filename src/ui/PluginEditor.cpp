#include "ui/PluginEditor.h"

namespace
{
constexpr int browserWidth = 250;
constexpr int headerHeight = 34;
constexpr int controlsHeight = 58;
constexpr int keyboardHeight = 62;
constexpr int margin = 8;
constexpr int chipControlsHeight = 48;

constexpr int channelTriangle = 1, channelNoise = 2, channelDpcm = 3, channelVrc6Pulse = 4, channelVrc6Saw = 5, channelMmc5 = 6, channelS5b = 7,
              channelFds = 8, channelN163 = 9, channelVrc7 = 10, channelEpsmSquare = 11, channelEpsmFm = 12, channelEpsmRhythm = 13, channelDrumKit = 14;
constexpr int wavePanelHeight = 150;
constexpr int kitDividerHeight = 9;
constexpr int kitListMinHeight = 130;
constexpr int kitPanelMinHeight = 80;
constexpr float kitListDefaultShare = 0.4f;

bool isSoundFile(const juce::String& path)
{
    return PresetLibrary::isSampleFile(path) || path.endsWithIgnoreCase(".fms") || path.endsWithIgnoreCase(".fti") || path.endsWithIgnoreCase(".chmk") || path.endsWithIgnoreCase(".vnes");
}
} // namespace

KitDivider::KitDivider()
{
    setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
}

void KitDivider::paint(juce::Graphics& g)
{
    g.setColour(isMouseOverOrDragging() ? theme::accent : theme::textDim.withAlpha(0.6f));

    const auto centre = getLocalBounds().getCentre().toFloat();

    for (int i = -2; i <= 2; ++i)
        g.fillEllipse(centre.x + (float) i * 7.0f - 1.5f, centre.y - 1.5f, 3.0f, 3.0f);
}

void KitDivider::mouseDown(const juce::MouseEvent& e)
{
    grabOffset = e.y;
}

void KitDivider::mouseDrag(const juce::MouseEvent& e)
{
    // In the parent's coordinates, as this moves along with the mouse.
    if (onMoved)
        onMoved(e.getEventRelativeTo(getParentComponent()).y - grabOffset);
}

void KitDivider::mouseDoubleClick(const juce::MouseEvent&)
{
    if (onReset)
        onReset();
}

ChipmunkEditor::ChipmunkEditor(ChipmunkProcessor& p)
    : AudioProcessorEditor(p),
      processor(p),
      browser(p),
      dpcmEditor(p),
      wavePanel(p.getDocument()),
      keyboard(p.getKeyboardState(), juce::MidiKeyboardComponent::horizontalKeyboard),
      activity(p)
{
    setLookAndFeel(&lookAndFeel);

    addAndMakeVisible(browser);

    auto& document = p.getDocument();

    panels[envelopeVolume] = std::make_unique<EnvelopePanel>(document, envelopeVolume, "Volume", "0 to 15, per frame");
    panels[envelopeArpeggio] = std::make_unique<EnvelopePanel>(document, envelopeArpeggio, "Arpeggio", "Semitones from the note");
    panels[envelopePitch] = std::make_unique<EnvelopePanel>(document, envelopePitch, "Pitch", "Period offset. Higher is flatter");
    panels[envelopeDutyCycle] = std::make_unique<EnvelopePanel>(document, envelopeDutyCycle, "Duty", "Pulse width. Overrides Duty above");
    panels[envelopeS5bMixer] = std::make_unique<EnvelopePanel>(document, envelopeS5bMixer, "Mixer", "0 tone+noise, 1 noise, 2 tone, 3 off");
    panels[envelopeS5bNoise] = std::make_unique<EnvelopePanel>(document, envelopeS5bNoise, "Noise Freq", "0 to 31. Lower is brighter");

    for (auto& panel : panels)
        addAndMakeVisible(*panel);

    addChildComponent(dpcmEditor);

    auto& parameters = p.getParameters();

    const auto setupLabel = [this](juce::Label& label, const juce::String& text) {
        addAndMakeVisible(label);
        label.setText(text, juce::dontSendNotification);
        label.setFont(juce::FontOptions(11.0f));
        label.setColour(juce::Label::textColourId, theme::textDim);
        label.setBorderSize({ 0, 2, 0, 0 });
    };

    setupLabel(channelLabel, "Channel");
    setupLabel(dutyLabel, "Duty");
    setupLabel(outputLabel, "Output (dB)");
    setupLabel(glideLabel, "Glide (frames)");
    setupLabel(bendLabel, "Bend range (st)");
    setupLabel(vibratoLabel, "Vibrato speed");

    addAndMakeVisible(channelBox);
    channelBox.addItemList(ChipmunkProcessor::channelNames(), 1);
    channelAttachment = std::make_unique<ComboAttachment>(parameters, "channel", channelBox);

    addAndMakeVisible(dutyBox);
    dutyBox.addItemList({ "12.5%", "25%", "50%", "75%", "", "", "", "" }, 1);
    dutyAttachment = std::make_unique<ComboAttachment>(parameters, "duty", dutyBox);

    addChildComponent(kitDivider);
    kitDivider.setTooltip("Drag to divide the room between the keys and the envelopes. Double-click to reset");
    kitDivider.onMoved = [this](int top) {
        processor.setKitListShare((float) (top - envelopeArea.getY()) / (float) juce::jmax(1, envelopeArea.getHeight()));
        layoutPanels();
    };
    kitDivider.onReset = [this] {
        processor.setKitListShare(kitListDefaultShare);
        layoutPanels();
    };

    addAndMakeVisible(voicesBox);
    voicesBox.addItem("Mono", 1);
    for (int voices = 2; voices <= PolyEngine::maxVoices; ++voices)
        voicesBox.addItem(juce::String(voices) + " voices", voices);
    voicesBox.setTooltip("How many notes can play at once on this track. Each voice is a whole chip, so it's beyond what a real NES could play and uses more CPU");
    voicesBox.onChange = [this] { updateVoiceControls(); };
    voicesAttachment = std::make_unique<ComboAttachment>(parameters, "voices", voicesBox);

    for (auto* slider : { &outputSlider, &glideSlider, &bendSlider, &vibratoSlider })
    {
        addAndMakeVisible(*slider);
        slider->setSliderStyle(juce::Slider::LinearHorizontal);
        slider->setTextBoxStyle(juce::Slider::TextBoxRight, false, 38, 20);
    }

    outputAttachment = std::make_unique<SliderAttachment>(parameters, "gain", outputSlider);
    glideAttachment = std::make_unique<SliderAttachment>(parameters, "glide", glideSlider);
    bendAttachment = std::make_unique<SliderAttachment>(parameters, "bendRange", bendSlider);
    vibratoAttachment = std::make_unique<SliderAttachment>(parameters, "vibratoSpeed", vibratoSlider);

    for (auto* button : { &velocityButton, &legatoButton, &gridButton })
        addAndMakeVisible(*button);

    velocityButton.setTooltip("Note velocity sets the channel volume");
    legatoButton.setTooltip("Overlapping notes change pitch without restarting the envelopes");
    gridButton.setTooltip("Start notes on the chip's fixed 60 Hz frames, as a real NES would");

    velocityAttachment = std::make_unique<ButtonAttachment>(parameters, "velocity", velocityButton);
    legatoAttachment = std::make_unique<ButtonAttachment>(parameters, "legato", legatoButton);
    gridAttachment = std::make_unique<ButtonAttachment>(parameters, "authenticGrid", gridButton);

    // Chip-specific settings. They're part of the sound rather than device parameters, so they
    // save with presets and take part in undo.
    setupLabel(sawVolumeLabel, "Saw volume");
    setupLabel(s5bShapeLabel, "Envelope shape");
    setupLabel(s5bOctaveLabel, "Octave offset");
    setupLabel(s5bPeriodLabel, "Envelope period");

    addAndMakeVisible(sawVolumeBox);
    sawVolumeBox.addItemList({ "Full", "Half", "Quarter" }, 1);
    sawVolumeBox.setTooltip("The saw's overall level. Full gives it the most range, and can distort");
    sawVolumeBox.onChange = [this] { editChipSettings([this](Instrument& i) { i.sawMasterVolume = sawVolumeBox.getSelectedId() - 1; }); };

    addAndMakeVisible(s5bShapeBox);
    s5bShapeBox.addItemList({ "Off", "Saw down", "Decay", "Triangle", "Decay, hold", "Saw up", "Attack, hold", "Triangle up", "Attack" }, 1);
    s5bShapeBox.setTooltip("The chip's hardware envelope. Short periods turn it into a buzzy tone");
    s5bShapeBox.onChange = [this] { editChipSettings([this](Instrument& i) { i.s5bEnvelopeShape = s5bShapeBox.getSelectedId() - 1; }); };

    addAndMakeVisible(s5bAutoPitchButton);
    s5bAutoPitchButton.setTooltip("Run the envelope at a rate that follows the note. Otherwise it uses the period");
    s5bAutoPitchButton.onClick = [this] { editChipSettings([this](Instrument& i) { i.s5bAutoPitch = s5bAutoPitchButton.getToggleState(); }); };

    for (auto* slider : { &s5bOctaveSlider, &s5bPeriodSlider })
    {
        addAndMakeVisible(*slider);
        slider->setSliderStyle(juce::Slider::LinearHorizontal);
        slider->setTextBoxStyle(juce::Slider::TextBoxRight, false, 48, 20);
    }

    s5bOctaveSlider.setRange(-8.0, 8.0, 1.0);
    s5bOctaveSlider.onValueChange = [this] { editChipSettings([this](Instrument& i) { i.s5bAutoPitchOctave = (int) s5bOctaveSlider.getValue(); }); };
    s5bPeriodSlider.setRange(0.0, 65535.0, 1.0);
    s5bPeriodSlider.setSkewFactorFromMidPoint(2000.0);
    s5bPeriodSlider.onValueChange = [this] { editChipSettings([this](Instrument& i) { i.s5bEnvelopePeriod = (int) s5bPeriodSlider.getValue(); }); };

    addAndMakeVisible(machineBox);
    machineBox.addItemList({ "NTSC", "PAL" }, 1);
    machineBox.setTooltip("PAL runs envelopes at 50 Hz instead of 60 Hz, and notes stay in tune. Changing it cuts the sound");
    machineAttachment = std::make_unique<ComboAttachment>(parameters, "machine", machineBox);

    // FDS, N163, VRC7 and EPSM settings, shown only for their own channels.
    addChildComponent(wavePanel);

    setupLabel(fdsMasterLabel, "Wave volume");
    setupLabel(fdsSpeedLabel, "Mod speed");
    setupLabel(fdsDepthLabel, "Mod depth");
    setupLabel(fdsDelayLabel, "Mod delay (frames)");
    setupLabel(n163SizeLabel, "Wave size");
    setupLabel(n163PositionLabel, "RAM position");
    setupLabel(n163ChannelsLabel, "Chip channels");
    setupLabel(vrc7PatchLabel, "Patch");
    setupLabel(patchLabel, "Patch registers");

    addAndMakeVisible(fdsMasterBox);
    fdsMasterBox.addItemList({ "100%", "66%", "50%", "40%" }, 1);
    fdsMasterBox.onChange = [this] { editChipSettings([this](Instrument& i) { i.fdsMasterVolume = fdsMasterBox.getSelectedId() - 1; }); };

    addAndMakeVisible(n163SizeBox);
    for (int size = 4; size <= 64; size += 4)
        n163SizeBox.addItem(juce::String(size), size);
    n163SizeBox.setTooltip("How many steps each wave has. Doubling it lowers the pitch an octave, as on the chip. Changing it starts from a new sine");
    n163SizeBox.onChange = [this] {
        editChipSettings([this](Instrument& i) {
            if (n163SizeBox.getSelectedId() != i.waveSize)
                i.setDefaultWave(n163SizeBox.getSelectedId(), 15);
        });
    };

    addAndMakeVisible(vrc7PatchBox);
    vrc7PatchBox.addItemList({ "Custom", "Bell", "Guitar", "Piano", "Flute", "Clarinet", "Rattling Bell", "Trumpet", "Reed Organ", "Soft Bell", "Xylophone", "Vibraphone", "Brass", "Bass Guitar", "Synthesizer", "Chorus" }, 1);
    vrc7PatchBox.onChange = [this] { editChipSettings([this](Instrument& i) { i.vrc7Patch = vrc7PatchBox.getSelectedId() - 1; }); };

    for (auto* slider : { &fdsSpeedSlider, &fdsDepthSlider, &fdsDelaySlider, &n163PositionSlider, &n163ChannelsSlider })
    {
        addAndMakeVisible(*slider);
        slider->setSliderStyle(juce::Slider::LinearHorizontal);
        slider->setTextBoxStyle(juce::Slider::TextBoxRight, false, 44, 20);
    }

    fdsSpeedSlider.setRange(0.0, 4095.0, 1.0);
    fdsSpeedSlider.setSkewFactorFromMidPoint(200.0);
    fdsDepthSlider.setRange(0.0, 63.0, 1.0);
    fdsDelaySlider.setRange(0.0, 255.0, 1.0);
    n163PositionSlider.setRange(0.0, 254.0, 2.0);
    n163PositionSlider.setTooltip("Only matters when several N163 sounds have to share the chip's memory on a real NES");

    fdsSpeedSlider.onValueChange = [this] { editChipSettings([this](Instrument& i) { i.fdsModSpeed = (int) fdsSpeedSlider.getValue(); }); };
    fdsDepthSlider.onValueChange = [this] { editChipSettings([this](Instrument& i) { i.fdsModDepth = (int) fdsDepthSlider.getValue(); }); };
    fdsDelaySlider.onValueChange = [this] { editChipSettings([this](Instrument& i) { i.fdsModDelay = (int) fdsDelaySlider.getValue(); }); };
    n163PositionSlider.onValueChange = [this] { editChipSettings([this](Instrument& i) { i.n163WavePosition = (int) n163PositionSlider.getValue(); }); };

    n163ChannelsSlider.setTooltip("The chip divides its time between channels. More of them means a lower, noisier whine, as on a real NES");
    n163ChannelsAttachment = std::make_unique<SliderAttachment>(parameters, "n163Channels", n163ChannelsSlider);

    for (auto* button : { &fdsAutoButton, &vrc7ReleaseButton, &vrc7StopButton })
        addAndMakeVisible(*button);

    fdsAutoButton.setTooltip("Run the modulation at a speed that follows the note");
    fdsAutoButton.onClick = [this] { editChipSettings([this](Instrument& i) { i.fdsAutoMod = fdsAutoButton.getToggleState(); }); };
    vrc7ReleaseButton.onClick = [this] { editChipSettings([this](Instrument& i) { i.vrc7OverrideRelease = vrc7ReleaseButton.getToggleState(); }); };
    vrc7StopButton.onClick = [this] { editChipSettings([this](Instrument& i) { i.vrc7OverrideStop = vrc7StopButton.getToggleState(); }); };

    addAndMakeVisible(patchField);
    patchField.onReturnKey = [this] { commitPatchField(); };
    patchField.onFocusLost = [this] { commitPatchField(); };
    patchField.setTextToShowWhenEmpty("Numbers from 0 to 255", theme::textDim);

    addAndMakeVisible(undoButton);
    addAndMakeVisible(redoButton);
    undoButton.onClick = [this] { processor.getDocument().undo(); refreshFromProcessor(); };
    redoButton.onClick = [this] { processor.getDocument().redo(); refreshFromProcessor(); };

    addAndMakeVisible(activity);
    addAndMakeVisible(keyboard);
    keyboard.setWantsKeyboardFocus(false);
    keyboard.clearKeyMappings();
    keyboard.setAvailableRange(24, 119);
    keyboard.setLowestVisibleKey(36);
    keyboard.setKeyWidth(18.0f);
    keyboard.setColour(juce::MidiKeyboardComponent::keyDownOverlayColourId, theme::accent);
    keyboard.setColour(juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId, theme::accent.withAlpha(0.3f));

    setWantsKeyboardFocus(true);

    setResizable(true, true);
    setResizeLimits(1000, 640, 1800, 1300);
    setSize(1100, 760);

    refreshFromProcessor();
    startTimerHz(30);
}

ChipmunkEditor::~ChipmunkEditor()
{
    stopTimer();
    setLookAndFeel(nullptr);
}

void ChipmunkEditor::refreshFromProcessor()
{
    auto& document = processor.getDocument();
    const auto version = document.version();

    if (version != lastVersion)
    {
        lastVersion = version;

        for (auto& panel : panels)
            panel->refresh();

        wavePanel.refresh();
        dpcmEditor.refresh();
        browser.refreshName();
        refreshChipControls();

        if (lastChannel == channelDrumKit)
        {
            updateKitPanels();
            layoutPanels();
        }
    }

    undoButton.setEnabled(document.canUndo());
    redoButton.setEnabled(document.canRedo());

    const auto channel = processor.getChannelIndex();

    if (channel != lastChannel)
    {
        lastChannel = channel;
        updateForChannel(channel);
    }

    for (int type = 0; type < numEnvelopeTypes; ++type)
        panels[(size_t) type]->setPlayhead(processor.getEnvelopePosition(type));

    activity.refresh();
}

void ChipmunkEditor::updateForChannel(int channel)
{
    const auto isDpcm = channel == channelDpcm;
    const auto isS5b = channel == channelS5b;
    const auto isVrc6Pulse = channel == channelVrc6Pulse;
    const auto hasDuty = channel == 0 || isVrc6Pulse || channel == channelMmc5;
    const auto hasWave = channel == channelFds || channel == channelN163;
    const auto hasS5bEnvelopes = isS5b || channel == channelEpsmSquare;
    const auto isDrums = channel == channelEpsmRhythm;
    const auto isKit = channel == channelDrumKit;
    const auto hasNoiseEnvelopes = channel == channelNoise || isKit;

    panels[envelopePitch]->setUnusedReason(hasNoiseEnvelopes ? "Not used by Noise" : "");
    panels[envelopeDutyCycle]->setUnusedReason(channel == channelTriangle ? "Not used by Triangle"
                                             : channel == channelVrc6Saw ? "Not used by the saw"
                                             : hasNoiseEnvelopes ? "1 is the short, metallic noise mode" : "");

    // VRC6 pulses have 8 widths and everything else has 4, in steps of a different size.
    const auto maxDuty = isVrc6Pulse ? envelopeDutyMaxVrc6 : 3;

    panels[envelopeDutyCycle]->setMaxDuty(maxDuty);
    panels[envelopeDutyCycle]->repaint();

    // This has to be read first, as a menu reports no selection once its label changes.
    const auto selectedDuty = dutyBox.getSelectedId();

    static const juce::StringArray vrc6Widths { "6.25%", "12.5%", "18.75%", "25%", "31.25%", "37.5%", "43.75%", "50%" };
    static const juce::StringArray pulseWidths { "12.5%", "25%", "50%", "75%" };

    for (int i = 0; i < 8; ++i)
    {
        dutyBox.changeItemText(i + 1, isVrc6Pulse ? vrc6Widths[i] : pulseWidths[i & 3]);
        dutyBox.setItemEnabled(i + 1, i <= maxDuty);
    }

    // Relabelling doesn't update the text already showing, so select the same item again.
    dutyBox.setSelectedId(0, juce::dontSendNotification);
    dutyBox.setSelectedId(selectedDuty, juce::dontSendNotification);
    dutyBox.setEnabled(hasDuty);

    // Samples just play, so these have nothing to act on.
    for (auto i = 0; i < numEnvelopeTypes; ++i)
    {
        const auto isS5bEnvelope = i == envelopeS5bMixer || i == envelopeS5bNoise;
        const auto isDutyPanel = i == envelopeDutyCycle;
        const auto showsDuty = isKit || (channel <= channelS5b && ! isS5b); // Every older channel shows it, used or not.

        panels[(size_t) i]->setVisible(! isDpcm && ! (isDrums && i != envelopeVolume) && ! (isKit && i == envelopePitch)
                                       && (isS5bEnvelope ? hasS5bEnvelopes : ! isDutyPanel || showsDuty));
    }

    dpcmEditor.setKitMode(isKit);
    dpcmEditor.setVisible(isDpcm || isKit);
    kitDivider.setVisible(isKit);

    wavePanel.setVisible(hasWave);

    if (hasWave)
        wavePanel.setChip(channel == channelFds);

    for (juce::Component* control : std::initializer_list<juce::Component*> { &glideSlider, &bendSlider, &vibratoSlider, &velocityButton, &legatoButton })
        control->setEnabled(! isDpcm);

    // The kit edits one noise sound at a time, so with none chosen the panels have nothing to show.
    if (isKit)
        updateKitPanels();

    for (juce::Component* label : std::initializer_list<juce::Component*> { &glideLabel, &bendLabel, &vibratoLabel })
        label->setAlpha(isDpcm ? 0.4f : 1.0f);

    updateVoiceControls();

    for (juce::Component* control : std::initializer_list<juce::Component*> { &sawVolumeBox, &sawVolumeLabel })
        control->setVisible(channel == channelVrc6Saw);

    for (juce::Component* control : std::initializer_list<juce::Component*> { &s5bShapeBox, &s5bShapeLabel, &s5bOctaveSlider, &s5bOctaveLabel, &s5bPeriodSlider, &s5bPeriodLabel, &s5bAutoPitchButton })
        control->setVisible(hasS5bEnvelopes);

    for (juce::Component* control : std::initializer_list<juce::Component*> { &fdsMasterBox, &fdsMasterLabel, &fdsSpeedSlider, &fdsSpeedLabel, &fdsDepthSlider, &fdsDepthLabel, &fdsDelaySlider, &fdsDelayLabel, &fdsAutoButton })
        control->setVisible(channel == channelFds);

    for (juce::Component* control : std::initializer_list<juce::Component*> { &n163SizeBox, &n163SizeLabel, &n163PositionSlider, &n163PositionLabel, &n163ChannelsSlider, &n163ChannelsLabel })
        control->setVisible(channel == channelN163);

    for (juce::Component* control : std::initializer_list<juce::Component*> { &vrc7PatchBox, &vrc7PatchLabel, &vrc7ReleaseButton, &vrc7StopButton })
        control->setVisible(channel == channelVrc7);

    refreshChipControls();

    layoutPanels();
    repaint();
}

void ChipmunkEditor::updateKitPanels()
{
    const auto hasSound = processor.getDocument().selectedKitKey() >= 0;

    for (const auto type : { envelopeVolume, envelopeArpeggio, envelopeDutyCycle })
    {
        if (panels[(size_t) type]->isVisible() != hasSound)
        {
            panels[(size_t) type]->setVisible(hasSound);
            layoutPanels();
        }
    }
}

void ChipmunkEditor::editChipSettings(const std::function<void(Instrument&)>& change)
{
    if (refreshingChipControls)
        return;

    processor.getDocument().edit(change);
}

void ChipmunkEditor::refreshChipControls()
{
    const auto instrument = processor.getDocument().snapshot();

    refreshingChipControls = true;

    sawVolumeBox.setSelectedId(instrument.sawMasterVolume + 1, juce::dontSendNotification);
    s5bShapeBox.setSelectedId(instrument.s5bEnvelopeShape + 1, juce::dontSendNotification);
    s5bAutoPitchButton.setToggleState(instrument.s5bAutoPitch, juce::dontSendNotification);
    s5bOctaveSlider.setValue(instrument.s5bAutoPitchOctave, juce::dontSendNotification);
    s5bPeriodSlider.setValue(instrument.s5bEnvelopePeriod, juce::dontSendNotification);

    fdsMasterBox.setSelectedId(instrument.fdsMasterVolume + 1, juce::dontSendNotification);
    fdsSpeedSlider.setValue(instrument.fdsModSpeed, juce::dontSendNotification);
    fdsDepthSlider.setValue(instrument.fdsModDepth, juce::dontSendNotification);
    fdsDelaySlider.setValue(instrument.fdsModDelay, juce::dontSendNotification);
    fdsAutoButton.setToggleState(instrument.fdsAutoMod, juce::dontSendNotification);
    n163SizeBox.setSelectedId(juce::jlimit(4, 64, instrument.waveSize), juce::dontSendNotification);
    n163PositionSlider.setValue(instrument.n163WavePosition, juce::dontSendNotification);
    vrc7PatchBox.setSelectedId(instrument.vrc7Patch + 1, juce::dontSendNotification);
    vrc7ReleaseButton.setToggleState(instrument.vrc7OverrideRelease, juce::dontSendNotification);
    vrc7StopButton.setToggleState(instrument.vrc7OverrideStop, juce::dontSendNotification);

    // FM patches are registers, typed as a list. VRC7's preset patches are inside the chip.
    const auto channel = processor.getChannelIndex();
    const auto usesEpsmPatch = channel == channelEpsmFm || channel == channelEpsmRhythm;
    const auto showsPatchField = usesEpsmPatch || (channel == channelVrc7 && instrument.vrc7Patch == 0);

    patchField.setVisible(showsPatchField);
    patchLabel.setVisible(showsPatchField);

    if (showsPatchField && ! patchField.hasKeyboardFocus(false))
    {
        juce::StringArray values;

        if (usesEpsmPatch)
            for (const auto reg : instrument.epsmPatchRegisters)
                values.add(juce::String((int) reg));
        else
            for (const auto reg : instrument.vrc7PatchRegisters)
                values.add(juce::String((int) reg));

        patchField.setText(values.joinIntoString(" "), false);
    }

    const auto hasEnvelope = instrument.s5bEnvelopeShape > 0;

    s5bAutoPitchButton.setEnabled(hasEnvelope);
    s5bOctaveSlider.setEnabled(hasEnvelope && instrument.s5bAutoPitch);
    s5bPeriodSlider.setEnabled(hasEnvelope && ! instrument.s5bAutoPitch);

    refreshingChipControls = false;
}

void ChipmunkEditor::commitPatchField()
{
    if (refreshingChipControls || ! patchField.isVisible())
        return;

    juce::StringArray tokens;
    tokens.addTokens(patchField.getText(), " ,", "");
    tokens.removeEmptyStrings();

    const auto usesEpsm = processor.getChannelIndex() != channelVrc7;

    if (tokens.size() == (usesEpsm ? 31 : 8))
    {
        editChipSettings([&](Instrument& i) {
            for (int t = 0; t < tokens.size(); ++t)
            {
                const auto value = (uint8_t) juce::jlimit(0, 255, tokens[t].getIntValue());

                if (usesEpsm)
                    i.epsmPatchRegisters[(size_t) t] = value;
                else
                    i.vrc7PatchRegisters[(size_t) t] = value;
            }
        });
    }

    refreshChipControls();
}

void ChipmunkEditor::paint(juce::Graphics& g)
{
    g.fillAll(theme::background);

    g.setColour(theme::text);
    g.setFont(juce::FontOptions(18.0f, juce::Font::bold));
    g.drawText("Chipmunk", margin + 2, 0, 120, headerHeight, juce::Justification::centredLeft);

    g.setColour(theme::textDim);
    g.setFont(juce::FontOptions(12.0f));
    g.drawText(processor.getChannelIndex() == channelDrumKit
                   ? "Each key plays a sample or a noise sound. Click a noise key to edit its envelopes."
                   : processor.getChannelIndex() == channelDpcm
                   ? "Drop audio files to map them to keys. MIDI notes play them, and each hit rings to the end of its sample."
                   : processor.getChannelIndex() == channelEpsmRhythm
                   ? "Keys choose the drum, repeating every six: bass, snare, top cymbal, hi-hat, tom, rim."
                   : "Drag to draw, Shift-drag to fine-tune. Drag the markers above each graph. Scroll to zoom Arpeggio and Pitch.",
               110, 0, getWidth() - 110 - 312, headerHeight, juce::Justification::centredLeft);
}

void ChipmunkEditor::layoutControl(juce::Rectangle<int>& row, juce::Component& control, juce::Label& label, int width)
{
    auto column = row.removeFromLeft(width);
    label.setBounds(column.removeFromTop(16));
    control.setBounds(column.removeFromTop(24));
    row.removeFromLeft(10);
}

void ChipmunkEditor::resized()
{
    auto area = getLocalBounds();

    auto header = area.removeFromTop(headerHeight).reduced(margin, 5);
    redoButton.setBounds(header.removeFromRight(56));
    header.removeFromRight(4);
    undoButton.setBounds(header.removeFromRight(56));
    header.removeFromRight(8);
    machineBox.setBounds(header.removeFromRight(78));
    header.removeFromRight(8);
    voicesBox.setBounds(header.removeFromRight(88));

    auto bottom = area.removeFromBottom(keyboardHeight).reduced(margin, 0).withTrimmedBottom(margin);
    activity.setBounds(bottom.removeFromLeft(browserWidth));
    bottom.removeFromLeft(margin);
    keyboard.setBounds(bottom);

    area = area.reduced(margin, 0);

    browser.setBounds(area.removeFromLeft(browserWidth).withTrimmedBottom(margin));
    area.removeFromLeft(margin);

    auto controls = area.removeFromTop(controlsHeight).reduced(0, 6);
    auto row = controls;
    const auto sliderWidth = juce::jmax(90, (row.getWidth() - 110 - 90 - 110 - 5 * 10) / 4);

    layoutControl(row, channelBox, channelLabel, 110);
    layoutControl(row, dutyBox, dutyLabel, 90);
    layoutControl(row, outputSlider, outputLabel, sliderWidth);
    layoutControl(row, glideSlider, glideLabel, sliderWidth);
    layoutControl(row, bendSlider, bendLabel, sliderWidth);
    layoutControl(row, vibratoSlider, vibratoLabel, sliderWidth);

    auto toggles = row;
    const auto toggleHeight = toggles.getHeight() / 3;
    velocityButton.setBounds(toggles.removeFromTop(toggleHeight));
    legatoButton.setBounds(toggles.removeFromTop(toggleHeight));
    gridButton.setBounds(toggles);

    area.removeFromBottom(margin);

    envelopeArea = area;
    layoutPanels();
}

void ChipmunkEditor::updateVoiceControls()
{
    // Legato and glide work between notes on one voice, and chords give each note its own.
    const auto usable = processor.getChannelIndex() != channelDpcm && voicesBox.getSelectedId() <= 1;

    legatoButton.setEnabled(usable);
    glideSlider.setEnabled(usable);
    glideLabel.setAlpha(usable ? 1.0f : 0.4f);
}

int ChipmunkEditor::kitPanelsMinHeight() const
{
    // With no sound chosen there are no panels, and the list can have all the room.
    return panels[envelopeVolume]->isVisible() ? kitPanelMinHeight * 3 + 10 : 0;
}

void ChipmunkEditor::layoutPanels()
{
    auto area = envelopeArea;

    if (lastChannel == channelDrumKit)
    {
        // Worked out here rather than when dragging, so it still fits after the window or panels change.
        const auto most = juce::jmax(kitListMinHeight, area.getHeight() - kitDividerHeight - kitPanelsMinHeight());
        const auto wanted = (int) std::lround(processor.getKitListShare() * (float) area.getHeight());

        dpcmEditor.setBounds(area.removeFromTop(juce::jlimit(kitListMinHeight, most, wanted)));
        kitDivider.setBounds(area.removeFromTop(kitDividerHeight));
    }
    else
    {
        dpcmEditor.setBounds(area);
    }

    if (lastChannel == channelFds || lastChannel == channelN163 || lastChannel == channelVrc7 || lastChannel == channelEpsmFm || lastChannel == channelEpsmRhythm)
    {
        auto row = area.removeFromTop(chipControlsHeight).reduced(0, 3);

        if (lastChannel == channelFds)
        {
            layoutControl(row, fdsMasterBox, fdsMasterLabel, 90);
            layoutControl(row, fdsSpeedSlider, fdsSpeedLabel, 170);
            layoutControl(row, fdsDepthSlider, fdsDepthLabel, 170);
            layoutControl(row, fdsDelaySlider, fdsDelayLabel, 170);
            fdsAutoButton.setBounds(row.removeFromLeft(90).withTrimmedTop(16).withHeight(24));
        }
        else if (lastChannel == channelN163)
        {
            layoutControl(row, n163SizeBox, n163SizeLabel, 90);
            layoutControl(row, n163ChannelsSlider, n163ChannelsLabel, 180);
            layoutControl(row, n163PositionSlider, n163PositionLabel, 220);
        }
        else if (lastChannel == channelVrc7)
        {
            layoutControl(row, vrc7PatchBox, vrc7PatchLabel, 130);
            vrc7ReleaseButton.setBounds(row.removeFromLeft(170).withTrimmedTop(16).withHeight(24));
            vrc7StopButton.setBounds(row.removeFromLeft(100).withTrimmedTop(16).withHeight(24));
            row.removeFromLeft(10);
        }

        if (patchField.isVisible())
        {
            auto column = row;
            patchLabel.setBounds(column.removeFromTop(16));
            patchField.setBounds(column.removeFromTop(24));
        }
    }
    else if (lastChannel == channelVrc6Saw || lastChannel == channelS5b || lastChannel == channelEpsmSquare)
    {
        auto row = area.removeFromTop(chipControlsHeight).reduced(0, 3);

        if (lastChannel == channelVrc6Saw)
        {
            layoutControl(row, sawVolumeBox, sawVolumeLabel, 110);
        }
        else
        {
            layoutControl(row, s5bShapeBox, s5bShapeLabel, 130);
            s5bAutoPitchButton.setBounds(row.removeFromLeft(110).withTrimmedTop(16).withHeight(24));
            row.removeFromLeft(10);
            layoutControl(row, s5bOctaveSlider, s5bOctaveLabel, 190);
            layoutControl(row, s5bPeriodSlider, s5bPeriodLabel, 220);
        }
    }

    if (wavePanel.isVisible())
    {
        wavePanel.setBounds(area.removeFromBottom(wavePanelHeight));
        area.removeFromBottom(5);
    }

    auto numVisible = 0;

    for (auto& panel : panels)
        numVisible += panel->isVisible() ? 1 : 0;

    if (numVisible == 0)
        return;

    constexpr int panelGap = 5;
    const auto panelHeight = (area.getHeight() - panelGap * (numVisible - 1)) / numVisible;

    for (auto& panel : panels)
    {
        if (panel->isVisible())
        {
            panel->setBounds(area.removeFromTop(panelHeight));
            area.removeFromTop(panelGap);
        }
    }
}

bool ChipmunkEditor::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0))
    {
        undoButton.triggerClick();
        return true;
    }

    if (key == juce::KeyPress('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0))
    {
        redoButton.triggerClick();
        return true;
    }

    return false;
}

bool ChipmunkEditor::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (const auto& file : files)
        if (isSoundFile(file))
            return true;

    return false;
}

void ChipmunkEditor::filesDropped(const juce::StringArray& files, int, int)
{
    juce::StringArray samples;

    for (const auto& file : files)
        if (PresetLibrary::isSampleFile(file))
            samples.add(file);

    if (! samples.isEmpty())
    {
        if (processor.getChannelIndex() != channelDrumKit)
            processor.setChannelIndex(channelDpcm);

        refreshFromProcessor();
        dpcmEditor.addFiles(samples);
        return;
    }

    for (const auto& file : files)
    {
        if (isSoundFile(file))
        {
            browser.importFile(juce::File(file));
            return;
        }
    }
}
