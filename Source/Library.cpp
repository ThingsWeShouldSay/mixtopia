#include "Library.h"
#include "SongTags.h"
#include "DecodedAudioCache.h"
#include "Settings.h"

namespace mixtopia::library
{
    namespace
    {
        void removePaths (juce::StringArray& from, const juce::StringArray& paths)
        {
            for (int i = from.size(); --i >= 0;)
                if (paths.contains (from[i]))
                    from.remove (i);
        }
    }

    //==============================================================================
    // Decodes, analyses and reads the tags of queued songs, one at a time, on its own thread; each
    // result goes back to the message thread.
    class Library::Worker : public juce::Thread
    {
    public:
        struct Result
        {
            juce::String path;
            tags::Tags tags;
            double lengthSeconds = 0.0;
            std::optional<analysis::SongInfo> info;
            bool missing = false;
        };

        Worker (te::Engine& e, std::function<void (Result)> deliver)
            : Thread ("Library analysis"), engine (e), onResult (std::move (deliver))
        {
            startThread (juce::Thread::Priority::low);
        }

        ~Worker() override
        {
            signalThreadShouldExit();
            wake.signal();
            stopThread (10000);
        }

        void queue (const juce::String& path)
        {
            {
                const std::scoped_lock lock (mutex);
                todo.push_back (path);
            }

            wake.signal();
        }

        bool isIdle()
        {
            const std::scoped_lock lock (mutex);
            return todo.empty() && ! busy;
        }

        void run() override
        {
            while (! threadShouldExit())
            {
                juce::String path;

                {
                    const std::scoped_lock lock (mutex);

                    if (! todo.empty())
                    {
                        path = todo.front();
                        todo.pop_front();
                        busy = true;
                    }
                }

                if (path.isEmpty())
                {
                    wake.wait (500);
                    continue;
                }

                onResult (process (path));

                const std::scoped_lock lock (mutex);
                busy = false;
            }
        }

    private:
        Result process (const juce::String& path)
        {
            Result result;
            result.path = path;
            juce::File file (path);

            if (! file.existsAsFile())
            {
                result.missing = true;
                return result;
            }

            auto stop = [this] { return threadShouldExit(); };
            result.tags = tags::read (file);
            // Read straight from the file (decoded in memory only): songs get a decoded copy on disk
            // only when they go onto the timeline (user, 2026-10-23: keep the OS drive small).
            auto readable = decodedCache::getReadableFile (file);

            if (std::unique_ptr<juce::AudioFormatReader> reader { engine.getAudioFileFormatManager().readFormatManager.createReaderFor (readable) })
                result.lengthSeconds = (double) reader->lengthInSamples / reader->sampleRate;

            if (threadShouldExit())
                return result;

            result.info = analysis::analyse (engine, readable, stop);
            return result;
        }

        te::Engine& engine;
        std::function<void (Result)> onResult;
        std::mutex mutex;
        std::deque<juce::String> todo;
        bool busy = false;
        juce::WaitableEvent wake;
    };

    //==============================================================================
    Library::Library (te::Engine& e, juce::File file)
        : engine (e), storage (file == juce::File() ? getDefaultFile() : file)
    {
        worker = std::make_unique<Worker> (engine, [weak = std::weak_ptr<bool> (alive), this] (Worker::Result result)
        {
            juce::MessageManager::callAsync ([weak, this, result = std::move (result)]
            {
                if (weak.expired())
                    return;

                auto* song = findSong (result.path);

                if (song == nullptr)
                    return;

                song->analysing = false;
                song->missing = result.missing;

                if (! result.missing)
                {
                    song->title = result.tags.title;
                    song->artist = result.tags.artist;
                    song->lengthSeconds = result.lengthSeconds;
                    song->failed = ! result.info.has_value();

                    if (result.info)
                    {
                        if (! song->gridByHand)
                        {
                            song->bpm = result.info->grid.valid ? result.info->grid.bpm : 0.0;
                            song->downbeat = result.info->grid.valid ? result.info->grid.downbeat : 0.0;
                        }

                        if (! song->keyByHand)
                            song->key = result.info->key;

                        song->loudness = result.info->loudness;
                    }
                }

                changed();
            });
        });

        load();
    }

    Library::~Library()
    {
        worker = nullptr;
        alive = nullptr;

        if (isTimerRunning() || lastSaveFailed)   // (a save that failed is tried once more: audit review of C11)
            saveNow();
    }

    juce::File Library::getDefaultFile()
    {
        return settings::getFile().getFile().getSiblingFile ("library.xml");
    }

    bool Library::isSongFile (const juce::File& f)
    {
        return f.hasFileExtension ("mp3;flac;wav");
    }

    const Song* Library::find (const juce::String& path) const
    {
        auto it = indexByPath.find (path);
        return it != indexByPath.end() ? &songs[it->second] : nullptr;
    }

    void Library::reindex()
    {
        indexByPath.clear();

        for (size_t i = 0; i < songs.size(); ++i)
            indexByPath[songs[i].path] = i;
    }

    Song* Library::findSong (const juce::String& path)
    {
        return const_cast<Song*> (find (path));
    }

    const Collection* Library::findCollection (const juce::String& name) const
    {
        for (auto& c : collections)
            if (c.name == name)
                return &c;

        return nullptr;
    }

    Collection* Library::findCollectionMutable (const juce::String& name)
    {
        return const_cast<Collection*> (findCollection (name));
    }

    void Library::changed()
    {
        sendChangeMessage();
        startTimer (1000);
    }

    void Library::analyseAgain (const juce::StringArray& paths)
    {
        for (auto& path : paths)
            if (auto* song = findSong (path); song != nullptr && ! song->missing)
            {
                song->failed = false;
                queueAnalysis (path);
            }

        changed();
    }

    void Library::queueAnalysis (const juce::String& path)
    {
        if (auto* song = findSong (path))
            song->analysing = true;

        worker->queue (path);
    }

    bool Library::waitUntilAnalysed (int timeoutMs)
    {
        auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

        while (juce::Time::getMillisecondCounter() < end)
        {
            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);

            if (worker->isIdle() && std::none_of (songs.begin(), songs.end(), [] (auto& s) { return s.analysing; }))
                return true;
        }

        return false;
    }

    //==============================================================================
    juce::StringArray Library::songFilesIn (const juce::Array<juce::File>& items)
    {
        juce::StringArray files;

        for (auto& item : items)
        {
            if (item.isDirectory())
            {
                auto inside = item.findChildFiles (juce::File::findFiles, true);
                inside.sort();

                for (auto& f : inside)
                    if (isSongFile (f))
                        files.add (f.getFullPathName());
            }
            else if (isSongFile (item))
            {
                files.add (item.getFullPathName());
            }
        }

        return files;
    }

    int Library::add (const juce::Array<juce::File>& items)
    {
        int added = 0;

        for (auto& path : songFilesIn (items))
        {
            juce::File f (path);

            if (find (f.getFullPathName()) != nullptr)
                continue;

            Song song;
            song.path = f.getFullPathName();
            auto fromName = tags::fromFileName (f.getFileNameWithoutExtension());
            song.title = fromName.title;
            song.artist = fromName.artist;
            song.addedMs = juce::Time::currentTimeMillis();
            indexByPath[song.path] = songs.size();
            songs.push_back (song);
            queueAnalysis (song.path);
            ++added;
        }

        if (added > 0)
            changed();

        return added;
    }

    void Library::remove (const juce::StringArray& paths)
    {
        songs.erase (std::remove_if (songs.begin(), songs.end(), [&] (auto& s) { return paths.contains (s.path); }), songs.end());
        reindex();

        for (auto& c : collections)
            removePaths (c.paths, paths);

        changed();
    }

    //==============================================================================
    bool Library::createCollection (const juce::String& name)
    {
        auto trimmed = name.trim();

        if (trimmed.isEmpty() || findCollection (trimmed) != nullptr)
            return false;

        collections.push_back ({ trimmed, {} });
        changed();
        return true;
    }

    bool Library::renameCollection (const juce::String& oldName, const juce::String& newName)
    {
        auto trimmed = newName.trim();
        auto* c = findCollectionMutable (oldName);

        if (c == nullptr || trimmed.isEmpty() || (trimmed != oldName && findCollection (trimmed) != nullptr))
            return false;

        c->name = trimmed;
        changed();
        return true;
    }

    void Library::deleteCollection (const juce::String& name)
    {
        collections.erase (std::remove_if (collections.begin(), collections.end(), [&] (auto& c) { return c.name == name; }), collections.end());
        changed();
    }

    void Library::addToCollection (const juce::String& name, const juce::StringArray& paths)
    {
        if (auto* c = findCollectionMutable (name))
        {
            for (auto& p : paths)
                if (find (p) != nullptr)
                    c->paths.addIfNotAlreadyThere (p);

            changed();
        }
    }

    void Library::removeFromCollection (const juce::String& name, const juce::StringArray& paths)
    {
        if (auto* c = findCollectionMutable (name))
        {
            removePaths (c->paths, paths);
            changed();
        }
    }

    void Library::moveInCollection (const juce::String& name, const juce::StringArray& paths, int insertIndex)
    {
        auto* c = findCollectionMutable (name);

        if (c == nullptr)
            return;

        juce::StringArray moving, rest;
        int before = 0;

        for (int i = 0; i < c->paths.size(); ++i)
        {
            if (paths.contains (c->paths[i]))
            {
                moving.add (c->paths[i]);
                before += i < insertIndex ? 1 : 0;
            }
            else
            {
                rest.add (c->paths[i]);
            }
        }

        auto at = juce::jlimit (0, rest.size(), insertIndex - before);

        for (int i = 0; i < moving.size(); ++i)
            rest.insert (at + i, moving[i]);

        c->paths = rest;
        changed();
    }

    //==============================================================================
    void Library::rememberGrid (const juce::String& path, double bpm, double downbeat)
    {
        if (auto* s = findSong (path))
        {
            s->bpm = bpm;
            s->downbeat = downbeat;
            s->gridByHand = true;
            changed();
        }
    }

    void Library::rememberKey (const juce::String& path, const juce::String& camelot)
    {
        if (auto* s = findSong (path))
        {
            s->key = camelot;
            s->keyByHand = true;
            changed();
        }
    }

    void Library::forgetGrid (const juce::String& path)
    {
        if (auto* s = findSong (path); s != nullptr && s->gridByHand)
        {
            s->gridByHand = false;
            queueAnalysis (path);   // (cached: back to the detected values at once)
            changed();
        }
    }

    void Library::forgetKey (const juce::String& path)
    {
        if (auto* s = findSong (path); s != nullptr && s->keyByHand)
        {
            s->keyByHand = false;
            queueAnalysis (path);
            changed();
        }
    }

    void Library::applyCorrections (const juce::String& path, analysis::SongInfo& info) const
    {
        if (auto* s = find (path))
        {
            if (s->gridByHand)
            {
                info.grid.valid = s->bpm > 0.0;
                info.grid.bpm = s->bpm;
                info.grid.downbeat = s->downbeat;
            }

            if (s->keyByHand)
                info.key = s->key;
        }
    }

    //==============================================================================
    void Library::refreshMissing()
    {
        auto any = false;

        for (auto& s : songs)
        {
            auto missing = ! s.getFile().existsAsFile();
            any = any || missing != s.missing;
            s.missing = missing;
        }

        if (any)
            sendChangeMessage();
    }

    int Library::relink (const juce::String& oldPath, const juce::File& newFile)
    {
        auto relinkOne = [this] (Song& s, const juce::File& to)
        {
            auto newPath = to.getFullPathName();

            for (auto& c : collections)
                for (auto& p : c.paths)
                    if (p == s.path)
                        p = newPath;

            s.path = newPath;
            s.missing = false;
            reindex();
            queueAnalysis (newPath);
        };

        auto* song = findSong (oldPath);

        if (song == nullptr || ! newFile.existsAsFile() || find (newFile.getFullPathName()) != nullptr)
            return 0;

        relinkOne (*song, newFile);
        int found = 1;

        // Other missing songs that were in the same folder may have moved with it.
        for (auto& s : songs)
            if (s.missing)
                if (auto candidate = newFile.getParentDirectory().getChildFile (s.getFile().getFileName());
                    candidate.existsAsFile() && find (candidate.getFullPathName()) == nullptr)
                {
                    relinkOne (s, candidate);
                    ++found;
                }

        changed();
        return found;
    }

    //==============================================================================
    void Library::load()
    {
        // A file that can't be read is kept aside (with a message), never replaced by an empty library (CLEANUP-AUDIT C4).
        if (settings::setAsideIfDamaged (storage, "MIXTOPIA_LIBRARY"))
            return;

        auto xml = juce::parseXML (storage);

        if (xml == nullptr || ! xml->hasTagName ("MIXTOPIA_LIBRARY"))
            return;

        for (auto* e : xml->getChildWithTagNameIterator ("SONG"))
        {
            Song s;
            s.path = e->getStringAttribute ("path");
            s.title = e->getStringAttribute ("title");
            s.artist = e->getStringAttribute ("artist");
            s.lengthSeconds = e->getDoubleAttribute ("length");
            s.bpm = e->getDoubleAttribute ("bpm");
            s.downbeat = e->getDoubleAttribute ("downbeat");
            s.key = e->getStringAttribute ("key");
            s.loudness = e->hasAttribute ("loudness") ? e->getDoubleAttribute ("loudness") : -std::numeric_limits<double>::infinity();
            s.gridByHand = e->getBoolAttribute ("gridByHand");
            s.keyByHand = e->getBoolAttribute ("keyByHand");
            s.failed = e->getBoolAttribute ("failed");
            s.addedMs = e->getStringAttribute ("added").getLargeIntValue();

            if (s.path.isNotEmpty() && find (s.path) == nullptr)
            {
                indexByPath[s.path] = songs.size();
                songs.push_back (s);
            }
        }

        for (auto* e : xml->getChildWithTagNameIterator ("COLLECTION"))
        {
            Collection c;
            c.name = e->getStringAttribute ("name");

            for (auto* item : e->getChildWithTagNameIterator ("ITEM"))
                c.paths.add (item->getStringAttribute ("path"));

            if (c.name.isNotEmpty() && findCollection (c.name) == nullptr)
                collections.push_back (c);
        }

        refreshMissing();

        // Songs whose analysis didn't finish (or failed) last time get another try.
        for (auto& s : songs)
            if (! s.missing && (s.failed || s.lengthSeconds <= 0.0))
            {
                s.failed = false;
                queueAnalysis (s.path);
            }
    }

    void Library::saveNow()
    {
        stopTimer();
        juce::XmlElement xml ("MIXTOPIA_LIBRARY");
        xml.setAttribute ("version", 1);

        for (auto& s : songs)
        {
            auto* e = xml.createNewChildElement ("SONG");
            e->setAttribute ("path", s.path);
            e->setAttribute ("title", s.title);
            e->setAttribute ("artist", s.artist);
            e->setAttribute ("length", s.lengthSeconds);
            e->setAttribute ("bpm", s.bpm);
            e->setAttribute ("downbeat", s.downbeat);
            e->setAttribute ("key", s.key);

            if (std::isfinite (s.loudness))
                e->setAttribute ("loudness", s.loudness);

            if (s.gridByHand)  e->setAttribute ("gridByHand", true);
            if (s.keyByHand)   e->setAttribute ("keyByHand", true);
            if (s.failed)      e->setAttribute ("failed", true);

            e->setAttribute ("added", juce::String (s.addedMs));
        }

        for (auto& c : collections)
        {
            auto* e = xml.createNewChildElement ("COLLECTION");
            e->setAttribute ("name", c.name);

            for (auto& p : c.paths)
                e->createNewChildElement ("ITEM")->setAttribute ("path", p);
        }

        storage.getParentDirectory().createDirectory();

        lastSaveFailed = ! settings::replaceFileContents (storage, xml.toString());

        if (lastSaveFailed && onSaveFailed)
            onSaveFailed ("Couldn't save the song library (" + storage.getFullPathName() + ") - is the disk full? It will be tried "
                          "again with the next change.");
    }
}
