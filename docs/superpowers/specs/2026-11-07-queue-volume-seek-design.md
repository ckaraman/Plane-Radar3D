# Volume on the queue screen, seeking on the time bar (10n): short design

## Intent

On the card's music page, with the queue panel open, the user has no volume control.
They also want to touch or drag the time bar to jump to any second of the song. Both
are for everyday listening in the browser (YouTube Music, Spotify web in Chrome).

## Goals

- **Queue screen volume:** a volume bar on the queue screen, working like the one on the
  music page (drag, tap; same helper messages `SETVOL` / `MUTE`).
- **Seeking:** on both screens (the music page and the queue screen), a tap on the time
  bar jumps there. Dragging grows the knob and shows the target time in a bubble above
  it. The jump happens when the finger lifts. While the finger is on the bar, the page
  does not turn.

## Non-goals

- Seeking in desktop apps (Spotify desktop and others). If an app does not take it, the
  bar stays a display only.
- Opera GX side-panel players (no tab ids; left as is, see 10m).
- Changes to the browser extension.

## Anti-goals

- A drag on the time bar must never turn the page, skip a song or change the volume.
- With an older helper, the card must not pretend to seek (a knob that jumps and snaps back).

## Approaches

Queue screen volume layout (picked from mockups, 07.11):

- **A (picked):** prev / play / next move left, a shorter volume bar (speaker icon and
  bar) to their right. Same order as the music page.
- B: a long bar beside the cover, the buttons stay. Not picked.
- C: a speaker button that opens a pop-up bar. Not picked: one extra tap every time.

Seek path:

- **1 (picked): Windows' media controls.** Card -> helper `PR:SEEK {"ms":N}` -> the
  current media session's `TryChangePlaybackPositionAsync` (the same session the helper
  already reads now playing from). Chrome passes it to the page (`mediaSession`
  "seekto"). Works without the extension, also for plain YouTube. On the Mac: the
  MediaRemote Adapter's seek command (its README lists one; must be tried on a Mac).
- 2: through the browser extension (the page's own bar). Not picked: only the tabs the
  extension knows, fragile against site changes, every browser must reload the extension.

## Decided

- **Can seek:** the helper adds `"sk":1` to the `NOW` message only when the session says
  it accepts position changes (Windows: `is_playback_position_enabled` in the session's
  controls; Mac: when the adapter reports it, otherwise on). The card makes the bar
  draggable only then. An older helper sends no `sk`, so an older helper keeps today's
  display-only bar.
- **After a seek:** the card shows the chosen position for a short while (about 3 s) or
  until the helper reports a position near it, so the bar does not jump back while
  Windows still reports the old one.
- **Versions:** card firmware 5in-0.51.0, Deck 1.13.0.

## Left to the builder

- Exact pixel positions of the moved buttons and the shorter volume bar on the queue
  screen (keep the buttons' touch areas no smaller than today).
- Touch area height of the time bar (it takes no touches today) so that it does not
  steal touches from the title or the buttons.
- How the bubble is drawn, within the existing paint helpers.
