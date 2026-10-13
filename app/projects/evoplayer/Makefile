PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF := PS5MediaPlayerPRO.elf

# Progressive 4K product stages: 0=SAFE 1=G_VO 2=G_V8 3=G_AUDIO 4=H_FULL
# Default 4 after G-H stage ladder PASS (2026-07-21). Rollback: STAGE=0
STAGE ?= 4

EVO_VERSION := $(shell tr -d '[:space:]' < VERSION)

EXTRA_CFLAGS ?=

CXXFLAGS := -Wall -g -O2 -std=c++17 $(EXTRA_CFLAGS) \
    -DEVO_PLAYER_VERSION=\"$(EVO_VERSION)\" \
    -DEVO_TARGET_PS5=1 \
    -Icore/include \
    -Iinclude \
    -Ipp/include \
    -Iui/include \
    -Iui_rml/include \
    -Iui_kit/include \
    -Imedia/include \
    -Iaddons/include \
    -I$(PS5_PAYLOAD_SDK)/target/user/homebrew/include/SDL2 \
    -I$(PS5_PAYLOAD_SDK)/target/user/homebrew/include \
    -I$(PS5_PAYLOAD_SDK)/target/user/homebrew/include/freetype2 \
    -DPP_4K_PRODUCT_STAGE=$(STAGE)

CFLAGS := -Wall -g -O2 $(EXTRA_CFLAGS) \
    -DEVO_PLAYER_VERSION=\"$(EVO_VERSION)\" \
    -DEVO_TARGET_PS5=1 \
    -Icore/include \
    -Iinclude \
    -Ipp/include \
    -Iui/include \
    -Iui_rml/include \
    -Imedia/include \
    -Iaddons/include \
    -I$(PS5_PAYLOAD_SDK)/target/user/homebrew/include/SDL2 \
    -I$(PS5_PAYLOAD_SDK)/target/user/homebrew/include \
    -DPP_4K_PRODUCT_STAGE=$(STAGE)

LIBS := \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/librmlui.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libfreetype.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libharfbuzz.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libpng16.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libz.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libbz2.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/liblzma.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libzstd.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libSDL2.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libavformat.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libavcodec.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libswresample.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libavutil.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libswscale.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libdav1d.a \
    -lssl \
    -lcrypto \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libxml2.a \
    $(PS5_PAYLOAD_SDK)/target/user/homebrew/lib/libiconv.a \
    -lSceNotification \
    -lSceSystemService \
    -lSceUserService \
    -lScePad \
    -lSceAudioOut \
    -lSceVideoOut \
    -lSceKeyboard \
    -lSceImeDialog \
    -lc++ -lc++abi -lpthread

# GL-4 (#80) Stage 3: the CPU YUV->BGRA converters (pp_converter[_parallel
# |_fused], pp_compute_pipeline) and the tiled VideoOut blit (tile_copy) are
# gone - the video goes up as R8/RG8 planes + a GLSL shader.
# GL-6 (#82): the hand-rolled sceAgc present/geo/OSD path (pp_agc*) and the
# sceVideoOut buffer manager (pp_videoout) are deleted - ps5-opengl owns
# sceAgc + sceVideoOut for the whole session (evo_gl_context_device.cpp).
PP_SRCS := \
    pp/src/pp_audio_clock.c \
    pp/src/pp_stage_breadcrumb.c \
    pp/src/pp_clock.c \
    pp/src/pp_playback.c \
    pp/src/evo_theme.c

UI_SRCS := \
    ui/src/evo_input.c \
    ui/src/evo_feedback.c \
    ui/src/evo_keyboard.c

MEDIA_SRCS := \
    media/src/prospero_thumbnail.c \
    media/src/evo_direct_mem.c \
    media/src/evo_stream_io.c \
    media/src/evo_playlist_sniff.c \
    media/src/evo_parallel_io.c \
    media/src/evo_packet_queue.c \
    media/src/evo_audio_resample.c \
    media/src/evo_audio_out.c \
    media/src/evo_hw.c \
    media/src/evo_perf_monitor.c \
    media/src/evo_demux.c \
    media/src/evo_vdec_ffmpeg.c \
    media/src/evo_dovi.c \
    media/src/evo_adec_native.c \
    media/src/evo_vdec_native.c \
    media/src/evo_playback.c \
    src/evo_toast.c \
    src/evo_data_path.c \
    src/evo_boot_log.c \
    src/evo_error.c \
    src/evo_log_server.c \
    src/evo_stats.c \
    src/evo_usb_remote.c \
    src/evo_jailbreak.c \
    src/evo_prx_keep.c

# evo_net (HTTP/TLS, used by the Twitch module) and the CA roots it trusts.
ADDON_SRCS := \
    addons/src/evo_net.c \
    addons/src/evo_ca_bundle.c

# The bare-metal AGC device context and its 100% GPU RmlUi render interface
# are the only device render path - the AGC runtime owns sceAgc and
# sceVideoOut outright. The OpenGL path, the ps5-opengl submodule and the
# --gl / --gl-smoke / --gl-hdr-probe build modes were removed. This is off
# only for the host compile check, which has no graphics at all.
AGC_DEVICE ?= 1
ifeq ($(AGC_DEVICE),1)
    MEDIA_SRCS += \
        media/src/evo_agc_transient_ring.c \
        media/src/evo_agc_writer.c \
        media/src/evo_agc_shader_header.c \
        media/src/evo_agc_runtime.c
    # EVO_AGC_HAVE_VIDEO_PIPES: the four video pipelines are converted to .pipe
    # and compiled by tools/build_agc_pipes.py, so the video present path is
    # live. Drop it to bring the runtime up with the UI pipeline only.
    CFLAGS   += -DEVO_AGC_DEVICE=1 -DEVO_AGC_HAVE_VIDEO_PIPES=1 -Ishaders/agc
    CXXFLAGS += -DEVO_AGC_DEVICE=1 -DEVO_AGC_HAVE_VIDEO_PIPES=1 -Ishaders/agc
endif

C_SRCS := $(PP_SRCS) $(UI_SRCS) $(MEDIA_SRCS) $(ADDON_SRCS)
C_OBJS := $(C_SRCS:.c=.o)

CORE_SRCS := \
    main.cpp \
    core/src/Application.cpp \
    core/src/Bridge.cpp \
    core/src/services/SettingsService.cpp \
    core/src/services/MediaMetadataService.cpp \
    core/src/services/SoundEffectEngine.cpp \
    core/src/services/PlaybackController.cpp \
    core/src/screens/ScreenManager.cpp \
    core/src/screens/PlayerScreen.cpp \
    core/src/screens/SettingsScreen.cpp \
    core/src/screens/AudioTrackPickerScreen.cpp \
    core/src/screens/AboutSupportScreen.cpp \
    core/src/screens/SafeToCloseScreen.cpp \
    core/src/screens/ModalDialogScreen.cpp \
    core/src/screens/StatefulScreen.cpp \
    core/src/screens/TwitchHomeScreen.cpp \
    core/src/screens/TwitchBrowseScreen.cpp \
    core/src/screens/TwitchCategoryScreen.cpp \
    core/src/screens/TwitchSearchScreen.cpp \
    core/src/screens/TwitchChannelScreen.cpp \
    core/src/screens/TwitchLoginScreen.cpp \
    core/src/screens/TwitchFollowingScreen.cpp

CORE_OBJS := $(CORE_SRCS:.cpp=.o)

# Twitch core (stage 2 task 6): sources from the read-only core mount at
# TWITCH_CORE_DIR, compiled into this app build. The twitch module below
# (stage 2 task 7: evo_twitch_launcher.cpp and friends) calls into these -
# objects land under twitch/obj, inside the app tree, since the core mount
# itself is read-only.
TWITCH_CORE_DIR ?= /twitch-core
TWITCH_CORE_SRCS := \
    url.cpp \
    gql.cpp \
    streams.cpp \
    browse.cpp \
    hls.cpp \
    playback.cpp \
    irc.cpp \
    chat.cpp \
    login.cpp \
    videos.cpp \
    stream_websocket.cpp \
    chat_log.cpp \
    third_party_emotes.cpp \
    badges.cpp \
    settings.cpp \
    progress.cpp \
    history.cpp \
    notify.cpp \
    auth.cpp \
    helix.cpp \
    qr.cpp
TWITCH_CORE_OBJS := $(addprefix twitch/obj/core_,$(TWITCH_CORE_SRCS:.cpp=.o))

# JSON_NO_IO plus the filesystem switches keep the vendored nlohmann header
# off <iostream>/<filesystem> - EVO forbids iostreams.
TWITCH_CORE_CXXFLAGS := $(CXXFLAGS) -std=c++17 -fexceptions \
    -DJSON_NO_IO -DJSON_HAS_FILESYSTEM=0 -DJSON_HAS_EXPERIMENTAL_FILESYSTEM=0 \
    -I$(TWITCH_CORE_DIR)/include -I$(TWITCH_CORE_DIR)/third_party

twitch/obj/core_%.o: $(TWITCH_CORE_DIR)/src/%.cpp
	@mkdir -p $(dir $@)
	$(CCACHE) $(CXX) $(TWITCH_CORE_CXXFLAGS) $(DEPFLAGS) -c -o $@ $<

# qrcodegen.cpp lives under third_party/, not src/, so the pattern rule above
# (which only matches $(TWITCH_CORE_DIR)/src/%.cpp) can't build it; it gets
# its own rule further down, right after `all:` (a rule naming one concrete
# file, unlike a %-pattern rule, counts as Make's default goal if it is the
# first such rule in the file - it must not land above `all:`).
TWITCH_CORE_OBJS += twitch/obj/core_qrcodegen.o

# Twitch module (stage 2 task 7): the EVO side of the core - the HTTP adapter
# on evo_net, the boot self-test, the launcher, the data service, the image
# service, the chat service (and its sender), the clip maker, the login
# service, the videos' watch progress, the watch history and the go-live
# notifications.
# Same flags as the core plus its own headers; objects next to the core's in
# twitch/obj. twitch/tests/ is host-only and never built here.
TWITCH_MOD_SRCS := \
    twitch/src/evo_twitch_http.cpp \
    twitch/src/evo_twitch_selftest.cpp \
    twitch/src/evo_twitch_launcher.cpp \
    twitch/src/evo_twitch_data.cpp \
    twitch/src/evo_twitch_images.cpp \
    twitch/src/evo_twitch_stream.cpp \
    twitch/src/evo_twitch_settings.cpp \
    twitch/src/evo_twitch_progress.cpp \
    twitch/src/evo_twitch_history.cpp \
    twitch/src/evo_twitch_alerts.cpp \
    twitch/src/evo_twitch_chat.cpp \
    twitch/src/evo_twitch_chat_send.cpp \
    twitch/src/evo_twitch_clip.cpp \
    twitch/src/evo_twitch_auth.cpp
TWITCH_MOD_OBJS := $(patsubst twitch/src/%.cpp,twitch/obj/mod_%.o,$(TWITCH_MOD_SRCS))

twitch/obj/mod_%.o: twitch/src/%.cpp
	@mkdir -p $(dir $@)
	$(CCACHE) $(CXX) $(TWITCH_CORE_CXXFLAGS) -Itwitch/include $(DEPFLAGS) -c -o $@ $<

# Application.cpp, the Twitch screens and the dev remote call the module's
# entry points (evo_twitch.h and its data and image services).
core/src/Application.o: CXXFLAGS += -Itwitch/include
core/src/screens/TwitchHomeScreen.o: CXXFLAGS += -Itwitch/include
core/src/screens/TwitchBrowseScreen.o: CXXFLAGS += -Itwitch/include
core/src/screens/TwitchCategoryScreen.o: CXXFLAGS += -Itwitch/include
core/src/screens/TwitchSearchScreen.o: CXXFLAGS += -Itwitch/include
core/src/screens/TwitchChannelScreen.o: CXXFLAGS += -Itwitch/include
# The quality picker lists the launcher's qualities and asks it to switch.
core/src/screens/AudioTrackPickerScreen.o: CXXFLAGS += -Itwitch/include
src/evo_usb_remote.o: CFLAGS += -Itwitch/include
# Application.cpp pumps the chat service and the player draws chat: their
# headers name the core's chat types (twitch/chat_log.hpp, twitch/badges.hpp,
# twitch/third_party_emotes.hpp), which stay clear of the JSON library.
core/src/Application.o: CXXFLAGS += -I$(TWITCH_CORE_DIR)/include
core/src/screens/PlayerScreen.o: CXXFLAGS += -Itwitch/include -I$(TWITCH_CORE_DIR)/include
# The login screen, Settings' Twitch account row, Following and Home read the
# login service, whose view carries the core's QR type (twitch/qr.hpp).
core/src/screens/TwitchLoginScreen.o: CXXFLAGS += -Itwitch/include -I$(TWITCH_CORE_DIR)/include
core/src/screens/SettingsScreen.o: CXXFLAGS += -Itwitch/include -I$(TWITCH_CORE_DIR)/include
# SettingsService keeps the settings in the app's one file through
# evo_twitch_settings.h, whose AppSettings is the core's (twitch/settings.hpp).
core/src/services/SettingsService.o: CXXFLAGS += -Itwitch/include -I$(TWITCH_CORE_DIR)/include
core/src/screens/TwitchFollowingScreen.o: CXXFLAGS += -Itwitch/include -I$(TWITCH_CORE_DIR)/include
core/src/screens/TwitchHomeScreen.o: CXXFLAGS += -I$(TWITCH_CORE_DIR)/include

RMLUI_SRCS := \
    ui_rml/src/evo_rmlui_render.cpp \
    ui_rml/src/evo_rmlui_system.cpp \
    ui_rml/src/evo_rmlui_app.cpp \
    ui_rml/src/evo_rmlui_bridge.cpp \
    ui_rml/src/evo_rmlui_fileinterface.cpp \
    ui_rml/src/evo_rmlui_bundle.cpp \
    ui_rml/src/evo_rmlui_bundle_data.cpp \
    ui_rml/src/evo_rmlui_devstate.cpp

ifeq ($(AGC_DEVICE),1)
    RMLUI_SRCS += ui_rml/src/evo_rmlui_render_agc.cpp
endif

RMLUI_OBJS := $(RMLUI_SRCS:.cpp=.o)

# ps5-homebrew-ui (third_party/ps5-homebrew-ui): the UI kit EVO's own screens
# are moving to. Only the parts that build a DrawList are compiled - the kit's
# OpenGL backend (gfx/gl_*, renderer, canvas, backdrop), its platform layer and
# the music player (needs stb_vorbis) are not; ui_kit/src/hui_agc_batch.cpp
# draws the lists through sceAgc instead. The kit is C++20; its objects land
# under ui_kit/obj so the vendored tree stays untouched.
HUI_ROOT := ../../third_party/ps5-homebrew-ui
HUI_SRCS := \
    gfx/draw_list.cpp gfx/font.cpp gfx/triangulate.cpp \
    $(patsubst $(HUI_ROOT)/src/%,%,$(wildcard $(HUI_ROOT)/src/ui/*.cpp)) \
    $(patsubst $(HUI_ROOT)/src/%,%,$(wildcard $(HUI_ROOT)/src/ui/components/*.cpp)) \
    $(patsubst $(HUI_ROOT)/src/%,%,$(wildcard $(HUI_ROOT)/src/core/*.cpp)) \
    audio/cues.cpp audio/mixer.cpp audio/wav.cpp
HUI_OBJS := $(addprefix ui_kit/obj/hui/,$(HUI_SRCS:.cpp=.o))

UI_KIT_SRCS :=
ifeq ($(AGC_DEVICE),1)
    UI_KIT_SRCS += ui_kit/src/hui_agc_batch.cpp ui_kit/src/evo_hui_app.cpp \
                   ui_kit/src/evo_hui_screens.cpp \
                   ui_kit/src/evo_hui_settings.cpp \
                   ui_kit/src/evo_hui_osd.cpp \
                   ui_kit/src/evo_hui_misc.cpp \
                   ui_kit/src/evo_hui_extra.cpp \
                   ui_kit/src/evo_hui_twitch_common.cpp \
                   ui_kit/src/evo_hui_twitch_home.cpp \
                   ui_kit/src/evo_hui_twitch_browse.cpp \
                   ui_kit/src/evo_hui_twitch_search.cpp \
                   ui_kit/src/evo_hui_twitch_channel.cpp \
                   ui_kit/src/evo_hui_twitch_videos.cpp \
                   ui_kit/src/evo_hui_twitch_chat.cpp \
                   ui_kit/src/evo_hui_twitch_clip.cpp \
                   ui_kit/src/evo_hui_twitch_login.cpp \
                   ui_kit/src/evo_hui_twitch_following.cpp
endif
UI_KIT_OBJS := $(UI_KIT_SRCS:.cpp=.o)

HUI_CXXFLAGS := $(filter-out -std=c++17,$(CXXFLAGS)) -std=c++20 \
    -I$(HUI_ROOT)/src -I$(HUI_ROOT) -Iui_kit/include

ui_kit/obj/hui/%.o: $(HUI_ROOT)/src/%.cpp
	@mkdir -p $(dir $@)
	$(CCACHE) $(CXX) $(HUI_CXXFLAGS) $(DEPFLAGS) -c -o $@ $<

ui_kit/src/%.o: ui_kit/src/%.cpp
	$(CCACHE) $(CXX) $(HUI_CXXFLAGS) $(DEPFLAGS) -c -o $@ $<

# Chat in the player draws around the video's place (evo_twitch_chat_layout.h).
ui_kit/src/evo_hui_twitch_chat.o: HUI_CXXFLAGS += -Itwitch/include

# -MMD -MP: emit a .d next to each .o listing its header prerequisites, so an
# object rebuilds when a header it includes changes. Without this, editing e.g.
# a shared ABI header left stale .o files linked against the old ABI (#28, cost
# real hardware cycles). package-app.sh's app-cflags.stamp only force-cleans on
# a *flag* change, not a header edit.
DEPFLAGS := -MMD -MP
ALL_DEPS := $(C_OBJS:.o=.d) $(RMLUI_OBJS:.o=.d) $(CORE_OBJS:.o=.d) $(HUI_OBJS:.o=.d) $(UI_KIT_OBJS:.o=.d) $(TWITCH_CORE_OBJS:.o=.d) $(TWITCH_MOD_OBJS:.o=.d)

# CCACHE=ccache (set by scripts/package-app.sh and build-evoplayer.sh) caches each
# object by its preprocessed source + flags, so switching between the payload
# compile check and the app-module build, or between --usb-remote and release,
# does not recompile unchanged files. Empty = compile directly.
CCACHE ?=

%.o: %.c
	$(CCACHE) $(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

%.o: %.cpp
	$(CCACHE) $(CXX) $(CXXFLAGS) $(DEPFLAGS) -c -o $@ $<

all: $(ELF)

# qrcodegen.cpp's own rule (see the TWITCH_CORE_OBJS comment above): it lives
# under third_party/, not src/, so the core_%.o pattern rule can't build it,
# and since it includes "qrcodegen.hpp" bare (no subdirectory prefix), it
# needs its own extra include path too.
twitch/obj/core_qrcodegen.o: $(TWITCH_CORE_DIR)/third_party/qrcodegen/qrcodegen.cpp
	@mkdir -p $(dir $@)
	$(CCACHE) $(CXX) $(TWITCH_CORE_CXXFLAGS) -I$(TWITCH_CORE_DIR)/third_party/qrcodegen $(DEPFLAGS) -c -o $@ $<

# The CA roots evo_net's EVO_NET_VERIFY_TLS trusts: generated from the dev
# image's /etc/ssl/certs/ca-certificates.crt and git-ignored. build-evoplayer.sh
# and package-app.sh regenerate it before every build; this rule makes it when
# it is missing.
addons/src/evo_ca_bundle.c:
	python3 ../../tools/gen_ca_bundle.py

# The app's name and title id, from sce_sys/param.json (EVO_APP_NAME and
# EVO_TITLE_ID, read by include/evo_about_text.hpp). Generated and git-ignored
# like the CA bundle. Every object waits for it (order-only); the .d files
# rebuild the ones that include it when it changes. The script rewrites the
# header only when its content changes.
include/evo_identity.h: sce_sys/param.json ../../tools/gen_identity.py
	python3 ../../tools/gen_identity.py

# The RmlUi assets built into the binary (tools/bundle_rml_assets.py).
# build-evoplayer.sh and package-app.sh regenerate it before every build; this
# rule makes it when it is missing.
ui_rml/src/evo_rmlui_bundle_data.cpp:
	python3 ../../tools/bundle_rml_assets.py
$(C_OBJS) $(RMLUI_OBJS) $(CORE_OBJS) $(HUI_OBJS) $(UI_KIT_OBJS) $(TWITCH_CORE_OBJS) $(TWITCH_MOD_OBJS): | include/evo_identity.h

-include $(ALL_DEPS)

# -----------------------------------------------------------------------------
# App-module packaging hooks (Phase 1b - docs/evo-pro/phase-1b-app-module.md).
# scripts/package-app.sh compiles the object set with the native-app toolchain
# (prospero-clang18: -femulated-tls -fno-plt -fno-stack-protector) and does its
# own link/convert/sign, so it needs the objects without the SDK-crt link.
#   objects        build every .o, no ELF
#   print-objects  emit the object paths (relative to this dir), space-separated
# -----------------------------------------------------------------------------
.PHONY: objects print-objects
objects: $(C_OBJS) $(RMLUI_OBJS) $(CORE_OBJS) $(HUI_OBJS) $(UI_KIT_OBJS) $(TWITCH_CORE_OBJS) $(TWITCH_MOD_OBJS)

print-objects:
	@echo $(C_OBJS) $(RMLUI_OBJS) $(CORE_OBJS) $(HUI_OBJS) $(UI_KIT_OBJS) $(TWITCH_CORE_OBJS) $(TWITCH_MOD_OBJS)

$(ELF): $(C_OBJS) $(RMLUI_OBJS) $(CORE_OBJS) $(HUI_OBJS) $(UI_KIT_OBJS) $(TWITCH_CORE_OBJS) $(TWITCH_MOD_OBJS)
	$(CXX) $(CFLAGS) -o $@ $^ $(LIBS)
	@echo "STAGE=$(STAGE)"
	@sha256sum $@
	@ls -la $@

clean:
	rm -f $(ELF) $(C_OBJS) $(RMLUI_OBJS) $(CORE_OBJS) $(HUI_OBJS) $(UI_KIT_OBJS) $(TWITCH_CORE_OBJS) $(TWITCH_MOD_OBJS) $(ALL_DEPS)
	rm -rf artifacts/4k_stages ui_kit/obj twitch/obj
