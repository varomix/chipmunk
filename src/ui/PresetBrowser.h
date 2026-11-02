#pragma once

#include "plugin/PluginProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

// Search, step through and audition sounds, save your own, and import files.
class PresetBrowser final : public juce::Component,
                            private juce::ListBoxModel
{
public:
    explicit PresetBrowser(ChipmunkProcessor&);

    void refreshList();
    void refreshName();
    void importFile(const juce::File&);

    void resized() override;
    void paint(juce::Graphics&) override;

    // Moves through the visible list, loading and auditioning each sound.
    void step(int delta);

    juce::TextEditor& getSearchField() { return searchField; }
    juce::TextEditor& getNameField() { return nameField; }
    juce::ListBox& getList() { return list; }
    juce::TextButton& getSaveButton() { return saveButton; }
    juce::ToggleButton& getFavoritesToggle() { return favoritesOnly; }
    int getNumVisible() const { return (int) visible.size(); }
    juce::String getVisibleName(int row) const;
    const juce::String& getStatus() const { return status; }
    void clickRow(int row, float x);

private:
    int getNumRows() override { return (int) visible.size(); }
    void paintListBoxItem(int row, juce::Graphics&, int width, int height, bool isSelected) override;
    void selectedRowsChanged(int lastRowSelected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override;
    void returnKeyPressed(int row) override;

    void loadRow(int row);
    void saveCurrent();
    void importProject(const juce::File&);
    void chooseFileToImport();
    void setStatus(const juce::String&, bool isError);

    ChipmunkProcessor& processor;
    std::vector<int> visible;
    bool ignoreSelection = false;

    juce::TextEditor searchField, nameField;
    juce::ToggleButton favoritesOnly { "Favorites" };
    juce::ListBox list;
    juce::TextButton previousButton { "<" }, nextButton { ">" }, saveButton { "Save" }, importButton { "Import" };
    juce::String status;
    bool statusIsError = false;
    std::unique_ptr<juce::FileChooser> chooser;
};
