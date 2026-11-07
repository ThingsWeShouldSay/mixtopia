#pragma once

#include <JuceHeader.h>
#include "Library.h"
#include "Theme.h"

// The song library's panel (M7, top left): a collection drop-down ("All songs" or a collection,
// plus New / Rename / Delete), a search box (title and artist), a "Not in mix" filter, "Add
// songs...", and the song list - Title, Artist, BPM, Key, Length (the song's own length; user,
// 2026-10-23) - with resizable, sortable columns (widths
// remembered) and the full text in a tooltip where it's cut off. Songs in the current mix get a
// green dot. In a collection a "#" column shows the set order; dragging rows rearranges it while
// it's sorted by "#".
//   - double-click: preview the song (onPreview)
//   - drag rows onto a lane: add them to the mix (the timeline accepts the drag)
//   - right-click: Show on timeline, Add to collection, Remove from collection, Locate..., Remove
// Files and folders dropped on the panel are added to the library.
class LibraryPanel : public juce::Component,
                     public juce::FileDragAndDropTarget,
                     public juce::TextDragAndDropTarget,
                     public juce::DragAndDropTarget,
                     private juce::TableListBoxModel,
                     private juce::ChangeListener,
                     private juce::Timer
{
public:
    explicit LibraryPanel (mixtopia::library::Library&);
    ~LibraryPanel() override;

    // The songs (full paths) in the current mix, asked for now and then.
    std::function<std::set<juce::String>()> getPathsInMix;
    std::function<void (const juce::String& path)> onPreview, onShowOnTimeline;
    std::function<void (const juce::String&)> onStatusMessage;

    void resized() override;
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;

    bool isInterestedInFileDrag (const juce::StringArray&) override     { return true; }
    void filesDropped (const juce::StringArray&, int, int) override;

    // Many file managers (Nautilus, Dolphin under Wayland) offer dropped files as text: file:// URIs.
    bool isInterestedInTextDrag (const juce::String&) override;
    void textDropped (const juce::String&, int x, int y) override;

    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragMove (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

    // Search and sorting run again (tests: timing a big library).
    void setSearchText (const juce::String& t)      { searchBox.setText (t, false); refresh(); }
    void sortBy (int column, bool forwards)         { sortOrderChanged (column, forwards); }
    void showCollection (const juce::String& name)  { shownCollection = name; refresh(); }
    void dropFiles (const juce::StringArray& files) { filesDropped (files, 0, -1); }
    juce::String getListedPath (int row) const      { return juce::isPositiveAndBelow (row, (int) rows.size()) ? rows[(size_t) row] : juce::String(); }
    static constexpr int lengthColumnId = 6;

    // A song's length as the Length column shows it: "6:42", "1:02:15" ("-" while unknown).
    static juce::String formatLength (double seconds);
    int getNumListed() const                        { return (int) rows.size(); }
    static constexpr int artistColumnId = 3;

    // One row: header row height, and each song row.
    static constexpr int controlsHeight = 44, headerHeight = 24, rowHeight = 22;
    static constexpr float textSize = 13.0f;

private:
    enum Column { orderColumn = 1, titleColumn, artistColumn, bpmColumn, keyColumn, lengthColumn };

    // TableListBoxModel
    int getNumRows() override                       { return (int) rows.size(); }
    void paintRowBackground (juce::Graphics&, int row, int width, int height, bool selected) override;
    void paintCell (juce::Graphics&, int row, int column, int width, int height, bool selected) override;
    void cellClicked (int row, int column, const juce::MouseEvent&) override;
    void cellDoubleClicked (int row, int column, const juce::MouseEvent&) override;
    void sortOrderChanged (int column, bool forwards) override;
    juce::String getCellTooltip (int row, int column) override;
    juce::var getDragSourceDescription (const juce::SparseSet<int>& selectedRows) override;

    void changeListenerCallback (juce::ChangeBroadcaster*) override     { refresh(); }
    void timerCallback() override;

    const mixtopia::library::Song* songAt (int row) const;
    juce::String cellText (const mixtopia::library::Song&, int column) const;
    juce::StringArray selectedPaths() const;
    void refresh();
    void rebuildCollectionBox();
    void collectionChosen();
    void showSongMenu (int row);
    void askForName (const juce::String& title, const juce::String& current, std::function<void (const juce::String&)> then);
    void chooseFilesToAdd();
    void addSongs (const juce::Array<juce::File>&, int dropY);   // dropY < 0: not dropped
    void locate (const juce::String& path);
    void status (const juce::String& t)     { if (onStatusMessage) onStatusMessage (t); }
    bool isSetOrder() const;
    int insertRowAt (int y) const;

    mixtopia::library::Library& library;
    juce::String shownCollection;                   // {} = all songs
    juce::String columnsAtStart;                    // saved again only if the user changed them
    std::vector<juce::String> rows;                 // paths, as listed
    std::set<juce::String> inMix;
    int sortColumn = titleColumn;
    bool sortForwards = true;
    int dropInsertRow = -1;                         // while rearranging a collection

    juce::ComboBox collectionBox;
    juce::TextEditor searchBox;
    mixtopia::theme::IconButton notInMixButton { mixtopia::theme::Icon::filter, "Not in mix" },
                                addButton { mixtopia::theme::Icon::plus, "Add songs" };
    juce::TableListBox table { {}, this };
    std::unique_ptr<juce::FileChooser> chooser;

    enum { allSongsId = 1, newCollectionId = 2, renameCollectionId = 3, deleteCollectionId = 4, firstCollectionId = 100 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LibraryPanel)
};
