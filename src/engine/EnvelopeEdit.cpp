#include "engine/EnvelopeEdit.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>

void setEnvelopeValue(Envelope& env, int type, int index, int value, int maxDuty)
{
    if (index < 0 || index >= Envelope::maxLength)
        return;

    const auto range = envelopeRange(type, maxDuty);

    if (index >= env.length)
        setEnvelopeLength(env, type, index + 1);

    env.values[(size_t) index] = (int8_t) std::clamp(value, range.min, range.max);
}

void setEnvelopeLength(Envelope& env, int type, int length)
{
    length = std::clamp(length, 0, Envelope::maxLength);

    const auto fill = env.length > 0 ? (int) env.values[(size_t) env.length - 1] : Envelope::defaultValue(type);

    for (int i = env.length; i < length; ++i)
        env.values[(size_t) i] = (int8_t) fill;

    env.length = length;

    if (env.loop >= length)
        env.loop = -1;

    if (env.release >= length || env.loop < 0)
        env.release = -1;
}

void setEnvelopeLoop(Envelope& env, int index)
{
    if (index < 0 || env.length == 0)
    {
        env.loop = -1;
        env.release = -1;
        return;
    }

    const auto last = env.release >= 0 ? env.release - 1 : env.length - 1;
    env.loop = std::clamp(index, 0, last);
}

void setEnvelopeRelease(Envelope& env, int type, int index)
{
    if (index < 0 || ! envelopeCanRelease(type) || env.length < 2)
    {
        env.release = -1;
        return;
    }

    const auto release = std::clamp(index, env.loop >= 0 ? env.loop + 1 : 1, env.length - 1);

    if (env.loop < 0)
        env.loop = release - 1;

    env.release = release;
}

void clearEnvelope(Envelope& env)
{
    const auto relative = env.relative;
    env = {};
    env.relative = relative;
}

void sanitizeEnvelope(Envelope& env, int type, int maxDuty)
{
    const auto range = envelopeRange(type, maxDuty);

    env.length = std::clamp(env.length, 0, Envelope::maxLength);

    for (int i = 0; i < env.length; ++i)
        env.values[(size_t) i] = (int8_t) std::clamp((int) env.values[(size_t) i], range.min, range.max);

    if (env.loop >= env.length || env.loop < -1)
        env.loop = -1;

    if (! envelopeCanRelease(type) || env.loop < 0 || env.release >= env.length || env.release <= env.loop)
        env.release = -1;
}

std::string formatEnvelope(const Envelope& env)
{
    std::string text;

    for (int i = 0; i < env.length; ++i)
    {
        if (i == env.loop)
            text += "| ";

        if (i == env.release)
            text += "/ ";

        text += std::to_string((int) env.values[(size_t) i]);

        if (i + 1 < env.length)
            text += ' ';
    }

    return text;
}

bool parseEnvelope(const std::string& text, int type, Envelope& out, std::string& error, int maxDuty)
{
    const auto range = envelopeRange(type, maxDuty);

    Envelope env;
    env.relative = out.relative;

    size_t pos = 0;

    while (pos < text.size())
    {
        const auto c = (unsigned char) text[pos];

        if (std::isspace(c) || c == ',')
        {
            ++pos;
        }
        else if (c == '|')
        {
            if (env.loop >= 0)
            {
                error = "Only one loop marker ( | ) is allowed";
                return false;
            }

            env.loop = env.length;
            ++pos;
        }
        else if (c == '/')
        {
            if (! envelopeCanRelease(type))
            {
                error = "Only volume envelopes can have a release ( / )";
                return false;
            }

            if (env.release >= 0)
            {
                error = "Only one release marker ( / ) is allowed";
                return false;
            }

            env.release = env.length;
            ++pos;
        }
        else
        {
            const auto* start = text.c_str() + pos;
            char* end = nullptr;
            errno = 0;
            const auto value = std::strtol(start, &end, 10);

            if (end == start)
            {
                error = "Unexpected '" + std::string(1, (char) c) + "'. Use numbers, | and /";
                return false;
            }

            if (value < range.min || value > range.max || errno == ERANGE)
            {
                error = text.substr(pos, (size_t) (end - start)) + " is outside " + std::to_string(range.min) + " to " + std::to_string(range.max);
                return false;
            }

            if (env.length == Envelope::maxLength)
            {
                error = "Envelopes can have at most " + std::to_string(Envelope::maxLength) + " steps";
                return false;
            }

            env.values[(size_t) env.length++] = (int8_t) value;
            pos += (size_t) (end - start);
        }
    }

    if (env.loop >= env.length)
    {
        error = "A loop marker ( | ) needs values after it";
        return false;
    }

    if (env.release >= 0)
    {
        if (env.release >= env.length)
        {
            error = "A release marker ( / ) needs values after it";
            return false;
        }

        if (env.release == 0)
        {
            error = "A release marker ( / ) needs values before it";
            return false;
        }

        if (env.loop >= env.release)
        {
            error = "The loop ( | ) must come before the release ( / )";
            return false;
        }

        // Holding on the last value before the release is what a loopless release means.
        if (env.loop < 0)
            env.loop = env.release - 1;
    }

    out = env;
    return true;
}
