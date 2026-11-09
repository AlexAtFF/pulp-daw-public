#pragma once
// Colours, fonts and small drawing helpers. The look is PULP's own.
#include <juce_gui_basics/juce_gui_basics.h>

namespace theme {
using juce::Colour;

const Colour bg{0xff24282c};          // workspace behind the windows
const Colour panel{0xff363b41};       // window and toolbar body
const Colour panelHi{0xff444a51};     // raised parts
const Colour panelLo{0xff2a2e33};     // recessed parts
const Colour inset{0xff1b1e21};       // displays, grids
const Colour line{0xff15181a};
const Colour text{0xffd9dee3};
const Colour textDim{0xff8e979f};
const Colour accent{0xffff9a2e};      // tangerine
const Colour accent2{0xff53c7b5};     // teal
const Colour good{0xff7ed957};
const Colour warn{0xffffd04a};
const Colour bad{0xffff5a4f};
const Colour stepOff{0xff565d65};
const Colour stepOffAlt{0xff6b5a57};
const Colour stepOn{0xffe8edf2};

inline juce::Font font(float size, bool bold = false) {
    return juce::Font(juce::FontOptions("Segoe UI", size, bold ? juce::Font::bold : juce::Font::plain));
}
inline juce::Font mono(float size) {
    return juce::Font(juce::FontOptions("Consolas", size, juce::Font::plain));
}

inline void bevel(juce::Graphics& g, juce::Rectangle<float> r, Colour fill, float radius = 3.0f) {
    g.setColour(fill);
    g.fillRoundedRectangle(r, radius);
    g.setColour(fill.brighter(0.12f));
    g.drawHorizontalLine((int)r.getY(), r.getX() + radius, r.getRight() - radius);
    g.setColour(line);
    g.drawRoundedRectangle(r.reduced(0.5f), radius, 1.0f);
}

inline void insetBox(juce::Graphics& g, juce::Rectangle<float> r, float radius = 2.0f) {
    g.setColour(inset);
    g.fillRoundedRectangle(r, radius);
    g.setColour(line);
    g.drawRoundedRectangle(r.reduced(0.5f), radius, 1.0f);
}

// Sixteen channel / clip colours.
inline Colour palette(int i) {
    static const juce::uint32 c[16] = {
        0xff6f8fa6, 0xffb9735a, 0xff7fa66f, 0xffa68f5f, 0xff8b78b0, 0xff5fa69c, 0xffb06f8f, 0xff9aa0a6,
        0xff4f7ea8, 0xffd08a3e, 0xff5b9a57, 0xffc7b24a, 0xff7460b8, 0xff3fa7a0, 0xffc0567a, 0xff78828c};
    return Colour(c[((i % 16) + 16) % 16]);
}

} // namespace theme
