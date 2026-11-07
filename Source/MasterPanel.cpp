#include "MasterPanel.h"
#include "MasterLimiter.h"
#include "ClipMixer.h"
#include "ClipColours.h"
#include "LevelMatch.h"
#include "EditHelpers.h"
#include "SongNames.h"
#include "Theme.h"

namespace theme = mixtopia::theme;

namespace
{
    constexpr int ticksPerSecond = 60;
    constexpr float scaleTopLufs = 0.0f, scaleBottomLufs = -20.0f;
    constexpr double bumpDb = 2.0;               // the Mix bar turns orange this far above the loudest song's line
    constexpr double animationSeconds = 0.04;    // time constant: a resize is ~95% done after 0.12 s
    const juce::Colour mixColour (theme::text1), bumpColour (0xffff9f43), limiterColour (theme::warn),
                       emptyColour (theme::inset), gridColour (theme::line);

    float lufsToFraction (double lufs)
    {
        return juce::jlimit (0.0f, 1.0f, ((float) lufs - scaleBottomLufs) / (scaleTopLufs - scaleBottomLufs));
    }

    juce::String formatLufs (double lufs)
    {
        return lufs > -99.0 && std::isfinite (lufs) ? juce::String (lufs, 1) : juce::String ("-");
    }

    ClipMixerPlugin* findMixer (te::AudioClipBase& clip)
    {
        if (auto* list = clip.getPluginList())
            for (auto* plugin : *list)
                if (auto* mixer = dynamic_cast<ClipMixerPlugin*> (plugin))
                    return mixer;

        return nullptr;
    }
}

MasterPanel::MasterPanel()
{
    push.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    push.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    push.setRange (0.0, MasterLimiterPlugin::maxPushDb, 0.1);
    push.setTextValueSuffix (" dB");
    push.setDoubleClickReturnValue (true, 0.0);
    push.setTooltip ("Pushes the mix into the limiter: louder, but too much flattens the punch (watch the Limiting bar). "
                     "0 dB = no push. Double-click to reset.");
    push.setWantsKeyboardFocus (false);
    push.onValueChange = [this] { if (auto* l = getLimiter()) l->setPush ((float) push.getValue()); repaint (limiterArea); };

    addChildComponent (push);
    startTimerHz (ticksPerSecond);
}

void MasterPanel::setLimiterControlVisible (bool shouldShow)
{
    push.setVisible (shouldShow);
    resized();
    repaint();
}

void MasterPanel::setEdit (te::Edit* newEdit)
{
    edit = newEdit;
    songs.clear();
    slots = 2.0f;
    mixMeter.reset();
    wasPlaying = false;
    mixLufs = -100.0;
    refreshControls();
}

MasterLimiterPlugin* MasterPanel::getLimiter() const
{
    if (edit != nullptr)
        for (auto* plugin : edit->getMasterPluginList())
            if (auto* limiter = dynamic_cast<MasterLimiterPlugin*> (plugin))
                return limiter;

    return nullptr;
}

void MasterPanel::refreshControls()
{
    if (auto* l = getLimiter())
        push.setValue (l->driveDb.get(), juce::dontSendNotification);
}

void MasterPanel::resetSongMeters()
{
    for (auto& song : songs)
    {
        song.afterLines.reset();
    }
}

void MasterPanel::updateSongs (double dt)
{
    auto& transport = edit->getTransport();
    auto playing = transport.isPlaying();
    auto now = transport.getPosition();

    // A jump (click on the ruler, scrub) while playing: the readings start again at the new place.
    if (playing && wasPlaying && std::abs (now.inSeconds() - (lastPosition + dt)) > 0.5)
        resetSongMeters();

    lastPosition = now.inSeconds();

    // The songs under the playhead, on lanes you can hear, in start-time order.
    std::vector<te::AudioClipBase*> under;

    for (auto* track : te::getAudioTracks (*edit))
        if (mixtopia::isLaneAudible (*track))
            for (auto* c : track->getClips())
                if (auto* clip = dynamic_cast<te::AudioClipBase*> (c); clip != nullptr && clip->getPosition().time.contains (now))
                    under.push_back (clip);

    std::stable_sort (under.begin(), under.end(), [] (auto* a, auto* b) { return a->getPosition().getStart() < b->getPosition().getStart(); });

    // Pieces of one chopped song playing at once (a stacked hook) share one bar (user, 2026-10-27).
    auto familyOf = [] (const te::AudioClipBase& clip)
    {
        return mixtopia::chops::isPiece (clip) ? "song:" + mixtopia::chops::songKey (clip) : "clip:" + clip.itemID.toString();
    };

    std::vector<Song> updated;

    for (size_t i = 0; i < under.size(); ++i)
    {
        auto* clip = under[i];
        auto family = familyOf (*clip);

        if (std::any_of (updated.begin(), updated.end(), [&] (const Song& s) { return s.id == family; }))
            continue;

        auto existing = std::find_if (songs.begin(), songs.end(), [&] (const Song& s) { return s.id == family; });
        Song song;

        if (existing != songs.end())
        {
            song = std::move (*existing);
        }
        else
        {
            song.id = family;
            song.position = (float) updated.size();   // appears in place; only the others slide
        }

        song.name = mixtopia::shortTitle (clip->getName());
        song.colour = mixtopia::clipcolours::get (*clip);
        auto lufs = mixtopia::levelmatch::getLoudness (*clip);
        song.normalLufs = lufs && std::isfinite (*lufs) ? std::optional (*lufs + mixtopia::levelmatch::getAppliedGainDb (*clip)) : std::nullopt;
        updated.push_back (std::move (song));
    }

    songs = std::move (updated);

    // Collect every song's loudness steps (so none pile up); keep those of the songs under the playhead.
    // Pieces of one song add up step by step (their sound together).
    std::array<double, 64> steps;
    std::map<juce::String, std::vector<double>> summed;

    for (auto* track : te::getAudioTracks (*edit))
        for (auto* c : track->getClips())
            if (auto* clip = dynamic_cast<te::AudioClipBase*> (c))
                if (auto* mixer = findMixer (*clip))
                {
                    auto family = familyOf (*clip);
                    auto song = std::find_if (songs.begin(), songs.end(), [&] (const Song& s) { return s.id == family; });
                    std::vector<double> mine;

                    while (auto num = mixer->takeLoudnessSteps (steps.data(), (int) steps.size()))
                        mine.insert (mine.end(), steps.begin(), steps.begin() + num);

                    if (song == songs.end() || ! playing)
                        continue;

                    auto& total = summed[family];

                    for (size_t i = 0; i < mine.size(); ++i)
                    {
                        if (i < total.size())
                            total[i] += mine[i];
                        else
                            total.push_back (mine[i]);
                    }
                }

    for (auto& [family, total] : summed)
        if (auto song = std::find_if (songs.begin(), songs.end(), [&] (const Song& s) { return s.id == family; }); song != songs.end())
            for (auto step : total)
                song->afterLines.addStep (step);

    // Animate: the number of slots (at least two) and each bar's place.
    auto k = (float) (1.0 - std::exp (-dt / animationSeconds));
    auto targetSlots = (float) std::max<size_t> (2, songs.size());
    slots = std::abs (targetSlots - slots) < 0.002f ? targetSlots : slots + (targetSlots - slots) * k;

    for (size_t i = 0; i < songs.size(); ++i)
    {
        auto& p = songs[i].position;
        p = std::abs ((float) i - p) < 0.002f ? (float) i : p + ((float) i - p) * k;
    }
}

void MasterPanel::timerCallback()
{
    auto dt = 1.0 / ticksPerSecond;
    auto* limiter = getLimiter();

    reductionDb = std::max (limiter != nullptr ? limiter->takePeakGainReductionDb() : 0.0f, reductionDb * 0.92f);

    if (edit != nullptr && limiter != nullptr)
    {
        auto playing = edit->getTransport().isPlaying();

        if (playing != wasPlaying)
        {
            resetSongMeters();

            if (playing)
                mixMeter.reset();
        }

        updateSongs (dt);
        wasPlaying = playing;

        std::array<double, 64> steps;

        while (auto num = limiter->takeLoudnessSteps (steps.data(), (int) steps.size()))
            for (int i = 0; i < num; ++i)
                mixMeter.addStep (steps[(size_t) i]);

        mixLufs = playing ? mixMeter.getShortTerm() : -100.0;
    }

    repaint (meterArea.getUnion (reductionArea));
}

void MasterPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    theme::drawLabel (g, "MASTER", { 14, 10, 100, 16 });

    // Layout: scale | song bars | Mix bar; values and names underneath.
    auto area = meterArea;
    auto labels = area.removeFromBottom (30);
    auto scale = area.removeFromLeft (30);
    auto mixBar = area.removeFromRight (24);
    area.removeFromRight (8);
    auto songArea = area;
    auto yFor = [&] (double lufs) { return (float) songArea.getBottom() - lufsToFraction (lufs) * (float) songArea.getHeight(); };

    g.setFont (theme::font (theme::textTiny));

    for (auto v : { 0, -5, -10, -15, -20 })
    {
        auto y = yFor (v);
        g.setColour (theme::text3);
        g.drawText (juce::String (v), scale.getX(), (int) y - 7, 24, 14, juce::Justification::centredRight);
        g.setColour (gridColour);
        g.drawHorizontalLine ((int) y, (float) songArea.getX(), (float) mixBar.getRight());
    }

    // Song bars (clipped to their area while sliding/resizing).
    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (songArea.getX(), songArea.getY(), songArea.getWidth(), songArea.getHeight() + labels.getHeight());

        constexpr float gap = 6.0f;
        auto barWidth = ((float) songArea.getWidth() - gap * (slots - 1.0f)) / slots;
        auto xFor = [&] (float position) { return (float) songArea.getX() + position * (barWidth + gap); };
        auto small = slots > 2.5f;

        for (int i = 0; i < (int) std::ceil (slots - 0.001f); ++i)
        {
            g.setColour (emptyColour);
            g.fillRoundedRectangle (juce::Rectangle<float> (xFor ((float) i), (float) songArea.getY(), barWidth, (float) songArea.getHeight()), 2.0f);

            if (i >= (int) songs.size())
            {
                g.setColour (theme::text3);
                g.setFont (theme::font (theme::textTiny));
                g.drawText ("-", juce::Rectangle<float> (xFor ((float) i), (float) labels.getY() + 2.0f, barWidth, 14.0f), juce::Justification::centred);
            }
        }

        for (auto& song : songs)
        {
            auto x = xFor (song.position);
            auto level = song.afterLines.getShortTerm();

            if (std::isfinite (level))
            {
                auto top = yFor (level);
                g.setColour (song.colour);
                g.fillRoundedRectangle (juce::Rectangle<float> (x, top, barWidth, (float) songArea.getBottom() - top), 2.0f);
            }

            if (song.normalLufs)
            {
                auto y = yFor (*song.normalLufs);
                g.setColour (juce::Colours::white.withAlpha (0.85f));
                juce::Line<float> line (x, y, x + barWidth, y);
                const float dashes[] = { 4.0f, 3.0f };
                g.drawDashedLine (line, dashes, 2, 2.0f);
            }

            g.setColour (song.colour);
            g.setFont (theme::font (theme::textTiny, true));
            g.drawText (formatLufs (level), juce::Rectangle<float> (x, (float) labels.getY() + 2.0f, barWidth, 14.0f), juce::Justification::centred);
            g.setFont (theme::font (theme::textTiny));
            g.drawText (song.name, juce::Rectangle<float> (x - 2.0f, (float) labels.getY() + 16.0f, barWidth + 4.0f, 14.0f),
                        juce::Justification::centred, true);
            juce::ignoreUnused (small);
        }
    }

    // Mix (total output); orange above the loudest song's line + 2 dB.
    {
        g.setColour (emptyColour);
        g.fillRoundedRectangle (mixBar.toFloat(), 2.0f);

        std::optional<double> loudestLine;

        for (auto& song : songs)
            if (song.normalLufs)
                loudestLine = std::max (loudestLine.value_or (-100.0), *song.normalLufs);

        auto bumpFrom = loudestLine ? *loudestLine + bumpDb : 100.0;
        auto isBump = mixLufs > bumpFrom;

        if (mixLufs > -99.0)
        {
            auto bottom = (float) mixBar.getBottom();
            auto split = yFor (std::min (mixLufs, bumpFrom)), top = yFor (mixLufs);
            g.setColour (mixColour);
            g.fillRect (juce::Rectangle<float> ((float) mixBar.getX(), split, (float) mixBar.getWidth(), bottom - split));

            if (isBump)
            {
                g.setColour (bumpColour);
                g.fillRect (juce::Rectangle<float> ((float) mixBar.getX(), top, (float) mixBar.getWidth(), split - top));
            }
        }

        g.setColour (isBump ? bumpColour : mixColour);
        g.setFont (theme::font (theme::textTiny, true));
        g.drawText (formatLufs (mixLufs), mixBar.getX() - 8, labels.getY() + 2, mixBar.getWidth() + 16, 14, juce::Justification::centred);
        g.setColour (theme::text2);
        g.setFont (theme::font (theme::textTiny));
        g.drawText ("Mix", mixBar.getX() - 8, labels.getY() + 16, mixBar.getWidth() + 16, 14, juce::Justification::centred);
    }

    // The limiter: its gain reduction (a bar growing from the left, 0..12 dB) and, if shown, the push knob.
    g.setColour (theme::raised);
    g.fillRoundedRectangle (limiterArea.toFloat(), 5.0f);
    auto gr = reductionArea;
    auto heading = gr.removeFromTop (16);
    theme::drawLabel (g, "LIMITER", heading);
    g.setColour (reductionDb > 0.1f ? limiterColour : theme::text3);
    g.setFont (theme::font (theme::textTiny, true));
    g.drawText (reductionDb > 0.1f ? juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92")) + juce::String (reductionDb, 1) + " dB" : juce::String ("idle"),
                heading, juce::Justification::centredRight);
    auto bar = gr.removeFromTop (16).withSizeKeepingCentre (gr.getWidth(), 6).toFloat();
    g.setColour (emptyColour);
    g.fillRoundedRectangle (bar, 3.0f);
    g.setColour (limiterColour);
    g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, reductionDb / 12.0f)), 3.0f);

    if (push.isVisible())
    {
        theme::drawLabel (g, "PUSH", push.getBounds().withY (push.getY() - 18).withHeight (16), juce::Justification::centred);
        auto value = push.getValue();
        g.setColour (theme::text1);
        g.setFont (theme::font (theme::textNormal, true));
        g.drawText ((value > 0.05 ? "+" : "") + juce::String (value, 1) + " dB", push.getBounds().withY (push.getBottom() + 2).withHeight (16),
                    juce::Justification::centred);
    }
}

void MasterPanel::resized()
{
    auto r = getLocalBounds().reduced (8);
    r.removeFromTop (24);

    // The limiter box at the bottom (gain reduction; the push knob under it when shown); the meters
    // get the rest.
    limiterArea = r.removeFromBottom (push.isVisible() ? 150 : 52);
    r.removeFromBottom (12);
    meterArea = r.withTrimmedTop (6);

    auto inside = limiterArea.reduced (10, 8);
    reductionArea = inside.removeFromTop (32);

    if (push.isVisible())
        push.setBounds (inside.withTrimmedTop (22).withTrimmedBottom (20).withSizeKeepingCentre (64, 64));
}

juce::String MasterPanel::getTooltip()
{
    auto mouse = getMouseXYRelative();

    if (meterArea.contains (mouse))
        return "Loudness over the last 3 s (LUFS). One bar per song under the playhead; the dashed line is the song's normal "
               "loudness. Mix = all songs together before the limiter; orange = more than 2 dB above the loudest song's line.";

    return {};
}
