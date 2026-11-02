#include "engine/Dpcm.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace
{
constexpr float ratesHz[2][dpcmNumRates] = {
    { 4181.7124f, 4709.9287f, 5264.038f, 5593.0405f, 6257.9478f, 7046.3506f, 7919.3496f, 8363.425f,
      9419.857f, 11186.081f, 12604.035f, 13982.602f, 16884.65f, 21306.822f, 24857.959f, 33143.945f },
    { 4177.4043f, 4696.63f, 5261.4146f, 5579.2183f, 6023.9385f, 7044.945f, 7917.1763f, 8397.005f,
      9446.631f, 11233.831f, 12595.508f, 14089.89f, 16965.377f, 21315.475f, 25191.016f, 33252.14f }
};

constexpr float dpcmMaxRateNtsc = 33143.945f;
constexpr float dpcmMaxRatePal = 33252.14f;

int counterToWaveSample(int counter)
{
    return counter * (65536 / 64) - 32768;
}

// C#'s Math.Round rounds ties to even, and so does nearbyint in the default rounding mode.
int roundLikeCSharp(double value)
{
    return (int) std::nearbyint(value);
}

void resample(const std::vector<int16_t>& source, int minSample, int maxSample, std::vector<int16_t>& dest)
{
    const auto ratio = (float) (maxSample - minSample) / (float) dest.size();
    const auto last = (int) source.size() - 1;

    for (size_t i = 0; i < dest.size(); ++i)
    {
        const auto position = (float) minSample + (float) i * ratio;
        const auto s0 = (float) source[(size_t) std::min((int) std::floor(position), last)];
        const auto s1 = (float) source[(size_t) std::min((int) std::ceil(position), last)];
        const auto alpha = position - std::floor(position);

        dest[i] = (int16_t) (s0 + (s1 - s0) * alpha);
    }
}

std::vector<uint8_t> waveToDpcm(const std::vector<int16_t>& wave, int minSample, int maxSample, float waveRate, float dpcmRate,
                                int counterStart, DpcmPadding padding)
{
    if (wave.empty())
        return {};

    const auto count = (float) (maxSample - minSample) * (dpcmRate / waveRate);
    auto numSamples = 0;

    if (padding == DpcmPadding::roundTo16Bytes)
        numSamples = roundLikeCSharp(count / 128.0f) * 128;
    else if (padding == DpcmPadding::roundTo16BytesPlusOne)
        numSamples = roundLikeCSharp(count / 128.0f) * 128 + 8;
    else
        numSamples = roundLikeCSharp(count);

    std::vector<int16_t> resampled((size_t) std::max(0, numSamples));

    if (! resampled.empty())
        resample(wave, minSample, maxSample, resampled);

    std::vector<uint8_t> dpcm((resampled.size() + 7) / 8);

    auto counter = counterStart;
    auto lastBit = 0;

    for (int i = 0; i < (int) resampled.size(); ++i)
    {
        auto up = false;

        if (i != (int) resampled.size() - 1)
        {
            // Is it better to go up or down?
            const auto next = (int) resampled[(size_t) i + 1];
            const auto distUp = std::abs(counterToWaveSample(std::min(counter + 1, 63)) - next);
            const auto distDown = std::abs(counterToWaveSample(std::max(counter - 1, 0)) - next);
            up = distUp < distDown;
        }
        else
        {
            up = counterToWaveSample(counter) < resampled[(size_t) i];
        }

        if (up)
        {
            dpcm[(size_t) i / 8] |= (uint8_t) (1 << (i & 7));
            counter = std::min(counter + 1, 63);
            lastBit = 1;
        }
        else
        {
            counter = std::max(counter - 1, 0);
            lastBit = 0;
        }
    }

    // The last byte may not be fully written, so fill it with alternating bits.
    const auto rounded = ((int) resampled.size() + 7) / 8 * 8;

    for (int i = (int) resampled.size(); i < rounded; ++i)
    {
        if (lastBit == 0)
            dpcm[(size_t) i / 8] |= (uint8_t) (1 << (i & 7));

        lastBit ^= 1;
    }

    return dpcm;
}

uint8_t reverseBits(uint8_t b)
{
    uint8_t result = 0;

    for (int i = 0; i < 8; ++i)
        if ((b & (1 << i)) != 0)
            result |= (uint8_t) (1 << (7 - i));

    return result;
}

void nonZeroWaveRange(const std::vector<int16_t>& wave, int threshold, int& first, int& last)
{
    first = 0;
    last = 0;

    for (int i = 0; i < (int) wave.size(); ++i)
    {
        if (std::abs((int) wave[(size_t) i]) > threshold)
        {
            first = i;
            break;
        }
    }

    for (int i = (int) wave.size() - 1; i >= 0; --i)
    {
        if (std::abs((int) wave[(size_t) i]) > threshold)
        {
            last = i + 1;
            break;
        }
    }
}

int16_t scaled(int16_t value, float volume)
{
    return (int16_t) std::clamp(roundLikeCSharp((double) ((float) value * volume)), -32768, 32767);
}

int roundUp(int value, int multiple) { return (value + multiple - 1) / multiple * multiple; }
int roundDown(int value, int multiple) { return value / multiple * multiple; }
} // namespace

float dpcmRateHz(int rate, bool pal)
{
    return ratesHz[pal ? 1 : 0][std::clamp(rate, 0, dpcmNumRates - 1)];
}

std::vector<int16_t> dpcmToWave(const std::vector<uint8_t>& dmc, int initialCounter)
{
    std::vector<int16_t> wave(dmc.size() * 8);
    auto counter = initialCounter;

    for (size_t i = 0; i < dmc.size(); ++i)
    {
        for (int j = 0; j < 8; ++j)
        {
            wave[i * 8 + (size_t) j] = (int16_t) counterToWaveSample(counter);
            counter = (dmc[i] & (1 << j)) != 0 ? std::min(counter + 1, 63) : std::max(counter - 1, 0);
        }
    }

    return wave;
}

std::vector<uint8_t> processDpcmSample(const DpcmSource& source)
{
    const auto targetRate = dpcmRateHz(source.processingRate, source.palProcessing);

    auto usesVolumeEnvelope = false;
    for (const auto& point : source.volumeEnvelope)
        usesVolumeEnvelope = usesVolumeEnvelope || std::abs(point.volume - 1.0f) > 0.02f;

    const auto nearlyOne = std::abs(source.finePitch - 1.0f) < 0.0001f;
    const auto isRounded = source.padding == DpcmPadding::roundTo16Bytes || source.padding == DpcmPadding::roundTo16BytesPlusOne;

    std::vector<uint8_t> processed;

    // Fast path for when there is (almost) nothing to do.
    if (! source.isWave && source.volumeAdjust == 100 && nearlyOne && ! usesVolumeEnvelope && source.processingRate == 15 && ! source.trimZeroVolume)
    {
        processed = source.dmc;

        if (source.reverseBits)
            for (auto& b : processed)
                b = reverseBits(b);
    }
    else
    {
        std::vector<int16_t> wave;
        const auto sourceRate = source.isWave ? (float) source.waveRate : (source.palProcessing ? dpcmMaxRatePal : dpcmMaxRateNtsc);

        if (source.isWave)
        {
            wave = source.wave;
        }
        else
        {
            auto dmc = source.dmc;

            if (source.reverseBits)
                for (auto& b : dmc)
                    b = reverseBits(b);

            wave = dpcmToWave(dmc, source.initialValueDiv2);
        }

        if (! nearlyOne)
        {
            const auto newLength = roundLikeCSharp((double) ((float) wave.size() / source.finePitch));

            if (newLength > 0 && newLength != (int) wave.size())
            {
                std::vector<int16_t> pitched((size_t) newLength);
                resample(wave, 0, (int) wave.size(), pitched);
                wave = std::move(pitched);
            }
        }

        const auto overall = (float) source.volumeAdjust / 100.0f;

        if (usesVolumeEnvelope && ! wave.empty())
        {
            const auto lastSample = (int) wave.size() - 1;
            const std::array<int, 4> samples { 0, source.volumeEnvelope[1].sample, source.volumeEnvelope[2].sample, lastSample };

            for (size_t i = 0; i + 1 < samples.size(); ++i)
            {
                const auto s0 = std::clamp(samples[i], 0, lastSample);
                const auto s1 = std::clamp(samples[i + 1], 0, lastSample);
                const auto v0 = source.volumeEnvelope[i].volume * overall;
                const auto v1 = source.volumeEnvelope[i + 1].volume * overall;

                for (int j = s0; j <= s1; ++j)
                {
                    const auto ratio = s0 == s1 ? 0.0f : (float) (j - s0) / (float) (s1 - s0);
                    const auto smooth = ratio * ratio * (3.0f - 2.0f * ratio);
                    wave[(size_t) j] = scaled(wave[(size_t) j], v0 + (v1 - v0) * smooth);
                }
            }
        }
        else if (source.volumeAdjust != 100)
        {
            for (auto& s : wave)
                s = scaled(s, (float) std::max(0, source.volumeAdjust) / 100.0f);
        }

        auto minSample = 0;
        auto maxSample = (int) wave.size();

        if (source.trimZeroVolume)
            nonZeroWaveRange(wave, source.isWave ? 512 : 1024, minSample, maxSample);

        const auto startVolume = source.volumeEnvelope[0].volume * ((float) std::max(0, source.volumeAdjust) / 100.0f);
        const auto initial = std::clamp(roundLikeCSharp((double) ((float) (source.initialValueDiv2 - dpcmDefaultInitialValueDiv2) * startVolume)) + dpcmDefaultInitialValueDiv2, 0, 63);

        processed = waveToDpcm(wave, minSample, maxSample, sourceRate, targetRate, initial, source.padding);
    }

    // Remove any leftover silence, which rounded samples can't do.
    if (source.trimZeroVolume && ! isRounded)
    {
        auto first = 0;
        auto last = (int) processed.size();

        for (int i = 0; i < (int) processed.size(); ++i)
        {
            if (processed[(size_t) i] != 0x55 && processed[(size_t) i] != 0xaa)
            {
                first = i;
                break;
            }
        }

        for (int i = (int) processed.size() - 1; i >= 0; --i)
        {
            if (processed[(size_t) i] != 0x55 && processed[(size_t) i] != 0xaa)
            {
                last = i + 1;
                break;
            }
        }

        processed = std::vector<uint8_t>(processed.begin() + first, processed.begin() + last);
    }

    const auto oldSize = (int) processed.size();
    auto newSize = oldSize;

    switch (source.padding)
    {
        case DpcmPadding::padTo16Bytes: newSize = roundUp(oldSize, 16); break;
        case DpcmPadding::padTo16BytesPlusOne: newSize = roundUp(std::max(0, oldSize - 1), 16) + 1; break;
        case DpcmPadding::trimTo16Bytes: newSize = roundDown(oldSize, 16); break;
        case DpcmPadding::trimTo16BytesPlusOne: newSize = roundDown(std::max(0, oldSize - 1), 16) + 1; break;
        default: break;
    }

    if (newSize != oldSize)
    {
        const auto lastByte = oldSize > 0 ? processed[(size_t) oldSize - 1] : (uint8_t) 0;
        processed.resize((size_t) newSize, (lastByte & 0x80) != 0 ? 0xaa : 0x55);
    }

    if ((int) processed.size() > dpcmMaxSampleBytes)
        processed.resize((size_t) dpcmMaxSampleBytes);

    return processed;
}

ConvertedDpcm convertWaveToDpcm(const std::string& name, std::vector<int16_t> wave, int waveRate)
{
    auto peak = 0;
    for (auto s : wave)
        peak = std::max(peak, std::abs((int) s));

    if (peak > 0)
        for (auto& s : wave)
            s = (int16_t) std::clamp((int) std::lround((double) s * 32000.0 / peak), -32768, 32767);

    // Keep very quiet tails, which would otherwise turn into extra samples of nothing.
    auto first = 0, last = (int) wave.size();
    nonZeroWaveRange(wave, 512, first, last);

    std::vector<int16_t> trimmed(wave.begin() + first, wave.begin() + std::max(first, last));

    ConvertedDpcm result;
    result.rate = 0;

    const auto seconds = trimmed.size() / (double) std::max(1, waveRate);

    for (int rate = dpcmNumRates - 1; rate >= 0; --rate)
    {
        if (seconds * dpcmRateHz(rate) / 8.0 <= dpcmMaxSampleBytes - 16)
        {
            result.rate = rate;
            break;
        }
    }

    result.wasTruncated = seconds * dpcmRateHz(result.rate) / 8.0 > dpcmMaxSampleBytes;

    DpcmSource source;
    source.isWave = true;
    source.wave = std::move(trimmed);
    source.waveRate = waveRate;
    source.processingRate = result.rate;

    auto sample = std::make_shared<DpcmSampleData>();
    sample->name = name;
    sample->data = processDpcmSample(source);
    result.sample = std::move(sample);

    return result;
}

std::string midiNoteName(int midiNote)
{
    static constexpr const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return std::string(names[midiNote % 12]) + std::to_string(midiNote / 12 - 2);
}
