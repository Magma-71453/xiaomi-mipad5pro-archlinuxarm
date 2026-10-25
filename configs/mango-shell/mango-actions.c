/*
 * mango-actions — tap-to-edit window controls for the touch desktop.
 *
 * Enabled from the dock ("actions" button). While active a full-screen
 * transparent layer surface captures input:
 *
 *   tap a window  -> three round buttons appear along a random edge of it
 *                    [ × ]  close
 *                    [ ○ ]  full screen
 *                    [ ✋ ] grab & drag the window
 *
 *   tap elsewhere -> deselect (mode stays on)
 *   button again  -> leave the mode
 */
#define _GNU_SOURCE
#include <cairo/cairo.h>
#include <cjson/cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <glob.h>
#include <sys/file.h>
#include <math.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define BTN 64.0
#define NBUTTONS 3
#define GAP 18.0
#define HANDLE 15.0

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct wl_touch *touch;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zwlr_layer_surface_v1 *ls;
static struct wl_surface *surface;
static struct wl_buffer *buf;
static unsigned char *pixels;
static int scale = 2, win_w = 1280, win_h = 800;
static char sock_path[108];
static int active;          /* mode on */
static int configured;

/* selection */
static int sel_id;
static double sel_x, sel_y, sel_w, sel_h;
static double btn_x[NBUTTONS], btn_y[NBUTTONS];   /* top-left of each button */
static double hx[4], hy[4];                        /* corner handle centres */
static int btn_hover = -1;
static int want_full_damage = 1;   /* full repaint at the start/end of a gesture */
static int dragging;
static int resizing;
static int corner = -1;          /* 0 TL, 1 TR, 2 BL, 3 BR while dragging */
static double last_x, last_y;

/* ------------------------------------------------------------ IPC helper */

static int ipc(const char *command, char *out, size_t outsz) {
	if (!sock_path[0])
		return 0;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return 0;
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	snprintf(addr.sun_path, sizeof addr.sun_path, "%s", sock_path);
	struct timeval tv = { 0, 250000 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		close(fd);
		return 0;
	}
	char line[600];
	snprintf(line, sizeof line, "%s\n", command);
	send(fd, line, strlen(line), MSG_NOSIGNAL);
	size_t used = 0;
	ssize_t n;
	while ((n = recv(fd, out + used, outsz - 1 - used, 0)) > 0) {
		used += (size_t)n;
		if (used && out[used - 1] == '}')
			break;
	}
	out[used] = 0;
	close(fd);
	return (int)used;
}

/* Fire-and-forget.  mango closes the connection after every command, so a
 * long-lived socket silently swallowed the following commands (the close /
 * fullscreen buttons stopped working); reconnecting per command is cheap on a
 * unix socket, and dropping the reply wait is what made dragging smooth. */
static int cmd(const char *fmt, ...) {
	char b[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	if (!sock_path[0])
		return 0;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return 0;
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	snprintf(addr.sun_path, sizeof addr.sun_path, "%s", sock_path);
	if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		close(fd);
		return 0;
	}
	size_t len = strlen(b);
	if (len + 2 >= sizeof b) {
		close(fd);
		return 0;
	}
	b[len++] = '\n';
	int ok = send(fd, b, len, MSG_NOSIGNAL) == (ssize_t)len;
	close(fd);
	return ok;
}

/* ------------------------------------------------------------- drawing */

static void circle(cairo_t *cr, double cx, double cy, double r) {
	cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
}

static void draw(void) {
	if (getenv("ACT_DEBUG"))
		fprintf(stderr, "draw sel=%d active=%d buf=%p conf=%d\n", sel_id, active, (void *)buf, configured);
	if (!buf || !configured)
		return;
	cairo_surface_t *cs = cairo_image_surface_create_for_data(
		pixels, CAIRO_FORMAT_ARGB32, win_w * scale, win_h * scale, win_w * scale * 4);
	cairo_t *cr = cairo_create(cs);
	cairo_scale(cr, scale, scale);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	if (active) {
		double bx = win_w - 152, by = 18, bw = 134, bh = 46;
		cairo_new_path(cr);
		cairo_arc(cr, bx + bh / 2, by + bh / 2, bh / 2, M_PI / 2, 1.5 * M_PI);
		cairo_line_to(cr, bx + bw - bh / 2, by);
		cairo_arc(cr, bx + bw - bh / 2, by + bh / 2, bh / 2, 1.5 * M_PI, 2 * M_PI);
		cairo_line_to(cr, bx + bw, by + bh);
		cairo_line_to(cr, bx, by + bh);
		cairo_close_path(cr);
		cairo_set_source_rgba(cr, 0.20, 0.20, 0.25, 0.96);
		cairo_fill_preserve(cr);
		cairo_set_source_rgba(cr, 1, 1, 1, 0.25);
		cairo_set_line_width(cr, 1.5);
		cairo_stroke(cr);
		cairo_select_font_face(cr, "Noto Sans CJK SC", CAIRO_FONT_SLANT_NORMAL,
				       CAIRO_FONT_WEIGHT_BOLD);
		cairo_set_font_size(cr, 17);
		cairo_set_source_rgba(cr, 0.96, 0.96, 1, 1);
		cairo_move_to(cr, bx + 24, by + 30);
		cairo_show_text(cr, "完成");
	}
	if (active && sel_id > 0) {
		/* very light dim so the mode is visible but the screen stays readable */
		cairo_set_source_rgba(cr, 0, 0, 0, 0.18);
		cairo_paint(cr);

		for (int i = 0; i < 4; i++) {
			cairo_new_path(cr);
			cairo_arc(cr, hx[i], hy[i], HANDLE, 0, 2 * M_PI);
			cairo_set_source_rgba(cr, 0.16, 0.16, 0.20, 0.9);
			cairo_fill_preserve(cr);
			cairo_set_source_rgba(cr, 1, 1, 1, 0.35);
			cairo_set_line_width(cr, 1.5);
			cairo_stroke(cr);
			cairo_new_path(cr);
			cairo_arc(cr, hx[i], hy[i], 3.0, 0, 2 * M_PI);
			cairo_set_source_rgba(cr, 0.85, 0.85, 0.9, 0.95);
			cairo_fill(cr);
		}

		for (int i = 0; i < NBUTTONS; i++) {
			double cx = btn_x[i] + BTN / 2, cy = btn_y[i] + BTN / 2;
			cairo_new_path(cr);
			cairo_arc(cr, cx, cy, BTN / 2, 0, 2 * M_PI);
			if (btn_hover == i)
				cairo_set_source_rgba(cr, 0.30, 0.30, 0.36, 0.98);
			else
				cairo_set_source_rgba(cr, 0.16, 0.16, 0.20, 0.96);
			cairo_fill_preserve(cr);
			cairo_set_source_rgba(cr, 1, 1, 1, 0.22);
			cairo_set_line_width(cr, 1.5);
			cairo_stroke(cr);

			cairo_new_path(cr);
			cairo_set_source_rgba(cr, 0.95, 0.95, 0.98, 1);
			cairo_select_font_face(cr, "Noto Sans CJK SC", CAIRO_FONT_SLANT_NORMAL,
					       CAIRO_FONT_WEIGHT_BOLD);
			if (i == 0) {                 /* × */
				cairo_set_line_width(cr, 4.0);
				cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
				cairo_move_to(cr, cx - 11, cy - 11);
				cairo_line_to(cr, cx + 11, cy + 11);
				cairo_move_to(cr, cx + 11, cy - 11);
				cairo_line_to(cr, cx - 11, cy + 11);
				cairo_stroke(cr);
			} else if (i == 1) {          /* ○ full screen */
				cairo_set_line_width(cr, 3.2);
				cairo_arc(cr, cx, cy, 12, 0, 2 * M_PI);
				cairo_stroke(cr);
			} else if (i == 2) {          /* ✋ hand: grab & drag */
				cairo_select_font_face(cr, "Noto Sans Symbols2", CAIRO_FONT_SLANT_NORMAL,
						       CAIRO_FONT_WEIGHT_NORMAL);
				cairo_set_font_size(cr, 30);
				cairo_text_extents_t ext;
				cairo_text_extents(cr, "✋", &ext);
				cairo_move_to(cr, cx - ext.width / 2 - ext.x_bearing,
					      cy - ext.height / 2 - ext.y_bearing);
				cairo_show_text(cr, "✋");
				cairo_new_path(cr);
			} else {                      /* ⤢ resize: drag bottom-right */
				cairo_set_line_width(cr, 3.0);
				cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
				cairo_move_to(cr, cx - 12, cy + 12);
				cairo_line_to(cr, cx + 12, cy - 12);
				cairo_move_to(cr, cx + 12, cy - 12);
				cairo_line_to(cr, cx + 12, cy - 3);
				cairo_move_to(cr, cx + 12, cy - 12);
				cairo_line_to(cr, cx + 3, cy - 12);
				cairo_move_to(cr, cx - 12, cy + 12);
				cairo_line_to(cr, cx - 12, cy + 3);
				cairo_move_to(cr, cx - 12, cy + 12);
				cairo_line_to(cr, cx - 3, cy + 12);
				cairo_stroke(cr);
			}
		}
	}

	cairo_destroy(cr);
	cairo_surface_flush(cs);
	cairo_surface_destroy(cs);
	wl_surface_attach(surface, buf, 0, 0);

	/* The overlay covers the whole screen; damaging all of it made every drag
	 * motion upload ~16 MB.  Only the selection box and the button strip ever
	 * change. */
	static double prev_x, prev_y, prev_w, prev_h;
	static int have_prev;
	if (!have_prev || sel_id == 0 || prev_w <= 0 || want_full_damage) {
		want_full_damage = 0;
		wl_surface_damage_buffer(surface, 0, 0, win_w * scale, win_h * scale);
	} else {
		double x0 = fmin(prev_x, sel_x) - 30, y0 = fmin(prev_y, sel_y) - 30;
		double x1 = fmax(prev_x + prev_w, sel_x + sel_w) + 30;
		double y1 = fmax(prev_y + prev_h, sel_y + sel_h) + 30;
		if (x0 < 0) x0 = 0;
		if (y0 < 0) y0 = 0;
		if (x1 > win_w) x1 = win_w;
		if (y1 > win_h) y1 = win_h;
		if (x1 > x0 && y1 > y0)
			wl_surface_damage_buffer(surface, (int32_t)(x0 * scale), (int32_t)(y0 * scale),
					  (int32_t)((x1 - x0) * scale), (int32_t)((y1 - y0) * scale));
		/* the action buttons follow the selection, so both their previous and
		 * current boxes have to be repainted or they leave ghosts behind */
		double bx0 = 1e9, by0 = 1e9, bx1 = -1e9, by1 = -1e9;
		for (int i = 0; i < NBUTTONS; i++) {
			if (btn_x[i] < -1e8)
				continue;
			if (btn_x[i] - 8 < bx0) bx0 = btn_x[i] - 8;
			if (btn_y[i] - 8 < by0) by0 = btn_y[i] - 8;
			if (btn_x[i] + BTN + 8 > bx1) bx1 = btn_x[i] + BTN + 8;
			if (btn_y[i] + BTN + 8 > by1) by1 = btn_y[i] + BTN + 8;
		}
		if (bx1 > bx0)
			wl_surface_damage_buffer(surface, (int32_t)(bx0 * scale), (int32_t)(by0 * scale),
					  (int32_t)((bx1 - bx0) * scale), (int32_t)((by1 - by0) * scale));
		static double pbx0, pby0, pbx1, pby1;
		static int pb_ok;
		if (pb_ok && pbx1 > pbx0)
			wl_surface_damage_buffer(surface, (int32_t)(pbx0 * scale), (int32_t)(pby0 * scale),
					  (int32_t)((pbx1 - pbx0) * scale), (int32_t)((pby1 - pby0) * scale));
		pbx0 = bx0; pby0 = by0; pbx1 = bx1; pby1 = by1; pb_ok = 1;
	}
	prev_x = sel_x;
	prev_y = sel_y;
	prev_w = sel_w;
	prev_h = sel_h;
	have_prev = 1;
	wl_surface_commit(surface);
}

static void alloc_buffer(int w, int h) {
	int stride = w * scale * 4, size = stride * h * scale;
	int fd = memfd_create("mango-actions", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0)
		return;
	if (pixels)
		munmap(pixels, (size_t)win_w * scale * win_h * scale * 4);
	pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) {
		close(fd);
		return;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	if (buf)
		wl_buffer_destroy(buf);
	buf = wl_shm_pool_create_buffer(pool, 0, w * scale, h * scale, stride,
					WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	win_w = w;
	win_h = h;
}

/* --------------------------------------------------------- selection/buttons */

static int client_at(double x, double y, int *id, double *cx, double *cy,
		     double *cw, double *ch) {
	char *b = malloc(262144);
	if (!b)
		return 0;
	int ok = 0;
	if (ipc("get all-clients", b, 262144)) {
		cJSON *root = cJSON_Parse(b);
		cJSON *arr = root ? cJSON_GetObjectItem(root, "clients") : NULL;
		if (cJSON_IsArray(arr)) {
			cJSON *c;
			cJSON_ArrayForEach(c, arr) {
				if (!cJSON_IsTrue(cJSON_GetObjectItem(c, "is_visible")))
					continue;
				if (!cJSON_IsTrue(cJSON_GetObjectItem(c, "is_floating")))
					continue;
				double x0 = cJSON_GetObjectItem(c, "x")->valuedouble;
				double y0 = cJSON_GetObjectItem(c, "y")->valuedouble;
				double w0 = cJSON_GetObjectItem(c, "width")->valuedouble;
				double h0 = cJSON_GetObjectItem(c, "height")->valuedouble;
				if (x >= x0 && x <= x0 + w0 && y >= y0 && y <= y0 + h0) {
					*id = cJSON_GetObjectItem(c, "id")->valueint;
					*cx = x0; *cy = y0; *cw = w0; *ch = h0;
					ok = 1;
				}
			}
		}
		if (root)
			cJSON_Delete(root);
	}
	free(b);
	return ok;
}

/* buttons sit centred on the window's bottom edge; four corner grips resize */
static void layout_buttons(void) {
	double total = NBUTTONS * BTN + (NBUTTONS - 1) * GAP;
	double x0 = sel_x + (sel_w - total) / 2;
	double y0 = sel_y + sel_h - BTN - 16;
	for (int i = 0; i < NBUTTONS; i++) {
		btn_x[i] = x0 + i * (BTN + GAP);
		btn_y[i] = y0;
	}
	if (btn_x[0] < 8) {
		for (int i = 0; i < NBUTTONS; i++)
			btn_x[i] += 8 - btn_x[0];
	}
	if (btn_x[NBUTTONS - 1] + BTN > win_w - 8) {
		double d = btn_x[NBUTTONS - 1] + BTN - (win_w - 8);
		for (int i = 0; i < NBUTTONS; i++)
			btn_x[i] -= d;
	}
	if (btn_y[0] < 8)
		for (int i = 0; i < NBUTTONS; i++)
			btn_y[i] = 8;
	/* keep clear of the dock strip along the bottom */
	if (btn_y[0] + BTN > win_h - 96)
		for (int i = 0; i < NBUTTONS; i++)
			btn_y[i] = win_h - 96 - BTN;

	hx[0] = sel_x;             hy[0] = sel_y;
	hx[1] = sel_x + sel_w;     hy[1] = sel_y;
	hx[2] = sel_x;             hy[2] = sel_y + sel_h;
	hx[3] = sel_x + sel_w;     hy[3] = sel_y + sel_h;
}

static int hit_corner(double x, double y) {
	for (int i = 0; i < 4; i++) {
		double dx = x - hx[i], dy = y - hy[i];
		if (dx * dx + dy * dy <= (HANDLE + 10) * (HANDLE + 10))
			return i;
	}
	return -1;
}

static int hit_button(double x, double y) {
	for (int i = 0; i < NBUTTONS; i++) {
		double dx = x - (btn_x[i] + BTN / 2), dy = y - (btn_y[i] + BTN / 2);
		if (dx * dx + dy * dy <= (BTN / 2) * (BTN / 2))
			return i;
	}
	return -1;
}

static void clear_selection(void) {
	sel_id = 0;
	btn_hover = -1;
	draw();
}

static struct timeval last_cmd_tv;
static int cmd_allowed(void) {
	struct timeval now;
	gettimeofday(&now, NULL);
	long us = (now.tv_sec - last_cmd_tv.tv_sec) * 1000000L + (now.tv_usec - last_cmd_tv.tv_usec);
	if (last_cmd_tv.tv_sec && us >= 0 && us < 10000)
		return 0;
	last_cmd_tv = now;
	return 1;
}

static void on_press(double x, double y) {
	want_full_damage = 1;
	if (getenv("ACT_DEBUG"))
		fprintf(stderr, "press %.0f,%.0f active=%d sel=%d\n", x, y, active, sel_id);
	if (!active) return;
	if (x >= win_w - 152 && x <= win_w - 18 && y >= 18 && y <= 64) {
		/* 完成: leave the mode */
		const char *rt = getenv("XDG_RUNTIME_DIR");
		char flag[512];
		snprintf(flag, sizeof flag, "%s/mango-actions", rt ? rt : "/tmp");
		unlink(flag);
		sel_id = 0;
		active = 0;
		if (configured) {
			wl_surface_attach(surface, NULL, 0, 0);
			wl_surface_commit(surface);
		}
		return;
	}
	if (sel_id > 0) {
		int k = hit_corner(x, y);
		if (k >= 0) {
			corner = k;
			resizing = 1;
			last_x = x;
			last_y = y;
			return;
		}
		int b = hit_button(x, y);
		if (b == 0) {
			cmd("dispatch killclient client,%d", sel_id);
			clear_selection();
			return;
		}
		if (b == 1) {
			cmd("dispatch togglefullscreen client,%d", sel_id);
			clear_selection();
			return;
		}
		if (b == 2) {
			dragging = 1;
			last_x = x;
			last_y = y;
			return;
		}
		if (b == 3) {
			resizing = 1;
			last_x = x;
			last_y = y;
			return;
		}
		/* tapped outside the buttons: try selecting another window */
	}
	int id;
	double wx, wy, ww, wh;
	if (client_at(x, y, &id, &wx, &wy, &ww, &wh)) {
		/* bring the tapped window forward before showing its buttons */
		cmd("dispatch focusid client,%d", id);
		sel_id = id;
		sel_x = wx; sel_y = wy; sel_w = ww; sel_h = wh;
		layout_buttons();
		draw();
	} else {
		clear_selection();
	}
}

static double acc_mx, acc_my, acc_rx, acc_ry;

static void flush_move(void) {
	if (!sel_id)
		return;
	if ((acc_mx || acc_my) &&
	    cmd("dispatch movewin,%+d,%+d client,%d", (int)acc_mx, (int)acc_my, sel_id))
		acc_mx = acc_my = 0;
	if ((acc_rx || acc_ry) &&
	    cmd("dispatch resizewin,%+d,%+d client,%d", (int)acc_rx, (int)acc_ry, sel_id))
		acc_rx = acc_ry = 0;
	cmd_allowed();   /* keep the 10 ms gate even when a send failed */
}

static void on_motion(double x, double y) {
	if (!active) return;
	if (resizing && sel_id > 0) {
		int dx = (int)(x - last_x), dy = (int)(y - last_y);
		if (!dx && !dy)
			return;
		if (corner < 0) {                 /* ⤢ button: bottom-right grows */
			acc_rx += dx;
			acc_ry += dy;
			sel_w += dx;
			sel_h += dy;
		} else {
			switch (corner) {
			case 0:                       /* top-left */
				acc_mx += dx; acc_my += dy;
				acc_rx -= dx; acc_ry -= dy;
				sel_x += dx; sel_y += dy; sel_w -= dx; sel_h -= dy;
				break;
			case 1:                       /* top-right */
				acc_my += dy;
				acc_rx += dx; acc_ry -= dy;
				sel_y += dy; sel_w += dx; sel_h -= dy;
				break;
			case 2:                       /* bottom-left */
				acc_mx += dx;
				acc_rx -= dx; acc_ry += dy;
				sel_x += dx; sel_w -= dx; sel_h += dy;
				break;
			default:                      /* bottom-right */
				acc_rx += dx; acc_ry += dy;
				sel_w += dx; sel_h += dy;
				break;
			}
		}
		last_x = x;
		last_y = y;
		layout_buttons();
		draw();
		/* never send more than ~100 commands per second */
		if (cmd_allowed())
			flush_move();
		return;
	}
	if (dragging && sel_id > 0) {
		int dx = (int)(x - last_x), dy = (int)(y - last_y);
		if (dx || dy) {
			acc_mx += dx;
			acc_my += dy;
			sel_x += dx;
			sel_y += dy;
			last_x = x;
			last_y = y;
			layout_buttons();
			draw();
			if (cmd_allowed())
				flush_move();
		}
		return;
	}
	if (sel_id > 0) {
		int k = hit_corner(x, y);
		if (k >= 0) {
			corner = k;
			resizing = 1;
			last_x = x;
			last_y = y;
			return;
		}
		int b = hit_button(x, y);
		if (b != btn_hover) {
			btn_hover = b;
			draw();
		}
	}
}

static void on_release(void) {
	flush_move();
	dragging = 0;
	resizing = 0;
	corner = -1;
	/* clear whatever the partial updates left behind */
	want_full_damage = 1;
	draw();
}

/* ------------------------------------------------------------ wayland glue */

static void ls_configure(void *d, struct zwlr_layer_surface_v1 *l, uint32_t serial,
			 uint32_t w, uint32_t h) {
	(void)d;
	zwlr_layer_surface_v1_ack_configure(l, serial);
	configured = 1;
	if (w && h && ((int)w != win_w || (int)h != win_h))
		alloc_buffer((int)w, (int)h);
	if (active)
		draw();
	else {
		wl_surface_attach(surface, NULL, 0, 0);
		wl_surface_commit(surface);
	}
}
static void ls_closed(void *d, struct zwlr_layer_surface_v1 *l) { (void)d; (void)l; exit(0); }
static const struct zwlr_layer_surface_v1_listener ls_listener = {
	.configure = ls_configure, .closed = ls_closed
};

static double ptr_x, ptr_y;
static void ptr_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf,
		      wl_fixed_t x, wl_fixed_t y) {
	(void)d;(void)p;(void)s;(void)sf;
	ptr_x = wl_fixed_to_double(x);
	ptr_y = wl_fixed_to_double(y);
	if (getenv("ACT_DEBUG"))
		fprintf(stderr, "ptr enter %.0f,%.0f\n", ptr_x, ptr_y);
}
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf) { (void)d;(void)p;(void)s;(void)sf; }
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
	(void)d;(void)p;(void)t;
	ptr_x = wl_fixed_to_double(x);
	ptr_y = wl_fixed_to_double(y);
	if (getenv("ACT_DEBUG"))
		fprintf(stderr, "ptr motion %.0f,%.0f\n", ptr_x, ptr_y);
	on_motion(ptr_x, ptr_y);
}
static void ptr_button(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, uint32_t b, uint32_t st) {
	(void)d;(void)p;(void)s;(void)t;
	if (b != BTN_LEFT) return;
	/* mango delivers touch through pointer emulation, so drive the UI here */
	if (st == WL_POINTER_BUTTON_STATE_PRESSED)
		on_press(ptr_x, ptr_y);
	else
		on_release();
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

static void tch_down(void *d, struct wl_touch *t, uint32_t s, uint32_t tm,
		     struct wl_surface *sf, int32_t id, wl_fixed_t x, wl_fixed_t y) {
	(void)d;(void)t;(void)s;(void)tm;(void)sf;(void)id;
	if (getenv("ACT_DEBUG"))
		fprintf(stderr, "touch down\n");
	on_press(wl_fixed_to_double(x), wl_fixed_to_double(y));
}
static void tch_up(void *d, struct wl_touch *t, uint32_t s, uint32_t tm, int32_t id) {
	(void)d;(void)t;(void)s;(void)tm;(void)id;
	on_release();
}
static void tch_motion(void *d, struct wl_touch *t, uint32_t tm, int32_t id, wl_fixed_t x, wl_fixed_t y) {
	(void)d;(void)t;(void)tm;(void)id;
	on_motion(wl_fixed_to_double(x), wl_fixed_to_double(y));
}
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
	if (getenv("ACT_DEBUG"))
		fprintf(stderr, "seat caps=0x%x\n", caps);
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
	}
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d;(void)r;(void)n; }
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

int main(int argc, char **argv) {
	const char *sc = getenv("DOCK_SCALE");
	if (sc) scale = atoi(sc);
	if (scale < 1) scale = 1;
	const char *sig = getenv("MANGO_INSTANCE_SIGNATURE");
	if (argc > 1 && argv[1])
		snprintf(sock_path, sizeof sock_path, "%s", argv[1]);
	else if (sig)
		snprintf(sock_path, sizeof sock_path, "%s", sig);
	if (!sock_path[0]) {
		/* started by hand (or by a launcher that forgets the argument): find the
		 * compositor socket ourselves, otherwise every command silently goes
		 * nowhere and the edit mode looks broken. */
		const char *rt = getenv("XDG_RUNTIME_DIR");
		char pat[512];
		snprintf(pat, sizeof pat, "%s/mango-*.sock", rt ? rt : "/run/user/1000");
		glob_t g;
		if (glob(pat, 0, NULL, &g) == 0 && g.gl_pathc > 0)
			snprintf(sock_path, sizeof sock_path, "%s", g.gl_pathv[0]);
		globfree(&g);
	}
	fprintf(stderr, "mango-actions: ipc socket = %s\n", sock_path[0] ? sock_path : "<none>");

	/* one instance only: two full-screen overlays fight over the same touches */
	{
		const char *rt2 = getenv("XDG_RUNTIME_DIR");
		char lk[512];
		snprintf(lk, sizeof lk, "%s/mango-actions.lock", rt2 ? rt2 : "/tmp");
		int lockfd = open(lk, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
		if (lockfd >= 0 && flock(lockfd, LOCK_EX | LOCK_NB) < 0) {
			fprintf(stderr, "mango-actions: another instance is running, exiting\n");
			return 0;
		}
	}
	srand((unsigned)time(NULL));

	display = wl_display_connect(NULL);
	if (!display)
		return 1;
	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !layer_shell)
		return 1;

	alloc_buffer(win_w, win_h);
	surface = wl_compositor_create_surface(compositor);
	ls = zwlr_layer_shell_v1_get_layer_surface(
		layer_shell, surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "mango-actions");
	zwlr_layer_surface_v1_set_anchor(ls, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
					     ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
					     ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
					     ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_size(ls, 0, 0);
	zwlr_layer_surface_v1_set_exclusive_zone(ls, -1);
	zwlr_layer_surface_v1_set_keyboard_interactivity(ls,
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
	zwlr_layer_surface_v1_add_listener(ls, &ls_listener, NULL);
	wl_surface_set_buffer_scale(surface, scale);
	wl_surface_commit(surface);

	/* the layer is mapped by the first configure; no extra commit here */

	const char *rt = getenv("XDG_RUNTIME_DIR");
	char flag[512];
	snprintf(flag, sizeof flag, "%s/mango-actions", rt ? rt : "/tmp");

	int was_active = 0;
	while (1) {
		struct pollfd pfd = { .fd = wl_display_get_fd(display), .events = POLLIN };
		wl_display_flush(display);
		poll(&pfd, 1, 150);
		if (pfd.revents & POLLIN) {
			if (wl_display_dispatch(display) < 0)
				return 0;
		}

		int now = access(flag, F_OK) == 0;
		if (getenv("ACT_DEBUG"))
			fprintf(stderr, "poll active=%d configured=%d\n", now, configured);
		if (now != was_active) {
			was_active = now;
			active = now;
			if (!now)
				clear_selection();
			if (now) {
				alloc_buffer(win_w, win_h);
				draw();
			} else if (configured) {
				wl_surface_attach(surface, NULL, 0, 0);
				wl_surface_commit(surface);
			}
		}
	}
	return 0;
}
