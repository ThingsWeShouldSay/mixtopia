#include "PreviewStrip.h"
#include "DecodedAudioCache.h"

namespace theme = mixtopia::theme;

namespace
{
    const juce::Colour stripColour (theme::panel), shapeColour (theme::accent.withAlpha (0.6f)), shapeBackground (theme::inset);
}

PreviewStrip::PreviewStrip (te::Engine& e, mixtopia::library::Library& l)
    : engine (e), library (l), overviews (e.getAudioFileFormatManager().readFormatManager)
{
    overviews.onReady = [this] { repaint(); };
    readAhead.startThread();
    player.setSource (&transport);
    engine.getDeviceManager().deviceManager.addAudioCallback (&player);

    playButton.setTooltip ("Play / pause the preview (Esc stops it)");
    playButton.setWantsKeyboardFocus (false);
    playButton.onClick = [this]
    {
        if (isPlaying())
            transport.stop();
        else if (path.isNotEmpty())
        {
            if (transport.getCurrentPosition() >= transport.getLengthInSeconds() - 0.05)
                transport.setPosition (0.0);

            transport.start();

            if (onStartedPlaying)
                onStartedPlaying();
        }
    };
    addAndMakeVisible (playButton);

    juce::Desktop::getInstance().addGlobalMouseListener (&clickElsewhere);
    startTimerHz (30);
}

PreviewStrip::~PreviewStrip()
{
    juce::Desktop::getInstance().removeGlobalMouseListener (&clickElsewhere);
    engine.getDeviceManager().deviceManager.removeAudioCallback (&player);
    transport.setSource (nullptr);
    player.setSource (nullptr);
}

void PreviewStrip::ClickElsewhere::mouseDown (const juce::MouseEvent& e)
{
    if (owner.isPlaying() && e.eventComponent != &owner && ! owner.isParentOf (e.eventComponent))
        owner.stop();
}

bool PreviewStrip::isPlaying() const
{
    return transport.isPlaying();
}

bool PreviewStrip::load (const juce::String& newPath)
{
    if (newPath == path && source != nullptr)
        return true;

    juce::File file (newPath);

    if (! file.existsAsFile())
    {
        if (onStatusMessage)
            onStatusMessage ("That song's file is missing.");
        return false;
    }

    // Played straight from the file (its decoded copy if it's on the timeline already).
    auto newPlayable = mixtopia::decodedCache::getReadableFile (file);
    auto* reader = engine.getAudioFileFormatManager().readFormatManager.createReaderFor (newPlayable);

    if (reader == nullptr)
        return false;

    transport.stop();
    transport.setSource (nullptr);
    source = std::make_unique<juce::AudioFormatReaderSource> (reader, true);
    transport.setSource (source.get(), 65536, &readAhead, reader->sampleRate);
    path = newPath;
    playable = newPlayable;
    repaint();
    return true;
}

void PreviewStrip::setActive (bool shouldBeActive)
{
    if (active != shouldBeActive)
    {
        active = shouldBeActive;

        if (onActiveChanged)
            onActiveChanged();
    }
}

void PreviewStrip::play (const juce::String& newPath, double fromSeconds)
{
    if (! load (newPath))
        return;

    seek (fromSeconds);
    transport.start();
    setActive (true);

    if (onStartedPlaying)
        onStartedPlaying();
}

void PreviewStrip::stop()
{
    transport.stop();
    setActive (false);
}

void PreviewStrip::seek (double seconds)
{
    transport.setPosition (juce::jlimit (0.0, juce::jmax (0.0, transport.getLengthInSeconds() - 0.01), seconds));
}

void PreviewStrip::jumpBars (int bars)
{
    if (path.isEmpty())
        return;

    // On the song's own beat grid: to the bar line `bars` bars on (8 bars = a phrase).
    auto* song = library.find (path);
    auto bpm = song != nullptr && song->bpm > 0.0 ? song->bpm : 0.0;
    auto now = transport.getCurrentPosition();

    if (bpm > 0.0)
    {
        auto bar = 4.0 * 60.0 / bpm;
        auto index = std::floor ((now - song->downbeat) / bar + 1.0e-6);
        seek (song->downbeat + (index + bars) * bar);
    }
    else
    {
        seek (now + bars * 2.0);   // no grid: 2 s per "bar"
    }
}

//==============================================================================
juce::Rectangle<int> PreviewStrip::getWaveformArea() const
{
    auto r = getLocalBounds().reduced (6, 6);
    r.removeFromLeft (26 + 8);                                    // play button
    r.removeFromLeft (juce::jmin (260, r.getWidth() / 3) + 8);    // name
    return r;
}

double PreviewStrip::xToSeconds (int x) const
{
    auto area = getWaveformArea();
    return juce::jlimit (0.0, 1.0, (x - area.getX()) / (double) juce::jmax (1, area.getWidth())) * transport.getLengthInSeconds();
}

void PreviewStrip::resized()
{
    auto r = getLocalBounds().reduced (6, 6);
    playButton.setBounds (r.removeFromLeft (26).withSizeKeepingCentre (26, 24));
}

void PreviewStrip::paint (juce::Graphics& g)
{
    g.fillAll (stripColour);
    g.setColour (theme::line);
    g.fillRect (0, 0, getWidth(), 1);

    auto r = getLocalBounds().reduced (6, 6);
    r.removeFromLeft (26 + 8);
    auto nameArea = r.removeFromLeft (juce::jmin (260, r.getWidth() / 3));
    auto* song = library.find (path);

    g.setColour (path.isEmpty() ? theme::text3 : theme::text1);
    g.setFont (theme::font (theme::textSmall, path.isNotEmpty()));
    auto name = song == nullptr ? juce::String ("Double-click a song to preview it")
                                : (song->artist.isNotEmpty() ? song->artist + " - " : juce::String()) + song->title;
    g.drawText (name, nameArea, juce::Justification::centredLeft, true);

    auto area = getWaveformArea();
    g.setColour (shapeBackground);
    g.fillRoundedRectangle (area.toFloat(), 3.0f);

    if (path.isEmpty())
        return;

    // A rough shape of the whole song: its average loudness, column by column.
    auto length = transport.getLengthInSeconds();

    if (auto* overview = overviews.get (playable); overview != nullptr && length > 0.0)
    {
        g.setColour (shapeColour);

        for (int x = 0; x < area.getWidth(); x += 2)
        {
            auto t0 = length * x / area.getWidth(), t1 = length * (x + 2) / area.getWidth();
            auto level = juce::jlimit (0.0f, 1.0f, overview->getRelativeLevel (t0, t1));
            auto h = juce::jmax (1.0f, level * 0.95f * (float) area.getHeight());
            g.fillRect ((float) (area.getX() + x), (float) area.getBottom() - h, 2.0f, h);
        }
    }

    if (length > 0.0)
    {
        auto x = (float) area.getX() + (float) (transport.getCurrentPosition() / length) * (float) area.getWidth();
        g.setColour (theme::text1);
        g.fillRect (x - 1.0f, (float) area.getY() - 2.0f, 2.0f, (float) area.getHeight() + 4.0f);
    }
}

void PreviewStrip::timerCallback()
{
    // Played to the end: done.
    if (active && ! isPlaying() && transport.getLengthInSeconds() > 0.0
         && transport.getCurrentPosition() >= transport.getLengthInSeconds() - 0.05)
        setActive (false);

    if (isPlaying() != shownPlaying)
    {
        shownPlaying = isPlaying();
        playButton.setIcon (shownPlaying ? theme::Icon::pause : theme::Icon::play);
        repaint();
    }
    else if (shownPlaying)
    {
        repaint (getWaveformArea().expanded (2, 4));
    }
}

void PreviewStrip::mouseDown (const juce::MouseEvent& e)
{
    if (path.isEmpty() || ! getWaveformArea().expanded (0, 6).contains (e.getPosition()))
        return;

    // Play from where it's clicked.
    seek (xToSeconds (e.x));

    if (! isPlaying())
    {
        transport.start();

        if (onStartedPlaying)
            onStartedPlaying();
    }
}

void PreviewStrip::mouseDrag (const juce::MouseEvent& e)
{
    // Scrub: the playhead follows the mouse and keeps playing from where it is.
    if (path.isNotEmpty() && isPlaying())
        seek (xToSeconds (e.x));
}
