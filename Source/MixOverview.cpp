#include "MixOverview.h"
#include "ClipColours.h"
#include "SongNames.h"
#include "Theme.h"
#include "Loops.h"
#include "Chops.h"

namespace theme = mixtopia::theme;

namespace
{
    const juce::Colour highlightColour (theme::solo), rangeColour (theme::accent);
    constexpr double highlightMs = 12000.0;
}

MixOverview::MixOverview()
{
    startTimerHz (15);
}

void MixOverview::setTimeline (TimelineComponent* t, te::Edit* e)
{
    timeline = t;
    edit = e;
    highlighted = {};
    repaint();
}

void MixOverview::highlight (const std::vector<te::EditItemID>& ids)
{
    highlighted = ids;
    highlightUntilMs = juce::Time::getMillisecondCounterHiRes() + highlightMs;
    repaint();
}

void MixOverview::timerCallback()
{
    if (! highlighted.empty() && juce::Time::getMillisecondCounterHiRes() > highlightUntilMs)
        highlighted.clear();

    repaint();
}

std::vector<MixOverview::Block> MixOverview::getBlocks() const
{
    std::vector<Block> blocks;

    if (timeline == nullptr || edit == nullptr)
        return blocks;

    auto tracks = te::getAudioTracks (*edit);

    // One block per piece (a chopped song's pieces that still touch are one clip), numbered in time order.
    for (auto& piece : mixtopia::chops::listPieces (*edit))
    {
        auto* clip = piece.clip;
        Block b;
        b.id = clip->itemID;
        b.number = piece.label;
        b.lane = tracks.indexOf (dynamic_cast<te::AudioTrack*> (clip->getTrack()));
        b.start = piece.time.getStart();
        b.end = piece.time.getEnd();
        b.fullName = clip->getName();
        b.shortName = mixtopia::shortTitle (clip->getName());
        b.colour = mixtopia::clipcolours::get (*clip);

        if (auto loop = mixtopia::loops::getLoop (*clip))
        {
            b.loopFrom = mixtopia::tempo::sourceToEditSeconds (*clip, loop->end);
            b.loopTo = mixtopia::tempo::sourceToEditSeconds (*clip, loop->end + loop->added());
            b.repeats = loop->repeats;
        }

        blocks.push_back (b);
    }

    return blocks;
}

juce::Rectangle<float> MixOverview::getRowsArea() const
{
    return getLocalBounds().toFloat().reduced (10.0f, 0.0f).withTrimmedTop (30.0f).withTrimmedBottom (22.0f);
}

double MixOverview::getTotalSeconds() const
{
    // At least 10 minutes, so a short mix doesn't fill the strip with a few huge blocks.
    return juce::jmax (600.0, edit != nullptr ? edit->getLength().inSeconds() * 1.02 : 0.0);
}

float MixOverview::timeToX (double seconds) const
{
    auto area = getRowsArea();
    return area.getX() + (float) (seconds / getTotalSeconds()) * area.getWidth();
}

double MixOverview::xToTime (float x) const
{
    auto area = getRowsArea();
    return juce::jlimit (0.0, getTotalSeconds(), (double) ((x - area.getX()) / juce::jmax (1.0f, area.getWidth())) * getTotalSeconds());
}

juce::Rectangle<float> MixOverview::getVisibleBox() const
{
    if (timeline == nullptr)
        return {};

    auto range = timeline->getVisibleTimeRange();
    auto area = getRowsArea();
    auto x0 = timeToX (range.getStart()), x1 = juce::jmax (x0 + 3.0f, timeToX (range.getEnd()));
    return { x0, area.getY() - 3.0f, x1 - x0, area.getHeight() + 6.0f };
}

juce::Rectangle<float> MixOverview::blockBounds (const Block& b, int numLanes) const
{
    auto area = getRowsArea();
    auto rowHeight = area.getHeight() / (float) juce::jmax (1, numLanes);
    auto x0 = timeToX (b.start), x1 = juce::jmax (x0 + 2.0f, timeToX (b.end));
    return { x0, area.getY() + rowHeight * (float) b.lane + 2.0f, x1 - x0, rowHeight - 4.0f };
}

void MixOverview::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    theme::drawLabel (g, "MIX OVERVIEW", { 12, 6, 200, 16 });

    if (edit == nullptr || timeline == nullptr)
        return;

    auto area = getRowsArea();
    auto numLanes = te::getAudioTracks (*edit).size();
    auto rowHeight = area.getHeight() / (float) juce::jmax (1, numLanes);

    for (int lane = 0; lane < numLanes; ++lane)
    {
        g.setColour (theme::inset);
        g.fillRoundedRectangle (area.getX(), area.getY() + rowHeight * (float) lane + 1.0f, area.getWidth(), rowHeight - 2.0f, 3.0f);
    }

    // Time scale: a label every 1, 2, 5, 10 ... minutes, at least ~60 px apart.
    auto total = getTotalSeconds();
    double step = 60.0;

    for (auto minutes : { 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0 })
    {
        step = minutes * 60.0;

        if (area.getWidth() * step / total >= 60.0)
            break;
    }

    g.setFont (theme::font (theme::textTiny));

    for (double t = 0.0; t <= total; t += step)
    {
        auto x = timeToX (t);
        g.setColour (theme::text3);
        g.drawVerticalLine ((int) x, area.getBottom(), area.getBottom() + 4.0f);
        g.drawText (juce::String ((int) (t / 60.0)) + ":00", (int) x + 3, (int) area.getBottom() + 4, 40, 14, juce::Justification::left);
    }

    // The songs.
    for (auto& b : getBlocks())
    {
        if (b.lane < 0)
            continue;

        auto r = blockBounds (b, numLanes);
        g.setColour (theme::tint (b.colour, 0.3f));
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (b.colour);
        g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);
        // A loop's repeats: hatched, a teal line under them.
        auto loopArea = r.withLeft (juce::jmax (r.getX(), timeToX (b.loopFrom))).withRight (juce::jmin (r.getRight(), timeToX (b.loopTo)));

        if (b.repeats > 0)
        {
            juce::Graphics::ScopedSaveState save (g);
            g.reduceClipRegion (loopArea.toNearestInt());
            g.setColour (theme::accent.withAlpha (0.25f));

            for (auto x = loopArea.getX() - r.getHeight(); x < loopArea.getRight(); x += 6.0f)
                g.drawLine (x, r.getBottom(), x + r.getHeight(), r.getY(), 1.5f);

            g.setColour (theme::accent);
            g.fillRect (loopArea.withTop (loopArea.getBottom() - 2.5f));
        }

        g.setFont (theme::font (theme::textSmall));

        // A short block (e.g. a piece of a chopped song) keeps at least its number. The name sits on the block's
        // own colour, so the hatching never runs through it.
        auto textArea = r.reduced (r.getWidth() > 30.0f ? 7.0f : 3.0f, 0.0f);
        auto text = b.number + "  " + b.shortName;
        auto font = g.getCurrentFont();

        if (juce::GlyphArrangement::getStringWidth (font, text) > textArea.getWidth())
            text = b.number;

        auto nameWidth = juce::jmin (textArea.getWidth(), juce::GlyphArrangement::getStringWidth (font, text));
        auto nameBox = juce::Rectangle<float> (textArea.getX() - 3.0f, r.getCentreY() - 8.0f, nameWidth + 6.0f, 16.0f).getIntersection (r.reduced (1.0f));

        if (b.repeats > 0 && nameBox.intersects (loopArea))
        {
            g.setColour (theme::tint (b.colour, 0.3f));
            g.fillRoundedRectangle (nameBox, 2.0f);
        }

        g.setColour (theme::text1);
        g.drawText (text, textArea.toNearestInt(), juce::Justification::centredLeft, true);

        // "×3" in the hatching: at its right end, under the name when the block is tall enough, else beside it
        // (left out when it would cover the name).
        if (b.repeats > 0)
        {
            g.setFont (theme::font (theme::textSmall, true));
            auto times = juce::String (juce::CharPointer_UTF8 ("\xc3\x97")) + juce::String (b.repeats);
            auto w = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), times) + 6.0f;
            auto timesBox = r.getHeight() >= 40.0f ? juce::Rectangle<float> (loopArea.getRight() - w - 3.0f, loopArea.getBottom() - 19.0f, w, 15.0f)
                                                   : juce::Rectangle<float> (loopArea.getRight() - w - 3.0f, r.getCentreY() - 7.5f, w, 15.0f);

            if (timesBox.getX() >= loopArea.getX() && ! timesBox.intersects (nameBox))
            {
                g.setColour (theme::tint (b.colour, 0.3f).darker (0.4f));
                g.fillRoundedRectangle (timesBox, 2.0f);
                g.setColour (theme::accent);
                g.drawText (times, timesBox, juce::Justification::centred, false);
            }
        }

        if (std::find (highlighted.begin(), highlighted.end(), b.id) != highlighted.end())
        {
            g.setColour (highlightColour);
            g.drawRoundedRectangle (r.expanded (3.0f), 3.0f, 3.0f);
            g.setFont (theme::font (theme::textTiny, true));
            // Above the block, or beside it when it's in the top row (under the heading).
            if (r.getY() - 15.0f >= 20.0f)
                g.drawText ("first copy", (int) r.getX(), (int) r.getY() - 15, 80, 12, juce::Justification::left);
            else
                g.drawText ("first copy", (int) r.getRight() + 6, (int) r.getCentreY() - 6, 80, 12, juce::Justification::left);
        }
    }

    // The part shown in the lanes (user's choice, "range bar": a faint tint with thin lines along its
    // top and bottom - lavender until M8, now the teal accent), and the playhead.
    auto box = getVisibleBox();
    g.setColour (rangeColour.withAlpha (0.08f));
    g.fillRect (box);
    g.setColour (rangeColour);
    g.fillRect (box.withHeight (1.5f));
    g.fillRect (box.withTop (box.getBottom() - 1.5f));
    g.setColour (theme::text1);

    auto playX = timeToX (edit->getTransport().getPosition().inSeconds());
    g.fillRect (playX - 1.0f, area.getY() - 4.0f, 2.0f, area.getHeight() + 8.0f);
}

void MixOverview::mouseDown (const juce::MouseEvent& e)
{
    if (timeline == nullptr || edit == nullptr)
        return;

    // Inside the marked part: drag to scroll. Elsewhere: jump there (the lanes follow if it's off
    // screen) and keep hold of the view, so the same drag scrolls on (user: needed a second click).
    auto t = xToTime (e.position.x);

    if (! getVisibleBox().contains (e.position))
    {
        edit->getTransport().setPosition (te::TimePosition::fromSeconds (t));

        if (auto range = timeline->getVisibleTimeRange(); ! range.contains (t))
            timeline->scrollTo (juce::jmax (0.0, t - range.getLength() * 0.25));
    }

    draggingBox = true;
    dragOffsetSeconds = t - timeline->getVisibleTimeRange().getStart();
}

void MixOverview::mouseDrag (const juce::MouseEvent& e)
{
    if (draggingBox && timeline != nullptr)
        timeline->scrollTo (juce::jmax (0.0, xToTime (e.position.x) - dragOffsetSeconds));
}

void MixOverview::mouseMove (const juce::MouseEvent& e)
{
    setMouseCursor (getVisibleBox().contains (e.position) ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::PointingHandCursor);
}

juce::String MixOverview::getTooltip()
{
    if (edit == nullptr)
        return {};

    auto mouse = getMouseXYRelative().toFloat();
    auto numLanes = te::getAudioTracks (*edit).size();

    for (auto& b : getBlocks())
        if (b.lane >= 0 && blockBounds (b, numLanes).contains (mouse))
            return b.number + ". " + b.fullName;

    return {};
}
