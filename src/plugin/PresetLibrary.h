#pragma once

#include "engine/FmsImport.h"
#include "engine/Preset.h"

#include <juce_core/juce_core.h>

#include <set>
#include <vector>

// Factory sounds plus the .chmk files in the user's preset folder. Message thread only.
class PresetLibrary
{
public:
    struct Entry
    {
        Preset preset;
        juce::File file; // Doesn't exist for factory sounds.
        bool isFactory = false;
        bool isImported = false; // Held in memory until saved.
    };

    explicit PresetLibrary(const juce::File& folder);

    static juce::File defaultFolder();

    void rescan();

    const std::vector<Entry>& getEntries() const { return entries; }
    const juce::File& getFolder() const { return folder; }

    // Saves under the preset's name, replacing a user preset with the same name.
    bool save(const Preset&, juce::String& error);

    // Reads a .chmk or .fti file without adding it to the library.
    static bool readFile(const juce::File&, Preset& out, juce::String& error);

    // Reads every playable instrument in a FamiStudio project.
    static bool readProject(const juce::File&, FmsImportResult& out, juce::String& error);

    // Reads an audio file, or a raw .dmc file, as a DPCM sample.
    static bool readSampleFile(const juce::File&, ConvertedDpcm& out, juce::String& error);
    static bool isSampleFile(const juce::String& path);
    static juce::String sampleFilePatterns() { return "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.dmc"; }

    // Lists these sounds at the top of the library until they're replaced or the plugin closes.
    void setImported(const std::vector<Preset>&);

    bool isFavorite(const juce::String& name) const { return favorites.count(name) > 0; }
    void setFavorite(const juce::String& name, bool shouldBeFavorite);

private:
    void loadFavorites();
    juce::File favoritesFile() const { return folder.getSiblingFile("favorites.txt"); }

    juce::File folder;
    std::vector<Entry> entries;
    std::vector<Preset> imported;
    std::set<juce::String> favorites;
};
