#include "TempoStrip.h"
#include "Theme.h"
#include "DialogFocus.h"

namespace tempo = mixtopia::tempo;
namespace theme = mixtopia::theme;

namespace
{
    const juce::Colour stripColour (theme::panel), lineColour (0xffb9a8ff), steepColour (0xffff9f43);
    constexpr int dotRadius = 4, dotGrabRadius = 7;

    double parseBpm (const juce::String& text)
    {
        auto bpm = text.trim().getDoubleValue();
        return bpm >= tempo::minBpm && bpm <= tempo::maxBpm ? bpm : 0.0;
    }

    int barOf (double beat)     { return (int) std::round (beat / tempo::beatsPerBar) + 1; }
}

TempoStrip::TempoStrip (te::Edit& e, std::function<double (double)> toX, std::function<double (double)> toTime, int header)
    : edit (e), timeToX (std::move (toX)), xToTime (std::move (toTime)), headerWidth (header)
{
}

double TempoStrip::beatToX (double beat) const
{
    return timeToX (tempo::toTime (edit, beat).inSeconds());
}

double TempoStrip::xToBeat (double x) const
{
    return tempo::toBeats (edit, te::TimePosition::fromSeconds (juce::jmax (0.0, xToTime (x))));
}

std::pair<double, double> TempoStrip::getBpmRange (const tempo::TempoPlan& plan) const
{
    auto low = plan.projectBpm, high = plan.projectBpm;

    for (auto& p : plan.points)
    {
        low = juce::jmin (low, p.bpm);
        high = juce::jmax (high, p.bpm);
    }

    // At least 8 BPM of room, centred, so a single tempo sits in the middle.
    auto middle = (low + high) / 2.0, half = juce::jmax (4.0, (high - low) / 2.0 + 1.5);
    return { middle - half, middle + half };
}

float TempoStrip::bpmToY (double bpm, double low, double high) const
{
    auto top = 6.0f, bottom = (float) getHeight() - 6.0f;
    return bottom - (float) ((bpm - low) / (high - low)) * (bottom - top);
}

std::optional<size_t> TempoStrip::pointAt (juce::Point<int> p) const
{
    auto plan = tempo::getTempoPlan (edit);
    auto [low, high] = getBpmRange (plan);
    std::optional<size_t> best;
    auto bestDistance = (float) dotGrabRadius;

    for (size_t i = 0; i < plan.points.size(); ++i)
    {
        juce::Point<float> dot ((float) beatToX (plan.points[i].beat), bpmToY (plan.points[i].bpm, low, high));

        if (auto d = dot.getDistanceFrom (p.toFloat()); d <= bestDistance)
        {
            bestDistance = d;
            best = i;
        }
    }

    return best;
}

bool TempoStrip::isSteep (const tempo::TempoPlan& plan, size_t i) const
{
    return std::abs (tempo::bpmPerBarInto (plan, i)) > tempo::steepBpmPerBar;
}

juce::String TempoStrip::describePoint (const tempo::TempoPlan& plan, size_t i) const
{
    auto& p = plan.points[i];

    if (i == 0)
        return "First point, bar " + juce::String (barOf (p.beat)) + ": " + tempo::formatBpm (p.bpm) + " BPM";

    auto& before = plan.points[i - 1];
    auto bars = (int) std::round ((p.beat - before.beat) / tempo::beatsPerBar);
    auto range = "Bar " + juce::String (barOf (before.beat)) + " to bar " + juce::String (barOf (p.beat)) + ": ";

    if (before.bpm == p.bpm)
        return range + "steady " + tempo::formatBpm (p.bpm) + " BPM";

    auto rate = tempo::bpmPerBarInto (plan, i);
    return range + tempo::formatBpm (before.bpm) + " to " + tempo::formatBpm (p.bpm) + " BPM over " + juce::String (bars)
             + (bars == 1 ? " bar" : " bars") + " (" + (rate >= 0.0 ? "+" : "") + juce::String (rate, 2) + " BPM per bar)";
}

void TempoStrip::paint (juce::Graphics& g)
{
    g.fillAll (stripColour);
    g.setColour (theme::line);
    g.fillRect (0, getHeight() - 1, getWidth(), 1);
    g.fillRect (headerWidth - 1, 0, 1, getHeight());

    theme::drawLabel (g, "TEMPO", { 12, 0, headerWidth - 12, getHeight() });

    auto plan = tempo::getTempoPlan (edit);
    auto [low, high] = getBpmRange (plan);

    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (headerWidth, 0, getWidth() - headerWidth, getHeight());

    // The line, sampled every 2 pixels; steep ramps in orange.
    juce::Path line;

    for (int x = headerWidth; x <= getWidth() + 2; x += 2)
    {
        auto bpm = edit.tempoSequence.getBpmAt (te::TimePosition::fromSeconds (juce::jmax (0.0, xToTime (x))));
        auto y = bpmToY (bpm, low, high);

        if (x == headerWidth)   // (Path::isEmpty() stays true until the first segment)
            line.startNewSubPath ((float) x, y);
        else
            line.lineTo ((float) x, y);
    }

    g.setColour (lineColour);
    g.strokePath (line, juce::PathStrokeType (1.6f));

    for (size_t i = 1; i < plan.points.size(); ++i)
        if (isSteep (plan, i))
        {
            g.setColour (steepColour);
            g.drawLine ((float) beatToX (plan.points[i - 1].beat), bpmToY (plan.points[i - 1].bpm, low, high),
                        (float) beatToX (plan.points[i].beat), bpmToY (plan.points[i].bpm, low, high), 2.0f);
        }

    // BPM labels: the project tempo at the start, and each point's tempo by its dot, kept off the line (mockup 27):
    // right of the dot, on the side the line leaves from (under a ramp that rises, over one that falls); when that
    // side has no room (the strip's top or bottom), left of the dot on the side away from the line coming in.
    g.setFont (theme::font (theme::textSmall));
    auto label = [&] (const juce::String& text, double dotX, double bpm, double previousBpm, double nextBpm)
    {
        auto y = bpmToY (bpm, low, high);
        auto aboveY = (int) y - 17, belowY = (int) y + 3;
        auto aboveFits = aboveY >= 0, belowFits = belowY + 15 <= getHeight();
        auto width = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), text);
        auto x = (int) dotX + 7, top = aboveFits ? aboveY : belowY;

        auto fits = [&] (double other, int& result)   // the side away from a line going to `other`
        {
            if (other > bpm)  { result = belowY; return belowFits; }
            if (other < bpm)  { result = aboveY; return aboveFits; }
            result = aboveFits ? aboveY : belowY;
            return aboveFits || belowFits;
        };

        if (! fits (nextBpm, top) && fits (previousBpm, top))
            x = (int) dotX - 7 - width;

        g.setColour (theme::text2);
        g.drawText (text, x, juce::jlimit (0, getHeight() - 15, top), width + 2, 15, juce::Justification::left);
    };

    if (plan.points.empty() || beatToX (plan.points.front().beat) > headerWidth + 40)
        label (tempo::formatBpm (plan.projectBpm), headerWidth - 3, plan.projectBpm, plan.projectBpm, plan.projectBpm);

    // (A point that keeps the tempo of the one before it - the start of a ramp - needs no label.)
    for (size_t i = 0; i < plan.points.size(); ++i)
        if (i == 0 || plan.points[i].bpm != plan.points[i - 1].bpm)
            label (tempo::formatBpm (plan.points[i].bpm), beatToX (plan.points[i].beat), plan.points[i].bpm,
                   i > 0 ? plan.points[i - 1].bpm : plan.points[i].bpm,
                   i + 1 < plan.points.size() ? plan.points[i + 1].bpm : plan.points[i].bpm);

    // The dots; the first is a ring (its tempo also sets everything before it).
    for (size_t i = 0; i < plan.points.size(); ++i)
    {
        auto x = (float) beatToX (plan.points[i].beat);
        auto y = bpmToY (plan.points[i].bpm, low, high);
        auto highlighted = hoverPoint == i || dragPoint == i;
        auto radius = highlighted ? dotRadius + 1.5f : (float) dotRadius;
        g.setColour (highlighted ? juce::Colours::white : lineColour);

        if (i == 0)
        {
            g.setColour (stripColour);
            g.fillEllipse (x - radius - 1.0f, y - radius - 1.0f, (radius + 1.0f) * 2.0f, (radius + 1.0f) * 2.0f);
            g.setColour (highlighted ? juce::Colours::white : lineColour);
            g.drawEllipse (x - radius, y - radius, radius * 2.0f, radius * 2.0f, 2.0f);
        }
        else
        {
            g.fillEllipse (x - radius, y - radius, radius * 2.0f, radius * 2.0f);
        }
    }

    // The point under the mouse or being dragged, described.
    if (auto i = dragPoint ? dragPoint : hoverPoint; i && *i < plan.points.size())
    {
        auto text = describePoint (plan, *i);
        auto font = theme::font (theme::textNormal);
        auto width = juce::GlyphArrangement::getStringWidthInt (font, text) + 20;
        auto x = (int) beatToX (plan.points[*i].beat) + 12;
        juce::Rectangle<int> box (x, (getHeight() - 24) / 2, width, 24);

        if (box.getRight() > getWidth())
            box.setX (x - 24 - width);

        g.setColour (theme::hover);
        g.fillRoundedRectangle (box.toFloat(), theme::controlRadius);
        g.setColour (theme::line.brighter (0.2f));
        g.drawRoundedRectangle (box.toFloat().reduced (0.5f), theme::controlRadius, 1.0f);
        g.setFont (font);
        g.setColour (isSteep (plan, *i) ? steepColour : theme::text1);
        g.drawText (text, box.reduced (10, 0), juce::Justification::centredLeft);
    }
}

void TempoStrip::mouseMove (const juce::MouseEvent& e)
{
    auto i = pointAt (e.getPosition());

    if (i != hoverPoint)
    {
        hoverPoint = i;
        setMouseCursor (i ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void TempoStrip::mouseExit (const juce::MouseEvent&)
{
    if (hoverPoint)
    {
        hoverPoint.reset();
        repaint();
    }
}

void TempoStrip::mouseDown (const juce::MouseEvent& e)
{
    if (e.x < headerWidth)
        return;

    if (e.mods.isPopupMenu())
    {
        showMenu (e.getPosition());
        return;
    }

    dragPoint = pointAt (e.getPosition());

    if (dragPoint)
    {
        dragStartPlan = tempo::getTempoPlan (edit);
        edit.getUndoManager().beginNewTransaction ("Move tempo point");
    }
}

void TempoStrip::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragPoint)
        return;

    auto plan = dragStartPlan;
    auto i = *dragPoint;
    auto bar = (double) tempo::beatsPerBar;
    auto earliest = i > 0 ? plan.points[i - 1].beat + bar : bar;
    auto latest = i + 1 < plan.points.size() ? plan.points[i + 1].beat - bar : 1.0e9;
    auto beat = juce::jlimit (earliest, latest, std::round (xToBeat (e.x) / bar) * bar);

    if (beat != tempo::getTempoPlan (edit).points[i].beat)
    {
        plan.points[i].beat = beat;
        tempo::setTempoPlan (edit, plan);
        status (describePoint (plan, i));
    }

    repaint();
}

void TempoStrip::mouseUp (const juce::MouseEvent&)
{
    dragPoint.reset();
    repaint();
}

void TempoStrip::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (auto i = pointAt (e.getPosition()))
        editPoint (*i);
}

namespace
{
    // One dialog for every tempo change: the BPM. 0 if cancelled or out of range.
    double askForBpm (juce::Component* parent, const juce::String& title, const juce::String& note, double current,
                      const std::function<void (const juce::String&)>& status)
    {
        juce::AlertWindow window (title, note, juce::MessageBoxIconType::NoIcon, parent);
        window.addTextEditor ("bpm", tempo::formatBpm (current), "Tempo (BPM):");
        window.addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        window.addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        mixtopia::FocusWhenShown::start (*window.getTextEditor ("bpm"));

        if (window.runModalLoop() != 1)
            return 0.0;

        auto bpm = parseBpm (window.getTextEditorContents ("bpm"));

        if (bpm == 0.0)
            status ("Tempo must be between " + juce::String ((int) tempo::minBpm) + " and " + juce::String ((int) tempo::maxBpm) + " BPM.");

        return bpm;
    }
}

void TempoStrip::changeTempoAt (double editSeconds)
{
    auto plan = tempo::getTempoPlan (edit);
    auto beat = std::round (tempo::toBeats (edit, te::TimePosition::fromSeconds (editSeconds)) / tempo::beatsPerBar) * tempo::beatsPerBar;
    auto statusFn = [this] (const juce::String& t) { status (t); };

    // Bar 1: the project tempo (with points placed, that's also the first point's).
    if (beat < tempo::beatsPerBar)
    {
        auto note = plan.points.empty() ? juce::String ("The tempo of the whole mix.")
                                        : "The tempo before the first point, which is the first point's tempo too (bar "
                                            + juce::String (barOf (plan.points.front().beat)) + ").";

        if (auto bpm = askForBpm (this, "Change tempo at bar 1", note, plan.projectBpm, statusFn); bpm > 0.0 && bpm != plan.projectBpm)
        {
            edit.getUndoManager().beginNewTransaction ("Change tempo");

            if (plan.points.empty())
                tempo::setProjectBpm (edit, bpm);
            else
            {
                plan.points.front().bpm = bpm;
                tempo::setTempoPlan (edit, plan);
            }
        }

        return;
    }

    for (size_t i = 0; i < plan.points.size(); ++i)
        if (plan.points[i].beat == beat)
        {
            editPoint (i);
            return;
        }

    auto isFirst = plan.points.empty() || beat < plan.points.front().beat;
    auto current = edit.tempoSequence.getBpmAt (tempo::toTime (edit, beat));
    juce::String note;

    if (isFirst)
        note = "This is the first point: its tempo is also the tempo before it, from bar 1 (now " + tempo::formatBpm (plan.projectBpm)
                 + " BPM). Keep it to start a ramp here; a ramp forms when you place the next point.";
    else
    {
        size_t before = 0;

        for (size_t i = 0; i < plan.points.size(); ++i)
            if (plan.points[i].beat < beat)
                before = i;

        note = "A ramp forms from the point before it (bar " + juce::String (barOf (plan.points[before].beat)) + ", "
                 + tempo::formatBpm (plan.points[before].bpm) + " BPM) to this one.";
    }

    if (auto bpm = askForBpm (this, "Change tempo at bar " + juce::String (barOf (beat)), note, current, statusFn); bpm > 0.0)
    {
        edit.getUndoManager().beginNewTransaction ("Change tempo");
        plan = tempo::withTempoAt (plan, beat, bpm);
        tempo::setTempoPlan (edit, plan);

        for (size_t i = 0; i < plan.points.size(); ++i)
            if (plan.points[i].beat == beat)
                status (describePoint (plan, i));
    }
}

void TempoStrip::editPoint (size_t i)
{
    auto plan = tempo::getTempoPlan (edit);

    if (i >= plan.points.size())
        return;

    auto note = i == 0 ? juce::String ("The first point: its tempo is also the tempo before it, from bar 1.")
                       : "A ramp forms from the point before it (bar " + juce::String (barOf (plan.points[i - 1].beat)) + ", "
                           + tempo::formatBpm (plan.points[i - 1].bpm) + " BPM) to this one.";

    if (auto bpm = askForBpm (this, "Change tempo at bar " + juce::String (barOf (plan.points[i].beat)), note, plan.points[i].bpm,
                              [this] (const juce::String& t) { status (t); });
        bpm > 0.0 && bpm != plan.points[i].bpm)
    {
        plan.points[i].bpm = bpm;
        edit.getUndoManager().beginNewTransaction ("Change tempo");
        tempo::setTempoPlan (edit, plan);
        status (describePoint (tempo::getTempoPlan (edit), i));
    }
}

void TempoStrip::deletePoint (size_t i)
{
    auto plan = tempo::getTempoPlan (edit);

    if (i >= plan.points.size())
        return;

    plan.points.erase (plan.points.begin() + (long) i);
    edit.getUndoManager().beginNewTransaction ("Delete tempo point");
    tempo::setTempoPlan (edit, plan);
    plan = tempo::getTempoPlan (edit);

    if (i == 0 && ! plan.points.empty())
        status ("Deleted the first point. The next one is first now: the tempo before bar " + juce::String (barOf (plan.points.front().beat))
                  + " is " + tempo::formatBpm (plan.projectBpm) + " BPM (Undo to bring it back).");
    else
        status ("Deleted the tempo point.");
}

void TempoStrip::showMenu (juce::Point<int> p)
{
    auto point = pointAt (p);
    auto seconds = xToTime (p.x);

    enum { changeTempo = 1, deleteIt };
    juce::PopupMenu menu;
    menu.addItem (changeTempo, point ? "Change tempo..." : "Change tempo here...");

    if (point)
        menu.addItem (deleteIt, "Delete this point");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea ({ localPointToGlobal (p), localPointToGlobal (p) }),
                        [safeThis = juce::Component::SafePointer<TempoStrip> (this), point, seconds] (int result)
    {
        // Dialogs run after the menu has closed.
        juce::MessageManager::callAsync ([safeThis, result, point, seconds]
        {
            if (safeThis == nullptr)
                return;

            if (result == changeTempo)
                point ? safeThis->editPoint (*point) : safeThis->changeTempoAt (seconds);
            else if (result == deleteIt && point)
                safeThis->deletePoint (*point);
        });
    });
}
