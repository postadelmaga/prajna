/* chatai_webwpe.c — out-of-process WPE WebKit engine for the ChatAI plasmoid.
 *
 * Speaks the Zurf engine protocol (engine_proto.h) on fd 3 and publishes
 * frames into a double-buffered BGRA shm slot. Derived from Zurf's
 * engine_wpe_main.c, ported from libwpe/wpebackend-fdo to the WPE Platform
 * API (WPEWebKit >= 2.48), which is what current distributions ship.
 *
 * Rendering is software-only: our WPEDisplay advertises no DRM device and no
 * EGL display, so WebKit renders with Skia on the CPU into WPEBufferSHM
 * buffers that ZView::render_buffer copies into the shm slot. No GPU context
 * is created anywhere — neither here nor in plasmashell.
 *
 * Everything runs on the GLib main loop: the control fd is a GSource and
 * WebKit callbacks fire on the same thread, so no locking is needed.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib.h>
#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>

#include "clear_background_js.h"
#include "engine_proto.h"

#define MAX_TABS 16
#define MAX_PENDING 64

/* ------------------------------------------------------------------------ */
/* global state                                                             */

typedef struct tab {
  int id;
  int opener_id;
  WebKitWebView *view;
  WPEView *wview;
} tab_t;

typedef struct pending {
  int id;
  GObject *obj; /* WebKitFileChooserRequest / WebKitPermissionRequest */
} pending_t;

typedef struct download {
  int id;
  WebKitDownload *dl;
  char *path;
  gint64 last_report_us;
  int failed;
} download_t;

static zurf_shm_header *g_shm;
static uint8_t *g_shm_pixels;
static int g_ctl = 3;
static int g_width = 1000, g_height = 700; /* logical */
static double g_scale = 1.0;
static int g_frame_events;
/* Frame pacing. Every frame costs WebKit a full composite + readback in
 * software, so a panel widget defaults to 30 fps (--max-fps). */
static gint64 g_frame_interval_us = 1000000 / 30;
static int g_visible = 1;
static int g_focused;
static GMainLoop *g_loop;

static WPEDisplay *g_display;
static WebKitNetworkSession *g_session;
static WebKitSettings *g_settings;
static WebKitWebContext *g_context;
static tab_t *g_tabs[MAX_TABS];
static tab_t *g_active;
static int g_next_tab_id = 1;

static pending_t g_pending[MAX_PENDING];
static int g_next_req_id = 1;

static GPtrArray *g_downloads;
static int g_next_dl_id = 1;
static char *g_download_dir;
static int g_popups_as_tabs = 1;

static double g_ptr_x, g_ptr_y;
static int g_ptr_inside;
static guint g_press_count;

static int g_clip_from_host; /* guard: don't echo host clipboard back */

static char *g_find_text;

/* ------------------------------------------------------------------------ */
/* control-plane send                                                       */

static int write_all(int fd, const void *buf, size_t n) {
  const uint8_t *p = buf;
  while (n > 0) {
    ssize_t r = write(fd, p, n);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    p += r;
    n -= (size_t)r;
  }
  return 0;
}

static void send_event(uint8_t op, const void *payload, uint32_t len) {
  zurf_msg_header h = {.len = len, .op = op};
  if (write_all(g_ctl, &h, sizeof(h)) == 0 && len > 0)
    (void)!write_all(g_ctl, payload, len);
}

/* Build a payload from n i32 values followed by raw bytes. */
static void send_ints_bytes(uint8_t op, const int32_t *ints, int n, const void *bytes,
                            size_t blen) {
  size_t len = (size_t)n * 4 + blen;
  uint8_t *buf = g_malloc(len ? len : 1);
  if (n)
    memcpy(buf, ints, (size_t)n * 4);
  if (blen)
    memcpy(buf + (size_t)n * 4, bytes, blen);
  send_event(op, buf, (uint32_t)len);
  g_free(buf);
}

static void send_tab_str(uint8_t op, int id, const char *s) {
  int32_t v = id;
  send_ints_bytes(op, &v, 1, s, s ? strlen(s) : 0);
}

static void send_str(uint8_t op, const char *s) {
  send_event(op, s, s ? (uint32_t)strlen(s) : 0);
}

/* ------------------------------------------------------------------------ */
/* frame plane                                                              */

typedef struct rect {
  int x, y, w, h;
} rect_t;

static int rect_empty(rect_t r) { return r.w <= 0 || r.h <= 0; }

static rect_t rect_union(rect_t a, rect_t b) {
  if (rect_empty(a))
    return b;
  if (rect_empty(b))
    return a;
  int x0 = MIN(a.x, b.x), y0 = MIN(a.y, b.y);
  int x1 = MAX(a.x + a.w, b.x + b.w), y1 = MAX(a.y + a.h, b.y + b.h);
  return (rect_t){x0, y0, x1 - x0, y1 - y0};
}

static rect_t rect_clip(rect_t r, int w, int h) {
  int x0 = MAX(r.x, 0), y0 = MAX(r.y, 0);
  int x1 = MIN(r.x + r.w, w), y1 = MIN(r.y + r.h, h);
  return (rect_t){x0, y0, MAX(0, x1 - x0), MAX(0, y1 - y0)};
}

/* Damage bookkeeping for the two slots. A slot is always kept as a COMPLETE
 * frame, so the host can copy it whole at any time; but bringing the stale
 * slot (two frames old) up to date only needs the damage of the last two
 * frames, which while typing or streaming text is a few percent of it. */
static int g_slot_w[2], g_slot_h[2];
static rect_t g_prev_damage;
static int g_force_full = 1;

static void publish_pixels(const uint8_t *src, int w, int h, int stride, rect_t damage) {
  if (!g_shm || w <= 0 || h <= 0)
    return;
  if (w > ZURF_SHM_MAX_W)
    w = ZURF_SHM_MAX_W;
  if (h > ZURF_SHM_MAX_H)
    h = ZURF_SHM_MAX_H;
  rect_t full = {0, 0, w, h};
  damage = g_force_full ? full : rect_clip(damage, w, h);

  uint32_t seq = __atomic_load_n(&g_shm->seq, __ATOMIC_RELAXED);
  int slot = (int)(((seq / 2) + 1) % 2);
  uint8_t *dst = g_shm_pixels + (size_t)slot * ZURF_SHM_SLOT_SIZE;
  size_t row = (size_t)w * 4;

  rect_t copy = (g_slot_w[slot] == w && g_slot_h[slot] == h) ? rect_union(damage, g_prev_damage) : full;
  copy = rect_clip(copy, w, h);
  if (copy.w == w && (size_t)stride == row) {
    memcpy(dst + row * (size_t)copy.y, src + row * (size_t)copy.y, row * (size_t)copy.h);
  } else {
    size_t off = (size_t)copy.x * 4, len = (size_t)copy.w * 4;
    for (int y = copy.y; y < copy.y + copy.h; y++)
      memcpy(dst + row * (size_t)y + off, src + (size_t)stride * (size_t)y + off, len);
  }
  g_slot_w[slot] = w;
  g_slot_h[slot] = h;
  g_prev_damage = damage;
  g_force_full = 0;

  __atomic_store_n(&g_shm->seq, seq | 1u, __ATOMIC_RELEASE);
  __atomic_thread_fence(__ATOMIC_RELEASE);
  g_shm->width = (uint32_t)w;
  g_shm->height = (uint32_t)h;
  __atomic_store_n(&g_shm->seq, (seq | 1u) + 1u, __ATOMIC_RELEASE);
  if (g_frame_events) {
    int32_t d[4] = {damage.x, damage.y, damage.w, damage.h};
    send_event(ZURF_EV_FRAME, d, sizeof(d));
  }
}

/* Bounding box of WebKit's damage (device pixels); none = whole frame. */
static rect_t damage_bbox(const WPERectangle *damage, guint n, int w, int h) {
  if (!damage || n == 0)
    return (rect_t){0, 0, w, h};
  rect_t r = {0, 0, 0, 0};
  for (guint i = 0; i < n; i++)
    r = rect_union(r, (rect_t){damage[i].x, damage[i].y, damage[i].width, damage[i].height});
  return r;
}

static void publish_buffer(WPEBuffer *buffer, const WPERectangle *damage, guint n_damage) {
  int w = wpe_buffer_get_width(buffer);
  int h = wpe_buffer_get_height(buffer);
  rect_t dmg = damage_bbox(damage, n_damage, w, h);
  if (WPE_IS_BUFFER_SHM(buffer)) {
    GBytes *data = wpe_buffer_shm_get_data(WPE_BUFFER_SHM(buffer));
    guint stride = wpe_buffer_shm_get_stride(WPE_BUFFER_SHM(buffer));
    gsize size = 0;
    const uint8_t *px = g_bytes_get_data(data, &size);
    if (px && size >= (gsize)stride * (gsize)h)
      publish_pixels(px, w, h, (int)stride, dmg);
    return;
  }
  /* Not expected (we advertise no DRM device), but stay correct. */
  GError *err = NULL;
  GBytes *px = wpe_buffer_import_to_pixels(buffer, &err);
  if (px) {
    gsize size = 0;
    const uint8_t *p = g_bytes_get_data(px, &size);
    if (size >= (gsize)w * 4 * (gsize)h)
      publish_pixels(p, w, h, w * 4, dmg);
  } else if (err) {
    g_warning("import_to_pixels: %s", err->message);
    g_error_free(err);
  }
}

/* ------------------------------------------------------------------------ */
/* WPE Platform glue: ZScreen, ZToplevel, ZView, ZClipboard, ZDisplay       */

/* --- screen --- */
#define Z_TYPE_SCREEN (z_screen_get_type())
G_DECLARE_FINAL_TYPE(ZScreen, z_screen, Z, SCREEN, WPEScreen)
struct _ZScreen {
  WPEScreen parent;
};
G_DEFINE_FINAL_TYPE(ZScreen, z_screen, WPE_TYPE_SCREEN)
static void z_screen_init(ZScreen *s) { (void)s; }
static void z_screen_class_init(ZScreenClass *k) { (void)k; }

/* --- toplevel --- */
#define Z_TYPE_TOPLEVEL (z_toplevel_get_type())
G_DECLARE_FINAL_TYPE(ZToplevel, z_toplevel, Z, TOPLEVEL, WPEToplevel)
struct _ZToplevel {
  WPEToplevel parent;
};
G_DEFINE_FINAL_TYPE(ZToplevel, z_toplevel, WPE_TYPE_TOPLEVEL)

static WPEScreen *z_display_screen(void);

static void z_toplevel_set_title(WPEToplevel *t, const char *title) {
  (void)t;
  (void)title;
}
static WPEScreen *z_toplevel_get_screen(WPEToplevel *t) {
  (void)t;
  return z_display_screen();
}
static gboolean z_toplevel_resize(WPEToplevel *t, int w, int h) {
  wpe_toplevel_resized(t, w, h);
  return TRUE;
}
static gboolean z_toplevel_false_b(WPEToplevel *t, gboolean b) {
  (void)t;
  (void)b;
  return FALSE;
}
static gboolean z_toplevel_false(WPEToplevel *t) {
  (void)t;
  return FALSE;
}
static void z_toplevel_init(ZToplevel *t) { (void)t; }
static void z_toplevel_class_init(ZToplevelClass *k) {
  WPEToplevelClass *tk = WPE_TOPLEVEL_CLASS(k);
  tk->set_title = z_toplevel_set_title;
  tk->get_screen = z_toplevel_get_screen;
  tk->resize = z_toplevel_resize;
  tk->set_fullscreen = z_toplevel_false_b;
  tk->set_maximized = z_toplevel_false_b;
  tk->set_minimized = z_toplevel_false;
}

/* --- view: the frame sink --- */
#define Z_TYPE_VIEW (z_view_get_type())
G_DECLARE_FINAL_TYPE(ZView, z_view, Z, VIEW, WPEView)
struct _ZView {
  WPEView parent;
  WPEBuffer *pending;   /* rendered, waiting for the vsync tick */
  guint tick_source;
  gint64 last_tick_us;
};
G_DEFINE_FINAL_TYPE(ZView, z_view, WPE_TYPE_VIEW)

static tab_t *tab_of_wview(WPEView *v);

/* Emulated vsync: WebKit renders the next frame only after buffer_rendered,
 * so pacing here caps the engine at ~60 fps and lets it idle when nothing
 * changes. The buffer is released right away — its pixels already live in
 * the shm slot. */
static gboolean z_view_tick(gpointer data) {
  ZView *zv = data;
  zv->tick_source = 0;
  zv->last_tick_us = g_get_monotonic_time();
  WPEBuffer *b = zv->pending;
  zv->pending = NULL;
  if (b) {
    wpe_view_buffer_rendered(WPE_VIEW(zv), b);
    wpe_view_buffer_released(WPE_VIEW(zv), b);
    g_object_unref(b);
  }
  return G_SOURCE_REMOVE;
}

static gboolean z_view_render_buffer(WPEView *view, WPEBuffer *buffer,
                                     const WPERectangle *damage, guint n_damage,
                                     GError **error) {
  (void)error;
  ZView *zv = Z_VIEW(view);
  tab_t *tab = tab_of_wview(view);
  if (tab && tab == g_active)
    publish_buffer(buffer, damage, n_damage);

  if (zv->pending) { /* shouldn't happen, but never leak a buffer */
    wpe_view_buffer_rendered(view, zv->pending);
    wpe_view_buffer_released(view, zv->pending);
    g_object_unref(zv->pending);
  }
  zv->pending = g_object_ref(buffer);
  if (!zv->tick_source) {
    gint64 elapsed = g_get_monotonic_time() - zv->last_tick_us;
    guint delay_ms = elapsed >= g_frame_interval_us ? 0 : (guint)((g_frame_interval_us - elapsed) / 1000);
    zv->tick_source = g_timeout_add(delay_ms, z_view_tick, zv);
  }
  return TRUE;
}

static void z_view_set_cursor_from_name(WPEView *view, const char *name) {
  tab_t *tab = tab_of_wview(view);
  if (tab && tab == g_active)
    send_str(ZURF_EV_CURSOR, name ? name : "default");
}

static void z_view_set_cursor_from_bytes(WPEView *view, GBytes *bytes, guint w, guint h,
                                         guint stride, guint hx, guint hy) {
  (void)bytes; (void)w; (void)h; (void)stride; (void)hx; (void)hy;
  tab_t *tab = tab_of_wview(view);
  if (tab && tab == g_active)
    send_str(ZURF_EV_CURSOR, "default"); /* custom bitmap cursors: not forwarded */
}

static gboolean z_view_can_be_mapped(WPEView *view) {
  (void)view;
  return TRUE;
}

static void z_view_dispose(GObject *obj) {
  ZView *zv = Z_VIEW(obj);
  if (zv->tick_source) {
    g_source_remove(zv->tick_source);
    zv->tick_source = 0;
  }
  g_clear_object(&zv->pending);
  G_OBJECT_CLASS(z_view_parent_class)->dispose(obj);
}

static void z_view_init(ZView *v) { (void)v; }
static void z_view_class_init(ZViewClass *k) {
  G_OBJECT_CLASS(k)->dispose = z_view_dispose;
  WPEViewClass *vk = WPE_VIEW_CLASS(k);
  vk->render_buffer = z_view_render_buffer;
  vk->set_cursor_from_name = z_view_set_cursor_from_name;
  vk->set_cursor_from_bytes = z_view_set_cursor_from_bytes;
  vk->can_be_mapped = z_view_can_be_mapped;
}

/* --- clipboard: in-memory, mirrored with the host --- */
#define Z_TYPE_CLIPBOARD (z_clipboard_get_type())
G_DECLARE_FINAL_TYPE(ZClipboard, z_clipboard, Z, CLIPBOARD, WPEClipboard)
struct _ZClipboard {
  WPEClipboard parent;
};
G_DEFINE_FINAL_TYPE(ZClipboard, z_clipboard, WPE_TYPE_CLIPBOARD)

static void blob_append(GByteArray *blob, const char *mime, const void *data, gsize len) {
  uint32_t ml = (uint32_t)strlen(mime), dl = (uint32_t)len;
  g_byte_array_append(blob, (const guint8 *)&ml, 4);
  g_byte_array_append(blob, (const guint8 *)mime, ml);
  g_byte_array_append(blob, (const guint8 *)&dl, 4);
  if (len)
    g_byte_array_append(blob, data, dl);
}

static int is_text_mime(const char *m) {
  return g_str_has_prefix(m, "text/plain") || strcmp(m, "UTF8_STRING") == 0 ||
         strcmp(m, "STRING") == 0 || strcmp(m, "TEXT") == 0;
}

static void z_clipboard_changed(WPEClipboard *cb, GPtrArray *formats, gboolean is_local,
                                WPEClipboardContent *content) {
  WPE_CLIPBOARD_CLASS(z_clipboard_parent_class)->changed(cb, formats, is_local, content);
  if (g_clip_from_host || !content || !formats)
    return;
  GByteArray *blob = g_byte_array_new();
  int have_text = 0;
  for (guint i = 0; i < formats->len; i++) {
    const char *mime = g_ptr_array_index(formats, i);
    if (!mime)
      continue;
    if (is_text_mime(mime)) {
      if (have_text)
        continue;
      const char *t = wpe_clipboard_content_get_text(content);
      if (t) {
        blob_append(blob, "text/plain;charset=utf-8", t, strlen(t));
        have_text = 1;
      }
    } else {
      GBytes *b = wpe_clipboard_content_get_bytes(content, mime);
      if (b) {
        gsize n = 0;
        const void *d = g_bytes_get_data(b, &n);
        blob_append(blob, mime, d, n);
      }
    }
  }
  if (blob->len)
    send_event(ZURF_EV_CLIPBOARD_SET, blob->data, blob->len);
  g_byte_array_unref(blob);
}

static void z_clipboard_init(ZClipboard *c) { (void)c; }
static void z_clipboard_class_init(ZClipboardClass *k) {
  WPE_CLIPBOARD_CLASS(k)->changed = z_clipboard_changed;
}

/* --- display --- */
#define Z_TYPE_DISPLAY (z_display_get_type())
G_DECLARE_FINAL_TYPE(ZDisplay, z_display, Z, DISPLAY, WPEDisplay)
struct _ZDisplay {
  WPEDisplay parent;
  WPEKeymap *keymap;
  WPEClipboard *clipboard;
  WPEScreen *screen;
};
G_DEFINE_FINAL_TYPE(ZDisplay, z_display, WPE_TYPE_DISPLAY)

static WPEScreen *z_display_screen(void) {
  return g_display ? Z_DISPLAY(g_display)->screen : NULL;
}

static gboolean z_display_connect(WPEDisplay *d, GError **error) {
  (void)error;
  ZDisplay *zd = Z_DISPLAY(d);
  zd->keymap = wpe_keymap_xkb_new();
  zd->clipboard = g_object_new(Z_TYPE_CLIPBOARD, "display", d, NULL);
  zd->screen = g_object_new(Z_TYPE_SCREEN, "id", 1, NULL);
  wpe_screen_set_position(zd->screen, 0, 0);
  wpe_screen_set_size(zd->screen, 2560, 1600);
  wpe_screen_set_physical_size(zd->screen, 600, 340);
  wpe_screen_set_scale(zd->screen, g_scale);
  wpe_screen_set_refresh_rate(zd->screen, 60000);
  return TRUE;
}

static WPEView *z_display_create_view(WPEDisplay *d) {
  return g_object_new(Z_TYPE_VIEW, "display", d, NULL);
}

static WPEToplevel *z_display_create_toplevel(WPEDisplay *d, guint max_views) {
  WPEToplevel *t = g_object_new(Z_TYPE_TOPLEVEL, "display", d, "max-views", max_views, NULL);
  wpe_toplevel_resized(t, g_width, g_height);
  wpe_toplevel_scale_changed(t, g_scale);
  wpe_toplevel_state_changed(t, WPE_TOPLEVEL_STATE_ACTIVE);
  return t;
}

static gpointer z_display_get_egl_display(WPEDisplay *d, GError **error) {
  (void)d;
  g_set_error_literal(error, WPE_DISPLAY_ERROR, WPE_DISPLAY_ERROR_NOT_SUPPORTED,
                      "software rendering only");
  return NULL;
}

static WPEKeymap *z_display_get_keymap(WPEDisplay *d) { return Z_DISPLAY(d)->keymap; }
static WPEClipboard *z_display_get_clipboard(WPEDisplay *d) { return Z_DISPLAY(d)->clipboard; }
static WPEBufferFormats *z_display_get_formats(WPEDisplay *d) {
  (void)d;
  return NULL;
}
static guint z_display_get_n_screens(WPEDisplay *d) {
  (void)d;
  return 1;
}
static WPEScreen *z_display_get_screen(WPEDisplay *d, guint i) {
  return i == 0 ? Z_DISPLAY(d)->screen : NULL;
}
static WPEDRMDevice *z_display_get_drm_device(WPEDisplay *d) {
  (void)d;
  return NULL;
}

static void z_display_init(ZDisplay *d) { (void)d; }
static void z_display_class_init(ZDisplayClass *k) {
  WPEDisplayClass *dk = WPE_DISPLAY_CLASS(k);
  dk->connect = z_display_connect;
  dk->create_view = z_display_create_view;
  dk->create_toplevel = z_display_create_toplevel;
  dk->get_egl_display = z_display_get_egl_display;
  dk->get_keymap = z_display_get_keymap;
  dk->get_clipboard = z_display_get_clipboard;
  dk->get_preferred_buffer_formats = z_display_get_formats;
  dk->get_n_screens = z_display_get_n_screens;
  dk->get_screen = z_display_get_screen;
  dk->get_drm_device = z_display_get_drm_device;
}

/* ------------------------------------------------------------------------ */
/* pending requests (file chooser, permissions)                             */

static int pending_add(GObject *obj) {
  for (int i = 0; i < MAX_PENDING; i++) {
    if (!g_pending[i].obj) {
      g_pending[i].id = g_next_req_id++;
      g_pending[i].obj = g_object_ref(obj);
      return g_pending[i].id;
    }
  }
  return -1;
}

static GObject *pending_take(int id) {
  for (int i = 0; i < MAX_PENDING; i++) {
    if (g_pending[i].obj && g_pending[i].id == id) {
      GObject *o = g_pending[i].obj;
      g_pending[i].obj = NULL;
      return o;
    }
  }
  return NULL;
}

/* ------------------------------------------------------------------------ */
/* tabs                                                                     */

static tab_t *tab_of_wview(WPEView *v) {
  return v ? g_object_get_data(G_OBJECT(v), "chatai-tab") : NULL;
}

static tab_t *tab_find(int id) {
  for (int i = 0; i < MAX_TABS; i++)
    if (g_tabs[i] && g_tabs[i]->id == id)
      return g_tabs[i];
  return NULL;
}

static void send_load_state(tab_t *tab) {
  if (!tab || tab != g_active)
    return;
  int32_t v[4] = {
      (int32_t)(webkit_web_view_get_estimated_load_progress(tab->view) * 1000.0),
      webkit_web_view_is_loading(tab->view),
      webkit_web_view_can_go_back(tab->view),
      webkit_web_view_can_go_forward(tab->view),
  };
  send_event(ZURF_EV_LOAD_STATE, v, sizeof(v));
}

static void view_apply_geometry(tab_t *tab) {
  WPEToplevel *tl = wpe_view_get_toplevel(tab->wview);
  if (tl) {
    wpe_toplevel_resized(tl, g_width, g_height);
    wpe_toplevel_scale_changed(tl, g_scale);
  }
  wpe_view_resized(tab->wview, g_width, g_height);
}

static void tab_set_visible(tab_t *tab, int visible) {
  if (!tab)
    return;
  wpe_view_set_visible(tab->wview, visible);
  if (visible && g_focused)
    wpe_view_focus_in(tab->wview);
  else
    wpe_view_focus_out(tab->wview);
}

static void tab_activate(tab_t *tab) {
  if (g_active == tab)
    return;
  tab_set_visible(g_active, 0);
  g_active = tab;
  g_force_full = 1;
  if (tab) {
    tab_set_visible(tab, g_visible);
    send_tab_str(ZURF_EV_TITLE, tab->id, webkit_web_view_get_title(tab->view));
    send_tab_str(ZURF_EV_URL, tab->id, webkit_web_view_get_uri(tab->view));
    send_load_state(tab);
    int32_t id = tab->id;
    send_event(ZURF_EV_TAB, &id, 4); /* "this tab is now active" */
  }
}

static tab_t *tab_register(WebKitWebView *view, int opener_id);
static void tab_apply_transparency(tab_t *tab);
static int g_transparent; /* see tab_apply_transparency */

static void on_notify_title(GObject *o, GParamSpec *p, gpointer d) {
  (void)p;
  tab_t *tab = d;
  send_tab_str(ZURF_EV_TITLE, tab->id, webkit_web_view_get_title(WEBKIT_WEB_VIEW(o)));
}

static void on_notify_uri(GObject *o, GParamSpec *p, gpointer d) {
  (void)p;
  tab_t *tab = d;
  send_tab_str(ZURF_EV_URL, tab->id, webkit_web_view_get_uri(WEBKIT_WEB_VIEW(o)));
  send_load_state(tab);
}

static void on_notify_load(GObject *o, GParamSpec *p, gpointer d) {
  (void)o;
  (void)p;
  send_load_state(d);
}

static void on_mouse_target_changed(WebKitWebView *v, WebKitHitTestResult *hit, guint mods,
                                    gpointer d) {
  (void)v;
  (void)mods;
  tab_t *tab = d;
  if (tab != g_active)
    return;
  const char *uri = webkit_hit_test_result_context_is_link(hit)
                        ? webkit_hit_test_result_get_link_uri(hit)
                        : "";
  send_str(ZURF_EV_LINK_HOVER, uri);
}

static gboolean on_file_chooser(WebKitWebView *v, WebKitFileChooserRequest *req, gpointer d) {
  (void)v;
  (void)d;
  int id = pending_add(G_OBJECT(req));
  if (id < 0) {
    webkit_file_chooser_request_cancel(req);
    return TRUE;
  }
  GString *accept = g_string_new(NULL);
  const gchar *const *mimes = webkit_file_chooser_request_get_mime_types(req);
  for (int i = 0; mimes && mimes[i]; i++) {
    if (accept->len)
      g_string_append_c(accept, ',');
    g_string_append(accept, mimes[i]);
  }
  int32_t ints[2] = {id, webkit_file_chooser_request_get_select_multiple(req)};
  send_ints_bytes(ZURF_EV_FILE_CHOOSER, ints, 2, accept->str, accept->len);
  g_string_free(accept, TRUE);
  return TRUE;
}

static gboolean on_permission(WebKitWebView *v, WebKitPermissionRequest *req, gpointer d) {
  (void)d;
  int kind = ZURF_PERM_OTHER;
  if (WEBKIT_IS_DEVICE_INFO_PERMISSION_REQUEST(req)) {
    /* enumerateDevices(): harmless, needed before any getUserMedia UI. */
    webkit_permission_request_allow(req);
    return TRUE;
  } else if (WEBKIT_IS_NOTIFICATION_PERMISSION_REQUEST(req)) {
    kind = ZURF_PERM_NOTIFICATIONS;
  } else if (WEBKIT_IS_GEOLOCATION_PERMISSION_REQUEST(req)) {
    kind = ZURF_PERM_GEOLOCATION;
  } else if (WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST(req)) {
    WebKitUserMediaPermissionRequest *um = WEBKIT_USER_MEDIA_PERMISSION_REQUEST(req);
    int audio = webkit_user_media_permission_is_for_audio_device(um);
    int video = webkit_user_media_permission_is_for_video_device(um);
    if (webkit_user_media_permission_is_for_display_device(um))
      kind = ZURF_PERM_SCREEN;
    else if (audio && video)
      kind = ZURF_PERM_MIC_AND_CAMERA;
    else if (video)
      kind = ZURF_PERM_CAMERA;
    else
      kind = ZURF_PERM_MICROPHONE;
  } else if (WEBKIT_IS_WEBSITE_DATA_ACCESS_PERMISSION_REQUEST(req)) {
    /* Storage access for embedded logins (e.g. Google sign-in iframes). */
    webkit_permission_request_allow(req);
    return TRUE;
  }
  int id = pending_add(G_OBJECT(req));
  if (id < 0) {
    webkit_permission_request_deny(req);
    return TRUE;
  }
  int32_t ints[2] = {id, kind};
  const char *uri = webkit_web_view_get_uri(v);
  send_ints_bytes(ZURF_EV_PERMISSION, ints, 2, uri, uri ? strlen(uri) : 0);
  return TRUE;
}

static gboolean on_show_notification(WebKitWebView *v, WebKitNotification *n, gpointer d) {
  (void)v;
  (void)d;
  const char *title = webkit_notification_get_title(n);
  const char *body = webkit_notification_get_body(n);
  GByteArray *b = g_byte_array_new();
  if (title)
    g_byte_array_append(b, (const guint8 *)title, strlen(title));
  g_byte_array_append(b, (const guint8 *)"", 1);
  if (body)
    g_byte_array_append(b, (const guint8 *)body, strlen(body));
  send_event(ZURF_EV_NOTIFICATION, b->data, b->len);
  g_byte_array_unref(b);
  return TRUE;
}

static gboolean on_context_menu(WebKitWebView *v, WebKitContextMenu *menu,
                                WebKitHitTestResult *hit, gpointer d) {
  (void)v;
  (void)menu;
  tab_t *tab = d;
  if (tab != g_active)
    return TRUE;
  int32_t flags = 0;
  const char *link = "", *media = "";
  if (webkit_hit_test_result_context_is_editable(hit))
    flags |= ZURF_CTX_EDITABLE;
  if (webkit_hit_test_result_context_is_selection(hit))
    flags |= ZURF_CTX_SELECTION;
  if (webkit_hit_test_result_context_is_link(hit)) {
    flags |= ZURF_CTX_LINK;
    link = webkit_hit_test_result_get_link_uri(hit);
  }
  if (webkit_hit_test_result_context_is_image(hit)) {
    flags |= ZURF_CTX_IMAGE;
    media = webkit_hit_test_result_get_image_uri(hit);
  } else if (webkit_hit_test_result_context_is_media(hit)) {
    flags |= ZURF_CTX_MEDIA;
    media = webkit_hit_test_result_get_media_uri(hit);
  }
  GByteArray *b = g_byte_array_new();
  int32_t ints[3] = {(int32_t)g_ptr_x, (int32_t)g_ptr_y, flags};
  g_byte_array_append(b, (const guint8 *)ints, sizeof(ints));
  if (link)
    g_byte_array_append(b, (const guint8 *)link, strlen(link));
  g_byte_array_append(b, (const guint8 *)"", 1);
  if (media)
    g_byte_array_append(b, (const guint8 *)media, strlen(media));
  g_byte_array_append(b, (const guint8 *)"", 1);
  send_event(ZURF_EV_CONTEXT_MENU, b->data, b->len);
  g_byte_array_unref(b);
  return TRUE; /* no native menu in WPE: the host draws its own */
}

static int is_web_scheme(const char *uri) {
  return uri && (g_str_has_prefix(uri, "http:") || g_str_has_prefix(uri, "https:") ||
                 g_str_has_prefix(uri, "about:") || g_str_has_prefix(uri, "data:") ||
                 g_str_has_prefix(uri, "blob:") || g_str_has_prefix(uri, "file:") ||
                 g_str_has_prefix(uri, "javascript:"));
}

/* window.open / target=_blank. Plain link clicks go to the system browser;
 * script-opened windows (OAuth popups need window.opener) become a related
 * tab the host shows on top, closed again by the page's window.close(). */
static WebKitWebView *on_create(WebKitWebView *v, WebKitNavigationAction *nav, gpointer d) {
  tab_t *opener = d;
  WebKitURIRequest *rq = webkit_navigation_action_get_request(nav);
  const char *uri = rq ? webkit_uri_request_get_uri(rq) : NULL;
  WebKitNavigationType type = webkit_navigation_action_get_navigation_type(nav);

  int as_popup = g_popups_as_tabs && type != WEBKIT_NAVIGATION_TYPE_LINK_CLICKED;
  if (!as_popup) {
    if (uri && uri[0] && strcmp(uri, "about:blank") != 0)
      send_str(ZURF_EV_OPEN_EXTERNAL, uri);
    return NULL;
  }
  WebKitWebView *nv = g_object_new(WEBKIT_TYPE_WEB_VIEW,
                                   "related-view", v,
                                   "display", g_display,
                                   NULL);
  tab_t *tab = tab_register(nv, opener ? opener->id : 0);
  if (!tab) {
    g_object_unref(nv);
    return NULL;
  }
  return nv; /* WebKit loads the request into it */
}

static void tab_close(int id);

static void on_close(WebKitWebView *v, gpointer d) {
  (void)v;
  tab_t *tab = d;
  tab_close(tab->id);
}

static gboolean on_decide_policy(WebKitWebView *v, WebKitPolicyDecision *dec,
                                 WebKitPolicyDecisionType type, gpointer d) {
  (void)v;
  (void)d;
  if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
    WebKitNavigationAction *nav =
        webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(dec));
    WebKitURIRequest *rq = webkit_navigation_action_get_request(nav);
    const char *uri = rq ? webkit_uri_request_get_uri(rq) : NULL;
    if (uri && !is_web_scheme(uri)) {
      /* mailto:, tel:, apps' custom schemes → let the desktop handle them */
      send_str(ZURF_EV_OPEN_EXTERNAL, uri);
      webkit_policy_decision_ignore(dec);
      return TRUE;
    }
  } else if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
    WebKitResponsePolicyDecision *r = WEBKIT_RESPONSE_POLICY_DECISION(dec);
    if (!webkit_response_policy_decision_is_mime_type_supported(r) &&
        webkit_response_policy_decision_is_main_frame_main_resource(r)) {
      webkit_policy_decision_download(dec);
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean on_script_dialog(WebKitWebView *v, WebKitScriptDialog *dlg, gpointer d) {
  (void)v;
  (void)d;
  switch (webkit_script_dialog_get_dialog_type(dlg)) {
  case WEBKIT_SCRIPT_DIALOG_CONFIRM:
  case WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM:
    webkit_script_dialog_confirm_set_confirmed(dlg, TRUE);
    break;
  case WEBKIT_SCRIPT_DIALOG_PROMPT:
    webkit_script_dialog_prompt_set_text(dlg, webkit_script_dialog_prompt_get_default_text(dlg));
    break;
  default: { /* alert(): surface it as a notification rather than drop it */
    const char *msg = webkit_script_dialog_get_message(dlg);
    GByteArray *b = g_byte_array_new();
    const char *title = webkit_web_view_get_title(v);
    if (title)
      g_byte_array_append(b, (const guint8 *)title, strlen(title));
    g_byte_array_append(b, (const guint8 *)"", 1);
    if (msg)
      g_byte_array_append(b, (const guint8 *)msg, strlen(msg));
    send_event(ZURF_EV_NOTIFICATION, b->data, b->len);
    g_byte_array_unref(b);
    break;
  }
  }
  return TRUE;
}

static void on_web_process_terminated(WebKitWebView *v, WebKitWebProcessTerminationReason r,
                                      gpointer d) {
  (void)d;
  g_warning("web process terminated (reason %d), reloading", (int)r);
  send_event(ZURF_EV_CRASHED, NULL, 0);
  webkit_web_view_reload(v);
}

static void on_found_text(WebKitFindController *fc, guint count, gpointer d) {
  (void)fc;
  (void)d;
  int32_t c = (int32_t)count;
  send_event(ZURF_EV_FIND_RESULT, &c, 4);
}

static void on_failed_to_find(WebKitFindController *fc, gpointer d) {
  (void)fc;
  (void)d;
  int32_t c = 0;
  send_event(ZURF_EV_FIND_RESULT, &c, 4);
}

static tab_t *tab_register(WebKitWebView *view, int opener_id) {
  int slot = -1;
  for (int i = 0; i < MAX_TABS; i++) {
    if (!g_tabs[i]) {
      slot = i;
      break;
    }
  }
  if (slot < 0)
    return NULL;
  tab_t *tab = g_new0(tab_t, 1);
  tab->id = g_next_tab_id++;
  tab->opener_id = opener_id;
  tab->view = view;
  tab->wview = webkit_web_view_get_wpe_view(view);
  /* The view owns `tab`: freed when the view finalizes, so late callbacks
   * holding `tab` as user data never touch freed memory. */
  g_object_set_data_full(G_OBJECT(view), "chatai-tab-owner", tab, g_free);
  g_object_set_data(G_OBJECT(tab->wview), "chatai-tab", tab);

  g_signal_connect(view, "notify::title", G_CALLBACK(on_notify_title), tab);
  g_signal_connect(view, "notify::uri", G_CALLBACK(on_notify_uri), tab);
  g_signal_connect(view, "notify::estimated-load-progress", G_CALLBACK(on_notify_load), tab);
  g_signal_connect(view, "notify::is-loading", G_CALLBACK(on_notify_load), tab);
  g_signal_connect(view, "mouse-target-changed", G_CALLBACK(on_mouse_target_changed), tab);
  g_signal_connect(view, "run-file-chooser", G_CALLBACK(on_file_chooser), tab);
  g_signal_connect(view, "permission-request", G_CALLBACK(on_permission), tab);
  g_signal_connect(view, "show-notification", G_CALLBACK(on_show_notification), tab);
  g_signal_connect(view, "context-menu", G_CALLBACK(on_context_menu), tab);
  g_signal_connect(view, "create", G_CALLBACK(on_create), tab);
  g_signal_connect(view, "close", G_CALLBACK(on_close), tab);
  g_signal_connect(view, "decide-policy", G_CALLBACK(on_decide_policy), tab);
  g_signal_connect(view, "script-dialog", G_CALLBACK(on_script_dialog), tab);
  g_signal_connect(view, "web-process-terminated", G_CALLBACK(on_web_process_terminated), tab);
  WebKitFindController *fc = webkit_web_view_get_find_controller(view);
  g_signal_connect(fc, "found-text", G_CALLBACK(on_found_text), tab);
  g_signal_connect(fc, "failed-to-find-text", G_CALLBACK(on_failed_to_find), tab);

  g_tabs[slot] = tab;
  if (g_transparent)
    tab_apply_transparency(tab);
  view_apply_geometry(tab);
  wpe_view_map(tab->wview);
  wpe_view_set_visible(tab->wview, FALSE);
  tab_activate(tab); /* new tabs (incl. popups) are born active */
  return tab;
}

static tab_t *tab_new(const char *url) {
  WebKitWebView *view = g_object_new(WEBKIT_TYPE_WEB_VIEW,
                                     "display", g_display,
                                     "web-context", g_context,
                                     "network-session", g_session,
                                     "settings", g_settings,
                                     NULL);
  tab_t *tab = tab_register(view, 0);
  if (!tab) {
    g_object_unref(view);
    return NULL;
  }
  webkit_web_view_load_uri(view, url && url[0] ? url : "about:blank");
  return tab;
}

static void tab_close(int id) {
  for (int i = 0; i < MAX_TABS; i++) {
    tab_t *tab = g_tabs[i];
    if (!tab || tab->id != id)
      continue;
    g_tabs[i] = NULL;
    int opener = tab->opener_id;
    if (g_active == tab) {
      g_active = NULL;
      tab_t *next = tab_find(opener);
      for (int j = 0; !next && j < MAX_TABS; j++)
        next = g_tabs[j];
      tab_activate(next);
    }
    WebKitWebView *view = tab->view;
    g_object_set_data(G_OBJECT(tab->wview), "chatai-tab", NULL);
    g_signal_handlers_disconnect_by_data(view, tab);
    g_signal_handlers_disconnect_by_data(webkit_web_view_get_find_controller(view), tab);
    int32_t v = id;
    send_event(ZURF_EV_TAB_CLOSED, &v, 4);
    g_object_unref(view); /* finalizes the view, then frees `tab` */
    return;
  }
}

/* ------------------------------------------------------------------------ */
/* downloads                                                                */

static download_t *dl_find(WebKitDownload *dl) {
  for (guint i = 0; i < g_downloads->len; i++) {
    download_t *d = g_ptr_array_index(g_downloads, i);
    if (d->dl == dl)
      return d;
  }
  return NULL;
}

static void dl_report(download_t *d, int state) {
  struct {
    int32_t id, state;
    int64_t received, total;
  } __attribute__((packed)) hdr;
  hdr.id = d->id;
  hdr.state = state;
  hdr.received = (int64_t)webkit_download_get_received_data_length(d->dl);
  WebKitURIResponse *resp = webkit_download_get_response(d->dl);
  hdr.total = resp ? (int64_t)webkit_uri_response_get_content_length(resp) : 0;
  size_t plen = d->path ? strlen(d->path) : 0;
  uint8_t *buf = g_malloc(sizeof(hdr) + plen);
  memcpy(buf, &hdr, sizeof(hdr));
  if (plen)
    memcpy(buf + sizeof(hdr), d->path, plen);
  send_event(ZURF_EV_DOWNLOAD, buf, (uint32_t)(sizeof(hdr) + plen));
  g_free(buf);
}

static char *unique_path(const char *dir, const char *name) {
  char *base = g_path_get_basename(name && name[0] ? name : "download");
  /* never let a server pick the directory */
  for (char *p = base; *p; p++)
    if (*p == '/')
      *p = '_';
  char *path = g_build_filename(dir, base, NULL);
  if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
    g_free(base);
    return path;
  }
  g_free(path);
  const char *dot = strrchr(base, '.');
  size_t stem = dot && dot != base ? (size_t)(dot - base) : strlen(base);
  for (int n = 1; n < 10000; n++) {
    char *cand = g_strdup_printf("%.*s (%d)%s", (int)stem, base, n, base + stem);
    path = g_build_filename(dir, cand, NULL);
    g_free(cand);
    if (!g_file_test(path, G_FILE_TEST_EXISTS))
      break;
    g_free(path);
    path = NULL;
  }
  g_free(base);
  return path;
}

static gboolean on_dl_decide_destination(WebKitDownload *dl, const char *suggested, gpointer u) {
  (void)u;
  download_t *d = dl_find(dl);
  const char *dir = g_download_dir ? g_download_dir : g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
  if (!dir)
    dir = g_get_home_dir();
  g_mkdir_with_parents(dir, 0755);
  char *path = unique_path(dir, suggested);
  if (!path) {
    webkit_download_cancel(dl);
    return TRUE;
  }
  webkit_download_set_destination(dl, path);
  if (d) {
    g_free(d->path);
    d->path = path;
    dl_report(d, ZURF_DL_STARTED);
  } else {
    g_free(path);
  }
  return TRUE;
}

static void on_dl_received(WebKitDownload *dl, guint64 len, gpointer u) {
  (void)len;
  (void)u;
  download_t *d = dl_find(dl);
  if (!d)
    return;
  gint64 now = g_get_monotonic_time();
  if (now - d->last_report_us < 250000)
    return;
  d->last_report_us = now;
  dl_report(d, ZURF_DL_PROGRESS);
}

static void on_dl_failed(WebKitDownload *dl, GError *err, gpointer u) {
  (void)u;
  download_t *d = dl_find(dl);
  if (!d)
    return;
  d->failed = 1;
  int cancelled = g_error_matches(err, WEBKIT_DOWNLOAD_ERROR, WEBKIT_DOWNLOAD_ERROR_CANCELLED_BY_USER);
  dl_report(d, cancelled ? ZURF_DL_CANCELLED : ZURF_DL_FAILED);
}

static void on_dl_finished(WebKitDownload *dl, gpointer u) {
  (void)u;
  download_t *d = dl_find(dl);
  if (!d)
    return;
  if (!d->failed)
    dl_report(d, ZURF_DL_FINISHED);
  g_ptr_array_remove(g_downloads, d);
}

static void dl_free(gpointer p) {
  download_t *d = p;
  g_signal_handlers_disconnect_by_data(d->dl, NULL);
  g_object_unref(d->dl);
  g_free(d->path);
  g_free(d);
}

static void on_download_started(WebKitNetworkSession *s, WebKitDownload *dl, gpointer u) {
  (void)s;
  (void)u;
  download_t *d = g_new0(download_t, 1);
  d->id = g_next_dl_id++;
  d->dl = g_object_ref(dl);
  g_ptr_array_add(g_downloads, d);
  g_signal_connect(dl, "decide-destination", G_CALLBACK(on_dl_decide_destination), NULL);
  g_signal_connect(dl, "received-data", G_CALLBACK(on_dl_received), NULL);
  g_signal_connect(dl, "failed", G_CALLBACK(on_dl_failed), NULL);
  g_signal_connect(dl, "finished", G_CALLBACK(on_dl_finished), NULL);
}

static void dl_cancel(int id) {
  for (guint i = 0; i < g_downloads->len; i++) {
    download_t *d = g_ptr_array_index(g_downloads, i);
    if (d->id == id) {
      webkit_download_cancel(d->dl);
      return;
    }
  }
}

/* ------------------------------------------------------------------------ */
/* input                                                                    */

static guint32 now_ms(void) { return (guint32)(g_get_monotonic_time() / 1000); }

static WPEModifiers map_mods(int m) {
  WPEModifiers w = 0;
  if (m & ZURF_MOD_SHIFT)
    w |= WPE_MODIFIER_KEYBOARD_SHIFT;
  if (m & ZURF_MOD_CTRL)
    w |= WPE_MODIFIER_KEYBOARD_CONTROL;
  if (m & ZURF_MOD_ALT)
    w |= WPE_MODIFIER_KEYBOARD_ALT;
  if (m & ZURF_MOD_META)
    w |= WPE_MODIFIER_KEYBOARD_META;
  if (m & ZURF_MOD_BUTTON_LEFT)
    w |= WPE_MODIFIER_POINTER_BUTTON1;
  if (m & ZURF_MOD_BUTTON_MIDDLE)
    w |= WPE_MODIFIER_POINTER_BUTTON2;
  if (m & ZURF_MOD_BUTTON_RIGHT)
    w |= WPE_MODIFIER_POINTER_BUTTON3;
  return w;
}

static void dispatch(WPEEvent *ev) {
  if (g_active)
    wpe_view_event(g_active->wview, ev);
  wpe_event_unref(ev);
}

static void key_event(guint keyval, guint keycode, int down, WPEModifiers mods) {
  if (!g_active || (!keyval && !keycode))
    return;
  if (keycode == 0) {
    /* No hardware code from the host: derive one so `KeyboardEvent.code`
     * is meaningful (the keymap is a default XKB one). */
    WPEKeymapEntry *e = NULL;
    guint n = 0;
    if (wpe_keymap_get_entries_for_keyval(wpe_display_get_keymap(g_display), keyval, &e, &n) && n)
      keycode = e[0].keycode;
    g_free(e);
  }
  dispatch(wpe_event_keyboard_new(down ? WPE_EVENT_KEYBOARD_KEY_DOWN : WPE_EVENT_KEYBOARD_KEY_UP,
                                  g_active->wview, WPE_INPUT_SOURCE_KEYBOARD, now_ms(), mods,
                                  keycode, keyval));
}

/* Zurf's Windows-VK keys (kept for protocol compatibility). */
static guint keyval_of_vk(int vk) {
  switch (vk) {
  case 0x08: return WPE_KEY_BackSpace;
  case 0x09: return WPE_KEY_Tab;
  case 0x0d: return WPE_KEY_Return;
  case 0x1b: return WPE_KEY_Escape;
  case 0x20: return WPE_KEY_space;
  case 0x21: return WPE_KEY_Page_Up;
  case 0x22: return WPE_KEY_Page_Down;
  case 0x23: return WPE_KEY_End;
  case 0x24: return WPE_KEY_Home;
  case 0x25: return WPE_KEY_Left;
  case 0x26: return WPE_KEY_Up;
  case 0x27: return WPE_KEY_Right;
  case 0x28: return WPE_KEY_Down;
  case 0x2d: return WPE_KEY_Insert;
  case 0x2e: return WPE_KEY_Delete;
  case 0x74: return WPE_KEY_F5;
  default:
    if (vk >= 'A' && vk <= 'Z')
      return (guint)(vk - 'A' + 'a');
    if (vk >= '0' && vk <= '9')
      return (guint)vk;
    return 0;
  }
}

static void op_key_vk(int vk, int down, int mods) {
  int printable = (vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9') || vk == 0x20;
  if (printable && (mods & (ZURF_MOD_CTRL | ZURF_MOD_ALT)) == 0)
    return; /* text arrives via ZURF_OP_TEXT */
  key_event(keyval_of_vk(vk), 0, down, map_mods(mods));
}

static void op_text(const char *utf8) {
  for (const char *p = utf8; p && *p; p = g_utf8_next_char(p)) {
    gunichar c = g_utf8_get_char_validated(p, -1);
    if (c == (gunichar)-1 || c == (gunichar)-2)
      break;
    guint kv = wpe_unicode_to_keyval(c);
    key_event(kv, 0, 1, 0);
    key_event(kv, 0, 0, 0);
  }
}

static void pointer_enter_if_needed(double x, double y, WPEModifiers mods) {
  if (g_ptr_inside || !g_active)
    return;
  g_ptr_inside = 1;
  dispatch(wpe_event_pointer_move_new(WPE_EVENT_POINTER_ENTER, g_active->wview,
                                      WPE_INPUT_SOURCE_MOUSE, now_ms(), mods, x, y, 0, 0));
}

static void op_mouse_move(int x, int y, int mods) {
  if (!g_active)
    return;
  WPEModifiers m = map_mods(mods);
  pointer_enter_if_needed(x, y, m);
  double dx = x - g_ptr_x, dy = y - g_ptr_y;
  g_ptr_x = x;
  g_ptr_y = y;
  dispatch(wpe_event_pointer_move_new(WPE_EVENT_POINTER_MOVE, g_active->wview,
                                      WPE_INPUT_SOURCE_MOUSE, now_ms(), m, x, y, dx, dy));
}

static void op_mouse_leave(void) {
  if (!g_active || !g_ptr_inside)
    return;
  g_ptr_inside = 0;
  dispatch(wpe_event_pointer_move_new(WPE_EVENT_POINTER_LEAVE, g_active->wview,
                                      WPE_INPUT_SOURCE_MOUSE, now_ms(), 0, g_ptr_x, g_ptr_y, 0, 0));
}

static void op_mouse_button(int x, int y, int button, int down, int clicks, int mods) {
  if (!g_active)
    return;
  guint b = button == 1 ? WPE_BUTTON_MIDDLE : button == 2 ? WPE_BUTTON_SECONDARY : WPE_BUTTON_PRIMARY;
  WPEModifiers m = map_mods(mods);
  guint32 t = now_ms();
  pointer_enter_if_needed(x, y, m);
  g_ptr_x = x;
  g_ptr_y = y;
  if (down)
    g_press_count = clicks > 0 ? (guint)clicks
                               : wpe_view_compute_press_count(g_active->wview, x, y, b, t);
  dispatch(wpe_event_pointer_button_new(down ? WPE_EVENT_POINTER_DOWN : WPE_EVENT_POINTER_UP,
                                        g_active->wview, WPE_INPUT_SOURCE_MOUSE, t, m, b, x, y,
                                        down ? (g_press_count ? g_press_count : 1) : 0));
}

/* WPE deltas: +dy = scroll up; precise = pixels/2.5, otherwise wheel steps. */
static void scroll(double x, double y, double dx, double dy, int precise) {
  if (!g_active)
    return;
  pointer_enter_if_needed(x, y, 0);
  dispatch(wpe_event_scroll_new(g_active->wview,
                                precise ? WPE_INPUT_SOURCE_TOUCHPAD : WPE_INPUT_SOURCE_MOUSE,
                                now_ms(), 0, dx, dy, precise, FALSE, x, y));
}

static void op_resize(int w, int h) {
  if (w <= 0 || h <= 0)
    return;
  g_width = w;
  g_height = h;
  for (int i = 0; i < MAX_TABS; i++)
    if (g_tabs[i])
      view_apply_geometry(g_tabs[i]);
}

static void op_set_scale(int pct) {
  double s = pct / 100.0;
  if (s < 0.5 || s > 4.0)
    return;
  g_scale = s;
  WPEScreen *scr = z_display_screen();
  if (scr)
    wpe_screen_set_scale(scr, s);
  for (int i = 0; i < MAX_TABS; i++)
    if (g_tabs[i])
      view_apply_geometry(g_tabs[i]);
}

static void op_focus(int focused) {
  g_focused = focused;
  if (!g_active)
    return;
  if (focused)
    wpe_view_focus_in(g_active->wview);
  else
    wpe_view_focus_out(g_active->wview);
}

/* ------------------------------------------------------------------------ */
/* clipboard from host                                                      */

static void op_clipboard_set(const uint8_t *p, uint32_t len) {
  WPEClipboardContent *c = wpe_clipboard_content_new();
  uint32_t off = 0;
  int any = 0;
  while (off + 4 <= len) {
    uint32_t ml;
    memcpy(&ml, p + off, 4);
    off += 4;
    if (off + ml + 4 > len)
      break;
    char *mime = g_strndup((const char *)p + off, ml);
    off += ml;
    uint32_t dl;
    memcpy(&dl, p + off, 4);
    off += 4;
    if (off + dl > len) {
      g_free(mime);
      break;
    }
    if (is_text_mime(mime)) {
      char *t = g_strndup((const char *)p + off, dl);
      wpe_clipboard_content_set_text(c, t);
      g_free(t);
    } else {
      GBytes *b = g_bytes_new(p + off, dl);
      wpe_clipboard_content_set_bytes(c, mime, b);
      g_bytes_unref(b);
    }
    any = 1;
    g_free(mime);
    off += dl;
  }
  g_clip_from_host = 1;
  wpe_clipboard_set_content(wpe_display_get_clipboard(g_display), any ? c : NULL);
  g_clip_from_host = 0;
  wpe_clipboard_content_unref(c);
}

/* ------------------------------------------------------------------------ */
/* JS helpers                                                               */

static void on_selection_js(GObject *obj, GAsyncResult *res, gpointer data) {
  (void)data;
  JSCValue *v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(obj), res, NULL);
  if (!v)
    return;
  if (jsc_value_is_string(v)) {
    char *s = jsc_value_to_string(v);
    if (s && s[0])
      send_str(ZURF_EV_CLIPBOARD, s);
    g_free(s);
  }
  g_object_unref(v);
}

static void on_eval_ret_js(GObject *obj, GAsyncResult *res, gpointer data) {
  int32_t req = (int32_t)(intptr_t)data;
  JSCValue *v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(obj), res, NULL);
  char *s = NULL;
  if (v && !jsc_value_is_undefined(v) && !jsc_value_is_null(v))
    s = jsc_value_is_string(v) ? jsc_value_to_string(v) : jsc_value_to_json(v, 0);
  send_ints_bytes(ZURF_EV_JS_RESULT, &req, 1, s, s ? strlen(s) : 0);
  g_free(s);
  if (v)
    g_object_unref(v);
}

/* ------------------------------------------------------------------------ */
/* find                                                                     */

static void op_find(int flags, const char *text) {
  if (!g_active)
    return;
  WebKitFindController *fc = webkit_web_view_get_find_controller(g_active->view);
  if (!text || !text[0]) {
    webkit_find_controller_search_finish(fc);
    g_clear_pointer(&g_find_text, g_free);
    return;
  }
  guint32 opts = WEBKIT_FIND_OPTIONS_WRAP_AROUND;
  if (!(flags & ZURF_FIND_CASE_SENSITIVE))
    opts |= WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE;
  if (g_find_text && strcmp(g_find_text, text) == 0) {
    if (flags & ZURF_FIND_BACKWARDS)
      webkit_find_controller_search_previous(fc);
    else
      webkit_find_controller_search_next(fc);
    return;
  }
  g_free(g_find_text);
  g_find_text = g_strdup(text);
  if (flags & ZURF_FIND_BACKWARDS)
    opts |= WEBKIT_FIND_OPTIONS_BACKWARDS;
  webkit_find_controller_search(fc, text, opts, 1000);
}

/* ------------------------------------------------------------------------ */
/* prefs                                                                    */

/* Toggle a WebKit runtime feature by identifier (see
 * webkit_settings_get_all_features); unknown ids are ignored so the engine
 * keeps working across WebKit versions. */
static void set_feature(const char *id, gboolean on) {
  WebKitFeatureList *all = webkit_settings_get_all_features();
  for (gsize i = 0; i < webkit_feature_list_get_length(all); i++) {
    WebKitFeature *f = webkit_feature_list_get(all, i);
    if (strcmp(webkit_feature_get_identifier(f), id) == 0) {
      webkit_settings_set_feature_enabled(g_settings, f, on);
      break;
    }
  }
  webkit_feature_list_unref(all);
}

static int truthy(const char *v) { return v && (v[0] == '1' || v[0] == 't' || v[0] == 'y'); }

/* Transparent page background: the WebKit base colour is cleared and the
 * page's own background containers are made see-through by a user script
 * (clear_background.js), so the blurred Plasma popup shows behind the chat. */
static void tab_apply_transparency(tab_t *tab) {
  WebKitWebView *view = tab->view;
  WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(view);
  webkit_user_content_manager_remove_all_scripts(ucm);
  webkit_user_content_manager_remove_all_style_sheets(ucm);
  WebKitColor bg = {1, 1, 1, g_transparent ? 0 : 1};
  webkit_web_view_set_background_color(view, &bg);
  if (g_transparent) {
    WebKitUserStyleSheet *css = webkit_user_style_sheet_new(
        "html:not([data-chatai-probe]),html:not([data-chatai-probe])>body,"
        "[data-chatai-clear],[data-chatai-clear-before]::before,[data-chatai-clear-after]::after"
        "{background-color:transparent!important}",
        WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, WEBKIT_USER_STYLE_LEVEL_USER, NULL, NULL);
    webkit_user_content_manager_add_style_sheet(ucm, css);
    webkit_user_style_sheet_unref(css);
    WebKitUserScript *js = webkit_user_script_new(
        CLEAR_BACKGROUND_JS, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
    webkit_user_content_manager_add_script(ucm, js);
    webkit_user_script_unref(js);
  }
  /* the page already shown: start/stop in place, no reload */
  webkit_web_view_evaluate_javascript(view,
      g_transparent ? CLEAR_BACKGROUND_JS : "window.__chataiClear && window.__chataiClear.stop()",
      -1, NULL, NULL, NULL, NULL, NULL);
}

static void apply_pref(const char *k, const char *v) {
  if (strcmp(k, "user-agent") == 0) {
    if (v[0])
      webkit_settings_set_user_agent(g_settings, v);
    else
      webkit_settings_set_user_agent_with_application_details(g_settings, NULL, NULL);
  } else if (strcmp(k, "download-dir") == 0) {
    g_free(g_download_dir);
    g_download_dir = v[0] ? g_strdup(v) : NULL;
  } else if (strcmp(k, "dark-mode") == 0) {
    wpe_settings_set_boolean(wpe_display_get_settings(g_display), WPE_SETTING_DARK_MODE,
                             truthy(v), WPE_SETTINGS_SOURCE_APPLICATION, NULL);
  } else if (strcmp(k, "js-open-windows") == 0) {
    webkit_settings_set_javascript_can_open_windows_automatically(g_settings, truthy(v));
  } else if (strcmp(k, "js-clipboard") == 0) {
    webkit_settings_set_javascript_can_access_clipboard(g_settings, truthy(v));
  } else if (strcmp(k, "media-gesture") == 0) {
    webkit_settings_set_media_playback_requires_user_gesture(g_settings, truthy(v));
  } else if (strcmp(k, "spatial-nav") == 0) {
    webkit_settings_set_enable_spatial_navigation(g_settings, truthy(v));
  } else if (strcmp(k, "max-fps") == 0) {
    int fps = atoi(v);
    if (fps >= 5 && fps <= 240)
      g_frame_interval_us = 1000000 / fps;
  } else if (strcmp(k, "new-window-policy") == 0) {
    g_popups_as_tabs = strcmp(v, "external") != 0;
  } else if (strcmp(k, "transparent") == 0) {
    if (g_transparent != truthy(v)) {
      g_transparent = truthy(v);
      for (int i = 0; i < MAX_TABS; i++)
        if (g_tabs[i])
          tab_apply_transparency(g_tabs[i]);
    }
  }
}

static void op_set_prefs(const char *text) {
  char **lines = g_strsplit(text, "\n", -1);
  for (int i = 0; lines[i]; i++) {
    char *eq = strchr(lines[i], '=');
    if (!eq)
      continue;
    *eq = 0;
    apply_pref(g_strstrip(lines[i]), eq + 1);
  }
  g_strfreev(lines);
}

/* ------------------------------------------------------------------------ */
/* control-plane receive                                                    */

static int read_exact(int fd, void *buf, size_t n) {
  uint8_t *p = buf;
  while (n > 0) {
    ssize_t r = read(fd, p, n);
    if (r < 0 && errno == EINTR)
      continue;
    if (r <= 0)
      return -1;
    p += r;
    n -= (size_t)r;
  }
  return 0;
}

static int32_t geti(const uint8_t *p, int idx) {
  int32_t v;
  memcpy(&v, p + 4 * idx, 4);
  return v;
}

static char *dup_str(const uint8_t *p, uint32_t n) { return g_strndup((const char *)p, n); }

static void handle(uint8_t op, const uint8_t *pl, uint32_t len) {
  WebKitWebView *view = g_active ? g_active->view : NULL;
  switch (op) {
  case ZURF_OP_NAVIGATE:
    if (view) {
      char *url = dup_str(pl, len);
      webkit_web_view_load_uri(view, url);
      g_free(url);
    }
    break;
  case ZURF_OP_RELOAD:
    if (view)
      webkit_web_view_reload(view);
    break;
  case ZURF_OP_RELOAD_BYPASS:
    if (view)
      webkit_web_view_reload_bypass_cache(view);
    break;
  case ZURF_OP_STOP:
    if (view)
      webkit_web_view_stop_loading(view);
    break;
  case ZURF_OP_GO_BACK:
    if (view)
      webkit_web_view_go_back(view);
    break;
  case ZURF_OP_GO_FORWARD:
    if (view)
      webkit_web_view_go_forward(view);
    break;
  case ZURF_OP_RESIZE:
    if (len >= 8)
      op_resize(geti(pl, 0), geti(pl, 1));
    break;
  case ZURF_OP_SET_SCALE:
    if (len >= 4)
      op_set_scale(geti(pl, 0));
    break;
  case ZURF_OP_MOUSE_MOVE:
    if (len >= 12)
      op_mouse_move(geti(pl, 0), geti(pl, 1), geti(pl, 2));
    break;
  case ZURF_OP_MOUSE_LEAVE:
    op_mouse_leave();
    break;
  case ZURF_OP_MOUSE_BUTTON:
    if (len >= 24)
      op_mouse_button(geti(pl, 0), geti(pl, 1), geti(pl, 2), geti(pl, 3), geti(pl, 4), geti(pl, 5));
    break;
  case ZURF_OP_MOUSE_WHEEL:
    /* Zurf: notches*120, +dy = up */
    if (len >= 16)
      scroll(geti(pl, 0), geti(pl, 1), geti(pl, 2) / 120.0, geti(pl, 3) / 120.0, 0);
    break;
  case ZURF_OP_SCROLL:
    /* Qt conventions, +dy = up. precise: pixels*100 — WebKit scales precise
     * deltas by 2.5, undo it so a touchpad moves content 1:1. Otherwise
     * angle units*100 (120 per notch) → WebKit wheel steps. */
    if (len >= 20) {
      int precise = geti(pl, 4);
      double k = precise ? 100.0 * 2.5 : 100.0 * 120.0;
      scroll(geti(pl, 0), geti(pl, 1), geti(pl, 2) / k, geti(pl, 3) / k, precise);
    }
    break;
  case ZURF_OP_KEY:
    if (len >= 16)
      op_key_vk(geti(pl, 0), geti(pl, 1), geti(pl, 2));
    break;
  case ZURF_OP_KEY_NATIVE:
    if (len >= 16)
      key_event((guint)geti(pl, 0), (guint)geti(pl, 1), geti(pl, 2), map_mods(geti(pl, 3)));
    break;
  case ZURF_OP_TEXT: {
    char *t = dup_str(pl, len);
    op_text(t);
    g_free(t);
    break;
  }
  case ZURF_OP_INSERT_TEXT:
    if (view) {
      char *t = dup_str(pl, len);
      webkit_web_view_execute_editing_command_with_argument(view, "InsertText", t);
      g_free(t);
    }
    break;
  case ZURF_OP_EDIT:
    if (view) {
      char *cmd = dup_str(pl, len);
      webkit_web_view_execute_editing_command(view, cmd);
      g_free(cmd);
    }
    break;
  case ZURF_OP_EVAL_JS:
    if (view) {
      char *js = dup_str(pl, len);
      webkit_web_view_evaluate_javascript(view, js, -1, NULL, NULL, NULL, NULL, NULL);
      g_free(js);
    }
    break;
  case ZURF_OP_EVAL_JS_RET:
    if (len >= 4) {
      int32_t req = geti(pl, 0);
      if (view) {
        char *js = dup_str(pl + 4, len - 4);
        webkit_web_view_evaluate_javascript(view, js, -1, NULL, NULL, NULL, on_eval_ret_js,
                                            (gpointer)(intptr_t)req);
        g_free(js);
      } else {
        send_event(ZURF_EV_JS_RESULT, &req, 4);
      }
    }
    break;
  case ZURF_OP_SET_VISIBLE:
    if (len >= 4) {
      g_visible = geti(pl, 0);
      tab_set_visible(g_active, g_visible);
    }
    break;
  case ZURF_OP_FOCUS:
    if (len >= 4)
      op_focus(geti(pl, 0));
    break;
  case ZURF_OP_COPY:
    if (view) {
      webkit_web_view_execute_editing_command(view, WEBKIT_EDITING_COMMAND_COPY);
      webkit_web_view_evaluate_javascript(view, "window.getSelection().toString()", -1, NULL,
                                          NULL, NULL, on_selection_js, NULL);
    }
    break;
  case ZURF_OP_TAB_NEW: {
    char *url = dup_str(pl, len);
    tab_new(url);
    g_free(url);
    break;
  }
  case ZURF_OP_TAB_CLOSE:
    if (len >= 4)
      tab_close(geti(pl, 0));
    break;
  case ZURF_OP_TAB_SWITCH:
    if (len >= 4) {
      tab_t *t = tab_find(geti(pl, 0));
      if (t)
        tab_activate(t);
    }
    break;
  case ZURF_OP_FILE_CHOOSER_REPLY:
    if (len >= 4) {
      GObject *o = pending_take(geti(pl, 0));
      if (!o)
        break;
      WebKitFileChooserRequest *req = WEBKIT_FILE_CHOOSER_REQUEST(o);
      char *list = dup_str(pl + 4, len - 4);
      char **files = g_strsplit(list, "\n", -1);
      /* drop empties */
      int n = 0;
      for (int i = 0; files[i]; i++)
        if (files[i][0])
          files[n++] = files[i];
        else
          g_free(files[i]);
      files[n] = NULL;
      if (n > 0)
        webkit_file_chooser_request_select_files(req, (const gchar *const *)files);
      else
        webkit_file_chooser_request_cancel(req);
      g_strfreev(files);
      g_free(list);
      g_object_unref(o);
    }
    break;
  case ZURF_OP_PERMISSION_REPLY:
    if (len >= 8) {
      GObject *o = pending_take(geti(pl, 0));
      if (!o)
        break;
      if (geti(pl, 1))
        webkit_permission_request_allow(WEBKIT_PERMISSION_REQUEST(o));
      else
        webkit_permission_request_deny(WEBKIT_PERMISSION_REQUEST(o));
      g_object_unref(o);
    }
    break;
  case ZURF_OP_FIND:
    if (len >= 4) {
      char *t = dup_str(pl + 4, len - 4);
      op_find(geti(pl, 0), t);
      g_free(t);
    }
    break;
  case ZURF_OP_CLIPBOARD_SET:
    op_clipboard_set(pl, len);
    break;
  case ZURF_OP_SET_PREFS: {
    char *t = dup_str(pl, len);
    op_set_prefs(t);
    g_free(t);
    break;
  }
  case ZURF_OP_DOWNLOAD_CANCEL:
    if (len >= 4)
      dl_cancel(geti(pl, 0));
    break;
  case ZURF_OP_SHUTDOWN:
    g_main_loop_quit(g_loop);
    break;
  default:
    break;
  }
}

static gboolean on_ctl_readable(GIOChannel *ch, GIOCondition cond, gpointer data) {
  (void)ch;
  (void)data;
  if (cond & (G_IO_HUP | G_IO_ERR)) {
    g_main_loop_quit(g_loop);
    return G_SOURCE_REMOVE;
  }
  zurf_msg_header h;
  if (read_exact(g_ctl, &h, sizeof(h)) != 0 || h.len > ZURF_MSG_MAX) {
    g_main_loop_quit(g_loop);
    return G_SOURCE_REMOVE;
  }
  uint8_t *payload = NULL;
  if (h.len > 0) {
    payload = g_malloc(h.len);
    if (read_exact(g_ctl, payload, h.len) != 0) {
      g_free(payload);
      g_main_loop_quit(g_loop);
      return G_SOURCE_REMOVE;
    }
  }
  handle(h.op, payload, h.len);
  g_free(payload);
  return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------------ */
/* GPU policy                                                               */

/* The integrated GPU: the one that drives the display (boot_vga=1), or
 * failing that the first Intel card; nouveau devices are skipped. Returns its render node and PCI slot.
 * On hybrid laptops the discrete card is often renderD128, i.e. the one
 * Mesa picks by default (WEBKIT_WEB_RENDER_DEVICE_FILE alone doesn't stop
 * that: verified via fdinfo, rendering landed on nouveau). */
static gboolean integrated_gpu(char **node, char **pci) {
  *node = *pci = NULL;
  GDir *dir = g_dir_open("/sys/class/drm", 0, NULL);
  if (!dir)
    return FALSE;
  char *intel = NULL;
  const char *name;
  while (!*node && (name = g_dir_read_name(dir))) {
    if (!g_str_has_prefix(name, "renderD"))
      continue;
    /* never nouveau: its Mesa driver is what crashed the web process */
    char *dpath = g_strdup_printf("/sys/class/drm/%s/device/driver", name);
    char *drv = g_file_read_link(dpath, NULL);
    gboolean is_nouveau = drv && g_str_has_suffix(drv, "/nouveau");
    g_free(drv);
    g_free(dpath);
    if (is_nouveau)
      continue;
    char *path = g_strdup_printf("/sys/class/drm/%s/device/boot_vga", name);
    char *vpath = g_strdup_printf("/sys/class/drm/%s/device/vendor", name);
    char *val = NULL, *vendor = NULL;
    if (g_file_get_contents(path, &val, NULL, NULL) && val[0] == '1')
      *node = g_strdup(name);
    else if (!intel && g_file_get_contents(vpath, &vendor, NULL, NULL) && g_str_has_prefix(vendor, "0x8086"))
      intel = g_strdup(name);
    g_free(val);
    g_free(vendor);
    g_free(path);
    g_free(vpath);
  }
  g_dir_close(dir);
  if (!*node)
    *node = g_steal_pointer(&intel);
  g_free(intel);
  if (!*node)
    return FALSE;
  char *link = g_strdup_printf("/sys/class/drm/%s/device", *node);
  char *real = realpath(link, NULL);
  g_free(link);
  if (real) {
    /* DRI_PRIME wants "pci-0000_00_02_0" for slot 0000:00:02.0 */
    char *slot = g_strdelimit(g_path_get_basename(real), ":.", '_');
    *pci = g_strdup_printf("pci-%s", slot);
    g_free(slot);
    free(real);
  }
  char *dev = g_strdup_printf("/dev/dri/%s", *node);
  g_free(*node);
  *node = dev;
  return TRUE;
}

/* WebKit's web process always creates an EGL context for compositing. By
 * default it gets Mesa's software rasterizer: no GPU driver is loaded at all,
 * so driver bugs (e.g. nouveau crashing on hybrid laptops) can't take the
 * page down and a sleeping discrete GPU stays asleep. The environment is
 * inherited by the WebKit child processes. */
static void configure_gpu(int use_gpu) {
  g_setenv("WEBKIT_SKIA_ENABLE_CPU_RENDERING", "1", FALSE);
  g_unsetenv("DRI_PRIME");
  if (g_file_test("/usr/share/glvnd/egl_vendor.d/50_mesa.json", G_FILE_TEST_EXISTS))
    g_setenv("__EGL_VENDOR_LIBRARY_FILENAMES", "/usr/share/glvnd/egl_vendor.d/50_mesa.json", TRUE);
  if (!use_gpu) {
    g_setenv("LIBGL_ALWAYS_SOFTWARE", "1", TRUE);
    g_setenv("GALLIUM_DRIVER", "llvmpipe", TRUE);
    g_setenv("LP_NUM_THREADS", "0", FALSE); /* rasterize inline: fewer threads, less RAM */
    return;
  }
  char *node, *pci;
  if (!integrated_gpu(&node, &pci)) {
    fprintf(stderr, "chatai-webwpe: no integrated GPU found, using software rendering\n");
    configure_gpu(0);
    return;
  }
  /* Both are needed: WebKit's device for buffers, DRI_PRIME so Mesa's EGL
   * creates its context there and not on the first (discrete) card. */
  g_setenv("WEBKIT_WEB_RENDER_DEVICE_FILE", node, TRUE);
  if (pci)
    g_setenv("DRI_PRIME", pci, TRUE);
  fprintf(stderr, "chatai-webwpe: GPU rendering on %s (%s)\n", node, pci ? pci : "?");
  g_free(node);
  g_free(pci);
}

/* ------------------------------------------------------------------------ */
/* main                                                                     */

static const char *arg_value(int argc, char **argv, const char *name) {
  size_t n = strlen(name);
  for (int i = 1; i < argc; i++)
    if (strncmp(argv[i], name, n) == 0 && argv[i][n] == '=')
      return argv[i] + n + 1;
  return NULL;
}

static int arg_flag(int argc, char **argv, const char *name) {
  for (int i = 1; i < argc; i++)
    if (strcmp(argv[i], name) == 0)
      return 1;
  return 0;
}

int main(int argc, char **argv) {
  const char *shm_path = arg_value(argc, argv, "--zurf-shm");
  const char *cache_dir = arg_value(argc, argv, "--zurf-cache");
  const char *url = arg_value(argc, argv, "--zurf-url");
  const char *w_s = arg_value(argc, argv, "--zurf-w");
  const char *h_s = arg_value(argc, argv, "--zurf-h");
  const char *fd_s = arg_value(argc, argv, "--zurf-control-fd");
  const char *scale_s = arg_value(argc, argv, "--zurf-scale");
  const char *mem_s = arg_value(argc, argv, "--memory-limit-mb");
  const char *fps_s = arg_value(argc, argv, "--max-fps");
  if (fps_s && atoi(fps_s) >= 5 && atoi(fps_s) <= 240)
    g_frame_interval_us = 1000000 / atoi(fps_s);
  g_frame_events = arg_flag(argc, argv, "--zurf-frame-events");
  if (!shm_path || !cache_dir) {
    fprintf(stderr, "chatai-webwpe: missing --zurf-shm / --zurf-cache\n");
    return 2;
  }
  if (fd_s)
    g_ctl = atoi(fd_s);
  if (w_s && atoi(w_s) > 0)
    g_width = atoi(w_s);
  if (h_s && atoi(h_s) > 0)
    g_height = atoi(h_s);
  if (scale_s && atoi(scale_s) >= 50)
    g_scale = atoi(scale_s) / 100.0;

  int shm_fd = open(shm_path, O_RDWR | O_CLOEXEC);
  if (shm_fd < 0) {
    fprintf(stderr, "chatai-webwpe: cannot open %s\n", shm_path);
    return 2;
  }
  void *map = mmap(NULL, ZURF_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
  close(shm_fd);
  if (map == MAP_FAILED) {
    fprintf(stderr, "chatai-webwpe: mmap failed\n");
    return 2;
  }
  g_shm = map;
  g_shm_pixels = (uint8_t *)map + sizeof(zurf_shm_header);

  configure_gpu(arg_flag(argc, argv, "--gpu") || truthy(g_getenv("CHATAI_WEB_GPU")));
  /* JavaScript interpreter only: chat sites' JIT tiers keep profiling and
   * compiler data in the malloc heap (measured on a logged-in ChatGPT:
   * ~150-200 MB less), at the cost of slower scripts. Inherited by the web
   * process through the environment. */
  if (arg_flag(argc, argv, "--no-jit"))
    g_setenv("JSC_useJIT", "0", TRUE);

  g_loop = g_main_loop_new(NULL, FALSE);
  g_downloads = g_ptr_array_new_with_free_func(dl_free);

  g_display = g_object_new(Z_TYPE_DISPLAY, NULL);
  GError *err = NULL;
  if (!wpe_display_connect(g_display, &err)) {
    fprintf(stderr, "chatai-webwpe: display connect failed: %s\n", err ? err->message : "?");
    return 1;
  }
  wpe_display_set_primary(g_display);

  /* Memory pressure: let WebKit trim caches early and kill a runaway web
   * process instead of growing without bound inside a panel widget. */
  int mem_mb = mem_s ? atoi(mem_s) : 0;
  if (mem_mb > 0) {
    WebKitMemoryPressureSettings *mp = webkit_memory_pressure_settings_new();
    webkit_memory_pressure_settings_set_memory_limit(mp, (guint)mem_mb);
    webkit_memory_pressure_settings_set_conservative_threshold(mp, 0.5);
    webkit_memory_pressure_settings_set_strict_threshold(mp, 0.75);
    g_context = g_object_new(WEBKIT_TYPE_WEB_CONTEXT, "memory-pressure-settings", mp, NULL);
    webkit_network_session_set_memory_pressure_settings(mp);
    webkit_memory_pressure_settings_free(mp);
  }
  if (!g_context)
    g_context = webkit_web_context_new();

  char *data_dir = g_build_filename(cache_dir, "data", NULL);
  char *cache_sub = g_build_filename(cache_dir, "cache", NULL);
  g_mkdir_with_parents(data_dir, 0700);
  g_session = webkit_network_session_new(data_dir, cache_sub);
  webkit_network_session_set_persistent_credential_storage_enabled(g_session, TRUE);
  WebKitCookieManager *cm = webkit_network_session_get_cookie_manager(g_session);
  char *cookies = g_build_filename(data_dir, "cookies.sqlite", NULL);
  webkit_cookie_manager_set_persistent_storage(cm, cookies, WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
  webkit_cookie_manager_set_accept_policy(cm, WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY);
  g_free(cookies);
  g_free(data_dir);
  g_free(cache_sub);
  g_signal_connect(g_session, "download-started", G_CALLBACK(on_download_started), NULL);

  webkit_web_context_set_cache_model(g_context, WEBKIT_CACHE_MODEL_DOCUMENT_BROWSER);

  g_settings = webkit_settings_new();
  webkit_settings_set_enable_page_cache(g_settings, FALSE); /* no bfcache bloat */
  webkit_settings_set_enable_media_stream(g_settings, TRUE);  /* voice input */
  webkit_settings_set_enable_webrtc(g_settings, TRUE);
  webkit_settings_set_enable_mediasource(g_settings, TRUE);
  webkit_settings_set_enable_encrypted_media(g_settings, FALSE);
  webkit_settings_set_enable_webgl(g_settings, FALSE); /* CPU-only engine */
  webkit_settings_set_enable_2d_canvas_acceleration(g_settings, FALSE);
  webkit_settings_set_javascript_can_access_clipboard(g_settings, TRUE);
  webkit_settings_set_javascript_can_open_windows_automatically(g_settings, TRUE);
  webkit_settings_set_media_playback_requires_user_gesture(g_settings, FALSE);
  webkit_settings_set_enable_developer_extras(g_settings, FALSE);
  /* Popup closed → page hidden: throttle its JS timers hard. */
  set_feature("HiddenPageDOMTimerThrottling", TRUE);
  set_feature("HiddenPageDOMTimerThrottlingAutoIncreases", TRUE);
  /* Footprint: one web process for everything (pages, iframes, popups),
   * no process swap on cross-site navigation (which leaves the old process
   * cached), no back/forward cache. A chat widget has one page; the
   * isolation these buy a full browser isn't worth 2-4x the RAM here.
   * --multi-process restores WebKit's defaults. */
  if (!arg_flag(argc, argv, "--multi-process")) {
    set_feature("UsesSingleWebProcess", TRUE);
    set_feature("ProcessSwapOnCrossSiteNavigation", FALSE);
    set_feature("UsesBackForwardCache", FALSE);
    /* COOP forces a process swap on sites like ChatGPT, Gemini and Claude
     * and WebKit keeps the original ~75 MB process alive. None of them
     * need cross-origin isolation here (crossOriginIsolated is already
     * false), so one process is enough. */
    set_feature("CrossOriginOpenerPolicy", FALSE);
  }

  if (!tab_new(url && url[0] ? url : "about:blank")) {
    fprintf(stderr, "chatai-webwpe: first tab creation failed\n");
    return 1;
  }

  GIOChannel *ch = g_io_channel_unix_new(g_ctl);
  g_io_channel_set_encoding(ch, NULL, NULL);
  g_io_add_watch(ch, G_IO_IN | G_IO_HUP | G_IO_ERR, on_ctl_readable, NULL);

  send_event(ZURF_EV_READY, NULL, 0);
  g_main_loop_run(g_loop);

  /* Orderly teardown so WebKit flushes cookies/local storage to disk. */
  for (int i = 0; i < MAX_TABS; i++)
    if (g_tabs[i])
      tab_close(g_tabs[i]->id);
  g_clear_object(&g_session);
  return 0;
}
