/* engine_proto.h — host ⇄ web-engine process contract.
 *
 * This is the Zurf engine protocol (see ../Zurf/src/engine_proto.h): the
 * base ops/events below are byte-for-byte compatible with it, so the engine
 * in this directory can also drive Zurf, and Zurf's engines can be pointed at
 * the plasmoid. The extensions (ops >= 32, events >= 96) are what an embedded
 * chat client needs on top: native keys, file chooser, permissions, cursors,
 * clipboard sync, downloads, find, popups, HiDPI.
 *
 * The host spawns the engine with:
 *
 *   <engine> --zurf-control-fd=3 --zurf-shm=<path> --zurf-cache=<dir>
 *            [--zurf-url=<url>] [--zurf-w=<w>] [--zurf-h=<h>]
 *            [--zurf-frame-events] [--zurf-scale=<percent>]
 *
 * Two planes:
 *  - FRAME PLANE: a seqlock'd latest-value BGRA slot mmap'ed from <path>.
 *    Writer = engine, reader = host. With --zurf-frame-events the engine
 *    also sends ZURF_EV_FRAME after each publish so the host never polls.
 *  - CONTROL PLANE: fd 3, a SOCK_STREAM socketpair. Little-endian frames:
 *        u32 payload_len | u8 op | u8 pad[3] | payload
 *    Strings are raw UTF-8 bytes (not NUL-terminated) unless stated.
 */
#ifndef ZURF_ENGINE_PROTO_H
#define ZURF_ENGINE_PROTO_H

#include <stdint.h>

/* ---- frame plane ----
 *
 * Double-buffered seqlock: TWO pixel slots follow the header. The writer
 * fills the slot the current stable frame is NOT in, then publishes it by
 * bumping `seq` (odd = header mutation in flight; stable frames are even).
 * The stable slot index is (seq / 2) % 2. Pixel rows are tightly packed
 * (stride = width * 4). Width/height are in DEVICE pixels. */

typedef struct zurf_shm_header {
  uint32_t seq;
  uint32_t width;
  uint32_t height;
  uint32_t reserved;
} zurf_shm_header;

#define ZURF_SHM_MAX_W 3840
#define ZURF_SHM_MAX_H 2400
#define ZURF_SHM_SLOT_SIZE ((size_t)ZURF_SHM_MAX_W * ZURF_SHM_MAX_H * 4)
#define ZURF_SHM_SIZE (sizeof(zurf_shm_header) + 2 * ZURF_SHM_SLOT_SIZE)

/* ---- control plane ---- */

typedef struct zurf_msg_header {
  uint32_t len; /* payload bytes after this 8-byte header */
  uint8_t op;
  uint8_t pad[3];
} zurf_msg_header;

/* Modifier bits used by every input op. 1/2/4 are Zurf's; the rest are
 * extensions (pointer-button bits let the page see drags). */
enum {
  ZURF_MOD_SHIFT = 1,
  ZURF_MOD_CTRL = 2,
  ZURF_MOD_ALT = 4,
  ZURF_MOD_META = 8,
  ZURF_MOD_BUTTON_LEFT = 256,
  ZURF_MOD_BUTTON_MIDDLE = 512,
  ZURF_MOD_BUTTON_RIGHT = 1024,
};

/* Core → engine. Coordinates are LOGICAL pixels. Navigation/input ops target
 * the ACTIVE tab. */
enum {
  /* -- Zurf base -- */
  ZURF_OP_NAVIGATE = 1,     /* url */
  ZURF_OP_RELOAD = 2,       /* - */
  ZURF_OP_GO_BACK = 3,      /* - */
  ZURF_OP_GO_FORWARD = 4,   /* - */
  ZURF_OP_RESIZE = 5,       /* 2x i32 (w, h) logical */
  ZURF_OP_MOUSE_MOVE = 6,   /* 3x i32 (x, y, mods) */
  ZURF_OP_MOUSE_BUTTON = 7, /* 6x i32 (x, y, button 0/1/2=L/M/R, down, clicks, mods) */
  ZURF_OP_MOUSE_WHEEL = 8,  /* 4x i32 (x, y, dx, dy) — notches*120, +dy = up */
  ZURF_OP_KEY = 9,          /* 4x i32 (windows vk, down, mods, codepoint) */
  ZURF_OP_TEXT = 10,        /* utf-8 (typed as key events) */
  ZURF_OP_EVAL_JS = 11,     /* js source */
  ZURF_OP_SHUTDOWN = 12,    /* - */
  ZURF_OP_TAB_NEW = 13,     /* url */
  ZURF_OP_TAB_CLOSE = 14,   /* 1x i32 tab id */
  ZURF_OP_TAB_SWITCH = 15,  /* 1x i32 tab id */
  ZURF_OP_COPY = 16,        /* - ; answer via ZURF_EV_CLIPBOARD */
  ZURF_OP_EVAL_JS_RET = 17, /* i32 request id + js source */
  ZURF_OP_INSERT_TEXT = 18, /* utf-8, inserted at the caret, no key semantics */
  ZURF_OP_SET_VISIBLE = 19, /* 1x i32 bool */

  /* -- extensions -- */
  ZURF_OP_KEY_NATIVE = 32,          /* 4x i32 (xkb keysym, xkb keycode, down, mods) */
  ZURF_OP_FILE_CHOOSER_REPLY = 33,  /* i32 request id + '\n'-separated paths
                                     * (empty = cancelled) */
  ZURF_OP_PERMISSION_REPLY = 34,    /* 2x i32 (request id, allow) */
  ZURF_OP_FIND = 35,                /* i32 flags (ZURF_FIND_*) + utf-8 text;
                                     * empty text ends the search */
  ZURF_OP_CLIPBOARD_SET = 36,       /* host clipboard changed: clipboard blob */
  ZURF_OP_SET_SCALE = 37,           /* 1x i32 device-pixel ratio * 100 */
  ZURF_OP_SET_PREFS = 38,           /* "key=value\n" lines, see ZURF_PREF_* */
  ZURF_OP_DOWNLOAD_CANCEL = 39,     /* 1x i32 download id */
  ZURF_OP_FOCUS = 40,               /* 1x i32 bool */
  ZURF_OP_MOUSE_LEAVE = 41,         /* - */
  ZURF_OP_STOP = 42,                /* - stop loading */
  ZURF_OP_SCROLL = 43,              /* 5x i32 (x, y, dx*100, dy*100, precise);
                                     * Qt conventions: +dy = scroll up;
                                     * precise → pixels, else angle units
                                     * (120 per wheel notch) */
  ZURF_OP_EDIT = 44,                /* utf-8 WebKit editing command name
                                     * ("Copy", "Cut", "Paste", "SelectAll", ...) */
  ZURF_OP_RELOAD_BYPASS = 45,       /* - reload ignoring cache */
};

enum {
  ZURF_FIND_BACKWARDS = 1,
  ZURF_FIND_CASE_SENSITIVE = 2,
};

/* Engine → core. */
enum {
  /* -- Zurf base -- */
  ZURF_EV_TITLE = 64,      /* i32 tab id + utf-8 */
  ZURF_EV_URL = 65,        /* i32 tab id + utf-8 */
  ZURF_EV_READY = 66,      /* - */
  ZURF_EV_TAB = 67,        /* i32 — tab created (now active) */
  ZURF_EV_TAB_CLOSED = 68, /* i32 */
  ZURF_EV_CLIPBOARD = 69,  /* utf-8 selection text (answer to ZURF_OP_COPY) */
  ZURF_EV_JS_RESULT = 70,  /* i32 request id + utf-8 ("" on error) */

  /* -- extensions -- */
  ZURF_EV_FRAME = 96,            /* new frame published (--zurf-frame-events):
                                  * 4x i32 (x, y, w, h) device-pixel bounding box
                                  * of what changed since the previous frame;
                                  * empty payload = whole frame. Both slots
                                  * always hold complete frames. */
  ZURF_EV_CURSOR = 97,           /* utf-8 CSS cursor name */
  ZURF_EV_FILE_CHOOSER = 98,     /* 2x i32 (request id, multiple) + utf-8
                                  * ','-separated accepted MIME types */
  ZURF_EV_PERMISSION = 99,       /* 2x i32 (request id, ZURF_PERM_*) + utf-8 origin */
  ZURF_EV_NOTIFICATION = 100,    /* utf-8 title '\0' body */
  ZURF_EV_LOAD_STATE = 101,      /* 4x i32 (progress 0..1000, loading,
                                  * can_go_back, can_go_forward) */
  ZURF_EV_LINK_HOVER = 102,      /* utf-8 url ("" = left the link) */
  ZURF_EV_CONTEXT_MENU = 103,    /* 3x i32 (x, y, ZURF_CTX_* flags) + utf-8
                                  * link '\0' media uri '\0' selected text */
  ZURF_EV_OPEN_EXTERNAL = 104,   /* utf-8 url the host should open outside */
  ZURF_EV_DOWNLOAD = 105,        /* 2x i32 (id, ZURF_DL_*) + 2x i64 (received,
                                  * total; total 0 = unknown) + utf-8 path */
  ZURF_EV_FIND_RESULT = 106,     /* 1x i32 match count (0 = not found) */
  ZURF_EV_CLIPBOARD_SET = 107,   /* page wrote the clipboard: clipboard blob */
  ZURF_EV_CRASHED = 108,         /* - web process gone (engine reloads it) */
};

enum {
  ZURF_PERM_NOTIFICATIONS = 1,
  ZURF_PERM_GEOLOCATION = 2,
  ZURF_PERM_MICROPHONE = 3,
  ZURF_PERM_CAMERA = 4,
  ZURF_PERM_MIC_AND_CAMERA = 5,
  ZURF_PERM_SCREEN = 6,
  ZURF_PERM_CLIPBOARD = 7,
  ZURF_PERM_OTHER = 8,
};

enum {
  ZURF_CTX_EDITABLE = 1,
  ZURF_CTX_SELECTION = 2,
  ZURF_CTX_LINK = 4,
  ZURF_CTX_IMAGE = 8,
  ZURF_CTX_MEDIA = 16,
};

enum {
  ZURF_DL_PROGRESS = 0,
  ZURF_DL_FINISHED = 1,
  ZURF_DL_FAILED = 2,
  ZURF_DL_CANCELLED = 3,
  ZURF_DL_STARTED = 4,
};

/* Clipboard blob: repeated { u32 mime_len | mime | u32 data_len | data }.
 * "text/plain;charset=utf-8" is the text entry; images travel as image/png. */

/* ZURF_OP_SET_PREFS keys (values are strings; booleans are "0"/"1"):
 *   user-agent         full UA string ("" = engine default)
 *   download-dir       absolute directory for downloads
 *   dark-mode          prefers-color-scheme: dark
 *   js-open-windows    allow window.open without a user gesture
 *   js-clipboard       allow navigator.clipboard / execCommand copy-paste
 *   media-gesture      media playback requires a user gesture
 *   spatial-nav        spatial navigation
 *   new-window-policy  "popup" (auth popups as tabs) | "external" (all to host)
 *   max-fps            frame rate cap (5..240; default 30, also --max-fps)
 *   transparent        1 = see-through page background (frames carry alpha)
 */

#define ZURF_MSG_MAX (1 << 24) /* 16 MiB: room for a pasted screenshot */

#endif /* ZURF_ENGINE_PROTO_H */
