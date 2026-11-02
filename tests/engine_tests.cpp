#include "engine/NesEngine.h"

#include "Simple_Apu.h"

// The emulator headers define these as macros.
#undef min
#undef max
#undef clamp
#undef abs

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <tuple>
#include <vector>

namespace
{
int failures = 0;

void check(bool ok, const char* what)
{
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

struct RegWrite { long cycle; int reg; int value; };

// Exercises everything the frame sequencer clocks: hardware envelope, sweep,
// length counters and the linear counter, plus a 4017 mode change.
std::vector<RegWrite> sequencerScript(long totalCycles)
{
    std::mt19937 rng(1234);
    std::vector<RegWrite> writes = {
        {100, 0x4015, 0x0f}, {120, 0x4000, 0x8f}, {140, 0x4001, 0xa3}, {160, 0x4002, 0x80}, {180, 0x4003, 0x31},
        {200, 0x4004, 0x5a}, {220, 0x4006, 0xfd}, {240, 0x4007, 0x09},
        {260, 0x4008, 0x4f}, {280, 0x400a, 0x40}, {300, 0x400b, 0x12},
        {320, 0x400c, 0x07}, {340, 0x400e, 0x05}, {360, 0x400f, 0x28},
    };

    for (long c = 7000; c < totalCycles; c += 3000 + (long) (rng() % 14000))
    {
        switch (rng() % 5)
        {
            case 0: writes.push_back({c, 0x4002, (int) (rng() & 0xff)}); break;
            case 1: writes.push_back({c, 0x4003, (int) (rng() & 0xf9) | 1}); break;
            case 2: writes.push_back({c, 0x400a, (int) (rng() & 0xff)}); break;
            case 3: writes.push_back({c, 0x400f, (int) (rng() & 0xf8)}); break;
            case 4: writes.push_back({c, 0x4017, (rng() & 1) ? 0xc0 : 0x40}); break;
        }
    }

    return writes;
}

std::unique_ptr<Simple_Apu> makeRawApu()
{
    auto apu = std::make_unique<Simple_Apu>();
    apu->sample_rate(44100, false, Simple_Apu::tnd_mode_single);
    apu->set_audio_expansions(0);
    apu->bass_freq(0, 24);
    apu->reset();
    return apu;
}

void drain(Simple_Apu& apu, std::vector<short>& out)
{
    const auto count = apu.samples_avail();
    const auto start = out.size();
    out.resize(start + (size_t) count);
    apu.read_samples(out.data() + start, count);
}

// Drives identical writes through real end_frame() calls and random end_chunk() calls.
void testChunkingMatchesFrames()
{
    std::puts("chunked rendering matches whole-frame rendering");

    const int numFrames = 600;
    const auto script = sequencerScript(numFrames * 29780L);

    std::vector<short> frames, chunks;

    {
        auto apu = makeRawApu();
        long frameStart = 0, nextEnd = 29781;
        size_t w = 0;

        for (int f = 0; f < numFrames; ++f)
        {
            while (w < script.size() && script[w].cycle + 8 < nextEnd)
            {
                apu->skip_cycles(script[w].cycle - frameStart - apu->chunk_time());
                apu->write_register(script[w].reg, script[w].value);
                ++w;
            }

            apu->end_frame();
            drain(*apu, frames);
            frameStart = nextEnd;
            nextEnd += (f % 2 == 0) ? 29780 : 29781;
        }
    }

    {
        auto apu = makeRawApu();
        std::mt19937 rng(99);
        long chunkStart = 0, frameEnd = 29781;
        size_t w = 0;

        for (int f = 0; f < numFrames; ++f)
        {
            while (true)
            {
                const auto now = chunkStart + apu->chunk_time();
                const bool hasWrite = w < script.size() && script[w].cycle + 8 < frameEnd;
                const auto stop = hasWrite ? script[w].cycle : frameEnd;
                const auto split = std::min(stop, now + 1 + (long) (rng() % 3000));

                if (split > now)
                    apu->skip_cycles(split - now);

                if (split < stop || ! hasWrite)
                {
                    const auto length = apu->chunk_time();
                    apu->end_chunk(length);
                    drain(*apu, chunks);
                    chunkStart += length;

                    if (! hasWrite && split == frameEnd)
                        break;
                }
                else
                {
                    apu->write_register(script[w].reg, script[w].value);
                    ++w;
                }
            }

            frameEnd += (f % 2 == 0) ? 29780 : 29781;
        }
    }

    std::printf("  frames: %zu samples, chunks: %zu samples\n", frames.size(), chunks.size());

    size_t differences = 0;
    int nonZero = 0;
    const auto common = std::min(frames.size(), chunks.size());

    for (size_t i = 0; i < common; ++i)
    {
        differences += frames[i] != chunks[i] ? 1 : 0;
        nonZero += frames[i] != 0 ? 1 : 0;
    }

    std::printf("  differing samples: %zu of %zu\n", differences, common);
    check(frames.size() == chunks.size(), "same sample count");
    check(differences == 0, "bit-identical output");
    check(nonZero > (int) common / 2, "test signal is not silent");
}

struct NoteEvent { long position; int note; int velocity; };

int a4Period()
{
    return makeNoteTable(false)[(size_t) midiNoteToNoteValue(69)];
}

std::vector<float> renderSchedule(double rate, long length, const std::vector<NoteEvent>& events, const std::vector<int>& blockSizes)
{
    NesEngine engine;
    engine.prepare(rate);

    std::vector<float> out((size_t) length);
    long position = 0;
    size_t nextEvent = 0, nextBlock = 0;

    while (position < length)
    {
        auto blockEnd = std::min(length, position + blockSizes[nextBlock++ % blockSizes.size()]);

        while (position < blockEnd)
        {
            while (nextEvent < events.size() && events[nextEvent].position == position)
            {
                const auto& e = events[nextEvent++];
                if (e.velocity > 0)
                    engine.noteOn(e.note, e.velocity);
                else
                    engine.noteOff(e.note);
            }

            auto stop = blockEnd;
            if (nextEvent < events.size())
                stop = std::min(stop, events[nextEvent].position);

            engine.render(out.data() + position, (int) (stop - position));
            position = stop;
        }
    }

    return out;
}

// Position of the first waveform edge, at half the height of its first peak.
double firstEdge(const std::vector<float>& audio)
{
    float peak = 0.0f;
    size_t i = 0;

    for (; i < audio.size(); ++i)
    {
        if (peak > 0.05f && audio[i] < peak)
            break;
        peak = std::max(peak, audio[i]);
    }

    const auto half = peak * 0.5f;

    for (i = 1; i < audio.size(); ++i)
        if (audio[i] >= half)
            return (double) (i - 1) + (half - audio[i - 1]) / (audio[i] - audio[i - 1]);

    return -1.0;
}

void testOnsetTiming()
{
    std::puts("note onsets land where they were requested");

    for (double rate : {44100.0, 48000.0, 96000.0, 192000.0})
    {
        double minOffset = 1e9, maxOffset = -1e9;

        for (long position : {0L, 1L, 2L, 3L, 37L, 64L, 255L, 256L, 257L, 1000L, 4097L, 12345L})
        {
            const auto audio = renderSchedule(rate, position + 2000, {{position, 69, 127}}, {512});
            const auto offset = firstEdge(audio) - (double) position;
            minOffset = std::min(minOffset, offset);
            maxOffset = std::max(maxOffset, offset);
        }

        // On the chip the first edge comes one duty step after the write: 2 * (period + 1) cycles.
        const auto dutyStep = 2.0 * (a4Period() + 1) * rate / 1789773.0;

        std::printf("  %6.0f Hz: edge %.2f to %.2f samples after the event, %.2f of that is the duty step, %.2f is output delay\n",
                    rate, minOffset, maxOffset, dutyStep, minOffset - dutyStep);
        check(maxOffset - minOffset < 0.5, "onset position varies by under half a sample");
    }
}

std::vector<NoteEvent> melody(int seed, long length)
{
    std::mt19937 rng((unsigned) seed);
    std::vector<NoteEvent> events;
    int note = -1;

    for (long position = 10 + (long) (rng() % 200); position < length; position += 500 + (long) (rng() % 9000))
    {
        if (note >= 0 && (rng() & 1))
        {
            events.push_back({position, note, 0});
            note = -1;
        }
        else
        {
            note = 36 + (int) (rng() % 60);
            events.push_back({position, note, 20 + (int) (rng() % 108)});
        }
    }

    return events;
}

void testBlockSizeIndependence()
{
    std::puts("output does not depend on block size");

    const long length = 48000 * 5;
    const auto events = melody(7, length);
    const auto reference = renderSchedule(48000.0, length, events, {512});

    bool same = true;

    for (const auto& blocks : std::vector<std::vector<int>> {{1}, {16}, {64}, {441}, {1024}, {4096}, {13, 700, 1, 2048, 97}})
        same = same && renderSchedule(48000.0, length, events, blocks) == reference;

    check(same, "bit-identical across block sizes, including 1 and 4096");
}

void testPitchAndLevel()
{
    std::puts("pitch and level");

    for (double rate : {44100.0, 48000.0, 96000.0})
    {
        const long length = (long) rate * 4;
        const auto audio = renderSchedule(rate, length, {{0, 69, 127}}, {512});

        long firstCrossing = -1, lastCrossing = -1, crossings = 0;
        float peak = 0.0f;

        for (long i = (long) rate; i < length; ++i)
        {
            peak = std::max(peak, std::abs(audio[(size_t) i]));

            if (audio[(size_t) i - 1] < 0.0f && audio[(size_t) i] >= 0.0f)
            {
                if (firstCrossing < 0)
                    firstCrossing = i;
                lastCrossing = i;
                ++crossings;
            }
        }

        const auto measured = (crossings - 1) * rate / (double) (lastCrossing - firstCrossing);
        const auto expected = 1789773.0 / (16.0 * (a4Period() + 1));
        const auto cents = 1200.0 * std::log2(measured / expected);

        std::printf("  %6.0f Hz: A4 measured %.3f Hz, chip target %.3f Hz, error %.2f cents, peak %.3f\n", rate, measured, expected, cents, peak);
        check(std::abs(cents) < 1.0, "within 1 cent of the chip's pitch");
        check(peak > 0.05f && peak < 1.0f, "healthy level without clipping");
    }

    const auto audio = renderSchedule(48000.0, 48000 * 3, {{100, 60, 127}, {24000, 60, 0}}, {512});
    float tail = 0.0f;
    for (long i = 48000 * 2; i < 48000 * 3; ++i)
        tail = std::max(tail, std::abs(audio[(size_t) i]));

    std::printf("  peak 1.5 s after note-off: %.5f\n", tail);
    check(tail < 0.002f, "silent after note-off");
}

struct WriteLog final : RegisterSink
{
    std::vector<std::pair<int, int>> writes;

    void writeRegister(int reg, int data) override { writes.emplace_back(reg, data); }
    void skipCycles(int) override {}

    int last(int reg) const
    {
        for (auto it = writes.rbegin(); it != writes.rend(); ++it)
            if (it->first == reg)
                return it->second;
        return -1;
    }
};

void testVoiceLogic()
{
    std::puts("voice logic");

    constexpr int regVolume = 0x4000, regSweep = 0x4001, regLo = 0x4002, regHi = 0x4003;
    const auto table = makeNoteTable(false);
    const auto periodOf = [&](int midi) { return (int) table[(size_t) midiNoteToNoteValue(midi)]; };

    WriteLog log;
    Instrument decay;
    decay.envelopes[envelopeVolume].length = 4;
    for (int i = 0; i < 4; ++i)
        decay.envelopes[envelopeVolume].values[(size_t) i] = (int8_t) (15 - i * 3);

    ChannelEngine channel(log, ChannelType::pulse1);
    VoiceController voice(channel, decay);

    const auto frame = [&](int count = 1) {
        for (int i = 0; i < count; ++i)
        {
            voice.beginFrame();
            channel.update();
        }
    };
    // Follows the high period bits the way the chip does, including smooth vibrato's sweep trick.
    const auto period = [&] {
        int hi = 0;
        for (const auto& [reg, data] : log.writes)
        {
            if (reg == regHi)
                hi = data & 7;
            else if (reg == regSweep && data == 0x87)
                ++hi;
            else if (reg == regSweep && data == 0x8f)
                --hi;
        }
        return log.last(regLo) | (hi << 8);
    };

    voice.noteOn(69, 127);
    frame();
    check(period() == periodOf(69), "note-on plays the note table's period");
    check((log.last(regVolume) & 0x0f) == 15, "first frame uses the envelope's first value");
    check((log.last(regVolume) >> 6) == 2, "duty setting reaches the chip");

    frame(2);
    const auto decayed = log.last(regVolume) & 0x0f;
    check(decayed == 9, "volume envelope advances each frame");

    voice.noteOn(72, 127);
    frame();
    check(period() == periodOf(72), "last note wins");
    check((log.last(regVolume) & 0x0f) == 6, "legato note does not restart the envelope");

    voice.noteOff(72);
    frame();
    check(period() == periodOf(69), "releasing the top note returns to the held note");

    voice.noteOff(69);
    frame();
    check((log.last(regVolume) & 0x0f) == 0, "last note-off silences the channel");

    voice.noteOn(60, 127);
    frame();
    check((log.last(regVolume) & 0x0f) == 15, "a detached note restarts the envelope");
    voice.noteOff(60);
    frame();

    auto settings = VoiceController::Settings {};
    settings.legato = false;
    voice.setSettings(settings);
    voice.noteOn(60, 127);
    frame(3);
    voice.noteOn(64, 127);
    frame();
    check((log.last(regVolume) & 0x0f) == 15, "with legato off, overlapping notes restart the envelope");
    voice.allNotesOff();
    frame();
    settings.legato = true;
    voice.setSettings(settings);

    voice.noteOn(69, 64);
    frame();
    check((log.last(regVolume) & 0x0f) == 8, "velocity scales volume");
    voice.noteOff(69);
    frame();
    settings.velocitySensitive = false;
    voice.setSettings(settings);
    voice.noteOn(69, 64);
    frame();
    check((log.last(regVolume) & 0x0f) == 15, "velocity can be turned off");
    voice.noteOff(69);
    frame();
    settings.velocitySensitive = true;
    voice.setSettings(settings);

    voice.noteOn(69, 127);
    frame();
    voice.setPitchBend(1.0);
    frame();
    check(std::abs(period() - periodOf(71)) <= 1, "full bend reaches the bend range");
    voice.setPitchBend(-1.0);
    frame();
    check(std::abs(period() - periodOf(67)) <= 1, "bend goes down too");
    voice.setPitchBend(0.0);
    frame();
    check(period() == periodOf(69), "centered bend returns to the exact note");

    voice.setModWheel(127);
    std::vector<int> periods;
    for (int i = 0; i < 12; ++i)
    {
        frame();
        periods.push_back(period());
    }
    const auto [lo, hi] = std::minmax_element(periods.begin(), periods.end());
    check(*lo < periodOf(69) && *hi > periodOf(69), "mod wheel adds vibrato around the note");
    voice.setModWheel(0);
    frame();
    voice.noteOff(69);
    frame();
    voice.noteOn(69, 127);
    frame();
    check(period() == periodOf(69), "vibrato is gone once the wheel returns to zero");
    voice.noteOff(69);
    frame();

    settings.glideFrames = 8;
    voice.setSettings(settings);
    voice.noteOn(60, 127);
    frame();
    voice.noteOn(72, 127);
    frame();
    const auto startOfGlide = period();
    frame(4);
    const auto midGlide = period();
    frame(6);
    check(startOfGlide > midGlide && midGlide > periodOf(72), "glide moves gradually toward the new note");
    check(period() == periodOf(72), "glide lands on the new note");
    voice.allNotesOff();
    frame();

    // Release envelopes play out and then stop.
    Instrument releasing;
    auto& env = releasing.envelopes[envelopeVolume];
    env.length = 5;
    env.loop = 0;
    env.release = 2;
    for (auto [i, v] : std::vector<std::pair<int, int>> {{0, 15}, {1, 15}, {2, 9}, {3, 6}, {4, 3}})
        env.values[(size_t) i] = (int8_t) v;

    WriteLog releaseLog;
    ChannelEngine releaseChannel(releaseLog, ChannelType::pulse1);
    VoiceController releaseVoice(releaseChannel, releasing);
    const auto releaseFrame = [&](int count = 1) {
        for (int i = 0; i < count; ++i)
        {
            releaseVoice.beginFrame();
            releaseChannel.update();
        }
    };

    releaseVoice.noteOn(69, 127);
    releaseFrame(5);
    check((releaseLog.last(regVolume) & 0x0f) == 15, "the sustain loop holds while the key is down");
    releaseVoice.noteOff(69);
    releaseFrame();
    check((releaseLog.last(regVolume) & 0x0f) == 9, "note-off jumps to the release point");
    releaseFrame(4);
    check((releaseLog.last(regVolume) & 0x0f) == 0, "the note stops when the release has played");
}

void testChannelsAndFrameClock()
{
    std::puts("channels and frame clock");

    const auto rmsOf = [](const std::vector<float>& audio, size_t from, size_t to) {
        double sum = 0.0;
        for (size_t i = from; i < to; ++i)
            sum += audio[i] * audio[i];
        return std::sqrt(sum / (double) (to - from));
    };

    for (const auto [type, name] : std::vector<std::pair<ChannelType, const char*>> {{ChannelType::pulse1, "pulse"}, {ChannelType::pulse2, "pulse 2"}, {ChannelType::triangle, "triangle"}, {ChannelType::noise, "noise"}})
    {
        NesEngine engine;
        engine.prepare(48000.0);
        engine.setChannelType(type);

        std::vector<float> audio(48000);
        engine.noteOn(69, 127);
        engine.render(audio.data(), 24000);
        engine.noteOff(69);
        engine.render(audio.data() + 24000, 24000);

        const auto on = rmsOf(audio, 2400, 24000), off = rmsOf(audio, 40000, 48000);
        std::printf("  %-8s rms while held %.4f, after note-off %.6f\n", name, on, off);
        check(on > 0.01 && off < 0.001, "sounds while held and silent after note-off");
    }

    // Pitch as really heard from the emulator, including across the smooth vibrato boundary.
    {
        const auto measure = [](double bend, int bendRange) {
            NesEngine engine;
            engine.prepare(48000.0);
            auto settings = VoiceController::Settings {};
            settings.bendRangeSemitones = bendRange;
            engine.setVoiceSettings(settings);
            engine.noteOn(69, 127);
            engine.setPitchBend(bend);

            std::vector<float> audio(48000 * 3);
            engine.render(audio.data(), (int) audio.size());

            long first = -1, last = -1, crossings = 0;
            for (long i = 48000; i < (long) audio.size(); ++i)
            {
                if (audio[(size_t) i - 1] < 0.0f && audio[(size_t) i] >= 0.0f)
                {
                    if (first < 0)
                        first = i;
                    last = i;
                    ++crossings;
                }
            }
            return (crossings - 1) * 48000.0 / (double) (last - first);
        };

        const auto base = measure(0.0, 2);

        for (const auto [bend, range, semitones] : std::vector<std::tuple<double, int, double>> {{1.0, 2, 2.0}, {-1.0, 2, -2.0}, {-1.0, 12, -12.0}, {0.5, 2, 1.0}})
        {
            const auto measured = measure(bend, range);
            const auto cents = 1200.0 * std::log2(measured / base) - semitones * 100.0;
            std::printf("  bend %+.0f semitones: %.2f Hz, %.1f cents from target\n", semitones, measured, cents);
            check(std::abs(cents) < 12.0, "bent pitch is audibly right");
        }
    }

    // Switching channel while a note plays must not leave the old one ringing.
    {
        NesEngine engine;
        engine.prepare(48000.0);
        std::vector<float> audio(48000);
        engine.noteOn(69, 127);
        engine.render(audio.data(), 12000);
        engine.setChannelType(ChannelType::triangle);
        engine.render(audio.data() + 12000, 36000);
        check(rmsOf(audio, 40000, 48000) < 0.001, "changing channel silences the old one");
    }

    // Free-running frame rate.
    for (const auto [pal, expected] : std::vector<std::pair<bool, double>> {{false, 60.0988}, {true, 50.007}})
    {
        NesEngine engine;
        engine.prepare(48000.0, pal);
        std::vector<float> audio(48000);
        for (int second = 0; second < 10; ++second)
            engine.render(audio.data(), 48000);

        const auto rate = (double) engine.getFrameCount() / 10.0;
        std::printf("  %s: %.2f frames per second\n", pal ? "PAL" : "NTSC", rate);
        check(std::abs(rate - expected) < 0.2, "frame clock runs at the machine's rate");
    }

    // A note restarts the clock by default, but waits for the grid in authentic mode.
    {
        std::vector<float> audio(48000);
        const auto edgeAfter = [&](bool authentic, int position) {
            NesEngine engine;
            engine.prepare(48000.0);
            engine.setAuthenticGrid(authentic);
            std::fill(audio.begin(), audio.end(), 0.0f);
            engine.render(audio.data(), position);
            engine.noteOn(69, 127);
            engine.render(audio.data() + position, 48000 - position);
            std::vector<float> rest(audio.begin() + position, audio.end());
            return firstEdge(rest);
        };

        const auto a = edgeAfter(false, 1000), b = edgeAfter(false, 1300);
        const auto c = edgeAfter(true, 1000), d = edgeAfter(true, 1300);
        std::printf("  edge after the event: default %.1f, %.1f samples; authentic grid %.1f, %.1f samples\n", a, b, c, d);
        check(std::abs(a - b) < 0.5, "default mode starts notes at the same delay wherever they land");
        check(std::abs(c - d) > 50.0, "authentic grid delays notes to the next frame");
    }
}

void testInstancesAreIndependent()
{
    std::puts("16 instances on separate threads");

    const int numInstances = 16;
    const long length = 48000 * 10;

    std::vector<std::vector<float>> expected, actual((size_t) numInstances);

    for (int i = 0; i < numInstances; ++i)
        expected.push_back(renderSchedule(48000.0, length, melody(100 + i, length), {256}));

    const auto start = std::chrono::steady_clock::now();

    {
        std::vector<std::thread> threads;

        for (int i = 0; i < numInstances; ++i)
            threads.emplace_back([&, i] { actual[(size_t) i] = renderSchedule(48000.0, length, melody(100 + i, length), {256}); });

        for (auto& t : threads)
            t.join();
    }

    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

    bool same = true;
    for (int i = 0; i < numInstances; ++i)
        same = same && expected[(size_t) i] == actual[(size_t) i];

    check(same, "each instance matches its solo render");

    // Single-threaded cost of one instance, which is what a core would see.
    const auto soloStart = std::chrono::steady_clock::now();
    renderSchedule(48000.0, length, melody(100, length), {64});
    const std::chrono::duration<double> solo = std::chrono::steady_clock::now() - soloStart;

    std::printf("  one instance: %.2f%% of one core at 48 kHz, 64-sample blocks\n", solo.count() / 10.0 * 100.0);
    std::printf("  16 parallel instances: %.2f s to render 10 s\n", elapsed.count());
}

// Interleaved stereo, since EPSM is stereo and a difference could be on either side.
std::vector<float> renderFmChip(ChannelType type, int chip, int seed, long length)
{
    NesEngine engine;
    engine.prepare(48000.0, false, chip);
    engine.setChannelType(type);

    const auto events = melody(seed, length);
    std::vector<float> left((size_t) length), right((size_t) length), out;
    long position = 0;
    size_t nextEvent = 0;

    while (position < length)
    {
        while (nextEvent < events.size() && events[nextEvent].position <= position)
        {
            const auto& e = events[nextEvent++];
            if (e.velocity > 0)
                engine.noteOn(e.note, e.velocity);
            else
                engine.noteOff(e.note);
        }

        auto stop = std::min(length, position + 256);
        if (nextEvent < events.size())
            stop = std::min(stop, events[nextEvent].position);

        engine.render(left.data() + position, right.data() + position, (int) (stop - position));
        position = stop;
    }

    out.reserve((size_t) length * 2);
    for (long i = 0; i < length; ++i)
    {
        out.push_back(left[(size_t) i]);
        out.push_back(right[(size_t) i]);
    }

    return out;
}

// These two emulators have static state that the others don't. This has to run before anything
// else makes one of these chips, because VRC7 builds its shared tables only the first time.
void testFmInstancesAreIndependent()
{
    std::puts("VRC7 and EPSM instances on separate threads");

    const int numInstances = 16;
    const long length = 48000 * 2;

    const auto render = [&](int i)
    {
        return i % 2 == 0 ? renderFmChip(ChannelType::vrc7Fm1, NesEngine::chipVrc7, 100 + i, length)
                          : renderFmChip(ChannelType::epsmFm1, NesEngine::chipEpsm, 100 + i, length);
    };

    std::vector<std::vector<float>> actual((size_t) numInstances);

    {
        std::vector<std::thread> threads;

        // The first half start together, and the rest arrive while those are playing,
        // like tracks added to a Set during playback.
        for (int i = 0; i < numInstances; ++i)
            threads.emplace_back([&, i]
            {
                if (i >= numInstances / 2)
                    std::this_thread::sleep_for(std::chrono::milliseconds((i - numInstances / 2 + 1) * 5));

                actual[(size_t) i] = render(i);
            });

        for (auto& t : threads)
            t.join();
    }

    bool same = true, audible = true;

    for (int i = 0; i < numInstances; ++i)
    {
        same = same && render(i) == actual[(size_t) i];
        audible = audible && std::any_of(actual[(size_t) i].begin(), actual[(size_t) i].end(), [](float s) { return std::abs(s) > 0.01f; });
    }

    check(audible, "every instance makes sound");
    check(same, "each instance matches its solo render");
}
} // namespace

int main()
{
    testFmInstancesAreIndependent();
    testChunkingMatchesFrames();
    testOnsetTiming();
    testBlockSizeIndependence();
    testPitchAndLevel();
    testVoiceLogic();
    testChannelsAndFrameClock();
    testInstancesAreIndependent();

    std::printf(failures == 0 ? "\nAll engine tests passed\n" : "\n%d checks failed\n", failures);
    return failures == 0 ? 0 : 1;
}
