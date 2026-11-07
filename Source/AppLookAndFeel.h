#pragma once

#include <JuceHeader.h>
#include "BinaryData.h"
#include "Theme.h"

namespace mixtopia
{
    // Mixtopia's look: the Inter font (bundled, so the app looks the same on every desktop; user's
    // choice 2026-10-21 - JUCE had picked Liberation Sans, an Arial look-alike that's hard to read
    // small) and, since M8, the Graphite + teal theme (Theme.h) for JUCE's own controls: flat
    // buttons, boxes, menus, tooltips, sliders, knobs, scrollbars, table headers and dialogs.
    class AppLookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        AppLookAndFeel()
            : LookAndFeel_V4 (getScheme()),
              regular (juce::Typeface::createSystemTypefaceFor (BinaryData::InterRegular_ttf, BinaryData::InterRegular_ttfSize)),
              bold (juce::Typeface::createSystemTypefaceFor (BinaryData::InterBold_ttf, BinaryData::InterBold_ttfSize))
        {
            using namespace theme;
            const std::pair<int, juce::Colour> colours[] {
                { juce::ResizableWindow::backgroundColourId, bg },
                { juce::DocumentWindow::backgroundColourId, bg },
                { juce::TextButton::buttonColourId, raised },
                { juce::TextButton::buttonOnColourId, accent },
                { juce::TextButton::textColourOffId, text1 },
                { juce::TextButton::textColourOnId, accent },
                { juce::ComboBox::backgroundColourId, raised },
                { juce::ComboBox::outlineColourId, juce::Colours::transparentBlack },
                { juce::ComboBox::textColourId, text1 },
                { juce::ComboBox::arrowColourId, text2 },
                { juce::TextEditor::backgroundColourId, inset },
                { juce::TextEditor::outlineColourId, line },
                { juce::TextEditor::focusedOutlineColourId, accent.withAlpha (0.7f) },
                { juce::TextEditor::textColourId, text1 },
                { juce::TextEditor::highlightColourId, accent.withAlpha (0.35f) },
                { juce::TextEditor::highlightedTextColourId, text1 },
                { juce::CaretComponent::caretColourId, accent },
                { juce::Label::textColourId, text1 },
                { juce::PopupMenu::backgroundColourId, hover },
                { juce::PopupMenu::textColourId, text1 },
                { juce::PopupMenu::highlightedBackgroundColourId, raised.brighter (0.12f) },
                { juce::PopupMenu::highlightedTextColourId, text1 },
                { juce::PopupMenu::headerTextColourId, text3 },
                { juce::TooltipWindow::backgroundColourId, hover },
                { juce::TooltipWindow::textColourId, text1 },
                { juce::TooltipWindow::outlineColourId, line },
                { juce::ListBox::backgroundColourId, panel },
                { juce::ListBox::outlineColourId, juce::Colours::transparentBlack },
                { juce::ScrollBar::thumbColourId, hover.brighter (0.15f) },
                { juce::ScrollBar::trackColourId, juce::Colours::transparentBlack },
                { juce::TableHeaderComponent::backgroundColourId, panel },
                { juce::TableHeaderComponent::textColourId, text3 },
                { juce::TableHeaderComponent::outlineColourId, line },
                { juce::TableHeaderComponent::highlightColourId, hover },
                { juce::AlertWindow::backgroundColourId, panel },
                { juce::AlertWindow::textColourId, text1 },
                { juce::AlertWindow::outlineColourId, line },
                { juce::ProgressBar::backgroundColourId, inset },
                { juce::ProgressBar::foregroundColourId, accent },
                { juce::ToggleButton::textColourId, text1 },
                { juce::ToggleButton::tickColourId, accent },
                { juce::ToggleButton::tickDisabledColourId, text3 },
                { juce::Slider::thumbColourId, text1 },
                { juce::Slider::trackColourId, accent },
                { juce::Slider::backgroundColourId, hover },
                { juce::Slider::rotarySliderFillColourId, accent },
                { juce::Slider::rotarySliderOutlineColourId, hover },
                { juce::Slider::textBoxTextColourId, text1 },
                { juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack },
            };

            for (auto& [id, colour] : colours)
                setColour (id, colour);
        }

        juce::Typeface::Ptr getTypefaceForFont (const juce::Font& font) override
        {
            if (font.getTypefaceName() == juce::Font::getDefaultSansSerifFontName())
                return font.isBold() ? bold : regular;

            return LookAndFeel_V4::getTypefaceForFont (font);
        }

        //==============================================================================
        // Buttons: flat, slightly rounded; a toggle that's on gets the teal tint and border.
        void drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& background, bool over, bool down) override
        {
            using namespace theme;
            auto r = button.getLocalBounds().toFloat();
            auto on = button.getToggleState();

            if (on && background == button.findColour (juce::TextButton::buttonOnColourId) && background == accent)
            {
                g.setColour (accent.withAlpha (down ? 0.26f : over ? 0.22f : 0.16f));
                g.fillRoundedRectangle (r, controlRadius);
                g.setColour (accent.withAlpha (0.55f));
                g.drawRoundedRectangle (r.reduced (0.5f), controlRadius, 1.0f);
                return;
            }

            // Custom colours (M / S when on): a solid fill.
            auto fill = background == raised ? (down ? hover.brighter (0.08f) : over ? hover : raised)
                                             : (over ? background.brighter (0.1f) : background);
            g.setColour (button.isEnabled() ? fill : fill.withAlpha (0.5f));
            g.fillRoundedRectangle (r, controlRadius);
        }

        juce::Font getTextButtonFont (juce::TextButton& b, int) override    { return theme::font (sizeFor (b, theme::textSmall)); }

        void drawButtonText (juce::Graphics& g, juce::TextButton& button, bool, bool) override
        {
            auto colour = button.findColour (button.getToggleState() ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId);
            g.setColour (button.isEnabled() ? colour : theme::text3.withAlpha (0.6f));
            g.setFont (getTextButtonFont (button, button.getHeight()));
            g.drawText (button.getButtonText(), button.getLocalBounds().reduced (4, 0), juce::Justification::centred, true);
        }

        void drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool over, bool) override
        {
            using namespace theme;
            auto box = juce::Rectangle<float> (2.0f, ((float) button.getHeight() - 16.0f) / 2.0f, 16.0f, 16.0f);
            auto on = button.getToggleState();
            g.setColour (on ? accent.withAlpha (0.18f) : over ? hover : raised);
            g.fillRoundedRectangle (box, 3.0f);
            g.setColour (on ? accent : line.brighter (0.3f));
            g.drawRoundedRectangle (box.reduced (0.5f), 3.0f, 1.0f);

            if (on)
                drawIcon (g, Icon::check, box.reduced (2.0f), accent);

            g.setColour (button.isEnabled() ? text1 : text3);
            g.setFont (font (textNormal));
            g.drawText (button.getButtonText(), button.getLocalBounds().withTrimmedLeft (26), juce::Justification::centredLeft, true);
        }

        //==============================================================================
        // Drop-down boxes and text fields.
        void drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box) override
        {
            using namespace theme;
            auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height);
            g.setColour (box.isMouseOver (true) && box.isEnabled() ? hover : raised);
            g.fillRoundedRectangle (r, controlRadius);
            drawIcon (g, Icon::caret, r.removeFromRight ((float) height).withSizeKeepingCentre (16.0f, 16.0f),
                      box.isEnabled() ? text2 : text3.withAlpha (0.5f));
        }

        // A component can ask for bigger text with the property "mixtopiaTextSize" (the Export window does).
        static float sizeFor (juce::Component& c, float normal)    { return (float) c.getProperties().getWithDefault ("mixtopiaTextSize", normal); }

        juce::Font getComboBoxFont (juce::ComboBox& box) override   { return theme::font (sizeFor (box, theme::textSmall + 0.5f)); }

        void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
        {
            label.setBounds (4, 1, box.getWidth() - box.getHeight() - 2, box.getHeight() - 2);
            label.setFont (getComboBoxFont (box));
        }

        void fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor& editor) override
        {
            g.setColour (editor.findColour (juce::TextEditor::backgroundColourId).withMultipliedAlpha (editor.isEnabled() ? 1.0f : 0.6f));
            g.fillRoundedRectangle (0.0f, 0.0f, (float) width, (float) height, theme::controlRadius);
        }

        void drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor& editor) override
        {
            auto focused = editor.hasKeyboardFocus (true) && ! editor.isReadOnly();
            g.setColour (editor.findColour (focused ? juce::TextEditor::focusedOutlineColourId : juce::TextEditor::outlineColourId));
            g.drawRoundedRectangle (0.5f, 0.5f, (float) width - 1.0f, (float) height - 1.0f, theme::controlRadius, 1.0f);
        }

        //==============================================================================
        // Menus (the menu bar, its menus and right-click menus).
        juce::Font getPopupMenuFont() override      { return theme::font (theme::textNormal); }

        void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardHeight, int& width, int& height) override
        {
            if (isSeparator)
            {
                width = 50;
                height = 11;
                return;
            }

            juce::ignoreUnused (standardHeight);
            height = 28;
            width = juce::GlyphArrangement::getStringWidthInt (getPopupMenuFont(), text) + 64;
        }

        void drawPopupMenuBackground (juce::Graphics& g, int width, int height) override
        {
            g.fillAll (theme::hover);
            g.setColour (theme::line.brighter (0.15f));
            g.drawRect (0, 0, width, height);
        }

        void drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive, bool isHighlighted,
                                bool isTicked, bool hasSubMenu, const juce::String& text, const juce::String& shortcutKeyText,
                                const juce::Drawable* icon, const juce::Colour* textColour) override
        {
            using namespace theme;
            juce::ignoreUnused (icon);

            if (isSeparator)
            {
                g.setColour (line.brighter (0.15f));
                g.fillRect (area.reduced (10, 0).withHeight (1).withY (area.getCentreY()));
                return;
            }

            auto r = area.reduced (4, 1);

            if (isHighlighted && isActive)
            {
                g.setColour (findColour (juce::PopupMenu::highlightedBackgroundColourId));
                g.fillRoundedRectangle (r.toFloat(), controlRadius);
            }

            if (isTicked)
                drawIcon (g, Icon::check, r.removeFromLeft (30).toFloat().withSizeKeepingCentre (16.0f, 16.0f), isActive ? accent : text3);
            else
                r.removeFromLeft (30);

            if (hasSubMenu)
            {
                auto a = r.removeFromRight (22).toFloat().withSizeKeepingCentre (5.0f, 9.0f);
                juce::Path chevron;
                chevron.startNewSubPath (a.getTopLeft());
                chevron.lineTo (a.getRight(), a.getCentreY());
                chevron.lineTo (a.getBottomLeft());
                g.setColour (isActive ? text2 : text3);
                g.strokePath (chevron, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }

            auto colour = textColour != nullptr ? *textColour : text1;
            g.setColour (isActive ? colour : text3.withAlpha (0.7f));
            g.setFont (getPopupMenuFont());
            g.drawText (text, r.withTrimmedRight (8), juce::Justification::centredLeft, true);

            if (shortcutKeyText.isNotEmpty())
            {
                g.setColour (text3);
                g.drawText (shortcutKeyText, r.withTrimmedRight (8), juce::Justification::centredRight, true);
            }
        }

        juce::Font getMenuBarFont (juce::MenuBarComponent&, int, const juce::String&) override    { return theme::font (theme::textNormal); }

        int getMenuBarItemWidth (juce::MenuBarComponent& bar, int index, const juce::String& text) override
        {
            return juce::GlyphArrangement::getStringWidthInt (getMenuBarFont (bar, index, text), text) + 24;
        }

        void drawMenuBarBackground (juce::Graphics& g, int width, int height, bool, juce::MenuBarComponent&) override
        {
            g.fillAll (theme::panel);
            g.setColour (theme::line);
            g.fillRect (0, height - 1, width, 1);
        }

        void drawMenuBarItem (juce::Graphics& g, int width, int height, int index, const juce::String& text, bool over, bool open,
                              bool, juce::MenuBarComponent& bar) override
        {
            if (open || over)
            {
                g.setColour (theme::hover);
                g.fillRoundedRectangle (juce::Rectangle<float> (2.0f, 3.0f, (float) width - 4.0f, (float) height - 6.0f), theme::controlRadius);
            }

            g.setColour (theme::text1);
            g.setFont (getMenuBarFont (bar, index, text));
            g.drawText (text, 0, 0, width, height, juce::Justification::centred);
        }

        //==============================================================================
        // Tooltips: 13 px on the menu colour with a hairline border.
        juce::Rectangle<int> getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos, juce::Rectangle<int> parentArea) override
        {
            auto layout = tooltipLayout (tipText);
            auto w = (int) std::ceil (layout.getWidth()) + 20, h = (int) std::ceil (layout.getHeight()) + 14;
            return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 24,
                                         screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6) : screenPos.y + 6, w, h)
                     .constrainedWithin (parentArea);
        }

        void drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height) override
        {
            auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height);
            g.setColour (theme::hover);
            g.fillRoundedRectangle (r, theme::controlRadius);
            g.setColour (theme::line.brighter (0.2f));
            g.drawRoundedRectangle (r.reduced (0.5f), theme::controlRadius, 1.0f);
            tooltipLayout (text).draw (g, r.reduced (10.0f, 7.0f));
        }

        //==============================================================================
        // Sliders: a thin track with a round thumb (the metronome volume); knobs as an arc.
        void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos, float, float,
                               juce::Slider::SliderStyle style, juce::Slider& slider) override
        {
            if (style != juce::Slider::LinearHorizontal)
            {
                LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, 0, 0, style, slider);
                return;
            }

            using namespace theme;
            auto cy = (float) y + (float) height / 2.0f;
            auto left = (float) x + 7.0f, right = (float) (x + width) - 7.0f;
            auto thumbX = juce::jlimit (left, right, sliderPos);
            g.setColour (hover);
            g.fillRoundedRectangle (left, cy - 1.5f, right - left, 3.0f, 1.5f);
            g.setColour (slider.isEnabled() ? accent : text3);
            g.fillRoundedRectangle (left, cy - 1.5f, thumbX - left, 3.0f, 1.5f);
            g.setColour (bg);
            g.fillEllipse (thumbX - 8.5f, cy - 8.5f, 17.0f, 17.0f);
            g.setColour (slider.isMouseOverOrDragging() ? juce::Colours::white : text1);
            g.fillEllipse (thumbX - 6.5f, cy - 6.5f, 13.0f, 13.0f);
        }

        void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float position, float startAngle, float endAngle,
                               juce::Slider& slider) override
        {
            using namespace theme;
            auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height);
            auto radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) / 2.0f - 3.0f;
            auto centre = bounds.getCentre();
            auto angle = startAngle + position * (endAngle - startAngle);

            juce::Path track, value;
            track.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, startAngle, endAngle, true);
            g.setColour (hover);
            g.strokePath (track, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            if (position > 0.001f)
            {
                value.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, startAngle, angle, true);
                g.setColour (slider.isEnabled() ? accent : text3);
                g.strokePath (value, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }

            auto inner = radius - 7.0f;
            g.setColour (slider.isMouseOverOrDragging() ? hover : raised);
            g.fillEllipse (centre.x - inner, centre.y - inner, inner * 2.0f, inner * 2.0f);
            g.setColour (line.brighter (0.2f));
            g.drawEllipse (centre.x - inner, centre.y - inner, inner * 2.0f, inner * 2.0f, 1.0f);

            auto pointer = juce::Point<float> (std::sin (angle), -std::cos (angle));
            g.setColour (text1);
            g.drawLine ({ centre + pointer * (inner * 0.3f), centre + pointer * (inner - 2.0f) }, 2.2f);
        }

        //==============================================================================
        void drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y, int width, int height, bool vertical, int thumbStart,
                            int thumbSize, bool over, bool down) override
        {
            auto thumb = vertical ? juce::Rectangle<float> ((float) x + 2.0f, (float) thumbStart, (float) width - 4.0f, (float) thumbSize)
                                  : juce::Rectangle<float> ((float) thumbStart, (float) y + 3.0f, (float) thumbSize, (float) height - 6.0f);
            g.setColour (down ? theme::text3 : over ? theme::hover.brighter (0.3f) : theme::hover.brighter (0.12f));
            g.fillRoundedRectangle (thumb, juce::jmin (thumb.getWidth(), thumb.getHeight()) / 2.0f);
        }

        int getDefaultScrollbarWidth() override     { return 12; }

        void drawTableHeaderBackground (juce::Graphics& g, juce::TableHeaderComponent& header) override
        {
            g.fillAll (theme::panel);
            g.setColour (theme::line);
            g.fillRect (0, header.getHeight() - 1, header.getWidth(), 1);
        }

        void drawTableHeaderColumn (juce::Graphics& g, juce::TableHeaderComponent&, const juce::String& columnName, int, int width, int height,
                                    bool over, bool down, int columnFlags) override
        {
            if (over || down)
            {
                g.setColour (theme::hover.withAlpha (0.6f));
                g.fillRect (0, 0, width, height - 1);
            }

            auto r = juce::Rectangle<int> (0, 0, width, height).reduced (10, 0);

            if ((columnFlags & (juce::TableHeaderComponent::sortedForwards | juce::TableHeaderComponent::sortedBackwards)) != 0)
            {
                juce::Path arrow;
                auto a = r.removeFromRight (12).toFloat().withSizeKeepingCentre (8.0f, 5.0f);
                arrow.addTriangle (a.getX(), (columnFlags & juce::TableHeaderComponent::sortedForwards) != 0 ? a.getBottom() : a.getY(),
                                   a.getRight(), (columnFlags & juce::TableHeaderComponent::sortedForwards) != 0 ? a.getBottom() : a.getY(),
                                   a.getCentreX(), (columnFlags & juce::TableHeaderComponent::sortedForwards) != 0 ? a.getY() : a.getBottom());
                g.setColour (theme::text2);
                g.fillPath (arrow);
            }

            g.setColour (theme::text3);
            g.setFont (theme::font (theme::textTiny, true));
            g.drawText (columnName.toUpperCase(), r, juce::Justification::centredLeft, true);
        }

        //==============================================================================
        // Dialogs and progress windows.
        juce::Font getAlertWindowTitleFont() override       { return theme::font (16.0f, true); }
        juce::Font getAlertWindowMessageFont() override     { return theme::font (theme::textNormal); }
        juce::Font getAlertWindowFont() override            { return theme::font (theme::textNormal); }

        void drawProgressBar (juce::Graphics& g, juce::ProgressBar& bar, int width, int height, double progress, const juce::String& text) override
        {
            auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height);
            g.setColour (theme::inset);
            g.fillRoundedRectangle (r, theme::controlRadius);

            if (progress >= 0.0 && progress <= 1.0)
            {
                g.setColour (theme::accent);
                g.fillRoundedRectangle (r.withWidth (r.getWidth() * (float) progress), theme::controlRadius);
            }
            else
            {
                // Unknown length: a moving stripe.
                auto t = (float) std::fmod (juce::Time::getMillisecondCounterHiRes() / 1200.0, 1.0);
                g.setColour (theme::accent.withAlpha (0.7f));
                g.fillRoundedRectangle (r.withWidth (r.getWidth() * 0.3f).withX (r.getWidth() * (t * 1.3f - 0.3f)).getIntersection (r),
                                        theme::controlRadius);
                bar.repaint();
            }

            if (text.isNotEmpty())
            {
                g.setColour (theme::text1);
                g.setFont (theme::font (theme::textSmall));
                g.drawText (text, r, juce::Justification::centred);
            }
        }

    private:
        static ColourScheme getScheme()
        {
            using namespace theme;
            // window background, widget background, menu background, outline, default text, default fill,
            // highlighted text, highlighted fill, menu text
            return { bg, raised, hover, line, text1, accent, text1, raised.brighter (0.12f), text1 };
        }

        static juce::TextLayout tooltipLayout (const juce::String& text)
        {
            juce::AttributedString s;
            s.setJustification (juce::Justification::centredLeft);
            s.append (text, theme::font (theme::textNormal), theme::text1);
            juce::TextLayout layout;
            layout.createLayoutWithBalancedLineLengths (s, 420.0f);
            return layout;
        }

        juce::Typeface::Ptr regular, bold;
    };
}
