#include "engine/ChannelEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace
{
constexpr int cyclesBetweenChannels = 120;
constexpr int regVrc6Pulse1Volume = 0x9000;
constexpr int regVrc6SawVolume = 0xb000;
constexpr int regVrc6SawLo = 0xb001;
constexpr int regVrc6SawHi = 0xb002;
constexpr int regMmc5Pulse1Volume = 0x5000;
constexpr int regS5bAddress = 0xc000;
constexpr int regS5bData = 0xf000;
constexpr int s5bRegLoA = 0x00, s5bRegHiA = 0x01, s5bRegNoiseFreq = 0x06, s5bRegMixer = 0x07,
              s5bRegVolA = 0x08, s5bRegEnvLo = 0x0b, s5bRegEnvHi = 0x0c, s5bRegShape = 0x0d;

constexpr int regFdsWaveStart = 0x4040, regFdsVolumeEnvelope = 0x4080, regFdsFreqLo = 0x4082, regFdsFreqHi = 0x4083, regFdsSweepEnvelope = 0x4084,
              regFdsSweepBias = 0x4085, regFdsModLo = 0x4086, regFdsModHi = 0x4087, regFdsModTable = 0x4088, regFdsVolume = 0x4089;
constexpr int regN163Address = 0xf800, regN163Data = 0x4800, n163RegFreqLo = 0x78, n163RegPhaseLo = 0x79, n163RegFreqMid = 0x7a,
              n163RegPhaseMid = 0x7b, n163RegFreqHi = 0x7c, n163RegPhaseHi = 0x7d, n163RegWave = 0x7e, n163RegVolume = 0x7f;
constexpr int regVrc7Select = 0x9010, regVrc7Write = 0x9030, vrc7RegLo = 0x10, vrc7RegHi = 0x20, vrc7RegVolume = 0x30;
constexpr int regEpsmAddress0 = 0x401c, regEpsmData0 = 0x401d, regEpsmAddress1 = 0x401e, regEpsmData1 = 0x401f;
constexpr int epsmRegRhythm = 0x10, epsmRegRhythmLevel = 0x18;
constexpr int epsmCycleAddressSkip = 4, epsmCycleDataSkip = 20, epsmCycleDataSkipShort = 10, epsmCycleKeyOnSkip = 36;

bool isInRange(ChannelType type, ChannelType first, ChannelType last)
{
    return (int) type >= (int) first && (int) type <= (int) last;
}

bool isN163(ChannelType type) { return isInRange(type, ChannelType::n163Wave1, ChannelType::n163Wave8); }
bool isVrc7(ChannelType type) { return isInRange(type, ChannelType::vrc7Fm1, ChannelType::vrc7Fm6); }
bool isEpsmSquare(ChannelType type) { return isInRange(type, ChannelType::epsmSquare1, ChannelType::epsmSquare3); }
constexpr std::array<int, 4> epsmOperatorOrder { 0, 2, 1, 3 };
constexpr std::array<int, 8> epsmAlgorithmMask { 0x8, 0x8, 0x8, 0x8, 0xc, 0xe, 0xe, 0xf };
constexpr int epsmSlowestFadingRelease = 2;

bool isEpsmFm(ChannelType type) { return isInRange(type, ChannelType::epsmFm1, ChannelType::epsmFm6); }
bool isEpsmRhythm(ChannelType type) { return isInRange(type, ChannelType::epsmRhythm1, ChannelType::epsmRhythm6); }

int maximumPeriodFor(ChannelType type)
{
    switch (type)
    {
        case ChannelType::vrc6Pulse1: case ChannelType::vrc6Pulse2: case ChannelType::vrc6Saw:
        case ChannelType::s5bSquare1: case ChannelType::s5bSquare2: case ChannelType::s5bSquare3:
        case ChannelType::fdsWave: case ChannelType::epsmSquare1: case ChannelType::epsmSquare2: case ChannelType::epsmSquare3:
            return 0xfff;
        default:
            return isN163(type) || isVrc7(type) || isEpsmFm(type) ? 0xffff : 0x7ff;
    }
}

NoteTable noteTableFor(ChannelType type, bool pal, int numN163Channels)
{
    if (type == ChannelType::vrc6Saw) return makeSawNoteTable(pal);
    if (type == ChannelType::fdsWave) return makeFdsNoteTable(pal);
    if (isN163(type)) return makeN163NoteTable(pal, numN163Channels);
    if (isVrc7(type)) return makeVrc7NoteTable();
    if (isEpsmSquare(type)) return makeEpsmSquareNoteTable();
    if (isEpsmFm(type)) return makeEpsmFmNoteTable();
    return makeNoteTable(pal);
}

// How far pitch values are scaled, from Channel.GetShiftsForType. Chips with fine pitch steps
// have large period values, so envelopes and slides are multiplied to reach as far.
int pitchShiftFor(ChannelType type, int numN163Channels)
{
    if (isVrc7(type) || isEpsmFm(type))
        return 3;

    if (isN163(type))
        return numN163Channels == 1 ? 2 : numN163Channels == 2 ? 3 : numN163Channels <= 4 ? 4 : 5;

    return 0;
}

int slideShiftFor(ChannelType type, int numN163Channels)
{
    const auto pitchShift = pitchShiftFor(type, numN163Channels);

    return pitchShift > 0 ? pitchShift : type == ChannelType::noise ? -4 : -1; // Most channels have a bit of fraction.
}

// The position of a channel among others like it.
int groupIndexFor(ChannelType type)
{
    if (isN163(type)) return (int) type - (int) ChannelType::n163Wave1;
    if (isVrc7(type)) return (int) type - (int) ChannelType::vrc7Fm1;
    if (isEpsmSquare(type)) return (int) type - (int) ChannelType::epsmSquare1;
    if (isEpsmFm(type)) return (int) type - (int) ChannelType::epsmFm1;
    if (isEpsmRhythm(type)) return (int) type - (int) ChannelType::epsmRhythm1;
    return 0;
}

bool sameEnvelope(const Envelope& a, const Envelope& b)
{
    if (a.length != b.length || a.loop != b.loop || a.release != b.release || a.relative != b.relative)
        return false;

    for (int i = 0; i < a.length; ++i)
        if (a.values[(size_t) i] != b.values[(size_t) i])
            return false;

    return true;
}

bool allEnvelopesIdentical(const Instrument& a, const Instrument& b)
{
    if (&a == &b)
        return true;

    for (size_t t = 0; t < numEnvelopeTypes; ++t)
        if (! sameEnvelope(a.envelopes[t], b.envelopes[t]))
            return false;

    // These are envelopes in FamiStudio too.
    return sameEnvelope(a.waveRepeat, b.waveRepeat) && a.waveLength == b.waveLength && a.wave == b.wave && a.fdsModulation == b.fdsModulation;
}

constexpr std::array<int, 8> fdsModulationDeltas { 0, 1, 2, 4, 0, -4, -2, -1 };

// Port of Envelope.BuildFdsModulationTable. The chip stores deltas rather than values.
std::array<int8_t, 32> buildFdsModulationTable(const std::array<int8_t, 32>& values)
{
    std::array<int8_t, 32> table {};
    auto previous = 0;

    table[0] = 4; // Force starting at zero.

    for (size_t i = 1; i < table.size(); ++i)
    {
        if (values[i] == 0)
        {
            table[i] = 4; // Reset to zero.
            previous = 0;
            continue;
        }

        auto smallestDifference = 99999;

        for (size_t j = 0; j < fdsModulationDeltas.size(); ++j)
        {
            if (j == 4)
                continue;

            const auto candidate = previous + fdsModulationDeltas[j] * 2; // The delta applies twice.
            const auto difference = std::abs(candidate - values[i]);

            if (difference < smallestDifference && candidate >= -64 && candidate <= 63)
            {
                smallestDifference = difference;
                table[i] = (int8_t) j;
            }
        }

        previous += fdsModulationDeltas[(size_t) table[i]] * 2;
    }

    return table;
}

constexpr int regFrameCounter = 0x4017;
constexpr int regPulseVolume = 0x4000;
constexpr int regPulseSweep = 0x4001;
constexpr int regPulseLo = 0x4002;
constexpr int regPulseHi = 0x4003;
constexpr int regTriangleLinear = 0x4008;
constexpr int regTriangleLo = 0x400a;
constexpr int regTriangleHi = 0x400b;
constexpr int regNoiseVolume = 0x400c;
constexpr int regNoiseLo = 0x400e;
constexpr int regDmcFreq = 0x4010;
constexpr int regDmcRaw = 0x4011;
constexpr int regDmcStart = 0x4012;
constexpr int regDmcLength = 0x4013;
constexpr int regStatus = 0x4015;

constexpr std::array<int, 13> vibratoSpeedLookup { 0, 64, 32, 21, 16, 13, 11, 9, 8, 7, 6, 5, 4 };
constexpr std::array<int, 16> vibratoDepthLookup { 0x01, 0x03, 0x05, 0x07, 0x09, 0x0d, 0x13, 0x17,
                                                   0x1b, 0x21, 0x2b, 0x3b, 0x57, 0x7f, 0xbf, 0xff };

// C#'s Math.Round rounds ties to even, and so does nearbyint in the default rounding mode.
int multiplyVolumes(int v0, int v1)
{
    const auto vol = (int) std::nearbyint((double) ((v0 / 15.0f) * (v1 / 15.0f) * 15.0f));
    return vol == 0 && v0 != 0 && v1 != 0 ? 1 : vol;
}
} // namespace

ChannelEngine::ChannelEngine(RegisterSink& s, ChannelType t, bool isPal, int n163Channels)
    : sink(s),
      type(t),
      noteTable(noteTableFor(t, isPal, n163Channels)),
      slideShift(slideShiftFor(t, n163Channels)),
      pitchShift(pitchShiftFor(t, n163Channels)),
      channelGroupIndex(groupIndexFor(t)),
      numN163Channels(n163Channels),
      maximumPeriod(maximumPeriodFor(t))
{
    note.value = noteStop;
    note.hasFinePitch = true;
}

void ChannelEngine::buildVibratoEnvelope(int speed, int depth)
{
    vibratoEnvelope = {};
    vibratoEnvelope.loop = 0;

    if (speed == 0 || depth == 0)
    {
        vibratoEnvelope.length = 127;
        return;
    }

    vibratoEnvelope.length = vibratoSpeedLookup[(size_t) speed];

    for (int i = 0; i < vibratoEnvelope.length; ++i)
    {
        const auto phase = (double) ((float) i * 2.0f) * M_PI / vibratoEnvelope.length;
        vibratoEnvelope.values[(size_t) i] = (int8_t) std::nearbyint(-std::sin(phase) * (vibratoDepthLookup[(size_t) depth] / 2));
    }
}

void ChannelEngine::playNote(ChannelNote newNote, int noteSlidePitch, int noteSlideStep, int noteVolumeSlideStep)
{
    // Pass on the same effect values if this note doesn't specify them.
    if (! newNote.hasFinePitch && note.hasFinePitch)
    {
        newNote.hasFinePitch = true;
        newNote.finePitch = note.finePitch;
    }

    if (newNote.instrument == nullptr && note.instrument != nullptr)
        newNote.instrument = note.instrument;

    if (newNote.arpeggio == nullptr && note.arpeggio != nullptr && ! newNote.isMusical())
        newNote.arpeggio = note.arpeggio;

    if (newNote.isValid())
    {
        // Any note that isn't a release cancels the current slide.
        if (! newNote.isRelease())
            slideStep = 0;

        if (newNote.slideTarget != 0)
        {
            slidePitch = noteSlidePitch;
            slideStep = noteSlideStep;
            newNote.value = newNote.slideTarget;
        }
    }

    if (newNote.hasVibrato)
    {
        if (newNote.vibratoDepth != 0)
        {
            buildVibratoEnvelope(newNote.vibratoSpeed, newNote.vibratoDepth);
            envelopes[envelopePitch] = &vibratoEnvelope;
            envelopeIdx[envelopePitch] = 0;
            envelopeValues[envelopePitch] = 0;
            pitchEnvelopeOverride = true;
        }
        else
        {
            envelopes[envelopePitch] = nullptr;
            pitchEnvelopeOverride = false;
            resetInstrumentOnNextAttack = true;
        }
    }

    if (newNote.isStop())
    {
        note = newNote;
    }
    else if (newNote.isRelease())
    {
        // Jump to the release point.
        if (note.instrument != nullptr)
            for (size_t j = 0; j < numEnvelopeTypes; ++j)
                if (envelopes[j] != nullptr && envelopes[j]->release >= 0)
                    envelopeIdx[j] = envelopes[j]->release;

        if (note.instrument != nullptr && waveInstrument != nullptr && waveInstrument->waveRepeat.release >= 0)
        {
            waveRepeatIndex = waveInstrument->waveRepeat.release;
            waveRepeatCounter = waveInstrument->waveRepeat.values[(size_t) waveRepeatIndex] + 1;
        }

        noteReleased = true;
        newNote.value = note.value;
        note = newNote;
    }
    else if (newNote.isMusical())
    {
        auto instrumentChanged = note.instrument != newNote.instrument || forceInstrumentReload;
        const auto arpeggioChanged = note.arpeggio != newNote.arpeggio;

        // Attack can only be disabled when the envelopes match, as envelope positions carry over.
        const auto noteHasAttack = newNote.hasAttack || note.instrument == nullptr || newNote.instrument == nullptr
                                 || (isN163(type) && note.instrument != newNote.instrument)
                                 || ! allEnvelopesIdentical(*note.instrument, *newNote.instrument);

        note = newNote;

        if (arpeggioChanged)
        {
            if (note.arpeggio != nullptr)
            {
                envelopes[envelopeArpeggio] = note.arpeggio;
                arpeggioEnvelopeOverride = true;
            }
            else
            {
                envelopes[envelopeArpeggio] = note.instrument != nullptr ? &note.instrument->envelopes[envelopeArpeggio] : nullptr;
                arpeggioEnvelopeOverride = false;
            }

            envelopeIdx[envelopeArpeggio] = 0;
            envelopeValues[envelopeArpeggio] = 0;
        }
        else if (noteHasAttack && arpeggioEnvelopeOverride)
        {
            envelopeIdx[envelopeArpeggio] = 0;
            envelopeValues[envelopeArpeggio] = 0;
        }

        // DPCM should always retrigger. Attack only affects the initial DMC value.
        if (type == ChannelType::dpcm)
        {
            noteTriggered = true;
            resetInstrumentOnNextAttack = false;
        }
        else if (noteHasAttack)
        {
            instrumentChanged = instrumentChanged || resetInstrumentOnNextAttack;

            if (instrumentChanged)
            {
                for (size_t j = 0; j < numEnvelopeTypes; ++j)
                {
                    if ((j != envelopePitch || ! pitchEnvelopeOverride) && (j != envelopeArpeggio || ! arpeggioEnvelopeOverride))
                        envelopes[j] = note.instrument != nullptr ? &note.instrument->envelopes[j] : nullptr;
                }

                waveInstrument = note.instrument;
            }

            envelopeIdx.fill(0);
            envelopeValues[envelopeDutyCycle] = dutyCycle;
            envelopeValues[envelopePitch] = 0; // In case we use relative envelopes.

            // A note-off and a new note can land together from MIDI, and the attack has to win.
            noteReleased = false;

            waveRepeatIndex = 0;

            if (waveInstrument != nullptr)
                waveRepeatCounter = waveInstrument->waveRepeat.values[0] + 1;

            noteTriggered = true;
            resetInstrumentOnNextAttack = false;
        }

        if (instrumentChanged)
        {
            loadInstrument(note.instrument);
            forceInstrumentReload = false;
        }
    }
    else
    {
        // Empty notes just keep whatever value we had.
        newNote.value = note.value;
        note = newNote;
    }

    if (! note.hasFinePitch)
    {
        note.hasFinePitch = true;
        note.finePitch = 0;
    }

    if (note.hasDutyCycle)
    {
        dutyCycle = note.dutyCycle;
        envelopeValues[envelopeDutyCycle] = dutyCycle;
    }

    if (note.hasVolume)
    {
        volume = note.volume << 4;
        volumeSlideStep = 0;
    }

    if (note.hasVolumeSlide)
    {
        volumeSlideTarget = note.volumeSlideTarget << 4;
        volumeSlideStep = noteVolumeSlideStep;
    }

    if (note.hasPhaseReset)
        resetPhase = true;
}

void ChannelEngine::loadInstrument(const Instrument* instrument)
{
    if (instrument == nullptr)
        return;

    if (type == ChannelType::fdsWave)
        loadFdsInstrument(*instrument);
    else if (isN163(type))
        loadN163Instrument(*instrument);
    else if (isVrc7(type))
        loadVrc7Instrument(*instrument);
    else if (isEpsmFm(type))
    {
        epsmStereoFlags = instrument->epsmPatchRegisters[1];
        epsmAlgorithm = instrument->epsmPatchRegisters[0];
        epsmPendingPatch = instrument->epsmPatchRegisters.data();
    }
    else if (isEpsmRhythm(type))
        epsmStereoFlags = instrument->epsmPatchRegisters[1] & 0xc0;

    if (instrument->s5bEnvelopeShape > 0)
    {
        instEnvShape = instrument->s5bEnvelopeShape + 7; // 1 to 8 maps to 0x8 to 0xf.
        instAutoPitch = instrument->s5bAutoPitch;
        instAutoOctave = instrument->s5bAutoPitchOctave;
        instEnvPeriod = instrument->s5bEnvelopePeriod;
    }
    else
    {
        instEnvShape = 0;
    }
}

void ChannelEngine::setS5bGroup(S5bSharedState* shared, bool isFirst, bool isLast)
{
    s5b = shared;
    s5bIsFirst = isFirst;
    s5bIsLast = isLast;
}

void ChannelEngine::setVrc7Group(ChannelEngine* const* channels, int count)
{
    vrc7Group = channels;
    vrc7GroupSize = count;
}

void ChannelEngine::update()
{
    updateEnvelopes();
    updateWaveRepeat();
    updateSlide();
    updateVolumeSlide();
    updateApu();
}

void ChannelEngine::updateEnvelopes()
{
    if (note.instrument == nullptr)
        return;

    for (size_t j = 0; j < numEnvelopeTypes; ++j)
    {
        const auto* env = envelopes[j];

        playedSteps[j] = -1;

        if (env == nullptr || env->isEmpty((int) j))
        {
            if (j != envelopeDutyCycle)
                envelopeValues[j] = Envelope::defaultValue((int) j);
            continue;
        }

        auto idx = envelopeIdx[j];

        // Non-looping relative envelopes end up at -1 when done.
        if (idx < 0)
            continue;

        playedSteps[j] = idx;

        if (env->relative)
            envelopeValues[j] += env->values[(size_t) idx];
        else
            envelopeValues[j] = env->values[(size_t) idx];

        ++idx;

        if (env->release >= 0 && idx == env->release)
            envelopeIdx[j] = env->loop;
        else if (idx >= env->length)
            envelopeIdx[j] = env->loop >= 0 && env->release < 0 ? env->loop : (env->relative ? -1 : env->length - 1);
        else
            envelopeIdx[j] = idx;
    }
}

// Waveforms count frames instead of holding values, so they can last different lengths of time.
void ChannelEngine::updateWaveRepeat()
{
    if (note.instrument == nullptr || waveInstrument == nullptr)
        return;

    const auto& env = waveInstrument->waveRepeat;

    if (env.length == 0 || env.isEmpty(envelopeArpeggio) || --waveRepeatCounter != 0)
        return;

    const auto next = waveRepeatIndex + 1;

    if (env.release >= 0 && next == env.release)
        waveRepeatIndex = env.loop;
    else if (next >= env.length)
        waveRepeatIndex = env.loop >= 0 && env.release < 0 ? env.loop : env.length - 1;
    else
        waveRepeatIndex = next;

    waveRepeatCounter = env.values[(size_t) waveRepeatIndex];
}

void ChannelEngine::updateSlide()
{
    if (slideStep != 0)
    {
        slidePitch += slideStep;

        if ((slideStep > 0 && slidePitch > 0) || (slideStep < 0 && slidePitch < 0))
        {
            slidePitch = 0;
            slideStep = 0;
        }
    }
    else
    {
        slidePitch = 0;
    }
}

void ChannelEngine::updateVolumeSlide()
{
    if (volumeSlideStep != 0)
    {
        volume += volumeSlideStep;

        if ((volumeSlideStep > 0 && volume >= volumeSlideTarget) || (volumeSlideStep < 0 && volume <= volumeSlideTarget))
        {
            volume = volumeSlideTarget;
            volumeSlideStep = 0;
        }
    }
}

int ChannelEngine::slideOffset() const
{
    return slideShift < 0 ? (slidePitch >> -slideShift) : (slidePitch << slideShift); // Remove the fraction part.
}

int ChannelEngine::getPeriod() const
{
    const auto noteValue = std::clamp(note.value + envelopeValues[envelopeArpeggio], 0, (int) noteTable.size() - 1);
    const auto pitch = (note.finePitch + envelopeValues[envelopePitch]) * (1 << pitchShift);

    return std::clamp((int) noteTable[(size_t) noteValue] + pitch + slideOffset() + bendPeriodOffset, 0, maximumPeriod);
}

int ChannelEngine::getVolume() const
{
    return multiplyVolumes(volume >> 4, envelopeValues[envelopeVolume]);
}

void ChannelEngine::updateApu()
{
    switch (type)
    {
        case ChannelType::pulse1:
        case ChannelType::pulse2: updatePulse(); break;
        case ChannelType::triangle: updateTriangle(); break;
        case ChannelType::noise: updateNoise(); break;
        case ChannelType::dpcm: updateDpcm(); break;
        case ChannelType::vrc6Pulse1:
        case ChannelType::vrc6Pulse2: updateVrc6Pulse(); break;
        case ChannelType::vrc6Saw: updateVrc6Saw(); break;
        case ChannelType::mmc5Pulse1:
        case ChannelType::mmc5Pulse2: updateMmc5Pulse(); break;
        case ChannelType::s5bSquare1:
        case ChannelType::s5bSquare2:
        case ChannelType::s5bSquare3: updateS5b(); break;
        case ChannelType::fdsWave: updateFds(); break;
        default:
            if (isN163(type)) updateN163();
            else if (isVrc7(type)) updateVrc7();
            else if (isEpsmSquare(type)) updateEpsmSquare();
            else if (isEpsmFm(type)) updateEpsmFm();
            else if (isEpsmRhythm(type)) updateEpsmRhythm();
            break;
    }

    noteTriggered = false;
    noteReleased = false;
    sink.skipCycles(cyclesBetweenChannels);
}

void ChannelEngine::updatePulse()
{
    const auto regOffset = type == ChannelType::pulse2 ? 4 : 0;
    const auto duty = envelopeValues[envelopeDutyCycle] & 3;

    if (note.isStop())
    {
        sink.writeRegister(regPulseVolume + regOffset, (duty << 6) | 0x30);
    }
    else if (note.isMusical())
    {
        const auto period = getPeriod();
        const auto periodHi = (period >> 8) & 0x07;
        const auto periodLo = period & 0xff;
        const auto deltaHi = periodHi - prevPeriodHi;

        // Write low first, this matches the NSF driver.
        sink.writeRegister(regPulseLo + regOffset, periodLo);

        if (deltaHi != 0) // Avoid resetting the sequence.
        {
            if (std::abs(deltaHi) == 1)
            {
                // Blargg's smooth vibrato technique, using the sweep to avoid resetting the phase.
                // http://forums.nesdev.com/viewtopic.php?t=231
                sink.writeRegister(regFrameCounter, 0x40); // reset frame counter in case it was about to clock
                sink.writeRegister(regPulseLo + regOffset, deltaHi < 0 ? 0x00 : 0xff); // low 8 bits of timer period are $FF ($00 when negative)
                sink.writeRegister(regPulseSweep + regOffset, deltaHi < 0 ? 0x8f : 0x87); // sweep enabled, shift = 7, set negative flag
                sink.writeRegister(regFrameCounter, 0xc0); // clock sweep immediately
                sink.writeRegister(regPulseSweep + regOffset, 0x08); // disable sweep
                sink.writeRegister(regPulseLo + regOffset, periodLo); // restore correct low
            }
            else
            {
                sink.writeRegister(regPulseHi + regOffset, periodHi);
            }

            prevPeriodHi = periodHi;
        }

        sink.writeRegister(regPulseVolume + regOffset, (duty << 6) | 0x30 | getVolume());
    }
}

void ChannelEngine::updateTriangle()
{
    if (note.isStop())
    {
        sink.writeRegister(regTriangleLinear, 0x80);
    }
    else if (note.isMusical())
    {
        const auto period = getPeriod();

        sink.writeRegister(regTriangleLo, period & 0xff);
        sink.writeRegister(regTriangleHi, (period >> 8) & 0x07);
        sink.writeRegister(regTriangleLinear, 0x80 | getVolume());
    }
}

void ChannelEngine::updateNoise()
{
    if (note.isStop())
    {
        sink.writeRegister(regNoiseVolume, 0xf0);
    }
    else if (note.isMusical())
    {
        const auto duty = envelopeValues[envelopeDutyCycle];
        const auto period = (((note.value + slideOffset() + envelopeValues[envelopeArpeggio]) & 0x0f) ^ 0x0f) | ((duty << 7) & 0x80);

        sink.writeRegister(regNoiseLo, period);
        sink.writeRegister(regNoiseVolume, 0x30 | getVolume());
    }
}

void ChannelEngine::updateVrc6Pulse()
{
    const auto regOffset = type == ChannelType::vrc6Pulse2 ? 0x1000 : 0;
    const auto duty = envelopeValues[envelopeDutyCycle] & 7;

    if (note.isStop())
    {
        sink.writeRegister(regVrc6Pulse1Volume + regOffset, duty << 4);
    }
    else if (note.isMusical())
    {
        const auto period = getPeriod();
        const auto periodHi = (period >> 8) & 0x0f;

        periodHiForReset = periodHi;

        sink.writeRegister(regVrc6Pulse1Volume + 1 + regOffset, period & 0xff);
        sink.writeRegister(regVrc6Pulse1Volume + 2 + regOffset, periodHi | 0x80);
        sink.writeRegister(regVrc6Pulse1Volume + regOffset, (duty << 4) | getVolume());
    }
}

void ChannelEngine::updateVrc6Saw()
{
    if (note.isStop())
    {
        sink.writeRegister(regVrc6SawVolume, 0x00);
    }
    else if (note.isMusical())
    {
        const auto period = getPeriod();
        const auto periodHi = (period >> 8) & 0x0f;
        const auto masterVolume = note.instrument != nullptr ? note.instrument->sawMasterVolume : 0;

        periodHiForReset = periodHi;

        sink.writeRegister(regVrc6SawLo, period & 0xff);
        sink.writeRegister(regVrc6SawHi, periodHi | 0x80);
        sink.writeRegister(regVrc6SawVolume, getVolume() << (2 - masterVolume));
    }
}

void ChannelEngine::updateMmc5Pulse()
{
    const auto regOffset = type == ChannelType::mmc5Pulse2 ? 4 : 0;
    const auto duty = envelopeValues[envelopeDutyCycle] & 3;

    if (note.isStop())
    {
        sink.writeRegister(regMmc5Pulse1Volume + regOffset, (duty << 6) | 0x30);
    }
    else if (note.isMusical())
    {
        const auto period = getPeriod();
        const auto periodHi = (period >> 8) & 0x07;

        // Write low first to match the NSF driver.
        sink.writeRegister(regMmc5Pulse1Volume + 2 + regOffset, period & 0xff);

        if (periodHi != prevPeriodHi) // Avoid resetting the sequence.
        {
            sink.writeRegister(regMmc5Pulse1Volume + 3 + regOffset, periodHi);
            prevPeriodHi = periodHi;
        }

        sink.writeRegister(regMmc5Pulse1Volume + regOffset, (duty << 6) | 0x30 | getVolume());
    }
}

void ChannelEngine::updateS5b()
{
    const auto channelIndex = type == ChannelType::s5bSquare3 ? 2 : type == ChannelType::s5bSquare2 ? 1 : 0;
    const auto invToneMask = 0xff - (9 << channelIndex);

    const auto writeS5b = [this](int reg, int data) {
        sink.writeRegister(regS5bAddress, reg);
        sink.writeRegister(regS5bData, data);
    };

    if (s5bIsFirst)
    {
        s5b->envAutoPitch = false;
        s5b->envReset = false;
        s5b->noiseFreq = 0;
        s5b->envPeriodEffect = false;
    }

    s5b->envReset = s5b->envReset || (noteTriggered && instEnvShape > 0);

    if (note.hasEnvelopePeriod)
    {
        s5b->envPeriod = note.envelopePeriod;
        s5b->envPeriodEffect = true;
    }

    if (note.isStop())
    {
        writeS5b(s5bRegVolA + channelIndex, 0);
    }
    else if (note.isMusical())
    {
        // Unlike the 2A03 and VRC6 pulse channels, the 5B's frequency formula doesn't add 1 to the period.
        const auto period = getPeriod() + 1;

        writeS5b(s5bRegLoA + channelIndex * 2, period & 0xff);
        writeS5b(s5bRegHiA + channelIndex * 2, (period >> 8) & 0x0f);
        writeS5b(s5bRegVolA + channelIndex, getVolume() | (instEnvShape != 0 ? 0x10 : 0x00));

        const auto mixer = envelopeValues[envelopeS5bMixer];
        const auto noiseEnabled = (mixer & 2) == 0;

        s5b->toneReg = (s5b->toneReg & invToneMask) | (((mixer & 1) | ((mixer & 2) << 2)) << channelIndex);

        if (noiseEnabled)
            s5b->noiseFreq = envelopeValues[envelopeS5bNoise];

        if (instEnvShape != 0)
        {
            s5b->envPeriod = instAutoPitch ? period : (s5b->envPeriodEffect || ! noteTriggered ? s5b->envPeriod : instEnvPeriod);
            s5b->envShape = instEnvShape;
            s5b->envAutoOctave = instAutoOctave;
            s5b->envAutoPitch = instAutoPitch;
        }
    }

    // The last channel writes the shared registers.
    if (s5bIsLast)
    {
        if (s5b->envAutoPitch)
        {
            if (s5b->envAutoOctave > 0)
            {
                s5b->envPeriod >>= std::abs(s5b->envAutoOctave) - 1;

                if ((s5b->envPeriod & 1) != 0)
                    ++s5b->envPeriod;

                s5b->envPeriod >>= 1;
            }
            else
            {
                s5b->envPeriod <<= std::abs(s5b->envAutoOctave);
            }
        }

        writeS5b(s5bRegEnvLo, s5b->envPeriod & 0xff);
        writeS5b(s5bRegEnvHi, (s5b->envPeriod >> 8) & 0xff);
        writeS5b(s5bRegNoiseFreq, s5b->noiseFreq);
        writeS5b(s5bRegMixer, s5b->toneReg);

        if (s5b->envReset)
            writeS5b(s5bRegShape, s5b->envShape);
    }
}

void ChannelEngine::updateDpcm()
{
    auto wroteCounter = false;

    if (note.isStop())
    {
        sink.writeRegister(regStatus, 0x0f);
    }
    else if (note.isMusical() && noteTriggered)
    {
        sink.writeRegister(regStatus, 0x0f);

        const auto* mapping = dpcmKit != nullptr && note.value <= noteMusicalMax ? &dpcmKit->mappings[(size_t) note.value] : nullptr;

        if (mapping != nullptr && mapping->sample != nullptr && ! mapping->sample->data.empty())
        {
            const auto useInitialValue = note.hasAttack || note.hasDeltaCounter;
            auto initialValue = 0;

            if (useInitialValue)
            {
                initialValue = (mapping->overrideInitialValue ? mapping->initialValueDiv2 : mapping->sample->initialValueDiv2) * 2;

                if (note.hasDeltaCounter)
                {
                    initialValue = note.deltaCounter;
                    note.hasDeltaCounter = false; // Clear so it isn't set multiple times.
                }
            }

            sink.selectDpcmSample(mapping->sample.get());
            sink.writeRegister(regDmcStart, 0);
            sink.writeRegister(regDmcLength, (int) mapping->sample->data.size() >> 4);
            sink.writeRegister(regDmcFreq, mapping->pitch | (mapping->loop ? 0x40 : 0x00));

            if (useInitialValue)
                sink.writeRegister(regDmcRaw, initialValue);

            sink.writeRegister(regStatus, 0x1f);
            wroteCounter = true;
        }
    }

    if (note.hasDeltaCounter && ! wroteCounter)
    {
        sink.writeRegister(regDmcRaw, note.deltaCounter);
        note.hasDeltaCounter = false;
    }
}

// Writes take 4 cycles in the emulator, and some loops in the sound engine take longer.
void ChannelEngine::writeWithSkip(int reg, int data, int skip)
{
    sink.writeRegister(reg, data);

    if (skip > 4)
        sink.skipCycles(skip - 4);
}

// Port of ChannelStateFds.
void ChannelEngine::loadFdsInstrument(const Instrument& instrument)
{
    const auto table = buildFdsModulationTable(instrument.fdsModulation);

    loadedWaveIndex = -1; // Force a reload on instrument change.
    fdsMasterVolume = instrument.fdsMasterVolume;
    conditionalLoadFdsWave();

    // Only write the table if it has actually changed.
    if (table != fdsPreviousModTable)
    {
        sink.writeRegister(regFdsModHi, 0x80);
        sink.writeRegister(regFdsSweepBias, 0x00);

        for (const auto value : table)
            writeWithSkip(regFdsModTable, value & 0xff, 16); // Mimics the sound engine's loop.
    }

    fdsPreviousModTable = table;
}

void ChannelEngine::conditionalLoadFdsWave()
{
    if (waveRepeatIndex == loadedWaveIndex || waveInstrument == nullptr)
        return;

    // A sound with no waveform yet still has to make a sound.
    std::array<int8_t, 64> defaultWave;

    // A sound made for another chip can have waves of the wrong size, so it counts as having none.
    const auto hasWave = waveInstrument->waveLength >= (waveRepeatIndex + 1) * 64 && waveInstrument->waveSize == 64;

    if (! hasWave)
        for (int i = 0; i < 64; ++i)
            defaultWave[(size_t) i] = Instrument::defaultWaveStep(i, 64, 63);

    const auto* wave = hasWave ? waveInstrument->wave.data() + waveRepeatIndex * 64 : defaultWave.data();

    // The table is written from end to start to mimic the sound engine, which saves cycles.
    sink.skipCycles(2); // LDY

    for (int i = 0x3f; i >= 0; i -= 2)
    {
        sink.skipCycles(3); // Read volume.
        writeWithSkip(regFdsVolume, 0x80 | fdsMasterVolume, 4);
        writeWithSkip(regFdsWaveStart + i, wave[i] & 0xff, 12);
        writeWithSkip(regFdsWaveStart + i - 1, wave[i - 1] & 0xff, 10);
        writeWithSkip(regFdsVolume, fdsMasterVolume, i > 1 ? 9 : 8);
    }

    loadedWaveIndex = waveRepeatIndex;
}

void ChannelEngine::resetFdsModulation()
{
    sink.writeRegister(regFdsModLo, 0x00);
    sink.writeRegister(regFdsSweepBias, 0x00);
    sink.writeRegister(regFdsModHi, 0x80);
    sink.writeRegister(regFdsSweepEnvelope, 0x80);
}

void ChannelEngine::updateFds()
{
    const auto hasDepthEffect = note.hasFdsModDepth;
    const auto hasSpeedEffect = note.hasFdsModSpeed;

    if (hasDepthEffect)
        fdsModDepth = note.fdsModDepth;

    if (hasSpeedEffect)
        fdsModSpeed = note.fdsModSpeed;

    if (note.isStop())
    {
        sink.writeRegister(regFdsVolumeEnvelope, 0x80); // Zero volume.
        resetFdsModulation();
    }
    else if (note.isMusical())
    {
        conditionalLoadFdsWave();

        const auto period = getPeriod();
        const auto* instrument = note.instrument;

        periodHiForReset = (period >> 8) & 0x0f;

        sink.writeRegister(regFdsFreqHi, periodHiForReset);
        sink.writeRegister(regFdsFreqLo, period & 0xff);
        sink.writeRegister(regFdsVolumeEnvelope, 0x80 | (getVolume() << 1));

        if (noteTriggered)
        {
            if (instrument != nullptr)
            {
                fdsModDelayCounter = instrument->fdsModDelay;

                // An effect on this frame takes over from the instrument.
                if (! hasDepthEffect)
                    fdsModDepth = instrument->fdsModDepth;

                if (! hasSpeedEffect)
                    fdsModSpeed = instrument->fdsModSpeed;
            }
            else
            {
                fdsModDelayCounter = 0;
            }
        }

        if (fdsModDelayCounter == 0)
        {
            const auto finalSpeed = instrument != nullptr && instrument->fdsAutoMod && fdsModDepth > 0
                                  ? std::min(period * instrument->fdsAutoModNumerator / instrument->fdsAutoModDenominator, 0xfff)
                                  : fdsModSpeed;

            sink.writeRegister(regFdsModHi, (finalSpeed >> 8) & 0xff);
            sink.writeRegister(regFdsModLo, finalSpeed & 0xff);
            sink.writeRegister(regFdsSweepEnvelope, 0x80 | fdsModDepth);
        }
        else
        {
            resetFdsModulation();
            --fdsModDelayCounter;
        }
    }
}

// Port of ChannelStateN163.
void ChannelEngine::writeN163(int reg, int data, int skip)
{
    sink.writeRegister(regN163Address, reg);
    writeWithSkip(regN163Data, data, skip);
}

void ChannelEngine::loadN163Instrument(const Instrument& instrument)
{
    loadedWaveIndex = -1;
    n163WaveLengthRegister = 256 - instrument.waveSize;
    n163WavePosition = instrument.n163WavePosition / 2;

    writeN163(n163RegWave - 8 * channelGroupIndex, n163WavePosition * 2);
}

void ChannelEngine::conditionalLoadN163Wave()
{
    if (waveRepeatIndex == loadedWaveIndex)
        return;

    if (waveInstrument != nullptr)
    {
        const auto size = std::clamp(waveInstrument->waveSize & ~3, 4, 64);

        // A sound with no waveform yet still has to make a sound.
        std::array<int8_t, 64> defaultWave;
        const auto hasWave = waveInstrument->waveLength >= (waveRepeatIndex + 1) * size;

        if (! hasWave)
            for (int i = 0; i < size; ++i)
                defaultWave[(size_t) i] = Instrument::defaultWaveStep(i, size, 15);

        const auto* wave = hasWave ? waveInstrument->wave.data() + waveRepeatIndex * size : defaultWave.data();

        // The chip packs two 4-bit steps into each byte.
        for (int i = 0; i < size; i += 2)
            writeN163(n163WavePosition + i / 2, ((wave[i + 1] << 4) | (wave[i] & 0x0f)) & 0xff, 18); // Mimics the sound engine's loop.
    }

    loadedWaveIndex = waveRepeatIndex;
}

void ChannelEngine::updateN163()
{
    const auto regOffset = -8 * channelGroupIndex;
    const auto channelMask = (numN163Channels - 1) << 4;

    if (note.isStop())
    {
        writeN163(n163RegVolume + regOffset, channelMask);
    }
    else if (note.isMusical())
    {
        conditionalLoadN163Wave();

        const auto period = getPeriod() << 2; // Periods are clamped to 16 bits for convenience, and the chip has 18.

        writeN163(n163RegFreqLo + regOffset, period & 0xff);
        writeN163(n163RegFreqMid + regOffset, (period >> 8) & 0xff);
        writeN163(n163RegFreqHi + regOffset, n163WaveLengthRegister | ((period >> 16) & 0x03));
        writeN163(n163RegVolume + regOffset, channelMask | getVolume());
    }
}

// Port of ChannelStateVrc7.
void ChannelEngine::writeVrc7(int reg, int data)
{
    writeWithSkip(regVrc7Select, reg, 16);
    writeWithSkip(regVrc7Write, data, 84);
}

void ChannelEngine::loadVrc7Instrument(const Instrument& instrument)
{
    if (instrument.vrc7Patch == 0)
    {
        // Other channels on custom patches will need to load theirs again.
        for (int i = 0; i < vrc7GroupSize; ++i)
        {
            auto& other = *vrc7Group[i];

            if (other.note.instrument != nullptr && other.note.instrument != &instrument && other.note.instrument->vrc7Patch == 0)
                other.forceInstrumentReload = true;
        }

        for (size_t i = 0; i < instrument.vrc7PatchRegisters.size(); ++i)
            writeVrc7((int) i, instrument.vrc7PatchRegisters[i]);
    }

    vrc7InstrumentBits = instrument.vrc7Patch << 4;
}

void ChannelEngine::updateVrc7()
{
    if (note.isStop())
    {
        vrc7PreviousPeriodHi = (vrc7PreviousPeriodHi & ~0x30 & 0xff) | (vrc7OverrideStop ? 0x20 : 0x00);
        writeVrc7(vrc7RegHi + channelGroupIndex, vrc7PreviousPeriodHi);
    }
    else if (note.isMusical())
    {
        if (noteReleased)
        {
            vrc7PreviousPeriodHi &= ~0x10 & 0xff;
        }
        else if (noteTriggered)
        {
            if ((vrc7PreviousPeriodHi & 0x10) != 0)
                writeVrc7(vrc7RegHi + channelGroupIndex, 0);
            else
                vrc7PreviousPeriodHi |= 0x10;
        }

        auto period = getPeriod();
        auto octave = 0;

        while (period >= 0x200)
        {
            period >>= 1;
            ++octave;
        }

        auto sustain = 0;

        if (const auto* instrument = note.instrument)
        {
            vrc7OverrideStop = instrument->vrc7OverrideStop;

            if (instrument->vrc7OverrideRelease)
                sustain = 0x20;
        }

        // Bit 0x10 of the high period starts a note going from 0 to 1, and releases it going back.
        const auto periodHi = sustain | (vrc7PreviousPeriodHi & 0x10) | ((octave & 0x7) << 1) | ((period >> 8) & 1);

        writeVrc7(vrc7RegVolume + channelGroupIndex, vrc7InstrumentBits | (15 - getVolume()));
        writeVrc7(vrc7RegLo + channelGroupIndex, period & 0xff);
        writeVrc7(vrc7RegHi + channelGroupIndex, periodHi); // This triggers the note, so it goes last.

        vrc7PreviousPeriodHi = periodHi;
    }
}

// Port of ChannelStateEPSMSquare. Unlike the 5B, the period takes no extra step.
void ChannelEngine::updateEpsmSquare()
{
    const auto invToneMask = 0xff - (9 << channelGroupIndex);

    const auto writeSquare = [this](int reg, int data) {
        sink.writeRegister(regEpsmAddress0, reg);
        sink.writeRegister(regEpsmData0, data);
    };

    if (s5bIsFirst)
    {
        s5b->envAutoPitch = false;
        s5b->envReset = false;
        s5b->noiseFreq = 0;
        s5b->envPeriodEffect = false;
    }

    s5b->envReset = s5b->envReset || (noteTriggered && instEnvShape > 0);

    if (note.hasEnvelopePeriod)
    {
        s5b->envPeriod = note.envelopePeriod;
        s5b->envPeriodEffect = true;
    }

    if (note.isStop())
    {
        writeSquare(s5bRegVolA + channelGroupIndex, 0);
    }
    else if (note.isMusical())
    {
        const auto period = getPeriod();

        writeSquare(s5bRegLoA + channelGroupIndex * 2, period & 0xff);
        writeSquare(s5bRegHiA + channelGroupIndex * 2, (period >> 8) & 0x0f);
        writeSquare(s5bRegVolA + channelGroupIndex, getVolume() | (instEnvShape != 0 ? 0x10 : 0x00));

        const auto mixer = envelopeValues[envelopeS5bMixer];

        s5b->toneReg = (s5b->toneReg & invToneMask) | (((mixer & 1) | ((mixer & 2) << 2)) << channelGroupIndex);

        if ((mixer & 2) == 0)
            s5b->noiseFreq = envelopeValues[envelopeS5bNoise];

        if (instEnvShape != 0)
        {
            s5b->envPeriod = instAutoPitch ? period : (s5b->envPeriodEffect || ! noteTriggered ? s5b->envPeriod : instEnvPeriod);
            s5b->envShape = instEnvShape;
            s5b->envAutoOctave = instAutoOctave;
            s5b->envAutoPitch = instAutoPitch;
        }
    }

    if (s5bIsLast)
    {
        if (s5b->envAutoPitch)
        {
            if (s5b->envAutoOctave > 0)
            {
                s5b->envPeriod >>= std::abs(s5b->envAutoOctave) - 1;

                if ((s5b->envPeriod & 1) != 0)
                    ++s5b->envPeriod;

                s5b->envPeriod >>= 1;
            }
            else
            {
                s5b->envPeriod <<= std::abs(s5b->envAutoOctave);
            }
        }

        writeSquare(s5bRegEnvLo, s5b->envPeriod & 0xff);
        writeSquare(s5bRegEnvHi, (s5b->envPeriod >> 8) & 0xff);
        writeSquare(s5bRegNoiseFreq, s5b->noiseFreq);
        writeSquare(s5bRegMixer, s5b->toneReg);

        if (s5b->envReset)
            writeSquare(s5bRegShape, s5b->envShape);
    }
}

// Port of ChannelStateEPSMFm and its base. Some registers need more time, going by the OPN2 code.
void ChannelEngine::writeEpsm(int reg, int data, bool highBank, int extraCycles)
{
    const auto dataSkip = (reg & 0xf0) < 0x30 || (reg & 0xf0) > 0x90 ? epsmCycleDataSkipShort : epsmCycleDataSkip;

    writeWithSkip(highBank ? regEpsmAddress1 : regEpsmAddress0, reg, epsmCycleAddressSkip);
    writeWithSkip(highBank ? regEpsmData1 : regEpsmData0, data, dataSkip + extraCycles);
}

void ChannelEngine::updateEpsmFm()
{
    const auto highBank = channelGroupIndex >= 3;
    const auto index = highBank ? channelGroupIndex - 3 : channelGroupIndex;
    const auto key = highBank ? (0x4 | index) : index;

    if (note.isStop() && ! epsmStopped)
    {
        writeEpsm(0x28, key);
        writeEpsm(0xb4 + index, 0x00, highBank); // Volume 0.
        epsmStopped = true;
    }
    else if (note.isMusical())
    {
        epsmStopped = false;

        auto period = getPeriod();
        auto octave = 0;

        while (period >= 0x200)
        {
            period >>= 1;
            ++octave;
        }

        const auto periodLo = (period << 2) & 0xff;
        const auto periodHi = ((octave & 0x7) << 3) | ((period >> 6) & 7);

        // A logarithmic volume adjustment.
        constexpr int steps = 16, adjustment = 5;
        const auto step = (std::log(127.0 + adjustment) - std::log((double) adjustment)) / (steps - 1);
        const auto volumeOffset = (int) (std::exp(std::log((double) adjustment) + (15 - getVolume()) * step) - adjustment);

        if (epsmPendingPatch != nullptr || epsmLastVolume != volumeOffset)
        {
            // The patch loads here, rather than along with the instrument, to avoid chirps.
            if (const auto* patch = epsmPendingPatch)
            {
                writeEpsm(0xb0 + index, patch[0], highBank);
                writeEpsm(0xb4 + index, patch[1], highBank);
                writeEpsm(0x22, patch[30]); // The LFO applies to all channels.

                for (size_t y = 0; y < 4; ++y)
                {
                    const auto op = index + epsmOperatorOrder[y] * 4;
                    const auto* regs = patch + y * 7;

                    epsmOperatorVolumes[(size_t) epsmOperatorOrder[y]] = regs[3];
                    writeEpsm(0x30 + op, regs[2], highBank);
                    writeEpsm(0x50 + op, regs[4], highBank);
                    writeEpsm(0x60 + op, regs[5], highBank);
                    writeEpsm(0x70 + op, regs[6], highBank);
                    writeEpsm(0x80 + op, regs[7], highBank);
                    writeEpsm(0x90 + op, regs[8], highBank);
                }

                epsmPendingPatch = nullptr;
            }

            // Only the operators that reach the output change with volume.
            const auto mask = epsmAlgorithmMask[(size_t) (epsmAlgorithm & 0x7)];

            for (int op = 0; op < 4; ++op)
            {
                const auto scaled = (mask >> op) & 1;
                const auto value = epsmOperatorVolumes[(size_t) op];

                writeEpsm(0x40 + op * 4 + index, scaled != 0 ? std::clamp(value + volumeOffset, 0, 127) : value, highBank);
            }

            epsmLastVolume = volumeOffset;
        }

        writeEpsm(0xa4 + index, periodHi, highBank);
        writeEpsm(0xa0 + index, periodLo, highBank);

        if (noteTriggered)
        {
            writeEpsm(0x28, key, false, epsmCycleKeyOnSkip);
            writeEpsm(0x28, 0xf0 + key);
            writeEpsm(0xb4 + index, epsmStereoFlags, highBank);
        }
        else if (noteReleased)
        {
            writeEpsm(0x28, key);
        }
    }
}

void ChannelEngine::updateEpsmRhythm()
{
    if (note.isMusical() && noteTriggered)
    {
        const auto drum = rhythmFollowsNote ? (note.value - noteMusicalMin) % 6 : channelGroupIndex;

        writeEpsm(epsmRegRhythmLevel + drum, epsmStereoFlags | (getVolume() << 1));
        writeEpsm(epsmRegRhythm, 1 << drum);
    }
}

bool ChannelEngine::ringsOutOnRelease() const
{
    if (isVrc7(type))
        return true;

    if (! isEpsmFm(type) || note.instrument == nullptr)
        return false;

    // A release rate this low takes 20 seconds or more to fade, so the note has to be stopped instead.
    // Only the operators that reach the output decide this.
    const auto& patch = note.instrument->epsmPatchRegisters;
    const auto mask = epsmAlgorithmMask[(size_t) (patch[0] & 0x7)];

    for (size_t y = 0; y < 4; ++y)
        if (((mask >> epsmOperatorOrder[y]) & 1) != 0 && (patch[y * 7 + 7] & 0xf) < epsmSlowestFadingRelease)
            return false;

    return true;
}

bool ChannelEngine::isDpcmKeyLooping(int noteValue) const
{
    return dpcmKit != nullptr && noteValue >= 0 && noteValue <= noteMusicalMax
        && dpcmKit->mappings[(size_t) noteValue].sample != nullptr
        && dpcmKit->mappings[(size_t) noteValue].loop;
}

void ChannelEngine::postUpdate()
{
    if (isN163(type))
    {
        sink.skipCycles((channelGroupIndex == 0 ? 2 : 0) + (resetPhase ? 10 : 11)); // ldx + lda/and/beq

        if (resetPhase)
        {
            const auto regOffset = -8 * channelGroupIndex;

            sink.skipCycles(6); // ldy/lda
            writeN163(n163RegPhaseLo + regOffset, 0);
            sink.skipCycles(4); // lda
            writeN163(n163RegPhaseMid + regOffset, 0);
            sink.skipCycles(4); // lda
            writeN163(n163RegPhaseHi + regOffset, 0);
            resetPhase = false;
        }

        sink.skipCycles(7); // inx/cpx/bne
        return;
    }

    sink.skipCycles(resetPhase ? 4 : 5); // lsr/bcc

    if (resetPhase)
    {
        if (type == ChannelType::pulse1 || type == ChannelType::pulse2)
        {
            sink.skipCycles(4); // ldy
            sink.writeRegister(regPulseHi + (type == ChannelType::pulse2 ? 4 : 0), prevPeriodHi);
        }
        else if (type == ChannelType::mmc5Pulse1 || type == ChannelType::mmc5Pulse2)
        {
            sink.skipCycles(4); // ldy
            sink.writeRegister(regMmc5Pulse1Volume + 3 + (type == ChannelType::mmc5Pulse2 ? 4 : 0), prevPeriodHi);
        }
        else if (type == ChannelType::vrc6Pulse1 || type == ChannelType::vrc6Pulse2 || type == ChannelType::vrc6Saw)
        {
            // Clear and set the high bit to reset the phase.
            const auto reg = type == ChannelType::vrc6Saw ? regVrc6SawHi : regVrc6Pulse1Volume + 2 + (type == ChannelType::vrc6Pulse2 ? 0x1000 : 0);

            sink.skipCycles(6); // tax/lda
            sink.writeRegister(reg, periodHiForReset);
            sink.skipCycles(2); // ora
            sink.writeRegister(reg, periodHiForReset | 0x80);
            sink.skipCycles(2); // txa
        }
        else if (type == ChannelType::fdsWave)
        {
            sink.skipCycles(6); // lda/ora
            sink.writeRegister(regFdsFreqHi, periodHiForReset | 0x80);
            sink.skipCycles(2); // and
            sink.writeRegister(regFdsFreqHi, periodHiForReset);
        }

        resetPhase = false;
    }
}

void ChannelEngine::computeSlide(int fromValue, int toValue, int numFrames, int& outSlidePitch, int& outSlideStep) const
{
    auto delta = type == ChannelType::noise ? fromValue - toValue
                                            : (int) noteTable[(size_t) fromValue] - (int) noteTable[(size_t) toValue];

    delta = slideShift < 0 ? delta * (1 << -slideShift) : delta >> slideShift;

    const auto absStep = (int) std::ceil((float) std::abs(delta) / (float) std::max(1, numFrames));
    const auto sign = (delta > 0) - (delta < 0);

    outSlidePitch = delta;
    outSlideStep = std::clamp(absStep * -sign, -128, 127);
}

int ChannelEngine::periodOffsetForBend(int noteValue, double semitones) const
{
    // Pitch is proportional to 1 / (period + 1).
    const auto period = (double) noteTable[(size_t) noteValue];
    return (int) std::lround((period + 1.0) * std::pow(2.0, -semitones / 12.0) - 1.0 - period);
}

bool ChannelEngine::isVolumeEnvelopeFinished() const
{
    const auto* env = envelopes[envelopeVolume];

    if (env == nullptr || env->isEmpty(envelopeVolume))
        return true;

    // A loop only keeps going while there's no release point to leave it through.
    const auto canEnd = env->loop < 0 || env->release >= 0;

    return canEnd && envelopeIdx[envelopeVolume] >= env->length - 1
        && envelopeValues[envelopeVolume] == env->values[(size_t) env->length - 1];
}
