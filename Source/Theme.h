#pragma once

#include <JuceHeader.h>

// Mixtopia's look (M8, user's pick 2026-10-22 "Graphite + teal", Mockups/16-18): flat, dark
// graphite surfaces in a few clear layers, one teal accent for "on" and selection, crisp edges with
// slight rounding, Inter text no smaller than 11 px. Song colours are in ClipColours.h (kept away
// from the accent).
namespace mixtopia::theme
{
    // Surfaces, from the back to the front.
    inline const juce::Colour bg (0xff0f1013),        // window background, gaps between cards
                              panel (0xff17181d),     // cards (toolbar groups, library, timeline, master)
                              raised (0xff202228),    // buttons, ruler, boxes on a card
                              hover (0xff2a2c33),     // mouse over; menus and tooltips
                              inset (0xff0c0d10),     // text fields, meter wells, overview rows
                              line (0xff2b2d35),      // hairlines and card borders
                              laneA (0xff15161b), laneB (0xff18191f), grid (0xff24262d);

    // Text: main, secondary, quiet (labels, scales).
    inline const juce::Colour text1 (0xffe6e7eb), text2 (0xffa3a6b0), text3 (0xff7a7d87);

    inline const juce::Colour accent (0xff2fd4c3),
                              mute (0xffff6b5b), solo (0xfff5d547), warn (0xffe0a83a), good (0xff8be28b),
                              darkText (0xff141414);   // on a bright fill (selected song's title bar, M/S on)

    // Spacing: outer margin, gap between cards, corner radius of cards and of small controls.
    constexpr int margin = 8, gap = 6;
    constexpr float cardRadius = 6.0f, controlRadius = 4.0f;

    // Text sizes (px): nothing smaller than 11 (user).
    constexpr float textTiny = 11.0f, textSmall = 12.0f, textNormal = 13.0f, textLarge = 14.0f, textTime = 22.0f;

    inline juce::Font font (float size, bool bold = false)
    {
        return juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
    }

    // A song colour's dark tint: the body of its clip and overview block.
    inline juce::Colour tint (juce::Colour c, float amount = 0.24f)
    {
        return bg.interpolatedWith (c, amount);
    }

    // A card: panel fill with a hairline border.
    inline void drawCard (juce::Graphics& g, juce::Rectangle<float> r)
    {
        g.setColour (panel);
        g.fillRoundedRectangle (r, cardRadius);
        g.setColour (line);
        g.drawRoundedRectangle (r.reduced (0.5f), cardRadius, 1.0f);
    }

    // A section label: "MASTER", "MIX OVERVIEW", "BPM".
    inline void drawLabel (juce::Graphics& g, const juce::String& t, juce::Rectangle<int> r,
                           juce::Justification j = juce::Justification::centredLeft)
    {
        g.setColour (text3);
        g.setFont (font (textTiny, true));
        g.drawText (t, r, j, true);
    }

    //==============================================================================
    // Icons: original line drawings on a 16 x 16 grid (the same shapes as the mockups).
    enum class Icon { none, importFile, exportFile, play, pause, stop, undo, redo, snap, follow, click, speaker, lane,
                      zoomOut, zoomIn, fit, search, filter, plus, caret, check, topArea, tempoStrip };

    inline void drawIcon (juce::Graphics& g, Icon icon, juce::Rectangle<float> area, juce::Colour colour)
    {
        // Each icon: stroked parts (with their line width) and filled parts, in SVG path syntax.
        struct Part { const char* path; float width; bool filled; };
        auto parts = [] (Icon i) -> std::vector<Part>
        {
            switch (i)
            {
                case Icon::importFile:  return { { "M8 2v8 M5 7l3 3 3-3", 1.6f, false }, { "M2.5 10.5v3h11v-3", 1.6f, false } };
                case Icon::exportFile:  return { { "M8 10V2 M5 5l3-3 3 3", 1.6f, false }, { "M2.5 10.5v3h11v-3", 1.6f, false } };
                case Icon::play:        return { { "M4.5 2.5 L13 8 L4.5 13.5Z", 0, true } };
                case Icon::pause:       return { { "M4 3h3v10H4Z M9 3h3v10H9Z", 0, true } };
                case Icon::stop:        return { { "M3.5 3.5h9v9h-9Z", 0, true } };
                case Icon::undo:        return { { "M5.5 4 L2.5 7 L5.5 10 M2.8 7H10a3.3 3.3 0 0 1 0 6.6H7", 1.6f, false } };
                case Icon::redo:        return { { "M10.5 4 L13.5 7 L10.5 10 M13.2 7H6a3.3 3.3 0 0 0 0 6.6h3", 1.6f, false } };
                case Icon::snap:        return { { "M2 5.5h12 M2 10.5h12 M5.5 2v12 M10.5 2v12", 1.1f, false }, { "M8.4 8.4h4.2v4.2H8.4Z", 0, true } };
                case Icon::follow:      return { { "M4 2v12", 1.8f, false }, { "M7.5 5 L10.5 8 L7.5 11 M10.5 8H4.5", 1.6f, false } };
                case Icon::click:       return { { "M5 14 L6.6 2.5h2.8L11 14Z M8 11.5 L11.8 5", 1.6f, false }, { "M7.3 11.6h1.4", 2.0f, false } };
                case Icon::speaker:     return { { "M2.5 6h2.5l3.5-3v10L5 10H2.5Z", 0, true }, { "M10.8 5.5a3.5 3.5 0 0 1 0 5", 1.6f, false } };
                case Icon::lane:        return { { "M2 4h8 M2 8h8 M2 12h8", 1.8f, false }, { "M13 2v6 M10 5h6", 1.6f, false } };
                case Icon::zoomOut:     return { { "M2.5 7a4.5 4.5 0 1 0 9 0a4.5 4.5 0 1 0 -9 0 M10.3 10.3 L14 14 M4.8 7h4.4", 1.6f, false } };
                case Icon::zoomIn:      return { { "M2.5 7a4.5 4.5 0 1 0 9 0a4.5 4.5 0 1 0 -9 0 M10.3 10.3 L14 14 M4.8 7h4.4 M7 4.8v4.4", 1.6f, false } };
                case Icon::fit:         return { { "M2 3v10 M14 3v10", 1.8f, false }, { "M5 8h6 M7 6 L5 8 L7 10 M9 6 L11 8 L9 10", 1.6f, false } };
                case Icon::search:      return { { "M3 7a4 4 0 1 0 8 0a4 4 0 1 0 -8 0 M10 10 L13.5 13.5", 1.6f, false } };
                case Icon::filter:      return { { "M2.5 3.5h11 L9.2 8.7v4.3 L6.8 14V8.7Z", 1.6f, false } };
                case Icon::plus:        return { { "M8 3v10 M3 8h10", 1.6f, false } };
                case Icon::caret:       return { { "M4.5 6.5 L8 10 L11.5 6.5", 1.6f, false } };
                case Icon::check:       return { { "M3 8.5 L6.5 12 L13 4.5", 2.0f, false } };
                case Icon::topArea:     return { { "M2 2.5h12v11H2Z", 1.4f, false }, { "M2 2.5h12v4.5H2Z", 0, true } };
                case Icon::tempoStrip:  return { { "M2 13h12", 1.2f, false }, { "M2 10h4 L10 5h4", 1.8f, false } };
                case Icon::none:        break;
            }

            return {};
        };

        auto transform = juce::AffineTransform::scale (area.getWidth() / 16.0f, area.getHeight() / 16.0f)
                                                .translated (area.getX(), area.getY());
        g.setColour (colour);

        for (auto& part : parts (icon))
        {
            auto path = juce::Drawable::parseSVGPath (part.path);

            if (part.filled)
            {
                path.applyTransform (transform);
                g.fillPath (path);
            }
            else
            {
                juce::Path stroked;
                juce::PathStrokeType (part.width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded).createStrokedPath (stroked, path);
                stroked.applyTransform (transform);
                g.fillPath (stroked);
            }
        }
    }

    //==============================================================================
    // A flat button with an icon and/or a label. Toggles show "on" with a teal tint, a teal border
    // and a teal icon; "off" is a plain raised button.
    class IconButton : public juce::Button
    {
    public:
        IconButton (Icon i, const juce::String& label = {}) : juce::Button (label), icon (i)
        {
            setWantsKeyboardFocus (false);
        }

        void setIcon (Icon i)       { if (icon != i) { icon = i; repaint(); } }

        // Width that fits the icon and label.
        int getIdealWidth (int height) const
        {
            auto label = getButtonText();

            if (label.isEmpty())
                return height;

            return (icon == Icon::none ? 12 : 31) + juce::GlyphArrangement::getStringWidthInt (font (textSmall), label) + 12;
        }

        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            auto r = getLocalBounds().toFloat();
            auto on = getToggleState();
            auto enabled = isEnabled();

            if (on)
            {
                g.setColour (accent.withAlpha (down ? 0.26f : over ? 0.22f : 0.16f));
                g.fillRoundedRectangle (r, controlRadius);
                g.setColour (accent.withAlpha (0.55f));
                g.drawRoundedRectangle (r.reduced (0.5f), controlRadius, 1.0f);
            }
            else
            {
                g.setColour (! enabled ? raised.withAlpha (0.6f) : down ? hover.brighter (0.08f) : over ? hover : raised);
                g.fillRoundedRectangle (r, controlRadius);
            }

            auto iconColour = on ? accent : ! enabled ? text3.withAlpha (0.6f) : over ? text1 : text2;
            auto label = getButtonText();
            auto iconSize = 16.0f;

            if (label.isEmpty())
            {
                drawIcon (g, icon, r.withSizeKeepingCentre (iconSize, iconSize), iconColour);
                return;
            }

            auto content = r.reduced (9.0f, 0.0f);

            if (icon != Icon::none)
            {
                drawIcon (g, icon, content.removeFromLeft (iconSize).withSizeKeepingCentre (iconSize, iconSize), iconColour);
                content.removeFromLeft (6.0f);
            }

            g.setColour (on ? accent : ! enabled ? text3.withAlpha (0.6f) : text1);
            g.setFont (font (textSmall));
            g.drawText (label, content, icon == Icon::none ? juce::Justification::centred : juce::Justification::centredLeft, true);
        }

    private:
        Icon icon;
    };

    //==============================================================================
    // A button that opens a drop-down menu: its label on the left, a caret on the right; outlined in teal while
    // its menu is open (setMenuOpen).
    class DropDownButton : public juce::TextButton
    {
    public:
        using juce::TextButton::TextButton;

        void setMenuOpen (bool open)    { menuOpen = open; repaint(); }

        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            getLookAndFeel().drawButtonBackground (g, *this, findColour (buttonColourId), over, down);
            auto r = getLocalBounds().toFloat();

            if (menuOpen)
            {
                g.setColour (accent);
                g.drawRoundedRectangle (r.reduced (0.5f), controlRadius, 1.0f);
            }

            auto enabled = isEnabled();
            auto content = r.reduced (10.0f, 0.0f);
            drawIcon (g, Icon::caret, content.removeFromRight (14.0f).withSizeKeepingCentre (14.0f, 14.0f),
                      ! enabled ? text3.withAlpha (0.6f) : over ? text1 : text2);
            g.setColour (! enabled ? text3.withAlpha (0.6f) : text1);
            g.setFont (font (textSmall));
            g.drawText (getButtonText(), content, juce::Justification::centredLeft, true);
        }

    private:
        bool menuOpen = false;
    };
}
