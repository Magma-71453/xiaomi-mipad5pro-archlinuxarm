/*
 * mango-bar — lightweight top status bar for MangoWM (elish).
 *
 * Layout follows docs/bar-design.svg:
 *   [ window pills ........................ 周一,10/25 12:34:56  (vol wifi bt) (power) ]
 *
 * Wayland layer-shell + cairo on an shm buffer; no GTK/Qt, no polling thread.
 * Data sources: "mango-dispatch get all-clients" (JSON), /sys for wifi+bt,
 * "wpctl get-volume" for the sink level.
 */
#define _GNU_SOURCE
#include <cairo/cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define BAR_H        52
#define PAD          10
#define PILL_H       34
#define PILL_R       9
#define PILL_PAD_X   12
#define PILL_MAX_W   210
#define PILL_GAP     8
#define MAX_PILLS    4
#define RADIUS       14

#define C_BAR_BG     "#171A21"
#define C_BAR_BORDER "#323844"
#define C_PILL_BG    "#222730"
#define C_PILL_BD    "#353C49"
#define C_PILL_FG    "#CAD0DB"
#define C_ACT_BG     "#30394B"
#define C_ACT_BD     "#637CA8"
#define C_ACT_FG     "#F5F7FC"
#define C_TEXT       "#E6E9F0"
#define C_POWER_BG   "#33252B"
#define C_POWER_BD   "#62404A"
#define C_POWER_FG   "#E0717E"
#define C_ICON       "#C8CEDA"

#define FONT_UI  "Noto Sans CJK SC"
#define FONT_ICON "Noto Sans Symbols 2"

struct client {
	int id;
	char appid[64];
	char title[192];
	int focused;
};

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct wl_output *output;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zwlr_layer_surface_v1 *layer_surface;
static struct wl_surface *surface;
static int buf_w, buf_h;
static int scale = 2;
static int out_w = 1280, out_h = 800;
static int bar_w = 1280;

static struct client clients[MAX_PILLS];
static int nclients;
static char clock_str[64];
static int vol_pct = 0, vol_mute = 0, wifi_on = 0, bt_on = 0;
static int power_armed = 0;
static time_t power_armed_at = 0;
static int dirty = 1;
static int pointer_x = -1, pointer_y = -1;

/* ------------------------------------------------------------------ util */

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

static void set_hex(cairo_t *cr, const char *hex, double a) {
	unsigned r = 0, g = 0, b = 0;
	if (hex && hex[0] == '#') sscanf(hex + 1, "%2x%2x%2x", &r, &g, &b);
	cairo_set_source_rgba(cr, r / 255.0, g / 255.0, b / 255.0, a);
}

/* run a command, capture stdout into buf (NUL terminated, newlines kept) */
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

/* --------------------------------------------------------------- data pull */

static void json_str(const char *p, const char *key, char *out, size_t outlen) {
	out[0] = 0;
	const char *k = strstr(p, key);
	if (!k) return;
	k += strlen(key);
	const char *end = strchr(k, '"');
	if (!end) return;
	size_t n = (size_t)(end - k);
	if (n >= outlen) n = outlen - 1;
	memcpy(out, k, n);
	out[n] = 0;
}

static void refresh_clients(void) {
	char buf[16384];
	if (run_cmd("/usr/local/bin/mango-dispatch get all-clients 2>/dev/null", buf, sizeof buf) <= 0)
		return;

	struct client got[MAX_PILLS];
	int n = 0;
	const char *p = buf;
	while (n < MAX_PILLS && (p = strstr(p, "{\"id\":")) != NULL) {
		struct client *c = &got[n];
		memset(c, 0, sizeof *c);
		c->id = atoi(p + 6);
		json_str(p, "\"title\":\"", c->title, sizeof c->title);
		json_str(p, "\"appid\":\"", c->appid, sizeof c->appid);
		c->focused = strstr(p, "\"is_focused\":true") != NULL;
		n++;
		p += 6;
	}

	int changed = (n != nclients);
	for (int i = 0; !changed && i < n; i++)
		changed = got[i].id != clients[i].id || got[i].focused != clients[i].focused ||
			  strcmp(got[i].title, clients[i].title) != 0 ||
			  strcmp(got[i].appid, clients[i].appid) != 0;
	if (changed) {
		memcpy(clients, got, sizeof(struct client) * n);
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
	if (v < 0) {
		if (run_cmd("pactl get-sink-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", buf, sizeof buf) > 0) {
			char *pc = strchr(buf, '/');
			if (pc) v = atoi(pc + 1);
		}
	}
	if (v < 0) v = 0;

	struct stat st;
	int wifi = (stat("/sys/class/net/wlp1s0", &st) == 0);
	if (wifi) {
		if (run_cmd("cat /sys/class/net/wlp1s0/operstate 2>/dev/null", buf, sizeof buf) > 0)
			wifi = strstr(buf, "up") != NULL;
	}
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

/* --------------------------------------------------------------- geometry */

struct geom { double x, y, w, h; };


static void repaint(void);

static struct geom pill_box[MAX_PILLS];
static struct geom power_box, status_box;
static int n_pill_boxes;

static int inside(struct geom *g, double x, double y) {
	return x >= g->x && x <= g->x + g->w && y >= g->y && y <= g->y + g->h;
}

/* --------------------------------------------------------------- drawing */

static void draw_icon_speaker(cairo_t *cr, double cx, double cy) {
	set_hex(cr, C_ICON, 1);
	cairo_set_line_width(cr, 1.6);
	cairo_move_to(cr, cx - 6, cy - 2.5);
	cairo_line_to(cr, cx - 3, cy - 2.5);
	cairo_line_to(cr, cx + 0.5, cy - 6);
	cairo_line_to(cr, cx + 0.5, cy + 6);
	cairo_line_to(cr, cx - 3, cy + 2.5);
	cairo_line_to(cr, cx - 6, cy + 2.5);
	cairo_close_path(cr);
	cairo_fill(cr);
	if (!vol_mute) {
		cairo_arc(cr, cx + 1, cy, 4.5, -0.9, 0.9);
		cairo_stroke(cr);
		if (vol_pct > 45) {
			cairo_arc(cr, cx + 1, cy, 8, -0.8, 0.8);
			cairo_stroke(cr);
		}
	} else {
		cairo_move_to(cr, cx + 2, cy - 4);
		cairo_line_to(cr, cx + 9, cy + 4);
		cairo_move_to(cr, cx + 9, cy - 4);
		cairo_line_to(cr, cx + 2, cy + 4);
		cairo_stroke(cr);
	}
}

static void draw_icon_wifi(cairo_t *cr, double cx, double cy) {
	set_hex(cr, wifi_on ? C_ICON : "#5A6270", 1);
	cairo_set_line_width(cr, 1.6);
	if (wifi_on) {
		cairo_arc(cr, cx, cy + 5, 8, -2.35, -0.79);
		cairo_stroke(cr);
		cairo_arc(cr, cx, cy + 5, 4.5, -2.35, -0.79);
		cairo_stroke(cr);
	}
	cairo_arc(cr, cx, cy + 4.5, 1.4, 0, 2 * M_PI);
	cairo_fill(cr);
}

static void draw_icon_bt(cairo_t *cr, double cx, double cy) {
	set_hex(cr, bt_on ? C_ICON : "#5A6270", 1);
	cairo_set_line_width(cr, 1.6);
	cairo_move_to(cr, cx - 4, cy - 3);
	cairo_line_to(cr, cx + 4, cy + 4);
	cairo_line_to(cr, cx, cy + 7);
	cairo_line_to(cr, cx, cy - 7);
	cairo_line_to(cr, cx + 4, cy - 4);
	cairo_line_to(cr, cx - 4, cy + 3);
	cairo_stroke(cr);
}

static void draw_icon_power(cairo_t *cr, double cx, double cy) {
	set_hex(cr, power_armed ? "#FFFFFF" : C_POWER_FG, 1);
	cairo_set_line_width(cr, 1.8);
	cairo_arc(cr, cx, cy + 1, 6.5, -M_PI / 2 + 0.6, 3 * M_PI / 2 - 0.6);
	cairo_stroke(cr);
	cairo_move_to(cr, cx, cy - 8);
	cairo_line_to(cr, cx, cy - 1);
	cairo_stroke(cr);
}

static void draw_text(cairo_t *cr, const char *txt, const char *font, double size,
		      const char *color, double x, double y, int center) {
	cairo_select_font_face(cr, font, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, size);
	cairo_text_extents_t te;
	cairo_text_extents(cr, txt, &te);
	set_hex(cr, color, 1);
	cairo_move_to(cr, center ? x - (te.width / 2 + te.x_bearing) : x, y);
	cairo_show_text(cr, txt);
}

static void ellipsize(cairo_t *cr, const char *in, double maxw, double size, char *out, size_t outlen) {
	cairo_select_font_face(cr, FONT_UI, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, size);
	snprintf(out, outlen, "%s", in);
	cairo_text_extents_t te;
	cairo_text_extents(cr, out, &te);
	if (te.width <= maxw) return;
	for (size_t n = strlen(out); n > 1; n--) {
		out[n] = 0;
		char tmp[256];
		snprintf(tmp, sizeof tmp, "%s…", out);
		cairo_text_extents(cr, tmp, &te);
		if (te.width <= maxw) { snprintf(out, outlen, "%s", tmp); return; }
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

	/* bar background */
	set_hex(cr, C_BAR_BG, 0.94);
	rounded_rect(cr, 0.5, 0.5, W - 1, BAR_H - 1, RADIUS);
	cairo_fill_preserve(cr);
	set_hex(cr, C_BAR_BORDER, 1);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);

	/* ---- left: window pills ---- */
	n_pill_boxes = 0;
	double x = PAD;
	for (int i = 0; i < nclients && i < MAX_PILLS; i++) {
		char label[256];
		snprintf(label, sizeof label, "%s · %s",
			 clients[i].appid[0] ? clients[i].appid : "window", clients[i].title);
		char shown[256];
		ellipsize(cr, label, PILL_MAX_W - 2 * PILL_PAD_X, 14, shown, sizeof shown);
		cairo_select_font_face(cr, FONT_UI, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
		cairo_set_font_size(cr, 14);
		cairo_text_extents_t te;
		cairo_text_extents(cr, shown, &te);
		double w = te.width + 2 * PILL_PAD_X;
		if (w > PILL_MAX_W) w = PILL_MAX_W;
		if (x + w > W - 320) break;

		pill_box[n_pill_boxes].x = x;
		pill_box[n_pill_boxes].y = (BAR_H - PILL_H) / 2.0;
		pill_box[n_pill_boxes].w = w;
		pill_box[n_pill_boxes].h = PILL_H;
		n_pill_boxes++;

		set_hex(cr, clients[i].focused ? C_ACT_BG : C_PILL_BG, 1);
		rounded_rect(cr, x, (BAR_H - PILL_H) / 2.0, w, PILL_H, PILL_R);
		cairo_fill_preserve(cr);
		set_hex(cr, clients[i].focused ? C_ACT_BD : C_PILL_BD, 1);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);

		draw_text(cr, shown, FONT_UI, 14, clients[i].focused ? C_ACT_FG : C_PILL_FG,
			  x + PILL_PAD_X, BAR_H / 2.0 + 5, 0);
		x += w + PILL_GAP;
	}

	/* ---- centre: clock ---- */
	draw_text(cr, clock_str, FONT_UI, 15, C_TEXT, W / 2.0, BAR_H / 2.0 + 5.5, 1);

	/* ---- right: status pill + power ---- */
	double pw = 38;
	double px = W - PAD - pw;
	power_box.x = px; power_box.y = (BAR_H - pw) / 2.0; power_box.w = pw; power_box.h = pw;

	if (power_armed) {
		set_hex(cr, "#8E3B48", 1);
		rounded_rect(cr, px, (BAR_H - pw) / 2.0, pw, pw, pw / 2);
		cairo_fill(cr);
	} else {
		set_hex(cr, C_POWER_BG, 1);
		rounded_rect(cr, px, (BAR_H - pw) / 2.0, pw, pw, pw / 2);
		cairo_fill_preserve(cr);
		set_hex(cr, C_POWER_BD, 1);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	}
	draw_icon_power(cr, px + pw / 2, BAR_H / 2.0);

	double sw = 132, sh = 38;
	double sx = px - 8 - sw;
	status_box.x = sx; status_box.y = (BAR_H - sh) / 2.0; status_box.w = sw; status_box.h = sh;
	set_hex(cr, "#272D37", 1);
	rounded_rect(cr, sx, (BAR_H - sh) / 2.0, sw, sh, sh / 2);
	cairo_fill_preserve(cr);
	set_hex(cr, "#434B59", 1);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	draw_icon_speaker(cr, sx + 28, BAR_H / 2.0);
	draw_icon_wifi(cr, sx + 66, BAR_H / 2.0);
	draw_icon_bt(cr, sx + 104, BAR_H / 2.0);

	cairo_destroy(cr);
	cairo_surface_destroy(cs);
	dirty = 0;
}

/* ------------------------------------------------------------------ shm */

/* Two rotating shm buffers: a compositor only releases a buffer once a newer
 * one replaces it, so a single buffer would deadlock after the first frame. */
#define NBUF 2

struct shmbuf {
	struct wl_buffer *wl;
	unsigned char *px;
	int w, h;
	int busy;
};

static struct shmbuf bufs[NBUF];
static int cur_buf;

static void shmbuf_release(void *data, struct wl_buffer *wl) {
	struct shmbuf *b = data;
	wl_buffer_destroy(wl);
	b->wl = NULL;
	if (b->px) {
		munmap(b->px, (size_t)b->w * b->h * 4);
		b->px = NULL;
	}
	b->busy = 0;
}
static const struct wl_buffer_listener buffer_listener = { shmbuf_release };

static int shmbuf_prepare(struct shmbuf *b, int w, int h) {
	if (b->wl && b->w == w && b->h == h) return 1;
	if (b->busy) return 0;
	int stride = w * 4;
	int size = stride * h;
	int fd = memfd_create("mango-bar", MFD_CLOEXEC);
	if (fd < 0) return 0;
	if (ftruncate(fd, (off_t)size) < 0) { close(fd); return 0; }
	b->px = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (b->px == MAP_FAILED) { b->px = NULL; close(fd); return 0; }
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	b->wl = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	b->w = w;
	b->h = h;
	wl_buffer_add_listener(b->wl, &buffer_listener, b);
	return 1;
}

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *ls,
			    uint32_t serial, uint32_t w, uint32_t h) {
	(void)data; (void)h;
	bar_w = (int)w;
	buf_w = (int)w * scale;
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

static void repaint(void) {
	for (int tries = 0; tries < NBUF; tries++) {
		struct shmbuf *b = &bufs[cur_buf];
		int idx = cur_buf;
		cur_buf = (cur_buf + 1) % NBUF;
		if (b->busy) continue;
		if (!shmbuf_prepare(b, buf_w, buf_h)) continue;
		draw(b->px);
		wl_surface_attach(surface, b->wl, 0, 0);
		wl_surface_set_buffer_scale(surface, scale);
		wl_surface_damage_buffer(surface, 0, 0, buf_w, buf_h);
		wl_surface_commit(surface);
		b->busy = 1;
		(void)idx;
		return;
	}
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
	(void)d; (void)p; (void)t; (void)button;
	if (state != WL_POINTER_BUTTON_STATE_PRESSED) return;

	double x = pointer_x, y = pointer_y;

	for (int i = 0; i < n_pill_boxes; i++) {
		if (inside(&pill_box[i], x, y)) {
			char cmd[256];
			snprintf(cmd, sizeof cmd,
				 "/usr/local/bin/mango-dispatch dispatch focusid client,%d >/dev/null 2>&1", clients[i].id);
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

	if (inside(&status_box, x, y)) {
		spawn("/usr/local/bin/mango-volume mute-toggle >/dev/null 2>&1 || wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle");
		return;
	}
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = NULL,
	.frame = NULL,
	.axis_source = NULL,
	.axis_stop = NULL,
	.axis_discrete = NULL,
};

static void seat_capabilities(void *d, struct wl_seat *s, uint32_t caps) {
	(void)d;
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	}
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = NULL,
};

static void output_geometry(void *d, struct wl_output *o, int32_t x, int32_t y,
			    int32_t pw, int32_t ph, int32_t sub, const char *make,
			    const char *model, int32_t transform) {
	(void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sub;
	(void)make; (void)model; (void)transform;
}
static void output_mode(void *d, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t refresh) {
	(void)d; (void)o; (void)flags; (void)refresh;
	if (w > 0 && h > 0) { out_w = w; out_h = h; }
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

static void registry_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d; (void)r; (void)name; }
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
		int pr = poll(&pfd, 1, 250);
		if (pr > 0 && (pfd.revents & POLLIN)) {
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
