/*
 * mango-dock — floating, content-sized app dock for MangoWM (wlr-layer-shell).
 *
 * Look: light frosted pill, rounded corners, soft shadow, icons grouped with a
 * separator (HyperOS-ish). Width follows the icon list, so adding/removing an
 * entry resizes the dock ("dynamic length").
 *
 * Signals: SIGUSR1 = hide, SIGUSR2 = show (used by mango-shell-watch to get out
 * of the way when a window covers the dock).
 *
 * Geometry is written to $XDG_RUNTIME_DIR/mango-dock.geom ("x y w h") for the
 * watcher.
 */
#define _GNU_SOURCE
#include <cairo/cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/* ---- geometry (logical px) ---- */
#define DOCK_H     68
#define ICON       52
#define GAP        14
#define PAD        14
#define SEP_W       9      /* width reserved for the separator (line + margins) */
#define RADIUS     20
#define MARGIN_BOT 14
#define MARGIN_LEFT 14
#define SHADOW      6

struct app {
	const char *icon;
	const char *cmd;
	int sep_before;
	cairo_surface_t *surf;
};

static struct app *apps;
static int napps;

/* Defaults, used when ~/.config/mango-dock/apps.conf is missing. */
static const struct app default_apps[] = {
	{ "/usr/share/mango-dock/icons/kitty.png",   "kitty",           0 },
	{ "/usr/share/mango-dock/icons/firefox.png", "firefox",         0 },
	{ "/usr/share/mango-dock/icons/rofi.png",    "rofi -show drun", 1 },
	{ "/usr/share/mango-dock/icons/kbd.png",     "/usr/local/bin/mango-osk-toggle", 0 },
	{ "/usr/share/mango-dock/icons/actions.png", "/usr/local/bin/mango-actions-toggle", 1 },
};

/*
 * apps.conf, one app per line:
 *
 *   <icon.png> | <command> [| sep]
 *
 * The icon may be a bare file name (looked up in /usr/share/mango-dock/icons)
 * or an absolute path. Lines starting with '#' and blank lines are ignored.
 */
static void load_apps(void) {
	const char *home = getenv("HOME");
	char path[512];
	if (home)
		snprintf(path, sizeof path, "%s/.config/mango-dock/apps.conf", home);
	else
		path[0] = 0;

	FILE *f = path[0] ? fopen(path, "r") : NULL;
	if (!f) {
		napps = (int)(sizeof default_apps / sizeof default_apps[0]);
		apps = malloc(sizeof(struct app) * napps);
		memcpy(apps, default_apps, sizeof default_apps);
		return;
	}

	char line[512];
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		char *p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (!*p || *p == '#')
			continue;
		char *icon = p;
		char *bar1 = strchr(p, '|');
		if (!bar1)
			continue;
		*bar1 = 0;
		char *cmd = bar1 + 1;
		char *bar2 = strchr(cmd, '|');
		int sep = 0;
		if (bar2) {
			*bar2 = 0;
			sep = 1;
		}
		/* trim */
		while (*cmd == ' ' || *cmd == '\t') cmd++;
		char *end = cmd + strlen(cmd);
		while (end > cmd && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
		end = icon + strlen(icon);
		while (end > icon && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
		if (!*cmd || !*icon)
			continue;

		apps = realloc(apps, sizeof(struct app) * (napps + 1));
		struct app *a = &apps[napps];
		memset(a, 0, sizeof *a);
		if (icon[0] == '/')
			a->icon = strdup(icon);
		else {
			char full[512];
			snprintf(full, sizeof full, "/usr/share/mango-dock/icons/%s", icon);
			a->icon = strdup(full);
		}
		a->cmd = strdup(cmd);
		a->sep_before = sep;
		napps++;
	}
	fclose(f);
	if (napps == 0) {
		napps = (int)(sizeof default_apps / sizeof default_apps[0]);
		apps = malloc(sizeof(struct app) * napps);
		memcpy(apps, default_apps, sizeof default_apps);
	}
}

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct wl_touch *touch;
static struct wl_output *output;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zwlr_layer_surface_v1 *layer_surface;
static struct wl_surface *surface;
static struct wl_buffer *buffer;
static unsigned char *pixels;
static int buf_w, buf_h;

static int scale = 2;
static int out_w = 1280, out_h = 800;   /* logical output size */
static int win_w = 300, win_h = DOCK_H; /* surface size (logical) */
static int hidden = 0;
static int pointer_x = -1, pointer_y = -1;
static int hover = -1;

/* ------------------------------------------------------------------ util */

static void spawn(const char *cmd) {
	if (fork() == 0) {
		setsid();
		if (!getenv("DISPLAY")) setenv("DISPLAY", ":0", 1);
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
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

/* width of the pill for the current icon list */
static int content_width(void) {
	int w = PAD * 2;
	for (int i = 0; i < napps; i++) {
		if (i) w += GAP;
		if (apps[i].sep_before) w += SEP_W;
		w += ICON;
	}
	return w;
}

static void icon_rect(int i, double *x, double *y, double *w, double *h) {
	double cx = PAD;
	for (int k = 0; k < i; k++) {
		cx += ICON + GAP;
		if (apps[k + 1].sep_before) cx += SEP_W;
	}
	*x = cx;
	*y = (win_h - ICON) / 2.0;
	*w = ICON;
	*h = ICON;
}

static int hit_test(double x, double y) {
	for (int i = 0; i < napps; i++) {
		double ix, iy, iw, ih;
		icon_rect(i, &ix, &iy, &iw, &ih);
		if (x >= ix - GAP / 2.0 && x <= ix + iw + GAP / 2.0 &&
		    y >= iy - 8 && y <= iy + ih + 8)
			return i;
	}
	return -1;
}

/* --------------------------------------------------------------- drawing */

static void draw(void) {
	if (!buffer) return;
	cairo_surface_t *cs = cairo_image_surface_create_for_data(
		pixels, CAIRO_FORMAT_ARGB32, win_w * scale, win_h * scale, win_w * scale * 4);
	cairo_t *cr = cairo_create(cs);
	cairo_scale(cr, scale, scale);

	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	double top = SHADOW;
	double pill_h = win_h - SHADOW;

	/* soft shadow */
	for (int i = SHADOW; i >= 1; i--) {
		cairo_set_source_rgba(cr, 0, 0, 0, 0.045);
		rounded_rect(cr, i * 0.6, top + i * 0.5, win_w - i * 1.2, pill_h, RADIUS);
		cairo_fill(cr);
	}

	/* frosted light pill */
	rounded_rect(cr, 0.5, top + 0.5, win_w - 1, pill_h - 1, RADIUS);
	cairo_set_source_rgba(cr, 0.94, 0.94, 0.96, 0.80);
	cairo_fill_preserve(cr);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.55);
	cairo_set_line_width(cr, 1.0);
	cairo_stroke(cr);

	for (int i = 0; i < napps; i++) {
		double x, y, w, h;
		icon_rect(i, &x, &y, &w, &h);
		y += top;

		if (apps[i].sep_before) {
			double sx = x - SEP_W / 2.0 - GAP / 2.0 + 1.5;
			cairo_set_source_rgba(cr, 0, 0, 0, 0.13);
			cairo_set_line_width(cr, 1.0);
			cairo_move_to(cr, sx, top + 14);
			cairo_line_to(cr, sx, top + pill_h - 14);
			cairo_stroke(cr);
		}

		int active = (i == hover);
		double size = active ? ICON * 1.12 : ICON;
		double ix = x + (ICON - size) / 2.0;
		double iy = y + (ICON - size) / 2.0 - (active ? 3 : 0);

		if (active) {
			rounded_rect(cr, x - 7, y - 7, ICON + 14, ICON + 14, 15);
			cairo_set_source_rgba(cr, 0, 0, 0, 0.08);
			cairo_fill(cr);
		}
		if (apps[i].surf) {
			cairo_save(cr);
			rounded_rect(cr, ix, iy, size, size, size * 0.23);
			cairo_clip(cr);
			double s = size / cairo_image_surface_get_width(apps[i].surf);
			cairo_translate(cr, ix, iy);
			cairo_scale(cr, s, s);
			cairo_set_source_surface(cr, apps[i].surf, 0, 0);
			cairo_paint(cr);
			cairo_restore(cr);
		}
	}
	cairo_destroy(cr);
	cairo_surface_flush(cs);
	cairo_surface_destroy(cs);

	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, win_w * scale, win_h * scale);
	wl_surface_commit(surface);
}

/* ------------------------------------------------------------- geometry */

static void write_geom(void) {
	const char *rt = getenv("XDG_RUNTIME_DIR");
	char path[512];
	if (!rt) return;
	snprintf(path, sizeof path, "%s/mango-dock.geom", rt);
	FILE *f = fopen(path, "w");
	if (!f) return;
	int x = (out_w - win_w) / 2;
	int y = out_h - MARGIN_BOT - win_h;
	fprintf(f, "%d %d %d %d %d\n", x, y, win_w, win_h, hidden);
	fclose(f);
}

static void alloc_buffer(int w, int h) {
	int stride = w * scale * 4;
	int size = stride * h * scale;
	int fd = memfd_create("mango-dock", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0) { perror("mango-dock: shm"); exit(1); }
	if (pixels) munmap(pixels, (size_t)buf_w * scale * buf_h * scale * 4);
	pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) { perror("mango-dock: mmap"); exit(1); }
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	if (buffer) wl_buffer_destroy(buffer);
	buffer = wl_shm_pool_create_buffer(pool, 0, w * scale, h * scale, stride,
					   WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	buf_w = w;
	buf_h = h;
}

/* re-request size + horizontal centering (content length may change) */
static void relayout(void) {
	if (!layer_surface || !surface) return;
	win_w = content_width();
	zwlr_layer_surface_v1_set_size(layer_surface, win_w, win_h);
	zwlr_layer_surface_v1_set_margin(layer_surface, 0, 0, MARGIN_BOT, (out_w - win_w) / 2);
	wl_surface_commit(surface);
}

/* ------------------------------------------------------------- listeners */

static void ls_configure(void *data, struct zwlr_layer_surface_v1 *ls,
			 uint32_t serial, uint32_t w, uint32_t h) {
	(void)data;
	zwlr_layer_surface_v1_ack_configure(ls, serial);
	if (w > 0) win_w = (int)w;
	if (h > 0) win_h = (int)h;
	if (win_w != buf_w || win_h != buf_h) alloc_buffer(win_w, win_h);
	draw();
	write_geom();
}

static void ls_closed(void *data, struct zwlr_layer_surface_v1 *ls) { (void)data; (void)ls; exit(0); }
static const struct zwlr_layer_surface_v1_listener ls_listener = { .configure = ls_configure, .closed = ls_closed };

static void ptr_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y) {
	(void)d; (void)p; (void)s; (void)sf;
	pointer_x = wl_fixed_to_int(x); pointer_y = wl_fixed_to_int(y);
	int h = hit_test(pointer_x, pointer_y);
	if (h != hover) { hover = h; draw(); }
}
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf) {
	(void)d; (void)p; (void)s; (void)sf;
	pointer_x = pointer_y = -1;
	if (hover != -1) { hover = -1; draw(); }
}
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
	(void)d; (void)p; (void)t;
	pointer_x = wl_fixed_to_int(x); pointer_y = wl_fixed_to_int(y);
	int h = hit_test(pointer_x, pointer_y);
	if (h != hover) { hover = h; draw(); }
}
static void ptr_button(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, uint32_t b, uint32_t st) {
	(void)d; (void)p; (void)s; (void)t;
	if (b != BTN_LEFT || st != WL_POINTER_BUTTON_STATE_PRESSED) return;
	int i = hit_test(pointer_x, pointer_y);
	if (i >= 0) spawn(apps[i].cmd);
}
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) { (void)d;(void)p;(void)t;(void)a;(void)v; }
static void ptr_frame(void *d, struct wl_pointer *p) { (void)d;(void)p; }
static void ptr_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d;(void)p;(void)s; }
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d;(void)p;(void)t;(void)a; }
static void ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d;(void)p;(void)a;(void)v; }
static void ptr_axis_value120(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d;(void)p;(void)a;(void)v; }
static void ptr_axis_relative_direction(void *d, struct wl_pointer *p, uint32_t a, uint32_t dir) { (void)d;(void)p;(void)a;(void)dir; }
static const struct wl_pointer_listener pointer_listener = {
	.enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion, .button = ptr_button,
	.axis = ptr_axis, .frame = ptr_frame, .axis_source = ptr_axis_source,
	.axis_stop = ptr_axis_stop, .axis_discrete = ptr_axis_discrete,
	.axis_value120 = ptr_axis_value120, .axis_relative_direction = ptr_axis_relative_direction,
};

static void tch_down(void *d, struct wl_touch *t, uint32_t s, uint32_t tm, struct wl_surface *sf, int32_t id, wl_fixed_t x, wl_fixed_t y) {
	(void)d;(void)t;(void)s;(void)tm;(void)sf;(void)id;
	int i = hit_test(wl_fixed_to_int(x), wl_fixed_to_int(y));
	if (i >= 0) spawn(apps[i].cmd);
}
static void tch_up(void *d, struct wl_touch *t, uint32_t s, uint32_t tm, int32_t id) { (void)d;(void)t;(void)s;(void)tm;(void)id; }
static void tch_motion(void *d, struct wl_touch *t, uint32_t tm, int32_t id, wl_fixed_t x, wl_fixed_t y) { (void)d;(void)t;(void)tm;(void)id;(void)x;(void)y; }
static void tch_frame(void *d, struct wl_touch *t) { (void)d;(void)t; }
static void tch_cancel(void *d, struct wl_touch *t) { (void)d;(void)t; }
static void tch_shape(void *d, struct wl_touch *t, int32_t id, wl_fixed_t a, wl_fixed_t b) { (void)d;(void)t;(void)id;(void)a;(void)b; }
static void tch_orientation(void *d, struct wl_touch *t, int32_t id, wl_fixed_t o) { (void)d;(void)t;(void)id;(void)o; }
static const struct wl_touch_listener touch_listener = {
	.down = tch_down, .up = tch_up, .motion = tch_motion, .frame = tch_frame,
	.cancel = tch_cancel, .shape = tch_shape, .orientation = tch_orientation,
};

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps) {
	(void)d;
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	}
	if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !touch) {
		touch = wl_seat_get_touch(s);
		wl_touch_add_listener(touch, &touch_listener, NULL);
	}
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d;(void)s;(void)n; }
static const struct wl_seat_listener seat_listener = { .capabilities = seat_caps, .name = seat_name };

static void out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y,
			 int32_t pw, int32_t ph, int32_t subpixel,
			 const char *make, const char *model, int32_t transform) {
	(void)d;(void)o;(void)x;(void)y;(void)subpixel;(void)make;(void)model;
	int w = pw, h = ph;
	if (transform == WL_OUTPUT_TRANSFORM_90 || transform == WL_OUTPUT_TRANSFORM_270 ||
	    transform == WL_OUTPUT_TRANSFORM_FLIPPED_90 || transform == WL_OUTPUT_TRANSFORM_FLIPPED_270) {
		int t = w; w = h; h = t;
	}
	(void)h;
	out_w = w > 0 ? w / scale : out_w;
	relayout();
}
static void out_mode(void *d, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t refresh) {
	(void)d;(void)o;(void)flags;(void)refresh;
	int ww = w, hh = h;
	/* surface is rotated by the compositor; width follows the long edge */
	if (hh > ww) { int t = ww; ww = hh; hh = t; }
	out_w = ww / scale;
	out_h = hh / scale;
	relayout();
}
static void out_done(void *d, struct wl_output *o) { (void)d;(void)o; }
static void out_scale(void *d, struct wl_output *o, int32_t f) { (void)d;(void)o;(void)f; }
static void out_name(void *d, struct wl_output *o, const char *n) { (void)d;(void)o;(void)n; }
static void out_description(void *d, struct wl_output *o, const char *s) { (void)d;(void)o;(void)s; }
static const struct wl_output_listener output_listener = {
	.geometry = out_geometry, .mode = out_mode, .done = out_done,
	.scale = out_scale, .name = out_name, .description = out_description,
};

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
	(void)d;
	if (!strcmp(iface, "wl_compositor"))
		compositor = wl_registry_bind(r, name, &wl_compositor_interface, ver < 4 ? ver : 4);
	else if (!strcmp(iface, "wl_shm"))
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, "zwlr_layer_shell_v1"))
		layer_shell = wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, ver < 4 ? ver : 4);
	else if (!strcmp(iface, "wl_seat")) {
		seat = wl_registry_bind(r, name, &wl_seat_interface, ver < 7 ? ver : 7);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (!strcmp(iface, "wl_output")) {
		output = wl_registry_bind(r, name, &wl_output_interface, ver < 4 ? ver : 4);
		wl_output_add_listener(output, &output_listener, NULL);
	}
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d;(void)r;(void)n; }
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

/* ------------------------------------------------------------------ main */

int main(void) {
	signal(SIGCHLD, SIG_IGN);   /* spawned apps must not become zombies */
	load_apps();
	const char *sc = getenv("DOCK_SCALE");
	if (sc) scale = atoi(sc);
	if (scale < 1) scale = 1;

	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGUSR1);
	sigaddset(&mask, SIGUSR2);
	sigprocmask(SIG_BLOCK, &mask, NULL);
	int sigfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);

	display = wl_display_connect(NULL);
	if (!display) { fprintf(stderr, "mango-dock: cannot connect\n"); return 1; }
	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !layer_shell) { fprintf(stderr, "mango-dock: missing globals\n"); return 1; }

	for (int i = 0; i < napps; i++) {
		apps[i].surf = cairo_image_surface_create_from_png(apps[i].icon);
		if (cairo_surface_status(apps[i].surf) != CAIRO_STATUS_SUCCESS) {
			fprintf(stderr, "mango-dock: icon missing: %s\n", apps[i].icon);
			cairo_surface_destroy(apps[i].surf);
			apps[i].surf = NULL;
		}
	}

	win_w = content_width();
	win_h = DOCK_H;
	alloc_buffer(win_w, win_h);

	surface = wl_compositor_create_surface(compositor);
	layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		layer_shell, surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "mango-dock");
	zwlr_layer_surface_v1_set_anchor(layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
	zwlr_layer_surface_v1_set_size(layer_surface, win_w, win_h);
	zwlr_layer_surface_v1_set_margin(layer_surface, 0, 0, MARGIN_BOT, (out_w - win_w) / 2);
	zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, DOCK_H + MARGIN_BOT);
	zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface,
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
	zwlr_layer_surface_v1_add_listener(layer_surface, &ls_listener, NULL);
	wl_surface_set_buffer_scale(surface, scale);
	wl_surface_commit(surface);
	wl_display_flush(display);

	while (1) {
		struct pollfd fds[2] = {
			{ .fd = wl_display_get_fd(display), .events = POLLIN },
			{ .fd = sigfd, .events = POLLIN },
		};
		wl_display_flush(display);
		if (poll(fds, 2, -1) < 0) {
			if (errno == EINTR) continue;
			break;
		}
		if (fds[0].revents & POLLIN) {
			if (wl_display_dispatch(display) < 0) break;
		}
		if (fds[1].revents & POLLIN) {
			struct signalfd_siginfo si;
			while (read(sigfd, &si, sizeof si) == sizeof si) {
				if (si.ssi_signo == SIGUSR1 && !hidden) {
					hidden = 1;
					wl_surface_attach(surface, NULL, 0, 0);
					wl_surface_commit(surface);
					write_geom();
				} else if (si.ssi_signo == SIGUSR2 && hidden) {
					hidden = 0;
					relayout();
					write_geom();
				}
			}
		}
	}
	return 0;
}
