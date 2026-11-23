// PSP media v1.1 - media player with Miku-style UI and settings
// Tabs: Home (cards) / PSP memory (folders) / Search / Settings / About
// Audio: MP3 (libmad). Photos: JPG, PNG. Video: files are listed, playback comes later.
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspaudio.h>
#include <psppower.h>
#include <pspiofilemgr.h>
#include <pspge.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <setjmp.h>
#include <mad.h>
#include <jpeglib.h>
#include <png.h>
#ifdef HAVE_FFMPEG
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/avutil.h>
#endif
#include <time.h>
#include "font.h"
#include "assets.h"
#include "lang.h"

PSP_MODULE_INFO("PSPmedia", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);

#define SW 480
#define SH 272
#define MAX_ALL 4000
#define MAX_DIR 1500
#define ROWS_L 9
#define ROWS_S 6

#define RGB(r, g, b) (0xFF000000u | ((unsigned)(b) << 16) | ((unsigned)(g) << 8) | (unsigned)(r))
#define C_WHITE RGB(255, 255, 255)
#define C_ACC RGB(70, 225, 235)
#define C_TITLE RGB(110, 230, 255)
#define C_DARK RGB(6, 16, 36)
#define C_GRAY RGB(175, 200, 220)
#define C_GREEN RGB(110, 255, 170)
#define C_YELLOW RGB(255, 225, 90)
#define C_BLACK RGB(0, 0, 0)

enum { T_DIR, T_AUDIO, T_VIDEO, T_IMAGE, T_OTHER };
typedef struct { char path[256]; char disp[64]; int type; uint32_t size; } Entry;
typedef struct { Entry *it[MAX_ALL]; int n, cursor, top; } View;

static Entry all[MAX_ALL]; static int nall;
static Entry dirbuf[MAX_DIR]; static int ndir;
static View vcat, vmem, vsrch;
static char curdir[256] = "ms0:/";
static int cnt[5];

static uint32_t fb[SW * SH];
static uint32_t *vram;
static uint32_t *bgimg[3];   // 0 blue, 1 dark, 2 custom (from a photo)

static int g_tab, g_sub, g_card, g_sfocus, g_ci, g_kind;
static unsigned g_frames, held;
static char msgbuf[96]; static int msgtimer;

/* ================= settings, language, key mapping ================= */
typedef struct {
  uint32_t magic, ver;
  uint8_t lang, bg, dim, rep, shuf, startvol, stick, sens, preview, cpu, hints, pad0;
  uint32_t keymap[7];
  char bgpath[256];
} Cfg;
#define CFG_MAGIC 0x504D4544u
#define CFG_VER 1
#define CFG_PATH "ms0:/PSP/PSPmedia.cfg"
static Cfg cfg;
enum { ACT_OK, ACT_BACK, ACT_PAUSE, ACT_STOP, ACT_PREV, ACT_NEXT, ACT_ALT, ACT_N };
// logical action -> default physical button (the code below works with these "logical" bits)
static const unsigned LOGICAL[ACT_N] = { PSP_CTRL_CROSS, PSP_CTRL_CIRCLE, PSP_CTRL_SQUARE, PSP_CTRL_START,
                                         PSP_CTRL_LTRIGGER, PSP_CTRL_RTRIGGER, PSP_CTRL_TRIANGLE };
static const int vol_steps[4] = { 25, 50, 75, 100 };

#define N_(s) s   /* marks a string that is translated later, at the place where it is used */
static const char *TR(const char *ru) {
  if (cfg.lang == 0) return ru;
  for (unsigned i = 0; i < sizeof tr_tab / sizeof tr_tab[0]; i++)
    if (!strcmp(tr_tab[i][0], ru)) return tr_tab[i][1];
  return ru;
}
static const char *btn_name(unsigned m) {
  switch (m) {
    case PSP_CTRL_CROSS: return "X";
    case PSP_CTRL_CIRCLE: return "O";
    case PSP_CTRL_SQUARE: return TR("Квадрат");
    case PSP_CTRL_TRIANGLE: return TR("Треугольник");
    case PSP_CTRL_START: return "Start";
    case PSP_CTRL_LTRIGGER: return "L";
    case PSP_CTRL_RTRIGGER: return "R";
  }
  return "?";
}
#define KN(a) btn_name(cfg.keymap[a])

static void keys_default(void) { for (int a = 0; a < ACT_N; a++) cfg.keymap[a] = LOGICAL[a]; }
static void keys_japan(void) { keys_default(); cfg.keymap[ACT_OK] = PSP_CTRL_CIRCLE; cfg.keymap[ACT_BACK] = PSP_CTRL_CROSS; }
static int keys_preset(void) {
  int def = 1, jp = 1;
  for (int a = 0; a < ACT_N; a++) {
    unsigned j = a == ACT_OK ? PSP_CTRL_CIRCLE : a == ACT_BACK ? PSP_CTRL_CROSS : LOGICAL[a];
    if (cfg.keymap[a] != LOGICAL[a]) def = 0;
    if (cfg.keymap[a] != j) jp = 0;
  }
  return def ? 0 : jp ? 1 : 2;
}
static int keymap_ok(const uint32_t *k) {
  unsigned seen = 0;
  for (int a = 0; a < ACT_N; a++) {
    int found = 0;
    for (int b = 0; b < ACT_N; b++) if (k[a] == LOGICAL[b]) found = 1;
    if (!found || (seen & k[a])) return 0;
    seen |= k[a];
  }
  return 1;
}
static void cfg_defaults(void) {
  memset(&cfg, 0, sizeof cfg);
  cfg.magic = CFG_MAGIC; cfg.ver = CFG_VER;
  cfg.lang = 0; cfg.bg = 0; cfg.dim = 1; cfg.rep = 1; cfg.shuf = 0; cfg.startvol = 2;
  cfg.stick = 1; cfg.sens = 1; cfg.preview = 1; cfg.cpu = 1; cfg.hints = 1;
  keys_default();
}
static void cfg_save(void) {
  FILE *f = fopen(CFG_PATH, "wb");
  if (!f) return;
  fwrite(&cfg, 1, sizeof cfg, f);
  fclose(f);
}
static void cfg_load(void) {
  cfg_defaults();
  FILE *f = fopen(CFG_PATH, "rb");
  if (!f) return;
  Cfg t;
  if (fread(&t, 1, sizeof t, f) == sizeof t && t.magic == CFG_MAGIC && t.ver == CFG_VER) {
    t.bgpath[255] = 0;
    if (t.lang > 1) t.lang = 0;
    if (t.bg > 3) t.bg = 0;
    if (t.dim > 2) t.dim = 1;
    if (t.rep > 2) t.rep = 1;
    if (t.shuf > 1) t.shuf = 0;
    if (t.startvol > 3) t.startvol = 2;
    if (t.stick > 1) t.stick = 1;
    if (t.sens > 2) t.sens = 1;
    if (t.preview > 1) t.preview = 1;
    if (t.cpu > 1) t.cpu = 1;
    if (t.hints > 1) t.hints = 1;
    if (!keymap_ok(t.keymap)) for (int a = 0; a < ACT_N; a++) t.keymap[a] = LOGICAL[a];
    cfg = t;
  }
  fclose(f);
}
static void apply_cpu(void) {
  if (cfg.cpu) scePowerSetClockFrequency(333, 333, 166);
  else scePowerSetClockFrequency(222, 222, 111);
}

/* ================= exit callback ================= */
static int exit_cb(int a, int b, void *c) { sceKernelExitGame(); return 0; }
static int cb_thread(SceSize args, void *argp) {
  int id = sceKernelCreateCallback("exit", exit_cb, NULL);
  sceKernelRegisterExitCallback(id);
  sceKernelSleepThreadCB();
  return 0;
}
static void setup_cb(void) {
  int t = sceKernelCreateThread("cb", cb_thread, 0x11, 0xFA0, 0, NULL);
  if (t >= 0) sceKernelStartThread(t, 0, NULL);
}

/* ================= graphics ================= */
static inline uint32_t blend(uint32_t d, uint32_t s, int a) {
  if (a >= 255) return s | 0xFF000000u;
  if (a <= 0) return d;
  int dr = d & 255, dg = (d >> 8) & 255, db = (d >> 16) & 255;
  int sr = s & 255, sg = (s >> 8) & 255, sb = (s >> 16) & 255;
  dr += (sr - dr) * a / 255; dg += (sg - dg) * a / 255; db += (sb - db) * a / 255;
  return 0xFF000000u | ((unsigned)db << 16) | ((unsigned)dg << 8) | (unsigned)dr;
}
static void flip(int wait) {
  if (wait) sceDisplayWaitVblankStart();
  for (int y = 0; y < SH; y++) memcpy(vram + y * 512, fb + y * SW, SW * 4);
}
static void fill_rect(int x, int y, int w, int h, uint32_t c, int a) {
  int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > SW ? SW : x + w, y1 = y + h > SH ? SH : y + h;
  for (int j = y0; j < y1; j++) {
    uint32_t *d = fb + j * SW;
    for (int i = x0; i < x1; i++) d[i] = blend(d[i], c, a);
  }
}
static void fill_hgrad(int x, int y, int w, int h, uint32_t c, int a0, int a1) {
  for (int i = 0; i < w; i++) fill_rect(x + i, y, 1, h, c, a0 + (a1 - a0) * i / (w > 1 ? w - 1 : 1));
}
static void fill_vgrad(int x, int y, int w, int h, uint32_t c0, uint32_t c1) {
  for (int j = 0; j < h; j++) {
    int a = 255 * j / (h > 1 ? h - 1 : 1);
    fill_rect(x, y + j, w, 1, blend(c0, c1, a), 255);
  }
}
static void rect_border(int x, int y, int w, int h, uint32_t c, int a, int t) {
  fill_rect(x, y, w, t, c, a); fill_rect(x, y + h - t, w, t, c, a);
  fill_rect(x, y + t, t, h - 2 * t, c, a); fill_rect(x + w - t, y + t, t, h - 2 * t, c, a);
}
static void fill_circle(int cx, int cy, int r, uint32_t c, int a) {
  for (int j = -r; j <= r; j++)
    for (int i = -r; i <= r; i++)
      if (i * i + j * j <= r * r) {
        int px = cx + i, py = cy + j;
        if (px >= 0 && px < SW && py >= 0 && py < SH) fb[py * SW + px] = blend(fb[py * SW + px], c, a);
      }
}
static void fill_tri(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t c, int a) {
  int minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
  int maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
  int miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
  int maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
  for (int y = miny; y <= maxy; y++) {
    if (y < 0 || y >= SH) continue;
    for (int x = minx; x <= maxx; x++) {
      if (x < 0 || x >= SW) continue;
      int w0 = (x1 - x0) * (y - y0) - (y1 - y0) * (x - x0);
      int w1 = (x2 - x1) * (y - y1) - (y2 - y1) * (x - x1);
      int w2 = (x0 - x2) * (y - y2) - (y0 - y2) * (x - x2);
      if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0))
        fb[y * SW + x] = blend(fb[y * SW + x], c, a);
    }
  }
}

/* ---- text (UTF-8, bitmap fonts with Latin + Cyrillic) ---- */
static const char *u8next(const char *s, uint32_t *cp) {
  unsigned char c = (unsigned char)s[0];
  if (c < 0x80) { *cp = c; return s + 1; }
  if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) { *cp = ((c & 0x1F) << 6) | (s[1] & 0x3F); return s + 2; }
  if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
    *cp = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); return s + 3;
  }
  *cp = '?'; return s + 1;
}
static int u8put(char *o, uint32_t cp) {
  if (cp < 0x80) { o[0] = cp; return 1; }
  if (cp < 0x800) { o[0] = 0xC0 | (cp >> 6); o[1] = 0x80 | (cp & 0x3F); return 2; }
  o[0] = 0xE0 | (cp >> 12); o[1] = 0x80 | ((cp >> 6) & 0x3F); o[2] = 0x80 | (cp & 0x3F); return 3;
}
static const Glyph *gfind(const Font *f, uint32_t cp) {
  int lo = 0, hi = f->n - 1;
  while (lo <= hi) {
    int m = (lo + hi) / 2;
    if (f->g[m].cp == cp) return &f->g[m];
    if (f->g[m].cp < cp) lo = m + 1; else hi = m - 1;
  }
  return NULL;
}
static const Glyph *gget(const Font *f, uint32_t cp) {
  const Glyph *g = gfind(f, cp);
  return g ? g : gfind(f, '?');
}
static int text_width(const Font *f, const char *s) {
  int w = 0; uint32_t cp;
  while (*s) { s = u8next(s, &cp); const Glyph *g = gget(f, cp); if (g) w += g->adv; }
  return w;
}
static void draw_glyph(const Font *f, const Glyph *g, int x, int y, uint32_t c, int alpha) {
  const unsigned char *d = f->data + g->off;
  int bx = x + g->ox, by = y + f->asc + g->oy;
  for (int j = 0; j < g->h; j++) {
    int py = by + j;
    if (py < 0 || py >= SH) continue;
    for (int i = 0; i < g->w; i++) {
      int px = bx + i;
      if (px < 0 || px >= SW) continue;
      int a = d[j * g->w + i];
      if (a) { a = a * alpha / 255; fb[py * SW + px] = blend(fb[py * SW + px], c, a); }
    }
  }
}
static int draw_text(const Font *f, int x, int y, uint32_t c, const char *s, int maxw, int shadow) {
  int total = text_width(f, s);
  int limit = maxw > 0 ? maxw : 100000;
  int trunc = total > limit;
  const Glyph *eg = gfind(f, 0x2026);
  int ew = eg ? eg->adv : 0, cx = x; uint32_t cp;
  while (*s) {
    const char *n = u8next(s, &cp);
    const Glyph *g = gget(f, cp);
    int adv = g ? g->adv : 0;
    if (trunc && cx - x + adv > limit - ew) {
      if (eg) { if (shadow) draw_glyph(f, eg, cx + 1, y + 1, C_BLACK, 150); draw_glyph(f, eg, cx, y, c, 255); }
      cx += ew; break;
    }
    if (g && g->w) { if (shadow) draw_glyph(f, g, cx + 1, y + 1, C_BLACK, 150); draw_glyph(f, g, cx, y, c, 255); }
    cx += adv; s = n;
  }
  return cx - x;
}
static void draw_text_r(const Font *f, int xr, int y, uint32_t c, const char *s) {
  draw_text(f, xr - text_width(f, s), y, c, s, 0, 0);
}
static int draw_wrapped(const Font *f, int x, int y, int w, int maxlines, uint32_t c, const char *s) {
  char buf[128]; int bl = 0, cw = 0, lines = 0; uint32_t cp;
  while (*s && lines < maxlines) {
    const char *n = u8next(s, &cp);
    const Glyph *g = gget(f, cp);
    int adv = g ? g->adv : 0;
    if (cw + adv > w && bl > 0) {
      buf[bl] = 0; draw_text(f, x, y + lines * f->lineh, c, buf, 0, 1);
      lines++; bl = 0; cw = 0;
      if (lines >= maxlines) break;
    }
    if (bl + (int)(n - s) < 120) { memcpy(buf + bl, s, n - s); bl += n - s; }
    cw += adv; s = n;
  }
  if (bl > 0 && lines < maxlines) { buf[bl] = 0; draw_text(f, x, y + lines * f->lineh, c, buf, 0, 1); lines++; }
  return lines;
}

/* ---- icons ---- */
static uint32_t type_color(int t) {
  switch (t) {
    case T_DIR: return RGB(255, 213, 74);
    case T_AUDIO: return RGB(90, 240, 190);
    case T_VIDEO: return RGB(255, 120, 170);
    case T_IMAGE: return RGB(130, 190, 255);
  }
  return RGB(200, 200, 200);
}
static void draw_icon(int t, int x, int y) {
  uint32_t c = type_color(t);
  switch (t) {
    case T_DIR: fill_rect(x, y + 3, 12, 8, c, 255); fill_rect(x, y + 1, 5, 3, c, 255); break;
    case T_AUDIO: fill_circle(x + 3, y + 9, 3, c, 255); fill_rect(x + 5, y + 1, 2, 9, c, 255); fill_rect(x + 5, y + 1, 6, 3, c, 255); break;
    case T_VIDEO: rect_border(x, y + 1, 12, 10, c, 255, 1); fill_tri(x + 4, y + 3, x + 4, y + 9, x + 9, y + 6, c, 255); break;
    case T_IMAGE: rect_border(x, y + 1, 12, 10, c, 255, 1); fill_circle(x + 3, y + 4, 1, c, 255); fill_tri(x + 1, y + 10, x + 5, y + 5, x + 10, y + 10, c, 255); break;
    default: rect_border(x, y + 1, 10, 10, c, 255, 1);
  }
}
static void big_icon(int t, int cx, int cy, int s) {   // s ~ half size
  uint32_t c = C_WHITE;
  switch (t) {
    case T_VIDEO:
      fill_circle(cx, cy, s, c, 60);
      fill_tri(cx - s / 3, cy - s / 2, cx - s / 3, cy + s / 2, cx + s / 2, cy, c, 235);
      break;
    case T_AUDIO:
      fill_circle(cx - s / 3, cy + s / 2, s / 3 + 2, c, 235);
      fill_rect(cx - s / 3 + s / 3 - 1, cy - s * 2 / 3, 4, s * 7 / 6, c, 235);
      fill_rect(cx + 2, cy - s * 2 / 3, s / 2 + 2, s / 4, c, 235);
      break;
    case T_IMAGE:
      rect_border(cx - s, cy - s * 2 / 3, s * 2, s * 4 / 3, c, 235, 2);
      fill_circle(cx - s / 2, cy - s / 5, s / 6, c, 235);
      fill_tri(cx - s + 3, cy + s * 2 / 3 - 2, cx - s / 5, cy - s / 6, cx + s / 3, cy + s * 2 / 3 - 2, c, 235);
      fill_tri(cx, cy + s * 2 / 3 - 2, cx + s / 2, cy, cx + s - 3, cy + s * 2 / 3 - 2, c, 235);
      break;
    case T_DIR:
      fill_rect(cx - s, cy - s / 2, s * 2, s * 4 / 3 - s / 3, c, 235);
      fill_rect(cx - s, cy - s * 2 / 3, s, s / 4, c, 235);
      break;
    default:
      rect_border(cx - s / 2, cy - s * 2 / 3, s, s * 4 / 3, c, 235, 2);
  }
}
static void blit_fit(const uint32_t *src, int sw, int sh, int dx, int dy, int dw, int dh) {
  if (!src || sw <= 0 || sh <= 0) return;
  int w, h;
  if ((long)sw * dh > (long)sh * dw) { w = dw; h = sh * dw / sw; } else { h = dh; w = sw * dh / sh; }
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  int ox = dx + (dw - w) / 2, oy = dy + (dh - h) / 2;
  for (int y = 0; y < h; y++) {
    int py = oy + y; if (py < 0 || py >= SH) continue;
    const uint32_t *s = src + (size_t)(y * sh / h) * sw;
    for (int x = 0; x < w; x++) {
      int px = ox + x; if (px < 0 || px >= SW) continue;
      fb[py * SW + px] = s[x * sw / w] | 0xFF000000u;
    }
  }
}

/* ================= input ================= */
enum { ST_UP = 1, ST_DOWN = 2, ST_LEFT = 4, ST_RIGHT = 8 };
static unsigned stick_ev;    // analog stick "key presses" with auto-repeat (faster when pushed further)
static unsigned raw_press;   // physical buttons pressed this frame (before remapping)
static unsigned axis_event(int d, int *cnt, unsigned bit, int dead) {
  if (d <= dead) { *cnt = 0; return 0; }
  int range = 127 - dead, mag = d - dead;
  if (mag > range) mag = range;
  int c = (*cnt)++;
  if (c == 0) return bit;
  if (c < 14) return 0;
  int interval = 8 - mag * 6 / range;
  return ((c - 14) % interval == 0) ? bit : 0;
}
static unsigned read_pad(void) {
  static unsigned old; static int hold; static int sc[4];
  static const int dz[3] = { 70, 45, 28 };
  SceCtrlData pad; sceCtrlPeekBufferPositive(&pad, 1);
  if (cfg.stick) {
    int dx = (int)pad.Lx - 128, dy = (int)pad.Ly - 128, dd = dz[cfg.sens > 2 ? 1 : cfg.sens];
    stick_ev = axis_event(-dy, &sc[0], ST_UP, dd) | axis_event(dy, &sc[1], ST_DOWN, dd) |
               axis_event(-dx, &sc[2], ST_LEFT, dd) | axis_event(dx, &sc[3], ST_RIGHT, dd);
  } else { stick_ev = 0; sc[0] = sc[1] = sc[2] = sc[3] = 0; }
  unsigned b = pad.Buttons, n = b & ~old;
  unsigned dp = PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT;
  if (b & dp) {
    if (n & dp) hold = 0;
    else { hold++; if (hold > 20 && hold % 3 == 0) n |= b & dp; }
  } else hold = 0;
  old = b; held = b; raw_press = n;
  // remap: physical button -> logical action bit (the rest of the program only sees logical bits)
  unsigned mask = 0;
  for (int a = 0; a < ACT_N; a++) mask |= LOGICAL[a];
  unsigned lp = n & ~mask;
  for (int a = 0; a < ACT_N; a++) if (n & cfg.keymap[a]) lp |= LOGICAL[a];
  return lp;
}
static void set_msg(const char *m) { strncpy(msgbuf, m, 95); msgbuf[95] = 0; msgtimer = 150; }

/* ================= file types ================= */
static int has_ext(const char *name, const char *list) {
  const char *dot = strrchr(name, '.');
  if (!dot) return 0;
  dot++;
  size_t n = strlen(dot);
  if (n == 0 || n > 5) return 0;
  char e[8];
  for (size_t i = 0; i < n; i++) e[i] = tolower((unsigned char)dot[i]);
  e[n] = 0;
  const char *p = list;
  while (*p) {
    const char *c = strchr(p, ',');
    size_t l = c ? (size_t)(c - p) : strlen(p);
    if (l == n && !strncmp(p, e, n)) return 1;
    if (!c) break;
    p = c + 1;
  }
  return 0;
}
static int classify(const char *name) {
  if (has_ext(name, "mp3,wav,ogg,aac,m4a,flac,wma,at3,mid")) return T_AUDIO;
  if (has_ext(name, "mp4,m4v,avi,mkv,pmp,3gp,mov,flv,wmv,mpg,mpeg")) return T_VIDEO;
  if (has_ext(name, "jpg,jpeg,png,gif,bmp")) return T_IMAGE;
  return T_OTHER;
}
static int is_mp3(const char *p) { return has_ext(p, "mp3"); }
static int is_jpeg(const char *p) { return has_ext(p, "jpg,jpeg"); }
static int is_bmp(const char *p) { return has_ext(p, "bmp"); }
static int is_viewable(const char *p) { return has_ext(p, "jpg,jpeg,png,bmp"); }
#ifdef HAVE_FFMPEG
static int is_video_playable(const char *p) { return has_ext(p, "mp4,m4v,avi,mkv,mov,3gp,flv,wmv,mpg,mpeg"); }
#else
static int is_avi(const char *p) { return has_ext(p, "avi"); }
static int is_video_playable(const char *p) { return is_avi(p); }
#endif
static const char *base(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }
static const char *type_upper(int t) {
  return t == T_DIR ? TR("ПАПКА") : t == T_AUDIO ? TR("МУЗЫКА") : t == T_VIDEO ? TR("ВИДЕО") : t == T_IMAGE ? TR("ФОТО") : TR("ФАЙЛ");
}
static void fmt_size(char *o, uint32_t b) {
  if (b < 1024 * 1024) snprintf(o, 24, TR("%u КБ"), (unsigned)((b + 1023) / 1024));
  else snprintf(o, 24, TR("%u.%u МБ"), (unsigned)(b / 1048576), (unsigned)((b % 1048576) * 10 / 1048576));
}

/* ================= scanning ================= */
static void splash(const char *l1, const char *l2) {
  memcpy(fb, bgimg[0], sizeof fb);
  fill_rect(0, 0, SW, SH, C_DARK, 150);
  draw_text(&FONT_L, 150, 96, C_TITLE, "PSP media", 0, 1);
  draw_text(&FONT_M, 150, 130, C_WHITE, l1, 310, 1);
  draw_text(&FONT_S, 150, 152, C_GRAY, l2, 310, 1);
  flip(0);
}
static void join(char *out, size_t n, const char *dir, const char *name) {
  size_t l = strlen(dir);
  snprintf(out, n, "%s%s%s", dir, (l && dir[l - 1] == '/') ? "" : "/", name);
}
static void parent_of(const char *in, char *out) {
  strncpy(out, in, 255); out[255] = 0;
  char *s = strrchr(out, '/');
  if (s) *s = 0;
  if (!strchr(out, '/')) strcat(out, "/");
}
static int is_skipped(const char *p) {
  return !strcasecmp(p, "ms0:/PSP/GAME") || !strcasecmp(p, "ms0:/PSP/SAVEDATA") ||
         !strcasecmp(p, "ms0:/PSP/SYSTEM") || !strcasecmp(p, "ms0:/PSP/PLUGINS");
}
static int alias_psp;
static int dir_exists(const char *p) { SceUID d = sceIoDopen(p); if (d < 0) return 0; sceIoDclose(d); return 1; }
static uint32_t clamp32(long long v) { return v < 0 ? 0 : v > 0xFFFFFFFFLL ? 0xFFFFFFFFu : (uint32_t)v; }
static void scan_dir(const char *dir, int depth) {
  static int tick;
  if (nall >= MAX_ALL || depth > 10) return;
  SceUID fd = sceIoDopen(dir);
  if (fd < 0) return;
  if ((tick++ & 7) == 0) {
    char b[48]; snprintf(b, sizeof b, TR("Найдено файлов: %d"), nall);
    splash(TR("Сканирую карту памяти..."), b);
  }
  SceIoDirent d;
  while (nall < MAX_ALL) {
    memset(&d, 0, sizeof d);
    if (sceIoDread(fd, &d) <= 0) break;
    if (!strcmp(d.d_name, ".") || !strcmp(d.d_name, "..")) continue;
    char p[256]; join(p, sizeof p, dir, d.d_name);
    if (FIO_S_ISDIR(d.d_stat.st_mode)) {
      if (depth == 0 && alias_psp && !strcasecmp(d.d_name, "PSP")) continue;
      if (!is_skipped(p)) scan_dir(p, depth + 1);
    } else {
      int t = classify(d.d_name);
      if (t != T_OTHER) {
        Entry *e = &all[nall++];
        strncpy(e->path, p, 255); e->path[255] = 0;
        strncpy(e->disp, d.d_name, 63); e->disp[63] = 0;
        e->type = t; e->size = clamp32(d.d_stat.st_size);
      }
    }
  }
  sceIoDclose(fd);
}
static int pcmp(const void *a, const void *b) {
  return strcasecmp(((const Entry *)a)->path, ((const Entry *)b)->path);
}
static void scan_all(void) {
  nall = 0;
  alias_psp = dir_exists("ms0:/GAME") && dir_exists("ms0:/PSP/GAME");
  splash(TR("Сканирую карту памяти..."), "");
  scan_dir("ms0:/", 0);
  qsort(all, nall, sizeof(Entry), pcmp);
  memset(cnt, 0, sizeof cnt);
  for (int i = 0; i < nall; i++) cnt[all[i].type]++;
}

/* ================= views ================= */
static void view_clamp(View *v, int rows) {
  if (v->cursor >= v->n) v->cursor = v->n - 1;
  if (v->cursor < 0) v->cursor = 0;
  if (v->cursor < v->top) v->top = v->cursor;
  if (v->cursor >= v->top + rows) v->top = v->cursor - rows + 1;
  if (v->top < 0) v->top = 0;
}
static void build_cat(int type) {
  vcat.n = 0; vcat.cursor = 0; vcat.top = 0;
  for (int i = 0; i < nall; i++) if (all[i].type == type) vcat.it[vcat.n++] = &all[i];
}
static int dcmp(const void *a, const void *b) {
  const Entry *x = a, *y = b;
  if (!strcmp(x->disp, "..")) return -1;
  if (!strcmp(y->disp, "..")) return 1;
  int xd = x->type == T_DIR, yd = y->type == T_DIR;
  if (xd != yd) return yd - xd;
  return strcasecmp(x->disp, y->disp);
}
static void load_dir(const char *dir) {
  char keep[256]; strncpy(keep, dir, 255); keep[255] = 0;
  strcpy(curdir, keep);
  ndir = 0;
  if (strcmp(curdir, "ms0:/")) {
    Entry *e = &dirbuf[ndir++];
    parent_of(curdir, e->path);
    strcpy(e->disp, ".."); e->type = T_DIR; e->size = 0;
  }
  SceUID fd = sceIoDopen(curdir);
  if (fd >= 0) {
    SceIoDirent d;
    while (ndir < MAX_DIR) {
      memset(&d, 0, sizeof d);
      if (sceIoDread(fd, &d) <= 0) break;
      if (!strcmp(d.d_name, ".") || !strcmp(d.d_name, "..")) continue;
      Entry *e = &dirbuf[ndir++];
      join(e->path, sizeof e->path, curdir, d.d_name);
      strncpy(e->disp, d.d_name, 63); e->disp[63] = 0;
      int isd = FIO_S_ISDIR(d.d_stat.st_mode);
      e->type = isd ? T_DIR : classify(d.d_name);
      e->size = isd ? 0 : clamp32(d.d_stat.st_size);
    }
    sceIoDclose(fd);
  }
  qsort(dirbuf, ndir, sizeof(Entry), dcmp);
  vmem.n = 0; vmem.cursor = 0; vmem.top = 0;
  for (int i = 0; i < ndir; i++) vmem.it[vmem.n++] = &dirbuf[i];
}

/* ---- search (case-insensitive, Latin + Cyrillic) ---- */
static uint16_t charset[96]; static int ncs;
static uint32_t qcp[32]; static int qn; static char qstr[128];
static uint32_t fold(uint32_t c) {
  if (c >= 'A' && c <= 'Z') return c + 32;
  if (c >= 0x410 && c <= 0x42F) return c + 32;
  if (c == 0x401) return 0x451;
  return c;
}
static int decode_fold(const char *s, uint32_t *out, int max) {
  int n = 0; uint32_t cp;
  while (*s && n < max) { s = u8next(s, &cp); out[n++] = fold(cp); }
  return n;
}
static int match_name(const char *name) {
  uint32_t t[96];
  int tn = decode_fold(name, t, 96);
  for (int i = 0; i + qn <= tn; i++) {
    int j = 0;
    while (j < qn && t[i + j] == qcp[j]) j++;
    if (j == qn) return 1;
  }
  return 0;
}
static void qstr_update(void) {
  int l = 0;
  for (int i = 0; i < qn; i++) l += u8put(qstr + l, qcp[i]);
  qstr[l] = 0;
}
static void run_search(void) {
  vsrch.n = 0; vsrch.cursor = 0; vsrch.top = 0;
  if (qn == 0) return;
  for (int i = 0; i < nall; i++) if (match_name(all[i].disp)) vsrch.it[vsrch.n++] = &all[i];
}
static void init_charset(void) {
  ncs = 0;
  for (int c = 'a'; c <= 'z'; c++) charset[ncs++] = c;
  for (int c = '0'; c <= '9'; c++) charset[ncs++] = c;
  charset[ncs++] = ' '; charset[ncs++] = '.'; charset[ncs++] = '_'; charset[ncs++] = '-';
  for (int c = 0x430; c <= 0x44F; c++) charset[ncs++] = c;
  charset[ncs++] = 0x451;
}

/* ================= audio (MP3 via libmad, own thread) ================= */
// Output goes through a normal 44.1 kHz stereo channel; other sample rates are
// resampled in software (works the same on real PSP and in PPSSPP).
static volatile int a_req, a_stop, a_pause, a_playing, a_finished, a_failed, a_secs, a_dur, a_err;
static volatile int a_vol = 0x6000;
static char a_path[256];
static char qpath[MAX_ALL][256];
static int nq, qpos, fail_run;
static short outbuf[1152 * 2] __attribute__((aligned(64)));
static unsigned char inbuf[16384 + MAD_BUFFER_GUARD];
static struct mad_stream st; static struct mad_frame fr; static struct mad_synth sy;

static inline short scale(mad_fixed_t s) {
  s += (1L << (MAD_F_FRACBITS - 16));
  if (s >= MAD_F_ONE) s = MAD_F_ONE - 1; else if (s < -MAD_F_ONE) s = -MAD_F_ONE;
  return s >> (MAD_F_FRACBITS + 1 - 16);
}

// returns 1 if at least one block of sound was really played
static int decode_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) { a_err = -1; return 0; }
  fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
  int ch = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, 1152, PSP_AUDIO_FORMAT_STEREO);
  if (ch < 0) { a_err = ch; fclose(f); return 0; }
  mad_stream_init(&st); mad_frame_init(&fr); mad_synth_init(&sy);
  int ofill = 0, outs = 0, quit = 0, first = 1;
  unsigned long total = 0, rate = 44100;
  long long step = 65536, acc = 0;
  short pl = 0, pr = 0;
  while (!a_stop && !quit) {
    if (st.buffer == NULL || st.error == MAD_ERROR_BUFLEN) {
      size_t rem = 0;
      if (st.next_frame) { rem = st.bufend - st.next_frame; memmove(inbuf, st.next_frame, rem); }
      size_t rd = fread(inbuf + rem, 1, 16384 - rem, f);
      if (rd == 0) break;
      mad_stream_buffer(&st, inbuf, rem + rd);
      st.error = MAD_ERROR_NONE;
    }
    if (mad_frame_decode(&fr, &st)) {
      if (MAD_RECOVERABLE(st.error) || st.error == MAD_ERROR_BUFLEN) continue;
      break;
    }
    mad_synth_frame(&sy, &fr);
    if (first) {
      first = 0;
      rate = fr.header.samplerate;
      if (rate < 8000 || rate > 48000) rate = 44100;
      step = ((long long)rate << 16) / 44100;
      if (fr.header.bitrate > 0) a_dur = (int)((long long)fsz * 8 / fr.header.bitrate);
    }
    for (unsigned i = 0; i < sy.pcm.length && !quit; i++) {
      short l = scale(sy.pcm.samples[0][i]);
      short r = sy.pcm.channels > 1 ? scale(sy.pcm.samples[1][i]) : l;
      while (acc < 65536 && !quit) {
        outbuf[ofill * 2] = pl + (short)(((long long)(l - pl) * acc) >> 16);
        outbuf[ofill * 2 + 1] = pr + (short)(((long long)(r - pr) * acc) >> 16);
        ofill++;
        if (ofill == 1152) {
          int rc = sceAudioOutputBlocking(ch, a_vol, outbuf);
          ofill = 0;
          if (rc < 0) { a_err = rc; quit = 1; } else outs++;
          while (a_pause && !a_stop) sceKernelDelayThread(50000);
          if (a_stop) quit = 1;
        }
        acc += step;
      }
      acc -= 65536; pl = l; pr = r; total++;
    }
    a_secs = (int)(total / rate);
  }
  if (!a_stop && !quit && ofill > 0 && outs > 0) {
    memset(outbuf + ofill * 2, 0, (1152 - ofill) * 4);
    sceAudioOutputBlocking(ch, a_vol, outbuf);
  }
  sceAudioChRelease(ch);
  mad_synth_finish(&sy); mad_frame_finish(&fr); mad_stream_finish(&st);
  fclose(f);
  if (outs == 0 && a_err == 0) a_err = -2;
  return outs > 0;
}
static int audio_thread(SceSize args, void *argp) {
  for (;;) {
    if (a_req) {
      a_req = 0; a_stop = 0; a_finished = 0; a_failed = 0; a_secs = 0; a_dur = 0; a_err = 0; a_playing = 1;
      int ok = decode_file(a_path);
      a_playing = 0;
      if (!a_stop) { if (ok) a_finished = 1; else a_failed = 1; }
    }
    sceKernelDelayThread(20000);
  }
  return 0;
}
static void play_path(const char *p) {
  a_stop = 1;
  for (int t = 0; a_playing && t < 100; t++) sceKernelDelayThread(10000);
  strncpy(a_path, p, 255); a_path[255] = 0;
  a_pause = 0; a_req = 1;
}
static void start_queue(View *v) {
  nq = 0; int sel = -1;
  for (int i = 0; i < v->n; i++) {
    Entry *e = v->it[i];
    if (e->type == T_AUDIO && is_mp3(e->path)) {
      if (i == v->cursor) sel = nq;
      strncpy(qpath[nq], e->path, 255); qpath[nq][255] = 0; nq++;
    }
  }
  if (sel < 0) { nq = 0; return; }
  fail_run = 0;
  qpos = sel; play_path(qpath[qpos]);
}
static void next_track(int dir) {
  if (nq == 0) return;
  if (cfg.shuf && nq > 1) { int r; do r = rand() % nq; while (r == qpos); qpos = r; }
  else qpos = (qpos + dir + nq) % nq;
  play_path(qpath[qpos]);
}
static void on_track_finished(void) {   // called when a track ended by itself
  fail_run = 0;
  if (nq == 0) return;
  if (cfg.rep == 2) { play_path(qpath[qpos]); return; }                   // repeat one
  if (cfg.rep == 0 && !cfg.shuf && qpos == nq - 1) return;                // repeat off: stop after the last
  next_track(1);
}
static void vol_change(int d) {
  int v = a_vol + d;
  if (v < 0) v = 0;
  if (v > 0x8000) v = 0x8000;
  a_vol = v;
}

/* ================= images ================= */
struct jerr { struct jpeg_error_mgr pub; jmp_buf jb; };
static void jexit(j_common_ptr c) { longjmp(((struct jerr *)c->err)->jb, 1); }
static void ms_init(j_decompress_ptr c) {}
static boolean ms_fill(j_decompress_ptr c) {
  static const unsigned char eoi[2] = { 0xFF, 0xD9 };
  c->src->next_input_byte = eoi; c->src->bytes_in_buffer = 2; return TRUE;
}
static void ms_skip(j_decompress_ptr c, long n) {
  if (n > 0) {
    if ((size_t)n > c->src->bytes_in_buffer) n = c->src->bytes_in_buffer;
    c->src->next_input_byte += n; c->src->bytes_in_buffer -= n;
  }
}
static void ms_term(j_decompress_ptr c) {}

// decode JPEG from a FILE or from memory; scales down by 1/2,1/4,1/8 while still >= tw x th
static uint32_t *jpeg_decode(FILE *f, const unsigned char *mem, size_t mlen, int *ow, int *oh, int tw, int th, long maxpix) {
  struct jpeg_decompress_struct ci; struct jerr je; struct jpeg_source_mgr sm;
  uint32_t *volatile pix = NULL; uint8_t *volatile row = NULL;
  ci.err = jpeg_std_error(&je.pub); je.pub.error_exit = jexit;
  if (setjmp(je.jb)) {
    jpeg_destroy_decompress(&ci);
    if (pix) free(pix);
    if (row) free(row);
    return NULL;
  }
  jpeg_create_decompress(&ci);
  if (f) jpeg_stdio_src(&ci, f);
  else {
    sm.init_source = ms_init; sm.fill_input_buffer = ms_fill; sm.skip_input_data = ms_skip;
    sm.resync_to_restart = jpeg_resync_to_restart; sm.term_source = ms_term;
    sm.next_input_byte = mem; sm.bytes_in_buffer = mlen; ci.src = &sm;
  }
  jpeg_read_header(&ci, TRUE);
  int d = 1;
  while (d < 8 && (int)ci.image_width / (d * 2) >= tw && (int)ci.image_height / (d * 2) >= th) d *= 2;
  ci.scale_num = 1; ci.scale_denom = d; ci.out_color_space = JCS_RGB;
  jpeg_start_decompress(&ci);
  int w = ci.output_width, h = ci.output_height;
  if ((long)w * h > maxpix || ci.output_components != 3) { jpeg_destroy_decompress(&ci); return NULL; }
  pix = malloc((size_t)w * h * 4); row = malloc(w * 3);
  if (!pix || !row) { jpeg_destroy_decompress(&ci); if (pix) free(pix); if (row) free(row); return NULL; }
  while ((int)ci.output_scanline < h) {
    uint8_t *r = row;
    int y = ci.output_scanline;
    jpeg_read_scanlines(&ci, &r, 1);
    uint32_t *o = pix + (size_t)y * w;
    for (int x = 0; x < w; x++)
      o[x] = 0xFF000000u | (row[x * 3 + 2] << 16) | (row[x * 3 + 1] << 8) | row[x * 3];
  }
  jpeg_finish_decompress(&ci); jpeg_destroy_decompress(&ci);
  free(row);
  *ow = w; *oh = h;
  return (uint32_t *)pix;
}
static uint32_t *png_decode(FILE *f, int *ow, int *oh) {
  unsigned char sig[8];
  if (fread(sig, 1, 8, f) != 8 || png_sig_cmp(sig, 0, 8)) return NULL;
  png_structp ps = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  png_infop pi = png_create_info_struct(ps);
  uint32_t *volatile pix = NULL; png_bytep *volatile rows = NULL;
  if (setjmp(png_jmpbuf(ps))) {
    png_destroy_read_struct(&ps, &pi, NULL);
    if (pix) free(pix);
    if (rows) free(rows);
    return NULL;
  }
  png_init_io(ps, f); png_set_sig_bytes(ps, 8); png_read_info(ps, pi);
  png_uint_32 w = png_get_image_width(ps, pi), h = png_get_image_height(ps, pi);
  if ((unsigned long)w * h > 3000000UL) png_error(ps, "too big");
  int ct = png_get_color_type(ps, pi), bd = png_get_bit_depth(ps, pi);
  if (bd == 16) png_set_strip_16(ps);
  if (ct == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(ps);
  if (ct == PNG_COLOR_TYPE_GRAY && bd < 8) png_set_expand_gray_1_2_4_to_8(ps);
  if (png_get_valid(ps, pi, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(ps);
  if (ct == PNG_COLOR_TYPE_GRAY || ct == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(ps);
  png_set_filler(ps, 0xFF, PNG_FILLER_AFTER);
  png_set_interlace_handling(ps);
  png_read_update_info(ps, pi);
  pix = malloc((size_t)w * h * 4); rows = malloc(h * sizeof(png_bytep));
  if (!pix || !rows) png_error(ps, "mem");
  for (png_uint_32 y = 0; y < h; y++) rows[y] = (png_bytep)(pix + (size_t)y * w);
  png_read_image(ps, rows);
  free(rows); rows = NULL;
  png_destroy_read_struct(&ps, &pi, NULL);
  *ow = w; *oh = h;
  return (uint32_t *)pix;
}
/* Small uncompressed BMP decoder (24/32-bit BI_RGB). PSP-friendly and bounded. */
static unsigned rd16le(FILE *f, int *ok) {
  int a = fgetc(f), b = fgetc(f);
  if (a < 0 || b < 0) { *ok = 0; return 0; }
  return (unsigned)a | ((unsigned)b << 8);
}
static unsigned rd32le(FILE *f, int *ok) {
  unsigned a = rd16le(f, ok), b = rd16le(f, ok);
  return a | (b << 16);
}
static uint32_t *bmp_decode(FILE *f, int *ow, int *oh) {
  int ok = 1;
  if (fgetc(f) != 'B' || fgetc(f) != 'M') return NULL;
  (void)rd32le(f, &ok); (void)rd16le(f, &ok); (void)rd16le(f, &ok);
  unsigned off = rd32le(f, &ok), dib = rd32le(f, &ok);
  if (!ok || dib < 40 || dib > 124) return NULL;
  int32_t wi = (int32_t)rd32le(f, &ok), hi = (int32_t)rd32le(f, &ok);
  unsigned planes = rd16le(f, &ok), bpp = rd16le(f, &ok);
  unsigned comp = rd32le(f, &ok);
  if (!ok || wi <= 0 || hi == 0 || hi == INT32_MIN || planes != 1 ||
      (bpp != 24 && bpp != 32) || comp != 0) return NULL;
  int topdown = hi < 0, w = wi, h = hi < 0 ? -hi : hi;
  if ((unsigned long)w * (unsigned long)h > 3000000UL) return NULL;
  long rowbytes = ((long)w * (bpp / 8) + 3) & ~3L;
  unsigned char *row = (unsigned char *)malloc((size_t)rowbytes);
  uint32_t *pix = (uint32_t *)malloc((size_t)w * h * 4);
  if (!row || !pix || fseek(f, (long)off, SEEK_SET) != 0) { free(row); free(pix); return NULL; }
  for (int sy = 0; sy < h; sy++) {
    if (fread(row, 1, (size_t)rowbytes, f) != (size_t)rowbytes) { free(row); free(pix); return NULL; }
    int y = topdown ? sy : h - 1 - sy;
    for (int x = 0; x < w; x++) {
      unsigned char *q = row + x * (bpp / 8);
      pix[(size_t)y * w + x] = RGB(q[2], q[1], q[0]);
    }
  }
  free(row); *ow = w; *oh = h; return pix;
}
static uint32_t *load_photo(const char *path, int *w, int *h) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  uint32_t *r = is_jpeg(path) ? jpeg_decode(f, NULL, 0, w, h, SW, SH, 3000000) :
                is_bmp(path) ? bmp_decode(f, w, h) : png_decode(f, w, h);
  fclose(f);
  return r;
}
static uint32_t *load_thumb(const char *path, int *w, int *h) {
  if (!is_jpeg(path)) return NULL;
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  uint32_t *r = jpeg_decode(f, NULL, 0, w, h, 150, 84, 1000000);
  fclose(f);
  return r;
}

/* ---- photo viewer ---- */
static int next_img(View *v, int pos, int dir) {
  for (int i = pos + dir; i >= 0 && i < v->n; i += dir)
    if (v->it[i]->type == T_IMAGE && is_viewable(v->it[i]->path)) return i;
  return -1;
}
static void photo_viewer(View *v, int pos) {
  int show = 1, reload = 1, w = 0, h = 0;
  uint32_t *im = NULL;
  for (;;) {
    Entry *e = v->it[pos];
    if (reload) {
      if (im) free(im);
      splash(TR("Загрузка фото..."), e->disp);
      im = load_photo(e->path, &w, &h); reload = 0;
    }
    for (int i = 0; i < SW * SH; i++) fb[i] = C_BLACK;
    if (im) blit_fit(im, w, h, 0, 0, SW, SH);
    else draw_text(&FONT_M, 90, 120, C_YELLOW, TR("Не удалось открыть (слишком большое или битое)"), 0, 1);
    if (show) {
      char b[32]; snprintf(b, sizeof b, "%d / %d", pos + 1, v->n);
      fill_rect(0, SH - 40, SW, 40, C_DARK, 175);
      draw_text(&FONT_M, 10, SH - 37, C_WHITE, e->disp, 380, 0);
      draw_text_r(&FONT_S, SW - 10, SH - 36, C_GRAY, b);
      char hb[128];
      snprintf(hb, sizeof hb, TR("%s/%s — листать   %s — скрыть подпись   %s — назад"), KN(ACT_PREV), KN(ACT_NEXT), KN(ACT_ALT), KN(ACT_BACK));
      draw_text(&FONT_S, 10, SH - 19, C_GRAY, hb, 0, 0);
    }
    flip(1);
    unsigned p = read_pad();
    if (a_finished) { a_finished = 0; on_track_finished(); }
    if (stick_ev & ST_RIGHT) p |= PSP_CTRL_RTRIGGER;
    if (stick_ev & ST_LEFT) p |= PSP_CTRL_LTRIGGER;
    if (p & PSP_CTRL_CIRCLE) break;
    if (p & PSP_CTRL_RTRIGGER) { int n = next_img(v, pos, 1); if (n >= 0) { pos = n; reload = 1; } }
    if (p & PSP_CTRL_LTRIGGER) { int n = next_img(v, pos, -1); if (n >= 0) { pos = n; reload = 1; } }
    if (p & PSP_CTRL_TRIANGLE) show = !show;
  }
  if (im) free(im);
}


#ifndef HAVE_FFMPEG
/* ================= video (AVI: MJPEG + 16-bit PCM) ================= */
static uint32_t avi_u32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t avi_u16(const unsigned char *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void video_player_avi(const char *path) {
  /* stop background music so we don't share the audio channel */
  a_stop = 1; nq = 0;
  for (int t = 0; a_playing && t < 100; t++) sceKernelDelayThread(10000);

  FILE *f = fopen(path, "rb");
  if (!f) { set_msg(TR("Не удалось открыть видео")); return; }

  unsigned char hdr[12];
  if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "AVI ", 4)) {
    fclose(f); set_msg(TR("Поддерживается только AVI (MJPEG)")); return;
  }

  uint32_t us_per_frame = 41667; /* default ~24 fps */
  int aud_ok = 0, aud_ch = 2, aud_rate = 44100, aud_bits = 16;
  long movi_pos = -1, movi_end = 0;
  int in_aud_strl = 0;

  while (1) {
    unsigned char ch[8];
    if (fread(ch, 1, 8, f) != 8) break;
    uint32_t sz = avi_u32(ch + 4);
    long next = ftell(f) + (long)((sz + 1u) & ~1u);

    if (!memcmp(ch, "LIST", 4) && sz >= 4) {
      unsigned char lt[4];
      if (fread(lt, 1, 4, f) != 4) break;
      if (!memcmp(lt, "movi", 4)) { movi_pos = ftell(f); movi_end = next; break; }
      if (!memcmp(lt, "hdrl", 4) || !memcmp(lt, "strl", 4)) {
        if (!memcmp(lt, "strl", 4)) in_aud_strl = 0;
        continue;
      }
    } else if (!memcmp(ch, "avih", 4) && sz >= 4) {
      unsigned char b[4];
      if (fread(b, 1, 4, f) == 4) {
        uint32_t u = avi_u32(b);
        if (u >= 10000 && u <= 200000) us_per_frame = u;
      }
    } else if (!memcmp(ch, "strh", 4) && sz >= 4) {
      unsigned char b[4];
      if (fread(b, 1, 4, f) == 4 && !memcmp(b, "auds", 4)) in_aud_strl = 1;
    } else if (!memcmp(ch, "strf", 4) && in_aud_strl && sz >= 16) {
      unsigned char b[16];
      if (fread(b, 1, 16, f) == 16) {
        if (avi_u16(b) == 1) { /* WAVE_FORMAT_PCM */
          aud_ch = avi_u16(b + 2);
          aud_rate = (int)avi_u32(b + 4);
          aud_bits = avi_u16(b + 14);
          if ((aud_ch == 1 || aud_ch == 2) && aud_bits == 16 && aud_rate >= 8000 && aud_rate <= 48000)
            aud_ok = 1;
        }
      }
      in_aud_strl = 0;
    }
    if (fseek(f, next, SEEK_SET) != 0) break;
  }

  if (movi_pos < 0) { fclose(f); set_msg(TR("Повреждённый AVI файл")); return; }

  int psp_ch = -1;
  if (aud_ok) psp_ch = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, 1152, PSP_AUDIO_FORMAT_STEREO);
  if (psp_ch < 0) aud_ok = 0;

  static short v_abuf[1152 * 2] __attribute__((aligned(64)));
  int afill = 0;
  long long astep = ((long long)aud_rate << 16) / 44100, aacc = 0;
  short apl = 0, apr = 0;

  unsigned char *buf = NULL;
  size_t bcap = 0;
  int paused = 0, show_hud = 1, frames = 0;
  uint64_t next_t = sceKernelGetSystemTimeWide();

  fseek(f, movi_pos, SEEK_SET);
  while (ftell(f) < movi_end) {
    unsigned p = read_pad();
    if (p & PSP_CTRL_CIRCLE) break;
    if (p & PSP_CTRL_SQUARE) { paused = !paused; next_t = sceKernelGetSystemTimeWide(); }
    if (p & PSP_CTRL_TRIANGLE) show_hud = !show_hud;
    if (p & PSP_CTRL_UP) vol_change(0x800);
    if (p & PSP_CTRL_DOWN) vol_change(-0x800);

    if (paused) { sceDisplayWaitVblankStart(); continue; }

    unsigned char ch[8];
    if (fread(ch, 1, 8, f) != 8) break;
    uint32_t sz = avi_u32(ch + 4);
    uint32_t pad = sz & 1u;

    if (!memcmp(ch, "LIST", 4)) { fseek(f, 4, SEEK_CUR); continue; }

    int is_vid = (ch[2] == 'd' && (ch[3] == 'c' || ch[3] == 'b'));
    int is_aud = (ch[2] == 'w' && ch[3] == 'b' && aud_ok);

    if ((is_vid || is_aud) && sz > 0 && sz < 2 * 1024 * 1024) {
      if (sz > bcap) {
        unsigned char *nb = realloc(buf, sz);
        if (!nb) break;
        buf = nb; bcap = sz;
      }
      if (fread(buf, 1, sz, f) != sz) break;
      if (pad) fseek(f, 1, SEEK_CUR);

      if (is_vid) {
        int w = 0, h = 0;
        uint32_t *im = jpeg_decode(NULL, buf, sz, &w, &h, SW, SH, 1000000);
        if (im) {
          for (int i = 0; i < SW * SH; i++) fb[i] = C_BLACK;
          blit_fit(im, w, h, 0, 0, SW, SH);
          free(im);
          if (show_hud) {
            int sec = (int)((uint64_t)frames * us_per_frame / 1000000ULL);
            char tb[32]; snprintf(tb, sizeof tb, "%02d:%02d", sec / 60, sec % 60);
            fill_rect(0, SH - 24, SW, 24, C_DARK, 175);
            draw_text(&FONT_S, 10, SH - 20, C_WHITE, base(path), 360, 0);
            draw_text_r(&FONT_S, SW - 10, SH - 20, C_GRAY, tb);
          }
          uint64_t now = sceKernelGetSystemTimeWide();
          if (now < next_t) sceKernelDelayThread((SceUInt)(next_t - now));
          next_t = sceKernelGetSystemTimeWide() + us_per_frame;
          flip(0);
          frames++;
        }
      } else if (is_aud) {
        int step_b = aud_ch * 2;
        int nsamp = (int)(sz / step_b);
        for (int i = 0; i < nsamp; i++) {
          short l = (short)avi_u16(buf + i * step_b);
          short r = aud_ch == 2 ? (short)avi_u16(buf + i * step_b + 2) : l;
          while (aacc < 65536) {
            v_abuf[afill * 2] = apl + (short)(((long long)(l - apl) * aacc) >> 16);
            v_abuf[afill * 2 + 1] = apr + (short)(((long long)(r - apr) * aacc) >> 16);
            afill++;
            if (afill == 1152) { sceAudioOutputBlocking(psp_ch, a_vol, v_abuf); afill = 0; }
            aacc += astep;
          }
          aacc -= 65536; apl = l; apr = r;
        }
      }
    } else {
      fseek(f, (long)(sz + pad), SEEK_CUR);
    }
  }

  if (buf) free(buf);
  if (psp_ch >= 0) sceAudioChRelease(psp_ch);
  fclose(f);
}
#endif

#ifdef HAVE_FFMPEG
/* ================= video (MP4 / MKV / AVI via FFmpeg) ================= */
static void video_player_ffmpeg(const char *path) {
  a_stop = 1; nq = 0;
  for (int t = 0; a_playing && t < 100; t++) sceKernelDelayThread(10000);

  splash(TR("Загрузка видео..."), base(path));

  AVFormatContext *fmt = NULL;
  if (avformat_open_input(&fmt, path, NULL, NULL) < 0) {
    set_msg(TR("Не удалось открыть видео")); return;
  }
  if (avformat_find_stream_info(fmt, NULL) < 0) {
    avformat_close_input(&fmt); set_msg(TR("Не удалось открыть видео")); return;
  }

  int vidx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  int aidx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
  if (vidx < 0) {
    avformat_close_input(&fmt); set_msg(TR("Видеопоток не найден")); return;
  }

  const AVCodec *vc = avcodec_find_decoder(fmt->streams[vidx]->codecpar->codec_id);
  AVCodecContext *vctx = vc ? avcodec_alloc_context3(vc) : NULL;
  if (!vctx || avcodec_parameters_to_context(vctx, fmt->streams[vidx]->codecpar) < 0) {
    if (vctx) avcodec_free_context(&vctx);
    avformat_close_input(&fmt); set_msg(TR("Неподдерживаемый видеокодек")); return;
  }
  vctx->skip_loop_filter = AVDISCARD_ALL;
  vctx->flags2 |= AV_CODEC_FLAG2_FAST;
  if (avcodec_open2(vctx, vc, NULL) < 0) {
    avcodec_free_context(&vctx); avformat_close_input(&fmt);
    set_msg(TR("Неподдерживаемый видеокодек")); return;
  }

  AVCodecContext *actx = NULL;
  struct SwrContext *swr = NULL;
  int psp_ch = -1;
  if (aidx >= 0) {
    const AVCodec *ac = avcodec_find_decoder(fmt->streams[aidx]->codecpar->codec_id);
    if (ac) {
      actx = avcodec_alloc_context3(ac);
      if (actx && avcodec_parameters_to_context(actx, fmt->streams[aidx]->codecpar) >= 0 &&
          avcodec_open2(actx, ac, NULL) >= 0) {
#if LIBSWRESAMPLE_VERSION_INT >= AV_VERSION_INT(4, 7, 0)
        AVChannelLayout out_ch = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&swr, &out_ch, AV_SAMPLE_FMT_S16, 44100,
                                &actx->ch_layout, actx->sample_fmt, actx->sample_rate, 0, NULL) < 0)
          swr = NULL;
#else
        int64_t in_ch = actx->channel_layout ? (int64_t)actx->channel_layout
                                             : av_get_default_channel_layout(actx->channels);
        swr = swr_alloc_set_opts(NULL, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, 44100,
                                 in_ch, actx->sample_fmt, actx->sample_rate, 0, NULL);
#endif
        if (swr && swr_init(swr) >= 0) {
          psp_ch = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, 1152, PSP_AUDIO_FORMAT_STEREO);
          if (psp_ch < 0) swr_free(&swr);
        } else if (swr) {
          swr_free(&swr);
        }
      } else if (actx) {
        avcodec_free_context(&actx);
      }
    }
  }

  AVPacket *pkt = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  struct SwsContext *sws = NULL;
  int sws_w = 0, sws_h = 0, sws_fmt = -1, dw = SW, dh = SH, ox = 0, oy = 0;

  static short ff_abuf[1152 * 2] __attribute__((aligned(64)));
  int afill = 0;
  short *rbuf = NULL;
  int rcap = 0;

  AVRational vtb = fmt->streams[vidx]->time_base;
  int dur_sec = fmt->duration > 0 ? (int)(fmt->duration / AV_TIME_BASE) : 0;
  uint64_t t0 = 0;
  int64_t pts0 = AV_NOPTS_VALUE;
  int cur_sec = 0, paused = 0, show_hud = 1, quit = 0;

  while (!quit && pkt && frame) {
    unsigned p = read_pad();
    if (p & PSP_CTRL_CIRCLE) break;
    if (p & PSP_CTRL_SQUARE) { paused = !paused; pts0 = AV_NOPTS_VALUE; }
    if (p & PSP_CTRL_TRIANGLE) show_hud = !show_hud;
    if (p & PSP_CTRL_UP) vol_change(0x800);
    if (p & PSP_CTRL_DOWN) vol_change(-0x800);
    if (p & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)) {
      int64_t target_s = (int64_t)cur_sec + ((p & PSP_CTRL_RTRIGGER) ? 10 : -10);
      if (target_s < 0) target_s = 0;
      av_seek_frame(fmt, -1, target_s * AV_TIME_BASE, (p & PSP_CTRL_LTRIGGER) ? AVSEEK_FLAG_BACKWARD : 0);
      avcodec_flush_buffers(vctx);
      if (actx) avcodec_flush_buffers(actx);
      pts0 = AV_NOPTS_VALUE; afill = 0;
    }

    if (paused) { sceDisplayWaitVblankStart(); continue; }
    if (av_read_frame(fmt, pkt) < 0) break;

    if (pkt->stream_index == vidx) {
      if (avcodec_send_packet(vctx, pkt) == 0) {
        while (avcodec_receive_frame(vctx, frame) == 0) {
          if (!sws || frame->width != sws_w || frame->height != sws_h || frame->format != sws_fmt) {
            if (sws) sws_freeContext(sws);
            sws_w = frame->width; sws_h = frame->height; sws_fmt = frame->format;
            if ((long)sws_w * SH > (long)sws_h * SW) { dw = SW; dh = sws_h * SW / sws_w; }
            else { dh = SH; dw = sws_w * SH / sws_h; }
            if (dw < 2) dw = 2;
            if (dh < 2) dh = 2;
            ox = (SW - dw) / 2; oy = (SH - dh) / 2;
            sws = sws_getContext(sws_w, sws_h, (enum AVPixelFormat)sws_fmt,
                                 dw, dh, AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR, NULL, NULL, NULL);
          }
          if (!sws) continue;

          int64_t pts = frame->best_effort_timestamp;
          if (pts != AV_NOPTS_VALUE) {
            int64_t pts_us = av_rescale_q(pts, vtb, (AVRational){ 1, 1000000 });
            if (pts0 == AV_NOPTS_VALUE) { pts0 = pts_us; t0 = sceKernelGetSystemTimeWide(); }
            int64_t target = (int64_t)t0 + (pts_us - pts0);
            int64_t now = (int64_t)sceKernelGetSystemTimeWide();
            if (target > now && target - now < 500000) sceKernelDelayThread((SceUInt)(target - now));
            else if (now - target > 500000) { pts0 = pts_us; t0 = (uint64_t)now; }
            cur_sec = (int)(pts_us / 1000000);
          }

          for (int i = 0; i < SW * SH; i++) fb[i] = C_BLACK;
          uint8_t *dst[4] = { (uint8_t *)(fb + oy * SW + ox), NULL, NULL, NULL };
          int dlns[4] = { SW * 4, 0, 0, 0 };
          sws_scale(sws, (const uint8_t *const *)frame->data, frame->linesize, 0, sws_h, dst, dlns);

          if (show_hud) {
            char tb[48];
            if (dur_sec > 0) snprintf(tb, sizeof tb, "%02d:%02d / %02d:%02d", cur_sec / 60, cur_sec % 60, dur_sec / 60, dur_sec % 60);
            else snprintf(tb, sizeof tb, "%02d:%02d", cur_sec / 60, cur_sec % 60);
            fill_rect(0, SH - 28, SW, 28, C_DARK, 175);
            draw_text(&FONT_S, 10, SH - 25, C_WHITE, base(path), 330, 0);
            draw_text_r(&FONT_S, SW - 10, SH - 25, C_GRAY, tb);
            fill_rect(10, SH - 6, SW - 20, 3, C_WHITE, 50);
            if (dur_sec > 0) {
              int pw = (SW - 20) * (cur_sec > dur_sec ? dur_sec : cur_sec) / dur_sec;
              fill_rect(10, SH - 6, pw, 3, C_ACC, 255);
            }
          }
          flip(0);
        }
      }
    } else if (pkt->stream_index == aidx && actx && swr && psp_ch >= 0) {
      if (avcodec_send_packet(actx, pkt) == 0) {
        while (avcodec_receive_frame(actx, frame) == 0) {
          int out_n = (int)av_rescale_rnd(swr_get_delay(swr, actx->sample_rate) + frame->nb_samples,
                                          44100, actx->sample_rate, AV_ROUND_UP);
          if (out_n > rcap) {
            short *nb = realloc(rbuf, (size_t)out_n * 4);
            if (!nb) break;
            rbuf = nb; rcap = out_n;
          }
          uint8_t *outp[1] = { (uint8_t *)rbuf };
          int conv = swr_convert(swr, outp, out_n, (const uint8_t **)frame->extended_data, frame->nb_samples);
          for (int i = 0; i < conv; i++) {
            ff_abuf[afill * 2] = rbuf[i * 2];
            ff_abuf[afill * 2 + 1] = rbuf[i * 2 + 1];
            afill++;
            if (afill == 1152) { sceAudioOutputBlocking(psp_ch, a_vol, ff_abuf); afill = 0; }
          }
        }
      }
    }
    av_packet_unref(pkt);
  }

  if (rbuf) free(rbuf);
  if (sws) sws_freeContext(sws);
  if (swr) swr_free(&swr);
  if (psp_ch >= 0) sceAudioChRelease(psp_ch);
  if (frame) av_frame_free(&frame);
  if (pkt) av_packet_free(&pkt);
  if (actx) avcodec_free_context(&actx);
  avcodec_free_context(&vctx);
  avformat_close_input(&fmt);
}
#endif

static void video_player(const char *path) {
#ifdef HAVE_FFMPEG
  video_player_ffmpeg(path);
#else
  video_player_avi(path);
#endif
}

/* ================= UI rendering ================= */
static const char *tabnames[6] = { N_("Главная"), N_("Память PSP"), N_("Поиск"), N_("Настройки"), N_("О приложении"), N_("Плеер") };

static void draw_header(void) {
  fill_rect(0, 0, SW, 27, C_DARK, 155);
  fill_rect(0, 27, SW, 1, C_ACC, 110);
  draw_text(&FONT_L, 8, 1, C_TITLE, "PSP media", 0, 1);
  int nt = a_playing ? 6 : 5;
  const Font *tf = (nt == 6) ? &FONT_S : &FONT_M;
  int tw[6], sum = 0, x0 = 104;
  for (int i = 0; i < nt; i++) { tw[i] = text_width(tf, TR(tabnames[i])); sum += tw[i]; }
  int gap = (SW - 8 - x0 - sum) / (nt - 1);
  if (gap > 18) gap = 18;
  if (gap < 4) gap = 4;
  int x = x0;
  for (int i = 0; i < nt; i++) {
    int act = (i == g_tab);
    uint32_t col = act ? C_WHITE : (i == 5 ? C_GREEN : C_GRAY);
    draw_text(tf, x, 5, col, TR(tabnames[i]), 0, 1);
    if (act) fill_rect(x, 23, tw[i], 2, i == 5 ? C_GREEN : C_WHITE, 255);
    x += tw[i] + gap;
  }
}

static void draw_bar(const char *hint) {
  fill_rect(0, 240, SW, 32, C_DARK, 175);
  fill_rect(0, 240, SW, 1, C_ACC, 110);
  if (a_playing) {
    if (a_pause) { fill_rect(10, 246, 4, 12, C_GREEN, 255); fill_rect(17, 246, 4, 12, C_GREEN, 255); }
    else fill_tri(10, 246, 10, 258, 21, 252, C_GREEN, 255);
    draw_text(&FONT_M, 28, 243, C_WHITE, base(a_path), 260, 0);
    char t[48], v[24];
    int s = a_secs, d = a_dur;
    if (d > 0) snprintf(t, sizeof t, "%02d:%02d / %02d:%02d", s / 60, s % 60, d / 60, d % 60);
    else snprintf(t, sizeof t, "%02d:%02d", s / 60, s % 60);
    snprintf(v, sizeof v, TR("Громк. %d%%"), a_vol * 100 / 0x8000);
    draw_text_r(&FONT_S, SW - 10, 245, C_GRAY, t);
    draw_text_r(&FONT_S, SW - 100, 245, C_GRAY, v);
    fill_rect(10, 264, SW - 20, 3, C_WHITE, 50);
    if (d > 0) {
      int w = (SW - 20) * (s > d ? d : s) / d;
      fill_rect(10, 264, w, 3, C_ACC, 255);
    }
  } else if (cfg.hints) {
    draw_text(&FONT_S, 10, 249, C_GRAY, hint, SW - 20, 0);
  }
}

static void draw_toast(void) {
  if (msgtimer <= 0) return;
  int w = text_width(&FONT_M, msgbuf) + 28;
  if (w > 440) w = 440;
  int x = (SW - w) / 2;
  fill_rect(x, 208, w, 26, C_DARK, 215);
  rect_border(x, 208, w, 26, C_YELLOW, 200, 1);
  draw_text(&FONT_M, x + 14, 212, C_YELLOW, msgbuf, w - 28, 0);
}

static void draw_rows(View *v, int x, int y, int w, int rows, int focus) {
  for (int r = 0; r < rows; r++) {
    int i = v->top + r;
    if (i >= v->n) break;
    Entry *e = v->it[i];
    int ry = y + r * 20, sel = (i == v->cursor);
    if (sel) fill_rect(x + 2, ry, w - 10, 19, focus ? C_ACC : C_GRAY, focus ? 105 : 45);
    draw_icon(e->type, x + 8, ry + 4);
    int playing = a_playing && !strcmp(e->path, a_path);
    int rw = 8;
    if (e->type != T_DIR) {
      char sz[24]; fmt_size(sz, e->size);
      rw = text_width(&FONT_S, sz) + 18;
      draw_text_r(&FONT_S, x + w - 12, ry + 2, C_GRAY, sz);
    }
    draw_text(&FONT_S, x + 26, ry + 2, playing ? C_GREEN : C_WHITE, e->disp, w - 26 - rw - 8, 0);
  }
  if (v->n > rows) {
    int th = rows * 20, bh = th * rows / v->n;
    if (bh < 8) bh = 8;
    int by = (th - bh) * v->top / (v->n - rows);
    fill_rect(x + w - 6, y, 3, th, C_WHITE, 40);
    fill_rect(x + w - 6, y + by, 3, bh, C_ACC, 255);
  }
}

/* ---- preview ---- */
static uint32_t *prev_img; static int prev_w, prev_h, prev_wait;
static char prev_path[256];
static void draw_preview(Entry *e) {
  fill_rect(314, 32, 158, 204, C_DARK, 150);
  if (!e) return;
  fill_vgrad(320, 38, 146, 82, RGB(20, 70, 130), RGB(8, 22, 54));
  if (prev_img && !strcmp(prev_path, e->path)) blit_fit(prev_img, prev_w, prev_h, 320, 38, 146, 82);
  else big_icon(e->type, 393, 79, 26);
  rect_border(320, 38, 146, 82, C_WHITE, 70, 1);
  int ln = draw_wrapped(&FONT_S, 320, 126, 146, 3, C_WHITE, e->disp);
  int y = 126 + ln * FONT_S.lineh + 4;
  if (e->type != T_DIR) {
    char sz[24], b[64]; fmt_size(sz, e->size);
    snprintf(b, sizeof b, TR("Размер: %s"), sz); draw_text(&FONT_S, 320, y, C_GRAY, b, 146, 0); y += 16;
  }
  char b2[64]; snprintf(b2, sizeof b2, TR("Тип: %s"), type_upper(e->type));
  draw_text(&FONT_S, 320, y, C_ACC, b2, 146, 0); y += 18;
  char nb[64]; const char *note = NULL; int warn = 0;
  if (e->type == T_VIDEO && !is_video_playable(e->path)) { note = TR("Неподдерживаемый формат"); warn = 1; }
  else if (e->type == T_VIDEO) { snprintf(nb, sizeof nb, TR("%s — смотреть"), KN(ACT_OK)); note = nb; }
  else if (e->type == T_AUDIO && !is_mp3(e->path)) { note = TR("Пока только MP3"); warn = 1; }
  else if (e->type == T_IMAGE && !is_viewable(e->path)) { note = TR("Пока JPG, PNG и BMP"); warn = 1; }
  else if (e->type == T_AUDIO) { snprintf(nb, sizeof nb, TR("%s — играть"), KN(ACT_OK)); note = nb; }
  else if (e->type == T_IMAGE) { snprintf(nb, sizeof nb, TR("%s — открыть"), KN(ACT_OK)); note = nb; }
  else if (e->type == T_DIR) { snprintf(nb, sizeof nb, TR("%s — войти"), KN(ACT_OK)); note = nb; }
  if (note) draw_text(&FONT_S, 320, y, warn ? C_YELLOW : C_GREEN, note, 146, 0);
}

static void render_list(View *v, const char *title) {
  fill_rect(8, 32, 300, 204, C_DARK, 150);
  draw_text(&FONT_M, 16, 35, C_ACC, title, 220, 1);
  char b[32]; snprintf(b, sizeof b, "%d / %d", v->n ? v->cursor + 1 : 0, v->n);
  draw_text_r(&FONT_S, 296, 37, C_GRAY, b);
  fill_rect(8, 52, 300, 1, C_ACC, 70);
  if (v->n == 0) draw_text(&FONT_S, 20, 64, C_GRAY, TR("Здесь пока пусто"), 0, 0);
  draw_rows(v, 8, 54, 300, ROWS_L, 1);
  draw_preview(v->n ? v->it[v->cursor] : NULL);
}

/* ---- home ---- */
static uint32_t *home_thumb; static int home_tw, home_th;
static void home_thumb_load(void) {
  if (home_thumb) { free(home_thumb); home_thumb = NULL; }
  for (int i = 0, tried = 0; i < nall && tried < 20; i++) {
    if (all[i].type == T_IMAGE && is_jpeg(all[i].path) && all[i].size < 20u * 1024 * 1024) {
      tried++;
      home_thumb = load_thumb(all[i].path, &home_tw, &home_th);
      if (home_thumb) break;
    }
  }
}
static void render_home(void) {
  static const int ctype[3] = { T_VIDEO, T_IMAGE, T_AUDIO };
  static const uint32_t top[3] = { RGB(40, 70, 170), RGB(40, 140, 200), RGB(20, 150, 140) };
  for (int c = 0; c < 3; c++) {
    int x = 10 + c * 154, sel = (c == g_card), t = ctype[c];
    fill_rect(x, 32, 146, 146, C_DARK, sel ? 150 : 105);
    if (sel) { rect_border(x, 32, 146, 146, C_ACC, 255, 2); fill_rect(x + 2, 34, 142, 142, C_ACC, 22); }
    else rect_border(x, 32, 146, 146, C_WHITE, 60, 1);
    fill_vgrad(x + 6, 38, 134, 78, top[c], RGB(6, 16, 40));
    if (t == T_IMAGE && home_thumb) blit_fit(home_thumb, home_tw, home_th, x + 6, 38, 134, 78);
    else big_icon(t, x + 73, 77, 24);
    rect_border(x + 6, 38, 134, 78, C_WHITE, 90, 1);
    Entry *first = NULL;
    for (int i = 0; i < nall; i++) if (all[i].type == t) { first = &all[i]; break; }
    if (first) {
      char sz[24], b[48]; fmt_size(sz, first->size);
      draw_text(&FONT_M, x + 8, 120, C_WHITE, first->disp, 130, 1);
      draw_text(&FONT_S, x + 8, 139, C_GRAY, sz, 0, 0);
      snprintf(b, sizeof b, TR("всего: %d"), cnt[t]);
      draw_text_r(&FONT_S, x + 138, 139, C_GRAY, b);
    } else draw_text(&FONT_M, x + 8, 120, C_GRAY, TR("Нет файлов"), 0, 1);
    char tb[48]; snprintf(tb, sizeof tb, TR("ТИП: %s"), type_upper(t));
    draw_text(&FONT_S, x + 8, 156, sel ? C_ACC : C_GRAY, tb, 130, 0);
  }
  fill_rect(10, 184, 454, 52, C_DARK, 135);
  char b[96];
  snprintf(b, sizeof b, TR("Найдено файлов: %d"), cnt[T_AUDIO] + cnt[T_VIDEO] + cnt[T_IMAGE]);
  draw_text(&FONT_M, 20, 190, C_TITLE, b, 430, 1);
  if (nall >= MAX_ALL) draw_text(&FONT_S, 20, 212, C_YELLOW, TR("Лимит списка достигнут, используй вкладку «Память PSP»"), 430, 0);
  else {
    snprintf(b, sizeof b, TR("Музыка: %d    Видео: %d    Фото: %d"), cnt[T_AUDIO], cnt[T_VIDEO], cnt[T_IMAGE]);
    draw_text(&FONT_S, 20, 212, C_WHITE, b, 430, 0);
  }
}

/* ---- search ---- */
static void render_search(void) {
  fill_rect(10, 34, 460, 26, C_DARK, 155);
  rect_border(10, 34, 460, 26, g_sfocus == 0 ? C_ACC : C_GRAY, g_sfocus == 0 ? 255 : 90, 1);
  if (qn == 0) draw_text(&FONT_M, 18, 38, C_GRAY, TR("Введите название файла…"), 0, 0);
  else {
    int w = draw_text(&FONT_M, 18, 38, C_WHITE, qstr, 440, 0);
    if (g_sfocus == 0 && (g_frames / 30) % 2 == 0) fill_rect(18 + w + 2, 38, 2, 16, C_ACC, 255);
  }
  fill_rect(10, 64, 460, 26, C_DARK, 130);
  for (int s = 0; s < 17; s++) {
    int idx = ((g_ci + s - 8) % ncs + ncs) % ncs;
    int x = 10 + s * 27 + 13;
    uint32_t cp = charset[idx];
    if (s == 8) fill_rect(x - 13, 65, 27, 24, g_sfocus == 0 ? C_ACC : C_GRAY, g_sfocus == 0 ? 120 : 50);
    if (cp == ' ') {
      fill_rect(x - 5, 76, 1, 6, C_WHITE, 255); fill_rect(x - 5, 81, 11, 1, C_WHITE, 255); fill_rect(x + 5, 76, 1, 6, C_WHITE, 255);
    } else {
      char b[4]; b[u8put(b, cp)] = 0;
      int w = text_width(&FONT_M, b);
      draw_text(&FONT_M, x - w / 2, 69, s == 8 ? C_WHITE : C_GRAY, b, 0, 0);
    }
  }
  fill_rect(10, 94, 460, 142, C_DARK, 150);
  char b[64];
  if (qn == 0) snprintf(b, sizeof b, TR("Результаты появятся здесь"));
  else snprintf(b, sizeof b, TR("Найдено: %d"), vsrch.n);
  draw_text(&FONT_S, 18, 97, C_ACC, b, 0, 0);
  fill_rect(10, 113, 460, 1, C_ACC, 60);
  draw_rows(&vsrch, 10, 115, 460, ROWS_S, g_sfocus == 1);
}

/* ---- about ---- */
static void render_about(void) {
  fill_rect(10, 34, 460, 202, C_DARK, 155);
  draw_text(&FONT_L, 20, 38, C_TITLE, "PSP media  v1.1", 0, 1);
  char ln[10][120];
  snprintf(ln[0], 120, "%s", TR("Плеер для PSP: музыка, фото и видео с карты памяти."));
  snprintf(ln[1], 120, "%s", TR("Сейчас: MP3, JPG, PNG, BMP, MP4, AVI, MKV."));
  snprintf(ln[2], 120, TR("%s/%s — вкладки     %s — открыть     %s — назад"), KN(ACT_PREV), KN(ACT_NEXT), KN(ACT_OK), KN(ACT_BACK));
  snprintf(ln[3], 120, TR("%s — пауза     %s — стоп музыки     %s — доп. действие"), KN(ACT_PAUSE), KN(ACT_STOP), KN(ACT_ALT));
  snprintf(ln[4], 120, "%s", TR("Select + Вверх/Вниз — громкость, Влево/Вправо — трек"));
  snprintf(ln[5], 120, "%s", TR("Стик вверх/вниз — прокрутка (сильнее наклон — быстрее)"));
  snprintf(ln[6], 120, "%s", TR("Стик влево/вправо: в списках — громкость, на главной —"));
  snprintf(ln[7], 120, "%s", TR("карточки, в фото — листать, в поиске — выбор буквы"));
  snprintf(ln[8], 120, "%s", TR("Кнопки перепутались? Держи Select + Start 2 секунды."));
  snprintf(ln[9], 120, "%s", TR("Фон, язык и кнопки меняются во вкладке «Настройки»."));
  for (int i = 0; i < 10; i++) draw_text(&FONT_S, 20, 68 + i * 16, i < 2 ? C_WHITE : (i >= 8 ? C_ACC : C_GRAY), ln[i], 440, 0);
}


/* ---- music player tab (visible only while music is playing) ---- */
static void render_player(void) {
  fill_rect(10, 34, 460, 202, C_DARK, 155);
  /* left: cover art box + simple animated bars */
  fill_vgrad(24, 48, 136, 136, RGB(20, 145, 145), RGB(8, 22, 54));
  big_icon(T_AUDIO, 92, 104, 30);
  for (int b = 0; b < 9; b++) {
    int h = a_pause ? 4 : (4 + (int)((g_frames / 3 + b * 7) % 18));
    if (h > 18) h = 18;
    fill_rect(34 + b * 13, 178 - h, 8, h, C_ACC, 210);
  }
  rect_border(24, 48, 136, 136, C_ACC, 160, 1);

  /* right: track info */
  draw_text(&FONT_S, 176, 48, C_ACC, TR("СЕЙЧАС ИГРАЕТ"), 280, 0);
  draw_wrapped(&FONT_M, 176, 66, 280, 2, C_WHITE, base(a_path));

  char tb[96];
  if (nq > 0) snprintf(tb, sizeof tb, TR("Трек: %d / %d"), qpos + 1, nq);
  else snprintf(tb, sizeof tb, TR("Трек: 1 / 1"));
  draw_text(&FONT_S, 176, 108, C_GRAY, tb, 280, 0);

  draw_text(&FONT_S, 176, 126, a_pause ? C_YELLOW : C_GREEN,
            a_pause ? TR("Статус: Пауза") : TR("Статус: Играет"), 280, 0);

  const char *rep_s = cfg.rep == 0 ? TR("Выкл") : cfg.rep == 1 ? TR("Все") : TR("Один трек");
  snprintf(tb, sizeof tb, TR("Повтор: %s   •   Перемеш.: %s"), rep_s, cfg.shuf ? TR("Вкл") : TR("Выкл"));
  draw_text(&FONT_S, 176, 144, C_GRAY, tb, 280, 0);

  int vp = a_vol * 100 / 0x8000;
  snprintf(tb, sizeof tb, TR("Громкость: %d%%"), vp);
  draw_text(&FONT_S, 176, 164, C_WHITE, tb, 140, 0);
  fill_rect(290, 170, 160, 6, C_WHITE, 45);
  fill_rect(290, 170, 160 * vp / 100, 6, C_GREEN, 230);

  /* bottom: progress bar & time */
  int s = a_secs, d = a_dur;
  char ts[32], td[32];
  snprintf(ts, sizeof ts, "%02d:%02d", s / 60, s % 60);
  if (d > 0) snprintf(td, sizeof td, "%02d:%02d", d / 60, d % 60);
  else strcpy(td, "--:--");
  draw_text(&FONT_S, 24, 196, C_WHITE, ts, 0, 0);
  draw_text_r(&FONT_S, 456, 196, C_GRAY, td);
  fill_rect(24, 216, 432, 6, C_WHITE, 50);
  if (d > 0) {
    int pw = 432 * (s > d ? d : s) / d;
    fill_rect(24, 216, pw, 6, C_ACC, 255);
  }
}

/* ---- custom background ---- */
static int load_custom_bg(const char *path) {
  int w, h;
  uint32_t *im = load_photo(path, &w, &h);
  if (!im) return 0;
  if (!bgimg[2]) bgimg[2] = malloc(SW * SH * 4);
  if (!bgimg[2]) { free(im); return 0; }
  float sx = SW / (float)w, sy = SH / (float)h, sc = sx > sy ? sx : sy;   // "cover": fill the screen, crop the rest
  float ox = (w - SW / sc) / 2, oy = (h - SH / sc) / 2;
  static int xm[SW];
  for (int x = 0; x < SW; x++) { int v = (int)(ox + x / sc); xm[x] = v < 0 ? 0 : v >= w ? w - 1 : v; }
  for (int y = 0; y < SH; y++) {
    int v = (int)(oy + y / sc); if (v < 0) v = 0; if (v >= h) v = h - 1;
    const uint32_t *src = im + (size_t)v * w;
    for (int x = 0; x < SW; x++) bgimg[2][y * SW + x] = src[xm[x]] | 0xFF000000u;
  }
  free(im);
  return 1;
}

/* ---- settings screen ---- */
enum { SK_CHOICE, SK_ACTION, SK_KEY, SK_PRESET };
enum { AC_PICKBG, AC_RESCAN, AC_RESET };
typedef struct { const char *label; uint8_t *val; int nopt; const char *opt[4]; int kind; int arg; } SItem;
static const SItem sitems[] = {
  { N_("Язык"), &cfg.lang, 2, { N_("Русский"), N_("English") }, SK_CHOICE, 0 },
  { N_("Фон"), &cfg.bg, 4, { N_("Авто"), N_("Синий"), N_("Тёмный"), N_("Свой") }, SK_CHOICE, 0 },
  { N_("Свой фон (выбрать фото)"), NULL, 0, { NULL }, SK_ACTION, AC_PICKBG },
  { N_("Затемнение фона"), &cfg.dim, 3, { N_("Мало"), N_("Средне"), N_("Сильно") }, SK_CHOICE, 0 },
  { N_("Повтор"), &cfg.rep, 3, { N_("Выкл"), N_("Все"), N_("Один трек") }, SK_CHOICE, 0 },
  { N_("Перемешивание"), &cfg.shuf, 2, { N_("Выкл"), N_("Вкл") }, SK_CHOICE, 0 },
  { N_("Громкость при запуске"), &cfg.startvol, 4, { "25%", "50%", "75%", "100%" }, SK_CHOICE, 0 },
  { N_("Аналоговый стик"), &cfg.stick, 2, { N_("Выкл"), N_("Вкл") }, SK_CHOICE, 0 },
  { N_("Чувствительность стика"), &cfg.sens, 3, { N_("Низкая"), N_("Средняя"), N_("Высокая") }, SK_CHOICE, 0 },
  { N_("Превью фото в списках"), &cfg.preview, 2, { N_("Выкл"), N_("Вкл") }, SK_CHOICE, 0 },
  { N_("Подсказки внизу"), &cfg.hints, 2, { N_("Выкл"), N_("Вкл") }, SK_CHOICE, 0 },
  { N_("Частота процессора"), &cfg.cpu, 2, { "222 MHz", "333 MHz" }, SK_CHOICE, 0 },
  { N_("Раскладка кнопок"), NULL, 0, { NULL }, SK_PRESET, 0 },
  { N_("Кнопка: ОК"), NULL, 0, { NULL }, SK_KEY, ACT_OK },
  { N_("Кнопка: Назад"), NULL, 0, { NULL }, SK_KEY, ACT_BACK },
  { N_("Кнопка: Пауза"), NULL, 0, { NULL }, SK_KEY, ACT_PAUSE },
  { N_("Кнопка: Стоп музыки"), NULL, 0, { NULL }, SK_KEY, ACT_STOP },
  { N_("Кнопка: Вкладка влево"), NULL, 0, { NULL }, SK_KEY, ACT_PREV },
  { N_("Кнопка: Вкладка вправо"), NULL, 0, { NULL }, SK_KEY, ACT_NEXT },
  { N_("Кнопка: Доп. действие"), NULL, 0, { NULL }, SK_KEY, ACT_ALT },
  { N_("Пересканировать карту памяти"), NULL, 0, { NULL }, SK_ACTION, AC_RESCAN },
  { N_("Сбросить все настройки"), NULL, 0, { NULL }, SK_ACTION, AC_RESET },
};
#define NSITEMS ((int)(sizeof sitems / sizeof sitems[0]))
#define ROWS_SET 8
static int g_set_sel, g_set_top, g_capture;   // g_capture = action being rebound + 1

static void render_settings(void) {
  fill_rect(10, 34, 460, 202, C_DARK, 155);
  draw_text(&FONT_M, 20, 37, C_ACC, TR("Настройки"), 200, 1);
  fill_rect(10, 56, 460, 1, C_ACC, 70);
  for (int r = 0; r < ROWS_SET; r++) {
    int i = g_set_top + r;
    if (i >= NSITEMS) break;
    const SItem *it = &sitems[i];
    int ry = 58 + r * 22, sel = (i == g_set_sel);
    if (sel) fill_rect(12, ry, 446, 21, C_ACC, 95);
    draw_text(&FONT_S, 22, ry + 3, it->kind == SK_ACTION ? C_TITLE : C_WHITE, TR(it->label), 260, 0);
    char vb[128]; vb[0] = 0; uint32_t vc = sel ? C_WHITE : C_GRAY;
    switch (it->kind) {
      case SK_CHOICE: snprintf(vb, sizeof vb, "< %s >", TR(it->opt[*it->val])); break;
      case SK_PRESET: {
        int pr = keys_preset();
        snprintf(vb, sizeof vb, "< %s >", TR(pr == 0 ? N_("Стандартные") : pr == 1 ? N_("Японские (O — ОК)") : N_("Свои")));
        break;
      }
      case SK_KEY:
        if (g_capture == it->arg + 1) { snprintf(vb, sizeof vb, "%s", TR("нажмите кнопку…")); vc = C_YELLOW; }
        else { snprintf(vb, sizeof vb, "%s", btn_name(cfg.keymap[it->arg])); vc = C_GREEN; }
        break;
      case SK_ACTION:
        if (it->arg == AC_PICKBG) snprintf(vb, sizeof vb, "%s", cfg.bgpath[0] ? base(cfg.bgpath) : TR("не выбрано"));
        else snprintf(vb, sizeof vb, ">");
        break;
    }
    int w = text_width(&FONT_S, vb);
    if (w > 190) w = 190;
    draw_text(&FONT_S, 450 - w, ry + 3, vc, vb, 190, 0);
  }
  if (NSITEMS > ROWS_SET) {
    int th = ROWS_SET * 22, bh = th * ROWS_SET / NSITEMS;
    if (bh < 8) bh = 8;
    int by = (th - bh) * g_set_top / (NSITEMS - ROWS_SET);
    fill_rect(462, 58, 3, th, C_WHITE, 40);
    fill_rect(462, 58 + by, 3, bh, C_ACC, 255);
  }
}

static void rescan_all(void) {
  scan_all(); load_dir(curdir); home_thumb_load();
  set_msg(TR("Карта памяти пересканирована"));
}
static void settings_apply(const SItem *it) {
  if (it->val == &cfg.cpu) apply_cpu();
  if (it->val == &cfg.startvol) a_vol = vol_steps[cfg.startvol] * 0x8000 / 100;
  if (it->val == &cfg.bg && cfg.bg == 3 && !bgimg[2]) {
    if (!(cfg.bgpath[0] && load_custom_bg(cfg.bgpath))) { cfg.bg = 0; set_msg(TR("Сначала выберите свой фон")); }
  }
  cfg_save();
}
static void set_change(int i, int dir) {
  const SItem *it = &sitems[i];
  if (it->kind == SK_CHOICE) { *it->val = (*it->val + dir + it->nopt) % it->nopt; settings_apply(it); }
  else if (it->kind == SK_PRESET) { if (keys_preset() == 0) keys_japan(); else keys_default(); cfg_save(); }
}
static void set_activate(int i) {
  const SItem *it = &sitems[i];
  if (it->kind == SK_CHOICE || it->kind == SK_PRESET) { set_change(i, 1); return; }
  if (it->kind == SK_KEY) { g_capture = it->arg + 1; return; }
  if (it->arg == AC_PICKBG) {
    build_cat(T_IMAGE);
    if (vcat.n == 0) set_msg(TR("Фото не найдены")); else g_sub = 1;
  } else if (it->arg == AC_RESCAN) rescan_all();
  else if (it->arg == AC_RESET) {
    cfg_defaults(); apply_cpu();
    a_vol = vol_steps[cfg.startvol] * 0x8000 / 100;
    if (bgimg[2]) { free(bgimg[2]); bgimg[2] = NULL; }
    cfg_save(); set_msg(TR("Настройки сброшены"));
  }
}
static void pick_bg(const char *path) {
  if (load_custom_bg(path)) {
    strncpy(cfg.bgpath, path, 255); cfg.bgpath[255] = 0;
    cfg.bg = 3; cfg_save(); g_sub = 0; set_msg(TR("Фон установлен"));
  } else set_msg(TR("Не удалось загрузить фото"));
}

static Entry *cur_entry(void) {
  View *v = NULL;
  if (g_tab == 0 && g_sub == 1) v = &vcat;
  else if (g_tab == 1) v = &vmem;
  else if (g_tab == 2 && g_sfocus == 1) v = &vsrch;
  else if (g_tab == 3 && g_sub == 1) v = &vcat;
  return (v && v->n) ? v->it[v->cursor] : NULL;
}

static uint32_t *current_bg(void) {
  if (cfg.bg == 3 && bgimg[2]) return bgimg[2];
  if (cfg.bg == 1) return bgimg[0];
  if (cfg.bg == 2) return bgimg[1];
  return bgimg[g_tab == 0 ? 0 : 1];   // auto: blue on Home, dark elsewhere
}

static void render(void) {
  static const int dim[3] = { 45, 85, 130 };
  memcpy(fb, current_bg(), sizeof fb);
  int da = dim[cfg.dim > 2 ? 1 : cfg.dim];
  if (g_tab == 0) fill_hgrad(0, 0, 330, SH, C_DARK, da + 35, 0);
  else fill_rect(0, 0, SW, SH, C_DARK, da);
  draw_header();
  char hint[200];
  snprintf(hint, sizeof hint, TR("%s открыть   %s назад   %s/%s вкладки   %s пауза   %s стоп"),
           KN(ACT_OK), KN(ACT_BACK), KN(ACT_PREV), KN(ACT_NEXT), KN(ACT_PAUSE), KN(ACT_STOP));
  if (g_tab == 0) {
    if (g_sub == 0) render_home();
    else {
      static const char *names[3] = { N_("Видео"), N_("Фото"), N_("Музыка") };
      render_list(&vcat, TR(names[g_kind]));
    }
  } else if (g_tab == 1) {
    render_list(&vmem, curdir);
  } else if (g_tab == 2) {
    render_search();
    snprintf(hint, sizeof hint, TR("Лево/Право — буква   %s — добавить   %s — стереть   Вниз — результаты"), KN(ACT_OK), KN(ACT_PAUSE));
  } else if (g_tab == 3) {
    if (g_sub == 1) {
      render_list(&vcat, TR("Выберите фон"));
      snprintf(hint, sizeof hint, TR("%s — выбрать фон   %s — отмена"), KN(ACT_OK), KN(ACT_BACK));
    } else {
      render_settings();
      if (g_capture) snprintf(hint, sizeof hint, "%s", TR("Нажмите новую кнопку   Select — отмена"));
      else snprintf(hint, sizeof hint, TR("Вверх/Вниз — выбор   Лево/Право — изменить   %s — применить"), KN(ACT_OK));
    }
  } else if (g_tab == 4) {
    render_about();
  } else if (g_tab == 5 && a_playing) {
    render_player();
    snprintf(hint, sizeof hint, TR("%s пауза   Лево/Право трек   Вверх/Вниз громк.   %s повтор   %s стоп"),
             KN(ACT_OK), KN(ACT_ALT), KN(ACT_STOP));
  }
  draw_bar(hint);
  draw_toast();
}

/* ================= actions ================= */
static void open_entry(View *v) {
  if (v->n == 0) return;
  Entry *e = v->it[v->cursor];
  char tmp[256];
  switch (e->type) {
    case T_DIR: strcpy(tmp, e->path); load_dir(tmp); break;
    case T_AUDIO:
      if (is_mp3(e->path)) start_queue(v); else set_msg(TR("Пока поддерживается только MP3"));
      break;
    case T_IMAGE:
      if (is_viewable(e->path)) photo_viewer(v, v->cursor); else set_msg(TR("Поддерживаются JPG, PNG и BMP"));
      break;
    case T_VIDEO:
      if (is_video_playable(e->path)) video_player(e->path);
      else set_msg(TR("Формат видео не поддерживается"));
      break;
    default: set_msg(TR("Этот тип файла не поддерживается"));
  }
}

static void gfx_init(void) {
  sceDisplaySetMode(0, SW, SH);
  void *edram = sceGeEdramGetAddr();
  vram = (uint32_t *)((uintptr_t)edram | 0x40000000);
  sceDisplaySetFrameBuf(edram, 512, PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_NEXTFRAME);
  for (int i = 0; i < 512 * SH; i++) vram[i] = C_BLACK;
}
static void load_backgrounds(void) {
  int w, h;
  bgimg[0] = jpeg_decode(NULL, bg1_jpg, bg1_jpg_len, &w, &h, SW, SH, 1000000);
  bgimg[1] = jpeg_decode(NULL, bg2_jpg, bg2_jpg_len, &w, &h, SW, SH, 1000000);
  for (int i = 0; i < 2; i++) {
    if (!bgimg[i]) {
      bgimg[i] = malloc(SW * SH * 4);
      for (int k = 0; k < SW * SH; k++) bgimg[i][k] = RGB(10, 40, 90);
    }
  }
}

int main(void) {
  setup_cb();
  cfg_load();
  apply_cpu();
  a_vol = vol_steps[cfg.startvol] * 0x8000 / 100;
  srand((unsigned)time(NULL));
  gfx_init();
  sceCtrlSetSamplingCycle(0);
  sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
  int at = sceKernelCreateThread("audio", audio_thread, 0x12, 0x10000, PSP_THREAD_ATTR_USER, NULL);
  if (at >= 0) sceKernelStartThread(at, 0, NULL);
  load_backgrounds();
  if (cfg.bg == 3 && !(cfg.bgpath[0] && load_custom_bg(cfg.bgpath))) cfg.bg = 0;
  init_charset();
  scan_all();
  load_dir("ms0:/");
  home_thumb_load();

  int dirty = 1;
  for (;;) {
    g_frames++;
    unsigned p = read_pad();

    /* emergency: hold Select + Start for 2 seconds to restore the default buttons */
    {
      static int rst;
      if ((held & PSP_CTRL_SELECT) && (held & PSP_CTRL_START)) {
        if (++rst == 120) { keys_default(); cfg_save(); set_msg(TR("Кнопки сброшены")); dirty = 1; }
      } else rst = 0;
    }

    /* waiting for a new button to assign (physical buttons are read directly) */
    if (g_capture) {
      if (raw_press & PSP_CTRL_SELECT) { g_capture = 0; dirty = 1; }
      else {
        for (int k = 0; k < ACT_N; k++) {
          if (!(raw_press & LOGICAL[k])) continue;
          unsigned cand = LOGICAL[k]; int cap = g_capture - 1;
          for (int a = 0; a < ACT_N; a++) if (a != cap && cfg.keymap[a] == cand) cfg.keymap[a] = cfg.keymap[cap];   // swap on conflict
          cfg.keymap[cap] = cand; cfg_save(); g_capture = 0; dirty = 1;
          break;
        }
      }
      p = 0; stick_ev = 0;
    }

    /* analog stick: up/down scroll everywhere; left/right = volume in lists, otherwise like the D-pad */
    {
      int list_ctx = (g_tab == 1) || (g_tab == 0 && g_sub == 1) || (g_tab == 3 && g_sub == 1) || (g_tab == 2 && g_sfocus == 1);
      if (stick_ev & ST_UP) p |= PSP_CTRL_UP;
      if (stick_ev & ST_DOWN) p |= PSP_CTRL_DOWN;
      if (list_ctx) {
        if (stick_ev & ST_LEFT) { vol_change(-0x400); dirty = 1; }
        if (stick_ev & ST_RIGHT) { vol_change(0x400); dirty = 1; }
      } else {
        if (stick_ev & ST_LEFT) p |= PSP_CTRL_LEFT;
        if (stick_ev & ST_RIGHT) p |= PSP_CTRL_RIGHT;
      }
    }

    /* Select + D-pad: volume and track switching */
    if (held & PSP_CTRL_SELECT) {
      if (p & PSP_CTRL_UP) vol_change(0x800);
      if (p & PSP_CTRL_DOWN) vol_change(-0x800);
      if (p & PSP_CTRL_LEFT) next_track(-1);
      if (p & PSP_CTRL_RIGHT) next_track(1);
      if (p & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT)) dirty = 1;
      p &= ~(PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT);
    }
    if (p & PSP_CTRL_START) { a_stop = 1; nq = 0; dirty = 1; }
    if (a_finished) { a_finished = 0; on_track_finished(); dirty = 1; }
    if (a_failed) {
      a_failed = 0; fail_run++; dirty = 1;
      if (nq == 0 || fail_run >= nq) {
        char m[96]; snprintf(m, sizeof m, TR("Не удалось воспроизвести файл (код %d)"), a_err);
        set_msg(m); nq = 0; fail_run = 0;
      } else next_track(1);
    }

    if (!a_playing && g_tab == 5) { g_tab = 0; g_sub = 0; dirty = 1; }
    {
      int nt = a_playing ? 6 : 5;
      if (p & PSP_CTRL_LTRIGGER) { g_tab = (g_tab + nt - 1) % nt; g_sub = 0; dirty = 1; }
      if (p & PSP_CTRL_RTRIGGER) { g_tab = (g_tab + 1) % nt; g_sub = 0; dirty = 1; }
    }

    if (g_tab == 0 && g_sub == 0) {
      if (p & PSP_CTRL_LEFT) { g_card = (g_card + 2) % 3; dirty = 1; }
      if (p & PSP_CTRL_RIGHT) { g_card = (g_card + 1) % 3; dirty = 1; }
      if (p & PSP_CTRL_CROSS) {
        g_kind = g_card;
        build_cat(g_card == 0 ? T_VIDEO : g_card == 1 ? T_IMAGE : T_AUDIO);
        g_sub = 1; dirty = 1;
      }
      if (p & PSP_CTRL_SQUARE) { a_pause = !a_pause; dirty = 1; }
    } else if ((g_tab == 0 && g_sub == 1) || g_tab == 1) {
      View *v = g_tab == 0 ? &vcat : &vmem;
      if (p & PSP_CTRL_UP) v->cursor--;
      if (p & PSP_CTRL_DOWN) v->cursor++;
      if (p & PSP_CTRL_LEFT) v->cursor -= ROWS_L;
      if (p & PSP_CTRL_RIGHT) v->cursor += ROWS_L;
      if (p & PSP_CTRL_CROSS) open_entry(v);
      if (p & PSP_CTRL_SQUARE) a_pause = !a_pause;
      if (p & PSP_CTRL_CIRCLE) {
        if (g_tab == 0) g_sub = 0;
        else if (strcmp(curdir, "ms0:/")) { char t[256]; parent_of(curdir, t); load_dir(t); }
      }
      view_clamp(v, ROWS_L);
      if (p) dirty = 1;
    } else if (g_tab == 2) {
      if (g_sfocus == 0) {
        if (p & PSP_CTRL_LEFT) g_ci = (g_ci + ncs - 1) % ncs;
        if (p & PSP_CTRL_RIGHT) g_ci = (g_ci + 1) % ncs;
        if ((p & PSP_CTRL_CROSS) && qn < 30) { qcp[qn++] = charset[g_ci]; qstr_update(); run_search(); }
        if ((p & PSP_CTRL_SQUARE) && qn > 0) { qn--; qstr_update(); run_search(); }
        if ((p & PSP_CTRL_TRIANGLE) && qn > 0) { qn = 0; qstr_update(); run_search(); }
        if ((p & PSP_CTRL_DOWN) && vsrch.n > 0) g_sfocus = 1;
      } else {
        if (p & PSP_CTRL_UP) { if (vsrch.cursor == 0) g_sfocus = 0; else vsrch.cursor--; }
        if (p & PSP_CTRL_DOWN) vsrch.cursor++;
        if (p & PSP_CTRL_LEFT) vsrch.cursor -= ROWS_S;
        if (p & PSP_CTRL_RIGHT) vsrch.cursor += ROWS_S;
        if (p & PSP_CTRL_CROSS) open_entry(&vsrch);
        if (p & PSP_CTRL_CIRCLE) g_sfocus = 0;
        if (p & PSP_CTRL_TRIANGLE) a_pause = !a_pause;
        view_clamp(&vsrch, ROWS_S);
      }
      if (p) dirty = 1;
    } else if (g_tab == 3 && g_sub == 0) {          /* settings */
      if (p & PSP_CTRL_UP) g_set_sel--;
      if (p & PSP_CTRL_DOWN) g_set_sel++;
      if (g_set_sel < 0) g_set_sel = 0;
      if (g_set_sel >= NSITEMS) g_set_sel = NSITEMS - 1;
      if (p & PSP_CTRL_LEFT) set_change(g_set_sel, -1);
      if (p & PSP_CTRL_RIGHT) set_change(g_set_sel, 1);
      if (p & PSP_CTRL_CROSS) set_activate(g_set_sel);
      if (g_set_sel < g_set_top) g_set_top = g_set_sel;
      if (g_set_sel >= g_set_top + ROWS_SET) g_set_top = g_set_sel - ROWS_SET + 1;
      if (p) dirty = 1;
    } else if (g_tab == 3 && g_sub == 1) {          /* choose background photo */
      View *v = &vcat;
      if (p & PSP_CTRL_UP) v->cursor--;
      if (p & PSP_CTRL_DOWN) v->cursor++;
      if (p & PSP_CTRL_LEFT) v->cursor -= ROWS_L;
      if (p & PSP_CTRL_RIGHT) v->cursor += ROWS_L;
      if ((p & PSP_CTRL_CROSS) && v->n) {
        Entry *e = v->it[v->cursor];
        if (is_viewable(e->path)) pick_bg(e->path); else set_msg(TR("Поддерживаются JPG, PNG и BMP"));
      }
      if (p & PSP_CTRL_CIRCLE) g_sub = 0;
      view_clamp(v, ROWS_L);
      if (p) dirty = 1;
    } else if (g_tab == 5 && a_playing) {           /* music player tab */
      if (p & (PSP_CTRL_CROSS | PSP_CTRL_SQUARE)) a_pause = !a_pause;
      if (p & PSP_CTRL_LEFT) next_track(-1);
      if (p & PSP_CTRL_RIGHT) next_track(1);
      if (p & PSP_CTRL_UP) vol_change(0x800);
      if (p & PSP_CTRL_DOWN) vol_change(-0x800);
      if (p & PSP_CTRL_TRIANGLE) { cfg.rep = (cfg.rep + 1) % 3; cfg_save(); }
      if (p & PSP_CTRL_CIRCLE) { g_tab = 0; g_sub = 0; }
      if (p) dirty = 1;
    }

    /* delayed photo preview (JPEG thumbnail of the highlighted file) */
    Entry *ce = cur_entry();
    const char *cp = ce ? ce->path : "";
    if (strcmp(cp, prev_path)) {
      strncpy(prev_path, cp, 255); prev_path[255] = 0;
      if (prev_img) { free(prev_img); prev_img = NULL; }
      prev_wait = 10; dirty = 1;
    } else if (prev_wait > 0 && --prev_wait == 0) {
      if (cfg.preview && ce && ce->type == T_IMAGE && is_jpeg(ce->path) && ce->size < 30u * 1024 * 1024) {
        prev_img = load_thumb(ce->path, &prev_w, &prev_h);
        dirty = 1;
      }
    }

    if (msgtimer > 0 && --msgtimer == 0) dirty = 1;
    if (a_playing && (g_frames % (g_tab == 5 ? 6 : 15) == 0)) dirty = 1;
    if (g_tab == 2 && g_frames % 30 == 0) dirty = 1;

    if (dirty) { render(); flip(1); dirty = 0; }
    else sceDisplayWaitVblankStart();
  }
  return 0;
}
