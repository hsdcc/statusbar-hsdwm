// Optimized X11 status bar with reduced memory usage
// Shows all occupied workspaces (accepts formats like "1,2,3" or "1 2 3")

#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/Xft/Xft.h>
#include <sys/inotify.h>
#include <sys/poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>
#include <stdarg.h>

#define DEFAULT_FONT "xterm-12"
#define DEFAULT_BG "#ffffff"
#define DEFAULT_FG "#000000"
#define DEFAULT_FOCUS_BG "#1e90ff"
#define DEFAULT_WS_COUNT 9
#define DEFAULT_CMD "date '+%a %b %d %H:%M:%S'"

#define MAX_TEXT 512
#define PADDING 8
#define TAG_PADDING 6
#define TAG_SPACING 12
#define MAX_WS 20  // Reasonable maximum for workspaces

typedef struct { int x, w; int tag; } TagRect;

static const char *env_or(const char *k, const char *d) {
    const char *v = getenv(k);
    return v ? v : d;
}

static void read_firstline(const char *path, char *out, size_t outlen) {
    if (!path || !out || outlen == 0) return;
    FILE *f = fopen(path, "r");
    if (!f) { out[0] = '\0'; return; }
    if (!fgets(out, (int)outlen, f)) out[0] = '\0';
    size_t L = strlen(out);
    if (L && out[L-1] == '\n') out[L-1] = '\0';
    fclose(f);
}

static void read_whole(const char *path, char *out, size_t outlen) {
    if (!path || !out || outlen == 0) return;
    FILE *f = fopen(path, "r");
    if (!f) { out[0] = '\0'; return; }
    size_t idx = 0; int c;
    while ((c = fgetc(f)) != EOF && idx + 1 < outlen) out[idx++] = (char)c;
    out[idx] = '\0';
    fclose(f);
}

static int parse_color(Display *dpy, Colormap cmap, const char *spec, XColor *out) {
    XColor tmp;
    if (!spec) return 0;
    if (!XParseColor(dpy, cmap, spec, &tmp)) return 0;
    if (!XAllocColor(dpy, cmap, &tmp)) return 0;
    *out = tmp;
    return 1;
}

static double lum_from_xcolor(const XColor *xc) {
    double r = (double)xc->red / 65535.0;
    double g = (double)xc->green / 65535.0;
    double b = (double)xc->blue / 65535.0;
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

static void set_strut(Display *dpy, Window win, int top) {
    Atom a_strut = XInternAtom(dpy, "_NET_WM_STRUT", False);
    Atom a_strut_partial = XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL", False);
    if (!a_strut || !a_strut_partial) return;
    long strut[4] = {0, 0, top, 0};
    long partial[12] = {0};
    partial[2] = top;
    XChangeProperty(dpy, win, a_strut, XA_CARDINAL, 32, PropModeReplace, (unsigned char*)strut, 4);
    XChangeProperty(dpy, win, a_strut_partial, XA_CARDINAL, 32, PropModeReplace, (unsigned char*)partial, 12);
}

static int get_ewmh_current_desktop(Display *dpy) {
    Atom a = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
    if (!a) return -1;
    Atom type; int format; unsigned long nitems, after;
    unsigned char *data = NULL;
    int res = XGetWindowProperty(dpy, DefaultRootWindow(dpy), a, 0, 1, False, AnyPropertyType,
                                 &type, &format, &nitems, &after, &data);
    if (res != Success || !data) return -1;
    long v = ((long*)data)[0];
    XFree(data);
    return (int)v + 1; // 1-based
}

/* ---------------- global-ish state ---------------- */
static Display *g_dpy = NULL;
static int g_scr = 0;
static Window g_root;
static Colormap g_cmap;
static int g_screen_w;
static int g_bar_h = 28;
static XftFont *g_font = NULL;
static XftDraw *g_draw = NULL;
static XftColor g_xft_fg, g_xft_shadow, g_xft_focus_text;
static XColor g_xc_bg, g_xc_fg, g_xc_focus;
static unsigned long g_bg_pixel;
static Window g_win;
static TagRect g_tagrects[MAX_WS];
static int g_tagrects_n = 0;
static int g_ws_count = DEFAULT_WS_COUNT;
static char g_focused_path[PATH_MAX] = "";
static char g_occupied_path[PATH_MAX] = "";
static const char *g_cmd = NULL;
static const char *g_switch_fmt = NULL;
static GC g_gc_bg = NULL;
static GC g_gc_focus = NULL;

/* forward */
static void draw_all(void);
static void do_switch(int ws);

/* helper run system with formatted string (safe-ish) */
static void run_format(const char *fmt, ...) {
    if (!fmt) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (buf[0]) system(buf);
}

/* switch workspace: format command with %d if provided, else try wmctrl fallback, then xdotool fallback */
static void do_switch(int ws) {
    if (ws < 1) return;
    if (g_switch_fmt && strchr(g_switch_fmt, '%')) {
        char cmdbuf[256];
        int n = snprintf(cmdbuf, sizeof(cmdbuf), g_switch_fmt, ws);
        if (n > 0 && n < (int)sizeof(cmdbuf)) {
            system(cmdbuf);
            return;
        }
    }
    /* fallback: wmctrl expects 0-based desktop index */
    char try1[64];
    snprintf(try1, sizeof(try1), "wmctrl -s %d >/dev/null 2>&1", ws - 1);
    if (system(try1) == 0) return;
    /* fallback: xdotool send super+N (best-effort) */
    char try2[64];
    snprintf(try2, sizeof(try2), "xdotool key super+%d >/dev/null 2>&1", ws % 10);
    system(try2);
}

/* draw_all: recompute content width, resize window centered, draw tags and status */
static void draw_all(void) {
    if (!g_dpy) return;

    char status_text[MAX_TEXT] = "";
    if (g_cmd) {
        FILE *f = popen(g_cmd, "r");
        if (f) {
            if (fgets(status_text, sizeof(status_text), f)) {
                size_t L = strlen(status_text); 
                if (L && status_text[L-1] == '\n') status_text[L-1] = '\0';
            } else status_text[0] = '\0';
            pclose(f);
        } else snprintf(status_text, sizeof(status_text), "(failed cmd)");
    }

    /* focused workspace -> prefer file, else EWMH */
    int focused_ws = 1;
    char fb[32] = "";
    read_firstline(g_focused_path, fb, sizeof(fb));
    if (fb[0]) focused_ws = atoi(fb);
    else {
        int e = get_ewmh_current_desktop(g_dpy);
        if (e > 0) focused_ws = e;
    }
    if (focused_ws < 1) focused_ws = 1;
    if (focused_ws > g_ws_count) focused_ws = g_ws_count;

    /* occupied workspaces: support commas, spaces, newlines, etc. */
    int occupied[MAX_WS + 1] = {0};
    if (g_occupied_path[0]) {
        char occ[256] = "";
        read_whole(g_occupied_path, occ, sizeof(occ));
        char *p = occ;
        while (*p) {
            /* skip non-digits */
            while (*p && !isdigit((unsigned char)*p)) ++p;
            if (!*p) break;
            char *end = NULL;
            long v = strtol(p, &end, 10);
            if (end == p) break;
            if (v >= 1 && v <= g_ws_count) occupied[v] = 1;
            p = end;
        }
    }

    // Always mark focused workspace as occupied
    occupied[focused_ws] = 1;

    /* measure tags widths */
    int x = PADDING;
    g_tagrects_n = 0;

    for (int i = 1; i <= g_ws_count; ++i) {
        if (!occupied[i]) continue;

        char tb[4]; 
        snprintf(tb, sizeof(tb), "%d", i);
        XGlyphInfo ginfo;
        XftTextExtentsUtf8(g_dpy, g_font, (FcChar8*)tb, strlen(tb), &ginfo);
        int w = (int)ginfo.xOff + TAG_PADDING;

        if (g_tagrects_n < MAX_WS) {
            g_tagrects[g_tagrects_n].x = x;
            g_tagrects[g_tagrects_n].w = w;
            g_tagrects[g_tagrects_n].tag = i;
            g_tagrects_n++;
        }
        x += w + TAG_SPACING;
    }

    int left_width = x;
    XGlyphInfo gstatus;
    XftTextExtentsUtf8(g_dpy, g_font, (FcChar8*)status_text, strlen(status_text), &gstatus);
    int status_w = (int)gstatus.xOff;
    int content_w = left_width + status_w + PADDING * 2;
    if (content_w < 200) content_w = 200;

    /* center on top */
    g_screen_w = DisplayWidth(g_dpy, g_scr);
    int win_x = (g_screen_w - content_w) / 2;
    if (win_x < 0) win_x = 0;
    XMoveResizeWindow(g_dpy, g_win, win_x, 0, content_w, g_bar_h);
    XSync(g_dpy, False);

    /* clear window background with bg_pixel for the content area */
    XFillRectangle(g_dpy, g_win, g_gc_bg, 0, 0, content_w, g_bar_h);

    /* draw tags */
    int text_y = g_font->ascent + (g_bar_h - (g_font->ascent + g_font->descent)) / 2;
    for (int i = 0; i < g_tagrects_n; ++i) {
        int tag = g_tagrects[i].tag;
        char tb[4]; 
        snprintf(tb, sizeof(tb), "%d", tag);
        int tx = g_tagrects[i].x;
        int w = g_tagrects[i].w;

        if (tag == focused_ws) {
            /* focus bg */
            int ry = (g_bar_h - (g_font->ascent + g_font->descent)) / 2 - 2;
            if (ry < 0) ry = 0;
            XFillRectangle(g_dpy, g_win, g_gc_focus, tx - 2, ry, w + 4, 
                          g_font->ascent + g_font->descent + 4);
            XftDrawStringUtf8(g_draw, &g_xft_focus_text, g_font, 
                             tx + TAG_PADDING / 2, text_y, 
                             (FcChar8*)tb, strlen(tb));
        } else {
            XftDrawStringUtf8(g_draw, &g_xft_fg, g_font, 
                             tx + TAG_PADDING / 2, text_y, 
                             (FcChar8*)tb, strlen(tb));
        }
    }

    /* draw status centered in remaining area */
    int status_x = left_width + PADDING;
    int inner_w = content_w - left_width - PADDING;
    int status_off = 0;
    if (status_w < inner_w) status_off = (inner_w - status_w) / 2;

    XftDrawStringUtf8(g_draw, &g_xft_shadow, g_font, 
                     status_x + status_off + 1, text_y + 1, 
                     (FcChar8*)status_text, strlen(status_text));
    XftDrawStringUtf8(g_draw, &g_xft_fg, g_font, 
                     status_x + status_off, text_y, 
                     (FcChar8*)status_text, strlen(status_text));
    XFlush(g_dpy);
}

/* ---------------- main ---------------- */
int main(void) {
    const char *fontname = env_or("XSTATUS_FONT", DEFAULT_FONT);
    const char *bg_spec  = env_or("XSTATUS_BG", DEFAULT_BG);
    const char *fg_spec  = env_or("XSTATUS_FG", DEFAULT_FG);
    const char *focus_spec = env_or("XSTATUS_FG_FOCUS", DEFAULT_FOCUS_BG);
    g_cmd = env_or("XSTATUS_CMD", DEFAULT_CMD);
    g_switch_fmt = getenv("XSTATUS_WS_SWITCH_CMD"); /* optional */
    g_ws_count = atoi(env_or("XSTATUS_WS_COUNT", "9"));
    if (g_ws_count <= 0) g_ws_count = DEFAULT_WS_COUNT;
    if (g_ws_count > MAX_WS) g_ws_count = MAX_WS;

    /* paths */
    const char *home = getenv("HOME");
    if (home) {
        snprintf(g_focused_path, sizeof(g_focused_path), "%s/.wm/focused.workspace", home);
        snprintf(g_occupied_path, sizeof(g_occupied_path), "%s/.wm/occupied.workspace", home);
    } else { 
        g_focused_path[0] = g_occupied_path[0] = 0; 
    }

    /* inotify */
    int inofd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    int watch_focused = -1, watch_occupied = -1;
    if (inofd >= 0) {
        if (g_focused_path[0]) 
            watch_focused = inotify_add_watch(inofd, g_focused_path, 
                                            IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM);
        if (g_occupied_path[0]) 
            watch_occupied = inotify_add_watch(inofd, g_occupied_path, 
                                             IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM);
    }

    /* X setup */
    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) { fprintf(stderr, "cannot open display\n"); return 1; }
    g_scr = DefaultScreen(g_dpy);
    g_root = RootWindow(g_dpy, g_scr);
    g_cmap = DefaultColormap(g_dpy, g_scr);
    g_screen_w = DisplayWidth(g_dpy, g_scr);
    g_bar_h = atoi(env_or("XSTATUS_HEIGHT", "28")); 
    if (g_bar_h <= 0) g_bar_h = 28;

    /* colors */
    if (!parse_color(g_dpy, g_cmap, bg_spec, &g_xc_bg)) { 
        g_xc_bg.red = g_xc_bg.green = g_xc_bg.blue = 0xffff; 
        g_bg_pixel = WhitePixel(g_dpy, g_scr); 
    } else {
        g_bg_pixel = g_xc_bg.pixel;
    }
    
    if (!parse_color(g_dpy, g_cmap, fg_spec, &g_xc_fg)) 
        g_xc_fg.red = g_xc_fg.green = g_xc_fg.blue = 0x0000;
    
    if (!parse_color(g_dpy, g_cmap, focus_spec, &g_xc_focus)) { 
        g_xc_focus.red = 0x1e00; 
        g_xc_focus.green = 0x9000; 
        g_xc_focus.blue = 0xff00; 
        XAllocColor(g_dpy, g_cmap, &g_xc_focus); 
    }

    /* Xft font + colors */
    Visual *vis = DefaultVisual(g_dpy, g_scr);
    g_font = XftFontOpenName(g_dpy, g_scr, fontname);
    if (!g_font) g_font = XftFontOpenName(g_dpy, g_scr, "xterm-12");
    if (!g_font) g_font = XftFontOpenName(g_dpy, g_scr, "monospace-12");
    if (!g_font) { 
        fprintf(stderr, "failed to open Xft font; try installing fonts or set XSTATUS_FONT to a valid Fc name\n"); 
        XCloseDisplay(g_dpy); 
        return 1; 
    }

    XRenderColor rc_fg = { 
        (unsigned short)g_xc_fg.red, 
        (unsigned short)g_xc_fg.green, 
        (unsigned short)g_xc_fg.blue, 
        0xffff 
    };
    
    if (!XftColorAllocValue(g_dpy, vis, g_cmap, &rc_fg, &g_xft_fg)) { 
        XRenderColor fb = {0, 0, 0, 0xffff}; 
        XftColorAllocValue(g_dpy, vis, g_cmap, &fb, &g_xft_fg); 
    }

    XRenderColor rc_sh;
    if ((rc_fg.red > 0x7fff) && (rc_fg.green > 0x7fff) && (rc_fg.blue > 0x7fff)) 
        rc_sh.red = rc_sh.green = rc_sh.blue = 0x0000;
    else 
        rc_sh.red = rc_sh.green = rc_sh.blue = 0xffff;
    
    rc_sh.alpha = 0x8000;
    XftColorAllocValue(g_dpy, vis, g_cmap, &rc_sh, &g_xft_shadow);

    double lum_focus = lum_from_xcolor(&g_xc_focus);
    XRenderColor rc_focus_text;
    if (lum_focus > 0.5) 
        rc_focus_text.red = rc_focus_text.green = rc_focus_text.blue = 0x0000;
    else 
        rc_focus_text.red = rc_focus_text.green = rc_focus_text.blue = 0xffff;
    
    rc_focus_text.alpha = 0xffff;
    XftColorAllocValue(g_dpy, vis, g_cmap, &rc_focus_text, &g_xft_focus_text);

    /* create window (small width initially) */
    XSetWindowAttributes wa;
    wa.override_redirect = True;
    wa.background_pixel = 0; /* we draw content area ourselves */
    wa.event_mask = ExposureMask | ButtonPressMask | StructureNotifyMask;
    g_win = XCreateWindow(g_dpy, g_root, 0, 0, 200, g_bar_h, 0, DefaultDepth(g_dpy, g_scr),
                          CopyFromParent, DefaultVisual(g_dpy, g_scr),
                          CWOverrideRedirect | CWBackPixel | CWEventMask, &wa);

    /* hint dock */
    Atom a_type = XInternAtom(g_dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom a_type_dock = XInternAtom(g_dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    if (a_type && a_type_dock) 
        XChangeProperty(g_dpy, g_win, a_type, XA_ATOM, 32, PropModeReplace, 
                       (unsigned char *)&a_type_dock, 1);

    /* Create GCs once for reuse */
    g_gc_bg = XCreateGC(g_dpy, g_win, 0, NULL);
    XSetForeground(g_dpy, g_gc_bg, g_bg_pixel);
    
    g_gc_focus = XCreateGC(g_dpy, g_win, 0, NULL);
    XSetForeground(g_dpy, g_gc_focus, g_xc_focus.pixel);

    /* Xft draw binding */
    g_draw = XftDrawCreate(g_dpy, g_win, vis, g_cmap);

    XMapRaised(g_dpy, g_win);
    set_strut(g_dpy, g_win, g_bar_h);

    /* poll fds: X connection + inotify (if available) */
    struct pollfd pfds[2];
    pfds[0].fd = ConnectionNumber(g_dpy); 
    pfds[0].events = POLLIN;
    int nfds = 1;
    if (inofd >= 0) { 
        pfds[1].fd = inofd; 
        pfds[1].events = POLLIN; 
        nfds = 2; 
    }

    /* initial draw */
    draw_all();

    /* event loop */
    int tick_ms = 100;
    int status_interval = atoi(env_or("XSTATUS_INTERVAL", "1"));
    if (status_interval <= 0) status_interval = 1;
    int tick_counter = 0;
    char inbuf[1024] __attribute__((aligned(__alignof__(struct inotify_event))));

    while (1) {
        int timeout = tick_ms;
        int ret = poll(pfds, nfds, timeout);
        if (ret > 0) {
            if (pfds[0].revents & POLLIN) {
                while (XPending(g_dpy)) {
                    XEvent ev;
                    XNextEvent(g_dpy, &ev);
                    if (ev.type == ButtonPress) {
                        int cx = ev.xbutton.x; /* window-relative */
                        for (int i = 0; i < g_tagrects_n; ++i) {
                            if (cx >= g_tagrects[i].x && cx < g_tagrects[i].x + g_tagrects[i].w) {
                                do_switch(g_tagrects[i].tag);
                                draw_all();
                                break;
                            }
                        }
                    } else if (ev.type == ConfigureNotify) {
                        /* re-center if screen dims changed */
                        g_screen_w = DisplayWidth(g_dpy, g_scr);
                        draw_all();
                    }
                }
            }
            if (nfds == 2 && (pfds[1].revents & POLLIN)) {
                ssize_t len = read(inofd, inbuf, sizeof(inbuf));
                (void)len;
                draw_all();
            }
        } else if (ret == 0) {
            tick_counter += tick_ms;
            if (tick_counter >= status_interval * 1000) {
                tick_counter = 0;
                draw_all();
            }
        } else {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
    }

    /* cleanup (normally not reached) */
    if (g_draw) XftDrawDestroy(g_draw);
    if (g_font) XftFontClose(g_dpy, g_font);
    if (g_gc_bg) XFreeGC(g_dpy, g_gc_bg);
    if (g_gc_focus) XFreeGC(g_dpy, g_gc_focus);
    if (g_win) XDestroyWindow(g_dpy, g_win);
    if (g_dpy) XCloseDisplay(g_dpy);
    if (inofd >= 0) close(inofd);
    return 0;
}

