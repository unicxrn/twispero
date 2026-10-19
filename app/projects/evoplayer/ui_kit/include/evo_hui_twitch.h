/*
 * evo_hui_twitch.h - the Twitch screens' params and kit entry points.
 *
 * These screens exist only in the kit (no RmlUi document). Their core/ screen
 * fills a params struct every frame, calls the update, then draws through
 * evo_rmlui_render_kit_screen(evo_hui_render_twitch_*, w, h), which handles
 * the RmlUi frame bookkeeping. Main thread only.
 *
 * Lifetimes. Strings are copied in the update; their pointers only need to
 * live through the call. Pictures (evo_hui_twitch_image_t) are NOT copied:
 * the kit keeps the pointer and reads the pixels while it draws, so they must
 * stay valid until this frame's evo_hui_render_twitch_* has returned. The
 * image service keeps what image() returned for 4 frames, so updating and
 * rendering in the same frame, as the core screens do, is enough. Don't
 * render a frame without updating first.
 */
#ifndef EVO_HUI_TWITCH_H
#define EVO_HUI_TWITCH_H

#ifdef __cplusplus
extern "C" {
#endif

/* A picture from the image service (twitch/include/evo_twitch_images.h), as
 * the service hands it out: rows of R,G,B,A bytes, which read as 32-bit words
 * are EVO's 0xAABBGGRR. rgba is NULL until the picture has arrived; the kit
 * then draws a placeholder. id keys the texture (it changes whenever the
 * service reuses a buffer for another picture). */
typedef struct {
    const unsigned char* rgba;
    int                  w;
    int                  h;
    unsigned long long   id;
} evo_hui_twitch_image_t;

typedef struct {
    const char*            login;
    const char*            name;      /* display name */
    const char*            title;     /* stream title */
    const char*            game;      /* category, "" when none */
    int                    viewers;
    evo_hui_twitch_image_t thumb;     /* 16:9 */
} evo_hui_twitch_stream_t;

typedef struct {
    const char*            name;
    int                    viewers;   /* -1 when Twitch did not say */
    evo_hui_twitch_image_t art;       /* 3:4 box art */
} evo_hui_twitch_category_t;

typedef struct {
    const char*            login;
    const char*            name;      /* display name */
    int                    viewers;   /* -1 when offline */
    evo_hui_twitch_image_t avatar;    /* square, drawn as a circle */
} evo_hui_twitch_channel_t;

/* A past broadcast, highlight, upload or clip as a card (stage 7): the
 * thumbnail with its length, the title, "N views · 3 days ago" and one more
 * line. */
typedef struct {
    const char*            id;        /* the video's id or the clip's slug */
    const char*            title;
    const char*            detail;    /* the third line, "" for none: the category, or on a
                                         category's clips the channel */
    const char*            age;       /* "3 days ago", "" when not known */
    int                    views;     /* -1 when not known */
    int                    seconds;   /* its length; 0 hides the chip */
    float                  progress;  /* how much of it was watched, 0..1, drawn as a bar
                                         along the thumbnail's foot; < 0 for none */
    evo_hui_twitch_image_t thumb;     /* 16:9 */
    const char*            chip;      /* the chip's text instead of the length ("42 min left");
                                         NULL or "" for the length */
} evo_hui_twitch_video_t;

/* What a row has to show. */
#define EVO_HUI_TWITCH_LOADING 0   /* skeletons */
#define EVO_HUI_TWITCH_READY   1
#define EVO_HUI_TWITCH_ERROR   2   /* *_error says why */

/* Where the cursor is on a page with tabs: a category, a channel. */
#define EVO_HUI_TWITCH_FOCUS_NONE    (-1)  /* loading or failed: nothing to pick */
#define EVO_HUI_TWITCH_FOCUS_GRID    0     /* on a card */
#define EVO_HUI_TWITCH_FOCUS_TABS    1     /* on the tab that is open */
#define EVO_HUI_TWITCH_FOCUS_FILTERS 2     /* on a filter chip under it */
#define EVO_HUI_TWITCH_FOCUS_WATCH   3     /* a channel's Watch live, only while live */

/* Video and clip cards to a row, on a channel page and a category. */
#define EVO_HUI_TWITCH_VIDEO_COLS 4

/* The filter chips under a Videos tab and a Clips tab, Twitch's own: past
 * broadcasts, highlights and uploads; clips from the last 24 hours, 7 days,
 * 30 days or all time. The selected chip is filled. */
#define EVO_HUI_TWITCH_VIDEOS_ARCHIVE   0
#define EVO_HUI_TWITCH_VIDEOS_HIGHLIGHT 1
#define EVO_HUI_TWITCH_VIDEOS_UPLOAD    2
#define EVO_HUI_TWITCH_VIDEO_FILTERS    3
#define EVO_HUI_TWITCH_CLIPS_DAY        0
#define EVO_HUI_TWITCH_CLIPS_WEEK       1
#define EVO_HUI_TWITCH_CLIPS_MONTH      2
#define EVO_HUI_TWITCH_CLIPS_ALL        3
#define EVO_HUI_TWITCH_CLIP_FILTERS     4

/* Where the cursor is on Home. */
#define EVO_HUI_TWITCH_ROW_HERO       0
#define EVO_HUI_TWITCH_ROW_FOLLOWED   1   /* only while followed_shown */
#define EVO_HUI_TWITCH_ROW_STREAMS    2
#define EVO_HUI_TWITCH_ROW_CATEGORIES 3
#define EVO_HUI_TWITCH_ROW_CONTINUE   4   /* only while continue_shown; drawn under Recently watched */
#define EVO_HUI_TWITCH_ROW_RECENT     5   /* only while recent_shown; drawn under Following */

#define EVO_HUI_TWITCH_HOME_STREAMS    20
#define EVO_HUI_TWITCH_HOME_CATEGORIES 30
#define EVO_HUI_TWITCH_HOME_FOLLOWED   20
#define EVO_HUI_TWITCH_HOME_CONTINUE   20
#define EVO_HUI_TWITCH_HOME_RECENT     30

/* Home's rows on the 1920 x 1080 canvas, for core/ to tell which cards the
 * kit draws (and so which pictures to ask for). A row scrolls sideways by
 * the least distance that keeps the cursor's card its margin (a share of its
 * width) inside the view (hui::ui::Scroller::reveal); a card is drawn while
 * any of it is between DRAWN_LEFT and DRAWN_RIGHT. */
#define EVO_HUI_TWITCH_HOME_VIEW_X          192.0f
#define EVO_HUI_TWITCH_HOME_VIEW_W          1632.0f
#define EVO_HUI_TWITCH_HOME_GAP             24.0f
#define EVO_HUI_TWITCH_HOME_STREAM_W        304.0f   /* Popular now and Following */
#define EVO_HUI_TWITCH_HOME_STREAM_MARGIN   0.6f
#define EVO_HUI_TWITCH_HOME_CATEGORY_W      216.0f
#define EVO_HUI_TWITCH_HOME_CATEGORY_MARGIN 0.8f
#define EVO_HUI_TWITCH_HOME_RECENT_W        216.0f   /* Recently watched: a column, the avatar centred in it */
#define EVO_HUI_TWITCH_HOME_RECENT_MARGIN   0.8f
#define EVO_HUI_TWITCH_HOME_DRAWN_LEFT      88.0f
#define EVO_HUI_TWITCH_HOME_DRAWN_RIGHT     1920.0f

/* Home: the top stream as the hero, the next ones in "Popular now", then
 * "Top categories". While logged in with followed channels live, "Following
 * · live now" sits between the hero and Popular now, and the rows under it
 * move down to make room; with followed_shown 0 Home is drawn exactly as
 * without it. "Continue watching" (stage 7, logged in or not) does the same
 * under Following: the videos with saved progress, their cards the size of a
 * stream's with two lines (the title, then `age`, which carries the channel;
 * views -1, detail "") and "42 min left" in the chip. "Recently watched"
 * (stage 7, logged in or not) comes between Following and Continue watching:
 * the channels watched live, newest first, as round avatars with their names,
 * a live one ringed in red with "N watching" under it (viewers >= 0). Down
 * goes hero, Following, Recently watched, Continue watching, Popular now, Top
 * categories. The kit scrolls the rows and the page to keep the cursor in
 * view, so the params carry only where the cursor is. */
typedef struct {
    int                    streams_status;    /* EVO_HUI_TWITCH_LOADING / READY / ERROR */
    const char*            streams_error;
    int                    stream_count;      /* streams[0] is the hero; the row shows 1.. */
    evo_hui_twitch_stream_t streams[EVO_HUI_TWITCH_HOME_STREAMS];
    evo_hui_twitch_image_t hero_art;          /* a larger picture of streams[0], or none yet */

    int                    categories_status;
    const char*            categories_error;
    int                    category_count;
    evo_hui_twitch_category_t categories[EVO_HUI_TWITCH_HOME_CATEGORIES];

    int                    focus_row;         /* EVO_HUI_TWITCH_ROW_*, kept while the rail is open */
    int                    stream_col;        /* cursor in Popular now: streams[1 + stream_col] */
    int                    category_col;      /* cursor in Top categories */
    int                    rail_focused;      /* 1 while the rail has the cursor: no ring, the page stays */
    const char*            starting_login;    /* a stream we asked for is starting, or NULL */

    int                    followed_shown;    /* 1: "Following · live now" is there (stage 5) */
    int                    followed_count;
    evo_hui_twitch_stream_t followed[EVO_HUI_TWITCH_HOME_FOLLOWED];
    int                    followed_col;      /* cursor in Following · live now */

    int                    continue_shown;    /* 1: "Continue watching" is there (stage 7) */
    int                    continue_count;
    evo_hui_twitch_video_t continue_items[EVO_HUI_TWITCH_HOME_CONTINUE];
    int                    continue_col;      /* cursor in Continue watching */
    const char*            starting_id;       /* a video we asked for is starting, or NULL */

    int                    recent_shown;      /* 1: "Recently watched" is there (stage 7) */
    int                    recent_count;
    evo_hui_twitch_channel_t recent[EVO_HUI_TWITCH_HOME_RECENT];  /* viewers >= 0 while live */
    int                    recent_col;        /* cursor in Recently watched */
    int                    live_picture;      /* 1: the hero art is a live picture; leave that rect clear */
    const char*            streams_language;  /* Popular now's language ("German") after its title;
                                                 "" or NULL for every language */
} evo_hui_twitch_home_params_t;

/* The hero's art on the 1920x1080 page, where Home's live picture goes
 * (Application sets the video rect from these). */
#define EVO_HUI_TWITCH_HERO_X 1024
#define EVO_HUI_TWITCH_HERO_Y 64
#define EVO_HUI_TWITCH_HERO_W 800
#define EVO_HUI_TWITCH_HERO_H 450

void evo_hui_update_twitch_home(const evo_hui_twitch_home_params_t* params);
int  evo_hui_render_twitch_home(int width, int height);

#define EVO_HUI_TWITCH_BROWSE_CATEGORIES 30
#define EVO_HUI_TWITCH_CATEGORY_STREAMS  30
/* Cards to a row: core/ moves the cursor on the same grid the kit draws. */
#define EVO_HUI_TWITCH_BROWSE_COLS       6
#define EVO_HUI_TWITCH_CATEGORY_COLS     4

/* Browse: the top categories as a grid of box art, six to a row. The kit
 * scrolls the page to keep the cursor's row in view. */
typedef struct {
    int                    status;            /* EVO_HUI_TWITCH_LOADING / READY / ERROR */
    const char*            error;
    int                    category_count;
    evo_hui_twitch_category_t categories[EVO_HUI_TWITCH_BROWSE_CATEGORIES];
    int                    cursor;            /* the card the cursor is on, kept while the rail is open */
    int                    rail_focused;      /* 1 while the rail has the cursor: no ring, the page stays */
    const char*            language;          /* "German streams" after the line under the title when
                                                 a stream language is picked; "" or NULL for every one */
} evo_hui_twitch_browse_params_t;

void evo_hui_update_twitch_browse(const evo_hui_twitch_browse_params_t* params);
int  evo_hui_render_twitch_browse(int width, int height);

#define EVO_HUI_TWITCH_CATEGORY_CLIPS    30

/* A category's tabs. */
#define EVO_HUI_TWITCH_CATEGORY_TAB_LIVE  0
#define EVO_HUI_TWITCH_CATEGORY_TAB_CLIPS 1

/* One category: its box art, name and viewers, the Live and Clips tabs, and
 * under them the open tab's grid four to a row: the live streams, or the
 * clip chips (EVO_HUI_TWITCH_CLIPS_*) over the clips. The page scrolls to
 * keep the cursor's row in view. */
typedef struct {
    const char*            name;
    int                    viewers;           /* -1 when not known */
    evo_hui_twitch_image_t art;               /* 3:4 box art, or none */
    int                    status;            /* the streams: EVO_HUI_TWITCH_LOADING / READY / ERROR */
    const char*            error;
    int                    stream_count;
    evo_hui_twitch_stream_t streams[EVO_HUI_TWITCH_CATEGORY_STREAMS];
    int                    cursor;            /* in the streams, kept while the cursor is elsewhere */
    int                    rail_focused;
    const char*            starting_login;    /* a stream we asked for is starting, or NULL */

    /* Stage 7. Zeroed, the Live tab is open with the cursor on its grid. */
    int                    tab;               /* EVO_HUI_TWITCH_CATEGORY_TAB_LIVE / _CLIPS */
    int                    focus;             /* EVO_HUI_TWITCH_FOCUS_GRID / _TABS / _FILTERS */
    int                    filter;            /* the clips' chip: EVO_HUI_TWITCH_CLIPS_* */
    int                    filter_cursor;     /* the chip under the cursor, on FILTERS */
    int                    clips_status;
    const char*            clips_error;
    int                    clip_count;
    evo_hui_twitch_video_t clips[EVO_HUI_TWITCH_CATEGORY_CLIPS];  /* detail: the channel */
    int                    clip_cursor;       /* in the clips, kept while the cursor is elsewhere */
    const char*            starting_id;       /* a clip we asked for is starting, or NULL */
    const char*            language;          /* the live list's: "German streams" after the viewers,
                                                 and L1/R1 offered; "" or NULL for every language */
} evo_hui_twitch_category_params_t;

void evo_hui_update_twitch_category(const evo_hui_twitch_category_params_t* params);
int  evo_hui_render_twitch_category(int width, int height);

#define EVO_HUI_TWITCH_SEARCH_CHANNELS   10
#define EVO_HUI_TWITCH_SEARCH_CATEGORIES 10
#define EVO_HUI_TWITCH_SEARCH_RECENTS    8

/* Where the cursor is on Search. */
#define EVO_HUI_TWITCH_SEARCH_FIELD      0
#define EVO_HUI_TWITCH_SEARCH_CHANNEL    1
#define EVO_HUI_TWITCH_SEARCH_CATEGORY   2
#define EVO_HUI_TWITCH_SEARCH_RECENT     3   /* only with no query and some recent searches */

/* Search: the field, then "Channels" and "Categories" for the query, each a
 * row that scrolls sideways. An empty query shows the field alone, with the
 * recent searches under it (stage 7), newest first, when there are any. */
typedef struct {
    const char*            query;             /* "" before the first search */
    int                    channels_status;   /* EVO_HUI_TWITCH_LOADING / READY / ERROR */
    const char*            channels_error;
    int                    channel_count;
    evo_hui_twitch_channel_t channels[EVO_HUI_TWITCH_SEARCH_CHANNELS];
    int                    categories_status;
    const char*            categories_error;
    int                    category_count;
    evo_hui_twitch_category_t categories[EVO_HUI_TWITCH_SEARCH_CATEGORIES];
    int                    focus_row;         /* EVO_HUI_TWITCH_SEARCH_*, kept while the rail is open */
    int                    channel_col;
    int                    category_col;
    int                    rail_focused;      /* 1 while the rail has the cursor */

    int                    recent_count;      /* the recent searches, shown with no query */
    const char*            recents[EVO_HUI_TWITCH_SEARCH_RECENTS];
    int                    recent_cursor;     /* the one under the cursor, on SEARCH_RECENT */
} evo_hui_twitch_search_params_t;

void evo_hui_update_twitch_search(const evo_hui_twitch_search_params_t* params);
int  evo_hui_render_twitch_search(int width, int height);

/* A channel page's tabs. */
#define EVO_HUI_TWITCH_CHANNEL_TAB_VIDEOS 0
#define EVO_HUI_TWITCH_CHANNEL_TAB_CLIPS  1
/* The most cards one tab lists: ten pages of 30. */
#define EVO_HUI_TWITCH_CHANNEL_CARDS  300
/* The live stream's tags (its language first), and the next scheduled streams. */
#define EVO_HUI_TWITCH_CHANNEL_TAGS      11
#define EVO_HUI_TWITCH_CHANNEL_SCHEDULE  3

/* One stream on a channel's schedule. */
typedef struct {
    const char*            when;              /* "In 5 h", "In 2 days" */
    const char*            title;             /* its title, or its category; never empty */
} evo_hui_twitch_schedule_t;

/* One channel: avatar, name, followers and description (with OFFLINE on the
 * followers' line while it isn't live), its live stream (preview, how long it
 * has been on, title, category, its tags, Watch live) when it is, then the
 * Videos and Clips tabs, the open tab's filter chips and its cards four to a
 * row. The next scheduled streams, when there are any, sit in a small
 * "Schedule" panel at the profile's right. The page scrolls to keep the
 * cursor in view. */
typedef struct {
    int                    status;            /* EVO_HUI_TWITCH_LOADING / READY / ERROR */
    const char*            error;
    int                    error_gone;        /* the channel doesn't exist: no retry, the way back */
    const char*            login;
    const char*            name;              /* display name */
    const char*            description;       /* "" when none */
    int                    followers;         /* -1 when not known */
    evo_hui_twitch_image_t avatar;            /* square, drawn as a circle */
    int                    live;
    const char*            title;             /* stream title, while live */
    const char*            game;              /* "" when none */
    int                    viewers;
    evo_hui_twitch_image_t preview;           /* 16:9 live preview, or none yet */
    int                    focus;             /* EVO_HUI_TWITCH_FOCUS_*; NONE while loading or failed */
    int                    rail_focused;
    int                    starting;          /* 1 while this channel's stream starts */

    /* Stage 7: the tabs. */
    int                    tab;               /* EVO_HUI_TWITCH_CHANNEL_TAB_VIDEOS / _CLIPS */
    int                    filter;            /* its chip: EVO_HUI_TWITCH_VIDEOS_* or _CLIPS_* */
    int                    filter_cursor;     /* the chip under the cursor, on FILTERS */
    int                    cards_status;      /* the open tab's list: LOADING / READY / ERROR */
    const char*            cards_error;
    int                    card_count;
    evo_hui_twitch_video_t cards[EVO_HUI_TWITCH_CHANNEL_CARDS];  /* detail: the category */
    int                    cursor;            /* the card under the cursor, kept while it is elsewhere */
    int                    more_loading;      /* 1 while the next page is on its way */
    const char*            starting_id;       /* a video or clip we asked for is starting, or NULL */

    /* Stage 7: more of the live stream, and the schedule. */
    const char*            uptime;            /* "Live for 2 h 14 min", while live; NULL or "" for none */
    int                    tag_count;
    const char*            tags[EVO_HUI_TWITCH_CHANNEL_TAGS];  /* drawn as chips, as many as fit */
    int                    schedule_count;
    evo_hui_twitch_schedule_t schedule[EVO_HUI_TWITCH_CHANNEL_SCHEDULE];  /* soonest first */
} evo_hui_twitch_channel_params_t;

void evo_hui_update_twitch_channel(const evo_hui_twitch_channel_params_t* params);
int  evo_hui_render_twitch_channel(int width, int height);

/* ---- Following (stage 5) -------------------------------------------- */

#define EVO_HUI_TWITCH_FOLLOWING_LIVE      100
#define EVO_HUI_TWITCH_FOLLOWING_CHANNELS  300
/* Cards to a row: core/ moves the cursor on the same grids the kit draws. */
#define EVO_HUI_TWITCH_FOLLOWING_LIVE_COLS    4
#define EVO_HUI_TWITCH_FOLLOWING_CHANNEL_COLS 7

/* Whether there is a login to show the follows of. */
#define EVO_HUI_TWITCH_FOLLOWING_CHECKING   0   /* the saved login isn't read yet: skeletons */
#define EVO_HUI_TWITCH_FOLLOWING_LOGGED_OUT 1   /* "Log in to see ...", Cross logs in */
#define EVO_HUI_TWITCH_FOLLOWING_LOGGED_IN  2

/* Following: "Live now", the followed channels that are live as a grid of
 * stream cards four to a row (like a category), then "Offline", the others
 * as round avatars with their names, seven to a row. One cursor runs through
 * both: live[0 .. live_count-1], then channels[0 .. channel_count-1]. The kit
 * scrolls the page to keep the cursor's row in view. */
typedef struct {
    int                    login_state;       /* EVO_HUI_TWITCH_FOLLOWING_* */
    int                    live_status;       /* EVO_HUI_TWITCH_LOADING / READY / ERROR */
    const char*            live_error;
    int                    live_count;
    evo_hui_twitch_stream_t live[EVO_HUI_TWITCH_FOLLOWING_LIVE];
    int                    channels_status;
    const char*            channels_error;
    int                    channel_count;     /* the offline ones: not in live[] */
    evo_hui_twitch_channel_t channels[EVO_HUI_TWITCH_FOLLOWING_CHANNELS];
    int                    cursor;            /* kept while the rail is open */
    int                    rail_focused;      /* 1 while the rail has the cursor: no ring, the page stays */
    const char*            starting_login;    /* a stream we asked for is starting, or NULL */
    int                    scope_missing;     /* 1: the login lacks the follows scope; the errors say to
                                                 log in again from Settings, Triangle can't help, and
                                                 Cross on the failed page opens Settings */
} evo_hui_twitch_following_params_t;

void evo_hui_update_twitch_following(const evo_hui_twitch_following_params_t* params);
int  evo_hui_render_twitch_following(int width, int height);

/* ---- logging in (stage 5) ------------------------------------------ */

/* What the login screen shows. */
#define EVO_HUI_TWITCH_LOGIN_STARTING 0   /* asking Twitch for a code: a spinner */
#define EVO_HUI_TWITCH_LOGIN_WAITING  1   /* the code and the QR, until the user says yes */
#define EVO_HUI_TWITCH_LOGIN_DONE     2   /* "Logged in as <name>" */
#define EVO_HUI_TWITCH_LOGIN_ERROR    3   /* error says why; Cross tries again */
#define EVO_HUI_TWITCH_LOGIN_CHECKING 4   /* the saved login isn't read yet: no code asked for */

/* The login screen, full screen with no rail: the steps and the code on the
 * left (twitch.tv/activate, then the code in large letters), the QR on a
 * white tile on the right. The QR is copied in the update, like the strings. */
typedef struct {
    int                  state;          /* EVO_HUI_TWITCH_LOGIN_* */
    const char*          code;           /* plain, "ABCDEFGH", while waiting (the kit draws it in two groups of four) */
    int                  seconds_left;   /* until the code changes; < 0 when not known */
    const unsigned char* qr;             /* qr_size * qr_size modules, row by row, non-zero = dark */
    int                  qr_size;        /* 0: no QR (the steps alone; one shown before goes) */
    const char*          name;           /* display name, when done */
    const char*          login;          /* shown instead when the fonts can't draw the name */
    const char*          error;          /* a plain sentence, when it failed */
} evo_hui_twitch_login_params_t;

void evo_hui_update_twitch_login(const evo_hui_twitch_login_params_t* params);
int  evo_hui_render_twitch_login(int width, int height);

/* ---- chat in the player (stage 4) ---------------------------------- */

typedef struct {
    int                    emote;   /* 1: a picture; 0: text */
    const char*            text;    /* the text, or the emote's name (drawn while its picture loads) */
    evo_hui_twitch_image_t pic;     /* the emote picture (56 px; Twitch's, 7TV's, BetterTTV's or FFZ's) */
} evo_hui_twitch_chat_segment_t;

#define EVO_HUI_TWITCH_CHAT_MESSAGE 0
#define EVO_HUI_TWITCH_CHAT_ACTION  1  /* "/me": the text takes the name's colour */
#define EVO_HUI_TWITCH_CHAT_NOTICE  2  /* sub, raid ...: system_text, then the segments if any */
#define EVO_HUI_TWITCH_CHAT_STATUS  3  /* the app's own line: system_text only */

typedef struct {
    unsigned long long                    serial;  /* the same for a line as long as it lives */
    int                                   kind;
    const char*                           name;    /* display name; "" for status lines */
    const char*                           login;   /* shown instead when the fonts can't draw the name */
    unsigned int                          color;   /* the name's colour, 0xRRGGBB */
    const char*                           system_text;
    const evo_hui_twitch_image_t*         badges;  /* 36 px pictures, rgba NULL while loading */
    int                                   badge_count;
    const evo_hui_twitch_chat_segment_t*  segments;
    int                                   segment_count;
    const char*                           reply_to;     /* a reply: who it answers, by display name
                                                           ("Replying to @name" above it); NULL or ""
                                                           otherwise */
    const char*                           reply_login;  /* shown instead when the fonts can't draw it */
    int                                   mention;      /* 1: it mentions the logged-in user: on a tint */
    int                                   selected;     /* 1: the line L1 will answer, drawn with a bar */
} evo_hui_twitch_chat_line_t;

#define EVO_HUI_TWITCH_CHAT_SIDE    0
#define EVO_HUI_TWITCH_CHAT_OVERLAY 1

/* Chat's text size (Settings > Twitch). 0, what a zeroed params struct has,
 * is the usual one. */
#define EVO_HUI_TWITCH_CHAT_TEXT_MEDIUM 0
#define EVO_HUI_TWITCH_CHAT_TEXT_SMALL  1
#define EVO_HUI_TWITCH_CHAT_TEXT_LARGE  2

/* The video's rect (video_x .. video_h) is where the player's blit really
 * puts the picture, in the kit's 1920 x 1080 canvas units: in side mode
 * inside the left EVO_CHAT_VIDEO_W columns (evo_twitch_chat_layout.h), in
 * overlay mode anywhere on the screen. Pass the rect the blit uses, not the
 * one it was asked for. The player never clears the frame, so the kit paints
 * black, every frame, over what the picture leaves uncovered of the area it
 * draws in (side mode: the whole left column; overlay mode: the overlay's
 * box), or old pixels would show through there. video_w <= 0 means unknown:
 * side mode then takes a 16:9 picture fitted in the column, overlay mode a
 * picture that fills the screen. */
typedef struct {
    int                                mode;       /* EVO_HUI_TWITCH_CHAT_SIDE or _OVERLAY */
    float                              video_x, video_y, video_w, video_h;
    const char*                        channel;    /* display name, for the side panel's header */
    const evo_hui_twitch_chat_line_t*  lines;      /* oldest first; the newest that fit are drawn */
    int                                line_count;
    int                                text_size;  /* EVO_HUI_TWITCH_CHAT_TEXT_* */
    int                                paused;     /* 1: a line is picked to reply to and chat holds
                                                      still; the side panel's header says so */
} evo_hui_twitch_chat_params_t;

void evo_hui_twitch_chat_update(const evo_hui_twitch_chat_params_t* p);
/* Draws over the frame being built, the video and the OSD included. It can
 * come before or after the OSD's render in the same frame: either way the
 * other kit screen keeps charge of the frame. Side mode paints its panel and
 * the bars around the video opaque on every call, because the player does
 * not clear the frame between video frames. */
int  evo_hui_render_twitch_chat(int width, int height);

/* ---- a clip made in the player (stage 7) --------------------------- */

/* The card that says a clip was made: the clip's page as a QR on a white
 * tile, "Scan to see your clip" and "It's ready in a few seconds" beside it,
 * Circle's "Close" and a bar that runs down until the card closes. It sits at
 * the bottom left of the picture, above where the OSD puts its keys, over the
 * video (and over the OSD when that is up). The QR is copied in the update.
 * No cursor. */
typedef struct {
    int                  visible;    /* 1 while it shows; 0 fades it out */
    const unsigned char* qr;         /* qr_size * qr_size modules, row by row, non-zero = dark */
    int                  qr_size;    /* 0: no QR (the card isn't drawn) */
    float                time_left;  /* 1 .. 0: the share of its time still to go */
    /* Where the OSD goes (evo_hui_set_osd_area), in canvas units: the whole
     * screen, or the video's box beside chat. area_w <= 0: the whole screen. */
    float                area_x, area_y, area_w, area_h;
} evo_hui_twitch_clip_params_t;

void evo_hui_twitch_clip_update(const evo_hui_twitch_clip_params_t* p);
/* Draws over the frame being built, after the OSD and chat; an overlay, so
 * whichever screen has the frame keeps it. */
int  evo_hui_render_twitch_clip(int width, int height);

#ifdef __cplusplus
}
#endif

#endif /* EVO_HUI_TWITCH_H */
