// statusbar-hsdwm (hardcoded config; no env vars)

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

/* -------------------------
   hardcoded configuration
   edit these values directly
   -------------------------*/

#define HARD_FONT "xterm-12"
#define HARD_BG "#ffffff"
#define HARD_FG "#000000"
#define HARD_FOCUS_BG "#1e90ff"
#define HARD_WS_COUNT 9
#define HARD_CMD "date '+%a %b %d %H:%M:%S'"
#define HARD_FULLSCREEN 1
#define HARD_BAR_HEIGHT 28
#define HARD_INTERVAL 1 /* seconds */

/* right commands: change these to whatever you want
   separate each entry as its own string
   note: keep commands cheap because they run each tick */
#define RIGHT_CMD_COUNT 2
static const char *RIGHT_CMDS[RIGHT_CMD_COUNT] = {
    "uptime",
    "whoami"
};

#define MAX_TEXT 512
#define PADDING 8
#define TAG_PADDING 6
#define TAG_SPACING 12
#define MAX_WS 20
#define MAX_RIGHT_CMDS 32

typedef struct { int x, w; int tag; } TagRect;

/* ---------------- global-ish state ---------------- */
static Display *g_dpy = NULL;
static int g_scr = 0;
static Window g_root;
static Colormap g_cmap;
static int g_screen_w;
static int g_bar_h = HARD_BAR_HEIGHT;
static XftFont *g_font = NULL;
static XftDraw *g_draw = NULL;
static XftColor g_xft_fg, g_xft_shadow, g_xft_focus_text;
static XColor g_xc_bg, g_xc_fg, g_xc_focus;
static unsigned long g_bg_pixel;
static Window g_win;
static TagRect g_tagrects[MAX_WS];
static int g_tagrects_n = 0;
static int g_ws_count = HARD_WS_COUNT;
static char g_focused_path[PATH_MAX] = "";
static char g_occupied_path[PATH_MAX] = "";
static const char *g_cmd = HARD_CMD;
static const char *g_switch_fmt = NULL; /* keep NULL: hardcode if you want */
static GC g_gc_bg = NULL;
static GC g_gc_focus = NULL;
static int g_fullscreen = HARD_FULLSCREEN;
static char *g_right_cmds[MAX_RIGHT_CMDS];
static int g_right_cmds_n = 0;

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

/* run a command and store at most outlen-1 bytes in out */
static void run_cmd_to_buf(const char *cmd, char *out, size_t outlen) {
    if (!cmd || !out || outlen == 0) return;
    FILE *f = popen(cmd, "r");
    if (!f) { out[0] = '\0'; return; }
    if (!fgets(out, (int)outlen, f)) out[0] = '\0';
    size_t L = strlen(out);
    if (L && out[L-1] == '\n') out[L-1] = '\0';
    pclose(f);
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
    return (int)v + 1;
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
    char try1[64];
    snprintf(try1, sizeof(try1), "wmctrl -s %d >/dev/null 2>&1", ws - 1);
    if (system(try1) == 0) return;
    char try2[64];
    snprintf(try2, sizeof(try2), "xdotool key super+%d >/dev/null 2>&1", ws % 10);
    system(try2);
}

/* draw_all: recompute content width, resize window centered, draw tags, status, and right modules */
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

    /* build right text by running each right cmd and joining with two spaces */
    char right_text[MAX_TEXT] = "";
    if (g_right_cmds_n > 0) {
        char tmp[MAX_TEXT];
        for (int i = 0; i < g_right_cmds_n; ++i) {
            tmp[0] = '\0';
            run_cmd_to_buf(g_right_cmds[i], tmp, sizeof(tmp));
            if (tmp[0]) {
                if (right_text[0]) strncat(right_text, "  ", sizeof(right_text) - strlen(right_text) - 1);
                strncat(right_text, tmp, sizeof(right_text) - strlen(right_text) - 1);
            }
        }
    }

    /* focused workspace -> prefer file, else EWMH */
    int focused_ws = 1;
    char fb[32] = "";
    FILE *f = NULL;
    if (g_focused_path[0]) {
        f = fopen(g_focused_path, "r");
        if (f) {
            if (fgets(fb, sizeof(fb), f)) {
                size_t L = strlen(fb);
                if (L && fb[L-1] == '\n') fb[L-1] = '\0';
            }
            fclose(f);
        }
    }
    if (fb[0]) focused_ws = atoi(fb);
    else {
        int e = get_ewmh_current_desktop(g_dpy);
        if (e > 0) focused_ws = e;
    }
    if (focused_ws < 1) focused_ws = 1;
    if (focused_ws > g_ws_count) focused_ws = g_ws_count;

    int occupied[MAX_WS + 1] = {0};
    if (g_occupied_path[0]) {
        char occ[256] = "";
        FILE *fo = fopen(g_occupied_path, "r");
        if (fo) {
            size_t idx = 0; int c;
            while ((c = fgetc(fo)) != EOF && idx + 1 < sizeof(occ)) occ[idx++] = (char)c;
            occ[idx] = '\0';
            fclose(fo);
        }
        char *p = occ;
        while (*p) {
            while (*p && !isdigit((unsigned char)*p)) ++p;
            if (!*p) break;
            char *end = NULL;
            long v = strtol(p, &end, 10);
            if (end == p) break;
            if (v >= 1 && v <= g_ws_count) occupied[v] = 1;
            p = end;
        }
    }
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

    XGlyphInfo gright;
    XftTextExtentsUtf8(g_dpy, g_font, (FcChar8*)right_text, strlen(right_text), &gright);
    int right_w = (int)gright.xOff;

    /* compute content width including right area */
    int content_w = left_width + status_w + right_w + PADDING * 3;
    if (content_w < 200) content_w = 200;

    g_screen_w = DisplayWidth(g_dpy, g_scr);
    int win_x = (g_screen_w - content_w) / 2;
    if (win_x < 0) win_x = 0;

    if (g_fullscreen) {
        content_w = g_screen_w;
        win_x = 0;
    }

    XMoveResizeWindow(g_dpy, g_win, win_x, 0, content_w, g_bar_h);
    XSync(g_dpy, False);

    XFillRectangle(g_dpy, g_win, g_gc_bg, 0, 0, content_w, g_bar_h);

    int text_y = g_font->ascent + (g_bar_h - (g_font->ascent + g_font->descent)) / 2;
    for (int i = 0; i < g_tagrects_n; ++i) {
        int tag = g_tagrects[i].tag;
        char tb[4];
        snprintf(tb, sizeof(tb), "%d", tag);
        int tx = g_tagrects[i].x;
        int w = g_tagrects[i].w;

        if (tag == focused_ws) {
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

    /* compute positions:
       left end     = left_width
       right start  = content_w - PADDING - right_w
       status area  = between left_end + PADDING  and  right_start - PADDING
    */
    int base_left_end = left_width;
    int right_start = content_w - PADDING - right_w;
    if (right_start < base_left_end + PADDING) {
        right_start = base_left_end + PADDING;
    }

    int status_area_left = base_left_end + PADDING;
    int status_area_right = right_start - PADDING;
    if (g_fullscreen) {
        status_area_left = 0;
        status_area_right = content_w;
    }

    int inner_w = status_area_right - status_area_left;
    int status_x = status_area_left;
    if (inner_w > 0) {
        if (status_w < inner_w) {
            status_x = status_area_left + (inner_w - status_w) / 2;
        } else {
            status_x = status_area_left;
        }
    } else {
        status_x = status_area_left;
    }

    if (status_x < 0) status_x = 0;
    if (status_x + status_w > content_w) {
        if (status_w >= content_w) status_x = 0;
        else status_x = content_w - status_w;
    }

    if (status_text[0]) {
        XftDrawStringUtf8(g_draw, &g_xft_shadow, g_font,
                         status_x + 1, text_y + 1,
                         (FcChar8*)status_text, strlen(status_text));
        XftDrawStringUtf8(g_draw, &g_xft_fg, g_font,
                         status_x, text_y,
                         (FcChar8*)status_text, strlen(status_text));
    }

    if (right_text[0]) {
        int right_draw_x = right_start;
        if (right_draw_x < 0) right_draw_x = 0;
        XftDrawStringUtf8(g_draw, &g_xft_shadow, g_font,
                         right_draw_x + 1, text_y + 1,
                         (FcChar8*)right_text, strlen(right_text));
        XftDrawStringUtf8(g_draw, &g_xft_fg, g_font,
                         right_draw_x, text_y,
                         (FcChar8*)right_text, strlen(right_text));
    }

    XFlush(g_dpy);
}

/* ---------------- main ---------------- */
int main(void) {
    const char *fontname = HARD_FONT;
    const char *bg_spec  = HARD_BG;
    const char *fg_spec  = HARD_FG;
    const char *focus_spec = HARD_FOCUS_BG;
    g_cmd = HARD_CMD;
    g_switch_fmt = NULL;
    g_ws_count = HARD_WS_COUNT;
    if (g_ws_count <= 0) g_ws_count = 1;
    if (g_ws_count > MAX_WS) g_ws_count = MAX_WS;

    g_fullscreen = HARD_FULLSCREEN;

    /* populate right cmds from RIGHT_CMDS array */
    g_right_cmds_n = 0;
    for (int i = 0; i < RIGHT_CMD_COUNT && g_right_cmds_n < MAX_RIGHT_CMDS; ++i) {
        if (RIGHT_CMDS[i] && RIGHT_CMDS[i][0]) {
            g_right_cmds[g_right_cmds_n++] = strdup(RIGHT_CMDS[i]);
        }
    }

    const char *home = getenv("HOME");
    if (home) {
        snprintf(g_focused_path, sizeof(g_focused_path), "%s/.wm/focused.workspace", home);
        snprintf(g_occupied_path, sizeof(g_occupied_path), "%s/.wm/occupied.workspace", home);
    } else {
        g_focused_path[0] = g_occupied_path[0] = 0;
    }

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

    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) { fprintf(stderr, "cannot open display\n"); return 1; }
    g_scr = DefaultScreen(g_dpy);
    g_root = RootWindow(g_dpy, g_scr);
    g_cmap = DefaultColormap(g_dpy, g_scr);
    g_screen_w = DisplayWidth(g_dpy, g_scr);
    g_bar_h = HARD_BAR_HEIGHT;
    if (g_bar_h <= 0) g_bar_h = 28;

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

    Visual *vis = DefaultVisual(g_dpy, g_scr);
    g_font = XftFontOpenName(g_dpy, g_scr, fontname);
    if (!g_font) g_font = XftFontOpenName(g_dpy, g_scr, "xterm-12");
    if (!g_font) g_font = XftFontOpenName(g_dpy, g_scr, "monospace-12");
    if (!g_font) {
        fprintf(stderr, "failed to open Xft font; try installing fonts or set HARD_FONT to a valid Fc name\n");
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

    XSetWindowAttributes wa;
    wa.override_redirect = False;
    wa.background_pixel = 0;
    wa.event_mask = ExposureMask | ButtonPressMask | StructureNotifyMask;
    g_win = XCreateWindow(g_dpy, g_root, 0, 0, 200, g_bar_h, 0, DefaultDepth(g_dpy, g_scr),
                          CopyFromParent, DefaultVisual(g_dpy, g_scr),
                          CWBackPixel | CWEventMask, &wa);

    Atom a_type = XInternAtom(g_dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom a_type_dock = XInternAtom(g_dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    if (a_type && a_type_dock)
        XChangeProperty(g_dpy, g_win, a_type, XA_ATOM, 32, PropModeReplace,
                       (unsigned char *)&a_type_dock, 1);

    Atom a_state = XInternAtom(g_dpy, "_NET_WM_STATE", False);
    Atom a_state_above = XInternAtom(g_dpy, "_NET_WM_STATE_ABOVE", False);
    Atom a_state_sticky = XInternAtom(g_dpy, "_NET_WM_STATE_STICKY", False);
    Atom states[2];
    int nstates = 0;
    if (a_state_above) states[nstates++] = a_state_above;
    if (a_state_sticky) states[nstates++] = a_state_sticky;
    if (nstates && a_state)
        XChangeProperty(g_dpy, g_win, a_state, XA_ATOM, 32, PropModeReplace,
                        (unsigned char*)states, nstates);

    Atom a_pid = XInternAtom(g_dpy, "_NET_WM_PID", False);
    if (a_pid) {
        unsigned long pid = (unsigned long)getpid();
        XChangeProperty(g_dpy, g_win, a_pid, XA_CARDINAL, 32, PropModeReplace,
                        (unsigned char*)&pid, 1);
    }

    g_gc_bg = XCreateGC(g_dpy, g_win, 0, NULL);
    XSetForeground(g_dpy, g_gc_bg, g_bg_pixel);

    g_gc_focus = XCreateGC(g_dpy, g_win, 0, NULL);
    XSetForeground(g_dpy, g_gc_focus, g_xc_focus.pixel);

    g_draw = XftDrawCreate(g_dpy, g_win, vis, g_cmap);

    set_strut(g_dpy, g_win, g_bar_h);

    XMapWindow(g_dpy, g_win);

    struct pollfd pfds[2];
    pfds[0].fd = ConnectionNumber(g_dpy);
    pfds[0].events = POLLIN;
    int nfds = 1;
    if (inofd >= 0) {
        pfds[1].fd = inofd;
        pfds[1].events = POLLIN;
        nfds = 2;
    }

    draw_all();

    int tick_ms = 100;
    int status_interval = HARD_INTERVAL;
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
                        int cx = ev.xbutton.x;
                        for (int i = 0; i < g_tagrects_n; ++i) {
                            if (cx >= g_tagrects[i].x && cx < g_tagrects[i].x + g_tagrects[i].w) {
                                do_switch(g_tagrects[i].tag);
                                draw_all();
                                break;
                            }
                        }
                    } else if (ev.type == ConfigureNotify) {
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

    if (g_draw) XftDrawDestroy(g_draw);
    if (g_font) XftFontClose(g_dpy, g_font);
    if (g_gc_bg) XFreeGC(g_dpy, g_gc_bg);
    if (g_gc_focus) XFreeGC(g_dpy, g_gc_focus);
    if (g_win) XDestroyWindow(g_dpy, g_win);
    if (g_dpy) XCloseDisplay(g_dpy);
    if (inofd >= 0) close(inofd);
    for (int i = 0; i < g_right_cmds_n; ++i) if (g_right_cmds[i]) free(g_right_cmds[i]);
    return 0;
}

