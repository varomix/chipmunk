#include "engine/Preset.h"

#include "engine/EnvelopeEdit.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <sstream>

namespace
{
constexpr const char* magicLine = "Chipmunk preset 1";
constexpr const char* oldMagicLine = "VaroNES preset 1"; // From before the rename.

const char* envelopeKey(int type)
{
    switch (type)
    {
        case envelopeVolume: return "volume";
        case envelopeArpeggio: return "arpeggio";
        case envelopePitch: return "pitch";
        case envelopeS5bMixer: return "s5bMixer";
        case envelopeS5bNoise: return "s5bNoise";
        default: return "dutyEnvelope";
    }
}

const char* channelName(ChannelType type)
{
    switch (type)
    {
        case ChannelType::triangle: return "triangle";
        case ChannelType::noise: return "noise";
        case ChannelType::dpcm: return "dpcm";
        case ChannelType::drumKit: return "drumkit";
        case ChannelType::vrc6Pulse1: case ChannelType::vrc6Pulse2: return "vrc6pulse";
        case ChannelType::vrc6Saw: return "vrc6saw";
        case ChannelType::mmc5Pulse1: case ChannelType::mmc5Pulse2: return "mmc5pulse";
        case ChannelType::s5bSquare1: case ChannelType::s5bSquare2: case ChannelType::s5bSquare3: return "s5b";
        case ChannelType::fdsWave: return "fds";
        case ChannelType::pulse1: case ChannelType::pulse2: return "pulse";
        default:
            if ((int) type <= (int) ChannelType::n163Wave8) return "n163";
            if ((int) type <= (int) ChannelType::vrc7Fm6) return "vrc7";
            if ((int) type <= (int) ChannelType::epsmSquare3) return "epsmsquare";
            if ((int) type <= (int) ChannelType::epsmFm6) return "epsmfm";
            return "epsmrhythm";
    }
}

int readInt(const std::string& value, int min, int max, int fallback)
{
    char* end = nullptr;
    const auto parsed = std::strtol(value.c_str(), &end, 10);
    return end == value.c_str() ? fallback : std::clamp((int) parsed, min, max);
}

constexpr const char* base64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string toBase64(const std::vector<uint8_t>& data)
{
    std::string text;
    text.reserve((data.size() + 2) / 3 * 4);

    for (size_t i = 0; i < data.size(); i += 3)
    {
        const auto remaining = data.size() - i;
        const auto chunk = ((uint32_t) data[i] << 16)
                         | ((remaining > 1 ? (uint32_t) data[i + 1] : 0) << 8)
                         | (remaining > 2 ? (uint32_t) data[i + 2] : 0);

        text += base64Alphabet[(chunk >> 18) & 63];
        text += base64Alphabet[(chunk >> 12) & 63];
        text += remaining > 1 ? base64Alphabet[(chunk >> 6) & 63] : '=';
        text += remaining > 2 ? base64Alphabet[chunk & 63] : '=';
    }

    return text;
}

bool fromBase64(const std::string& text, std::vector<uint8_t>& out)
{
    uint32_t bits = 0;
    auto numBits = 0;

    for (const auto c : text)
    {
        if (c == '=')
            break;

        const auto* found = std::strchr(base64Alphabet, c);

        if (found == nullptr || c == 0)
            return false;

        bits = (bits << 6) | (uint32_t) (found - base64Alphabet);
        numBits += 6;

        if (numBits >= 8)
        {
            numBits -= 8;
            out.push_back((uint8_t) ((bits >> numBits) & 0xff));
        }
    }

    return true;
}

std::string trim(const std::string& text)
{
    const auto first = text.find_first_not_of(" \t\r");

    if (first == std::string::npos)
        return {};

    return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}
} // namespace

std::string formatPreset(const Preset& preset)
{
    std::ostringstream out;

    out << magicLine << '\n'
        << "name=" << preset.name << '\n'
        << "channel=" << channelName(preset.channel) << '\n'
        << "duty=" << preset.voice.dutyCycle << '\n'
        << "velocity=" << (preset.voice.velocitySensitive ? 1 : 0) << '\n'
        << "legato=" << (preset.voice.legato ? 1 : 0) << '\n'
        << "glide=" << preset.voice.glideFrames << '\n'
        << "bendRange=" << preset.voice.bendRangeSemitones << '\n'
        << "vibratoSpeed=" << preset.voice.vibratoSpeed << '\n';

    if (preset.voices > 1)
        out << "voices=" << preset.voices << '\n';

    for (int type = 0; type < numEnvelopeTypes; ++type)
        out << envelopeKey(type) << '=' << formatEnvelope(preset.instrument.envelopes[(size_t) type]) << '\n';

    out << "pitchRelative=" << (preset.instrument.envelopes[envelopePitch].relative ? 1 : 0) << '\n'
        << "sawVolume=" << preset.instrument.sawMasterVolume << '\n'
        << "s5bShape=" << preset.instrument.s5bEnvelopeShape << '\n'
        << "s5bAutoPitch=" << (preset.instrument.s5bAutoPitch ? 1 : 0) << '\n'
        << "s5bOctave=" << preset.instrument.s5bAutoPitchOctave << '\n'
        << "s5bPeriod=" << preset.instrument.s5bEnvelopePeriod << '\n';

    // Only the chips that use these get them, which keeps other sounds' files short.
    const auto& instrument = preset.instrument;
    const auto writeList = [&out](const char* key, const auto& values, int count) {
        out << key << '=';
        for (int i = 0; i < count; ++i)
            out << (int) values[(size_t) i] << (i + 1 < count ? " " : "");
        out << '\n';
    };

    if (instrument.waveLength > 0)
    {
        out << "waveSize=" << instrument.waveSize << '\n';
        writeList("wave", instrument.wave, instrument.waveLength);
        out << "waveRepeatPoints=" << instrument.waveRepeat.loop << ' ' << instrument.waveRepeat.release << '\n';
        writeList("waveRepeat", instrument.waveRepeat.values, instrument.waveRepeat.length);
    }

    if (preset.channel == ChannelType::fdsWave)
    {
        writeList("fdsModulation", instrument.fdsModulation, (int) instrument.fdsModulation.size());
        out << "fds=" << instrument.fdsMasterVolume << ' ' << instrument.fdsModSpeed << ' ' << instrument.fdsModDepth << ' ' << instrument.fdsModDelay << ' '
            << (instrument.fdsAutoMod ? 1 : 0) << ' ' << instrument.fdsAutoModNumerator << ' ' << instrument.fdsAutoModDenominator << '\n';
    }

    if ((int) preset.channel >= (int) ChannelType::n163Wave1 && (int) preset.channel <= (int) ChannelType::n163Wave8)
        out << "n163Position=" << instrument.n163WavePosition << '\n';

    if ((int) preset.channel >= (int) ChannelType::vrc7Fm1 && (int) preset.channel <= (int) ChannelType::vrc7Fm6)
    {
        out << "vrc7=" << instrument.vrc7Patch << ' ' << (instrument.vrc7OverrideRelease ? 1 : 0) << ' ' << (instrument.vrc7OverrideStop ? 1 : 0) << '\n';
        writeList("vrc7Registers", instrument.vrc7PatchRegisters, (int) instrument.vrc7PatchRegisters.size());
    }

    if ((int) preset.channel >= (int) ChannelType::epsmFm1 && (int) preset.channel <= (int) ChannelType::epsmRhythm6)
        writeList("epsmRegisters", instrument.epsmPatchRegisters, (int) instrument.epsmPatchRegisters.size());

    if (preset.kit != nullptr)
    {
        // Each sample is written once, and keys refer to it by position. Keys can share samples.
        std::vector<const DpcmSampleData*> written;

        for (const auto& mapping : preset.kit->mappings)
        {
            if (mapping.sample != nullptr && std::find(written.begin(), written.end(), mapping.sample.get()) == written.end())
            {
                written.push_back(mapping.sample.get());
                out << "sample=" << mapping.sample->initialValueDiv2 << ' ' << toBase64(mapping.sample->data) << ' ' << mapping.sample->name << '\n';
            }
        }

        for (int value = noteMusicalMin; value <= noteMusicalMax; ++value)
        {
            const auto& mapping = preset.kit->mappings[(size_t) value];

            if (mapping.sample != nullptr)
            {
                const auto index = std::find(written.begin(), written.end(), mapping.sample.get()) - written.begin();
                out << "key=" << value << ' ' << index << ' ' << mapping.pitch << ' ' << (mapping.loop ? 1 : 0) << ' '
                    << (mapping.overrideInitialValue ? 1 : 0) << ' ' << mapping.initialValueDiv2 << '\n';
            }
        }

        // Noise sounds work the same way, each followed by the envelopes noise can use.
        std::vector<const KitSound*> sounds;

        for (int value = noteMusicalMin; value <= noteMusicalMax; ++value)
        {
            const auto& mapping = preset.kit->mappings[(size_t) value];

            if (mapping.noise == nullptr)
                continue;

            auto found = std::find(sounds.begin(), sounds.end(), mapping.noise.get());

            if (found == sounds.end())
            {
                sounds.push_back(mapping.noise.get());
                found = sounds.end() - 1;
                out << "sound=" << mapping.noise->name << '\n';

                for (const auto type : { envelopeVolume, envelopeArpeggio, envelopeDutyCycle })
                    out << "soundEnvelope=" << envelopeKey(type) << ' ' << formatEnvelope(mapping.noise->instrument.envelopes[(size_t) type]) << '\n';
            }

            out << "pad=" << value << ' ' << (found - sounds.begin()) << ' ' << mapping.noiseNote << '\n';
        }
    }

    return out.str();
}

bool parsePreset(const std::string& text, Preset& out, std::string& error)
{
    std::istringstream in(text);
    std::string line;

    if (! std::getline(in, line) || (trim(line) != magicLine && trim(line) != oldMagicLine))
    {
        error = "This isn't a Chipmunk preset";
        return false;
    }

    Preset preset;
    std::vector<std::shared_ptr<const DpcmSampleData>> samples;
    std::shared_ptr<DpcmKit> kit;
    std::vector<std::shared_ptr<KitSound>> sounds;

    while (std::getline(in, line))
    {
        const auto equals = line.find('=');

        if (equals == std::string::npos)
            continue;

        const auto key = trim(line.substr(0, equals));
        const auto value = trim(line.substr(equals + 1));

        if (key == "name")
            preset.name = value;
        else if (key == "channel")
            preset.channel = value == "triangle" ? ChannelType::triangle : value == "noise" ? ChannelType::noise
                           : value == "dpcm" ? ChannelType::dpcm : value == "vrc6pulse" ? ChannelType::vrc6Pulse1
                           : value == "vrc6saw" ? ChannelType::vrc6Saw : value == "mmc5pulse" ? ChannelType::mmc5Pulse1
                           : value == "s5b" ? ChannelType::s5bSquare1 : value == "fds" ? ChannelType::fdsWave
                           : value == "n163" ? ChannelType::n163Wave1 : value == "vrc7" ? ChannelType::vrc7Fm1
                           : value == "epsmsquare" ? ChannelType::epsmSquare1 : value == "epsmfm" ? ChannelType::epsmFm1
                           : value == "epsmrhythm" ? ChannelType::epsmRhythm1 : value == "drumkit" ? ChannelType::drumKit : ChannelType::pulse1;
        else if (key == "duty")
            preset.voice.dutyCycle = readInt(value, 0, envelopeDutyMaxVrc6, preset.voice.dutyCycle);
        else if (key == "velocity")
            preset.voice.velocitySensitive = readInt(value, 0, 1, 1) != 0;
        else if (key == "legato")
            preset.voice.legato = readInt(value, 0, 1, 1) != 0;
        else if (key == "glide")
            preset.voice.glideFrames = readInt(value, 0, 64, 0);
        else if (key == "voices")
            preset.voices = readInt(value, 1, 8, 1);
        else if (key == "bendRange")
            preset.voice.bendRangeSemitones = readInt(value, 0, 24, 2);
        else if (key == "vibratoSpeed")
            preset.voice.vibratoSpeed = readInt(value, 1, 12, 6);
        else if (key == "sawVolume")
            preset.instrument.sawMasterVolume = readInt(value, 0, 2, 1);
        else if (key == "s5bShape")
            preset.instrument.s5bEnvelopeShape = readInt(value, 0, 8, 0);
        else if (key == "s5bAutoPitch")
            preset.instrument.s5bAutoPitch = readInt(value, 0, 1, 1) != 0;
        else if (key == "s5bOctave")
            preset.instrument.s5bAutoPitchOctave = readInt(value, -8, 8, 3);
        else if (key == "s5bPeriod")
            preset.instrument.s5bEnvelopePeriod = readInt(value, 0, 65535, 1000);
        else if (key == "waveSize")
            preset.instrument.waveSize = readInt(value, 4, 64, 16);
        else if (key == "wave")
        {
            auto& instrument = preset.instrument;
            std::istringstream fields(value);
            int number = 0;
            instrument.waveLength = 0;

            while (instrument.waveLength < Instrument::maxWaveSteps && fields >> number)
                instrument.wave[(size_t) instrument.waveLength++] = (int8_t) std::clamp(number, 0, 63);
        }
        else if (key == "waveRepeatPoints")
        {
            std::istringstream fields(value);
            fields >> preset.instrument.waveRepeat.loop >> preset.instrument.waveRepeat.release;
        }
        else if (key == "waveRepeat")
        {
            auto& repeat = preset.instrument.waveRepeat;
            std::istringstream fields(value);
            int number = 0;
            repeat.length = 0;

            while (repeat.length < Envelope::maxLength && fields >> number)
                repeat.values[(size_t) repeat.length++] = (int8_t) std::clamp(number, 1, 127);
        }
        else if (key == "fdsModulation")
        {
            std::istringstream fields(value);
            int number = 0;

            for (size_t i = 0; i < preset.instrument.fdsModulation.size() && fields >> number; ++i)
                preset.instrument.fdsModulation[i] = (int8_t) std::clamp(number, -64, 63);
        }
        else if (key == "fds")
        {
            auto& instrument = preset.instrument;
            std::istringstream fields(value);
            int autoMod = 0;
            fields >> instrument.fdsMasterVolume >> instrument.fdsModSpeed >> instrument.fdsModDepth >> instrument.fdsModDelay
                   >> autoMod >> instrument.fdsAutoModNumerator >> instrument.fdsAutoModDenominator;
            instrument.fdsAutoMod = autoMod != 0;
            instrument.fdsMasterVolume = std::clamp(instrument.fdsMasterVolume, 0, 3);
            instrument.fdsModSpeed = std::clamp(instrument.fdsModSpeed, 0, 4095);
            instrument.fdsModDepth = std::clamp(instrument.fdsModDepth, 0, 63);
            instrument.fdsModDelay = std::clamp(instrument.fdsModDelay, 0, 255);
            instrument.fdsAutoModNumerator = std::clamp(instrument.fdsAutoModNumerator, 1, 32);
            instrument.fdsAutoModDenominator = std::clamp(instrument.fdsAutoModDenominator, 1, 32);
        }
        else if (key == "n163Position")
            preset.instrument.n163WavePosition = readInt(value, 0, 254, 0);
        else if (key == "vrc7")
        {
            auto& instrument = preset.instrument;
            std::istringstream fields(value);
            int overrideRelease = 0, overrideStop = 0;
            fields >> instrument.vrc7Patch >> overrideRelease >> overrideStop;
            instrument.vrc7Patch = std::clamp(instrument.vrc7Patch, 0, 15);
            instrument.vrc7OverrideRelease = overrideRelease != 0;
            instrument.vrc7OverrideStop = overrideStop != 0;
        }
        else if (key == "vrc7Registers" || key == "epsmRegisters")
        {
            std::istringstream fields(value);
            int number = 0;

            if (key == "vrc7Registers")
            {
                for (auto& reg : preset.instrument.vrc7PatchRegisters)
                    if (fields >> number)
                        reg = (uint8_t) std::clamp(number, 0, 255);
            }
            else
            {
                for (auto& reg : preset.instrument.epsmPatchRegisters)
                    if (fields >> number)
                        reg = (uint8_t) std::clamp(number, 0, 255);
            }
        }
        else if (key == "sample")
        {
            std::istringstream fields(value);
            int initial = dpcmDefaultInitialValueDiv2;
            std::string encoded, name;

            auto sample = std::make_shared<DpcmSampleData>();

            if (! (fields >> initial >> encoded) || ! fromBase64(encoded, sample->data) || (int) sample->data.size() > dpcmMaxSampleBytes)
            {
                error = "A DPCM sample is damaged";
                return false;
            }

            std::getline(fields, name);
            sample->name = trim(name);
            sample->initialValueDiv2 = std::clamp(initial, 0, 63);
            samples.push_back(std::move(sample));
        }
        else if (key == "key")
        {
            std::istringstream fields(value);
            int note = 0, index = 0, pitch = 15, loop = 0, override = 0, initial = dpcmDefaultInitialValueDiv2;

            if (! (fields >> note >> index >> pitch >> loop >> override >> initial)
                || note < noteMusicalMin || note > noteMusicalMax || index < 0 || index >= (int) samples.size())
            {
                error = "A DPCM key mapping is damaged";
                return false;
            }

            if (kit == nullptr)
                kit = std::make_shared<DpcmKit>();

            auto& mapping = kit->mappings[(size_t) note];
            mapping.sample = samples[(size_t) index];
            mapping.pitch = std::clamp(pitch, 0, dpcmNumRates - 1);
            mapping.loop = loop != 0;
            mapping.overrideInitialValue = override != 0;
            mapping.initialValueDiv2 = std::clamp(initial, 0, 63);
        }
        else if (key == "sound")
        {
            sounds.push_back(std::make_shared<KitSound>());
            sounds.back()->name = value;
        }
        else if (key == "soundEnvelope")
        {
            const auto space = value.find(' ');
            const auto type = value.substr(0, space);
            const auto text = space == std::string::npos ? std::string {} : value.substr(space + 1);

            for (const auto candidate : { envelopeVolume, envelopeArpeggio, envelopeDutyCycle })
            {
                std::string envelopeError;

                if (! sounds.empty() && type == envelopeKey(candidate)
                    && ! parseEnvelope(text, candidate, sounds.back()->instrument.envelopes[(size_t) candidate], envelopeError, envelopeDutyMaxVrc6))
                {
                    error = "A kit sound's " + type + ": " + envelopeError;
                    return false;
                }
            }
        }
        else if (key == "pad")
        {
            std::istringstream fields(value);
            int note = 0, index = 0, noiseNote = 48;

            if (! (fields >> note >> index >> noiseNote) || note < noteMusicalMin || note > noteMusicalMax || index < 0 || index >= (int) sounds.size())
            {
                error = "A kit key is damaged";
                return false;
            }

            if (kit == nullptr)
                kit = std::make_shared<DpcmKit>();

            auto& mapping = kit->mappings[(size_t) note];
            mapping.noise = sounds[(size_t) index];
            mapping.noiseNote = std::clamp(noiseNote, noteMusicalMin, noteMusicalMax);
        }
        else if (key == "pitchRelative")
            preset.instrument.envelopes[envelopePitch].relative = readInt(value, 0, 1, 0) != 0;
        else
        {
            for (int type = 0; type < numEnvelopeTypes; ++type)
            {
                if (key == envelopeKey(type))
                {
                    std::string envelopeError;

                    if (! parseEnvelope(value, type, preset.instrument.envelopes[(size_t) type], envelopeError, envelopeDutyMaxVrc6))
                    {
                        error = key + ": " + envelopeError;
                        return false;
                    }
                }
            }
        }
    }

    preset.kit = std::move(kit);
    out = preset;
    return true;
}

bool importFti(const uint8_t* data, size_t size, Preset& out, std::string& error)
{
    size_t offset = 0;

    const auto available = [&](size_t count) { return count <= size && offset <= size - count; };
    const auto readInt32 = [&](int32_t& value) {
        if (! available(4))
            return false;
        std::memcpy(&value, data + offset, 4); // Files are little-endian, as are the Macs this runs on.
        offset += 4;
        return true;
    };

    if (size < 7 || std::memcmp(data, "FTI", 3) != 0)
    {
        error = "This isn't a FamiTracker instrument file";
        return false;
    }

    if (std::memcmp(data + 3, "2.4", 3) != 0)
    {
        error = "Only version 2.4 .fti files are supported";
        return false;
    }

    if (data[6] != 1)
    {
        error = "Only 2A03 instruments can be imported for now";
        return false;
    }

    const auto truncated = [&] {
        error = "This .fti file is incomplete";
        return false;
    };

    offset = 7;

    int32_t nameLength = 0;

    if (! readInt32(nameLength) || nameLength < 0 || ! available((size_t) nameLength))
        return truncated();

    Preset preset;

    // With no duty envelope the chip stays at duty 0, as it does in FamiTracker.
    preset.voice.dutyCycle = 0;

    preset.name.assign(reinterpret_cast<const char*>(data + offset), (size_t) nameLength);
    offset += (size_t) nameLength;

    if (! available(1))
        return truncated();

    // FamiTracker's sequence order: volume, arpeggio, pitch, hi-pitch (unsupported), duty.
    static constexpr int sequenceTypes[] { envelopeVolume, envelopeArpeggio, envelopePitch, -1, envelopeDutyCycle };

    const auto sequenceCount = (int) data[offset++];

    for (int i = 0; i < sequenceCount; ++i)
    {
        if (! available(1))
            return truncated();

        if (data[offset++] != 1)
            continue;

        int32_t count = 0, loop = 0, release = 0, setting = 0;

        if (! readInt32(count) || ! readInt32(loop) || ! readInt32(release) || ! readInt32(setting)
            || count < 0 || ! available((size_t) count))
            return truncated();

        const auto type = i < 5 ? sequenceTypes[i] : -1;

        if (type >= 0)
        {
            auto& env = preset.instrument.envelopes[(size_t) type];

            env.length = std::min((int) count, Envelope::maxLength);

            for (int j = 0; j < env.length; ++j)
                env.values[(size_t) j] = (int8_t) data[offset + (size_t) j];

            // FamiTracker's release point is the last held step and allows no loop. Here it's
            // the first released step, and a release always loops on what comes before it.
            if (envelopeCanRelease(type) && release >= 0)
            {
                if (loop < 0)
                    loop = release;
                ++release;
            }
            else
            {
                release = -1;
            }

            env.loop = loop;
            env.release = release;

            if (type == envelopePitch)
                env.relative = true;

            sanitizeEnvelope(env, type, 3);
        }

        offset += (size_t) count;
    }

    // DPCM samples are optional here, so older or trimmed files still give their envelopes.
    int32_t numAssigned = 0;

    if (readInt32(numAssigned) && numAssigned >= 0 && numAssigned <= 96 && available((size_t) numAssigned * 4))
    {
        const auto assignments = offset;
        offset += (size_t) numAssigned * 4;

        std::array<std::shared_ptr<const DpcmSampleData>, 64> samples;
        int32_t numSamples = 0;

        if (readInt32(numSamples) && numSamples >= 0 && numSamples <= 64)
        {
            auto complete = true;

            for (int i = 0; i < numSamples && complete; ++i)
            {
                int32_t index = 0, sampleNameLength = 0, sampleSize = 0;

                complete = readInt32(index) && index >= 0 && index < 64
                        && readInt32(sampleNameLength) && sampleNameLength >= 0 && available((size_t) sampleNameLength);

                if (! complete)
                    break;

                auto sample = std::make_shared<DpcmSampleData>();
                sample->name.assign(reinterpret_cast<const char*>(data + offset), (size_t) sampleNameLength);
                offset += (size_t) sampleNameLength;

                complete = readInt32(sampleSize) && sampleSize >= 0 && available((size_t) sampleSize);

                if (! complete)
                    break;

                sample->data.assign(data + offset, data + offset + std::min(sampleSize, dpcmMaxSampleBytes));
                offset += (size_t) sampleSize;
                samples[(size_t) index] = std::move(sample);
            }

            auto kit = std::make_shared<DpcmKit>();

            for (int i = 0; i < numAssigned; ++i)
            {
                const auto* entry = data + assignments + (size_t) i * 4;
                const auto value = (int) entry[0] + 1;
                const auto sampleIndex = (int) entry[1] - 1;

                if (value >= noteMusicalMin && value <= noteMusicalMax && sampleIndex >= 0 && sampleIndex < 64 && samples[(size_t) sampleIndex] != nullptr)
                {
                    auto& mapping = kit->mappings[(size_t) value];
                    mapping.sample = samples[(size_t) sampleIndex];
                    mapping.pitch = entry[2] & 0x0f;
                    mapping.loop = (entry[2] & 0x80) != 0;
                }
            }

            if (! kit->isEmpty())
            {
                preset.kit = std::move(kit);

                auto hasEnvelopes = false;

                for (int t = 0; t < numEnvelopeTypes; ++t)
                    hasEnvelopes = hasEnvelopes || ! preset.instrument.envelopes[(size_t) t].isEmpty(t);

                if (! hasEnvelopes)
                    preset.channel = ChannelType::dpcm;
            }
        }
    }

    out = preset;
    return true;
}

namespace
{
// Factory sounds name a wave shape instead of listing its steps.
std::string expandWaves(std::string text, const std::string& channel)
{
    const auto marker = text.find("WAVE:");

    if (marker == std::string::npos)
        return text;

    const auto end = text.find('\n', marker);
    const auto shape = text.substr(marker + 5, end - marker - 5);
    const auto sizeAt = text.find("waveSize=");
    const auto size = std::stoi(text.substr(sizeAt + 9));
    const auto maxValue = channel == "fds" ? 63 : 15;

    std::string steps;

    for (int i = 0; i < size; ++i)
    {
        const auto phase = (double) i / size;
        const auto value = shape == "saw" ? phase * maxValue
                         : shape == "square" ? (phase < 0.5 ? (double) maxValue : 0.0)
                                             : (std::sin(phase * 2.0 * 3.14159265358979) * 0.5 + 0.5) * maxValue;

        steps += std::to_string((int) std::lround(value)) + " ";
    }

    return text.replace(marker, end - marker, steps);
}

// A kit needs samples and files can't be assumed, so this one is synthesized. Each drum is a
// falling sine mixed with noise, from a fixed seed so every instance gets identical data.
Preset makeSynthDrumKit()
{
    struct Drum
    {
        const char* name;
        int midiNote;
        double seconds, startHz, endHz, bodyDecay, noiseLevel, noiseDecay;
    };

    // General MIDI drum keys, so drum clips land on the expected sounds.
    static constexpr Drum drums[] {
        { "Kick", 36, 0.22, 160.0, 48.0, 14.0, 0.15, 90.0 },
        { "Snare", 38, 0.20, 240.0, 170.0, 28.0, 0.85, 18.0 },
        { "Closed Hat", 42, 0.05, 0.0, 0.0, 1.0, 1.0, 70.0 },
        { "Open Hat", 46, 0.22, 0.0, 0.0, 1.0, 1.0, 14.0 },
        { "Low Tom", 45, 0.25, 170.0, 85.0, 13.0, 0.08, 60.0 },
        { "High Tom", 50, 0.20, 260.0, 140.0, 15.0, 0.08, 60.0 },
    };

    static constexpr int waveRate = 44100;

    auto kit = std::make_shared<DpcmKit>();
    uint32_t noise = 0x2a03;

    for (const auto& drum : drums)
    {
        std::vector<int16_t> wave((size_t) (drum.seconds * waveRate));
        auto phase = 0.0;

        for (size_t i = 0; i < wave.size(); ++i)
        {
            const auto time = (double) i / waveRate;

            phase += 2.0 * 3.14159265358979 * (drum.endHz + (drum.startHz - drum.endHz) * std::exp(-time * 40.0)) / waveRate;

            // A simple shift register gives repeatable noise on every platform.
            noise ^= noise << 13;
            noise ^= noise >> 17;
            noise ^= noise << 5;

            const auto hiss = ((double) (noise & 0xffff) / 32767.5 - 1.0) * drum.noiseLevel * std::exp(-time * drum.noiseDecay);
            const auto body = (drum.startHz > 0.0 ? std::sin(phase) * std::exp(-time * drum.bodyDecay) : 0.0);

            wave[i] = (int16_t) (std::clamp(body + hiss, -1.0, 1.0) * 30000.0);
        }

        const auto converted = convertWaveToDpcm(drum.name, std::move(wave), waveRate);
        auto& mapping = kit->mappings[(size_t) (drum.midiNote - 23)];

        mapping.sample = converted.sample;
        mapping.pitch = converted.rate;
    }

    Preset preset;
    preset.name = "Synth Drum Kit";
    preset.channel = ChannelType::dpcm;
    preset.kit = std::move(kit);
    return preset;
}

// Samples for the deep drums and noise for the bright ones, which is the usual split on a NES.
Preset makeDrumKit()
{
    const auto samples = makeSynthDrumKit();
    auto kit = std::make_shared<DpcmKit>();

    const auto copySample = [&](int midiNote) {
        kit->mappings[(size_t) midiNoteToNoteValue(midiNote)] = samples.kit->mappings[(size_t) midiNoteToNoteValue(midiNote)];
    };

    for (const auto midiNote : { 36, 45, 50 })
        copySample(midiNote);

    const auto addNoise = [&](const char* name, int midiNote, int noisePitch, const char* volume) {
        auto sound = std::make_shared<KitSound>();
        std::string error;
        sound->name = name;
        parseEnvelope(volume, envelopeVolume, sound->instrument.envelopes[envelopeVolume], error, envelopeDutyMaxVrc6);

        auto& mapping = kit->mappings[(size_t) midiNoteToNoteValue(midiNote)];
        mapping.noise = std::move(sound);
        mapping.noiseNote = 48 + noisePitch;
    };

    addNoise("Snare", 38, 8, "15 13 11 9 7 5 4 3 2 1 0");
    addNoise("Closed Hat", 42, 14, "12 8 4 2 0");
    addNoise("Open Hat", 46, 14, "12 11 10 9 8 7 6 5 4 3 2 1 0");
    addNoise("Crash", 49, 12, "15 14 13 12 11 10 9 9 8 8 7 7 6 6 5 5 4 4 3 3 2 2 1 1 0");

    Preset preset;
    preset.name = "NES Drum Kit";
    preset.channel = ChannelType::drumKit;
    preset.kit = std::move(kit);
    return preset;
}
} // namespace

const std::vector<Preset>& factoryPresets()
{
    static const std::vector<Preset> presets = [] {
        static constexpr const char* definitions[][4] = {
            // name, channel, duty, envelope lines
            { "Init", "pulse", "2", "" },
            { "Pluck Lead", "pulse", "1", "volume=15 13 11 9 8 7 | 6\n" },
            { "Soft Lead", "pulse", "2", "volume=5 9 12 14 15 14 | 13 / 10 7 4 2 0\n" },
            { "Vibrato Lead", "pulse", "2", "volume=15 14 13 | 12\npitch=0 0 0 0 0 0 0 0 | 1 2 2 1 -1 -2 -2 -1\n" },
            { "Major Arp", "pulse", "1", "volume=15 14 13 | 12\narpeggio=| 0 0 4 4 7 7\n" },
            { "Minor Arp", "pulse", "1", "volume=15 14 13 | 12\narpeggio=| 0 0 3 3 7 7\n" },
            { "Octave Stab", "pulse", "0", "volume=15 13 11 9 7 5 3 2 1 0\narpeggio=12 12 0\n" },
            { "PWM Sweep", "pulse", "2", "volume=15 14 | 13\ndutyEnvelope=| 0 0 0 1 1 1 2 2 2 3 3 3 2 2 2 1 1 1\n" },
            { "Echo Blip", "pulse", "3", "volume=15 12 9 6 3 0 0 0 8 6 4 2 0 0 0 4 3 2 1 0\n" },
            { "Laser", "pulse", "0", "volume=15 15 14 13 12 11 10 9 8 7 6 5 4 3 2 1 0\npitch=8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8\npitchRelative=1\n" },
            { "Triangle Bass", "triangle", "2", "" },
            { "Triangle Pluck", "triangle", "2", "volume=15 15 15 15 15 15 15 15 0\n" },
            { "Triangle Kick", "triangle", "2", "volume=15 15 15 15 15 15 0\narpeggio=36 24 16 10 6 3 0\n" },
            { "Closed Hat", "noise", "2", "volume=12 8 4 2 0\n" },
            { "Open Hat", "noise", "2", "volume=12 11 10 9 8 7 6 5 4 3 2 1 0\n" },
            { "Snare", "noise", "2", "volume=15 13 11 9 7 6 5 4 3 2 1 0\narpeggio=4 2 0\n" },
            { "Metal Hit", "noise", "2", "volume=15 12 9 7 5 4 3 2 1 0\ndutyEnvelope=1\n" },
            { "Empty DPCM Kit", "dpcm", "2", "" },
            { "VRC6 Thin Lead", "vrc6pulse", "1", "volume=15 14 13 12 | 11\n" },
            { "VRC6 Sweep", "vrc6pulse", "7", "volume=15 15 14 | 13\ndutyEnvelope=| 0 1 2 3 4 5 6 7 6 5 4 3 2 1\n" },
            { "Saw Bass", "vrc6saw", "2", "volume=15 15 14 14 13 | 12\nsawVolume=0\n" },
            { "Saw Pluck", "vrc6saw", "2", "volume=15 13 11 9 8 7 6 5 4 3 2 1 0\nsawVolume=1\n" },
            { "MMC5 Lead", "mmc5pulse", "2", "volume=15 14 13 | 12 / 9 6 3 1 0\n" },
            { "MMC5 Echo Blip", "mmc5pulse", "1", "volume=15 11 7 3 0 0 0 7 5 3 1 0\n" },
            { "S5B Square", "s5b", "2", "volume=15 15 14 | 13\n" },
            { "S5B Buzz", "s5b", "2", "volume=15 15 14 | 13\ns5bShape=1\ns5bOctave=3\n" },
            { "S5B Static Hit", "s5b", "2", "volume=15 13 11 9 7 5 3 1 0\ns5bMixer=1\ns5bNoise=12 10 8 6 4 2 1 1 1\n" },
            { "FDS Sine Lead", "fds", "2", "volume=15 14 13 | 12 / 9 6 3 1 0\nwaveSize=64\nwave=WAVE:sine\n" },
            { "FDS Warble", "fds", "2", "volume=15 14 | 13 / 8 4 2 0\nwaveSize=64\nwave=WAVE:sine\nfds=0 40 20 10 0 1 1\n" },
            { "FDS Saw Pad", "fds", "2", "volume=6 9 12 14 | 14 / 9 6 3 0\nwaveSize=64\nwave=WAVE:saw\n" },
            { "FDS Square Bass", "fds", "2", "volume=15 14 | 13 / 6 2 0\nwaveSize=64\nwave=WAVE:square\n" },
            { "N163 Sine", "n163", "2", "volume=15 14 13 | 12 / 9 6 3 1 0\nwaveSize=16\nwave=WAVE:sine\n" },
            { "N163 Saw", "n163", "2", "volume=15 14 | 13 / 9 5 2 0\nwaveSize=32\nwave=WAVE:saw\n" },
            { "N163 Square", "n163", "2", "volume=15 14 | 13 / 8 4 2 0\nwaveSize=16\nwave=WAVE:square\n" },
            { "N163 Soft Bass", "n163", "2", "volume=15 15 14 | 13 / 6 2 0\nwaveSize=32\nwave=WAVE:sine\n" },
            { "VRC7 Bell", "vrc7", "2", "vrc7=1 1 0\n" },
            { "VRC7 Guitar", "vrc7", "2", "vrc7=2 1 0\n" },
            { "VRC7 Flute", "vrc7", "2", "vrc7=4 1 0\n" },
            { "VRC7 Brass", "vrc7", "2", "vrc7=12 1 0\n" },
            { "VRC7 Bass", "vrc7", "2", "vrc7=13 1 0\n" },
            { "EPSM Square", "epsmsquare", "2", "volume=15 14 13 | 12 / 9 6 3 1 0\n" },
            { "EPSM Buzz", "epsmsquare", "2", "volume=15 15 14 | 13\ns5bShape=1\ns5bOctave=3\n" },
            { "EPSM Hat", "epsmsquare", "2", "volume=11 8 5 3 1 0\ns5bMixer=1\ns5bNoise=1\n" },
            { "EPSM Bell Arp", "epsmsquare", "2", "volume=15 12 10 8 7 6 5 4 3 2 1 0\narpeggio=12 12 0\n" },
            { "EPSM FM Default", "epsmfm", "2", "" },
            { "EPSM FM Bright Lead", "epsmfm", "2", "epsmRegisters=59 192 6 15 31 20 9 104 0 51 32 31 8 6 104 0 1 18 31 20 9 104 0 1 0 31 20 9 104 0 0\n" },
            { "EPSM FM Bass", "epsmfm", "2", "epsmRegisters=56 192 116 20 31 8 4 136 0 66 28 31 10 4 136 0 1 12 31 8 4 136 0 1 0 31 8 4 136 0 0\n" },
            { "EPSM FM Organ", "epsmfm", "2", "epsmRegisters=7 192 1 0 31 0 0 240 0 2 0 31 0 0 240 0 4 0 31 0 0 240 0 8 0 31 0 0 240 0 0\n" },
            { "EPSM Rhythm Kit", "epsmrhythm", "2", "" },
            { "Explosion", "noise", "2", "volume=15 15 15 14 14 13 13 12 12 11 11 10 10 9 9 8 8 7 7 6 6 5 5 4 4 3 3 2 2 1 1 0\narpeggio=0 0 0 -1 -1 -2 -2 -3 -3 -4\n" },

            { "Hero Lead", "pulse", "2", "volume=15 15 14 13 | 12 / 9 6 3 1 0\npitch=0 0 0 0 0 0 0 0 0 0 | 1 2 2 1 -1 -2 -2 -1\n" },
            { "Square Bass", "pulse", "2", "volume=15 15 14 13 12 | 11 / 6 2 0\n" },
            { "Slap Bass", "pulse", "1", "volume=15 15 13 11 10 9 | 8 / 3 0\narpeggio=12 12 0\n" },
            { "Bell", "pulse", "0", "volume=15 12 10 8 7 6 5 4 4 3 3 2 2 1 1 0\narpeggio=12 12 0\n" },
            { "Dominant Arp", "pulse", "1", "volume=15 14 13 | 12\narpeggio=| 0 0 4 4 7 7 10 10\n" },
            { "Power Arp", "pulse", "2", "volume=15 14 13 | 12\narpeggio=| 0 0 7 7 12 12\n" },
            { "Fast Octave Trill", "pulse", "1", "volume=15 14 13 | 12\narpeggio=| 0 12\n" },
            { "Coin", "pulse", "1", "volume=15 14 13 12 11 10 9 8 7 6 5 4 3 2 1 0\narpeggio=0 0 0 0 5 5 5 5 5 5 5 5 5 5 5 5\n" },
            { "Jump", "pulse", "1", "volume=15 15 14 13 12 11 10 9 8 7 6 5 4 3 2 1 0\npitch=-6 -6 -6 -6 -6 -6 -6 -6 -6 -6 -6 -6 -6 -6 -6 -6\npitchRelative=1\n" },
            { "Triangle Lead", "triangle", "2", "volume=15 | 14\npitch=0 0 0 0 0 0 0 0 0 0 | 2 3 3 2 -2 -3 -3 -2\n" },
            { "Triangle Tom", "triangle", "2", "volume=15 15 15 15 15 15 15 15 15 15 0\narpeggio=12 10 8 6 5 4 3 2 1 0\n" },
            { "Triangle Snare Body", "triangle", "2", "volume=15 15 15 15 0\narpeggio=24 16 8 0\n" },
            { "Noise Kick", "noise", "2", "volume=15 13 10 7 4 2 0\narpeggio=8 4 0 -4 -6 -8 -8\n" },
            { "Shaker", "noise", "2", "volume=4 8 10 7 4 2 1 0\n" },
            { "Crash", "noise", "2", "volume=15 14 14 13 13 12 12 11 11 10 10 9 9 8 8 7 7 6 6 5 5 4 4 3 3 2 2 1 1 0\n" },
            { "Wind", "noise", "2", "volume=2 4 6 8 | 8 7 6 7 / 5 3 1 0\narpeggio=| 0 0 1 1 2 2 1 1\n" },
            { "VRC6 Soft Lead", "vrc6pulse", "5", "volume=6 10 13 15 | 14 / 10 7 4 2 0\npitch=0 0 0 0 0 0 0 0 0 0 | 1 2 2 1 -1 -2 -2 -1\n" },
            { "VRC6 Bass", "vrc6pulse", "7", "volume=15 15 14 13 | 12 / 6 2 0\n" },
            { "VRC6 Bell", "vrc6pulse", "1", "volume=15 12 10 8 7 6 5 4 3 2 1 0\narpeggio=12 12 0\n" },
            { "VRC6 Arp", "vrc6pulse", "3", "volume=15 14 13 | 12\narpeggio=| 0 0 4 4 7 7\n" },
            { "Saw Lead", "vrc6saw", "2", "volume=12 14 15 14 | 13 / 9 6 3 1 0\nsawVolume=1\n" },
            { "Saw Brass", "vrc6saw", "2", "volume=4 7 10 12 14 15 | 14 / 10 6 3 0\nsawVolume=1\n" },
            { "Saw Arp", "vrc6saw", "2", "volume=14 13 12 | 11\narpeggio=| 0 0 3 3 7 7\nsawVolume=2\n" },
            { "MMC5 Bass", "mmc5pulse", "3", "volume=15 15 14 13 | 12 / 6 2 0\n" },
            { "MMC5 Arp", "mmc5pulse", "1", "volume=15 14 13 | 12\narpeggio=| 0 0 3 3 7 7\n" },
            { "MMC5 Harmony Pad", "mmc5pulse", "2", "volume=3 5 7 9 10 | 10 / 7 5 3 1 0\n" },
            { "S5B Lead", "s5b", "2", "volume=15 15 14 | 13 / 10 7 4 2 0\npitch=0 0 0 0 0 0 0 0 0 0 | 1 2 2 1 -1 -2 -2 -1\n" },
            { "S5B Arp", "s5b", "2", "volume=15 14 13 | 12\narpeggio=| 0 0 4 4 7 7\n" },
            { "S5B Bell", "s5b", "2", "volume=15 12 10 8 7 6 5 4 3 2 1 0\narpeggio=12 12 0\n" },
            { "S5B Growl Bass", "s5b", "2", "volume=15 15 14 | 13\ns5bShape=1\ns5bOctave=1\n" },
            { "S5B Snare", "s5b", "2", "volume=15 14 12 10 8 6 4 2 0\ns5bMixer=0 0 0 1 1 1 1 1 1\ns5bNoise=6 7 8 9 10 11 12 13 14\narpeggio=7 4 0\n" },
            { "S5B Hat", "s5b", "2", "volume=11 8 5 3 1 0\ns5bMixer=1\ns5bNoise=1\n" },
        };

        std::vector<Preset> result;

        for (const auto& d : definitions)
        {
            Preset preset;
            std::string error;
            const auto text = std::string(magicLine) + "\nname=" + d[0] + "\nchannel=" + d[1] + "\nduty=" + d[2] + "\n" + expandWaves(d[3], d[1]);

            parsePreset(text, preset, error);
            result.push_back(preset);
        }

        result.push_back(makeSynthDrumKit());
        result.push_back(makeDrumKit());

        return result;
    }();

    return presets;
}
