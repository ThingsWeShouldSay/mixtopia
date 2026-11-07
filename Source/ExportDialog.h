#pragma once

#include <JuceHeader.h>
#include "Exporter.h"
#include "Theme.h"

// File > Export...: format, Normalize, the tags and the cover image, then Cancel / Export... (teal, bottom right).
// Our own window since 2026-11-03 (user: JUCE's alert window was too small and its 22 px text boxes hard to read):
// 16 px text in 36 px boxes, labels above them, Genre and Year side by side. Enter = Export, Esc = Cancel.
// ExportDialog::run() shows it modally and returns true for Export.
namespace mixtopia
{
    class ExportDialog : public juce::Component
    {
    public:
        struct Choices
        {
            int format = 1;                 // 1 = MP3, 2 = FLAC
            bool normalise = true;
            juce::String artist, album, genre, year;
            juce::File artwork;             // as chosen (may be missing / unusable: getUsableArtwork)
        };

        explicit ExportDialog (const Choices& initial)
        {
            using namespace theme;
            setUpText (intro, 14.0f, text2);
            intro.setText ("Exports exactly what you hear: through the master limiter, without muted lanes or the metronome.",
                           juce::dontSendNotification);
            intro.setJustificationType (juce::Justification::topLeft);

            format.addItem ("MP3 (320 kbps)", 1);
            format.addItem ("FLAC (16-bit, 44.1 kHz - lossless)", 2);
            format.setSelectedId (initial.format, juce::dontSendNotification);
            format.setWantsKeyboardFocus (false);   // (Enter must reach Export)

            normalise.setButtonText ("Normalize: raise a quiet mix to full level (never lowers it)");
            normalise.setToggleState (initial.normalise, juce::dontSendNotification);
            normalise.setWantsKeyboardFocus (false);

            const juce::String values[] { initial.artist, initial.album, initial.genre, initial.year };

            for (int i = 0; i < numFields; ++i)
            {
                auto& box = fields[i];
                box.setFont (font (fieldTextSize));
                box.setIndents (10, 0);
                box.setJustification (juce::Justification::centredLeft);
                box.setEscapeAndReturnKeysConsumed (false);   // Enter = Export, Esc = Cancel
                box.setSelectAllWhenFocused (true);
                box.setText (values[i], false);
                setUpText (fieldLabels[i], textLarge, text2);
            }

            fieldLabels[artist].setText ("Artist (also the album artist)", juce::dontSendNotification);
            fieldLabels[album].setText ("Album", juce::dontSendNotification);
            fieldLabels[genre].setText ("Genre", juce::dontSendNotification);
            fieldLabels[year].setText ("Year", juce::dontSendNotification);
            fields[year].setInputRestrictions (4, "0123456789");

            setUpText (formatLabel, textLarge, text2);
            formatLabel.setText ("Format", juce::dontSendNotification);
            setUpText (coverLabel, textLarge, text2);
            coverLabel.setText ("Cover image", juce::dontSendNotification);
            setUpText (coverName, fieldTextSize, text1);

            choose.onClick = [this]
            {
                juce::FileChooser chooser ("Choose the cover image", artwork != juce::File() ? artwork.getParentDirectory()
                                                                                             : juce::File::getSpecialLocation (juce::File::userPicturesDirectory),
                                           "*.jpg;*.jpeg;*.png");

                if (chooser.browseForFileToOpen())
                    setArtwork (chooser.getResult());
            };
            remove.onClick = [this] { setArtwork ({}); };
            setArtwork (initial.artwork);

            exportButton.setColour (juce::TextButton::buttonColourId, accent);
            exportButton.setColour (juce::TextButton::textColourOffId, bg);
            exportButton.onClick = [this] { finish (1); };
            cancelButton.onClick = [this] { finish (0); };

            for (auto* b : { &choose, &remove, &exportButton, &cancelButton })
            {
                b->setWantsKeyboardFocus (false);
                b->getProperties().set ("mixtopiaTextSize", textLarge);
            }

            format.getProperties().set ("mixtopiaTextSize", fieldTextSize);

            for (auto* c : std::initializer_list<juce::Component*> { &intro, &formatLabel, &format, &normalise, &coverLabel, &coverName,
                                                                     &choose, &remove, &exportButton, &cancelButton })
                addAndMakeVisible (c);

            for (int i = 0; i < numFields; ++i)
            {
                addAndMakeVisible (fieldLabels[i]);
                addAndMakeVisible (fields[i]);
            }

            setWantsKeyboardFocus (true);
            setSize (640, 550);
        }

        // Shows the window (modal) over `parent`; true = Export, with the choices in `choices`.
        static bool run (juce::Component* parent, Choices& choices)
        {
            ExportDialog dialog (choices);
            juce::DialogWindow::LaunchOptions options;
            options.content.setNonOwned (&dialog);
            options.dialogTitle = "Export the mix";
            options.dialogBackgroundColour = theme::panel;
            options.componentToCentreAround = parent;
            options.useNativeTitleBar = true;
            options.resizable = false;
            options.escapeKeyTriggersCloseButton = true;

            if (options.runModal() != 1)
                return false;

            choices = dialog.getChoices();
            return true;
        }

        Choices getChoices() const
        {
            return { format.getSelectedId(), normalise.getToggleState(), fields[artist].getText().trim(), fields[album].getText().trim(),
                     fields[genre].getText().trim(), fields[year].getText().trim(), artwork };
        }

        // The image to embed: none if it's gone missing or can't be used (the export goes ahead without it).
        static juce::File getUsableArtwork (const juce::File& f)   { return f != juce::File() && exporter::checkArtwork (f).isEmpty() ? f : juce::File(); }

        void resized() override
        {
            auto r = getLocalBounds().reduced (pad, 20);
            intro.setBounds (r.removeFromTop (40));
            r.removeFromTop (8);

            auto labelled = [] (juce::Label& label, juce::Component& c, juce::Rectangle<int> row)
            {
                label.setBounds (row.removeFromTop (labelHeight));
                c.setBounds (row.removeFromTop (rowHeight));
            };
            auto nextRow = [&r] { auto row = r.removeFromTop (labelHeight + rowHeight); r.removeFromTop (gap); return row; };

            labelled (formatLabel, format, nextRow());
            normalise.setBounds (r.removeFromTop (rowHeight));
            r.removeFromTop (gap);
            labelled (fieldLabels[artist], fields[artist], nextRow());
            labelled (fieldLabels[album], fields[album], nextRow());

            auto row = nextRow();
            auto yearArea = row.removeFromRight (120);
            row.removeFromRight (12);
            labelled (fieldLabels[genre], fields[genre], row);
            labelled (fieldLabels[year], fields[year], yearArea);

            auto cover = nextRow();
            coverLabel.setBounds (cover.removeFromTop (labelHeight));
            remove.setBounds (cover.removeFromRight (90));
            cover.removeFromRight (8);
            choose.setBounds (cover.removeFromRight (100));
            cover.removeFromRight (12);
            coverName.setBounds (cover);

            auto buttons = getLocalBounds().reduced (pad, 20).removeFromBottom (rowHeight);
            exportButton.setBounds (buttons.removeFromRight (120));
            buttons.removeFromRight (10);
            cancelButton.setBounds (buttons.removeFromRight (100));
        }

        void paint (juce::Graphics& g) override
        {
            // A line over the buttons.
            g.setColour (theme::line);
            g.fillRect (0, getHeight() - 20 - rowHeight - 16, getWidth(), 1);
        }

        bool keyPressed (const juce::KeyPress& key) override
        {
            if (key == juce::KeyPress (juce::KeyPress::returnKey))
            {
                finish (1);
                return true;
            }

            return false;   // (Esc: the window's close button)
        }

    private:
        enum { artist, album, genre, year, numFields };
        static constexpr int pad = 24, labelHeight = 22, rowHeight = 36, gap = 12;
        static constexpr float fieldTextSize = 16.0f;

        static void setUpText (juce::Label& label, float size, juce::Colour colour)
        {
            label.setFont (theme::font (size));
            label.setColour (juce::Label::textColourId, colour);
            label.setBorderSize ({});
            label.setMinimumHorizontalScale (1.0f);
        }

        void setArtwork (const juce::File& f)
        {
            artwork = f;
            auto problem = artwork != juce::File() ? exporter::checkArtwork (artwork) : juce::String();
            coverName.setText (artwork == juce::File() ? juce::String ("none")
                                                       : artwork.getFileName() + (problem.isNotEmpty() ? "  (can't be used - none)" : ""),
                               juce::dontSendNotification);
            coverName.setTooltip (problem.isNotEmpty() ? problem : artwork.getFullPathName());
            coverName.setColour (juce::Label::textColourId, problem.isNotEmpty() ? theme::warn : theme::text1);
            remove.setEnabled (artwork != juce::File());
        }

        void finish (int result)
        {
            if (auto* window = findParentComponentOfClass<juce::DialogWindow>())
                window->exitModalState (result);
        }

        juce::Label intro, formatLabel, coverLabel, coverName, fieldLabels[numFields];
        juce::ComboBox format;
        juce::ToggleButton normalise;
        juce::TextEditor fields[numFields];
        juce::TextButton choose { "Choose..." }, remove { "Remove" }, exportButton { "Export..." }, cancelButton { "Cancel" };
        juce::File artwork;
    };
}
