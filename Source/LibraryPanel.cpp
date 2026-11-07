#include "LibraryPanel.h"
#include "DialogFocus.h"
#include "ClipTempo.h"
#include "Settings.h"
#include "EditHelpers.h"

namespace lib = mixtopia::library;
namespace theme = mixtopia::theme;

namespace
{
    const juce::Colour panelColour (theme::panel), altRowColour (0xff1a1b21), selectedColour (theme::panel.interpolatedWith (theme::accent, 0.13f)),
                       textColour (theme::text1), dimColour (theme::text3), inMixColour (theme::accent), busyColour (theme::warn),
                       missingColour (theme::mute);
    const char* columnsSetting = "mixtopiaLibraryColumns";

    // "8A" < "8B" < "9A" < "10A".
    int keyOrder (const juce::String& camelot)
    {
        auto number = camelot.getIntValue();
        return number > 0 ? number * 2 + (camelot.endsWithIgnoreCase ("B") ? 1 : 0) : 1000;
    }
}

LibraryPanel::LibraryPanel (lib::Library& l)
    : library (l)
{
    collectionBox.onChange = [this] { collectionChosen(); };
    collectionBox.setWantsKeyboardFocus (false);

    searchBox.setTextToShowWhenEmpty ("Search title or artist", dimColour);
    searchBox.setFont (juce::FontOptions (textSize));
    searchBox.setIndents (30, 0);
    searchBox.setJustification (juce::Justification::centredLeft);
    searchBox.onTextChange = [this] { refresh(); };
    searchBox.onEscapeKey = [this] { searchBox.clear(); refresh(); };

    notInMixButton.setClickingTogglesState (true);
    notInMixButton.setTooltip ("Only show songs that aren't in the mix yet");
    notInMixButton.onClick = [this] { refresh(); };
    notInMixButton.setWantsKeyboardFocus (false);

    addButton.setTooltip ("Add song files or whole folders to the library (or drag them onto it)");
    addButton.onClick = [this] { chooseFilesToAdd(); };
    addButton.setWantsKeyboardFocus (false);

    auto& header = table.getHeader();
    auto flags = juce::TableHeaderComponent::visible | juce::TableHeaderComponent::resizable | juce::TableHeaderComponent::sortable;
    header.addColumn ("#", orderColumn, 30, 24, 60, flags);
    header.addColumn ("Title", titleColumn, 320, 80, 1000, flags);
    header.addColumn ("Artist", artistColumn, 210, 60, 800, flags);
    header.addColumn ("BPM", bpmColumn, 50, 40, 90, flags);
    header.addColumn ("Key", keyColumn, 46, 36, 90, flags);
    header.addColumn ("Length", lengthColumn, 62, 48, 100, flags);   // (layouts saved before it existed keep their widths; it joins at the end)
    header.setStretchToFitActive (false);

    if (auto saved = mixtopia::settings::getFile().getValue (columnsSetting); saved.isNotEmpty())
        header.restoreFromString (saved);

    header.setColumnVisible (orderColumn, false);
    header.setSortColumnId (titleColumn, true);
    columnsAtStart = header.toString();

    table.setHeaderHeight (headerHeight);
    table.setRowHeight (rowHeight);
    table.setMultipleSelectionEnabled (true);
    table.setColour (juce::ListBox::backgroundColourId, panelColour);
    table.setOutlineThickness (0);
    table.setWantsKeyboardFocus (true);

    for (auto* c : std::initializer_list<juce::Component*> { &collectionBox, &searchBox, &notInMixButton, &addButton, &table })
        addAndMakeVisible (c);

    library.addChangeListener (this);
    rebuildCollectionBox();
    refresh();
    startTimerHz (2);
}

LibraryPanel::~LibraryPanel()
{
    library.removeChangeListener (this);

    // Column widths (and sorting) for next time, if the user changed them.
    if (auto columns = table.getHeader().toString(); columns != columnsAtStart)
    {
        mixtopia::settings::getFile().setValue (columnsSetting, columns);
        mixtopia::settings::getFile().saveIfNeeded();
    }
}

void LibraryPanel::resized()
{
    auto r = getLocalBounds().reduced (1, 0);
    auto controls = r.removeFromTop (controlsHeight).reduced (8, 8);
    collectionBox.setBounds (controls.removeFromLeft (186));
    controls.removeFromLeft (6);
    addButton.setBounds (controls.removeFromRight (addButton.getIdealWidth (controls.getHeight())));
    controls.removeFromRight (6);
    notInMixButton.setBounds (controls.removeFromRight (notInMixButton.getIdealWidth (controls.getHeight())));
    controls.removeFromRight (6);
    searchBox.setBounds (controls);
    table.setBounds (r.withTrimmedBottom (4));
}

void LibraryPanel::paint (juce::Graphics& g)
{
    g.fillAll (panelColour);

    if (rows.empty())
    {
        g.setColour (dimColour);
        g.setFont (textSize);
        auto text = library.getSongs().empty() ? "Drag songs or folders here, or click \"Add songs...\""
                                                : "No songs match.";
        g.drawText (text, table.getBounds().withTrimmedTop (headerHeight + 8), juce::Justification::centredTop);
    }
}

void LibraryPanel::paintOverChildren (juce::Graphics& g)
{
    theme::drawIcon (g, theme::Icon::search, searchBox.getBounds().toFloat().removeFromLeft (30.0f).withSizeKeepingCentre (16.0f, 16.0f),
                     theme::text3);

    if (dropInsertRow < 0)
        return;

    // Where rearranged rows will go.
    auto y = dropInsertRow < (int) rows.size() ? table.getRowPosition (dropInsertRow, true).getY()
                                               : (rows.empty() ? headerHeight : table.getRowPosition ((int) rows.size() - 1, true).getBottom());
    g.setColour (theme::accent);
    g.fillRect (table.getX() + 2, table.getY() + y - 1, table.getWidth() - 4, 2);
}

//==============================================================================
juce::String LibraryPanel::formatLength (double seconds)
{
    if (seconds <= 0.0)
        return "-";

    auto whole = (int) std::round (seconds);
    return whole >= 3600 ? juce::String::formatted ("%d:%02d:%02d", whole / 3600, whole / 60 % 60, whole % 60)
                         : juce::String::formatted ("%d:%02d", whole / 60, whole % 60);
}

const lib::Song* LibraryPanel::songAt (int row) const
{
    return row >= 0 && row < (int) rows.size() ? library.find (rows[(size_t) row]) : nullptr;
}

juce::String LibraryPanel::cellText (const lib::Song& song, int column) const
{
    switch (column)
    {
        case orderColumn:
            if (auto* c = library.findCollection (shownCollection))
                return juce::String (c->paths.indexOf (song.path) + 1);
            return {};
        case titleColumn:   return song.title;
        case artistColumn:  return song.missing ? juce::String ("missing") : song.analysing ? juce::String ("analysing...") : song.artist;
        case bpmColumn:     return song.bpm > 0.0 ? mixtopia::tempo::formatBpm (song.bpm) : juce::String ("-");
        case keyColumn:     return song.key.isNotEmpty() ? song.key : juce::String ("-");
        case lengthColumn:  return formatLength (song.lengthSeconds);
        default:            return {};
    }
}

void LibraryPanel::paintRowBackground (juce::Graphics& g, int row, int, int, bool selected)
{
    g.fillAll (selected ? selectedColour : (row % 2 == 0 ? panelColour : altRowColour));

    if (selected)
    {
        g.setColour (theme::accent);
        g.fillRect (0, 0, 2, g.getClipBounds().getBottom());
    }
}

void LibraryPanel::paintCell (juce::Graphics& g, int row, int column, int width, int height, bool)
{
    auto* song = songAt (row);

    if (song == nullptr)
        return;

    auto x = 10;

    if (column == titleColumn)
    {
        if (inMix.count (song->path) > 0)
        {
            g.setColour (inMixColour);
            g.fillEllipse (8.0f, (float) height / 2.0f - 3.0f, 6.0f, 6.0f);
        }

        x = 20;
    }

    auto colour = song->missing ? dimColour : textColour;

    if (column == artistColumn && song->missing)
        colour = missingColour;
    else if (column == artistColumn && song->analysing)
        colour = busyColour;
    else if (column != titleColumn && ! song->missing)
        colour = theme::text2;

    g.setColour (colour);
    g.setFont (juce::FontOptions (textSize, (column == artistColumn && (song->missing || song->analysing)) ? juce::Font::bold : juce::Font::plain));
    g.drawText (cellText (*song, column), x, 0, width - x - 4, height, juce::Justification::centredLeft, true);
}

juce::String LibraryPanel::getCellTooltip (int row, int column)
{
    // The full text, when the column cuts it off.
    auto* song = songAt (row);

    if (song == nullptr || (column != titleColumn && column != artistColumn))
        return {};

    auto text = cellText (*song, column);
    auto room = table.getHeader().getColumnWidth (column) - (column == titleColumn ? 24 : 10);
    auto needed = juce::GlyphArrangement::getStringWidthInt (juce::Font (juce::FontOptions (textSize)), text);
    return needed > room ? text : juce::String();
}

void LibraryPanel::cellClicked (int row, int, const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        if (! table.isRowSelected (row))
            table.selectRow (row);

        showSongMenu (row);
    }
}

void LibraryPanel::cellDoubleClicked (int row, int, const juce::MouseEvent&)
{
    if (auto* song = songAt (row); song != nullptr && ! song->missing && onPreview)
        onPreview (song->path);
}

void LibraryPanel::sortOrderChanged (int column, bool forwards)
{
    sortColumn = column;
    sortForwards = forwards;
    refresh();
}

juce::var LibraryPanel::getDragSourceDescription (const juce::SparseSet<int>&)
{
    auto paths = selectedPaths();
    return paths.isEmpty() ? juce::var() : lib::dragDescription (paths);
}

juce::StringArray LibraryPanel::selectedPaths() const
{
    juce::StringArray paths;

    for (int i = 0; i < (int) rows.size(); ++i)
        if (table.isRowSelected (i))
            paths.add (rows[(size_t) i]);

    return paths;
}

bool LibraryPanel::isSetOrder() const
{
    return shownCollection.isNotEmpty() && sortColumn == orderColumn;
}

//==============================================================================
void LibraryPanel::refresh()
{
    auto selected = selectedPaths();

    if (getPathsInMix)
        inMix = getPathsInMix();

    // Songs to list: a collection in its set order, or the whole library.
    std::vector<juce::String> paths;

    if (auto* c = library.findCollection (shownCollection))
    {
        for (auto& p : c->paths)
            paths.push_back (p);
    }
    else
    {
        shownCollection = {};

        for (auto& s : library.getSongs())
            paths.push_back (s.path);
    }

    auto words = juce::StringArray::fromTokens (searchBox.getText().toLowerCase(), true);
    rows.clear();

    for (auto& p : paths)
    {
        auto* song = library.find (p);

        if (song == nullptr || (notInMixButton.getToggleState() && inMix.count (p) > 0))
            continue;

        auto haystack = (song->title + " " + song->artist).toLowerCase();

        if (std::all_of (words.begin(), words.end(), [&] (auto& w) { return haystack.contains (w); }))
            rows.push_back (p);
    }

    if (sortColumn != orderColumn)
    {
        auto less = [this] (const juce::String& a, const juce::String& b)
        {
            auto* x = library.find (a);
            auto* y = library.find (b);

            switch (sortColumn)
            {
                case artistColumn:  return x->artist.compareNatural (y->artist) < 0;
                case bpmColumn:     return x->bpm < y->bpm;
                case keyColumn:     return keyOrder (x->key) < keyOrder (y->key);
                case lengthColumn:  return x->lengthSeconds < y->lengthSeconds;
                default:            return x->title.compareNatural (y->title) < 0;
            }
        };

        std::stable_sort (rows.begin(), rows.end(), [&] (auto& a, auto& b) { return sortForwards ? less (a, b) : less (b, a); });
    }

    table.getHeader().setColumnVisible (orderColumn, shownCollection.isNotEmpty());
    table.updateContent();
    table.deselectAllRows();

    for (int i = 0; i < (int) rows.size(); ++i)
        if (selected.contains (rows[(size_t) i]))
            table.selectRow (i, false, false);

    rebuildCollectionBox();
    repaint();
}

void LibraryPanel::timerCallback()
{
    // The mix changes as songs are added or removed: keep the green dots (and the filter) current.
    if (! getPathsInMix)
        return;

    auto now = getPathsInMix();

    if (now != inMix)
    {
        inMix = now;

        if (notInMixButton.getToggleState())
            refresh();
        else
            table.repaint();
    }
}

//==============================================================================
void LibraryPanel::rebuildCollectionBox()
{
    collectionBox.clear (juce::dontSendNotification);
    collectionBox.addItem ("All songs (" + juce::String ((int) library.getSongs().size()) + ")", allSongsId);
    auto& collections = library.getCollections();
    int selectedId = allSongsId;

    if (! collections.empty())
        collectionBox.addSeparator();

    for (size_t i = 0; i < collections.size(); ++i)
    {
        collectionBox.addItem (collections[i].name + " (" + juce::String (collections[i].paths.size()) + ")", firstCollectionId + (int) i);

        if (collections[i].name == shownCollection)
            selectedId = firstCollectionId + (int) i;
    }

    collectionBox.addSeparator();
    collectionBox.addItem ("New collection...", newCollectionId);

    if (shownCollection.isNotEmpty())
    {
        collectionBox.addItem (juce::String (juce::CharPointer_UTF8 ("Rename \xe2\x80\x9c")) + shownCollection + juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x9d...")), renameCollectionId);
        collectionBox.addItem (juce::String (juce::CharPointer_UTF8 ("Delete \xe2\x80\x9c")) + shownCollection + juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x9d...")), deleteCollectionId);
    }

    collectionBox.setSelectedId (selectedId, juce::dontSendNotification);
}

void LibraryPanel::collectionChosen()
{
    auto id = collectionBox.getSelectedId();
    auto show = [this] (const juce::String& name)
    {
        shownCollection = name;
        sortColumn = name.isNotEmpty() ? (int) orderColumn : (int) titleColumn;
        table.getHeader().setSortColumnId (sortColumn, true);
        sortForwards = true;
        refresh();
    };

    if (id == allSongsId)
    {
        show ({});
    }
    else if (id >= firstCollectionId)
    {
        auto& collections = library.getCollections();

        if (auto i = (size_t) (id - firstCollectionId); i < collections.size())
            show (collections[i].name);
    }
    else
    {
        rebuildCollectionBox();   // (put the drop-down back to what's shown; dialogs follow)
        auto current = shownCollection;

        juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer<LibraryPanel> (this), id, current, show]
        {
            if (safeThis == nullptr)
                return;

            auto& self = *safeThis;

            if (id == newCollectionId)
            {
                self.askForName ("New collection", {}, [&self, show] (const juce::String& name)
                {
                    if (self.library.createCollection (name))
                        show (name.trim());
                    else
                        self.status ("There's already a collection called \"" + name.trim() + "\".");
                });
            }
            else if (id == renameCollectionId)
            {
                self.askForName ("Rename collection", current, [&self, current, show] (const juce::String& name)
                {
                    if (self.library.renameCollection (current, name))
                        show (name.trim());
                    else
                        self.status ("There's already a collection called \"" + name.trim() + "\".");
                });
            }
            else if (id == deleteCollectionId)
            {
                if (juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Delete collection",
                                                        "Delete the collection \"" + current + "\"? Its songs stay in the library.",
                                                        "Delete", "Cancel", &self))
                {
                    self.library.deleteCollection (current);
                    show ({});
                }
            }
        });
    }
}

void LibraryPanel::askForName (const juce::String& title, const juce::String& current, std::function<void (const juce::String&)> then)
{
    juce::AlertWindow window (title, {}, juce::MessageBoxIconType::NoIcon, this);
    window.addTextEditor ("name", current, "Name:");
    window.addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window.addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    // Type straight away: the name box gets the cursor (with any current name selected).
    mixtopia::FocusWhenShown::start (*window.getTextEditor ("name"));

    if (window.runModalLoop() == 1 && window.getTextEditorContents ("name").trim().isNotEmpty())
        then (window.getTextEditorContents ("name"));
}

//==============================================================================
void LibraryPanel::showSongMenu (int row)
{
    auto paths = selectedPaths();
    auto* song = songAt (row);

    if (song == nullptr || paths.isEmpty())
        return;

    auto path = song->path;
    auto count = paths.size();
    auto songs = count == 1 ? juce::String ("song") : juce::String (count) + " songs";
    enum { showOnTimeline = 1, removeFromCollection, locateId, removeFromLibrary, newCollectionForSongs, analyseAgainId, firstCollection = 1000 };

    juce::PopupMenu collections;
    auto& all = library.getCollections();

    for (size_t i = 0; i < all.size(); ++i)
        collections.addItem (firstCollection + (int) i, all[i].name, all[i].name != shownCollection);

    if (! all.empty())
        collections.addSeparator();

    collections.addItem (newCollectionForSongs, "New collection...");

    juce::PopupMenu menu;
    menu.addItem (showOnTimeline, "Show on timeline", inMix.count (path) > 0);
    menu.addSubMenu ("Add to collection", collections);

    if (shownCollection.isNotEmpty())
        menu.addItem (removeFromCollection, "Remove from \"" + shownCollection + "\"");

    if (song->missing)
        menu.addItem (locateId, "Locate...");

    menu.addItem (analyseAgainId, "Analyse again", ! song->missing);
    menu.addSeparator();
    menu.addItem (removeFromLibrary, "Remove " + songs + " from the library");

    menu.showMenuAsync (juce::PopupMenu::Options(), [safeThis = juce::Component::SafePointer<LibraryPanel> (this), paths, path, songs] (int result)
    {
        if (safeThis == nullptr || result == 0)
            return;

        juce::MessageManager::callAsync ([safeThis, paths, path, songs, result]
        {
            if (safeThis == nullptr)
                return;

            auto& self = *safeThis;
            auto& lib = self.library;

            if (result == showOnTimeline && self.onShowOnTimeline)
                self.onShowOnTimeline (path);
            else if (result == removeFromCollection)
                lib.removeFromCollection (self.shownCollection, paths);
            else if (result == locateId)
                self.locate (path);
            else if (result == analyseAgainId)
            {
                lib.analyseAgain (paths);
                self.status ("Analysing " + songs + " again (BPM, key, length)...");
            }
            else if (result == removeFromLibrary)
            {
                if (juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Remove from the library",
                                                        "Remove the " + songs + " from the library? The files themselves aren't touched.",
                                                        "Remove", "Cancel", &self))
                    lib.remove (paths);
            }
            else if (result == newCollectionForSongs)
            {
                self.askForName ("New collection", {}, [&self, paths] (const juce::String& name)
                {
                    if (self.library.createCollection (name))
                    {
                        self.library.addToCollection (name.trim(), paths);
                        self.status ("Added to the new collection \"" + name.trim() + "\".");
                    }
                    else
                    {
                        self.status ("There's already a collection called \"" + name.trim() + "\".");
                    }
                });
            }
            else if (result >= firstCollection && (size_t) (result - firstCollection) < lib.getCollections().size())
            {
                auto name = lib.getCollections()[(size_t) (result - firstCollection)].name;
                lib.addToCollection (name, paths);
                self.status ("Added to \"" + name + "\".");
            }
        });
    });
}

void LibraryPanel::chooseFilesToAdd()
{
    chooser = std::make_unique<juce::FileChooser> ("Add songs (or folders) to the library",
                                                   juce::File::getSpecialLocation (juce::File::userMusicDirectory), "*.mp3;*.flac;*.wav");
    auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                   | juce::FileBrowserComponent::canSelectDirectories | juce::FileBrowserComponent::canSelectMultipleItems;

    chooser->launchAsync (flags, [this] (const juce::FileChooser& fc)
    {
        if (fc.getResults().isEmpty())
            return;

        addSongs (fc.getResults(), -1);
    });
}

void LibraryPanel::addSongs (const juce::Array<juce::File>& items, int dropY)
{
    // To the library, and - while a collection is showing - to that collection too (user,
    // 2026-10-23: dropped songs went only into the library). At the end of the collection, or where
    // they were dropped when it's listed in set order. (Songs dropped on the TIMELINE never join a
    // collection: collections are made by hand - user.)
    auto paths = lib::Library::songFilesIn (items);
    auto added = library.add (items);
    juce::String collectionText;

    if (auto* collection = library.findCollection (shownCollection); collection != nullptr && ! paths.isEmpty())
    {
        juce::StringArray newOnes;

        for (auto& p : paths)
            if (! collection->paths.contains (p))
                newOnes.add (p);

        if (! newOnes.isEmpty())
        {
            library.addToCollection (shownCollection, newOnes);

            if (dropY >= 0 && isSetOrder() && searchBox.getText().isEmpty())
            {
                auto row = insertRowAt (dropY);
                library.moveInCollection (shownCollection, newOnes, juce::jmin (row, (int) rows.size()));
            }

            collectionText = " and " + juce::String (newOnes.size()) + (newOnes.size() == 1 ? " song" : " songs") + " to \"" + shownCollection + "\"";
        }
        else
        {
            collectionText = " (already in \"" + shownCollection + "\")";
        }
    }

    status ((added > 0 ? "Added " + juce::String (added) + (added == 1 ? " song" : " songs") + " to the library (finding their BPM and key...)"
                       : juce::String ("Those songs are already in the library")) + collectionText + ".");
}

void LibraryPanel::locate (const juce::String& path)
{
    juce::File missing (path);
    chooser = std::make_unique<juce::FileChooser> ("Where is \"" + missing.getFileName() + "\"?", missing.getParentDirectory(),
                                                   "*" + missing.getFileExtension());

    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, path] (const juce::FileChooser& fc)
    {
        if (fc.getResult() == juce::File())
            return;

        auto found = library.relink (path, fc.getResult());
        status (found == 0 ? juce::String ("That file is already in the library, or can't be read.")
                           : found == 1 ? juce::String ("Found it.") : "Found " + juce::String (found) + " songs (the others were in the same folder).");
    });
}

void LibraryPanel::filesDropped (const juce::StringArray& files, int, int y)
{
    juce::Array<juce::File> items;

    for (auto& f : files)
        items.add (juce::File (f));

    addSongs (items, y);
}

bool LibraryPanel::isInterestedInTextDrag (const juce::String& text)
{
    return ! mixtopia::filesFromDroppedText (text).isEmpty();
}

void LibraryPanel::textDropped (const juce::String& text, int x, int y)
{
    filesDropped (mixtopia::filesFromDroppedText (text), x, y);
}

//==============================================================================
// Rearranging a collection: rows dragged within the list while it's in set order.
bool LibraryPanel::isInterestedInDragSource (const SourceDetails& d)
{
    return isSetOrder() && d.sourceComponent != nullptr && (d.sourceComponent == &table || table.isParentOf (d.sourceComponent))
             && ! lib::pathsFromDragDescription (d.description).isEmpty();
}

int LibraryPanel::insertRowAt (int y) const
{
    auto local = y - table.getY();
    return const_cast<juce::TableListBox&> (table).getInsertionIndexForPosition (10, local);
}

void LibraryPanel::itemDragMove (const SourceDetails& d)
{
    dropInsertRow = insertRowAt (d.localPosition.y);
    repaint();
}

void LibraryPanel::itemDragExit (const SourceDetails&)
{
    dropInsertRow = -1;
    repaint();
}

void LibraryPanel::itemDropped (const SourceDetails& d)
{
    auto row = insertRowAt (d.localPosition.y);
    dropInsertRow = -1;
    repaint();

    auto* c = library.findCollection (shownCollection);

    if (c == nullptr)
        return;

    // Into the collection's own order: before the song listed at that row (or at the end).
    auto index = row < (int) rows.size() ? c->paths.indexOf (rows[(size_t) row]) : c->paths.size();
    library.moveInCollection (shownCollection, lib::pathsFromDragDescription (d.description), juce::jmax (0, index));
}
