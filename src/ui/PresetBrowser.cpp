#include "ui/PresetBrowser.h"

#include "ui/Theme.h"

namespace
{
constexpr int starWidth = 24;
constexpr int statusHeight = 42;
constexpr int titleHeight = 24;

const char* channelLabel(ChannelType type)
{
    switch (type)
    {
        case ChannelType::triangle: return "TRI";
        case ChannelType::noise: return "NOI";
        case ChannelType::dpcm: return "DMC";
        case ChannelType::vrc6Pulse1: case ChannelType::vrc6Pulse2: return "VRC";
        case ChannelType::vrc6Saw: return "SAW";
        case ChannelType::mmc5Pulse1: case ChannelType::mmc5Pulse2: return "MMC";
        case ChannelType::s5bSquare1: case ChannelType::s5bSquare2: case ChannelType::s5bSquare3: return "S5B";
        default: return "PUL";
    }
}
} // namespace

PresetBrowser::PresetBrowser(ChipmunkProcessor& p)
    : processor(p),
      list("Sounds", this)
{
    for (auto* field : { &searchField, &nameField })
    {
        addAndMakeVisible(*field);
        field->setFont(juce::FontOptions(13.0f));
    }

    searchField.setTextToShowWhenEmpty("Search sounds", theme::textDim);
    searchField.onTextChange = [this] { refreshList(); };
    searchField.onReturnKey = [this] { list.grabKeyboardFocus(); };
    searchField.onEscapeKey = [this] { searchField.clear(); list.grabKeyboardFocus(); };

    nameField.setTextToShowWhenEmpty("Sound name", theme::textDim);
    nameField.onReturnKey = [this] { saveCurrent(); };
    nameField.onTextChange = [this] { processor.getDocument().setName(nameField.getText().toStdString()); };

    addAndMakeVisible(favoritesOnly);
    favoritesOnly.onClick = [this] { refreshList(); };

    addAndMakeVisible(list);
    list.setRowHeight(22);
    list.setOutlineThickness(1);

    for (auto* button : { &saveButton, &importButton })
        addAndMakeVisible(*button);

    saveButton.setTooltip("Save under this name in your Presets folder");
    importButton.setTooltip("Load a .fms, .fti or .chmk file. You can also drop one anywhere on this window");

    saveButton.onClick = [this] { saveCurrent(); };
    importButton.onClick = [this] { chooseFileToImport(); };

    refreshList();
    refreshName();
}

void PresetBrowser::refreshName()
{
    const auto name = juce::String(processor.getDocument().name());

    if (! nameField.hasKeyboardFocus(true) && nameField.getText() != name)
        nameField.setText(name, juce::dontSendNotification);
}

void PresetBrowser::refreshList()
{
    const auto& entries = processor.getLibrary().getEntries();
    const auto query = searchField.getText().trim();
    const auto current = juce::String(processor.getDocument().name());

    visible.clear();
    auto selected = -1;

    for (int i = 0; i < (int) entries.size(); ++i)
    {
        const auto name = juce::String(entries[(size_t) i].preset.name);
        const auto label = channelLabel(entries[(size_t) i].preset.channel);

        if (query.isNotEmpty() && ! name.containsIgnoreCase(query) && ! juce::String(label).containsIgnoreCase(query))
            continue;

        if (favoritesOnly.getToggleState() && ! processor.getLibrary().isFavorite(name))
            continue;

        if (name == current && selected < 0)
            selected = (int) visible.size();

        visible.push_back(i);
    }

    list.updateContent();

    ignoreSelection = true;

    if (selected >= 0)
        list.selectRow(selected);
    else
        list.deselectAllRows();

    ignoreSelection = false;
    list.repaint();
}

juce::String PresetBrowser::getVisibleName(int row) const
{
    return juce::isPositiveAndBelow(row, (int) visible.size())
             ? juce::String(processor.getLibrary().getEntries()[(size_t) visible[(size_t) row]].preset.name)
             : juce::String();
}

void PresetBrowser::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool isSelected)
{
    if (! juce::isPositiveAndBelow(row, (int) visible.size()))
        return;

    const auto& entry = processor.getLibrary().getEntries()[(size_t) visible[(size_t) row]];
    const auto name = juce::String(entry.preset.name);

    if (isSelected)
    {
        g.setGradientFill(juce::ColourGradient::horizontal(theme::accent.withAlpha(0.4f), 0.0f, theme::accent.withAlpha(0.08f), (float) width));
        g.fillRect(0, 0, width, height);
        g.setColour(theme::accent);
        g.fillRect(0, 0, 2, height);
    }

    const auto isFavorite = processor.getLibrary().isFavorite(name);

    juce::Path star;
    star.addStar({ (float) starWidth * 0.5f, (float) height * 0.5f }, 5, 2.8f, 6.5f);

    if (isFavorite)
    {
        g.setColour(theme::favorite);
        g.fillPath(star);
    }
    else
    {
        g.setColour(theme::outline.brighter(0.3f));
        g.strokePath(star, juce::PathStrokeType(1.0f));
    }

    g.setColour(theme::text);
    g.setFont(juce::FontOptions(13.0f));
    g.drawText(name, starWidth, 0, width - starWidth - 40, height, juce::Justification::centredLeft, true);

    g.setColour(entry.isImported ? theme::favorite : entry.isFactory ? theme::textDim : theme::accent);
    g.setFont(juce::FontOptions(10.0f));
    g.drawText(channelLabel(entry.preset.channel), width - 38, 0, 32, height, juce::Justification::centredRight);
}

void PresetBrowser::loadRow(int row)
{
    if (! juce::isPositiveAndBelow(row, (int) visible.size()))
        return;

    processor.loadPreset(processor.getLibrary().getEntries()[(size_t) visible[(size_t) row]].preset);
    refreshName();
    processor.audition();
}

void PresetBrowser::selectedRowsChanged(int lastRowSelected)
{
    if (! ignoreSelection && lastRowSelected >= 0)
    {
        setStatus({}, false);
        loadRow(lastRowSelected);
    }
}

void PresetBrowser::clickRow(int row, float x)
{
    if (! juce::isPositiveAndBelow(row, (int) visible.size()))
        return;

    if (x < (float) starWidth)
    {
        const auto name = getVisibleName(row);
        processor.getLibrary().setFavorite(name, ! processor.getLibrary().isFavorite(name));
        list.repaintRow(row);
    }
}

void PresetBrowser::listBoxItemClicked(int row, const juce::MouseEvent& e)
{
    clickRow(row, e.position.x);
}

void PresetBrowser::returnKeyPressed(int row)
{
    loadRow(row);
}

void PresetBrowser::step(int delta)
{
    if (visible.empty())
        return;

    const auto current = list.getSelectedRow();
    const auto next = current < 0 ? (delta > 0 ? 0 : (int) visible.size() - 1)
                                  : juce::jlimit(0, (int) visible.size() - 1, current + delta);

    if (next == current)
        processor.audition();
    else
        list.selectRow(next);
}

void PresetBrowser::setStatus(const juce::String& message, bool isError)
{
    status = message;
    statusIsError = isError;
    repaint();
}

void PresetBrowser::saveCurrent()
{
    auto preset = processor.currentPreset();
    preset.name = nameField.getText().trim().toStdString();

    juce::String error;

    if (processor.getLibrary().save(preset, error))
    {
        processor.getDocument().setName(preset.name);
        setStatus("Saved", false);
        refreshList();
    }
    else
    {
        setStatus(error, true);
    }
}

void PresetBrowser::importProject(const juce::File& file)
{
    FmsImportResult result;
    juce::String error;

    if (! PresetLibrary::readProject(file, result, error))
    {
        setStatus(error, true);
        return;
    }

    juce::String skipped;

    if (result.skippedExpansion > 0)
        skipped << " " << result.skippedExpansion << " other-chip " << (result.skippedExpansion == 1 ? "sound isn't" : "sounds aren't") << " supported yet.";

    if (result.skippedDpcm > 0)
        skipped << " " << result.skippedDpcm << " DPCM " << (result.skippedDpcm == 1 ? "kit needs" : "kits need") << " this project re-saved in FamiStudio.";

    if (result.presets.empty())
    {
        setStatus("Nothing to load from " + file.getFileName() + "." + skipped, true);
        return;
    }

    processor.getLibrary().setImported(result.presets);
    searchField.clear();
    favoritesOnly.setToggleState(false, juce::dontSendNotification);

    processor.loadPreset(result.presets.front());
    nameField.setText(result.presets.front().name, juce::dontSendNotification);

    const auto count = (int) result.presets.size();
    setStatus(juce::String(count) + (count == 1 ? " sound" : " sounds") + " added at the top of the list. Press Save to keep one." + skipped, false);
    refreshList();
    processor.audition();
}

void PresetBrowser::importFile(const juce::File& file)
{
    if (file.hasFileExtension("fms"))
    {
        importProject(file);
        return;
    }

    Preset preset;
    juce::String error;

    if (PresetLibrary::readFile(file, preset, error))
    {
        processor.loadPreset(preset);
        nameField.setText(preset.name, juce::dontSendNotification);
        setStatus("Loaded. Press Save to keep it", false);
        refreshList();
        processor.audition();
    }
    else
    {
        setStatus(error, true);
    }
}

void PresetBrowser::chooseFileToImport()
{
    chooser = std::make_unique<juce::FileChooser>("Load a sound", juce::File(), "*.fms;*.fti;*.chmk;*.vnes");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                         [this](const juce::FileChooser& fc) {
                             if (fc.getResult().existsAsFile())
                                 importFile(fc.getResult());
                         });
}

void PresetBrowser::paint(juce::Graphics& g)
{
    theme::drawPanel(g, getLocalBounds().toFloat());

    g.setColour(theme::textDim);
    g.setFont(theme::labelFont(10.5f));
    g.drawText("SOUNDS", getLocalBounds().removeFromTop(titleHeight).reduced(10, 0).withTrimmedTop(3), juce::Justification::centredLeft, false);

    if (status.isNotEmpty())
    {
        g.setColour(statusIsError ? theme::error : theme::accent);
        g.setFont(juce::FontOptions(11.0f));
        g.drawFittedText(status, getLocalBounds().reduced(8, 4).removeFromBottom(statusHeight), juce::Justification::bottomLeft, 3);
    }
}

void PresetBrowser::resized()
{
    auto area = getLocalBounds().withTrimmedTop(titleHeight).reduced(8, 0).withTrimmedBottom(8);

    auto top = area.removeFromTop(24);
    favoritesOnly.setBounds(top.removeFromRight(88));
    top.removeFromRight(6);
    searchField.setBounds(top);
    area.removeFromTop(6);

    area.removeFromBottom(statusHeight);
    area.removeFromBottom(4);
    auto saveRow = area.removeFromBottom(24);
    area.removeFromBottom(6);

    saveButton.setBounds(saveRow.removeFromRight(52));
    saveRow.removeFromRight(4);
    importButton.setBounds(saveRow.removeFromRight(60));
    saveRow.removeFromRight(4);
    nameField.setBounds(saveRow);

    list.setBounds(area);
}
