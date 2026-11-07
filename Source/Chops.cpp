#include "Chops.h"
#include "ClipTempo.h"
#include "ClipEnvelopes.h"
#include "EditHelpers.h"
#include "Loops.h"

namespace mixtopia::chops
{
    namespace
    {
        // Where the clip's file starts in the mix: an edit beat (tempo-matched) or edit seconds.
        struct Placement
        {
            bool matched = false;
            double contentStart = 0.0;
        };

        Placement placementOf (te::AudioClipBase& clip)
        {
            if (tempo::isTempoMatched (clip))
                return { true, tempo::getContentStartBeat (clip) };

            return { false, tempo::getContentStart (clip).inSeconds() };
        }

        void placeAt (te::AudioClipBase& clip, Placement p, Piece range)
        {
            if (p.matched)
            {
                tempo::place (clip, p.contentStart, { range.start, range.end });
                return;
            }

            clip.setPosition ({ { te::TimePosition::fromSeconds (p.contentStart + range.start), te::TimePosition::fromSeconds (p.contentStart + range.end) },
                                te::TimeDuration::fromSeconds (range.start) });
        }

        void setFlag (te::AudioClipBase& clip, const juce::Identifier& property, bool on)
        {
            auto* um = &clip.edit.getUndoManager();

            if (on)
                clip.state.setProperty (property, true, um);
            else if (clip.state.hasProperty (property))
                clip.state.removeProperty (property, um);
        }

        te::AudioClipBase* copyClip (te::AudioClipBase& clip, te::AudioTrack& track)
        {
            auto* copy = te::insertClipCopy (track, te::ClipCopy::fromClip (clip).withNewItemID (clip.edit));
            return dynamic_cast<te::AudioClipBase*> (copy);
        }

        std::vector<double> cutsWithin (const std::vector<double>& cuts, Piece range)
        {
            std::vector<double> inside;

            for (auto t : cuts)
                if (t > range.start && t < range.end)
                    inside.push_back (t);

            return inside;
        }

        // The clip's state without what changes when a piece is separated or joined (place, ids,
        // cuts, fades) - two clips alike here sound alike.
        juce::ValueTree comparable (const juce::ValueTree& state)
        {
            auto copy = state.createCopy();

            for (auto id : { te::IDs::id, te::IDs::start, te::IDs::length, te::IDs::offset, fadeInProperty, fadeOutProperty, leadInProperty })
                copy.removeProperty (id, nullptr);

            copy.removeChild (copy.getChildWithName (cutsType), nullptr);

            std::function<void (juce::ValueTree)> dropIds = [&] (juce::ValueTree v)
            {
                for (auto child : v)
                {
                    child.removeProperty (te::IDs::id, nullptr);
                    dropIds (child);
                }
            };

            dropIds (copy);
            return copy;
        }

        // The next clip on the lane that starts where this one ends and plays on from where it stops
        // in the same song file at the same tempo.
        te::AudioClipBase* followingOn (te::AudioClipBase& clip)
        {
            auto* track = dynamic_cast<te::ClipTrack*> (clip.getTrack());

            if (track == nullptr)
                return nullptr;

            auto end = clip.getPosition().getEnd().inSeconds();
            auto here = placementOf (clip);

            for (auto* c : track->getClips())
            {
                auto* other = dynamic_cast<te::AudioClipBase*> (c);

                if (other == nullptr || other == &clip || std::abs (visibleStart (*other) - end) > 1.0e-6)
                    continue;

                auto there = placementOf (*other);

                if (other->state[te::IDs::source] == clip.state[te::IDs::source]
                     && here.matched == there.matched
                     && tempo::getClipBpm (*other) == tempo::getClipBpm (clip)
                     && tempo::getDownbeat (*other) == tempo::getDownbeat (clip)
                     && std::abs (here.contentStart - there.contentStart) < 1.0e-6)
                    return other;
            }

            return nullptr;
        }

        // Joins `next` (which follows on) onto `clip`: one clip playing both, a cut where they met. The
        // one playing at the playhead stays (its reader and stretcher carry on - a clip starting afresh
        // mid-song crackled, 2026-10-28); the other goes.
        Merge merge (te::AudioClipBase& clip, te::AudioClipBase& next)
        {
            auto first = tempo::getSourceRange (clip);
            auto second = tempo::getSourceRange (next);
            auto firstPieces = (int) getPieces (clip).size();
            auto cuts = getCuts (clip);
            cuts.push_back (first.end);

            for (auto t : getCuts (next))
                cuts.push_back (t);

            auto fadeIn = hasFadeIn (clip), leadIn = hasLeadIn (clip), fadeOut = hasFadeOut (next);
            auto p = placementOf (clip);
            auto keepNext = next.getPosition().time.contains (clip.edit.getTransport().getPosition());
            auto& keep = keepNext ? next : clip;
            auto& gone = keepNext ? clip : next;
            Merge m { gone.itemID, keep.itemID, keepNext ? 0 : firstPieces, keepNext ? firstPieces : 0 };

            gone.removeFromParent();
            placeAt (keep, p, { first.start, second.end });
            setFlag (keep, fadeInProperty, fadeIn);
            setFlag (keep, leadInProperty, leadIn);
            setFlag (keep, fadeOutProperty, fadeOut);
            setCuts (keep, cuts, &keep.edit.getUndoManager());
            return m;
        }

        juce::String letters (int index)
        {
            juce::String s;

            for (++index; index > 0; index = (index - 1) / 26)
                s = juce::String::charToString ((juce::juce_wchar) ('a' + (index - 1) % 26)) + s;

            return s;
        }
    }

    std::vector<double> getCuts (const te::AudioClipBase& clip)
    {
        std::vector<double> cuts;
        auto list = clip.state.getChildWithName (cutsType);

        if (! list.isValid())
            return cuts;

        auto range = tempo::getSourceRange (const_cast<te::AudioClipBase&> (clip));

        for (auto c : list)
            if (double t = c.getProperty (timeProperty); t > range.start + 1.0e-6 && t < range.end - 1.0e-6)
                cuts.push_back (t);

        std::sort (cuts.begin(), cuts.end());
        return cuts;
    }

    void setCuts (te::AudioClipBase& clip, std::vector<double> cuts, juce::UndoManager* um)
    {
        auto range = tempo::getSourceRange (clip);
        std::sort (cuts.begin(), cuts.end());
        cuts.erase (std::unique (cuts.begin(), cuts.end()), cuts.end());
        cuts = cutsWithin (cuts, { range.start + 1.0e-6, range.end - 1.0e-6 });

        auto old = clip.state.getChildWithName (cutsType);

        if (old.isValid())
        {
            std::vector<double> stored;

            for (auto c : old)
                stored.push_back (c.getProperty (timeProperty));

            if (stored == cuts)
                return;

            clip.state.removeChild (old, um);
        }

        if (cuts.empty())
            return;

        juce::ValueTree list (cutsType);

        for (auto t : cuts)
            list.appendChild (juce::ValueTree (cutType, { { timeProperty, t } }), nullptr);

        clip.state.appendChild (list, um);
    }

    std::vector<Piece> getPieces (te::AudioClipBase& clip)
    {
        auto range = tempo::getSourceRange (clip);
        std::vector<Piece> pieces;
        auto from = range.start;

        for (auto t : getCuts (clip))
        {
            pieces.push_back ({ from, t });
            from = t;
        }

        pieces.push_back ({ from, range.end });
        return pieces;
    }

    int pieceAt (te::AudioClipBase& clip, double editSeconds)
    {
        if (! clip.getPosition().time.contains (te::TimePosition::fromSeconds (editSeconds)))
            return -1;

        auto played = tempo::editToSourceSeconds (clip, editSeconds);
        auto cuts = getCuts (clip);
        return (int) (std::upper_bound (cuts.begin(), cuts.end(), played) - cuts.begin());
    }

    juce::Range<double> getPieceEditRange (te::AudioClipBase& clip, int piece)
    {
        auto pieces = getPieces (clip);

        if (! juce::isPositiveAndBelow (piece, (int) pieces.size()))
            return { clip.getPosition().getStart().inSeconds(), clip.getPosition().getEnd().inSeconds() };

        auto start = piece == 0 ? clip.getPosition().getStart().inSeconds() : tempo::sourceToEditSeconds (clip, pieces[(size_t) piece].start);
        auto end = piece + 1 == (int) pieces.size() ? clip.getPosition().getEnd().inSeconds() : tempo::sourceToEditSeconds (clip, pieces[(size_t) piece].end);
        return { start, end };
    }

    bool isPiece (const te::AudioClipBase& clip)
    {
        return hasFadeIn (clip) || hasFadeOut (clip) || clip.state.getChildWithName (cutsType).isValid();
    }

    juce::String songKey (const te::AudioClipBase& clip)
    {
        return getSourceFile (clip).getFullPathName();
    }

    std::vector<te::AudioClipBase*> clipsOfSong (te::Edit& edit, const juce::String& key)
    {
        std::vector<te::AudioClipBase*> clips;

        for (auto* track : te::getAudioTracks (edit))
            for (auto* c : track->getClips())
                if (auto* clip = dynamic_cast<te::AudioClipBase*> (c); clip != nullptr && songKey (*clip) == key)
                    clips.push_back (clip);

        return clips;
    }

    bool cut (te::AudioClipBase& clip, double playedSeconds)
    {
        auto range = tempo::getSourceRange (clip);

        if (playedSeconds < range.start + minPieceSeconds || playedSeconds > range.end - minPieceSeconds)
            return false;

        auto cuts = getCuts (clip);

        for (auto t : cuts)
            if (std::abs (t - playedSeconds) < minPieceSeconds)
                return false;

        cuts.push_back (playedSeconds);
        setCuts (clip, cuts, &clip.edit.getUndoManager());
        return true;
    }

    te::AudioClipBase* separate (te::AudioClipBase& clip, int index)
    {
        auto pieces = getPieces (clip);
        auto* track = dynamic_cast<te::AudioTrack*> (clip.getTrack());

        if (pieces.size() <= 1 || track == nullptr || ! juce::isPositiveAndBelow (index, (int) pieces.size()))
            return &clip;

        auto* um = &clip.edit.getUndoManager();
        auto p = placementOf (clip);
        auto cuts = getCuts (clip);
        auto fadeIn = hasFadeIn (clip), fadeOut = hasFadeOut (clip), leadIn = hasLeadIn (clip);
        auto piece = pieces[(size_t) index];
        auto hasBefore = index > 0, hasAfter = index + 1 < (int) pieces.size();

        // The parts: before the piece, the piece, after it (each with its edit range).
        struct Part { Piece range; std::vector<double> cuts; bool fadeIn, fadeOut, leadIn; juce::Range<double> time; };
        std::vector<Part> parts;
        auto editAt = [&] (int i) { return getPieceEditRange (clip, i); };

        if (hasBefore)
        {
            Piece before { pieces.front().start, piece.start };
            parts.push_back ({ before, cutsWithin (cuts, before), fadeIn, true, leadIn, { editAt (0).getStart(), editAt (index - 1).getEnd() } });
        }

        auto pieceAtIndex = parts.size();
        parts.push_back ({ piece, {}, hasBefore || fadeIn, hasAfter || fadeOut, ! hasBefore && leadIn, editAt (index) });

        if (hasAfter)
        {
            Piece after { piece.end, pieces.back().end };
            parts.push_back ({ after, cutsWithin (cuts, after), true, fadeOut, false,
                               { editAt (index + 1).getStart(), editAt ((int) pieces.size() - 1).getEnd() } });
        }

        // The part playing at the playhead keeps this clip, so its reader and stretcher carry on (a
        // part on a new clip starts them afresh mid-song: a crackle while playing, 2026-10-28); the
        // others become copies. Not playing there: the first part keeps it.
        auto now = clip.edit.getTransport().getPosition().inSeconds();
        size_t keeper = 0;

        for (size_t i = 0; i < parts.size(); ++i)
            if (parts[i].time.contains (now))
                keeper = i;

        std::vector<te::AudioClipBase*> clips (parts.size(), nullptr);

        for (size_t i = 0; i < parts.size(); ++i)
            clips[i] = i == keeper ? &clip : copyClip (clip, *track);   // (copies of it as it was)

        for (size_t i = 0; i < parts.size(); ++i)
        {
            if (clips[i] == nullptr)
                continue;

            placeAt (*clips[i], p, parts[i].range);
            setCuts (*clips[i], parts[i].cuts, um);
            setFlag (*clips[i], fadeInProperty, parts[i].fadeIn);
            setFlag (*clips[i], fadeOutProperty, parts[i].fadeOut);
            setFlag (*clips[i], leadInProperty, parts[i].leadIn);
        }

        return clips[pieceAtIndex];
    }

    te::AudioClipBase* copyPiece (te::AudioClipBase& clip, int index, te::AudioTrack& track, double beatsLater)
    {
        auto pieces = getPieces (clip);

        if (! juce::isPositiveAndBelow (index, (int) pieces.size()))
            return nullptr;

        auto piece = pieces[(size_t) index];
        auto p = placementOf (clip);
        auto fadeIn = index > 0 || hasFadeIn (clip);
        auto fadeOut = index + 1 < (int) pieces.size() || hasFadeOut (clip);
        auto* copy = copyClip (clip, track);

        if (copy == nullptr)
            return nullptr;

        if (p.matched)
        {
            p.contentStart += beatsLater;
        }
        else
        {
            auto& edit = clip.edit;
            auto start = getPieceEditRange (clip, index).getStart();
            auto moved = tempo::toTime (edit, tempo::toBeats (edit, te::TimePosition::fromSeconds (start)) + beatsLater).inSeconds();
            p.contentStart += moved - start;
        }

        if (index == 0 && hasLeadIn (clip))   // (a copy starts where its piece is seen to start)
            piece.start = tempo::editToSourceSeconds (clip, visibleStart (clip));

        placeAt (*copy, p, piece);
        setCuts (*copy, {}, &clip.edit.getUndoManager());
        setFlag (*copy, leadInProperty, false);
        setFlag (*copy, fadeInProperty, fadeIn);
        setFlag (*copy, fadeOutProperty, fadeOut);
        return copy;
    }

    te::AudioClipBase* duplicate (te::AudioClipBase& clip, int index, juce::String& status)
    {
        auto& edit = clip.edit;
        auto range = getPieceEditRange (clip, index);
        auto beats = tempo::toBeats (edit, te::TimePosition::fromSeconds (range.getEnd()))
                       - tempo::toBeats (edit, te::TimePosition::fromSeconds (range.getStart()));
        auto from = range.getEnd();
        auto to = tempo::toTime (edit, tempo::toBeats (edit, te::TimePosition::fromSeconds (from)) + beats).inSeconds();

        auto isFree = [&] (te::AudioTrack& track)
        {
            for (auto* c : track.getClips())
            {
                auto t = c->getPosition().time;

                if (t.getStart().inSeconds() < to - 1.0e-6 && t.getEnd().inSeconds() > from + fadeSeconds + 1.0e-6)
                    return false;
            }

            return true;
        };

        auto tracks = te::getAudioTracks (edit);
        auto own = tracks.indexOf (dynamic_cast<te::AudioTrack*> (clip.getTrack()));
        te::AudioTrack* target = nullptr;

        // Its own lane, then the lanes below it, then those above.
        std::vector<int> order { own };

        for (int i = own + 1; i < tracks.size(); ++i)
            order.push_back (i);

        for (int i = 0; i < own; ++i)
            order.push_back (i);

        for (auto i : order)
            if (auto* t = tracks[i]; t != nullptr && isFree (*t))
            {
                target = t;
                break;
            }

        if (target == nullptr)
        {
            edit.ensureNumberOfAudioTracks (tracks.size() + 1);
            target = te::getAudioTracks (edit).getLast();
        }

        auto lane = te::getAudioTracks (edit).indexOf (target);

        if (lane != own)
            status = "No room after it on lane " + juce::String (own + 1) + ": the copy went on lane " + juce::String (lane + 1)
                       + (lane >= tracks.size() ? " (a new lane)." : ".");

        return copyPiece (clip, index, *target, beats);
    }

    std::vector<Merge> joinTouching (te::Edit& edit)
    {
        std::vector<Merge> merges;

        for (auto* track : te::getAudioTracks (edit))
        {
            for (bool again = true; again;)
            {
                again = false;

                for (auto* c : track->getClips())
                {
                    auto* clip = dynamic_cast<te::AudioClipBase*> (c);

                    if (clip == nullptr || ! isPiece (*clip))
                        continue;

                    auto* next = followingOn (*clip);

                    if (next == nullptr || ! comparable (clip->state).isEquivalentTo (comparable (next->state)))
                        continue;

                    merges.push_back (merge (*clip, *next));
                    again = true;
                    break;
                }
            }
        }

        return merges;
    }

    te::AudioClipBase* nextJoinable (te::AudioClipBase& clip)
    {
        auto* next = followingOn (clip);

        if (next == nullptr || loops::getLoop (clip) != loops::getLoop (*next))
            return nullptr;

        return next;
    }

    Merge joinWithNext (te::AudioClipBase& clip)
    {
        auto* next = nextJoinable (clip);

        if (next == nullptr)
            return {};

        // Each part keeps its own lines: the first clip's up to the join, the next one's after it (a
        // 2 ms step where they differ, so the change can't click).
        auto join = tempo::getSourceRange (clip).end;
        constexpr double step = 0.002;

        for (int p = 0; p < envelopes::numParams; ++p)
        {
            auto param = (envelopes::Param) p;
            auto a = envelopes::getPoints (clip.state, param), b = envelopes::getPoints (next->state, param);

            if (a.size() == b.size() && std::equal (a.begin(), a.end(), b.begin(), [] (auto& x, auto& y) { return x.sourceTime == y.sourceTime && x.db == y.db; }))
                continue;

            std::vector<envelopes::Point> joined;

            for (auto& pt : a)
                if (pt.sourceTime < join)
                    joined.push_back (pt);

            joined.push_back ({ join, envelopes::evaluate (a, join) });
            joined.push_back ({ join + step, envelopes::evaluate (b, join + step) });

            for (auto& pt : b)
                if (pt.sourceTime > join + step)
                    joined.push_back (pt);

            envelopes::setPoints (clip.state, param, joined, &clip.edit.getUndoManager());
            envelopes::setPoints (next->state, param, joined, &clip.edit.getUndoManager());
        }

        return merge (clip, *next);
    }

    double visibleStart (const te::AudioClipBase& clip)
    {
        return clip.getPosition().getStart().inSeconds() + (hasLeadIn (clip) ? fadeSeconds : 0.0);
    }

    void updateLeadIns (te::Edit& edit)
    {
        for (auto* track : te::getAudioTracks (edit))
            for (auto* c : track->getClips())
            {
                auto* clip = dynamic_cast<te::AudioClipBase*> (c);

                if (clip == nullptr)
                    continue;

                // Starts exactly where another clip ends - on its lane, or on another (a piece moved to
                // another lane at the same time: the song carries on there) - a crossfade. (On one lane,
                // pieces that follow on have joined; across lanes the crossfade gives back the song.)
                auto seen = visibleStart (*clip);
                auto butts = false;

                if (hasFadeIn (*clip))
                    for (auto* anyTrack : te::getAudioTracks (edit))
                        for (auto* other : anyTrack->getClips())
                            if (other != clip && std::abs (other->getPosition().getEnd().inSeconds() - seen) < 1.0e-6)
                                butts = true;

                auto leadStart = tempo::editToSourceSeconds (*clip, seen - fadeSeconds);
                auto wanted = butts && leadStart >= 0.0;

                if (wanted == hasLeadIn (*clip))
                    continue;

                auto range = tempo::getSourceRange (*clip);
                range.start = wanted ? leadStart : tempo::editToSourceSeconds (*clip, seen);
                placeAt (*clip, placementOf (*clip), { range.start, range.end });
                setFlag (*clip, leadInProperty, wanted);
            }
    }

    std::vector<Merge> tidy (te::Edit& edit)
    {
        auto merges = joinTouching (edit);
        updateLeadIns (edit);
        return merges;
    }

    std::vector<Labelled> listPieces (te::Edit& edit)
    {
        std::vector<Labelled> all;
        std::vector<std::pair<Labelled, double>> withSource;   // (piece, its start in the song)

        for (auto* track : te::getAudioTracks (edit))
            for (auto* c : track->getClips())
                if (auto* clip = dynamic_cast<te::AudioClipBase*> (c))
                {
                    auto pieces = getPieces (*clip);

                    for (int i = 0; i < (int) pieces.size(); ++i)
                    {
                        Labelled l;
                        l.clip = clip;
                        l.piece = i;
                        l.time = getPieceEditRange (*clip, i);
                        withSource.push_back ({ l, loops::playedToSong (loops::getLoop (*clip), pieces[(size_t) i].start) });
                    }
                }

        std::stable_sort (withSource.begin(), withSource.end(), [] (auto& a, auto& b) { return a.first.time.getStart() < b.first.time.getStart(); });

        // A chopped song's pieces form one family (one number); every other clip is a song of its own.
        auto familyOf = [] (const te::AudioClipBase& clip)
        {
            return isPiece (clip) ? "song:" + songKey (clip) : "clip:" + clip.itemID.toString();
        };

        std::map<juce::String, int> numbers;
        std::map<juce::String, std::vector<double>> starts;   // a chopped song's distinct piece starts (ms)

        for (auto& [l, songStart] : withSource)
        {
            auto family = familyOf (*l.clip);

            if (numbers.find (family) == numbers.end())
                numbers[family] = (int) numbers.size() + 1;

            if (isPiece (*l.clip))
                starts[family].push_back (std::round (songStart * 1000.0));
        }

        for (auto& [family, list] : starts)
        {
            std::sort (list.begin(), list.end());
            list.erase (std::unique (list.begin(), list.end()), list.end());
        }

        std::map<juce::String, int> copiesSoFar;

        for (auto& [l, songStart] : withSource)
        {
            auto family = familyOf (*l.clip);
            l.songNumber = numbers[family];
            l.label = juce::String (l.songNumber);

            if (isPiece (*l.clip))
            {
                auto& list = starts[family];
                auto letter = letters ((int) (std::lower_bound (list.begin(), list.end(), std::round (songStart * 1000.0)) - list.begin()));
                auto count = ++copiesSoFar[family + letter];
                l.label += letter + (count > 1 ? juce::String (juce::CharPointer_UTF8 ("\xc2\xb7")) + juce::String (count) : juce::String());
            }

            all.push_back (l);
        }

        return all;
    }

    juce::String labelFor (const std::vector<Labelled>& list, const te::AudioClipBase& clip, int piece)
    {
        for (auto& l : list)
            if (l.clip == &clip && l.piece == piece)
                return l.label;

        return {};
    }
}
