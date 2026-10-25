/*
 * mango-osd — tiny volume OSD for MangoWM.
 *
 * A rounded pill at the top centre of the screen showing a level bar and the
 * percentage. It appears when SIGUSR1 arrives (reading the value from
 * $XDG_RUNTIME_DIR/mango-osd-value) and hides itself after 1.5 s.
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
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define OSD_W 360
#define OSD_H 92
#define OSD_R 26
#define TOP_MARGIN 58
#define SHOW_MS 1500

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zwlr_layer_surface_v1 *layer_surface;
static struct wl_surface *surface;
static struct wl_buffer *buf;
static unsigned char *pixels;
static int scale = 2, win_w = OSD_W, win_h = OSD_H, out_w = 1280;
static int visible = 0;
static int configured = 0;
static int pct = 40;
static long long hide_at = 0;
static time_t last_mtime = 0;

static const char *value_path(void) {
	static char path[512];
	const char *rt = getenv("XDG_RUNTIME_DIR");
	snprintf(path, sizeof path, "%s/mango-osd-value", rt ? rt : "/tmp");
	return path;
}

static long long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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

static void alloc_buffer(void) {
	int stride = win_w * scale * 4, size = stride * win_h * scale;
	int fd = memfd_create("mango-osd", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0) { perror("shm"); exit(1); }
	if (pixels) munmap(pixels, (size_t)win_w * scale * win_h * scale * 4);
	pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) { perror("mmap"); exit(1); }
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	if (buf) wl_buffer_destroy(buf);
	buf = wl_shm_pool_create_buffer(pool, 0, win_w * scale, win_h * scale, stride,
					WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
}

static void draw(void) {
	cairo_surface_t *cs = cairo_image_surface_create_for_data(
		pixels, CAIRO_FORMAT_ARGB32, win_w * scale, win_h * scale, win_w * scale * 4);
	cairo_t *cr = cairo_create(cs);
	cairo_scale(cr, scale, scale);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	rounded_rect(cr, 1, 1, win_w - 2, win_h - 2, OSD_R);
	cairo_set_source_rgba(cr, 0.09, 0.09, 0.11, 0.88);
	cairo_fill_preserve(cr);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);

	/* level track + fill */
	double bx = 26, by = win_h - 34, bw = win_w - 120, bh = 10;
	rounded_rect(cr, bx, by, bw, bh, bh / 2);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.16);
	cairo_fill(cr);
	double fw = bw * (pct / 100.0);
	if (fw > 1) {
		rounded_rect(cr, bx, by, fw, bh, bh / 2);
		cairo_set_source_rgba(cr, 1.0, 0.62, 0.30, 1.0);
		cairo_fill(cr);
	}

	/* percentage */
	cairo_set_source_rgba(cr, 0.96, 0.96, 0.98, 1);
	cairo_select_font_face(cr, "Noto Sans CJK SC", CAIRO_FONT_SLANT_NORMAL,
			       CAIRO_FONT_WEIGHT_BOLD);
	cairo_set_font_size(cr, 30);
	char tmp[16];
	snprintf(tmp, sizeof tmp, "%d%%", pct);
	cairo_move_to(cr, win_w - 92, 46);
	cairo_show_text(cr, tmp);

	/* speaker glyph (drawn, no font needed) */
	cairo_set_source_rgba(cr, 0.96, 0.96, 0.98, 1);
	cairo_set_line_width(cr, 3);
	cairo_move_to(cr, 34, 40);
	cairo_line_to(cr, 44, 40);
	cairo_line_to(cr, 56, 30);
	cairo_line_to(cr, 56, 62);
	cairo_line_to(cr, 44, 52);
	cairo_line_to(cr, 34, 52);
	cairo_close_path(cr);
	cairo_fill(cr);

	cairo_destroy(cr);
	cairo_surface_flush(cs);
	cairo_surface_destroy(cs);

	wl_surface_attach(surface, buf, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, win_w * scale, win_h * scale);
	wl_surface_commit(surface);
}

static void ls_configure(void *d, struct zwlr_layer_surface_v1 *ls, uint32_t serial,
			 uint32_t w, uint32_t h) {
	(void)d;
	zwlr_layer_surface_v1_ack_configure(ls, serial);
	configured = 1;
	if (w > 0) win_w = (int)w;
	if (h > 0) win_h = (int)h;
	alloc_buffer();
	if (visible)
		draw();
	else {
		wl_surface_attach(surface, NULL, 0, 0);
		wl_surface_commit(surface);
	}
}
static void ls_closed(void *d, struct zwlr_layer_surface_v1 *ls) { (void)d; (void)ls; exit(0); }
static const struct zwlr_layer_surface_v1_listener ls_listener = { .configure = ls_configure, .closed = ls_closed };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
	(void)d;
	if (!strcmp(iface, "wl_compositor"))
		compositor = wl_registry_bind(r, name, &wl_compositor_interface, ver < 4 ? ver : 4);
	else if (!strcmp(iface, "wl_shm"))
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, "zwlr_layer_shell_v1"))
		layer_shell = wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, ver < 4 ? ver : 4);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

static void show_value(void) {
	FILE *f = fopen(value_path(), "r");
	if (f) {
		if (fscanf(f, "%d", &pct) != 1) pct = 0;
		fclose(f);
	}
	if (pct < 0) pct = 0;
	if (pct > 100) pct = 100;
	visible = 1;
	hide_at = now_ms() + SHOW_MS;
	if (!configured)
		return;          /* draw once the compositor configured us */
	draw();
}

int main(void) {
	const char *sc = getenv("DOCK_SCALE");
	if (sc) scale = atoi(sc);
	if (scale < 1) scale = 1;

	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGUSR1);
	sigprocmask(SIG_BLOCK, &mask, NULL);
	int sigfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);

	display = wl_display_connect(NULL);
	if (!display) return 1;
	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !layer_shell) return 1;

	alloc_buffer();
	surface = wl_compositor_create_surface(compositor);
	layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		layer_shell, surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "mango-osd");
	zwlr_layer_surface_v1_set_anchor(layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
	zwlr_layer_surface_v1_set_size(layer_surface, win_w, win_h);
	zwlr_layer_surface_v1_set_margin(layer_surface, TOP_MARGIN, 0, 0, (out_w - win_w) / 2);
	zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, -1);
	zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface,
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
	zwlr_layer_surface_v1_add_listener(layer_surface, &ls_listener, NULL);
	wl_surface_set_buffer_scale(surface, scale);
	wl_surface_commit(surface);           /* start hidden: nothing attached yet */
	{ struct stat st; if (stat(value_path(), &st) == 0) last_mtime = st.st_mtime; }

	while (1) {
		int timeout = -1;
		if (visible) {
			long long left = hide_at - now_ms();
			timeout = left > 0 ? (int)left : 0;
		}
		struct pollfd fds[2] = {
			{ .fd = wl_display_get_fd(display), .events = POLLIN },
			{ .fd = sigfd, .events = POLLIN },
		};
		wl_display_flush(display);
		if (timeout < 0 || timeout > 300) timeout = 300;
		if (poll(fds, 2, timeout) < 0) {
			if (errno == EINTR) continue;
			break;
		}
		if (fds[0].revents & POLLIN && wl_display_dispatch(display) < 0) break;
		if (fds[1].revents & POLLIN) {
			struct signalfd_siginfo si;
			while (read(sigfd, &si, sizeof si) == sizeof si) { }
			show_value();
		}
		struct stat st;
		if (stat(value_path(), &st) == 0 && st.st_mtime != last_mtime) {
			last_mtime = st.st_mtime;   /* volume changed by another process */
			show_value();
		}
		if (visible && now_ms() >= hide_at) {
			visible = 0;
			wl_surface_attach(surface, NULL, 0, 0);
			wl_surface_commit(surface);
		}
	}
	return 0;
}
