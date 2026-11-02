#pragma once

#include <juce_core/juce_core.h>

#include <cmath>

// Holding Shift over a graph adjusts one step, slowly and from its current value, until the mouse
// is released. It stays on if Shift comes up first, or the step would jump to the mouse's height.
struct FineAdjust
{
    bool active = false;
    int step = 0;

    void start(int stepToAdjust, int currentValue, float y)
    {
        active = true;
        step = stepToAdjust;
        value = (float) currentValue;
        lastY = y;
    }

    // Returns the step's new value. Moving back reverses straight away, however far the mouse went past the limit.
    int moveTo(float y, float rowHeight, int minValue, int maxValue)
    {
        const auto pixelsPerValue = juce::jlimit(12.0f, 32.0f, rowHeight * 4.0f);

        value = juce::jlimit((float) minValue, (float) maxValue, value + (lastY - y) / pixelsPerValue);
        lastY = y;

        return (int) std::lround(value);
    }

private:
    float value = 0.0f, lastY = 0.0f;
};
