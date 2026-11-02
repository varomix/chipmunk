#include "ui/DpcmEditor.h"

#include "engine/EnvelopeEdit.h"
#include "ui/Theme.h"

namespace
{
constexpr int firstKey = 24, lastKey = 119;
constexpr int headerHeight = 44;
constexpr int statusHeight = 20;

juce::String keyName(int midiNote)
{
    return midiNoteName(midiNote);
}

void fillKeyBox(juce::ComboBox& box)
{
    for (int note = firstKey; note <= lastKey; ++note)
        box.addItem(keyName(note), note);
}
} // namespace

// One mapped key: which key, what it looks like, how fast it plays and whether it loops.
class DpcmEditor::Row final : public juce::Component
{
public:
    explicit Row(DpcmEditor& e)
        : editor(e)
    {
        fillKeyBox(keyBox);
        keyBox.setTooltip("Move this sample to another key");
        keyBox.onChange = [this] {
            if (keyBox.getSelectedId() != midiNote)
                editor.moveKey(midiNote, keyBox.getSelectedId());
        };

        for (int rate = dpcmNumRates - 1; rate >= 0; --rate)
            pitchBox.addItem(juce::String(dpcmRateHz(rate) / 1000.0f, 1) + " kHz", rate + 1);

        pitchBox.setTooltip("Playback speed. Lower is slower and deeper");
        pitchBox.onChange = [this] { editor.setKeyPitch(midiNote, pitchBox.getSelectedId() - 1); };

        loopButton.setTooltip("Repeat while the note is held, and stop when it's released");
        loopButton.onClick = [this] { editor.setKeyLoop(midiNote, loopButton.getToggleState()); };

        for (int pitch = 0; pitch < 16; ++pitch)
            noisePitchBox.addItem("Pitch " + juce::String(pitch + 1), pitch + 1);

        noisePitchBox.setTooltip("Noise pitch. Higher is brighter");
        noisePitchBox.onChange = [this] { editor.setKeyNoisePitch(midiNote, noisePitchBox.getSelectedId() - 1); };

        nameLabel.setEditable(false, true);
        nameLabel.setTooltip("Double-click to rename this sound. Keys sharing it all change");
        nameLabel.setInterceptsMouseClicks(false, false);
        nameLabel.onTextChange = [this] { editor.renameKeySound(midiNote, nameLabel.getText()); };

        addChildComponent(noisePitchBox);
        addChildComponent(nameLabel);

        removeButton.setTooltip("Remove this sample");
        removeButton.onClick = [this] { editor.removeKey(midiNote); };

        addAndMakeVisible(keyBox);
        addAndMakeVisible(pitchBox);
        addAndMakeVisible(loopButton);
        addAndMakeVisible(removeButton);

        setTooltipText();
    }

    void update(int note, const DpcmMapping& mapping)
    {
        midiNote = note;
        keyBox.setSelectedId(note, juce::dontSendNotification);

        // A noise key has its own sound and pitch in place of a sample's speed and loop.
        isNoise = mapping.sample == nullptr;
        isSelected = isNoise && editor.processor.getDocument().selectedKitKey() == midiNoteToNoteValue(note);
        pitchBox.setVisible(! isNoise);
        loopButton.setVisible(! isNoise);
        noisePitchBox.setVisible(isNoise);
        nameLabel.setVisible(isNoise);
        removeButton.setTooltip(isNoise ? "Remove this key" : "Remove this sample");

        if (isNoise)
        {
            noisePitchBox.setSelectedId((mapping.noiseNote & 15) + 1, juce::dontSendNotification);
            nameLabel.setText(juce::String::fromUTF8(mapping.noise->name.c_str()), juce::dontSendNotification);
            shownSample = nullptr;
            peaks.clear();
            repaint();
            return;
        }

        pitchBox.setSelectedId(mapping.pitch + 1, juce::dontSendNotification);
        loopButton.setToggleState(mapping.loop, juce::dontSendNotification);

        name = juce::String::fromUTF8(mapping.sample->name.c_str());
        seconds = (double) mapping.sample->data.size() * 8.0 / (double) dpcmRateHz(mapping.pitch);

        if (mapping.sample != shownSample)
        {
            shownSample = mapping.sample;
            buildWaveform();
        }

        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        g.setColour(theme::panel);
        g.fillRect(getLocalBounds().reduced(0, 1));

        auto area = waveformArea();

        g.setColour(isSelected ? theme::accent.withAlpha(0.25f) : theme::panel.darker(0.3f));
        g.fillRect(area);

        if (isNoise)
            return;

        g.setColour(theme::accent);
        const auto mid = (float) area.getCentreY();
        const auto halfHeight = (float) area.getHeight() * 0.5f - 1.0f;

        for (int x = 0; x < (int) peaks.size() && x < area.getWidth(); ++x)
        {
            const auto [low, high] = peaks[(size_t) x];
            const auto column = (float) area.getX() + (float) x;
            g.fillRect(column, mid - high * halfHeight, 1.0f, juce::jmax(1.0f, (high - low) * halfHeight));
        }

        // A backing keeps the text readable over dense waveforms.
        const auto label = name + "   " + juce::String(seconds, 2) + " s";
        const auto font = juce::FontOptions(12.0f);
        const auto labelWidth = juce::jmin(area.getWidth(), juce::roundToInt(juce::GlyphArrangement::getStringWidth(juce::Font(font), label) * 1.15f) + 20);
        const auto labelArea = area.withWidth(labelWidth).withHeight(18);

        g.setColour(theme::panel.darker(0.3f).withAlpha(0.85f));
        g.fillRect(labelArea);

        g.setColour(theme::text);
        g.setFont(font);
        g.drawText(label, labelArea.reduced(6, 0), juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(4, 6);

        keyBox.setBounds(area.removeFromLeft(72).withSizeKeepingCentre(72, 24));
        area.removeFromLeft(6);

        removeButton.setBounds(area.removeFromRight(26).withSizeKeepingCentre(26, 24));
        area.removeFromRight(6);
        loopButton.setBounds(area.removeFromRight(62).withSizeKeepingCentre(62, 24));
        area.removeFromRight(6);
        const auto pitchArea = area.removeFromRight(96).withSizeKeepingCentre(96, 24);
        pitchBox.setBounds(pitchArea);
        noisePitchBox.setBounds(pitchArea);
        nameLabel.setBounds(waveformArea().reduced(6, 0));

        buildWaveform();
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (! waveformArea().contains(e.getPosition()))
            return;

        if (isNoise)
            editor.selectKey(midiNote);

        editor.processor.auditionKey(midiNote);
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        if (isNoise && waveformArea().contains(e.getPosition()))
            nameLabel.showEditor();
    }

private:
    void setTooltipText() {}

    juce::Rectangle<int> waveformArea() const
    {
        const auto left = 4 + 72 + 6;
        const auto right = 96 + 6 + 62 + 6 + 26 + 6 + 4;
        return getLocalBounds().reduced(0, 6).withX(left).withWidth(juce::jmax(0, getWidth() - left - right));
    }

    void buildWaveform()
    {
        peaks.clear();

        const auto width = waveformArea().getWidth();

        if (shownSample == nullptr || width <= 0)
            return;

        const auto wave = dpcmToWave(shownSample->data, shownSample->initialValueDiv2);

        if (wave.empty())
            return;

        peaks.resize((size_t) width);

        for (int x = 0; x < width; ++x)
        {
            const auto start = (size_t) x * wave.size() / (size_t) width;
            const auto end = juce::jmax(start + 1, (size_t) (x + 1) * wave.size() / (size_t) width);
            auto low = 1.0f, high = -1.0f;

            for (auto i = start; i < end && i < wave.size(); ++i)
            {
                const auto value = (float) wave[i] / 32768.0f;
                low = juce::jmin(low, value);
                high = juce::jmax(high, value);
            }

            peaks[(size_t) x] = { low, high };
        }
    }

    DpcmEditor& editor;
    int midiNote = 60;
    juce::String name;
    double seconds = 0.0;
    bool isNoise = false, isSelected = false;
    std::shared_ptr<const DpcmSampleData> shownSample;
    std::vector<std::pair<float, float>> peaks;

    juce::ComboBox keyBox, pitchBox, noisePitchBox;
    juce::Label nameLabel;
    juce::ToggleButton loopButton { "Loop" };
    juce::TextButton removeButton { "X" };
};

DpcmEditor::DpcmEditor(ChipmunkProcessor& p)
    : processor(p),
      list("Keys", this)
{
    addAndMakeVisible(addButton);
    addButton.setTooltip("Add audio files. You can also drop them anywhere on this window");
    addButton.onClick = [this] { chooseFiles(); };

    addChildComponent(addNoiseButton);
    addNoiseButton.setTooltip("Add a noise sound on the next free key");
    addNoiseButton.onClick = [this] { addNoiseSound(); };

    addAndMakeVisible(startKeyLabel);
    startKeyLabel.setText("Next sample goes on", juce::dontSendNotification);
    startKeyLabel.setFont(juce::FontOptions(12.0f));
    startKeyLabel.setColour(juce::Label::textColourId, theme::textDim);
    startKeyLabel.setJustificationType(juce::Justification::centredRight);

    addAndMakeVisible(startKeyBox);
    fillKeyBox(startKeyBox);
    startKeyBox.setSelectedId(startKey, juce::dontSendNotification);
    startKeyBox.onChange = [this] { startKey = startKeyBox.getSelectedId(); };

    addAndMakeVisible(list);
    list.setRowHeight(56);
    list.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    list.setColour(juce::ListBox::outlineColourId, juce::Colours::transparentBlack);

    refresh();
}

DpcmEditor::~DpcmEditor() = default;

void DpcmEditor::refresh()
{
    keys.clear();

    if (const auto kit = processor.getDocument().kit())
        for (int value = noteMusicalMin; value <= noteMusicalMax; ++value)
            if (kit->mappings[(size_t) value].sample != nullptr || (kitMode && kit->mappings[(size_t) value].noise != nullptr))
                keys.push_back(value + 23);

    // Something is always showing in the envelope panels if there's a noise sound to show.
    if (kitMode)
    {
        auto& document = processor.getDocument();
        const auto kit = document.kit();
        const auto selected = document.selectedKitKey();

        if (kit != nullptr && (selected < 0 || kit->mappings[(size_t) selected].noise == nullptr))
        {
            auto first = -1;

            for (int value = noteMusicalMin; value <= noteMusicalMax && first < 0; ++value)
                first = kit->mappings[(size_t) value].noise != nullptr ? value : -1;

            if (first != selected)
                document.selectKitKey(first);
        }
    }

    list.updateContent();
    list.repaint();
    repaint();
}

juce::Component* DpcmEditor::refreshComponentForRow(int row, bool, juce::Component* existing)
{
    auto* rowComponent = dynamic_cast<Row*>(existing);

    if (! juce::isPositiveAndBelow(row, (int) keys.size()))
    {
        delete existing;
        return nullptr;
    }

    if (rowComponent == nullptr)
    {
        delete existing;
        rowComponent = new Row(*this);
    }

    if (const auto kit = processor.getDocument().kit())
    {
        const auto note = keys[(size_t) row];
        rowComponent->update(note, kit->mappings[(size_t) midiNoteToNoteValue(note)]);
    }

    return rowComponent;
}

void DpcmEditor::setKitMode(bool shouldShowNoise)
{
    if (kitMode == shouldShowNoise)
        return;

    kitMode = shouldShowNoise;
    addNoiseButton.setVisible(kitMode);
    resized();
    refresh();
}

void DpcmEditor::addNoiseSound()
{
    const auto current = processor.getDocument().kit();
    auto key = startKey;

    const auto isUsed = [&](int midiNote) {
        return current != nullptr && (current->mappings[(size_t) midiNoteToNoteValue(midiNote)].sample != nullptr
                                      || current->mappings[(size_t) midiNoteToNoteValue(midiNote)].noise != nullptr);
    };

    while (key <= lastKey && isUsed(key))
        ++key;

    if (key > lastKey)
    {
        setStatus("There are no free keys above " + keyName(startKey), true);
        return;
    }

    auto sound = std::make_shared<KitSound>();
    std::string error;
    sound->name = "Noise";
    parseEnvelope("15 13 11 9 7 5 4 3 2 1 0", envelopeVolume, sound->instrument.envelopes[envelopeVolume], error, envelopeDutyMaxVrc6);

    processor.getDocument().editKit([&](DpcmKit& kit) { kit.mappings[(size_t) midiNoteToNoteValue(key)].noise = sound; });
    selectKey(key);

    if (key < lastKey)
        setStartKey(key + 1);

    processor.auditionKey(key);
}

void DpcmEditor::selectKey(int midiNote)
{
    processor.getDocument().selectKitKey(midiNoteToNoteValue(midiNote));
}

void DpcmEditor::setKeyNoisePitch(int midiNote, int noisePitch)
{
    processor.getDocument().editKit([&](DpcmKit& kit) { kit.mappings[(size_t) midiNoteToNoteValue(midiNote)].noiseNote = 48 + juce::jlimit(0, 15, noisePitch); });
    processor.auditionKey(midiNote);
}

void DpcmEditor::renameKeySound(int midiNote, const juce::String& name)
{
    processor.getDocument().editKit([&](DpcmKit& kit) {
        const auto old = kit.mappings[(size_t) midiNoteToNoteValue(midiNote)].noise;

        if (old == nullptr)
            return;

        auto renamed = std::make_shared<KitSound>(*old);
        renamed->name = name.toStdString();

        for (auto& mapping : kit.mappings)
            if (mapping.noise == old)
                mapping.noise = renamed;
    });
}

void DpcmEditor::setStartKey(int midiNote)
{
    startKey = juce::jlimit(firstKey, lastKey, midiNote);
    startKeyBox.setSelectedId(startKey, juce::dontSendNotification);
}

void DpcmEditor::addFiles(const juce::StringArray& paths)
{
    const auto current = processor.getDocument().kit();
    auto kit = current != nullptr ? *current : DpcmKit {};

    auto key = startKey;
    auto added = 0, truncated = 0;
    juce::String firstError;

    for (const auto& path : paths)
    {
        ConvertedDpcm converted;
        juce::String error;

        if (! PresetLibrary::readSampleFile(juce::File(path), converted, error))
        {
            if (firstError.isEmpty())
                firstError = error;

            continue;
        }

        while (key <= lastKey && (kit.mappings[(size_t) midiNoteToNoteValue(key)].sample != nullptr || kit.mappings[(size_t) midiNoteToNoteValue(key)].noise != nullptr))
            ++key;

        if (key > lastKey)
        {
            firstError = "There are no free keys above " + keyName(startKey);
            break;
        }

        auto& mapping = kit.mappings[(size_t) midiNoteToNoteValue(key)];
        mapping = {};
        mapping.sample = converted.sample;
        mapping.pitch = converted.rate;

        ++added;
        truncated += converted.wasTruncated ? 1 : 0;
    }

    if (added > 0)
    {
        processor.getDocument().editKit([&](DpcmKit& edited) { edited = kit; });
        refresh();

        if (key < lastKey)
            setStartKey(key + 1);

        processor.auditionKey(key);
    }

    if (firstError.isNotEmpty())
    {
        setStatus(firstError, true);
    }
    else
    {
        juce::String message;
        message << "Added " << added << (added == 1 ? " sample." : " samples.");

        if (truncated > 0)
            message << " " << truncated << (truncated == 1 ? " was" : " were") << " too long for the chip and got cut off.";

        setStatus(message, false);
    }
}

void DpcmEditor::setKeyPitch(int midiNote, int pitch)
{
    processor.getDocument().editKit([&](DpcmKit& kit) { kit.mappings[(size_t) midiNoteToNoteValue(midiNote)].pitch = juce::jlimit(0, dpcmNumRates - 1, pitch); });
    processor.auditionKey(midiNote);
}

void DpcmEditor::setKeyLoop(int midiNote, bool loop)
{
    processor.getDocument().editKit([&](DpcmKit& kit) { kit.mappings[(size_t) midiNoteToNoteValue(midiNote)].loop = loop; });
}

void DpcmEditor::removeKey(int midiNote)
{
    processor.getDocument().editKit([&](DpcmKit& kit) { kit.mappings[(size_t) midiNoteToNoteValue(midiNote)] = {}; });
}

void DpcmEditor::moveKey(int from, int to)
{
    processor.getDocument().editKit([&](DpcmKit& kit) {
        std::swap(kit.mappings[(size_t) midiNoteToNoteValue(from)], kit.mappings[(size_t) midiNoteToNoteValue(to)]);
    });
}

void DpcmEditor::chooseFiles()
{
    chooser = std::make_unique<juce::FileChooser>("Add samples", juce::File(), PresetLibrary::sampleFilePatterns());
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems,
                         [this](const juce::FileChooser& fc) {
                             juce::StringArray paths;

                             for (const auto& file : fc.getResults())
                                 paths.add(file.getFullPathName());

                             if (! paths.isEmpty())
                                 addFiles(paths);
                         });
}

void DpcmEditor::setStatus(const juce::String& message, bool isError)
{
    status = message;
    statusIsError = isError;
    repaint();
}

void DpcmEditor::paint(juce::Graphics& g)
{
    g.setColour(theme::panel);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);

    g.setColour(theme::text);
    g.setFont(juce::FontOptions(14.0f, juce::Font::bold));
    g.drawText(kitMode ? "Drum kit" : "DPCM samples", 12, 0, 140, headerHeight, juce::Justification::centredLeft);

    if (keys.empty())
    {
        auto area = getLocalBounds().withTrimmedTop(headerHeight).withTrimmedBottom(statusHeight);
        g.setColour(theme::text);
        g.setFont(juce::FontOptions(16.0f));
        g.drawText("Drop audio files here", area.withTrimmedBottom(30), juce::Justification::centred);
        g.setColour(theme::textDim);
        g.setFont(juce::FontOptions(12.0f));
        g.drawText(kitMode ? "Each one goes on the next free key, and Add noise puts a noise sound there. Hits cut other hits of the same kind."
                           : "Each one goes on the next free key. Short drum hits work best, since the chip holds about a second at full quality.",
                   area.withTrimmedTop(30), juce::Justification::centred);
    }

    if (status.isNotEmpty())
    {
        g.setColour(statusIsError ? theme::error : theme::accent);
        g.setFont(juce::FontOptions(11.0f));
        g.drawText(status, getLocalBounds().reduced(12, 2).removeFromBottom(statusHeight - 4), juce::Justification::centredLeft, true);
    }
}

void DpcmEditor::resized()
{
    auto area = getLocalBounds();

    auto header = area.removeFromTop(headerHeight).reduced(8, 10);
    addButton.setBounds(header.removeFromRight(100));
    header.removeFromRight(10);

    if (kitMode)
    {
        addNoiseButton.setBounds(header.removeFromRight(84));
        header.removeFromRight(10);
    }

    startKeyBox.setBounds(header.removeFromRight(72));
    header.removeFromRight(4);
    startKeyLabel.setBounds(header.removeFromRight(130));

    area.removeFromBottom(statusHeight);
    list.setBounds(area.reduced(6, 0));
}
