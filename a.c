// statusbar-hsdwm (hardcoded config; cleaned up + color-fix)
// if the command stays alive we read lines as they come
// if it exits we respawn after HARD_INTERVAL seconds

#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/Xft/Xft.h>
#include <sys/inotify.h>
#include <sys/poll.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>

#if !defined(MAX)
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif

/* -------------------------
   hardcoded configuration
   edit these values directly
   -------------------------*/

#define HARD_FONT      "xterm-12"
#define HARD_BG        "#ffffff"
#define HARD_FG        "#000000"
#define HARD_FOCUS_BG  "#1e90ff"
#define HARD_WS_COUNT  9
#define HARD_CMD       "date '+%a %b %d %H:%M:%S'"
#define HARD_FULLSCREEN 1
#define HARD_BAR_HEIGHT 28
#define HARD_INTERVAL   1 /* seconds */

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

/* status command runtime state */
static pid_t g_cmd_pid = -1;
static int g_cmd_fd = -1; /* read end */
static char g_status_line[MAX_TEXT] = "";
static char g_cmd_readbuf[4096];
static size_t g_cmd_readpos = 0;
static time_t g_next_spawn = 0;

/* forward */
static void draw_all(void);
static void do_switch(int ws);
static void set_strut(Display *dpy, Window win, int top);
static int spawn_status_cmd(void);
static void stop_status_cmd_and_schedule_restart(int status_interval);

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

/* eWMH current desktop */
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

/* convenience: allocate an XftColor from an XColor; returns 1 on success */
static int alloc_xft_from_xcolor(Display *dpy, Visual *vis, Colormap cmap, const XColor *xc, XftColor *out) {
    XRenderColor rc;
    rc.red   = (unsigned short)xc->red;
    rc.green = (unsigned short)xc->green;
    rc.blue  = (unsigned short)xc->blue;
    rc.alpha = 0xffff;
    if (XftColorAllocValue(dpy, vis, cmap, &rc, out)) return 1;

    /* fallback: try pure black or white based on xc brightness */
    double lum = lum_from_xcolor(xc);
    if (lum > 0.5) {
        XRenderColor rb = { 0, 0, 0, 0xffff };
        return XftColorAllocValue(dpy, vis, cmap, &rb, out);
    } else {
        XRenderColor rw = { 0xffff, 0xffff, 0xffff, 0xffff };
        return XftColorAllocValue(dpy, vis, cmap, &rw, out);
    }
}

/* implementation: set _NET_WM_STRUT and _NET_WM_STRUT_PARTIAL so the dock reserves space */
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

/* spawn the status command with a pipe, nonblocking read end */
static int spawn_status_cmd(void) {
    if (!g_cmd || !g_cmd[0]) return 0;
    int p[2];
    if (pipe(p) < 0) {
        return 0;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(p[0]); close(p[1]);
        return 0;
    }

    if (pid == 0) {
        /* child */
        /* detach stdout (and stderr) to the pipe */
        close(p[0]);
        if (dup2(p[1], STDOUT_FILENO) < 0) _exit(127);
        if (dup2(p[1], STDERR_FILENO) < 0) { /* ignore */ }
        close(p[1]);

        /* ensure we don't inherit fds we shouldn't - optional, but good */
        for (int fd = 3; fd < 256; ++fd) close(fd);

        /* run via sh -c */
        execl("/bin/sh", "sh", "-c", g_cmd, (char*)NULL);
        _exit(127);
    }

    /* parent */
    close(p[1]);
    /* make nonblocking */
    int flags = fcntl(p[0], F_GETFL, 0);
    if (flags >= 0) fcntl(p[0], F_SETFL, flags | O_NONBLOCK);

    g_cmd_pid = pid;
    g_cmd_fd = p[0];
    g_cmd_readpos = 0;
    g_cmd_readbuf[0] = '\0';
    g_status_line[0] = '\0';
    return 1;
}

/* stop existing cmd (if any) and schedule restart after interval seconds */
static void stop_status_cmd_and_schedule_restart(int status_interval) {
    if (g_cmd_fd >= 0) {
        close(g_cmd_fd);
        g_cmd_fd = -1;
    }
    if (g_cmd_pid > 0) {
        int st = 0;
        waitpid(g_cmd_pid, &st, WNOHANG);
        g_cmd_pid = -1;
    }
    g_next_spawn = time(NULL) + (status_interval > 0 ? status_interval : 1);
}

/* internal helper to process bytes read from cmd pipe and update g_status_line when we have a full line */
static void process_cmd_bytes(const char *buf, ssize_t n) {
    if (!buf || n <= 0) return;
    size_t space = sizeof(g_cmd_readbuf) - g_cmd_readpos - 1;
    if ((size_t)n > space) n = (ssize_t)space;
    memcpy(g_cmd_readbuf + g_cmd_readpos, buf, n);
    g_cmd_readpos += n;
    g_cmd_readbuf[g_cmd_readpos] = '\0';

    /* extract last full line (up to newline) */
    char *last_nl = strrchr(g_cmd_readbuf, '\n');
    if (!last_nl) {
        /* no newline yet, don't update g_status_line (unless buffer too full) */
        if (g_cmd_readpos >= sizeof(g_cmd_readbuf) - 2) {
            /* force update with what we have */
            size_t L = g_cmd_readpos;
            if (L >= sizeof(g_status_line)) L = sizeof(g_status_line) - 1;
            memcpy(g_status_line, g_cmd_readbuf, L);
            g_status_line[L] = '\0';
            /* reset buffer */
            g_cmd_readpos = 0;
            g_cmd_readbuf[0] = '\0';
        }
        return;
    }

    /* take the last full line: find previous newline (or start) */
    char *prev = last_nl;
    if (prev != g_cmd_readbuf) {
        char *p = prev - 1;
        while (p >= g_cmd_readbuf && *p != '\n') --p;
        if (p >= g_cmd_readbuf && *p == '\n') prev = p + 1;
        else prev = g_cmd_readbuf;
    } else {
        prev = g_cmd_readbuf;
    }

    /* copy the last full line (strip newline) */
    size_t len = (size_t)(last_nl - prev);
    if (len >= sizeof(g_status_line)) len = sizeof(g_status_line) - 1;
    memcpy(g_status_line, prev, len);
    g_status_line[len] = '\0';

    /* if there is data after last_nl, shift it to the buffer start */
    size_t remain = (size_t)(g_cmd_readpos - (last_nl - g_cmd_readbuf) - 1);
    if (remain > 0) {
        memmove(g_cmd_readbuf, last_nl + 1, remain);
        g_cmd_readpos = remain;
        g_cmd_readbuf[g_cmd_readpos] = '\0';
    } else {
        g_cmd_readpos = 0;
        g_cmd_readbuf[0] = '\0';
    }
}

/* ---------------- draw_all ---------------- */
static void draw_all(void) {
    if (!g_dpy) return;

    char status_text[MAX_TEXT] = "";
    /* use the latest line we have from the spawned command */
    if (g_status_line[0]) {
        strncpy(status_text, g_status_line, sizeof(status_text)-1);
        status_text[sizeof(status_text)-1] = '\0';
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

    /* parse background first */
    if (!parse_color(g_dpy, g_cmap, bg_spec, &g_xc_bg)) {
        g_xc_bg.red = g_xc_bg.green = g_xc_bg.blue = 0xffff;
        g_bg_pixel = WhitePixel(g_dpy, g_scr);
    } else {
        g_bg_pixel = g_xc_bg.pixel;
    }

    /* parse foreground; if parsing fails, pick a contrasting fg based on background luminance */
    if (!parse_color(g_dpy, g_cmap, fg_spec, &g_xc_fg)) {
        double lum_bg = lum_from_xcolor(&g_xc_bg);
        if (lum_bg > 0.5) {
            g_xc_fg.red = g_xc_fg.green = g_xc_fg.blue = 0x0000; /* dark text on light bg */
        } else {
            g_xc_fg.red = g_xc_fg.green = g_xc_fg.blue = 0xffff; /* light text on dark bg */
        }
        XAllocColor(g_dpy, g_cmap, &g_xc_fg);
    }

    /* parse focus color, fallback to a sane blue-ish if missing */
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

    /* allocate Xft colors with safer fallbacks */
    if (!alloc_xft_from_xcolor(g_dpy, vis, g_cmap, &g_xc_fg, &g_xft_fg)) {
        /* last resort: black */
        XRenderColor rb = { 0, 0, 0, 0xffff };
        XftColorAllocValue(g_dpy, vis, g_cmap, &rb, &g_xft_fg);
    }

    double lum_fg = lum_from_xcolor(&g_xc_fg);
    XRenderColor rc_sh;
    if (lum_fg > 0.5)
        rc_sh.red = rc_sh.green = rc_sh.blue = 0; /* dark shadow for light foreground */
    else
        rc_sh.red = rc_sh.green = rc_sh.blue = 0xffff; /* light shadow for dark foreground */
    rc_sh.alpha = 0x8000;
    if (!XftColorAllocValue(g_dpy, vis, g_cmap, &rc_sh, &g_xft_shadow)) {
        /* fallback */
        XRenderColor rb = { 0, 0, 0, 0xffff };
        XftColorAllocValue(g_dpy, vis, g_cmap, &rb, &g_xft_shadow);
    }

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
    wa.background_pixel = g_bg_pixel; /* ensure window background matches requested bg */
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

    /* initial spawn immediately */
    g_cmd_fd = -1;
    g_cmd_pid = -1;
    g_next_spawn = 0;
    spawn_status_cmd();

    draw_all();

    /* main loop - rebuild pollfds each iteration so we include cmd fd when present */
    char inbuf[1024] __attribute__((aligned(__alignof__(struct inotify_event))));
    int status_interval = HARD_INTERVAL;
    if (status_interval <= 0) status_interval = 1;

    while (1) {
        /* rebuild pollfds */
        struct pollfd pfds[4];
        int nfds = 0;
        pfds[nfds].fd = ConnectionNumber(g_dpy);
        pfds[nfds].events = POLLIN;
        nfds++;

        if (inofd >= 0) {
            pfds[nfds].fd = inofd;
            pfds[nfds].events = POLLIN;
            nfds++;
        }

        if (g_cmd_fd >= 0) {
            pfds[nfds].fd = g_cmd_fd;
            pfds[nfds].events = POLLIN;
            nfds++;
        }

        /* compute timeout: small tick to let X events be responsive */
        int timeout = 200; /* ms */
        /* if there's no running cmd, but next_spawn is in the past, spawn immediately */
        time_t now = time(NULL);
        if (g_cmd_fd < 0 && (g_next_spawn == 0 || now >= g_next_spawn)) {
            spawn_status_cmd();
            /* after spawn, redraw immediately */
            draw_all();
            continue;
        }

        int ret = poll(pfds, nfds, timeout);

        if (ret > 0) {
            /* handle X events first */
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

            /* handle inotify if present */
            int idx = 1;
            if (inofd < 0) idx = 1; /* skip */
            if (inofd >= 0) {
                if (pfds[1].revents & POLLIN) {
                    ssize_t len = read(inofd, inbuf, sizeof(inbuf));
                    (void)len;
                    draw_all();
                }
                idx = 2;
            }

            /* handle cmd fd - it will be at pfds[idx] if present */
            if (g_cmd_fd >= 0) {
                int cmd_pfd_index = nfds - 1; /* it was appended last */
                if (pfds[cmd_pfd_index].revents & (POLLIN | POLLHUP | POLLERR)) {
                    char buf[1024];
                    ssize_t r = read(g_cmd_fd, buf, sizeof(buf));
                    if (r > 0) {
                        process_cmd_bytes(buf, r);
                        draw_all();
                    } else if (r == 0) {
                        /* EOF - command exited */
                        stop_status_cmd_and_schedule_restart(status_interval);
                        draw_all();
                    } else {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) {
                            /* error - close and schedule restart */
                            stop_status_cmd_and_schedule_restart(status_interval);
                            draw_all();
                        }
                    }
                }
            }
        } else if (ret == 0) {
            /* timeout */
            /* if command isn't running we may need to spawn after next_spawn */
            time_t now2 = time(NULL);
            if (g_cmd_fd < 0 && g_next_spawn != 0 && now2 >= g_next_spawn) {
                spawn_status_cmd();
                draw_all();
            }
            /* otherwise nothing to do; periodic redraw handled below */
        } else {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
    }

    /* cleanup */
    if (g_draw) XftDrawDestroy(g_draw);
    if (g_font) XftFontClose(g_dpy, g_font);
    if (g_gc_bg) XFreeGC(g_dpy, g_gc_bg);
    if (g_gc_focus) XFreeGC(g_dpy, g_gc_focus);
    if (g_win) XDestroyWindow(g_dpy, g_win);
    if (g_dpy) XCloseDisplay(g_dpy);
    if (inofd >= 0) close(inofd);
    if (g_cmd_fd >= 0) close(g_cmd_fd);
    if (g_cmd_pid > 0) {
        int st = 0;
        waitpid(g_cmd_pid, &st, WNOHANG);
    }
    for (int i = 0; i < g_right_cmds_n; ++i) if (g_right_cmds[i]) free(g_right_cmds[i]);
    return 0;
}

