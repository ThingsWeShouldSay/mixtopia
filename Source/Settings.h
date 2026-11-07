#pragma once

#include <JuceHeader.h>

// Mixtopia's own preferences, kept between sessions in ~/.config/Mixtopia/settings.xml (next to
// the library, library.xml).
namespace mixtopia::settings
{
    inline juce::File getFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Mixtopia");
    }

    // Until 2026-10-21 JUCE's Linux default put settings.xml (and the new library.xml) in a visible
    // ~/Mixtopia folder in the user's home. Move them next to the rest and remove that folder if
    // nothing else is in it.
    inline void moveFromOldFolder()
    {
        auto old = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Mixtopia");
        auto known = { "settings.xml", "library.xml", "library-test-session.xml" };

        if (! old.isDirectory())
            return;

        getFolder().createDirectory();

        for (auto name : known)
            if (auto from = old.getChildFile (name); from.existsAsFile())
            {
                if (auto to = getFolder().getChildFile (name); ! to.existsAsFile())
                    from.moveFileTo (to);
                else
                    from.deleteFile();   // (already moved once; the newer copy is kept)
            }

        if (old.getNumberOfChildFiles (juce::File::findFilesAndDirectories | juce::File::ignoreHiddenFiles) == 0
             && old.getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0)
            old.deleteFile();
    }

    // Files set aside because they couldn't be read (CLEANUP-AUDIT C4/C10); MainComponent tells the user once.
    inline juce::StringArray& getSetAsideFiles()
    {
        static juce::StringArray files;
        return files;
    }

    // A settings or library file that exists but doesn't read as XML with the expected top element (cut off by a crash
    // or a full disk, say) is moved aside as "<name>.damaged-<date>-<time>", so that it's never saved over and nothing
    // in it is lost; true if it was. (Until 2026-11-04 the app started empty and the next save replaced the file.)
    inline bool setAsideIfDamaged (const juce::File& file, const juce::String& topElement)
    {
        if (! file.existsAsFile())
            return false;

        if (auto xml = juce::parseXML (file); xml != nullptr && xml->hasTagName (topElement))
            return false;

        auto aside = file.getSiblingFile (file.getFileName() + ".damaged-" + juce::Time::getCurrentTime().formatted ("%Y-%m-%d-%H%M%S"))
                         .getNonexistentSibling();

        if (! file.moveFileTo (aside))
            return false;

        getSetAsideFiles().add (aside.getFullPathName());
        return true;
    }

    // Replaces a file Mixtopia keeps (a project, its last-manual-save copy, the library) SAFELY: the new contents go to a
    // temporary file next to it, which must then read back complete - same size, same bytes - before it's renamed over
    // the target. A full disk gives a short write that JUCE's file stream doesn't report, so until 2026-11-04 a cut-off
    // file replaced a good one while the save "succeeded" (CLEANUP-AUDIT C2). False if anything failed: the target is
    // left as it was.
    inline bool replaceFileContents (const juce::File& target, const juce::MemoryBlock& contents)
    {
        juce::TemporaryFile temp (target);

        {
            juce::FileOutputStream out (temp.getFile());

            if (! out.openedOk() || ! out.write (contents.getData(), contents.getSize()))
                return false;

            out.flush();

            if (out.getStatus().failed())
                return false;
        }

        juce::MemoryBlock written;
        return temp.getFile().loadFileAsData (written) && written == contents && temp.overwriteTargetFileWithTemporary();
    }

    inline bool replaceFileContents (const juce::File& target, const juce::String& text)
    {
        return replaceFileContents (target, juce::MemoryBlock (text.toRawUTF8(), text.getNumBytesAsUTF8()));
    }

    inline juce::PropertiesFile& getFile()
    {
        static juce::PropertiesFile file ([]
                                          {
                                              moveFromOldFolder();
                                              auto settingsFile = getFolder().getChildFile ("settings.xml");
                                              setAsideIfDamaged (settingsFile, "PROPERTIES");   // (JUCE's top element)
                                              return settingsFile;
                                          }(),
                                          []
                                          {
                                              juce::PropertiesFile::Options options;
                                              options.storageFormat = juce::PropertiesFile::storeAsXML;
                                              return options;
                                          }());

        return file;
    }

    // A Mixtopia started by Claude's GUI test scripts (scripts/dev/test-window.sh sets
    // MIXTOPIA_TEST_SESSION): it never asks anything (closes without saving, skips backup offers and
    // relinking), never autosaves, and leaves the user's backups alone. User, 2026-10-21: "choose
    // whatever option you need so I don't have to".
    inline bool isTestSession()
    {
        return juce::SystemStats::getEnvironmentVariable ("MIXTOPIA_TEST_SESSION", {}).isNotEmpty();
    }

    inline bool getBool (const juce::String& key, bool defaultValue)    { return getFile().getBoolValue (key, defaultValue); }

    inline void setBool (const juce::String& key, bool value)
    {
        getFile().setValue (key, value);
        getFile().saveIfNeeded();
    }
}
