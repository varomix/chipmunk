// Plays a song script through the engine and measures it against FamiStudio's own WAV export.
//
//     fms_compare <song.script> <reference.wav> [shared|separate] [out.wav] [channel mask in hex]
//     fms_compare <song.script> <reference trace from FamiStudio's unit-test command> trace
//     fms_compare - <reference.wav> render <render.wav>
//
// "shared" runs every channel on one chip, as FamiStudio does. "separate" gives each channel its
// own chip, as one device per Live track does, so the difference is what the shared mixer adds.

#include "../tests/SongScript.h"

#include "Simple_Apu.h"

#undef min
#undef max
#undef clamp
#undef abs

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace
{
constexpr int sampleRate = 48000;
constexpr int dpcmAddress = 0xc000;

// The order of FamiStudio's NesApu.InitAndReset. Each write takes emulated time, which sets the
// phase of the song's first notes, so only the chips in use are written to.
void setUpChip(Simple_Apu& apu, const SongScript& song, int expansions, int numN163Channels, void* reader, int (*read)(void*, unsigned))
{
    // FamiStudio's export uses a mixer per channel whenever a project outputs stereo.
    const auto isStereo = (expansions & Simple_Apu::expansion_mask_epsm) != 0;

    apu.sample_rate(sampleRate, false, isStereo ? Simple_Apu::tnd_mode_separate : Simple_Apu::tnd_mode_single);
    apu.dmc_reader(read, reader);
    apu.set_audio_expansions(expansions);
    apu.bass_freq(Simple_Apu::expansion_none, song.bassCutoffHz > 0 ? song.bassCutoffHz : 24);
    apu.reset();

    constexpr double globalVolumeDb = -2.0;

    struct Mixer { int chip, mask; double trebleDb; int rolloffHz, bassCutoffHz; };

    // FamiStudio's defaults, which projects can override.
    static constexpr Mixer mixers[] {
        { Simple_Apu::expansion_none, ~0, -5.0, 14000, 0 },
        { Simple_Apu::expansion_vrc6, Simple_Apu::expansion_mask_vrc6, -5.0, 14000, 0 },
        { Simple_Apu::expansion_vrc7, Simple_Apu::expansion_mask_vrc7, -15.0, 14000, 0 },
        { Simple_Apu::expansion_fds, Simple_Apu::expansion_mask_fds, 0.0, 2000, 5 },
        { Simple_Apu::expansion_mmc5, Simple_Apu::expansion_mask_mmc5, -5.0, 14000, 0 },
        { Simple_Apu::expansion_namco, Simple_Apu::expansion_mask_namco, -15.0, 14000, 0 },
        { Simple_Apu::expansion_sunsoft, Simple_Apu::expansion_mask_sunsoft, -5.0, 14000, 0 },
        { Simple_Apu::expansion_epsm, Simple_Apu::expansion_mask_epsm, -5.0, 14000, 0 },
    };

    for (auto mixer : mixers)
    {
        if ((expansions & mixer.mask) == 0 && mixer.chip != Simple_Apu::expansion_none)
            continue;

        auto volumeDb = 0.0;
        const auto projectMixer = song.mixers.find(mixer.chip);

        if (projectMixer != song.mixers.end())
        {
            volumeDb = projectMixer->second.volumeDb;
            mixer.trebleDb = projectMixer->second.trebleDb;
            mixer.rolloffHz = projectMixer->second.rolloffHz;
            mixer.bassCutoffHz = projectMixer->second.bassCutoffHz;
        }

        apu.treble_eq(mixer.chip, mixer.trebleDb, mixer.rolloffHz, sampleRate);
        apu.set_expansion_volume(mixer.chip, std::pow(10.0, (volumeDb + globalVolumeDb) / 20.0));

        if (mixer.chip == Simple_Apu::expansion_fds)
            apu.bass_freq(Simple_Apu::expansion_fds, mixer.bassCutoffHz);
    }

    if ((expansions & Simple_Apu::expansion_mask_namco) != 0)
        apu.set_namco_mix(true);

    for (const auto& [reg, value] : std::vector<std::pair<int, int>> { { 0x4015, 0x0f }, { 0x4008, 0x80 }, { 0x400f, 0x00 }, { 0x4000, 0x30 },
                                                                       { 0x4004, 0x30 }, { 0x400c, 0x30 }, { 0x4001, 0x08 }, { 0x4005, 0x08 } })
        apu.write_register((cpu_addr_t) reg, value);

    if ((expansions & Simple_Apu::expansion_mask_vrc6) != 0)
        apu.write_register(0x9003, 0x00);

    if ((expansions & Simple_Apu::expansion_mask_vrc7) != 0)
        apu.write_register(0xe000, 0x00);

    if ((expansions & Simple_Apu::expansion_mask_mmc5) != 0)
    {
        apu.write_register(0x5000, 0x10);
        apu.write_register(0x5004, 0x10);
        apu.write_register(0x5015, 0x03);
    }

    if ((expansions & Simple_Apu::expansion_mask_namco) != 0)
    {
        apu.write_register(0xf800, 0x7f);
        apu.write_register(0x4800, (numN163Channels - 1) << 4);
    }

    if ((expansions & Simple_Apu::expansion_mask_sunsoft) != 0)
    {
        apu.write_register(0xc000, 0x07);
        apu.write_register(0xf000, 0x38);

        if ((expansions & Simple_Apu::expansion_mask_namco) != 0) // See ChannelStateS5B.cs.
            apu.write_register(0xc000, 0x0e);
    }

    if ((expansions & Simple_Apu::expansion_mask_epsm) != 0)
    {
        for (const auto& [reg, value] : std::vector<std::pair<int, int>> { { 0x07, 0x38 }, { 0x29, 0x80 }, { 0x27, 0x00 }, { 0x11, 0x37 } })
        {
            apu.write_register(0x401c, reg);
            apu.skip_cycles(4);
            apu.write_register(0x401d, value);
            apu.skip_cycles(20);
        }
    }
}

class Chip final : public RegisterSink
{
public:
    Chip(const SongScript& song, int expansions, int numN163Channels) : isStereo((expansions & Simple_Apu::expansion_mask_epsm) != 0)
    {
        setUpChip(apu, song, expansions, numN163Channels, this, &Chip::readMemory);
    }

    void writeRegister(int reg, int data) override { apu.write_register((cpu_addr_t) reg, data); }
    void skipCycles(int cycles) override { apu.skip_cycles(cycles); }
    void selectDpcmSample(const DpcmSampleData* newSample) override { sample = newSample; }
    void enable(int expansion, int index, bool enabled) { apu.enable_channel(expansion, index, enabled); }

    void endFrame(std::vector<int16_t>& output)
    {
        apu.end_frame();
        const auto start = output.size();
        const auto count = apu.samples_avail();
        output.resize(start + (size_t) count * (isStereo ? 2 : 1));
        apu.read_samples(output.data() + start, count);
    }

private:
    static int readMemory(void* self, unsigned address)
    {
        const auto* sample = static_cast<Chip*>(self)->sample;
        const auto offset = (long) address - dpcmAddress;
        return sample != nullptr && offset >= 0 && offset < (long) sample->data.size() ? sample->data[(size_t) offset] : 64;
    }

    Simple_Apu apu;
    const DpcmSampleData* sample = nullptr;
    bool isStereo;
};

std::vector<int16_t> readWav(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    const std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    for (size_t position = 12; position + 8 <= data.size();)
    {
        uint32_t size;
        std::memcpy(&size, data.data() + position + 4, 4);
        if (data.compare(position, 4, "data") == 0)
        {
            std::vector<int16_t> samples(size / 2);
            std::memcpy(samples.data(), data.data() + position + 8, samples.size() * 2);
            return samples;
        }
        position += 8 + size + (size & 1);
    }
    return {};
}

struct TraceSink final : RegisterSink
{
    std::vector<std::string> lines;
    int frame = 0;

    void writeRegister(int reg, int data) override
    {
        char line[64];
        std::snprintf(line, sizeof(line), "Frame %d Register %04X %02X", frame, reg, data);
        lines.emplace_back(line);
    }

    void skipCycles(int) override {}
};

int compareTrace(const SongScript& song, const std::string& path)
{
    TraceSink sink;
    SongChannels channels(song.channelFileTypes, [&](size_t) -> RegisterSink& { return sink; }, true);

    for (int frame = 0; frame < song.totalFrames; ++frame)
    {
        sink.frame = frame;
        channels.playFrame(song, frame);
    }

    std::ifstream file(path);
    std::string line;
    size_t index = 0, mismatches = 0, first = 0;
    std::vector<std::string> expected;

    while (std::getline(file, line))
    {
        line.erase(line.find_last_not_of("\r\n") + 1);
        expected.push_back(line);
    }

    for (; index < std::min(expected.size(), sink.lines.size()); ++index)
    {
        if (expected[index] != sink.lines[index])
        {
            if (mismatches++ == 0)
                first = index;
        }
    }

    std::printf("%zu writes, reference %zu, mismatched %zu\n", sink.lines.size(), expected.size(), mismatches);

    if (mismatches > 0)
        for (auto i = first > 2 ? first - 2 : 0; i < first + 5 && i < std::min(expected.size(), sink.lines.size()); ++i)
            std::printf("  %zu: expected [%s] got [%s]\n", i, expected[i].c_str(), sink.lines[i].c_str());

    return mismatches == 0 && expected.size() == sink.lines.size() ? 0 : 1;
}

void fourierTransform(std::vector<std::complex<double>>& values)
{
    const auto size = values.size();

    for (size_t i = 1, j = 0; i < size; ++i)
    {
        auto bit = size >> 1;

        for (; (j & bit) != 0; bit >>= 1)
            j ^= bit;

        j ^= bit;

        if (i < j)
            std::swap(values[i], values[j]);
    }

    for (size_t length = 2; length <= size; length <<= 1)
    {
        const auto step = std::polar(1.0, -2.0 * M_PI / (double) length);

        for (size_t i = 0; i < size; i += length)
        {
            std::complex<double> twiddle(1.0);

            for (size_t j = 0; j < length / 2; ++j)
            {
                const auto a = values[i + j];
                const auto b = values[i + j + length / 2] * twiddle;

                values[i + j] = a + b;
                values[i + j + length / 2] = a - b;
                twiddle *= step;
            }
        }
    }
}

// The average gap in dB between two sounds' spectra over time, in bands a third of an octave wide.
// Phase doesn't count, so it reads near zero for sounds that are heard as the same.
double spectralDifference(const std::vector<double>& a, const std::vector<double>& b)
{
    constexpr size_t frameSize = 2048;
    constexpr int numBands = 30;
    constexpr double lowestHz = 50.0, floorDb = -70.0;

    const auto bandsOf = [&](const std::vector<double>& audio, size_t position) {
        std::vector<std::complex<double>> frame(frameSize);

        for (size_t i = 0; i < frameSize; ++i)
            frame[i] = audio[position + i] / 32768.0 * (0.5 - 0.5 * std::cos(2.0 * M_PI * (double) i / frameSize));

        fourierTransform(frame);

        std::array<double, numBands> bands {};

        for (size_t bin = 1; bin < frameSize / 2; ++bin)
        {
            const auto hz = (double) bin * sampleRate / frameSize;
            const auto band = (int) (std::log2(hz / lowestHz) * 3.0);

            if (band >= 0 && band < numBands)
                bands[(size_t) band] += std::norm(frame[bin]);
        }

        for (auto& band : bands)
            band = std::max(floorDb, 10.0 * std::log10(std::max(band, 1.0e-12)));

        return bands;
    };

    auto total = 0.0;
    auto count = 0;

    for (size_t position = 0; position + frameSize <= std::min(a.size(), b.size()); position += frameSize)
    {
        const auto bandsA = bandsOf(a, position), bandsB = bandsOf(b, position);

        for (size_t band = 0; band < bandsA.size(); ++band)
        {
            // Bands silent in both say nothing about how alike the sounds are.
            if (bandsA[band] > floorDb || bandsB[band] > floorDb)
            {
                total += std::abs(bandsA[band] - bandsB[band]);
                ++count;
            }
        }
    }

    return count > 0 ? total / count : 0.0;
}

// Reads a 16-bit WAV of any channel count as mono.
std::vector<double> readWavAsMono(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    const std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    uint16_t channels = 1;

    for (size_t position = 12; position + 8 <= data.size();)
    {
        uint32_t size;
        std::memcpy(&size, data.data() + position + 4, 4);

        if (data.compare(position, 4, "fmt ") == 0)
            std::memcpy(&channels, data.data() + position + 10, 2);

        position += 8 + size + (size & 1);
    }

    std::vector<double> mono;
    const auto samples = readWav(path);

    for (size_t i = 0; i + channels <= samples.size(); i += channels)
    {
        auto sum = 0.0;
        for (int c = 0; c < channels; ++c)
            sum += samples[i + (size_t) c];
        mono.push_back(sum / channels);
    }

    return mono;
}

// How close a Live Set's render comes. Separate chips mean phases differ, so the spectrum is what counts.
int compareRender(const std::string& referencePath, const std::string& renderPath)
{
    const auto reference = readWavAsMono(referencePath), render = readWavAsMono(renderPath);

    if (reference.empty() || render.empty())
    {
        std::puts("can't read the files");
        return 2;
    }

    auto referencePower = 0.0, renderPower = 0.0;
    const auto length = std::min(reference.size(), render.size());

    for (size_t i = 0; i < length; ++i)
    {
        referencePower += reference[i] * reference[i];
        renderPower += render[i] * render[i];
    }

    std::printf("level %+5.2f dB  length %6.1f s (ref %6.1f s)  spectrum %5.2f dB\n", 10.0 * std::log10(std::max(renderPower, 1.0) / std::max(referencePower, 1.0)),
                (double) render.size() / sampleRate, (double) reference.size() / sampleRate, spectralDifference(render, reference));
    return 0;
}

void writeWav(const std::string& path, const std::vector<int16_t>& samples, int channels)
{
    std::ofstream file(path, std::ios::binary);
    const auto put = [&](uint32_t value, int bytes) { file.write(reinterpret_cast<const char*>(&value), bytes); };
    const auto size = (uint32_t) samples.size() * 2;
    file.write("RIFF", 4); put(36 + size, 4); file.write("WAVEfmt ", 8); put(16, 4); put(1, 2); put((uint32_t) channels, 2);
    put(sampleRate, 4); put((uint32_t) (sampleRate * 2 * channels), 4); put((uint32_t) (2 * channels), 2); put(16, 2); file.write("data", 4); put(size, 4);
    file.write(reinterpret_cast<const char*>(samples.data()), size);
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::puts("usage: fms_compare <song.script> <reference.wav> [shared|separate] [out.wav]");
        return 2;
    }

    if (argc > 4 && std::strcmp(argv[3], "render") == 0)
        return compareRender(argv[2], argv[4]);

    const auto separate = argc > 3 && std::strcmp(argv[3], "separate") == 0;
    const auto channelMask = argc > 5 ? std::stoi(argv[5], nullptr, 16) : -1;

    SongScript song;
    {
        std::ifstream script(argv[1]);
        readSongScript(script, song);
    }

    if (argc > 3 && std::strcmp(argv[3], "trace") == 0)
        return compareTrace(song, argv[2]);

    auto expansions = 0, numN163 = 0;

    for (const auto type : song.channelFileTypes)
    {
        if (type >= 5 && type <= 7) expansions |= Simple_Apu::expansion_mask_vrc6;
        if (type >= 8 && type <= 13) expansions |= Simple_Apu::expansion_mask_vrc7;
        if (type == 14) expansions |= Simple_Apu::expansion_mask_fds;
        if (type >= 15 && type <= 16) expansions |= Simple_Apu::expansion_mask_mmc5;
        if (type >= 18 && type <= 25) { expansions |= Simple_Apu::expansion_mask_namco; ++numN163; }
        if (type >= 26 && type <= 28) expansions |= Simple_Apu::expansion_mask_sunsoft;
        if (type >= 29) expansions |= Simple_Apu::expansion_mask_epsm;
    }

    const auto isStereo = (expansions & Simple_Apu::expansion_mask_epsm) != 0;

    std::vector<std::unique_ptr<Chip>> chips;

    for (size_t i = 0; i < (separate ? song.channelFileTypes.size() : 1); ++i)
        chips.push_back(std::make_unique<Chip>(song, expansions, std::max(1, numN163)));

    SongChannels channels(song.channelFileTypes, [&](size_t i) -> RegisterSink& { return *chips[separate ? i : 0]; }, ! separate);

    // Muting only silences the output, as in FamiStudio, so every write keeps its timing. Chips
    // start with some channels off, so each one has to be set either way.
    for (size_t i = 0; i < song.channelFileTypes.size(); ++i)
    {
        const auto type = song.channelFileTypes[i];
        auto& chip = *chips[separate ? i : 0];
        const auto enabled = separate || ((channelMask >> i) & 1) != 0;

        if (type <= 4) chip.enable(Simple_Apu::expansion_none, type, enabled);
        else if (type <= 7) chip.enable(Simple_Apu::expansion_vrc6, type - 5, enabled);
        else if (type <= 13) chip.enable(Simple_Apu::expansion_vrc7, type - 8, enabled);
        else if (type == 14) chip.enable(Simple_Apu::expansion_fds, 0, enabled);
        else if (type <= 16) chip.enable(Simple_Apu::expansion_mmc5, type - 15, enabled);
        else if (type <= 25) chip.enable(Simple_Apu::expansion_namco, type - 18, enabled);
        else if (type <= 28) chip.enable(Simple_Apu::expansion_sunsoft, type - 26, enabled);
        else chip.enable(Simple_Apu::expansion_epsm, type - 29, enabled);
    }

    std::vector<std::vector<int16_t>> output(chips.size());

    for (int frame = 0; frame < song.totalFrames; ++frame)
    {
        channels.playFrame(song, frame);

        for (size_t i = 0; i < chips.size(); ++i)
            chips[i]->endFrame(output[i]);
    }

    auto length = output[0].size();
    for (const auto& channelOutput : output)
        length = std::min(length, channelOutput.size());

    std::vector<double> ours(length, 0.0);
    for (const auto& channelOutput : output)
        for (size_t i = 0; i < length; ++i)
            ours[i] += channelOutput[i];

    if (argc > 4 && std::strcmp(argv[4], "-") != 0)
    {
        std::vector<int16_t> rounded(length);
        for (size_t i = 0; i < length; ++i)
            rounded[i] = (int16_t) std::clamp(std::lround(ours[i]), -32768L, 32767L);
        writeWav(argv[4], rounded, isStereo ? 2 : 1);
    }

    const auto reference = readWav(argv[2]);

    if (reference.empty())
    {
        std::puts("can't read the reference");
        return 2;
    }

    // FamiStudio's export may start a few samples apart, so find the best alignment first.
    constexpr int maxLag = 480;
    const auto searchLength = std::min({ length, reference.size(), (size_t) sampleRate * 30 }) - maxLag;
    auto bestLag = 0;
    auto bestScore = -1.0;

    for (int lag = -maxLag; lag <= maxLag; ++lag)
    {
        auto score = 0.0;
        for (size_t i = maxLag; i < searchLength; i += 3)
            score += ours[i] * reference[(size_t) ((long) i + lag)];
        if (score > bestScore)
        {
            bestScore = score;
            bestLag = lag;
        }
    }

    const auto start = (size_t) std::max(0, -bestLag);
    const auto compared = std::min(length - start, reference.size() - (size_t) std::max(0, bestLag) - start);
    const auto refAt = [&](size_t i) { return (double) reference[(size_t) ((long) (start + i) + bestLag)]; };

    auto ourMean = 0.0, refMean = 0.0;
    for (size_t i = 0; i < compared; ++i)
    {
        ourMean += ours[start + i];
        refMean += refAt(i);
    }
    ourMean /= (double) compared;
    refMean /= (double) compared;

    auto cross = 0.0, ourPower = 0.0, refPower = 0.0;
    for (size_t i = 0; i < compared; ++i)
    {
        const auto a = ours[start + i] - ourMean, b = refAt(i) - refMean;
        cross += a * b;
        ourPower += a * a;
        refPower += b * b;
    }

    const auto gain = ourPower > 0.0 ? cross / ourPower : 1.0;
    const auto correlation = cross / std::sqrt(std::max(1.0, ourPower * refPower));

    auto residual = 0.0, worstWindow = 0.0, windowSum = 0.0;
    size_t worstAt = 0;
    constexpr size_t windowSize = sampleRate;

    for (size_t i = 0; i < compared; ++i)
    {
        const auto difference = refAt(i) - refMean - gain * (ours[start + i] - ourMean);
        residual += difference * difference;
        windowSum += difference * difference;
        if ((i + 1) % windowSize == 0)
        {
            if (windowSum > worstWindow)
            {
                worstWindow = windowSum;
                worstAt = i + 1 - windowSize;
            }
            windowSum = 0.0;
        }
    }

    const auto toDb = [](double ratio) { return 10.0 * std::log10(std::max(ratio, 1.0e-12)); };

    // A null test punishes phase differences nobody can hear, so also compare what's in the sound.
    const auto numChannels = isStereo ? 2 : 1;
    const auto referenceFor = [&](size_t i) { return refAt(i); };
    std::vector<double> ourMono(compared / (size_t) numChannels), refMono(compared / (size_t) numChannels);

    for (size_t i = 0; i < ourMono.size(); ++i)
    {
        for (int c = 0; c < numChannels; ++c)
        {
            ourMono[i] += ours[start + i * (size_t) numChannels + (size_t) c];
            refMono[i] += referenceFor(i * (size_t) numChannels + (size_t) c);
        }
    }

    std::printf("null %7.1f dB  corr %.5f  gain %+5.2f dB  lag %5d  length %6.1f s (ref %6.1f s)  worst sec %5.0f at %6.1f dB  spectrum %5.2f dB\n",
                toDb(residual / refPower), correlation, 20.0 * std::log10(std::max(gain, 1.0e-6)), bestLag,
                (double) length / sampleRate / numChannels, (double) reference.size() / sampleRate / numChannels, (double) worstAt / sampleRate / numChannels,
                toDb(worstWindow / (refPower / ((double) compared / windowSize))), spectralDifference(ourMono, refMono));
    return 0;
}
