#pragma once

#include <JuceHeader.h>
#include "Chops.h"

namespace te = tracktion;

// Every song (clip) has its own colour, kept in the clip, so it stays the same when the song moves
// to another lane: the waveform and the song's bar in the master panel's meter use it.
//
// Songs near each other in time never share a colour (user, 2026-10-22: the per-song meter tells
// the songs playing together apart by colour). "Near" = overlapping or less than a minute apart.
// A new song gets a colour no near song uses (the least used one overall among those, then palette
// order); a song that is moved or trimmed next to one of its colour changes colour (in the same
// undo step; the status bar says so). Songs from projects saved before colours existed get the
// colour of the lane they're on when opened. The pieces of a chopped song (Chops.h) all keep the
// song's colour: the rule is only between different songs.
namespace mixtopia::clipcolours
{
    // Pieces (and copies) of the same chopped song.
    inline bool sameSong (const te::Clip& a, const te::Clip& b)
    {
        auto* x = dynamic_cast<const te::AudioClipBase*> (&a);
        auto* y = dynamic_cast<const te::AudioClipBase*> (&b);
        return x != nullptr && y != nullptr && (chops::isPiece (*x) || chops::isPiece (*y)) && chops::songKey (*x) == chops::songKey (*y);
    }

    inline const juce::Identifier property { "mixtopiaColour" };

    // M8 palette (2026-10-22): bright, none near the teal accent (the old cyan and green were), and
    // every pair far enough apart to tell in small meter labels and overview blocks: the closest
    // pairs (lime/yellow, blue/violet) are 48 apart in CIELAB (delta E 1976); the mockup's orange and
    // gold, which the user found too close, were 28.
    struct Named { juce::Colour colour; const char* name; };

    inline const std::array<Named, 6> palette {
        Named { juce::Colour (0xffff7a1f), "orange" }, Named { juce::Colour (0xff4f8dff), "blue" },
        Named { juce::Colour (0xffbf5af2), "violet" }, Named { juce::Colour (0xff9be03c), "lime" },
        Named { juce::Colour (0xffff4f9e), "pink" },   Named { juce::Colour (0xffffd60a), "yellow" }
    };

    // The colours songs were given before M8, and what each becomes.
    inline const std::array<std::pair<juce::uint32, int>, 6> oldPalette {
        std::pair<juce::uint32, int> { 0xfff59e0b, 0 }, { 0xff22d3ee, 5 }, { 0xffa78bfa, 2 },
        { 0xff4ade80, 3 }, { 0xfff472b6, 4 }, { 0xff60a5fa, 1 }
    };

    constexpr double nearbySeconds = 60.0;

    inline juce::Colour forLane (int lane)
    {
        return palette[(size_t) (std::max (0, lane) % (int) palette.size())].colour;
    }

    inline std::optional<juce::Colour> getStored (const te::Clip& clip)
    {
        if (! clip.state.hasProperty (property))
            return {};

        return juce::Colour::fromString (clip.state[property].toString());
    }

    inline juce::Colour get (const te::Clip& clip)
    {
        if (auto c = getStored (clip))
            return *c;

        auto* track = clip.getTrack();
        return forLane (track != nullptr ? te::getAudioTracks (clip.edit).indexOf (dynamic_cast<te::AudioTrack*> (track)) : 0);
    }

    inline juce::String getName (juce::Colour c)
    {
        for (auto& p : palette)
            if (p.colour == c)
                return p.name;

        return "another colour";
    }

    inline bool overlaps (const te::Clip& a, const te::Clip& b)
    {
        auto x = a.getPosition().time, y = b.getPosition().time;
        return x.getStart() < y.getEnd() && y.getStart() < x.getEnd();
    }

    inline bool isNear (const te::Clip& a, const te::Clip& b)
    {
        auto x = a.getPosition().time, y = b.getPosition().time;
        return y.getStart().inSeconds() < x.getEnd().inSeconds() + nearbySeconds
                && y.getEnd().inSeconds() > x.getStart().inSeconds() - nearbySeconds;
    }

    // The best colour for a song where it is now: one no overlapping song uses, then none nearby,
    // then the least used overall, then palette order.
    inline size_t bestFor (const te::Clip& clip)
    {
        std::array<int, palette.size()> overlapping {}, nearby {}, overall {};

        for (auto* track : te::getAudioTracks (clip.edit))
            for (auto* other : track->getClips())
                if (other != &clip && ! sameSong (clip, *other))
                    if (auto c = getStored (*other))
                        for (size_t i = 0; i < palette.size(); ++i)
                            if (palette[i].colour == *c)
                            {
                                ++overall[i];
                                nearby[i] += isNear (clip, *other) ? 1 : 0;
                                overlapping[i] += overlaps (clip, *other) ? 1 : 0;
                            }

        size_t best = 0;

        for (size_t i = 1; i < palette.size(); ++i)
            if (std::tuple (overlapping[i], nearby[i], overall[i]) < std::tuple (overlapping[best], nearby[best], overall[best]))
                best = i;

        return best;
    }

    // Picks and stores a colour for a newly added song.
    inline void assignNew (te::Clip& clip)
    {
        clip.state.setProperty (property, palette[bestFor (clip)].colour.toString(), &clip.edit.getUndoManager());
    }

    // After a song was moved or trimmed: if a song near it has its colour, it gets another one (as
    // part of the current undo step). Returns a sentence for the status bar, or nothing.
    inline juce::String keepApartFromNeighbours (te::Clip& clip)
    {
        auto colour = get (clip);
        const te::Clip* clash = nullptr;

        for (auto* track : te::getAudioTracks (clip.edit))
            for (auto* other : track->getClips())
                if (other != &clip && ! sameSong (clip, *other) && isNear (clip, *other) && get (*other) == colour
                     && (clash == nullptr || (overlaps (clip, *other) && ! overlaps (clip, *clash))))
                    clash = other;

        if (clash == nullptr)
            return {};

        auto best = palette[bestFor (clip)].colour;

        if (best == colour)
            return {};   // every colour is taken nearby: nothing better to give it

        clip.state.setProperty (property, best.toString(), &clip.edit.getUndoManager());

        // The song's other pieces go with it.
        for (auto* track : te::getAudioTracks (clip.edit))
            for (auto* other : track->getClips())
                if (other != &clip && sameSong (clip, *other))
                    other->state.setProperty (property, best.toString(), &clip.edit.getUndoManager());

        return clip.getName() + " is now " + getName (best) + ": " + clash->getName() + " next to it is "
                 + getName (colour) + " too.";
    }

    // Projects from before song colours: each song keeps the colour its lane gave it. Projects from
    // before M8: the old colours become their new counterparts.
    inline void assignMissing (te::Edit& edit)
    {
        for (auto* track : te::getAudioTracks (edit))
            for (auto* clip : track->getClips())
            {
                if (auto stored = getStored (*clip))
                {
                    for (auto& [old, index] : oldPalette)
                        if (stored->getARGB() == old)
                            clip->state.setProperty (property, palette[(size_t) index].colour.toString(), nullptr);
                }
                else
                {
                    clip->state.setProperty (property, get (*clip).toString(), nullptr);
                }
            }
    }
}
