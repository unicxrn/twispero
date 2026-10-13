# core: the Twitch client library

The platform-independent C++17 library behind Twispero, plus `twitch-cli`, a small command-line client for Linux. The PS5 app compiles the same sources (see `TWITCH_CORE_SRCS` in `app/projects/evoplayer/Makefile`).

## Build (Linux)

You need CMake 3.20 or newer, a C++17 compiler, `nlohmann-json` and libcurl 8 with WebSocket support. On Arch that's `pacman -S cmake nlohmann-json curl`.

```bash
cmake -S core -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## twitch-cli

```bash
./build/twitch-cli top 10                      # the most-watched live streams
./build/twitch-cli categories 10               # the top categories
./build/twitch-cli category "Minecraft"        # who is live in one
./build/twitch-cli search <words>              # channels and categories
./build/twitch-cli channel <login>             # a channel page
./build/twitch-cli videos <login>              # past broadcasts (or highlight, upload)
./build/twitch-cli clips <login> week          # clips (day, week, month, all)
./build/twitch-cli vod <id>                    # a video's qualities
./build/twitch-cli clip <slug>                 # a clip's files
./build/twitch-cli play <channel> 720p60       # qualities and current segments, ads marked [AD]
./build/twitch-cli play <channel> source embed # source quality through another player type
./build/twitch-cli chat <channel>              # read chat without logging in
./build/twitch-cli login                       # log in with Twitch's device code
./build/twitch-cli me                          # who you're logged in as
./build/twitch-cli following                   # the channels you follow
./build/twitch-cli say <channel> <text>        # send a chat message
./build/twitch-cli logout                      # forget the saved login
```

Qualities can be picked by Twitch's key (`chunked`, `720p30`), by label (`1080p60`, `720p`), or with `best`, `worst` and `source`.

`login` and the commands after it read and write `~/.config/ps5-twitch/auth.json` (mode 0600, and `XDG_CONFIG_HOME` is respected).

## Layout

- `include/twitch` and `src`: the core. No threads and no platform code; it reaches the network only through the `HttpClient` and `WebSocket` interfaces. An unexpected response becomes a `twitch::ApiError`, never a crash or a raw JSON exception.
- `platform/curl`: libcurl versions of those interfaces for Linux, HTTPS and WSS only, with caps on response sizes.
- `cli`: `twitch-cli`.
- `third_party`: nlohmann/json (MIT) and qrcodegen 1.8.0 (MIT).

## Unofficial endpoints

`gql.twitch.tv` (with Twitch's web client ID) and `usher.ttvnw.net` aren't public APIs. Everything that touches them lives in `gql.cpp`, `streams.cpp`, `browse.cpp`, `playback.cpp` and `videos.cpp`. If Twitch changes them, those files are where to look.

`hls.cpp` also hard-codes Twitch's own ad markers (`twitch-stitched-ad` date ranges and the `live` segment title), and `login.cpp` encodes Twitch's username rules. Those are unofficial too, and can drift the same way.
