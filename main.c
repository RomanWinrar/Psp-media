// PSP media v1.0 - media player with Miku-style UI
// Tabs: Home (cards) / PSP memory (folders) / Search / About
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
#include "font.h"
#include "assets.h"

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
static uint32_t *bgimg[2];

static int g_tab, g_sub, g_card, g_sfocus, g_ci, g_kind;
static unsigned g_frames, held;
static char msgbuf[96]; static int msgtimer;

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
static unsigned read_pad(void) {
  static unsigned old; static int hold;
  SceCtrlData pad; sceCtrlPeekBufferPositive(&pad, 1);
  unsigned b = pad.Buttons, n = b & ~old;
  unsigned dp = PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT;
  if (b & dp) {
    if (n & dp) hold = 0;
    else { hold++; if (hold > 20 && hold % 3 == 0) n |= b & dp; }
  } else hold = 0;
  old = b; held = b;
  return n;
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
static int is_viewable(const char *p) { return has_ext(p, "jpg,jpeg,png"); }
static const char *base(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }
static const char *type_upper(int t) {
  return t == T_DIR ? "ПАПКА" : t == T_AUDIO ? "МУЗЫКА" : t == T_VIDEO ? "ВИДЕО" : t == T_IMAGE ? "ФОТО" : "ФАЙЛ";
}
static void fmt_size(char *o, uint32_t b) {
  if (b < 1024 * 1024) snprintf(o, 24, "%u КБ", (unsigned)((b + 1023) / 1024));
  else snprintf(o, 24, "%u.%u МБ", (unsigned)(b / 1048576), (unsigned)((b % 1048576) * 10 / 1048576));
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
static uint32_t clamp32(long long v) { return v < 0 ? 0 : v > 0xFFFFFFFFLL ? 0xFFFFFFFFu : (uint32_t)v; }
static void scan_dir(const char *dir, int depth) {
  static int tick;
  if (nall >= MAX_ALL || depth > 10) return;
  SceUID fd = sceIoDopen(dir);
  if (fd < 0) return;
  if ((tick++ & 7) == 0) {
    char b[48]; snprintf(b, sizeof b, "Найдено файлов: %d", nall);
    splash("Сканирую карту памяти...", b);
  }
  SceIoDirent d;
  while (nall < MAX_ALL) {
    memset(&d, 0, sizeof d);
    if (sceIoDread(fd, &d) <= 0) break;
    if (!strcmp(d.d_name, ".") || !strcmp(d.d_name, "..")) continue;
    char p[256]; join(p, sizeof p, dir, d.d_name);
    if (FIO_S_ISDIR(d.d_stat.st_mode)) {
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
  splash("Сканирую карту памяти...", "");
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
static volatile int a_req, a_stop, a_pause, a_playing, a_finished, a_secs, a_dur;
static volatile int a_vol = 0x6000;
static char a_path[256];
static char qpath[MAX_ALL][256];
static int nq, qpos;
static short pcm[1152 * 2] __attribute__((aligned(64)));
static unsigned char inbuf[16384 + MAD_BUFFER_GUARD];
static struct mad_stream st; static struct mad_frame fr; static struct mad_synth sy;

static inline short scale(mad_fixed_t s) {
  s += (1L << (MAD_F_FRACBITS - 16));
  if (s >= MAD_F_ONE) s = MAD_F_ONE - 1; else if (s < -MAD_F_ONE) s = -MAD_F_ONE;
  return s >> (MAD_F_FRACBITS + 1 - 16);
}
static void decode_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return;
  fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
  mad_stream_init(&st); mad_frame_init(&fr); mad_synth_init(&sy);
  int ch = -1, filled = 0;
  unsigned long total = 0, rate = 44100;
  while (!a_stop) {
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
    if (ch < 0) {
      rate = fr.header.samplerate;
      if (fr.header.bitrate > 0) a_dur = (int)((long long)fsz * 8 / fr.header.bitrate);
      ch = sceAudioSRCChReserve(1152, rate, 2);
      if (ch < 0) break;
    }
    for (unsigned i = 0; i < sy.pcm.length; i++) {
      short l = scale(sy.pcm.samples[0][i]);
      short r = sy.pcm.channels > 1 ? scale(sy.pcm.samples[1][i]) : l;
      pcm[filled * 2] = l; pcm[filled * 2 + 1] = r; filled++;
      if (filled == 1152) {
        sceAudioSRCOutputBlocking(a_vol, pcm);
        filled = 0; total += 1152; a_secs = total / rate;
        while (a_pause && !a_stop) sceKernelDelayThread(50000);
        if (a_stop) break;
      }
    }
  }
  if (ch >= 0) sceAudioSRCChRelease();
  mad_synth_finish(&sy); mad_frame_finish(&fr); mad_stream_finish(&st);
  fclose(f);
}
static int audio_thread(SceSize args, void *argp) {
  for (;;) {
    if (a_req) {
      a_req = 0; a_stop = 0; a_finished = 0; a_secs = 0; a_dur = 0; a_playing = 1;
      decode_file(a_path);
      a_playing = 0;
      if (!a_stop) a_finished = 1;
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
  qpos = sel; play_path(qpath[qpos]);
}
static void next_track(int dir) {
  if (nq == 0) return;
  qpos = (qpos + dir + nq) % nq;
  play_path(qpath[qpos]);
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
static uint32_t *load_photo(const char *path, int *w, int *h) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  uint32_t *r = is_jpeg(path) ? jpeg_decode(f, NULL, 0, w, h, SW, SH, 3000000) : png_decode(f, w, h);
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
      splash("Загрузка фото...", e->disp);
      im = load_photo(e->path, &w, &h); reload = 0;
    }
    for (int i = 0; i < SW * SH; i++) fb[i] = C_BLACK;
    if (im) blit_fit(im, w, h, 0, 0, SW, SH);
    else draw_text(&FONT_M, 90, 120, C_YELLOW, "Не удалось открыть (слишком большое или битое)", 0, 1);
    if (show) {
      char b[32]; snprintf(b, sizeof b, "%d / %d", pos + 1, v->n);
      fill_rect(0, SH - 40, SW, 40, C_DARK, 175);
      draw_text(&FONT_M, 10, SH - 37, C_WHITE, e->disp, 380, 0);
      draw_text_r(&FONT_S, SW - 10, SH - 36, C_GRAY, b);
      draw_text(&FONT_S, 10, SH - 19, C_GRAY, "L/R — листать   Треугольник — скрыть подпись   O — назад", 0, 0);
    }
    flip(1);
    unsigned p = read_pad();
    if (a_finished) { a_finished = 0; next_track(1); }
    if (p & PSP_CTRL_CIRCLE) break;
    if (p & PSP_CTRL_RTRIGGER) { int n = next_img(v, pos, 1); if (n >= 0) { pos = n; reload = 1; } }
    if (p & PSP_CTRL_LTRIGGER) { int n = next_img(v, pos, -1); if (n >= 0) { pos = n; reload = 1; } }
    if (p & PSP_CTRL_TRIANGLE) show = !show;
  }
  if (im) free(im);
}

/* ================= UI rendering ================= */
static const char *tabnames[4] = { "Главная", "Память PSP", "Поиск", "О приложении" };

static void draw_header(void) {
  fill_rect(0, 0, SW, 27, C_DARK, 155);
  fill_rect(0, 27, SW, 1, C_ACC, 110);
  draw_text(&FONT_L, 8, 1, C_TITLE, "PSP media", 0, 1);
  int x = 112;
  for (int i = 0; i < 4; i++) {
    int w = text_width(&FONT_M, tabnames[i]), act = (i == g_tab);
    draw_text(&FONT_M, x, 5, act ? C_WHITE : C_GRAY, tabnames[i], 0, 1);
    if (act) fill_rect(x, 23, w, 2, C_WHITE, 255);
    x += w + 18;
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
    snprintf(v, sizeof v, "Громк. %d%%", a_vol * 100 / 0x8000);
    draw_text_r(&FONT_S, SW - 10, 245, C_GRAY, t);
    draw_text_r(&FONT_S, SW - 100, 245, C_GRAY, v);
    fill_rect(10, 264, SW - 20, 3, C_WHITE, 50);
    if (d > 0) {
      int w = (SW - 20) * (s > d ? d : s) / d;
      fill_rect(10, 264, w, 3, C_ACC, 255);
    }
  } else {
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
    snprintf(b, sizeof b, "Размер: %s", sz); draw_text(&FONT_S, 320, y, C_GRAY, b, 146, 0); y += 16;
  }
  char b2[64]; snprintf(b2, sizeof b2, "Тип: %s", type_upper(e->type));
  draw_text(&FONT_S, 320, y, C_ACC, b2, 146, 0); y += 18;
  const char *note = NULL;
  if (e->type == T_VIDEO) note = "Видео: скоро";
  else if (e->type == T_AUDIO && !is_mp3(e->path)) note = "Пока только MP3";
  else if (e->type == T_IMAGE && !is_viewable(e->path)) note = "Пока JPG и PNG";
  else if (e->type == T_AUDIO) note = "X — играть";
  else if (e->type == T_IMAGE) note = "X — открыть";
  else if (e->type == T_DIR) note = "X — войти";
  if (note) draw_text(&FONT_S, 320, y, (e->type == T_VIDEO || !strncmp(note, "Пока", 4)) ? C_YELLOW : C_GREEN, note, 146, 0);
}

static void render_list(View *v, const char *title) {
  fill_rect(8, 32, 300, 204, C_DARK, 150);
  draw_text(&FONT_M, 16, 35, C_ACC, title, 220, 1);
  char b[32]; snprintf(b, sizeof b, "%d / %d", v->n ? v->cursor + 1 : 0, v->n);
  draw_text_r(&FONT_S, 296, 37, C_GRAY, b);
  fill_rect(8, 52, 300, 1, C_ACC, 70);
  if (v->n == 0) draw_text(&FONT_S, 20, 64, C_GRAY, "Здесь пока пусто", 0, 0);
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
      snprintf(b, sizeof b, "всего: %d", cnt[t]);
      draw_text_r(&FONT_S, x + 138, 139, C_GRAY, b);
    } else draw_text(&FONT_M, x + 8, 120, C_GRAY, "Нет файлов", 0, 1);
    char tb[48]; snprintf(tb, sizeof tb, "ТИП: %s", type_upper(t));
    draw_text(&FONT_S, x + 8, 156, sel ? C_ACC : C_GRAY, tb, 130, 0);
  }
  fill_rect(10, 184, 454, 52, C_DARK, 135);
  char b[96];
  snprintf(b, sizeof b, "Найдено файлов: %d", cnt[T_AUDIO] + cnt[T_VIDEO] + cnt[T_IMAGE]);
  draw_text(&FONT_M, 20, 190, C_TITLE, b, 430, 1);
  if (nall >= MAX_ALL) draw_text(&FONT_S, 20, 212, C_YELLOW, "Лимит списка достигнут, используй вкладку «Память PSP»", 430, 0);
  else {
    snprintf(b, sizeof b, "Музыка: %d    Видео: %d    Фото: %d", cnt[T_AUDIO], cnt[T_VIDEO], cnt[T_IMAGE]);
    draw_text(&FONT_S, 20, 212, C_WHITE, b, 430, 0);
  }
}

/* ---- search ---- */
static void render_search(void) {
  fill_rect(10, 34, 460, 26, C_DARK, 155);
  rect_border(10, 34, 460, 26, g_sfocus == 0 ? C_ACC : C_GRAY, g_sfocus == 0 ? 255 : 90, 1);
  if (qn == 0) draw_text(&FONT_M, 18, 38, C_GRAY, "Введите название файла…", 0, 0);
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
  if (qn == 0) snprintf(b, sizeof b, "Результаты появятся здесь");
  else snprintf(b, sizeof b, "Найдено: %d", vsrch.n);
  draw_text(&FONT_S, 18, 97, C_ACC, b, 0, 0);
  fill_rect(10, 113, 460, 1, C_ACC, 60);
  draw_rows(&vsrch, 10, 115, 460, ROWS_S, g_sfocus == 1);
}

/* ---- about ---- */
static void render_about(void) {
  fill_rect(10, 34, 460, 202, C_DARK, 155);
  draw_text(&FONT_L, 20, 38, C_TITLE, "PSP media  v1.0", 0, 1);
  static const char *lines[] = {
    "Плеер для PSP: музыка, фото и видео с карты памяти.",
    "Сейчас умеет: MP3, JPG, PNG. Видео — в следующей версии.",
    "",
    "L / R — переключить вкладку      X — открыть      O — назад",
    "Квадрат — пауза      Start — стоп музыки",
    "Select + Вверх/Вниз — громкость",
    "Select + Влево/Вправо — прошлый/следующий трек",
    "В просмотре фото: L/R — листать, Треугольник — подпись",
    "В поиске: X — добавить букву, Квадрат — стереть,",
    "Треугольник — очистить, Вниз — к результатам",
  };
  for (int i = 0; i < 10; i++) draw_text(&FONT_S, 20, 68 + i * 16, i < 2 ? C_WHITE : C_GRAY, lines[i], 440, 0);
}

static Entry *cur_entry(void) {
  View *v = NULL;
  if (g_tab == 0 && g_sub == 1) v = &vcat;
  else if (g_tab == 1) v = &vmem;
  else if (g_tab == 2 && g_sfocus == 1) v = &vsrch;
  return (v && v->n) ? v->it[v->cursor] : NULL;
}

static void render(void) {
  memcpy(fb, bgimg[g_tab == 0 ? 0 : 1], sizeof fb);
  if (g_tab == 0) fill_hgrad(0, 0, 330, SH, C_DARK, 120, 0);
  else fill_rect(0, 0, SW, SH, C_DARK, 85);
  draw_header();
  const char *hint = "X открыть   O назад   L/R вкладки   Квадрат пауза   Start стоп";
  if (g_tab == 0) {
    if (g_sub == 0) render_home();
    else {
      static const char *names[3] = { "Видео", "Фото", "Музыка" };
      render_list(&vcat, names[g_kind]);
    }
  } else if (g_tab == 1) {
    render_list(&vmem, curdir);
  } else if (g_tab == 2) {
    render_search();
    hint = "Лево/Право буква   X добавить   Квадрат стереть   Вниз результаты";
  } else render_about();
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
      if (is_mp3(e->path)) start_queue(v); else set_msg("Пока поддерживается только MP3");
      break;
    case T_IMAGE:
      if (is_viewable(e->path)) photo_viewer(v, v->cursor); else set_msg("Пока поддерживаются JPG и PNG");
      break;
    case T_VIDEO: set_msg("Воспроизведение видео будет в следующей версии"); break;
    default: set_msg("Этот тип файла не поддерживается");
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
  scePowerSetClockFrequency(333, 333, 166);
  gfx_init();
  sceCtrlSetSamplingCycle(0);
  sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
  int at = sceKernelCreateThread("audio", audio_thread, 0x12, 0x10000, PSP_THREAD_ATTR_USER, NULL);
  if (at >= 0) sceKernelStartThread(at, 0, NULL);
  load_backgrounds();
  init_charset();
  scan_all();
  load_dir("ms0:/");
  home_thumb_load();

  int dirty = 1;
  for (;;) {
    g_frames++;
    unsigned p = read_pad();

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
    if (a_finished) { a_finished = 0; next_track(1); dirty = 1; }

    if (p & PSP_CTRL_LTRIGGER) { g_tab = (g_tab + 3) % 4; g_sub = 0; dirty = 1; }
    if (p & PSP_CTRL_RTRIGGER) { g_tab = (g_tab + 1) % 4; g_sub = 0; dirty = 1; }

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
    }

    /* delayed photo preview (JPEG thumbnail of the highlighted file) */
    Entry *ce = cur_entry();
    const char *cp = ce ? ce->path : "";
    if (strcmp(cp, prev_path)) {
      strncpy(prev_path, cp, 255); prev_path[255] = 0;
      if (prev_img) { free(prev_img); prev_img = NULL; }
      prev_wait = 10; dirty = 1;
    } else if (prev_wait > 0 && --prev_wait == 0) {
      if (ce && ce->type == T_IMAGE && is_jpeg(ce->path) && ce->size < 30u * 1024 * 1024) {
        prev_img = load_thumb(ce->path, &prev_w, &prev_h);
        dirty = 1;
      }
    }

    if (msgtimer > 0 && --msgtimer == 0) dirty = 1;
    if (a_playing && g_frames % 15 == 0) dirty = 1;
    if (g_tab == 2 && g_frames % 30 == 0) dirty = 1;

    if (dirty) { render(); flip(1); dirty = 0; }
    else sceDisplayWaitVblankStart();
  }
  return 0;
}
