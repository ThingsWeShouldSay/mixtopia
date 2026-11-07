#pragma once

#include <JuceHeader.h>
#include "SongAnalysis.h"

namespace te = tracktion;

// The song library (M7): one for all projects, in ~/.config/Mixtopia/library.xml. Songs are added
// by hand (files, folders, or by dropping them on the timeline) - never by scanning. Each new song
// is decoded and analysed on a background thread (BPM, beat 1, key, loudness; the same caches the
// timeline uses, so placing it is instant) and its title/artist read from its tags.
//
// Corrections made on the timeline (song BPM, beat 1, key) are remembered here and used the next
// time the song is placed; "Use the detected ..." forgets them. Collections (e.g. one per DJ set)
// hold songs in an order the user arranges. A song is identified by its file's full path.
//
// All calls on the message thread; listeners hear about every change (ChangeBroadcaster).
namespace mixtopia::library
{
    struct Song
    {
        juce::String path;
        juce::String title, artist;
        double lengthSeconds = 0.0;
        double bpm = 0.0, downbeat = 0.0;     // 0 BPM: none found (yet)
        juce::String key;                     // Camelot
        double loudness = -std::numeric_limits<double>::infinity();
        bool gridByHand = false, keyByHand = false;
        juce::int64 addedMs = 0;

        // Not saved:
        bool analysing = false, failed = false, missing = false;

        juce::File getFile() const     { return juce::File (path); }
    };

    // Songs dragged out of the library panel (onto a lane, or within a collection) carry this.
    inline juce::var dragDescription (const juce::StringArray& paths)    { return "mixtopia-library\n" + paths.joinIntoString ("\n"); }

    inline juce::StringArray pathsFromDragDescription (const juce::var& description)
    {
        auto text = description.toString();

        if (! text.startsWith ("mixtopia-library\n"))
            return {};

        auto paths = juce::StringArray::fromLines (text.fromFirstOccurrenceOf ("\n", false, false));
        paths.removeEmptyStrings();
        return paths;
    }

    struct Collection
    {
        juce::String name;
        juce::StringArray paths;   // in set order
    };

    class Library : public juce::ChangeBroadcaster,
                    private juce::Timer
    {
    public:
        // `storage`: the library file (default: in Mixtopia's settings folder).
        explicit Library (te::Engine&, juce::File storage = {});
        ~Library() override;

        static juce::File getDefaultFile();
        static bool isSongFile (const juce::File&);

        const std::vector<Song>& getSongs() const    { return songs; }
        const Song* find (const juce::String& path) const;

        // Adds songs (folders: every song in them, subfolders too). Songs already in the library
        // are skipped. Returns how many were new.
        int add (const juce::Array<juce::File>&);
        static juce::StringArray songFilesIn (const juce::Array<juce::File>&);   // folders (and subfolders) expanded
        void remove (const juce::StringArray& paths);
        void analyseAgain (const juce::StringArray& paths);   // (failed songs are also retried at every start)

        // Collections. Names are unique; false if a name is taken or unknown.
        const std::vector<Collection>& getCollections() const     { return collections; }
        const Collection* findCollection (const juce::String& name) const;
        bool createCollection (const juce::String& name);
        bool renameCollection (const juce::String& oldName, const juce::String& newName);
        void deleteCollection (const juce::String& name);
        void addToCollection (const juce::String& name, const juce::StringArray& paths);       // at the end; no duplicates
        void removeFromCollection (const juce::String& name, const juce::StringArray& paths);
        void moveInCollection (const juce::String& name, const juce::StringArray& paths, int insertIndex);  // keeps their order

        // Corrections from the timeline.
        void rememberGrid (const juce::String& path, double bpm, double downbeat);
        void rememberKey (const juce::String& path, const juce::String& camelot);
        void forgetGrid (const juce::String& path);    // back to the detected BPM and beat 1
        void forgetKey (const juce::String& path);

        // A song's analysis with the remembered corrections applied (for placing it).
        void applyCorrections (const juce::String& path, analysis::SongInfo&) const;

        // Missing files: re-checks every song; points one at its new place (and finds the other
        // missing songs in that folder). Returns how many were found.
        void refreshMissing();
        int relink (const juce::String& oldPath, const juce::File& newFile);

        // Writes now (normally a second after the last change).
        void saveNow();

        // Called (message thread) when a save didn't complete - the old file is kept (CLEANUP-AUDIT C11).
        std::function<void (const juce::String&)> onSaveFailed;
        bool lastSaveFailed = false;   // (tried again when the library closes)

        // Blocks until the background analysis is idle (tests).
        bool waitUntilAnalysed (int timeoutMs);

    private:
        class Worker;

        void load();
        void queueAnalysis (const juce::String& path);
        void changed();
        void timerCallback() override     { stopTimer(); saveNow(); }
        Song* findSong (const juce::String& path);
        Collection* findCollectionMutable (const juce::String& name);

        te::Engine& engine;
        juce::File storage;
        std::vector<Song> songs;
        std::unordered_map<juce::String, size_t> indexByPath;   // songs[i].path -> i
        void reindex();
        std::vector<Collection> collections;
        std::unique_ptr<Worker> worker;
        std::shared_ptr<bool> alive = std::make_shared<bool> (true);
    };
}
