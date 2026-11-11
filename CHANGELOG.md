# Changelog

## 1.0.1 - 2026-11-11

Fixed: a song's waveform changed shape while the timeline was scrolled. Each pixel column used to show the song
between its own edges, so moving the view by part of a pixel made every column take a slightly different part of
the song and the shape re-formed - a visible wiggle while scrolling slowly. The shape is now drawn from one-pixel
slices that belong to the song, so scrolling, moving, trimming or cutting a song only changes which column shows
which slice.

Nothing else changed: renders, exports and saved projects are bit-for-bit identical to 1.0.

## 1.0 - 2026-11-07

First public release. Mixtopia builds a DJ mix on a multi-lane timeline: automatic tempo and first-beat detection
with manual correction, tempo ramps along the mix, time-stretching with the pitch kept, drawn volume, 3-band EQ and
filter lines, key detection in Camelot notation, per-song loudness matching and a master limiter, repeating part of
a song, cutting songs into pieces and rearranging them, and export to MP3 or FLAC that sounds exactly like playback.
