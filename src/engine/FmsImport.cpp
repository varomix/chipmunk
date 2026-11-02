#include "engine/FmsImport.h"

#include "engine/EnvelopeEdit.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <stdexcept>

// Follows the Serialize methods of FamiStudio's Project, Instrument, Envelope, Song, Channel,
// Pattern and Note classes. Versions are the ones noted in those methods.
namespace
{
constexpr int oldestSupportedVersion = 10;
constexpr int newestKnownVersion = 19;
constexpr int numEnvelopesInFile = 10;
constexpr int patternInstancesPerChannel = 256;

constexpr int expansionNone = 0, expansionVrc6 = 1, expansionVrc7 = 2, expansionFds = 3,
              expansionMmc5 = 4, expansionN163 = 5, expansionS5b = 6, expansionEpsm = 7;

constexpr int channelSquare1 = 0, channelSquare2 = 1, channelTriangle = 2, channelNoise = 3, channelDpcm = 4,
              channelVrc6Pulse1 = 5, channelVrc6Pulse2 = 6, channelVrc6Saw = 7, channelMmc5Pulse1 = 15, channelMmc5Pulse2 = 16,
              channelS5b1 = 26, channelS5b3 = 28;

// How often the songs use an instrument on each kind of channel.
enum UsageSlot { usagePulse, usageTriangle, usageNoise, usageMmc5, usageVrc6Pulse, usageVrc6Saw, usageS5b, usageEpsmSquare, usageEpsmFm, usageEpsmRhythm, numUsageSlots };
using Usage = std::array<int, numUsageSlots>;

constexpr int effectVolume = 1 << 0, effectVibrato = (1 << 1) | (1 << 2), effectFinePitch = 1 << 3, effectSpeed = 1 << 4,
              effectFdsDepth = 1 << 5, effectFdsSpeed = 1 << 6, effectDuty = 1 << 7, effectDelay = 1 << 8, effectCut = 1 << 9,
              effectVolumeSlide = 1 << 10, effectDmc = 1 << 11, effectPhase = 1 << 12, effectEnvPeriod = 1 << 13;

struct Malformed : std::runtime_error
{
    Malformed() : std::runtime_error("malformed") {}
};

class Reader
{
public:
    Reader(const uint8_t* d, size_t s, int v, size_t p = 0) : data(d), size(s), version(v), pos(p) {}

    void need(size_t count) const
    {
        if (count > size - pos)
            throw Malformed();
    }

    template <typename T>
    T read()
    {
        need(sizeof(T));
        T value;
        std::memcpy(&value, data + pos, sizeof(T)); // Files are little-endian, as are the Macs this runs on.
        pos += sizeof(T);
        return value;
    }

    int i32() { return read<int32_t>(); }
    int u16() { return read<uint16_t>(); }
    int i16() { return read<int16_t>(); }
    int u8() { return read<uint8_t>(); }
    int i8() { return read<int8_t>(); }
    bool flag() { return read<uint8_t>() != 0; }

    void skip(size_t count)
    {
        need(count);
        pos += count;
    }

    size_t count(size_t elementSize)
    {
        const auto n = i32();

        if (n < 0)
            throw Malformed();

        need((size_t) n * elementSize);
        return (size_t) n;
    }

    // UTF-16 since version 14, ASCII before that. Returned as UTF-8.
    std::string string()
    {
        const auto length = i32();

        if (length < 0)
            return {};

        need((size_t) length);
        const auto* bytes = data + pos;
        pos += (size_t) length;

        if (version < 14)
            return { reinterpret_cast<const char*>(bytes), (size_t) length };

        if (length % 2 != 0)
            throw Malformed();

        std::string text;

        for (int i = 0; i + 1 < length; i += 2)
        {
            uint32_t c = (uint32_t) bytes[i] | ((uint32_t) bytes[i + 1] << 8);

            if (c >= 0xd800 && c < 0xdc00 && i + 3 < length)
            {
                const uint32_t low = (uint32_t) bytes[i + 2] | ((uint32_t) bytes[i + 3] << 8);
                c = 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00);
                i += 2;
            }

            if (c == 0)
                continue;

            if (c < 0x80)
            {
                text += (char) c;
            }
            else if (c < 0x800)
            {
                text += (char) (0xc0 | (c >> 6));
                text += (char) (0x80 | (c & 0x3f));
            }
            else if (c < 0x10000)
            {
                text += (char) (0xe0 | (c >> 12));
                text += (char) (0x80 | ((c >> 6) & 0x3f));
                text += (char) (0x80 | (c & 0x3f));
            }
            else
            {
                text += (char) (0xf0 | (c >> 18));
                text += (char) (0x80 | ((c >> 12) & 0x3f));
                text += (char) (0x80 | ((c >> 6) & 0x3f));
                text += (char) (0x80 | (c & 0x3f));
            }
        }

        return text;
    }

    void skipInt32Array()
    {
        const auto n = i32();

        if (n > 0)
            skip((size_t) n * 4);
    }

    void skipInt16Array()
    {
        const auto n = i32();

        if (n > 0)
            skip((size_t) n * 2);
    }

    bool atEnd() const { return pos == size; }
    size_t position() const { return pos; }

    const int version;

private:
    const uint8_t* data;
    size_t size;
    size_t pos;
};

struct FileMapping
{
    int note, sampleId, pitch;
    bool loop, overrideInitialValue;
    int initialValueDiv2;
};

struct FileInstrument
{
    int id = -1;
    int expansion = expansionNone;
    std::vector<FileMapping> mappings;
    std::string name;
    Instrument instrument;
};

using SampleMap = std::map<int, std::shared_ptr<const DpcmSampleData>>;

// Scanning for a section tries every offset, and most can be ruled out by a look at their first
// fields. Failing by exception is far slower, and projects can hold a megabyte of samples to scan.
bool looksLikeListOfNamedItems(const uint8_t* data, size_t size, size_t position)
{
    const auto readInt = [&](size_t offset) {
        int32_t value;
        std::memcpy(&value, data + offset, 4);
        return value;
    };

    if (position + 4 > size)
        return false;

    const auto count = readInt(position);

    if (count < 0 || count > 1024)
        return false;

    if (count == 0)
        return true;

    // Each item starts with an id and then a name, whose length comes first.
    if (position + 12 > size)
        return false;

    const auto nameLength = readInt(position + 8);
    return nameLength >= -1 && nameLength <= 1024 && (nameLength < 0 || position + 12 + (size_t) nameLength <= size);
}

// Samples are stored just before the instruments. Throws if they don't end exactly at "end".
std::map<int, DpcmSource> readSampleSources(Reader& r, size_t end, std::map<int, std::string>& names)
{
    std::map<int, DpcmSource> sources;
    const auto numSamples = r.count(1);

    for (size_t i = 0; i < numSamples; ++i)
    {
        const auto id = r.i32();
        names[id] = r.string();

        DpcmSource source;
        source.isWave = r.flag();

        if (source.isWave)
        {
            source.waveRate = r.i32();
            const auto numValues = r.count(2);

            source.wave.resize(numValues);
            for (auto& value : source.wave)
                value = (int16_t) r.i16();

            if (source.waveRate <= 0)
                throw Malformed();
        }
        else
        {
            const auto numBytes = r.count(1);

            source.dmc.resize(numBytes);
            for (auto& value : source.dmc)
                value = (uint8_t) r.u8();
        }

        r.i32(); // color
        if (r.version >= 15) r.i32(); // bank
        if (r.version >= 16) r.string(); // folder

        source.processingRate = r.i32();
        r.i32(); // preview rate
        source.volumeAdjust = r.i32();
        const auto padding = r.i32();
        source.reverseBits = r.flag();
        source.trimZeroVolume = r.flag();
        source.palProcessing = r.flag();

        if (source.processingRate < 0 || source.processingRate >= dpcmNumRates || padding < 0 || padding > 6)
            throw Malformed();

        source.padding = (DpcmPadding) padding;

        for (auto& point : source.volumeEnvelope)
        {
            point.sample = r.i32();
            point.volume = r.read<float>();
        }

        r.string(); // source file name

        if (r.version >= 11)
        {
            source.finePitch = r.read<float>();
            source.initialValueDiv2 = r.i32();
        }

        if (! (source.finePitch > 0.0f) || source.initialValueDiv2 < 0 || source.initialValueDiv2 > 63)
            throw Malformed();

        sources[id] = std::move(source);
    }

    if (r.position() != end)
        throw Malformed();

    return sources;
}

void readEnvelope(Reader& r, Envelope* out, int type)
{
    const auto length = r.i32();
    const auto loop = r.i32();
    const auto release = r.i32();
    const auto relative = r.flag();
    const auto numValues = r.count(1);

    if (out != nullptr)
    {
        out->length = length;
        out->loop = loop;
        out->release = release;
        out->relative = relative;

        for (size_t i = 0; i < numValues; ++i)
        {
            const auto value = r.i8();

            if (i < out->values.size())
                out->values[i] = (int8_t) value;
        }

        sanitizeEnvelope(*out, type);
    }
    else
    {
        r.skip(numValues);
    }
}

// FamiStudio's waveform and modulation envelopes, stored directly in the instrument here.
template <typename Destination>
int readRawEnvelope(Reader& r, Destination& destination, Envelope* points = nullptr)
{
    const auto length = r.i32();
    const auto loop = r.i32();
    const auto release = r.i32();
    r.flag();
    const auto numValues = r.count(1);

    if (points != nullptr)
    {
        points->loop = loop;
        points->release = release;
    }

    for (size_t i = 0; i < numValues; ++i)
    {
        const auto value = r.i8();

        if (i < destination.size())
            destination[i] = (std::decay_t<decltype(destination[i])>) value;
    }

    return std::clamp(length, 0, (int) destination.size());
}

void readExpansionSettings(Reader& r, int expansion, Instrument& settings)
{
    const auto v = r.version;

    switch (expansion)
    {
        case expansionNone:
        case expansionMmc5:
            break;

        case expansionFds:
            settings.fdsMasterVolume = std::clamp(r.u8(), 0, 3);
            r.skip(2); // wave and modulation presets
            settings.fdsModSpeed = std::clamp(r.u16(), 0, 4095);
            settings.fdsModDepth = std::clamp(r.u8(), 0, 63);
            settings.fdsModDelay = r.u8();
            settings.waveSize = 64;
            if (v >= 18) r.skip(1);
            if (v >= 16)
            {
                settings.fdsAutoMod = r.flag();
                settings.fdsAutoModDenominator = std::clamp(r.u8(), 1, 32);
                settings.fdsAutoModNumerator = std::clamp(r.u8(), 1, 32);
            }
            if (v >= 14) { r.skip(4 + 4 + 1); r.skipInt16Array(); }
            break;

        case expansionN163:
            r.skip(1); // wave preset
            settings.waveSize = std::clamp(r.u8(), 4, 64);
            settings.n163WavePosition = r.u8();
            if (v >= 14) { r.skip(1 + 4 + 4 + 1); r.skipInt16Array(); }
            if (v >= 17) r.skip(1);
            break;

        case expansionS5b:
            if (v >= 16)
            {
                settings.s5bAutoPitch = r.flag();
                settings.s5bAutoPitchOctave = std::clamp(r.i8(), -8, 8);
                settings.s5bEnvelopeShape = std::clamp(r.u8(), 0, 8);
                settings.s5bEnvelopePeriod = r.u16();
            }
            break;

        case expansionVrc7:
            settings.vrc7Patch = std::clamp(r.u8(), 0, 15);
            for (auto& reg : settings.vrc7PatchRegisters)
                reg = (uint8_t) r.u8();
            // Older projects were made when these behaved this way.
            settings.vrc7OverrideRelease = v >= 19 ? r.flag() : true;
            settings.vrc7OverrideStop = v >= 19 ? r.flag() : false;
            break;

        case expansionEpsm:
            r.skip(1); // patch number, as the registers are always stored
            for (auto& reg : settings.epsmPatchRegisters)
                reg = (uint8_t) r.u8();
            if (v >= 16)
            {
                settings.s5bAutoPitch = r.flag();
                settings.s5bAutoPitchOctave = std::clamp(r.i8(), -8, 8);
                settings.s5bEnvelopeShape = std::clamp(r.u8(), 0, 8);
                settings.s5bEnvelopePeriod = r.u16();
            }
            break;

        case expansionVrc6:
            settings.sawMasterVolume = std::clamp(r.u8(), 0, 2);
            break;

        default:
            throw Malformed();
    }
}

FileInstrument readInstrument(Reader& r)
{
    FileInstrument result;
    result.id = r.i32();
    result.name = r.string();
    r.i32(); // color
    result.expansion = r.i32();

    if (result.expansion < expansionNone || result.expansion > expansionEpsm)
        throw Malformed();

    readExpansionSettings(r, result.expansion, result.instrument);

    const auto mask = r.version < 15 ? r.u8() : r.u16();

    for (int i = 0; i < numEnvelopesInFile; ++i)
    {
        if ((mask & (1 << i)) != 0)
        {
            // FamiStudio's first four envelope types are the same as ours, and it numbers the S5B's 8 and 9.
            const auto ours = i < 4 ? i : i == 8 ? (int) envelopeS5bMixer : i == 9 ? (int) envelopeS5bNoise : -1;
            auto& instrument = result.instrument;

            if (ours >= 0)
                readEnvelope(r, &instrument.envelopes[(size_t) ours], ours);
            else if (i == 4 || i == 6) // FDS and N163 waveforms.
                instrument.waveLength = readRawEnvelope(r, instrument.wave);
            else if (i == 5)           // FDS modulation.
                readRawEnvelope(r, instrument.fdsModulation);
            else                       // How long each waveform lasts.
                instrument.waveRepeat.length = readRawEnvelope(r, instrument.waveRepeat.values, &instrument.waveRepeat);
        }
    }

    if (r.version >= 16)
        r.string(); // folder

    if (result.expansion == expansionNone && r.version >= 15)
    {
        const auto numMappings = r.count(4);

        for (size_t i = 0; i < numMappings; ++i)
            result.mappings.push_back({ r.i32() });

        for (auto& mapping : result.mappings)
        {
            mapping.sampleId = r.i32();
            mapping.loop = r.flag();
            mapping.pitch = r.i32();
            mapping.overrideInitialValue = r.flag();
            mapping.initialValueDiv2 = r.i32();
        }
    }

    return result;
}

void readNote(Reader& r, int channelType, std::map<int, Usage>& usage, std::map<int, int>& dpcmUsage)
{
    const auto v = r.version;
    const auto value = r.u8();
    r.u8(); // flags

    if (value != 0 && value != 0x80 && value != 0xff)
    {
        r.u8(); // slide
        const auto instrument = r.i32();
        r.skip(2 + 2); // duration, release

        if (channelType == channelDpcm)
            ++dpcmUsage[instrument];
        else if (channelType == channelSquare1 || channelType == channelSquare2)
            ++usage[instrument][usagePulse];
        else if (channelType == channelTriangle)
            ++usage[instrument][usageTriangle];
        else if (channelType == channelNoise)
            ++usage[instrument][usageNoise];
        else if (channelType == channelMmc5Pulse1 || channelType == channelMmc5Pulse2)
            ++usage[instrument][usageMmc5];
        else if (channelType == channelVrc6Pulse1 || channelType == channelVrc6Pulse2)
            ++usage[instrument][usageVrc6Pulse];
        else if (channelType == channelVrc6Saw)
            ++usage[instrument][usageVrc6Saw];
        else if (channelType >= channelS5b1 && channelType <= channelS5b3)
            ++usage[instrument][usageS5b];
        else if (channelType >= 29 && channelType <= 31)
            ++usage[instrument][usageEpsmSquare];
        else if (channelType >= 32 && channelType <= 37)
            ++usage[instrument][usageEpsmFm];
        else if (channelType >= 38 && channelType <= 43)
            ++usage[instrument][usageEpsmRhythm];
    }

    const auto mask = r.u16();

    if ((mask & effectVolume) != 0) r.skip(1);
    if ((mask & effectVibrato) != 0) r.skip(1);
    if ((mask & effectSpeed) != 0) r.skip(1);
    if ((mask & effectFinePitch) != 0) r.skip(1);
    if ((mask & effectFdsSpeed) != 0) r.skip(2);
    if ((mask & effectFdsDepth) != 0) r.skip(1);
    if ((mask & effectDuty) != 0) r.skip(1);
    if ((mask & effectDelay) != 0) r.skip(1);
    if ((mask & effectCut) != 0) r.skip(1);
    if (v >= 11 && (mask & effectVolume) != 0 && (mask & effectVolumeSlide) != 0) r.skip(1);
    if (v >= 13 && (mask & effectDmc) != 0) r.skip(1);
    if (v >= 15 && (mask & effectPhase) != 0) r.skip(1);
    if (v >= 16 && (mask & effectEnvPeriod) != 0) r.skip(2);

    r.i32(); // arpeggio
}

// Reads everything from the instruments onward. Throws if it doesn't fit the format exactly.
void readFromInstruments(Reader& r, int numChannels, const uint8_t* data, size_t size, size_t samplesSearchStart, FmsImportResult& out)
{
    const auto instrumentsStart = r.position();

    const auto numInstruments = r.count(1);
    std::vector<FileInstrument> instruments;

    for (size_t i = 0; i < numInstruments; ++i)
        instruments.push_back(readInstrument(r));

    const auto numArpeggios = r.count(1);

    for (size_t i = 0; i < numArpeggios; ++i)
    {
        r.i32();
        r.string();
        r.i32();
        if (r.version >= 16) r.string();
        readEnvelope(r, nullptr, envelopeArpeggio);
    }

    std::map<int, Usage> usage;
    std::map<int, int> dpcmUsage;

    const auto numSongs = r.count(1);

    // Every project has a song. Without this, a few zero bytes would look like an empty project.
    if (numSongs == 0)
        throw Malformed();

    for (size_t song = 0; song < numSongs; ++song)
    {
        r.i32(); // id
        r.i32(); // pattern length
        const auto songLength = r.i32();

        if (songLength < 0 || songLength > patternInstancesPerChannel)
            throw Malformed();

        r.i32(); // beat length
        r.string();
        r.skip(4 + 4 + 4 + 4 + 4); // tempo, speed, color, loop point, note length
        r.skipInt32Array();
        if (r.version >= 19) r.i32();

        for (int i = 0; i < songLength; ++i)
        {
            r.skip(1 + 4 + 4 + 4);
            r.skipInt32Array();
            r.i32();
        }

        if (r.version >= 16)
            r.string();

        for (int c = 0; c < numChannels; ++c)
        {
            r.i32(); // song
            const auto numPatterns = r.count(1);
            const auto channelType = r.i32();

            for (size_t p = 0; p < numPatterns; ++p)
            {
                r.i32();
                r.string();
                r.skip(4 + 4 + 4); // channel type, color, song

                const auto numNotes = r.count(1);

                for (size_t n = 0; n < numNotes; ++n)
                {
                    r.i16(); // time
                    readNote(r, channelType, usage, dpcmUsage);
                }
            }

            r.skip(patternInstancesPerChannel * 4);
        }
    }

    if (! r.atEnd())
        throw Malformed();

    // Everything after here fits the format, so these are really the instruments. Find the samples
    // the same way: the one offset where they parse and end exactly where the instruments begin.
    SampleMap samples;

    for (auto position = samplesSearchStart; r.version >= 15 && position + 4 <= instrumentsStart; ++position)
    {
        if (! looksLikeListOfNamedItems(data, size, position))
            continue;

        try
        {
            Reader sampleReader(data, size, r.version, position);
            std::map<int, std::string> names;
            const auto sources = readSampleSources(sampleReader, instrumentsStart, names);

            // Processing can be slow, so it waits until this is known to be the real list.
            for (const auto& [id, source] : sources)
            {
                auto sample = std::make_shared<DpcmSampleData>();
                sample->name = names[id];
                sample->initialValueDiv2 = source.initialValueDiv2;
                sample->data = processDpcmSample(source);
                samples[id] = std::move(sample);
            }

            break;
        }
        catch (const Malformed&)
        {
        }
    }

    FmsImportResult result;

    for (const auto& file : instruments)
    {
        const auto found = usage.find(file.id);
        const auto usedOnMelodicChannel = found != usage.end();

        auto hasEnvelopes = false;

        for (int t = 0; t < numEnvelopeTypes; ++t)
            hasEnvelopes = hasEnvelopes || ! file.instrument.envelopes[(size_t) t].isEmpty(t);

        // Projects from before version 15 keep samples in a form that isn't read here.
        const auto holdsSamples = ! file.mappings.empty() || dpcmUsage.count(file.id) > 0;

        if (r.version < 15 && holdsSamples && ! usedOnMelodicChannel && ! hasEnvelopes)
        {
            ++result.skippedDpcm;
            continue;
        }

        Preset preset;
        preset.name = file.name;
        preset.instrument = file.instrument;

        // With no duty envelope the chip stays at duty 0, as it does in FamiStudio.
        preset.voice.dutyCycle = 0;

        // Chip instruments have one home. 2A03 instruments can also play on MMC5, as in FamiStudio.
        const Usage counts = usedOnMelodicChannel ? found->second : Usage {};

        if (file.expansion == expansionVrc6)
        {
            preset.channel = counts[usageVrc6Saw] > counts[usageVrc6Pulse] ? ChannelType::vrc6Saw : ChannelType::vrc6Pulse1;
        }
        else if (file.expansion == expansionS5b)
        {
            preset.channel = ChannelType::s5bSquare1;
        }
        else if (file.expansion == expansionFds)
        {
            preset.channel = ChannelType::fdsWave;
        }
        else if (file.expansion == expansionN163)
        {
            preset.channel = ChannelType::n163Wave1;
        }
        else if (file.expansion == expansionVrc7)
        {
            preset.channel = ChannelType::vrc7Fm1;
        }
        else if (file.expansion == expansionEpsm)
        {
            preset.channel = counts[usageEpsmRhythm] > std::max(counts[usageEpsmFm], counts[usageEpsmSquare]) ? ChannelType::epsmRhythm1
                           : counts[usageEpsmSquare] > counts[usageEpsmFm] ? ChannelType::epsmSquare1 : ChannelType::epsmFm1;
        }
        else
        {
            const auto most = std::max({ counts[usageTriangle], counts[usageNoise], counts[usageMmc5] });

            if (most > counts[usagePulse])
                preset.channel = counts[usageTriangle] == most ? ChannelType::triangle
                               : counts[usageNoise] == most ? ChannelType::noise : ChannelType::mmc5Pulse1;
        }

        if (! file.mappings.empty())
        {
            auto kit = std::make_shared<DpcmKit>();

            for (const auto& mapping : file.mappings)
            {
                const auto sample = samples.find(mapping.sampleId);

                if (sample != samples.end() && mapping.note >= noteMusicalMin && mapping.note <= noteMusicalMax)
                {
                    auto& entry = kit->mappings[(size_t) mapping.note];
                    entry.sample = sample->second;
                    entry.pitch = std::clamp(mapping.pitch, 0, dpcmNumRates - 1);
                    entry.loop = mapping.loop;
                    entry.overrideInitialValue = mapping.overrideInitialValue;
                    entry.initialValueDiv2 = std::clamp(mapping.initialValueDiv2, 0, 63);
                }
            }

            if (! kit->isEmpty())
            {
                preset.kit = std::move(kit);

                if (! usedOnMelodicChannel)
                    preset.channel = ChannelType::dpcm;
            }
        }

        result.presets.push_back(std::move(preset));
        result.instrumentIds.push_back(file.id);
    }

    out = std::move(result);
}
} // namespace

bool importFmsProject(const uint8_t* data, size_t size, int version, FmsImportResult& out, std::string& error)
{
    if (version < oldestSupportedVersion)
    {
        error = "This project is from a very old FamiStudio. Open it in FamiStudio and save it again";
        return false;
    }

    int numChannels = 5;
    size_t start = 0;

    try
    {
        Reader header(data, size, version);

        header.i32(); // next unique id

        if (version >= 15)
            header.skip(4); // sorting flags

        header.string();
        header.string();
        header.string();

        auto expansionMask = header.i32();

        if (version < 12)
            expansionMask = expansionMask > 0 ? 1 << (expansionMask - 1) : 0;

        const auto numN163Channels = header.i32();

        if (numN163Channels < 0 || numN163Channels > 8)
            throw Malformed();

        // VRC6, VRC7, FDS, MMC5, N163, S5B, EPSM.
        static constexpr int channelsPerExpansion[] { 3, 6, 1, 2, 0, 3, 15 };

        for (int i = 0; i < 7; ++i)
            if ((expansionMask & (1 << i)) != 0)
                numChannels += i == 4 ? numN163Channels : channelsPerExpansion[i];

        start = header.position();
    }
    catch (const Malformed&)
    {
        error = "This project file is damaged";
        return false;
    }

    // Export settings and DPCM samples come before the instruments, and nothing there is needed.
    // Find the instruments by the one offset where everything after it fits the format exactly.
    for (auto position = start; position + 4 <= size; ++position)
    {
        if (! looksLikeListOfNamedItems(data, size, position))
            continue;

        try
        {
            Reader reader(data, size, version, position);
            readFromInstruments(reader, numChannels, data, size, start, out);
            return true;
        }
        catch (const Malformed&)
        {
        }
    }

    error = version > newestKnownVersion ? "This project is from a newer FamiStudio than Chipmunk can read"
                                         : "This project file is damaged";
    return false;
}
