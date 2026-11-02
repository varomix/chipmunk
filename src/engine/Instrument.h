#pragma once

#include <array>
#include <cmath>
#include <cstdint>

// Port of FamiStudio's Project/Envelope.cs, limited to what the channels in scope use.
enum EnvelopeType
{
    envelopeVolume,
    envelopeArpeggio,
    envelopePitch,
    envelopeDutyCycle,
    envelopeS5bMixer,
    envelopeS5bNoise,
    numEnvelopeTypes
};

struct Envelope
{
    static constexpr int maxLength = 256;

    std::array<int8_t, maxLength> values {};
    int length = 0;
    int loop = -1;
    int release = -1;
    bool relative = false;

    static constexpr int defaultValue(int type) { return type == envelopeVolume ? 15 : type == envelopeS5bMixer ? 2 : 0; }

    bool isEmpty(int type) const
    {
        for (int i = 0; i < length; ++i)
            if (values[(size_t) i] != defaultValue(type))
                return false;

        return true;
    }
};

struct Instrument
{
    static constexpr int maxWaveSteps = 1024;

    std::array<Envelope, numEnvelopeTypes> envelopes;

    // VRC6 saw only: 0 is full, 1 half and 2 quarter. Half is what FamiStudio starts with.
    int sawMasterVolume = 1;

    // S5B only. Shape 0 is off, and 1 to 8 are the chip's envelope shapes 8 to 15. With auto
    // pitch the envelope follows the note, offset by the octave. Otherwise it uses the period.
    int s5bEnvelopeShape = 0;
    bool s5bAutoPitch = true;
    int s5bAutoPitchOctave = 3;
    int s5bEnvelopePeriod = 1000;

    // FDS and N163: one or more waveforms stored end to end, each "waveSize" steps long, and how
    // many frames each one lasts. FDS waveforms hold 64 steps of 0 to 63, N163 ones 0 to 15.
    std::array<int8_t, maxWaveSteps> wave {};
    int waveLength = 0;
    int waveSize = 16;
    Envelope waveRepeat;

    // FDS only. The modulation table is 32 steps of -64 to 63.
    std::array<int8_t, 32> fdsModulation {};
    int fdsMasterVolume = 0;
    int fdsModSpeed = 0;
    int fdsModDepth = 0;
    int fdsModDelay = 0;
    bool fdsAutoMod = false;
    int fdsAutoModNumerator = 1;
    int fdsAutoModDenominator = 1;

    // N163 only: where the waveform sits in the chip's RAM.
    int n163WavePosition = 0;

    // VRC7 only. Patch 0 uses the 8 custom registers, and 1 to 15 are built into the chip.
    int vrc7Patch = 1;
    std::array<uint8_t, 8> vrc7PatchRegisters {};
    bool vrc7OverrideRelease = false;
    bool vrc7OverrideStop = false;

    // EPSM FM and rhythm: the chip registers of the patch, in FamiStudio's order. The EPSM square
    // channels share the S5B hardware envelope settings above, as they're the same circuit.
    std::array<uint8_t, 31> epsmPatchRegisters { 0x04, 0xc0, 0x00, 0x20, 0x1f, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x07, 0x00,
                                                 0x00, 0x20, 0x1f, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x07, 0x00, 0x00 };

    // Replaces the waveforms with one sine. FamiStudio starts new FDS and N163 sounds the same way.
    void setDefaultWave(int size, int maxValue)
    {
        waveSize = size;
        waveLength = size;
        waveRepeat = {};

        for (int i = 0; i < size; ++i)
            wave[(size_t) i] = defaultWaveStep(i, size, maxValue);
    }

    static int8_t defaultWaveStep(int index, int size, int maxValue)
    {
        return (int8_t) std::lround((std::sin(index * 6.283185307179586 / size) * 0.5 + 0.5) * maxValue);
    }

    bool hasReleaseEnvelope() const
    {
        for (const auto& envelope : envelopes)
            if (envelope.release >= 0)
                return true;

        return false;
    }
};
