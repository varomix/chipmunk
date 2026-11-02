#pragma once

// Reads the note-event scripts that the local fms_to_script.py and fms_txt_to_script.py write, and
// sets up the channels to play them. Shared by the trace test and the audio comparison.
#include "engine/ChannelEngine.h"

#include <functional>
#include <istream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

struct SongScript
{
    struct Event
    {
        int channel = 0, instrumentId = -1, slidePitch = 0, slideStep = 0, volumeStep = 0;
        bool isVolumeStep = false;
        ChannelNote note;
    };

    std::map<int, std::unique_ptr<Instrument>> instruments;
    std::map<int, std::unique_ptr<Envelope>> arpeggios;
    std::map<int, std::vector<Event>> events;
    std::map<int, std::shared_ptr<DpcmKit>> kits;
    std::vector<int> channelFileTypes;
    int totalFrames = 0;

    // A project's own mixer settings, by chip, where it has them. Only audio cares about these.
    struct Mixer { double volumeDb = 0.0, trebleDb = 0.0; int rolloffHz = 0, bassCutoffHz = 0; };
    std::map<int, Mixer> mixers;
    int bassCutoffHz = 0;
};

inline void readScriptEnvelope(std::istream& in, Envelope& envelope, bool hasRelative)
{
    int relative = 0;
    in >> envelope.length >> envelope.loop >> envelope.release;

    if (hasRelative)
        in >> relative;

    envelope.relative = relative != 0;

    int value;
    for (int i = 0; i < envelope.length && in >> value; ++i)
        envelope.values[(size_t) i] = (int8_t) value;
}

inline void readSongScript(std::istream& script, SongScript& song)
{
    Instrument* current = nullptr;
    std::string line;

    while (std::getline(script, line))
    {
        std::istringstream in(line);
        char kind = 0;
        in >> kind;

        if (kind == 'I')
        {
            int id;
            in >> id;
            current = (song.instruments[id] = std::make_unique<Instrument>()).get();
        }
        else if (kind == 'X')
        {
            int autoPitch;
            in >> current->sawMasterVolume >> current->s5bEnvelopeShape >> autoPitch >> current->s5bAutoPitchOctave >> current->s5bEnvelopePeriod;
            current->s5bAutoPitch = autoPitch != 0;
        }
        else if (kind == 'C')
        {
            int index, fileType;
            in >> index >> fileType;
            song.channelFileTypes.push_back(fileType);
        }
        else if (kind == 'E')
        {
            int type;
            in >> type;
            readScriptEnvelope(in, current->envelopes[(size_t) type], true);
        }
        else if (kind == 'W')
        {
            in >> current->waveSize >> current->waveLength;
            int value;
            for (int i = 0; i < current->waveLength && in >> value; ++i)
                current->wave[(size_t) i] = (int8_t) value;
        }
        else if (kind == 'Y')
        {
            readScriptEnvelope(in, current->waveRepeat, false);
        }
        else if (kind == 'D')
        {
            int value;
            for (size_t i = 0; i < current->fdsModulation.size() && in >> value; ++i)
                current->fdsModulation[i] = (int8_t) value;
        }
        else if (kind == 'P')
        {
            int autoMod;
            in >> current->fdsMasterVolume >> current->fdsModSpeed >> current->fdsModDepth >> current->fdsModDelay >> autoMod
               >> current->fdsAutoModNumerator >> current->fdsAutoModDenominator;
            current->fdsAutoMod = autoMod != 0;
        }
        else if (kind == 'Q')
        {
            in >> current->n163WavePosition;
        }
        else if (kind == 'H')
        {
            int overrideRelease, overrideStop, value;
            in >> current->vrc7Patch >> overrideRelease >> overrideStop;
            current->vrc7OverrideRelease = overrideRelease != 0;
            current->vrc7OverrideStop = overrideStop != 0;
            for (auto& reg : current->vrc7PatchRegisters)
                if (in >> value)
                    reg = (uint8_t) value;
        }
        else if (kind == 'J')
        {
            int value;
            for (auto& reg : current->epsmPatchRegisters)
                if (in >> value)
                    reg = (uint8_t) value;
        }
        else if (kind == 'A')
        {
            int id;
            in >> id;
            readScriptEnvelope(in, *(song.arpeggios[id] = std::make_unique<Envelope>()), false);
        }
        else if (kind == 'K')
        {
            int id, value, pitch, loop, initialValue;
            std::string hex;
            in >> id >> value >> pitch >> loop >> initialValue >> hex;

            auto sample = std::make_shared<DpcmSampleData>();
            sample->initialValueDiv2 = initialValue;

            for (size_t i = 0; i + 1 < hex.size(); i += 2)
                sample->data.push_back((uint8_t) std::stoi(hex.substr(i, 2), nullptr, 16));

            auto& kit = song.kits[id];

            if (kit == nullptr)
                kit = std::make_shared<DpcmKit>();

            kit->mappings[(size_t) value].sample = sample;
            kit->mappings[(size_t) value].pitch = pitch;
            kit->mappings[(size_t) value].loop = loop != 0;
        }
        else if (kind == 'V')
        {
            int frame;
            SongScript::Event e;
            e.isVolumeStep = true;
            in >> frame >> e.channel >> e.volumeStep;
            song.events[frame].push_back(e);
        }
        else if (kind == 'N')
        {
            int frame, attack, inst, arp, hasFine, hasVib, hasDuty, hasVol, hasVolSlide, phase, hasDelta, hasEnvPeriod;
            int hasFdsSpeed = 0, hasFdsDepth = 0;
            SongScript::Event e;
            auto& n = e.note;

            in >> frame >> e.channel >> n.value >> attack >> inst >> arp >> n.slideTarget >> hasFine >> n.finePitch
               >> hasVib >> n.vibratoSpeed >> n.vibratoDepth >> hasDuty >> n.dutyCycle >> hasVol >> n.volume
               >> hasVolSlide >> n.volumeSlideTarget >> phase >> e.slidePitch >> e.slideStep >> e.volumeStep
               >> hasDelta >> n.deltaCounter >> hasEnvPeriod >> n.envelopePeriod;

            // Only FDS channels carry these.
            if (in >> hasFdsSpeed >> n.fdsModSpeed >> hasFdsDepth >> n.fdsModDepth)
            {
                n.hasFdsModSpeed = hasFdsSpeed != 0;
                n.hasFdsModDepth = hasFdsDepth != 0;
            }

            e.instrumentId = inst;
            n.hasEnvelopePeriod = hasEnvPeriod != 0;
            n.hasDeltaCounter = hasDelta != 0;
            n.hasAttack = attack != 0;
            n.instrument = inst >= 0 && song.instruments.count(inst) ? song.instruments[inst].get() : nullptr;
            n.arpeggio = arp >= 0 && song.arpeggios.count(arp) ? song.arpeggios[arp].get() : nullptr;
            n.hasFinePitch = hasFine != 0;
            n.hasVibrato = hasVib != 0;
            n.hasDutyCycle = hasDuty != 0;
            n.hasVolume = hasVol != 0;
            n.hasVolumeSlide = hasVolSlide != 0;
            n.hasPhaseReset = phase != 0;
            song.events[frame].push_back(e);
        }
        else if (kind == 'O')
        {
            int chip;
            SongScript::Mixer mixer;
            in >> chip >> mixer.volumeDb >> mixer.trebleDb >> mixer.rolloffHz >> mixer.bassCutoffHz;
            song.mixers[chip] = mixer;
        }
        else if (kind == 'B')
        {
            in >> song.bassCutoffHz;
        }
        else if (kind == 'T')
        {
            in >> song.totalFrames;
        }
    }
}

// FamiStudio's channel type numbers, which are also what the scripts use.
inline ChannelType channelTypeForFileType(int type)
{
    if (type <= 4)
    {
        static constexpr ChannelType first[] { ChannelType::pulse1, ChannelType::pulse2, ChannelType::triangle, ChannelType::noise, ChannelType::dpcm };
        return first[type];
    }

    const auto offset = [type](int from, ChannelType base) { return (ChannelType) ((int) base + type - from); };

    if (type <= 6) return offset(5, ChannelType::vrc6Pulse1);
    if (type == 7) return ChannelType::vrc6Saw;
    if (type <= 13) return offset(8, ChannelType::vrc7Fm1);
    if (type == 14) return ChannelType::fdsWave;
    if (type <= 16) return offset(15, ChannelType::mmc5Pulse1);
    if (type <= 25) return offset(18, ChannelType::n163Wave1);
    if (type <= 28) return offset(26, ChannelType::s5bSquare1);
    if (type <= 31) return offset(29, ChannelType::epsmSquare1);
    if (type <= 37) return offset(32, ChannelType::epsmFm1);
    return offset(38, ChannelType::epsmRhythm1);
}

// The song's channels in FamiStudio's order, joined up as they are on one chip. "sinkFor" gives
// each channel's sink. With "sharedChip" off, channels act alone, as one device per track does.
struct SongChannels
{
    std::vector<std::unique_ptr<ChannelEngine>> channels;
    std::vector<ChannelEngine*> vrc7Group;
    S5bSharedState s5bShared, epsmShared;

    SongChannels(const std::vector<int>& fileTypes, const std::function<RegisterSink&(size_t)>& sinkFor, bool sharedChip)
    {
        auto numN163 = 0;

        for (const auto type : fileTypes)
            numN163 += type >= 18 && type <= 25 ? 1 : 0;

        for (size_t i = 0; i < fileTypes.size(); ++i)
        {
            const auto type = fileTypes[i];

            channels.push_back(std::make_unique<ChannelEngine>(sinkFor(i), channelTypeForFileType(type), false, std::max(1, numN163)));

            if (! sharedChip)
                continue;

            if (type >= 26 && type <= 28)
                channels.back()->setS5bGroup(&s5bShared, type == 26, type == 28);
            else if (type >= 29 && type <= 31)
                channels.back()->setS5bGroup(&epsmShared, type == 29, type == 31);
            else if (type >= 8 && type <= 13)
                vrc7Group.push_back(channels.back().get());
        }

        for (auto* channel : vrc7Group)
            channel->setVrc7Group(vrc7Group.data(), (int) vrc7Group.size());
    }

    void playFrame(const SongScript& song, int frame)
    {
        const auto events = song.events.find(frame);

        if (events != song.events.end())
        {
            for (const auto& e : events->second)
            {
                if (e.isVolumeStep)
                {
                    channels[(size_t) e.channel]->setVolumeSlideStep(e.volumeStep);
                }
                else
                {
                    const auto kit = song.kits.find(e.instrumentId);

                    if (kit != song.kits.end())
                        channels[(size_t) e.channel]->setDpcmKit(kit->second.get());

                    channels[(size_t) e.channel]->playNote(e.note, e.slidePitch, e.slideStep, e.volumeStep);
                }
            }
        }

        for (auto& channel : channels)
            channel->update();

        for (auto& channel : channels)
            channel->postUpdate();
    }
};
