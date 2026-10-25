/*
 * mango-bar — lightweight top status bar for MangoWM (elish).
 *
 * Layout (design: docs/bar-design.svg, light "Tablet Status Bar"):
 *
 *   [foot · ~] [Firefox · Design] …   周日,10/25 23:37:24   (🔉 📶 ᛒ)   (⏻)
 *
 * Left   : one rounded rect per window of the current workspace, focused = white.
 * Middle : clock.
 * Right  : a pill holding three independently clickable icons (speaker, wifi,
 *          bluetooth), each opening its own settings tool, plus a power button
 *          that needs a second click before it powers off.
 *
 * Wayland layer-shell + cairo on a rotating pair of shm buffers. No GTK/Qt.
 */
#define _GNU_SOURCE
#include <cairo/cairo.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define BAR_H        54
#define EDGE         8
#define BAR_H_INNER  (BAR_H - EDGE * 2)
#define RADIUS       16
#define PILL_H       34
#define PILL_R       11
#define PILL_PAD_X   12
#define PILL_MAX_W   190
#define PILL_GAP     7
#define MAX_PILLS    4
#define TRAY_W       130
#define TRAY_H       34
#define POWER_D      34

#define C_BAR_BG      "#F7F8FA"
#define C_BAR_BD      "#E2E6ED"
#define C_PILL_ACT    "#FFFFFF"
#define C_PILL_ACT_BD "#D8DCE3"
#define C_PILL_FG_ACT "#1F2430"
#define C_PILL_IDLE   "#ECEEF2"
#define C_PILL_FG_IDLE "#6B7280"
#define C_CLOCK       "#1F2430"
#define C_TRAY_BG     "#E9ECF1"
#define C_TRAY_BD     "#DDE1E8"
#define C_ICON        "#4B5563"
#define C_ICON_OFF    "#AEB6C2"
#define C_POWER_BG    "#FBEAEC"
#define C_POWER_BD    "#F3D0D5"
#define C_POWER_FG    "#D04F61"

#define FONT_UI       "Noto Sans CJK SC"

struct client { int id; char appid[64]; char title[192]; int focused; int visible; };
struct box { double x, y, w, h; };

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct wl_output *output;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zwlr_layer_surface_v1 *layer_surface;
static struct wl_surface *surface;

static int scale = 2;
static int bar_w = 1280, buf_w = 2560, buf_h = BAR_H * 2;
static struct client clients[MAX_PILLS];
static int nclients;
static char clock_str[64];
static int vol_pct = 0, vol_mute = 0, wifi_on = 0, bt_on = 0;
static int power_armed = 0;
static time_t power_armed_at = 0;
static int dirty = 1;
static int pointer_x = -1, pointer_y = -1;

static char cmd_audio[256] = "foot -e alsamixer";
static char cmd_network[256] = "foot -e nmtui";
static char cmd_bluetooth[256] = "foot -e bluetoothctl";

static struct box pill_box[MAX_PILLS];
static int n_pill_boxes;
static struct box power_box, tray_box, tray_icon[3];

static void repaint(void);

/* ------------------------------------------------------------------ util */

static void set_hex(cairo_t *cr, const char *hex, double a) {
	unsigned r = 0, g = 0, b = 0;
	if (hex && hex[0] == '#') sscanf(hex + 1, "%2x%2x%2x", &r, &g, &b);
	cairo_set_source_rgba(cr, r / 255.0, g / 255.0, b / 255.0, a);
}

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
	double d = M_PI / 180.0;
	r = fmin(r, fmin(w, h) / 2.0);
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -90 * d, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, 90 * d);
	cairo_arc(cr, x + r, y + h - r, r, 90 * d, 180 * d);
	cairo_arc(cr, x + r, y + r, r, 180 * d, 270 * d);
	cairo_close_path(cr);
}

/* Cairo silently drops every later drawing call once it is handed a string
 * that is not valid UTF-8; window titles are outside our control. */
static void sanitize_utf8(char *s) {
	unsigned char *p = (unsigned char *)s;
	while (*p) {
		unsigned char c = *p;
		int need;
		if (c < 0x80) { p++; continue; }
		else if ((c & 0xE0) == 0xC0) need = 1;
		else if ((c & 0xF0) == 0xE0) need = 2;
		else if ((c & 0xF8) == 0xF0) need = 3;
		else { *p++ = '?'; continue; }
		int ok = 1;
		for (int i = 1; i <= need; i++)
			if ((p[i] & 0xC0) != 0x80) { ok = 0; break; }
		if (!ok) { *p++ = '?'; continue; }
		p += need + 1;
	}
}

static int run_cmd(const char *cmd, char *buf, size_t len) {
	buf[0] = 0;
	FILE *f = popen(cmd, "r");
	if (!f) return -1;
	size_t got = fread(buf, 1, len - 1, f);
	buf[got] = 0;
	pclose(f);
	return (int)got;
}

static void spawn(const char *cmd) {
	if (fork() == 0) {
		setsid();
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
}

static void load_actions(void) {
	const char *home = getenv("HOME");
	char path[512], line[512];
	if (!home) return;
	snprintf(path, sizeof path, "%s/.config/mango-bar/actions.conf", home);
	FILE *f = fopen(path, "r");
	if (!f) return;
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = 0;
		char *k = line, *v = eq + 1;
		while (*v == ' ') v++;
		if (!strcmp(k, "audio")) snprintf(cmd_audio, sizeof cmd_audio, "%s", v);
		else if (!strcmp(k, "network")) snprintf(cmd_network, sizeof cmd_network, "%s", v);
		else if (!strcmp(k, "bluetooth")) snprintf(cmd_bluetooth, sizeof cmd_bluetooth, "%s", v);
	}
	fclose(f);
}

/* --------------------------------------------------------------- data pull */

static void json_str(const char *p, const char *key, char *out, size_t outlen) {
	out[0] = 0;
	const char *k = strstr(p, key);
	if (!k) return;
	k += strlen(key);
	const char *end = strchr(k, '"');
	if (!end) return;
	size_t n = (size_t)(end - k);
	if (n >= outlen - 1) n = outlen - 1;
	memcpy(out, k, n);
	out[n] = 0;
	sanitize_utf8(out);
}

static void refresh_clients(void) {
	char buf[16384];
	if (run_cmd("/usr/local/bin/mango-dispatch get all-clients 2>/dev/null", buf, sizeof buf) <= 0)
		return;

	struct client got[MAX_PILLS];
	int n = 0;
	const char *p = buf;
	while (n < MAX_PILLS && (p = strstr(p, "{\"id\":")) != NULL) {
		struct client c;
		memset(&c, 0, sizeof c);
		c.id = atoi(p + 6);
		json_str(p, "\"title\":\"", c.title, sizeof c.title);
		json_str(p, "\"appid\":\"", c.appid, sizeof c.appid);
		c.focused = strstr(p, "\"is_focused\":true") != NULL;
		c.visible = strstr(p, "\"is_visible\":true") != NULL;
		if (c.visible) got[n++] = c;
		p += 6;
	}

	int changed = (n != nclients);
	for (int i = 0; !changed && i < n; i++)
		changed = got[i].id != clients[i].id || got[i].focused != clients[i].focused ||
			  strcmp(got[i].title, clients[i].title) != 0 ||
			  strcmp(got[i].appid, clients[i].appid) != 0;
	if (changed) {
		memcpy(clients, got, sizeof(struct client) * (size_t)n);
		nclients = n;
		dirty = 1;
	}
}

static void refresh_status(void) {
	char buf[256];
	int v = -1, mute = 0;
	if (run_cmd("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", buf, sizeof buf) > 0) {
		float f = 0;
		if (sscanf(buf, "Volume: %f", &f) == 1) v = (int)lroundf(f * 100.0f);
		mute = strstr(buf, "MUTED") != NULL;
	}
	if (v < 0) v = 0;

	struct stat st;
	int wifi = (stat("/sys/class/net/wlp1s0", &st) == 0);
	if (wifi && run_cmd("cat /sys/class/net/wlp1s0/operstate 2>/dev/null", buf, sizeof buf) > 0)
		wifi = strstr(buf, "up") != NULL;
	int bt = (stat("/sys/class/bluetooth/hci0", &st) == 0);

	if (v != vol_pct || mute != vol_mute || wifi != wifi_on || bt != bt_on) {
		vol_pct = v; vol_mute = mute; wifi_on = wifi; bt_on = bt;
		dirty = 1;
	}
}

static void refresh_clock(void) {
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	static const char *wd[] = {"周日","周一","周二","周三","周四","周五","周六"};
	char s[64];
	snprintf(s, sizeof s, "%s,%02d/%02d %02d:%02d:%02d", wd[tm.tm_wday],
		 tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
	if (strcmp(s, clock_str) != 0) {
		snprintf(clock_str, sizeof clock_str, "%s", s);
		dirty = 1;
	}
}

/* --------------------------------------------------------------- drawing */

static void text_at(cairo_t *cr, const char *txt, double size, const char *color,
		    double x, double y, int center) {
	cairo_select_font_face(cr, FONT_UI, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, size);
	cairo_text_extents_t te;
	cairo_text_extents(cr, txt, &te);
	set_hex(cr, color, 1);
	cairo_move_to(cr, center ? x - (te.width / 2 + te.x_bearing) : x, y);
	cairo_show_text(cr, txt);
}

static double text_width(cairo_t *cr, const char *txt, double size) {
	cairo_select_font_face(cr, FONT_UI, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, size);
	cairo_text_extents_t te;
	cairo_text_extents(cr, txt, &te);
	return te.width;
}

/* byte-wise truncation must not split a multi-byte character either */
static void ellipsize(cairo_t *cr, const char *in, double maxw, double size, char *out, size_t outlen) {
	snprintf(out, outlen, "%s", in);
	if (text_width(cr, out, size) <= maxw) return;
	size_t n = strlen(out);
	while (n > 1) {
		out[--n] = 0;
		sanitize_utf8(out);
		char tmp[320];
		snprintf(tmp, sizeof tmp, "%s…", out);
		if (text_width(cr, tmp, size) <= maxw) { snprintf(out, outlen, "%s", tmp); return; }
	}
}

static void draw_speaker(cairo_t *cr, double cx, double cy, int on, int mute) {
	set_hex(cr, on ? C_ICON : C_ICON_OFF, 1);
	cairo_set_line_width(cr, 1.5);
	cairo_move_to(cr, cx - 6, cy - 2.5);
	cairo_line_to(cr, cx - 3, cy - 2.5);
	cairo_line_to(cr, cx + 0.5, cy - 6);
	cairo_line_to(cr, cx + 0.5, cy + 6);
	cairo_line_to(cr, cx - 3, cy + 2.5);
	cairo_line_to(cr, cx - 6, cy + 2.5);
	cairo_close_path(cr);
	cairo_fill(cr);
	if (mute) {
		cairo_move_to(cr, cx + 2.5, cy - 4);
		cairo_line_to(cr, cx + 8.5, cy + 4);
		cairo_move_to(cr, cx + 8.5, cy - 4);
		cairo_line_to(cr, cx + 2.5, cy + 4);
		cairo_stroke(cr);
	} else {
		cairo_arc(cr, cx + 1, cy, 4.2, -0.9, 0.9);
		cairo_stroke(cr);
		if (vol_pct > 45) {
			cairo_arc(cr, cx + 1, cy, 7.6, -0.8, 0.8);
			cairo_stroke(cr);
		}
	}
}

static void draw_wifi(cairo_t *cr, double cx, double cy, int on) {
	set_hex(cr, on ? C_ICON : C_ICON_OFF, 1);
	cairo_set_line_width(cr, 1.5);
	if (on) {
		cairo_arc(cr, cx, cy + 5, 7.4, -2.35, -0.79);
		cairo_stroke(cr);
		cairo_arc(cr, cx, cy + 5, 4.2, -2.35, -0.79);
		cairo_stroke(cr);
	}
	cairo_arc(cr, cx, cy + 4.4, 1.3, 0, 2 * M_PI);
	cairo_fill(cr);
}

static void draw_bt(cairo_t *cr, double cx, double cy, int on) {
	set_hex(cr, on ? C_ICON : C_ICON_OFF, 1);
	cairo_set_line_width(cr, 1.5);
	cairo_move_to(cr, cx - 3.6, cy - 3);
	cairo_line_to(cr, cx + 3.6, cy + 3.6);
	cairo_line_to(cr, cx, cy + 6.4);
	cairo_line_to(cr, cx, cy - 6.4);
	cairo_line_to(cr, cx + 3.6, cy - 3.6);
	cairo_line_to(cr, cx - 3.6, cy + 3);
	cairo_stroke(cr);
}

static void draw_power(cairo_t *cr, double cx, double cy) {
	set_hex(cr, C_POWER_FG, 1);
	cairo_set_line_width(cr, 1.8);
	cairo_arc(cr, cx, cy + 1, 6, -M_PI / 2 + 0.65, 3 * M_PI / 2 - 0.65);
	cairo_stroke(cr);
	cairo_move_to(cr, cx, cy - 7.5);
	cairo_line_to(cr, cx, cy - 1);
	cairo_stroke(cr);
}

static void draw_pills(cairo_t *cr, double W, double cy) {
	n_pill_boxes = 0;
	double x = EDGE + 6;
	for (int i = 0; i < nclients && i < MAX_PILLS; i++) {
		char label[320], shown[320];
		snprintf(label, sizeof label, "%s · %s",
			 clients[i].appid[0] ? clients[i].appid : "window", clients[i].title);
		sanitize_utf8(label);
		ellipsize(cr, label, PILL_MAX_W - 2 * PILL_PAD_X, 14.5, shown, sizeof shown);
		sanitize_utf8(shown);
		double tw = text_width(cr, shown, 14.5);
		double w = tw + 2 * PILL_PAD_X;
		if (w > PILL_MAX_W) w = PILL_MAX_W;
		if (x + w > W - (TRAY_W + POWER_D + 40)) break;

		pill_box[n_pill_boxes].x = x;
		pill_box[n_pill_boxes].y = cy - PILL_H / 2.0;
		pill_box[n_pill_boxes].w = w;
		pill_box[n_pill_boxes].h = PILL_H;
		n_pill_boxes++;

		set_hex(cr, clients[i].focused ? C_PILL_ACT : C_PILL_IDLE, 1);
		rounded_rect(cr, x, cy - PILL_H / 2.0, w, PILL_H, PILL_R);
		cairo_fill_preserve(cr);
		if (clients[i].focused) {
			set_hex(cr, C_PILL_ACT_BD, 1);
			cairo_set_line_width(cr, 1);
			cairo_stroke(cr);
		} else {
			cairo_new_path(cr);
		}
		text_at(cr, shown, 14.5, clients[i].focused ? C_PILL_FG_ACT : C_PILL_FG_IDLE,
			x + PILL_PAD_X, cy + 5, 0);
		x += w + PILL_GAP;
	}
}

static void draw(unsigned char *fb) {
	cairo_surface_t *cs = cairo_image_surface_create_for_data(
		fb, CAIRO_FORMAT_ARGB32, buf_w, buf_h, buf_w * 4);
	cairo_t *cr = cairo_create(cs);
	cairo_scale(cr, scale, scale);

	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	double W = bar_w;
	double y0 = EDGE, h = BAR_H_INNER;
	double cy = y0 + h / 2.0;

	set_hex(cr, C_BAR_BG, 1);
	rounded_rect(cr, 0.5, y0 + 0.5, W - 1, h - 1, RADIUS);
	cairo_fill_preserve(cr);
	set_hex(cr, C_BAR_BD, 1);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);

	draw_pills(cr, W, cy);

	/* a client title can still put cairo into an error state; heal it so the
	 * clock / tray / power button always get drawn */
	if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
		cairo_destroy(cr);
		cr = cairo_create(cs);
		cairo_scale(cr, scale, scale);
	}

	text_at(cr, clock_str, 15, C_CLOCK, W / 2.0, cy + 5.5, 1);

	/* power button */
	double px = W - EDGE - 6 - POWER_D;
	power_box.x = px; power_box.y = cy - POWER_D / 2.0;
	power_box.w = POWER_D; power_box.h = POWER_D;
	set_hex(cr, power_armed ? C_POWER_FG : C_POWER_BG, 1);
	rounded_rect(cr, power_box.x, power_box.y, POWER_D, POWER_D, POWER_D / 2);
	cairo_fill_preserve(cr);
	set_hex(cr, power_armed ? C_POWER_FG : C_POWER_BD, 1);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	if (power_armed) {
		cairo_set_source_rgba(cr, 1, 1, 1, 1);
		cairo_set_line_width(cr, 1.8);
		cairo_arc(cr, px + POWER_D / 2, cy + 1, 6, -M_PI / 2 + 0.65, 3 * M_PI / 2 - 0.65);
		cairo_stroke(cr);
		cairo_move_to(cr, px + POWER_D / 2, cy - 7.5);
		cairo_line_to(cr, px + POWER_D / 2, cy - 1);
		cairo_stroke(cr);
	} else {
		draw_power(cr, px + POWER_D / 2, cy);
	}

	/* tray pill with three separate targets */
	double tx = px - 8 - TRAY_W;
	tray_box.x = tx; tray_box.y = cy - TRAY_H / 2.0;
	tray_box.w = TRAY_W; tray_box.h = TRAY_H;
	set_hex(cr, C_TRAY_BG, 1);
	rounded_rect(cr, tray_box.x, tray_box.y, TRAY_W, TRAY_H, TRAY_H / 2);
	cairo_fill_preserve(cr);
	set_hex(cr, C_TRAY_BD, 1);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);

	double iw = TRAY_W / 3.0;
	for (int i = 0; i < 3; i++) {
		tray_icon[i].x = tx + i * iw;
		tray_icon[i].y = tray_box.y;
		tray_icon[i].w = iw;
		tray_icon[i].h = TRAY_H;
	}
	draw_speaker(cr, tray_icon[0].x + iw / 2, cy, 1, vol_mute);
	draw_wifi(cr, tray_icon[1].x + iw / 2, cy, wifi_on);
	draw_bt(cr, tray_icon[2].x + iw / 2, cy, bt_on);

	{ const char *dump = getenv("MANGO_BAR_DUMP");
	  if (dump) cairo_surface_write_to_png(cs, dump); }

	cairo_destroy(cr);
	cairo_surface_destroy(cs);
	dirty = 0;
}

/* ------------------------------------------------------------------ shm */

#define NBUF 2
struct shmbuf { struct wl_buffer *wl; unsigned char *px; int w, h, busy; };
static struct shmbuf bufs[NBUF];
static int cur_buf;

static void shmbuf_release(void *data, struct wl_buffer *wl) {
	struct shmbuf *b = data;
	wl_buffer_destroy(wl);
	b->wl = NULL;
	if (b->px) { munmap(b->px, (size_t)b->w * b->h * 4); b->px = NULL; }
	b->busy = 0;
}
static const struct wl_buffer_listener buffer_listener = { shmbuf_release };

static int shmbuf_prepare(struct shmbuf *b, int w, int h) {
	if (b->wl && b->w == w && b->h == h) return 1;
	if (b->busy) return 0;
	int stride = w * 4, size = stride * h;
	int fd = memfd_create("mango-bar", MFD_CLOEXEC);
	if (fd < 0) return 0;
	if (ftruncate(fd, (off_t)size) < 0) { close(fd); return 0; }
	b->px = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (b->px == MAP_FAILED) { b->px = NULL; close(fd); return 0; }
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	b->wl = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	b->w = w; b->h = h;
	wl_buffer_add_listener(b->wl, &buffer_listener, b);
	return 1;
}

static void repaint(void) {
	for (int tries = 0; tries < NBUF; tries++) {
		struct shmbuf *b = &bufs[cur_buf];
		cur_buf = (cur_buf + 1) % NBUF;
		if (b->busy) continue;
		if (!shmbuf_prepare(b, buf_w, buf_h)) continue;
		draw(b->px);
		wl_surface_attach(surface, b->wl, 0, 0);
		wl_surface_set_buffer_scale(surface, scale);
		wl_surface_damage_buffer(surface, 0, 0, buf_w, buf_h);
		wl_surface_commit(surface);
		b->busy = 1;
		return;
	}
}

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *ls,
			    uint32_t serial, uint32_t w, uint32_t h) {
	(void)data; (void)h;
	if (w > 0) bar_w = (int)w;
	buf_w = bar_w * scale;
	buf_h = BAR_H * scale;
	zwlr_layer_surface_v1_ack_configure(ls, serial);
	dirty = 1;
	repaint();
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *ls) {
	(void)data; (void)ls;
	exit(0);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

/* ----------------------------------------------------------------- input */

static int inside(const struct box *b, double x, double y) {
	return x >= b->x && x <= b->x + b->w && y >= b->y && y <= b->y + b->h;
}

static void pointer_enter(void *d, struct wl_pointer *p, uint32_t serial,
			  struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
	(void)d; (void)p; (void)serial; (void)s;
	pointer_x = wl_fixed_to_int(x);
	pointer_y = wl_fixed_to_int(y);
}

static void pointer_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) {
	(void)d; (void)p; (void)serial; (void)s;
	pointer_x = pointer_y = -1;
}

static void pointer_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
	(void)d; (void)p; (void)t;
	pointer_x = wl_fixed_to_int(x);
	pointer_y = wl_fixed_to_int(y);
}

static void pointer_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t,
			   uint32_t button, uint32_t state) {
	(void)d; (void)p; (void)t; (void)button; (void)serial;
	if (state != WL_POINTER_BUTTON_STATE_PRESSED) return;

	double x = pointer_x, y = pointer_y;

	for (int i = 0; i < n_pill_boxes; i++) {
		if (inside(&pill_box[i], x, y)) {
			char cmd[256];
			snprintf(cmd, sizeof cmd,
				 "/usr/local/bin/mango-dispatch dispatch focusid client,%d >/dev/null 2>&1",
				 clients[i].id);
			spawn(cmd);
			return;
		}
	}

	if (inside(&power_box, x, y)) {
		time_t now = time(NULL);
		if (power_armed && now - power_armed_at <= 3) {
			power_armed = 0;
			dirty = 1;
			repaint();
			spawn("systemctl poweroff");
			return;
		}
		power_armed = 1;
		power_armed_at = now;
		dirty = 1;
		repaint();
		return;
	}

	for (int i = 0; i < 3; i++) {
		if (inside(&tray_icon[i], x, y)) {
			spawn(i == 0 ? cmd_audio : i == 1 ? cmd_network : cmd_bluetooth);
			return;
		}
	}
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
};

static void seat_capabilities(void *d, struct wl_seat *s, uint32_t caps) {
	(void)d;
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	}
}

static const struct wl_seat_listener seat_listener = { .capabilities = seat_capabilities };

static void output_geometry(void *d, struct wl_output *o, int32_t x, int32_t y,
			    int32_t pw, int32_t ph, int32_t sub, const char *make,
			    const char *model, int32_t transform) {
	(void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sub;
	(void)make; (void)model; (void)transform;
}
static void output_mode(void *d, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t refresh) {
	(void)d; (void)o; (void)flags; (void)refresh; (void)w; (void)h;
}
static void output_done(void *d, struct wl_output *o) { (void)d; (void)o; }
static void output_scale(void *d, struct wl_output *o, int32_t s) {
	(void)d; (void)o;
	scale = s > 0 ? s : 1;
}
static const struct wl_output_listener output_listener = {
	.geometry = output_geometry, .mode = output_mode,
	.done = output_done, .scale = output_scale,
};

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
			    const char *iface, uint32_t version) {
	(void)data; (void)version;
	if (!strcmp(iface, wl_compositor_interface.name))
		compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
	else if (!strcmp(iface, wl_shm_interface.name))
		shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, wl_seat_interface.name))
		seat = wl_registry_bind(reg, name, &wl_seat_interface, 7);
	else if (!strcmp(iface, wl_output_interface.name))
		output = wl_registry_bind(reg, name, &wl_output_interface, 2);
	else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name))
		layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
}
static void registry_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener registry_listener = {
	.global = registry_global, .global_remove = registry_remove,
};

/* ------------------------------------------------------------------ main */

static volatile sig_atomic_t running = 1;
static void on_term(int sig) { (void)sig; running = 0; }

int main(void) {
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	signal(SIGCHLD, SIG_IGN);

	load_actions();

	display = wl_display_connect(NULL);
	if (!display) { fprintf(stderr, "mango-bar: cannot connect to the compositor\n"); return 1; }

	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &registry_listener, NULL);
	wl_display_roundtrip(display);
	wl_display_roundtrip(display);

	if (!compositor || !shm || !layer_shell) {
		fprintf(stderr, "mango-bar: missing compositor/shm/layer-shell\n");
		return 1;
	}
	if (output) wl_output_add_listener(output, &output_listener, NULL);
	if (seat) wl_seat_add_listener(seat, &seat_listener, NULL);
	wl_display_roundtrip(display);

	surface = wl_compositor_create_surface(compositor);
	layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		layer_shell, surface, output, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "mango-bar");
	zwlr_layer_surface_v1_set_anchor(layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, BAR_H);
	zwlr_layer_surface_v1_set_size(layer_surface, 0, BAR_H);
	zwlr_layer_surface_v1_add_listener(layer_surface, &layer_listener, NULL);
	wl_surface_commit(surface);
	wl_display_roundtrip(display);

	refresh_clients();
	refresh_status();
	refresh_clock();

	int wfd = wl_display_get_fd(display);
	time_t last_clients = 0, last_status = 0;

	while (running) {
		wl_display_dispatch_pending(display);
		wl_display_flush(display);

		struct pollfd pfd = { .fd = wfd, .events = POLLIN };
		if (poll(&pfd, 1, 250) > 0 && (pfd.revents & POLLIN)) {
			if (wl_display_dispatch(display) < 0) break;
		}

		time_t now = time(NULL);
		refresh_clock();
		if (now != last_clients) { refresh_clients(); last_clients = now; }
		if (now - last_status >= 2) { refresh_status(); last_status = now; }
		if (power_armed && now - power_armed_at > 3) { power_armed = 0; dirty = 1; }
		if (dirty) repaint();
	}

	return 0;
}
