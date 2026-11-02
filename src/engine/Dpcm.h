#pragma once

#include "engine/Instrument.h"
#include "engine/NoteTables.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

constexpr int dpcmDefaultInitialValueDiv2 = 32;
constexpr int dpcmMaxSampleBytes = (255 << 4) + 1;
constexpr int dpcmNumRates = 16;

// Playback rates in Hz, from FamiStudio's DPCMSampleRate. Rate 15 is the fastest.
float dpcmRateHz(int rate, bool pal = false);

struct DpcmSampleData
{
    std::string name;
    std::vector<uint8_t> data;
    int initialValueDiv2 = dpcmDefaultInitialValueDiv2;
};

// A noise sound on a Drum Kit key. Keys can share one, playing it at different noise pitches.
struct KitSound
{
    std::string name;
    Instrument instrument;
};

struct DpcmMapping
{
    std::shared_ptr<const DpcmSampleData> sample;

    // On a Drum Kit, a key plays this instead of a sample, with this note setting the noise pitch.
    std::shared_ptr<const KitSound> noise;
    int noiseNote = 48;

    int pitch = 15;
    bool loop = false;
    bool overrideInitialValue = false;
    int initialValueDiv2 = dpcmDefaultInitialValueDiv2;
};

// What each key plays, indexed by note value. Treated as immutable once shared with
// the audio thread: edits make a new kit, which is cheap because the sample data is shared.
struct DpcmKit
{
    std::array<DpcmMapping, noteMusicalMax + 1> mappings;

    bool hasSamples() const
    {
        for (const auto& mapping : mappings)
            if (mapping.sample != nullptr)
                return true;

        return false;
    }

    bool isEmpty() const
    {
        for (const auto& mapping : mappings)
            if (mapping.sample != nullptr || mapping.noise != nullptr)
                return false;

        return true;
    }
};

// Sample processing, ported from FamiStudio's DPCMSample.Process and WaveUtils.
enum class DpcmPadding
{
    none,
    padTo16Bytes,
    padTo16BytesPlusOne,
    roundTo16Bytes,
    roundTo16BytesPlusOne,
    trimTo16Bytes,
    trimTo16BytesPlusOne
};

struct DpcmSource
{
    bool isWave = false;
    std::vector<int16_t> wave;
    int waveRate = 0;
    std::vector<uint8_t> dmc;

    int processingRate = 15;
    int volumeAdjust = 100;
    float finePitch = 1.0f;
    DpcmPadding padding = DpcmPadding::padTo16Bytes;
    int initialValueDiv2 = dpcmDefaultInitialValueDiv2;
    bool reverseBits = false;
    bool trimZeroVolume = false;
    bool palProcessing = false;

    struct VolumePoint { int sample; float volume; };
    std::array<VolumePoint, 4> volumeEnvelope { VolumePoint { 0, 1.0f }, { 0, 1.0f }, { 0, 1.0f }, { 0, 1.0f } };
};

std::vector<uint8_t> processDpcmSample(const DpcmSource&);

// Rebuilds audio from DPCM data, for drawing and for tests.
std::vector<int16_t> dpcmToWave(const std::vector<uint8_t>& dmc, int initialCounter);

// A converted sound, plus the rate that plays it at its original pitch.
struct ConvertedDpcm
{
    std::shared_ptr<const DpcmSampleData> sample;
    int rate = 15;
    bool wasTruncated = false;
};

// Converts audio for a drum-style workflow. It's normalized and trimmed of silence, since DPCM is
// coarse and hits should start immediately. It then uses the fastest rate that fits the whole
// sound in the chip's 4081-byte limit, which is about 1 second at the fastest rate.
ConvertedDpcm convertWaveToDpcm(const std::string& name, std::vector<int16_t> wave, int waveRate);

// Name of a key as Live shows it, where MIDI note 60 is C3.
std::string midiNoteName(int midiNote);
