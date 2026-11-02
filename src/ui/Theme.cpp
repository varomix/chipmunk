#include "ui/Theme.h"

#include <array>
#include <cstdint>

namespace theme
{
namespace
{
constexpr int firstGlyph = 0x20, lastGlyph = 0x5f;
constexpr int glyphWidth = 5, glyphHeight = 7, glyphAdvance = 6;

// A 5x7 font from space to underscore. Each byte is a column, with its lowest bit at the top.
constexpr std::array<std::array<uint8_t, glyphWidth>, lastGlyph - firstGlyph + 1> glyphs { {
    { 0x00, 0x00, 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0x5f, 0x00, 0x00 }, { 0x00, 0x07, 0x00, 0x07, 0x00 }, { 0x14, 0x7f, 0x14, 0x7f, 0x14 },
    { 0x24, 0x2a, 0x7f, 0x2a, 0x12 }, { 0x23, 0x13, 0x08, 0x64, 0x62 }, { 0x36, 0x49, 0x56, 0x20, 0x50 }, { 0x00, 0x08, 0x07, 0x03, 0x00 },
    { 0x00, 0x1c, 0x22, 0x41, 0x00 }, { 0x00, 0x41, 0x22, 0x1c, 0x00 }, { 0x2a, 0x1c, 0x7f, 0x1c, 0x2a }, { 0x08, 0x08, 0x3e, 0x08, 0x08 },
    { 0x00, 0x50, 0x30, 0x00, 0x00 }, { 0x08, 0x08, 0x08, 0x08, 0x08 }, { 0x00, 0x00, 0x60, 0x60, 0x00 }, { 0x20, 0x10, 0x08, 0x04, 0x02 },
    { 0x3e, 0x51, 0x49, 0x45, 0x3e }, { 0x00, 0x42, 0x7f, 0x40, 0x00 }, { 0x72, 0x49, 0x49, 0x49, 0x46 }, { 0x21, 0x41, 0x49, 0x4d, 0x33 },
    { 0x18, 0x14, 0x12, 0x7f, 0x10 }, { 0x27, 0x45, 0x45, 0x45, 0x39 }, { 0x3c, 0x4a, 0x49, 0x49, 0x31 }, { 0x41, 0x21, 0x11, 0x09, 0x07 },
    { 0x36, 0x49, 0x49, 0x49, 0x36 }, { 0x46, 0x49, 0x49, 0x29, 0x1e }, { 0x00, 0x00, 0x14, 0x00, 0x00 }, { 0x00, 0x40, 0x34, 0x00, 0x00 },
    { 0x00, 0x08, 0x14, 0x22, 0x41 }, { 0x14, 0x14, 0x14, 0x14, 0x14 }, { 0x00, 0x41, 0x22, 0x14, 0x08 }, { 0x02, 0x01, 0x59, 0x09, 0x06 },
    { 0x3e, 0x41, 0x5d, 0x59, 0x4e }, { 0x7c, 0x12, 0x11, 0x12, 0x7c }, { 0x7f, 0x49, 0x49, 0x49, 0x36 }, { 0x3e, 0x41, 0x41, 0x41, 0x22 },
    { 0x7f, 0x41, 0x41, 0x41, 0x3e }, { 0x7f, 0x49, 0x49, 0x49, 0x41 }, { 0x7f, 0x09, 0x09, 0x09, 0x01 }, { 0x3e, 0x41, 0x41, 0x51, 0x73 },
    { 0x7f, 0x08, 0x08, 0x08, 0x7f }, { 0x00, 0x41, 0x7f, 0x41, 0x00 }, { 0x20, 0x40, 0x41, 0x3f, 0x01 }, { 0x7f, 0x08, 0x14, 0x22, 0x41 },
    { 0x7f, 0x40, 0x40, 0x40, 0x40 }, { 0x7f, 0x02, 0x1c, 0x02, 0x7f }, { 0x7f, 0x04, 0x08, 0x10, 0x7f }, { 0x3e, 0x41, 0x41, 0x41, 0x3e },
    { 0x7f, 0x09, 0x09, 0x09, 0x06 }, { 0x3e, 0x41, 0x51, 0x21, 0x5e }, { 0x7f, 0x09, 0x19, 0x29, 0x46 }, { 0x26, 0x49, 0x49, 0x49, 0x32 },
    { 0x03, 0x01, 0x7f, 0x01, 0x03 }, { 0x3f, 0x40, 0x40, 0x40, 0x3f }, { 0x1f, 0x20, 0x40, 0x20, 0x1f }, { 0x3f, 0x40, 0x38, 0x40, 0x3f },
    { 0x63, 0x14, 0x08, 0x14, 0x63 }, { 0x03, 0x04, 0x78, 0x04, 0x03 }, { 0x61, 0x59, 0x49, 0x4d, 0x43 }, { 0x00, 0x7f, 0x41, 0x41, 0x41 },
    { 0x02, 0x04, 0x08, 0x10, 0x20 }, { 0x00, 0x41, 0x41, 0x41, 0x7f }, { 0x04, 0x02, 0x01, 0x02, 0x04 }, { 0x40, 0x40, 0x40, 0x40, 0x40 },
} };
} // namespace

juce::FontOptions labelFont(float height)
{
    return juce::FontOptions(height, juce::Font::bold).withKerningFactor(0.07f);
}

void drawPanel(juce::Graphics& g, juce::Rectangle<float> area, float corner)
{
    g.setGradientFill(juce::ColourGradient::vertical(panelTop, area.getY(), panel, area.getY() + juce::jmin(area.getHeight(), 90.0f)));
    g.fillRoundedRectangle(area, corner);

    g.setColour(juce::Colours::white.withAlpha(0.07f));
    g.drawRoundedRectangle(area.reduced(1.0f), corner - 1.0f, 1.0f);

    g.setColour(edge);
    g.drawRoundedRectangle(area.reduced(0.5f), corner, 1.0f);
}

void drawScreen(juce::Graphics& g, juce::Rectangle<float> area, float corner)
{
    g.setColour(screen);
    g.fillRoundedRectangle(area, corner);

    g.setGradientFill(juce::ColourGradient::vertical(juce::Colours::black.withAlpha(0.5f), area.getY(), juce::Colours::transparentBlack, area.getY() + 7.0f));
    g.fillRoundedRectangle(area.withHeight(juce::jmin(area.getHeight(), 7.0f)), corner);

    g.setColour(edge);
    g.drawRoundedRectangle(area.reduced(0.5f), corner, 1.0f);
}

juce::Rectangle<int> drawModule(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title)
{
    drawPanel(g, area.toFloat());

    auto strip = area.withWidth(moduleTitleWidth).reduced(1);

    {
        juce::Path shape;
        shape.addRoundedRectangle((float) strip.getX(), (float) strip.getY(), (float) strip.getWidth(), (float) strip.getHeight(), 5.0f, 5.0f, true, false, true, false);
        g.setColour(edge.withAlpha(0.45f));
        g.fillPath(shape);
    }

    {
        juce::Graphics::ScopedSaveState saved(g);
        const auto centre = strip.getCentre().toFloat();

        g.addTransform(juce::AffineTransform::rotation(-juce::MathConstants<float>::halfPi, centre.x, centre.y));
        g.setColour(textDim);
        g.setFont(labelFont(9.0f));
        g.drawText(title, juce::Rectangle<int>(strip.getHeight(), strip.getWidth()).withCentre(strip.getCentre()), juce::Justification::centred, false);
    }

    return area.withTrimmedLeft(moduleTitleWidth);
}

void drawLed(juce::Graphics& g, juce::Point<float> centre, float radius, juce::Colour colour, bool isOn)
{
    const auto bounds = juce::Rectangle<float>(radius * 2.0f, radius * 2.0f).withCentre(centre);

    if (isOn)
    {
        g.setColour(colour.withAlpha(0.25f));
        g.fillEllipse(bounds.expanded(2.5f));
        g.setColour(colour);
        g.fillEllipse(bounds);
        g.setColour(juce::Colours::white.withAlpha(0.45f));
        g.fillEllipse(bounds.reduced(radius * 0.55f).translated(-radius * 0.15f, -radius * 0.2f));
    }
    else
    {
        g.setColour(edge);
        g.fillEllipse(bounds);
        g.setColour(juce::Colours::white.withAlpha(0.14f));
        g.drawEllipse(bounds, 1.0f);
    }
}

bool canDrawPixelText(const juce::String& textToDraw)
{
    for (const auto character : textToDraw.toUpperCase())
        if (character < firstGlyph || character > lastGlyph)
            return false;

    return true;
}

int pixelTextWidth(const juce::String& textToDraw, int dotSize)
{
    return textToDraw.length() * glyphAdvance * dotSize;
}

void drawPixelText(juce::Graphics& g, const juce::String& textToDraw, juce::Rectangle<int> area, int dotSize)
{
    const auto top = area.getCentreY() - glyphHeight * dotSize / 2;
    auto x = area.getX();

    for (const auto character : textToDraw.toUpperCase())
    {
        if (character >= firstGlyph && character <= lastGlyph)
        {
            const auto& glyph = glyphs[(size_t) (character - firstGlyph)];

            for (int column = 0; column < glyphWidth; ++column)
                for (int row = 0; row < glyphHeight; ++row)
                    if ((glyph[(size_t) column] >> row) & 1)
                        g.fillRect(x + column * dotSize, top + row * dotSize, dotSize, dotSize);
        }

        x += glyphAdvance * dotSize;
    }
}

LookAndFeel::LookAndFeel()
{
    using juce::ResizableWindow; using juce::TextEditor; using juce::ComboBox; using juce::ListBox; using juce::ScrollBar; using juce::TooltipWindow;
    using juce::TextButton; using juce::Label; using juce::Slider; using juce::ToggleButton; using juce::PopupMenu;

    setColour(ResizableWindow::backgroundColourId, background);
    setColour(TextEditor::backgroundColourId, screen);
    setColour(TextEditor::textColourId, text);
    setColour(TextEditor::outlineColourId, edge);
    setColour(TextEditor::focusedOutlineColourId, accent);
    setColour(TextEditor::highlightColourId, accent.withAlpha(0.35f));
    setColour(juce::CaretComponent::caretColourId, accent);
    setColour(ComboBox::backgroundColourId, screen);
    setColour(ComboBox::outlineColourId, edge);
    setColour(ComboBox::textColourId, text);
    setColour(ComboBox::arrowColourId, accent);
    setColour(ListBox::backgroundColourId, screen);
    setColour(ListBox::outlineColourId, edge);
    setColour(ScrollBar::thumbColourId, outline.brighter(0.2f));
    setColour(ScrollBar::trackColourId, juce::Colours::transparentBlack);
    setColour(TextButton::buttonColourId, panelRaised);
    setColour(TextButton::buttonOnColourId, accent);
    setColour(TextButton::textColourOffId, text);
    setColour(TextButton::textColourOnId, edge);
    setColour(Label::textColourId, text);
    setColour(Slider::backgroundColourId, panelRaised);
    setColour(Slider::rotarySliderFillColourId, accent);
    setColour(Slider::trackColourId, accent);
    setColour(Slider::thumbColourId, accent);
    setColour(Slider::textBoxTextColourId, text);
    setColour(Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour(Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour(ToggleButton::textColourId, text);
    setColour(ToggleButton::tickColourId, accent);
    setColour(ToggleButton::tickDisabledColourId, textDim);
    setColour(PopupMenu::backgroundColourId, header);
    setColour(PopupMenu::textColourId, text);
    setColour(PopupMenu::highlightedBackgroundColourId, accent);
    setColour(PopupMenu::highlightedTextColourId, edge);
    setColour(TooltipWindow::backgroundColourId, header);
    setColour(TooltipWindow::textColourId, text);
    setColour(TooltipWindow::outlineColourId, outline);
}

void LookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height, float position, float startAngle, float endAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat();
    const auto centre = bounds.getCentre();
    const auto radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f - 1.5f;
    const auto arcWidth = juce::jmax(2.5f, radius * 0.16f);
    const auto arcRadius = radius - arcWidth * 0.5f;
    const auto angle = startAngle + position * (endAngle - startAngle);
    const auto colour = slider.findColour(juce::Slider::rotarySliderFillColourId).withMultipliedAlpha(slider.isEnabled() ? 1.0f : 0.35f);

    const auto arc = [&](float from, float to) {
        juce::Path path;
        path.addCentredArc(centre.x, centre.y, arcRadius, arcRadius, 0.0f, from, to, true);
        return path;
    };

    g.setColour(edge);
    g.strokePath(arc(startAngle, endAngle), juce::PathStrokeType(arcWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Values that can go either way grow out from zero.
    const auto isBipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;
    const auto origin = isBipolar ? startAngle + (float) slider.valueToProportionOfLength(0.0) * (endAngle - startAngle) : startAngle;

    if (std::abs(angle - origin) > 0.01f)
    {
        const auto value = arc(juce::jmin(origin, angle), juce::jmax(origin, angle));

        g.setColour(colour.withMultipliedAlpha(0.22f));
        g.strokePath(value, juce::PathStrokeType(arcWidth + 3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour(colour);
        g.strokePath(value, juce::PathStrokeType(arcWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    const auto capRadius = radius - arcWidth - 2.5f;
    const auto cap = juce::Rectangle<float>(capRadius * 2.0f, capRadius * 2.0f).withCentre(centre);

    g.setColour(juce::Colours::black.withAlpha(0.5f));
    g.fillEllipse(cap.expanded(1.0f).translated(0.0f, 1.0f));

    g.setGradientFill(juce::ColourGradient::vertical(juce::Colour(0xff4b4958), cap.getY(), juce::Colour(0xff201e28), cap.getBottom()));
    g.fillEllipse(cap);

    g.setColour(juce::Colours::white.withAlpha(0.1f));
    g.drawEllipse(cap.reduced(0.5f), 1.0f);

    juce::Path pointer;
    pointer.addRoundedRectangle(-1.0f, -capRadius + 1.0f, 2.0f, capRadius * 0.55f, 1.0f);
    g.setColour(text.withMultipliedAlpha(slider.isEnabled() ? 1.0f : 0.4f));
    g.fillPath(pointer, juce::AffineTransform::rotation(angle).translated(centre.x, centre.y));
}

juce::Label* LookAndFeel::createSliderTextBox(juce::Slider& slider)
{
    auto* label = LookAndFeel_V4::createSliderTextBox(slider);
    label->setFont(juce::FontOptions(11.0f));
    label->setMinimumHorizontalScale(0.8f);
    return label;
}

void LookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour&, bool isOver, bool isDown)
{
    const auto area = button.getLocalBounds().toFloat().reduced(0.5f);
    const auto isOn = button.getToggleState();
    const auto top = isOn ? accent.brighter(0.2f) : panelRaised.brighter(isOver ? 0.25f : 0.08f);
    const auto bottom = isOn ? accent.darker(0.25f) : panelRaised.darker(0.3f);

    g.setGradientFill(juce::ColourGradient::vertical(isDown ? bottom : top, area.getY(), isDown ? top : bottom, area.getBottom()));
    g.setOpacity(button.isEnabled() ? 1.0f : 0.4f);
    g.fillRoundedRectangle(area, 3.5f);

    g.setColour(juce::Colours::white.withAlpha(isOn ? 0.2f : 0.08f));
    g.drawLine(area.getX() + 3.0f, area.getY() + 1.0f, area.getRight() - 3.0f, area.getY() + 1.0f, 1.0f);

    g.setColour(edge);
    g.drawRoundedRectangle(area, 3.5f, 1.0f);
}

void LookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button, bool, bool)
{
    g.setColour((button.getToggleState() ? edge : text).withMultipliedAlpha(button.isEnabled() ? 1.0f : 0.35f));
    g.setFont(labelFont());
    g.drawFittedText(button.getButtonText().toUpperCase(), button.getLocalBounds().reduced(2, 0), juce::Justification::centred, 1, 0.8f);
}

void LookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& button, bool isOver, bool)
{
    const auto isOn = button.getToggleState();
    const auto area = button.getLocalBounds();

    drawLed(g, { 7.0f, (float) area.getCentreY() }, 3.5f, accent.withMultipliedAlpha(button.isEnabled() ? 1.0f : 0.4f), isOn);

    g.setColour((isOn || isOver ? text : textDim).withMultipliedAlpha(button.isEnabled() ? 1.0f : 0.4f));
    g.setFont(labelFont());
    g.drawFittedText(button.getButtonText().toUpperCase(), area.withTrimmedLeft(16), juce::Justification::centredLeft, 1, 0.8f);
}

void LookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box)
{
    const auto area = juce::Rectangle<int>(width, height).toFloat();

    drawScreen(g, area, 3.5f);

    if (box.hasKeyboardFocus(false))
    {
        g.setColour(accent.withAlpha(0.5f));
        g.drawRoundedRectangle(area.reduced(0.5f), 3.5f, 1.0f);
    }

    const auto centre = juce::Point<float>((float) width - 10.0f, (float) height * 0.5f);

    juce::Path arrow;
    arrow.startNewSubPath(centre.x - 3.5f, centre.y - 1.5f);
    arrow.lineTo(centre.x, centre.y + 2.0f);
    arrow.lineTo(centre.x + 3.5f, centre.y - 1.5f);

    g.setColour(accent.withMultipliedAlpha(box.isEnabled() ? 1.0f : 0.35f));
    g.strokePath(arrow, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Font LookAndFeel::getComboBoxFont(juce::ComboBox&)
{
    return juce::FontOptions(12.5f);
}

void LookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label)
{
    label.setBounds(2, 1, box.getWidth() - 20, box.getHeight() - 2);
    label.setFont(getComboBoxFont(box));
}

void LookAndFeel::fillTextEditorBackground(juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    g.setColour(editor.findColour(juce::TextEditor::backgroundColourId));
    g.fillRoundedRectangle(juce::Rectangle<int>(width, height).toFloat(), 3.5f);
}

void LookAndFeel::drawTextEditorOutline(juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    const auto isFocused = editor.hasKeyboardFocus(true) && ! editor.isReadOnly();

    g.setColour(editor.findColour(isFocused ? juce::TextEditor::focusedOutlineColourId : juce::TextEditor::outlineColourId));
    g.drawRoundedRectangle(juce::Rectangle<int>(width, height).toFloat().reduced(0.5f), 3.5f, 1.0f);
}

juce::Font LookAndFeel::getPopupMenuFont()
{
    return juce::FontOptions(13.0f);
}
} // namespace theme
