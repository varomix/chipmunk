#include "engine/NesEngine.h"

#include "Simple_Apu.h"

// The emulator headers define these as macros.
#undef min
#undef max
#undef clamp
#undef abs

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <utility>

namespace
{
// FamiStudio's default mixer settings.
constexpr int bassCutoffHz = 24;
constexpr double trebleDb = -5.0;
constexpr int trebleRolloffHz = 14000;
constexpr double globalVolumeDb = -2.0;

// Every chip a channel can use stays on, so switching channels never has to touch the audio
// buffers mid-stream. The silent ones cost almost nothing.
constexpr int alwaysOnExpansions = Simple_Apu::expansion_mask_vrc6 | Simple_Apu::expansion_mask_mmc5 | Simple_Apu::expansion_mask_sunsoft;

bool isTypeInRange(ChannelType type, ChannelType first, ChannelType last)
{
    return (int) type >= (int) first && (int) type <= (int) last;
}

constexpr int regPulse1Volume = 0x4000;
constexpr int regPulse1Sweep = 0x4001;
constexpr int regPulse2Volume = 0x4004;
constexpr int regPulse2Sweep = 0x4005;
constexpr int regTriangleLinear = 0x4008;
constexpr int regNoiseVolume = 0x400c;
constexpr int regNoiseHi = 0x400f;
constexpr int regDmcRaw = 0x4011;
constexpr int regStatus = 0x4015;
constexpr int regVrc6Control = 0x9003;
constexpr int regMmc5Pulse1Volume = 0x5000;
constexpr int regMmc5Pulse2Volume = 0x5004;
constexpr int regMmc5Status = 0x5015;
constexpr int regS5bAddress = 0xc000;
constexpr int regS5bData = 0xf000;
constexpr int s5bRegMixer = 0x07;
constexpr int dpcmSampleAddress = 0xc000;
constexpr int dacDefaultValue = 64;
} // namespace

NesEngine::NesEngine() = default;
NesEngine::~NesEngine() = default;

int NesEngine::chipsNeededFor(ChannelType type)
{
    if (type == ChannelType::fdsWave) return chipFds;
    if (isTypeInRange(type, ChannelType::n163Wave1, ChannelType::n163Wave8)) return chipN163;
    if (isTypeInRange(type, ChannelType::vrc7Fm1, ChannelType::vrc7Fm6)) return chipVrc7;
    if (isTypeInRange(type, ChannelType::epsmSquare1, ChannelType::epsmRhythm6)) return chipEpsm;
    return 0;
}

void NesEngine::prepare(double sampleRate, bool pal, int extraChips, int numN163Channels)
{
    const auto rate = (int) std::lround(sampleRate);

    isPal = pal;
    enabledChips = extraChips;
    numN163 = std::clamp(numN163Channels, 1, 8);

    auto expansions = alwaysOnExpansions;

    if ((extraChips & chipFds) != 0) expansions |= Simple_Apu::expansion_mask_fds;
    if ((extraChips & chipN163) != 0) expansions |= Simple_Apu::expansion_mask_namco;
    if ((extraChips & chipVrc7) != 0) expansions |= Simple_Apu::expansion_mask_vrc7;
    if ((extraChips & chipEpsm) != 0) expansions |= Simple_Apu::expansion_mask_epsm;

    sampleVoice.reset();
    sampleChannel.reset();
    voice.reset();
    channel.reset();
    dmcSample = nullptr;
    dacRestLevel = 0;

    {
        // Creating and resetting a chip writes static state in the EPSM and VRC7
        // emulators, and plugin instances can be prepared on different threads.
        static std::mutex chipSetupMutex;
        const std::scoped_lock lock(chipSetupMutex);

        apu = std::make_unique<Simple_Apu>();
        apu->sample_rate(rate, pal, Simple_Apu::tnd_mode_single);
        apu->dmc_reader(readDmcMemory, this);
        apu->set_audio_expansions(expansions);
        apu->bass_freq(Simple_Apu::expansion_none, bassCutoffHz);
        apu->reset();
    }

    // FamiStudio's default mixer. The chips have the same level, and differ in their treble.
    struct Mixer { int chip, mask; double trebleDb; int rolloffHz; };

    static constexpr Mixer mixers[] {
        { Simple_Apu::expansion_none, 0, trebleDb, trebleRolloffHz },
        { Simple_Apu::expansion_vrc6, Simple_Apu::expansion_mask_vrc6, trebleDb, trebleRolloffHz },
        { Simple_Apu::expansion_mmc5, Simple_Apu::expansion_mask_mmc5, trebleDb, trebleRolloffHz },
        { Simple_Apu::expansion_sunsoft, Simple_Apu::expansion_mask_sunsoft, trebleDb, trebleRolloffHz },
        { Simple_Apu::expansion_fds, Simple_Apu::expansion_mask_fds, 0.0, 2000 },
        { Simple_Apu::expansion_namco, Simple_Apu::expansion_mask_namco, -15.0, trebleRolloffHz },
        { Simple_Apu::expansion_vrc7, Simple_Apu::expansion_mask_vrc7, -15.0, trebleRolloffHz },
        { Simple_Apu::expansion_epsm, Simple_Apu::expansion_mask_epsm, trebleDb, trebleRolloffHz },
    };

    for (const auto& mixer : mixers)
    {
        if (mixer.chip == Simple_Apu::expansion_none || (expansions & mixer.mask) != 0)
        {
            apu->treble_eq(mixer.chip, mixer.trebleDb, mixer.rolloffHz, rate);
            apu->set_expansion_volume(mixer.chip, std::pow(10.0, globalVolumeDb / 20.0));
        }
    }

    if ((expansions & Simple_Apu::expansion_mask_fds) != 0)
        apu->bass_freq(Simple_Apu::expansion_fds, 5);

    if ((expansions & Simple_Apu::expansion_mask_namco) != 0)
        apu->set_namco_mix(true);

    // These chips start with their channels off.
    if ((expansions & Simple_Apu::expansion_mask_epsm) != 0)
        for (int i = 0; i < 15; ++i)
            apu->enable_channel(Simple_Apu::expansion_epsm, i, true);

    if ((expansions & Simple_Apu::expansion_mask_vrc7) != 0)
        for (int i = 0; i < 6; ++i)
            apu->enable_channel(Simple_Apu::expansion_vrc7, i, true);

    // Same initial state as FamiStudio's NesApu.InitAndReset.
    apu->write_register(regStatus, 0x0f);
    apu->write_register(regTriangleLinear, 0x80);
    apu->write_register(regNoiseHi, 0x00);
    apu->write_register(regPulse1Volume, 0x30);
    apu->write_register(regPulse2Volume, 0x30);
    apu->write_register(regNoiseVolume, 0x30);
    apu->write_register(regPulse1Sweep, 0x08);
    apu->write_register(regPulse2Sweep, 0x08);
    apu->write_register(regVrc6Control, 0x00);
    apu->write_register(regMmc5Pulse1Volume, 0x10);
    apu->write_register(regMmc5Pulse2Volume, 0x10);
    apu->write_register(regMmc5Status, 0x03);
    apu->write_register(regS5bAddress, s5bRegMixer);
    apu->write_register(regS5bData, 0x38);

    if ((expansions & Simple_Apu::expansion_mask_vrc7) != 0)
        apu->write_register(0xe000, 0x00); // Enable VRC7 audio.

    if ((expansions & Simple_Apu::expansion_mask_namco) != 0)
    {
        apu->write_register(0xf800, 0x7f);
        apu->write_register(0x4800, (numN163 - 1) << 4);
    }

    if ((expansions & Simple_Apu::expansion_mask_epsm) != 0)
    {
        for (const auto [reg, value] : { std::pair { 0x07, 0x38 }, std::pair { 0x29, 0x80 }, std::pair { 0x27, 0x00 }, std::pair { 0x11, 0x37 } })
        {
            apu->write_register(0x401c, reg);
            apu->skip_cycles(4);
            apu->write_register(0x401d, value);
            apu->skip_cycles(20);
        }
    }

    samplePosition = 0;
    emulatedCycles = 0;
    numPending = 0;
    frameCount = 0;
    frameIsDue = false;
    samplesPerFrame = sampleRate / (pal ? framesPerSecondPal : framesPerSecondNtsc);
    nextFramePosition = samplesPerFrame;

    createChannel(ChannelType::pulse1);

    // Let the emulator's power-on transient decay so it can't be heard on the first note.
    discardAudio(rate / 4);

    frameCount = 0;
}

void NesEngine::createChannel(ChannelType type)
{
    const auto isKit = type == ChannelType::drumKit;

    sampleVoice.reset();
    sampleChannel.reset();
    voice.reset();
    channel.emplace(static_cast<RegisterSink&>(*this), isKit ? ChannelType::noise : type, isPal, numN163);
    channel->setDpcmKit(dpcmKit);
    channel->setRhythmFollowsNote(true);
    voice.emplace(*channel, instrument);
    voice->setSettings(voiceSettings);
    currentType = type;

    if (isKit)
    {
        voice->setKeyMap(dpcmKit, true);
        sampleChannel.emplace(static_cast<RegisterSink&>(*this), ChannelType::dpcm, isPal, numN163);
        sampleChannel->setDpcmKit(dpcmKit);
        sampleVoice.emplace(*sampleChannel, instrument);
        sampleVoice->setSettings(voiceSettings);
    }
}

VoiceController& NesEngine::voiceForKey(int midiNote)
{
    const auto value = midiNoteToNoteValue(midiNote);
    const auto isSample = sampleVoice.has_value() && dpcmKit != nullptr && value >= noteMusicalMin && value <= noteMusicalMax
                       && dpcmKit->mappings[(size_t) value].sample != nullptr;

    return isSample ? *sampleVoice : *voice;
}

void NesEngine::setChannelType(ChannelType type)
{
    if (currentType == type)
        return;

    // Silence the old channel before leaving it, since its registers won't be touched again.
    voice->allNotesOff();
    channel->update();

    if (sampleVoice.has_value())
    {
        sampleVoice->allNotesOff();
        sampleChannel->update();
    }

    createChannel(type);
    restDac();
}

void NesEngine::restDac()
{
    // Rest the DAC where DPCM expects it, which avoids a pop on the first hit. Everything else
    // needs it at zero, because its level changes how loud triangle and noise are.
    const auto usesSamples = currentType == ChannelType::dpcm || (currentType == ChannelType::drumKit && dpcmKit != nullptr && dpcmKit->hasSamples());
    const auto level = usesSamples ? dacDefaultValue : 0;

    if (level == dacRestLevel)
        return;

    dacRestLevel = level;
    apu->write_register(regDmcRaw, level);

    // That jump is a loud pop, which goes unheard if the chip runs past it here. It doesn't take
    // long, as the mixer's filters only need a few milliseconds to settle.
    discardAudio((int) std::lround(samplesPerFrame * 8.0));
}

void NesEngine::discardAudio(int numSamples)
{
    std::array<float, maxChunkSamples> discarded;

    for (; numSamples > 0; numSamples -= maxChunkSamples)
        render(discarded.data(), nullptr, std::min(numSamples, maxChunkSamples));
}

void NesEngine::setDpcmKit(const DpcmKit* kit)
{
    auto stillInKit = false;

    if (kit != nullptr)
        for (const auto& mapping : kit->mappings)
            stillInKit = stillInKit || mapping.sample.get() == dmcSample;

    if (! stillInKit)
        dmcSample = nullptr;

    dpcmKit = kit;
    channel->setDpcmKit(kit);

    if (sampleVoice.has_value())
    {
        voice->setKeyMap(kit, true);
        sampleChannel->setDpcmKit(kit);
        restDac();
    }
}

void NesEngine::selectDpcmSample(const DpcmSampleData* sample)
{
    dmcSample = sample;
}

int NesEngine::readDmcMemory(void* enginePointer, unsigned address)
{
    const auto& engine = *static_cast<const NesEngine*>(enginePointer);
    const auto offset = (long) address - dpcmSampleAddress;

    return engine.dmcSample != nullptr && offset >= 0 && offset < (long) engine.dmcSample->data.size()
             ? engine.dmcSample->data[(size_t) offset]
             : dacDefaultValue;
}

void NesEngine::setVoiceSettings(const VoiceController::Settings& settings)
{
    voiceSettings = settings;
    voice->setSettings(settings);

    if (sampleVoice.has_value())
        sampleVoice->setSettings(settings);
}

void NesEngine::setInstrument(const Instrument& newInstrument)
{
    instrument = newInstrument;
    channel->instrumentEdited();
}

void NesEngine::noteEventHappened()
{
    if (! authenticGrid)
        frameIsDue = true;
}

void NesEngine::noteOn(int midiNote, int velocity)
{
    voiceForKey(midiNote).noteOn(midiNote, velocity);
    noteEventHappened();
}

void NesEngine::noteOff(int midiNote)
{
    voiceForKey(midiNote).noteOff(midiNote);
    noteEventHappened();
}

void NesEngine::allNotesOff()
{
    voice->allNotesOff();

    if (sampleVoice.has_value())
        sampleVoice->allNotesOff();

    noteEventHappened();
}

void NesEngine::setPitchBend(double normalized)
{
    voice->setPitchBend(normalized);
}

void NesEngine::setModWheel(int value)
{
    voice->setModWheel(value);
}

void NesEngine::setController(int controller, int value)
{
    if (controller == MidiEffect::stopSample)
    {
        if (sampleVoice.has_value())
        {
            sampleVoice->setController(MidiEffect::stopNote, value);
            noteEventHappened();
        }

        return;
    }

    voice->setController(controller, value);

    if (controller == MidiEffect::stopNote)
        noteEventHappened();
}

void NesEngine::setTransportPlaying(bool playing)
{
    voice->setTransportPlaying(playing);

    if (sampleVoice.has_value())
        sampleVoice->setTransportPlaying(playing);
}

void NesEngine::writeRegister(int reg, int data)
{
    apu->write_register((cpu_addr_t) reg, data);
}

void NesEngine::skipCycles(int cycles)
{
    apu->skip_cycles(cycles);
}

void NesEngine::runFrame()
{
    voice->beginFrame();
    channel->update();
    channel->postUpdate();

    if (sampleVoice.has_value())
    {
        sampleVoice->beginFrame();
        sampleChannel->update();
        sampleChannel->postUpdate();
    }

    ++frameCount;
}

void NesEngine::render(float* left, float* right, int numSamples)
{
    while (numSamples > 0)
    {
        if (frameIsDue)
        {
            // A note event restarts the frame clock from this exact sample.
            frameIsDue = false;
            runFrame();
            nextFramePosition = (double) samplePosition + samplesPerFrame;
        }
        else if ((double) samplePosition >= nextFramePosition)
        {
            runFrame();
            nextFramePosition += samplesPerFrame;
        }

        const auto untilFrame = (int64_t) std::ceil(nextFramePosition) - samplePosition;
        const auto count = (int) std::min({ (int64_t) numSamples, (int64_t) maxChunkSamples, untilFrame });

        renderChunk(left, right, count);

        left += count;

        if (right != nullptr)
            right += count;

        numSamples -= count;
    }
}

void NesEngine::renderChunk(float* left, float* right, int count)
{
    samplePosition += count;

    const auto target = apu->clocks_for_samples(samplePosition);
    const auto cycles = std::max(target - emulatedCycles, (int64_t) apu->chunk_time());

    apu->end_chunk((long) cycles);
    emulatedCycles += cycles;

    // The emulator gives pairs of samples when it's stereo, and "numPending" counts them as one.
    const auto numChannels = isStereo() ? 2 : 1;

    numPending += (int) apu->read_samples(pending.data() + numPending * numChannels, apu->samples_avail());

    for (int i = 0; i < count; ++i)
    {
        if (numChannels == 1)
        {
            left[i] = (float) pending[(size_t) i] / 32768.0f;
        }
        else
        {
            const auto l = (float) pending[(size_t) i * 2] / 32768.0f, r = (float) pending[(size_t) i * 2 + 1] / 32768.0f;

            left[i] = right != nullptr ? l : (l + r) * 0.5f;

            if (right != nullptr)
                right[i] = r;
        }
    }

    numPending -= count;
    std::memmove(pending.data(), pending.data() + count * numChannels, (size_t) (numPending * numChannels) * sizeof(int16_t));
}
