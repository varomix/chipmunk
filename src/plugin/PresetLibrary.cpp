#include "plugin/PresetLibrary.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cstring>

PresetLibrary::PresetLibrary(const juce::File& f)
    : folder(f)
{
    loadFavorites();
    rescan();
}

juce::File PresetLibrary::defaultFolder()
{
    return juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("Chipmunk").getChildFile("Presets");
}

void PresetLibrary::rescan()
{
    entries.clear();

    for (const auto& preset : imported)
        entries.push_back({ preset, {}, false, true });

    for (const auto& preset : factoryPresets())
        entries.push_back({ preset, {}, true });

    std::vector<Entry> user;

    for (const auto& file : folder.findChildFiles(juce::File::findFiles, false, "*.chmk;*.vnes"))
    {
        Entry entry;
        std::string error;

        if (parsePreset(file.loadFileAsString().toStdString(), entry.preset, error))
        {
            entry.file = file;
            entry.preset.name = file.getFileNameWithoutExtension().toStdString();
            user.push_back(std::move(entry));
        }
    }

    std::sort(user.begin(), user.end(), [](const Entry& a, const Entry& b) {
        return juce::String(a.preset.name).compareIgnoreCase(b.preset.name) < 0;
    });

    entries.insert(entries.end(), user.begin(), user.end());
}

bool PresetLibrary::save(const Preset& preset, juce::String& error)
{
    const auto fileName = juce::File::createLegalFileName(preset.name).trim();

    if (fileName.isEmpty())
    {
        error = "Give the sound a name first";
        return false;
    }

    if (! folder.createDirectory())
    {
        error = "Can't create " + folder.getFullPathName();
        return false;
    }

    auto named = preset;
    named.name = fileName.toStdString();

    if (! folder.getChildFile(fileName + ".chmk").replaceWithText(formatPreset(named)))
    {
        error = "Can't write to " + folder.getFullPathName();
        return false;
    }

    rescan();
    return true;
}

bool PresetLibrary::readFile(const juce::File& file, Preset& out, juce::String& error)
{
    std::string message;
    auto ok = false;

    if (file.hasFileExtension("fti"))
    {
        juce::MemoryBlock data;

        if (! file.loadFileAsData(data))
        {
            error = "Can't read " + file.getFileName();
            return false;
        }

        ok = importFti(static_cast<const uint8_t*>(data.getData()), data.getSize(), out, message);
    }
    else
    {
        ok = parsePreset(file.loadFileAsString().toStdString(), out, message);

        if (ok)
            out.name = file.getFileNameWithoutExtension().toStdString();
    }

    if (! ok)
        error = file.getFileName() + ": " + juce::String(message);

    return ok;
}

bool PresetLibrary::readProject(const juce::File& file, FmsImportResult& out, juce::String& error)
{
    juce::MemoryBlock contents;

    if (! file.loadFileAsData(contents))
    {
        error = "Can't read " + file.getFileName();
        return false;
    }

    // The header is the magic number, the format version and the uncompressed size.
    constexpr size_t headerSize = 12;
    const auto* bytes = static_cast<const char*>(contents.getData());

    if (contents.getSize() < headerSize || std::memcmp(bytes, "FMS!", 4) != 0)
    {
        error = file.getFileName() + " isn't a FamiStudio project";
        return false;
    }

    const auto version = (int) juce::ByteOrder::littleEndianInt(bytes + 4);

    juce::MemoryInputStream compressed(bytes + headerSize, contents.getSize() - headerSize, false);
    juce::GZIPDecompressorInputStream decompressor(&compressed, false, juce::GZIPDecompressorInputStream::deflateFormat);
    juce::MemoryBlock project;
    decompressor.readIntoMemoryBlock(project);

    std::string message;

    if (! importFmsProject(static_cast<const uint8_t*>(project.getData()), project.getSize(), version, out, message))
    {
        error = file.getFileName() + ": " + juce::String(message);
        return false;
    }

    return true;
}

bool PresetLibrary::isSampleFile(const juce::String& path)
{
    for (const auto* extension : { ".wav", ".aif", ".aiff", ".flac", ".ogg", ".dmc" })
        if (path.endsWithIgnoreCase(extension))
            return true;

    return false;
}

bool PresetLibrary::readSampleFile(const juce::File& file, ConvertedDpcm& out, juce::String& error)
{
    const auto name = file.getFileNameWithoutExtension().toStdString();

    if (file.hasFileExtension("dmc"))
    {
        juce::MemoryBlock data;

        if (! file.loadFileAsData(data) || data.getSize() == 0)
        {
            error = "Can't read " + file.getFileName();
            return false;
        }

        auto sample = std::make_shared<DpcmSampleData>();
        sample->name = name;
        const auto* bytes = static_cast<const uint8_t*>(data.getData());
        sample->data.assign(bytes, bytes + juce::jmin((int) data.getSize(), dpcmMaxSampleBytes));

        out.sample = std::move(sample);
        out.rate = 15;
        out.wasTruncated = (int) data.getSize() > dpcmMaxSampleBytes;
        return true;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    const std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));

    if (reader == nullptr)
    {
        error = "Can't read " + file.getFileName() + " as audio";
        return false;
    }

    // The chip can hold about 8 seconds at most, so don't read a long file for nothing.
    const auto numSamples = (int) juce::jmin(reader->lengthInSamples, (juce::int64) (reader->sampleRate * 20.0));

    if (numSamples <= 0)
    {
        error = file.getFileName() + " has no audio";
        return false;
    }

    juce::AudioBuffer<float> buffer((int) reader->numChannels, numSamples);
    reader->read(&buffer, 0, numSamples, 0, true, true);

    std::vector<int16_t> mono((size_t) numSamples);
    auto peak = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        auto sum = 0.0f;

        for (int c = 0; c < buffer.getNumChannels(); ++c)
            sum += buffer.getSample(c, i);

        sum /= (float) buffer.getNumChannels();
        peak = juce::jmax(peak, std::abs(sum));
        mono[(size_t) i] = (int16_t) juce::jlimit(-32768, 32767, juce::roundToInt(sum * 32767.0f));
    }

    if (peak < 0.0005f)
    {
        error = file.getFileName() + " is silent";
        return false;
    }

    out = convertWaveToDpcm(name, std::move(mono), juce::roundToInt(reader->sampleRate));
    out.wasTruncated = out.wasTruncated || reader->lengthInSamples > numSamples;
    return true;
}

void PresetLibrary::setImported(const std::vector<Preset>& presets)
{
    imported = presets;
    rescan();
}

void PresetLibrary::loadFavorites()
{
    juce::StringArray lines;
    favoritesFile().readLines(lines);

    for (const auto& line : lines)
        if (line.isNotEmpty())
            favorites.insert(line);
}

void PresetLibrary::setFavorite(const juce::String& name, bool shouldBeFavorite)
{
    if (shouldBeFavorite)
        favorites.insert(name);
    else
        favorites.erase(name);

    juce::StringArray lines;

    for (const auto& favorite : favorites)
        lines.add(favorite);

    favoritesFile().getParentDirectory().createDirectory();
    favoritesFile().replaceWithText(lines.joinIntoString("\n"));
}
