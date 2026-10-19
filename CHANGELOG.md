# Changelog

What changed in each release of Twispero. The newest is at the top.

## 1.1.0

A live picture on Home, a stream language filter and chat replies, plus fixes for a crash when closing from the PS button menu and for sound and picture going out of sync.

### Added

- Home's top stream plays live in its picture, without sound. Press X to watch it. It stops after 90 seconds without a button press, and Settings > Twitch can turn it off.
- Pick a stream language with L1 and R1 on Browse or on a category's Live tab. Home's Popular now row and every category's live streams follow it.
- Reply to a chat message. L1 pauses chat on the newest message, Up and Down pick another one, and L1 again opens the keyboard. This needs a Twitch login.
- Audio only for live streams in the quality menu (R2), as videos already had

### Fixed

- Closing the app from the PS button menu while something played could crash the console. The app now stops drawing the moment the PS5 takes over, which happens before the close goes through.
- Audio and video drifting apart after the stream buffers on a slow connection (#1). Thanks to jumboz95 for reporting it.
- A blank line on Home, channel pages and the player when a title is all Japanese, Korean or Chinese. The app still can't show those characters, so it skips the line now.
- Twitch's "processing" picture on a past broadcast that's still being recorded. It shows the live picture now.

## 1.0.0

The first release. Please report anything that breaks, and say which firmware you're on.

### Watch

- Live streams, past broadcasts, highlights, uploads and clips, on the PS5's own video decoder
- Pick the quality with R2, from source down to audio only, and set a default in Settings
- Skip 10 seconds with Left and Right in videos and clips, or hold to scrub
- Videos pick up where you stopped, and Home has a Continue watching row
- Home, Browse, categories, Search with your recent searches, and channel pages with uptime, tags, the schedule, videos and clips

### Chat

- Chat next to the video, over it, or off (R1)
- Emotes, badges and name colours, plus 7TV, BetterTTV and FrankerFaceZ emotes
- Replies, a tint on lines that mention you, and a note when slow mode or subscribers-only chat turns on
- Three text sizes

### With a Twitch login

- Log in with a code or a QR code on twitch.tv/activate
- Your followed channels, and a Following row on Home while any of them is live
- A note when a channel you follow goes live
- Send chat messages with the PS5 keyboard
- Clip a live stream with Square, then scan the QR code to see the clip

### Known issues

- Channel points, and following or unfollowing a channel, aren't possible; Twitch only allows them in its own apps
- No chat replay on videos
- If Twitch plays an ad, the picture can freeze until the stream comes back
- Emoji and Chinese, Japanese and Korean characters are left out of names and titles
- Only tested on firmware 13.60

### Install

Unzip PPSA99474.zip, copy the PPSA99474 folder to /data/homebrew on the console, and start the new tile once ShadowMount+ has picked it up. You need a jailbroken PS5 with etaHEN, kstuff and ShadowMount+. The README.txt in the folder has the details.
