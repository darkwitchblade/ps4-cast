# Cast to PS4 Chrome extension

Detects non-DRM media playing in Chrome, including media requested inside
cross-origin player iframes, and hands the selected URL directly to PS4 Cast.
The Mac does not proxy or relay video data.

## Install locally

1. Install the matching PS4 Cast package and leave the receiver open.
2. Open `chrome://extensions` in Chrome.
3. Enable **Developer mode**.
4. Choose **Load unpacked** and select this `chrome-extension` directory.
5. Pin **Cast to PS4** to the toolbar.
6. Open the extension, enter the PS4 address shown on the TV, and use the test
   button once.

When a playable stream is detected, a compact circular cast button appears only
beside that video. It prefers the free space outside the player, moves inside
only when the viewport is constrained, and changes to a green check after a
successful handoff. The toolbar popup remains available when a page exposes more
than one candidate or receiver settings need to be changed.

## Detection and privacy

- The broad website permission is required to observe media requests originating
  in third-party iframes. Candidates are kept only in the extension service
  worker's memory and expire automatically.
- Browser cookies, authorization headers, page contents, and browsing history
  are not stored or sent. Only the selected media URL plus Referer, Origin and
  User-Agent are sent to the configured private-LAN receiver.
- Playback is fetched by the PS4 directly. Closing Chrome after a successful
  handoff is safe unless the source URL itself expires.
- DASH (`.mpd`), DRM/EME, encrypted HLS, and providers that require browser
  cookies are reported as unsupported instead of being bypassed.

Use the extension only with media you are authorized to access.

## Pairing

The receiver requires an 8-character pairing token on every command it accepts.
You do not need to type it: `GET /token` is exempt from that check, so the
extension asks the console for it the first time you save or test a receiver and
remembers it. If the receiver ever regenerates its token, the next cast gets a
401, and the extension re-pairs and retries once on its own.

Pasting the TV's URL with `?t=` still works and takes precedence -- useful if you
ever want to pin a specific token by hand.
