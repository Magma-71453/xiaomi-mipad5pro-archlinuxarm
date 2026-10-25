/*
 * mango-bar — lightweight top status bar for MangoWM (elish).
 *
 *   (foot · ~) (bilibili · …)     周日,10/25 23:37:24   (tray…) (v) (🔉📶ᛒ) (⏻)
 *
 * Left   : one rounded rect per window of the current workspace.  Tap focuses,
 *          long press hides the window into the system tray.
 * Middle : clock.
 * Tray   : windows hidden by a long press are "parked" off screen; their icons
 *          live left of the status pill.  Tap restores, long press opens a menu
 *          (restore / launch entries / close).  The chevron button on the left
 *          of the tray collapses or expands the tray icons.
 * Status : one pill, three independent targets - speaker / wifi / bluetooth -
 *          each opening its own settings tool (kitty -1 -e ...).
 * Power  : two-step confirm, then systemctl poweroff.
 *
 * Wayland layer-shell + cairo on a rotating pair of shm buffers; no GTK/Qt.
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
#define MAX_TRAY     6
#define TRAY_ITEM    32
#define TRAY_GAP     6
#define ARROW_W      30
#define STATUS_W     130
#define POWER_D      34
#define LONG_PRESS_MS 550

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

struct client { int id; char appid[64]; char title[192]; int focused; int visible; int tag; int x, y, w, h; };
struct trayent { int id; char appid[64]; char title[192]; int tag; int x, y, w, h; };
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
static struct trayent tray[MAX_TRAY];
static int ntray;
static int tray_open = 1;
static char clock_str[64];
static int vol_pct = 0, vol_mute = 0, wifi_on = 0, bt_on = 0;
static int power_armed = 0;
static time_t power_armed_at = 0;
static int dirty = 1;
static int pointer_x = -1, pointer_y = -1;

static char cmd_audio[256] = "kitty -1 -e alsamixer";
static char cmd_network[256] = "kitty -1 -e nmtui";
static char cmd_bluetooth[256] = "kitty -1 -e bluetoothctl";

static struct box pill_box[MAX_PILLS];
static int n_pill_boxes;
static struct box tray_box[MAX_TRAY];
static int n_tray_boxes;
static struct box arrow_box, status_box, status_icon[3], power_box;

static void repaint(void);

/* ========================================================================== *
 * StatusNotifierItem host (the real "system tray")
 *
 * Applications that "close to tray" register with org.kde.StatusNotifierWatcher
 * on the session bus and expose org.kde.StatusNotifierItem.  Without a host
 * that answers the watcher interface they simply exit, so implement both sides
 * here: the watcher service plus one icon per registered item.
 * ========================================================================== */

#include <systemd/sd-bus.h>

#define MAX_SNI     8
#define SNI_ICON    32

struct sni_item {
	char *service;
	char *path;
	char *id;
	char *title;
	char *icon_name;
	int status;
	uint32_t *pix;      /* ARGB32, SNI_ICON x SNI_ICON */
	int pix_w, pix_h;
	int menu_is_item;
};

static sd_bus *bus;
static int bus_fd = -1;
static struct sni_item snis[MAX_SNI];
static int nsni;
static int sni_dirty;

static const char *SNI_WATCHER = "org.kde.StatusNotifierWatcher";
static const char *SNI_WATCHER_PATH = "/StatusNotifierWatcher";
static const char *SNI_IFACE = "org.kde.StatusNotifierItem";

static uint32_t *sni_pixmap_from_message(sd_bus_message *m, int *out_w, int *out_h) {
	int best_w = 0, best_h = 0;
	uint32_t *best = NULL;
	int r;
	if ((r = sd_bus_message_enter_container(m, 'a', "(iiay)")) < 0) return NULL;
	while (sd_bus_message_enter_container(m, 'r', "(iiay)") > 0) {
		int w = 0, h = 0;
		const void *data = NULL;
		size_t n = 0;
		if (sd_bus_message_read(m, "ii", &w, &h) < 0) break;
		if (sd_bus_message_read_array(m, 'y', &data, &n) < 0) break;
		if (w > 0 && h > 0 && n >= (size_t)w * h * 4 && w <= 128 && h <= 128) {
			uint32_t *argb = malloc((size_t)w * h * 4);
			if (argb) {
				const unsigned char *b = data;
				for (int i = 0; i < w * h; i++)
					argb[i] = ((uint32_t)b[i * 4] << 24) | ((uint32_t)b[i * 4 + 1] << 16) |
						  ((uint32_t)b[i * 4 + 2] << 8) | (uint32_t)b[i * 4 + 3];
				if (!best || w > best_w) {
					free(best);
					best = argb;
					best_w = w;
					best_h = h;
				} else {
					free(argb);
				}
			}
		}
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_exit_container(m);
	*out_w = best_w;
	*out_h = best_h;
	return best;
}

static void sni_refresh_icon(struct sni_item *it) {
	sd_bus_error err = SD_BUS_ERROR_NULL;
	sd_bus_message *m = NULL;
	free(it->pix);
	it->pix = NULL;
	it->pix_w = it->pix_h = 0;
	if (sd_bus_call_method(bus, it->service, it->path, "org.freedesktop.DBus.Properties",
			       "Get", &err, &m, "ss", SNI_IFACE, "IconPixmap") < 0) {
		sd_bus_error_free(&err);
		return;
	}
	if (sd_bus_message_enter_container(m, 'v', "a(iiay)") > 0) {
		it->pix = sni_pixmap_from_message(m, &it->pix_w, &it->pix_h);
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_unref(m);
	sd_bus_error_free(&err);
}

static char *sni_prop_str(struct sni_item *it, const char *name) {
	char *out = NULL;
	sd_bus_error err = SD_BUS_ERROR_NULL;
	if (sd_bus_get_property_string(bus, it->service, it->path, SNI_IFACE, name, &err, &out) < 0) {
		sd_bus_error_free(&err);
		return NULL;
	}
	return out;
}

static void sni_read_props(struct sni_item *it) {
	free(it->id); free(it->title); free(it->icon_name);
	it->id = sni_prop_str(it, "Id");
	it->title = sni_prop_str(it, "Title");
	it->icon_name = sni_prop_str(it, "IconName");
	sd_bus_error err = SD_BUS_ERROR_NULL;
	if (sd_bus_get_property_trivial(bus, it->service, it->path, SNI_IFACE, "Status", &err, 's',
					&it->status) < 0) {
		sd_bus_error_free(&err);
	}
	sni_refresh_icon(it);
}

static int sni_add(const char *service) {
	if (nsni >= MAX_SNI) return -1;
	for (int i = 0; i < nsni; i++)
		if (!strcmp(snis[i].service, service)) return 0;

	struct sni_item *it = &snis[nsni];
	memset(it, 0, sizeof *it);
	it->service = strdup(service);
	it->path = strdup("/StatusNotifierItem");
	/* some clients pass an object path instead of a bus name */
	if (service[0] == '/') {
		free(it->service);
		it->service = NULL;
		free(it->path);
		it->path = strdup(service);
	}
	sni_read_props(it);
	nsni++;
	sni_dirty = 1;
	return 0;
}

static void sni_remove(const char *service) {
	for (int i = 0; i < nsni; i++) {
		if (!strcmp(snis[i].service, service)) {
			free(snis[i].service); free(snis[i].path); free(snis[i].id);
			free(snis[i].title); free(snis[i].icon_name); free(snis[i].pix);
			memmove(&snis[i], &snis[i + 1], sizeof(struct sni_item) * (size_t)(nsni - i - 1));
			nsni--;
			sni_dirty = 1;
			return;
		}
	}
}

static void sni_emit(sd_bus *b, const char *member, const char *arg) {
	sd_bus_message *m = NULL;
	if (sd_bus_message_new_signal(b, &m, SNI_WATCHER_PATH, SNI_WATCHER, member) < 0) return;
	sd_bus_message_append(m, "s", arg ? arg : "");
	sd_bus_send(b, m, NULL);
	sd_bus_message_unref(m);
}

static int watcher_register_item(sd_bus_message *m, void *ud, sd_bus_error *err) {
	(void)ud; (void)err;
	const char *svc = NULL;
	if (sd_bus_message_read(m, "s", &svc) < 0 || !svc) return -EINVAL;
	sni_add(svc);
	sni_emit(sd_bus_message_get_bus(m), "StatusNotifierItemRegistered", svc);
	return sd_bus_reply_method_return(m, NULL);
}

static int watcher_register_host(sd_bus_message *m, void *ud, sd_bus_error *err) {
	(void)ud; (void)err;
	return sd_bus_reply_method_return(m, NULL);
}

static int watcher_get_prop(sd_bus *b, const char *path, const char *iface,
			    const char *prop, sd_bus_message *reply, void *ud, sd_bus_error *err) {
	(void)b; (void)ud;
	if (strcmp(path, SNI_WATCHER_PATH) || strcmp(iface, SNI_WATCHER)) return -EINVAL;
	if (!strcmp(prop, "IsStatusNotifierHostRegistered"))
		return sd_bus_message_append(reply, "b", 1);
	if (!strcmp(prop, "ProtocolVersion"))
		return sd_bus_message_append(reply, "i", 0);
	if (!strcmp(prop, "RegisteredStatusNotifierItems")) {
		int r = sd_bus_message_open_container(reply, 'a', "s");
		if (r < 0) return r;
		for (int i = 0; i < nsni; i++)
			if (snis[i].service) sd_bus_message_append(reply, "s", snis[i].service);
		return sd_bus_message_close_container(reply);
	}
	return sd_bus_error_setf(err, SD_BUS_ERROR_UNKNOWN_PROPERTY, "unknown %s", prop);
}

static const sd_bus_vtable watcher_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("RegisterStatusNotifierItem", "s", "", watcher_register_item,
		      SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("RegisterStatusNotifierHost", "s", "", watcher_register_host,
		      SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_PROPERTY("RegisteredStatusNotifierItems", "as", watcher_get_prop, 0,
			SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("IsStatusNotifierHostRegistered", "b", watcher_get_prop, 0, 0),
	SD_BUS_PROPERTY("ProtocolVersion", "i", watcher_get_prop, 0, 0),
	SD_BUS_SIGNAL("StatusNotifierItemRegistered", "s", 0),
	SD_BUS_SIGNAL("StatusNotifierItemUnregistered", "s", 0),
	SD_BUS_VTABLE_END
};

/* item signals (NewIcon / NewTitle / NewStatus) */
static int sni_filter(sd_bus_message *m, void *ud, sd_bus_error *err) {
	(void)ud; (void)err;
	const char *iface = sd_bus_message_get_interface(m);
	const char *member = sd_bus_message_get_member(m);
	const char *sender = sd_bus_message_get_sender(m);
	if (!iface || !member || !sender) return 0;
	if (strcmp(iface, SNI_IFACE)) return 0;
	for (int i = 0; i < nsni; i++) {
		if (!snis[i].service || strcmp(snis[i].service, sender)) continue;
		if (!strcmp(member, "NewIcon") || !strcmp(member, "NewIconThemePath")) {
			sni_refresh_icon(&snis[i]);
			sni_dirty = 1;
		} else if (!strcmp(member, "NewTitle")) {
			free(snis[i].title);
			snis[i].title = sni_prop_str(&snis[i], "Title");
			sni_dirty = 1;
		} else if (!strcmp(member, "NewStatus")) {
			sni_dirty = 1;
		}
		return 0;
	}
	return 0;
}

static int sni_bus_init(void) {
	if (sd_bus_default_user(&bus) < 0) return -1;
	sd_bus_set_exit_on_disconnect(bus, 0);

	if (sd_bus_add_object_vtable(bus, NULL, SNI_WATCHER_PATH, SNI_WATCHER, watcher_vtable, NULL) < 0)
		return -1;

	int r = sd_bus_request_name(bus, SNI_WATCHER, 0);
	if (r < 0) fprintf(stderr, "mango-bar: cannot own %s: %s\n", SNI_WATCHER, strerror(-r));

	sd_bus_add_filter(bus, NULL, sni_filter, NULL);
	sd_bus_match_signal(bus, NULL, NULL, NULL, SNI_IFACE, "NewIcon", NULL, NULL);
	sd_bus_match_signal(bus, NULL, NULL, NULL, SNI_IFACE, "NewTitle", NULL, NULL);
	sd_bus_match_signal(bus, NULL, NULL, NULL, SNI_IFACE, "NewStatus", NULL, NULL);

	sd_bus_flush(bus);
	bus_fd = sd_bus_get_fd(bus);
	return 0;
}

static void sni_activate(int idx) {
	if (idx < 0 || idx >= nsni) return;
	sd_bus_call_method_async(bus, NULL, snis[idx].service, snis[idx].path, SNI_IFACE,
				 "Activate", NULL, NULL, "ii", 0, 0);
}

static void sni_context(int idx) {
	if (idx < 0 || idx >= nsni) return;
	sd_bus_call_method_async(bus, NULL, snis[idx].service, snis[idx].path, SNI_IFACE,
				 "ContextMenu", NULL, NULL, "ii", 0, 0);
}


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

static long long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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

static int json_int(const char *p, const char *key) {
	const char *k = strstr(p, key);
	return k ? atoi(k + strlen(key)) : 0;
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
		c.tag = json_int(p, "\"tags\":[");
		c.x = json_int(p, "\"x\":");
		c.y = json_int(p, "\"y\":");
		c.w = json_int(p, "\"width\":");
		c.h = json_int(p, "\"height\":");

		int parked = 0;
		for (int t = 0; t < ntray; t++)
			if (tray[t].id == c.id) parked = 1;
		if (c.visible && !parked) got[n++] = c;
		p += 6;
	}

	int changed = (n != nclients);
	for (int i = 0; !changed && i < n; i++)
		changed = got[i].id != clients[i].id || got[i].focused != clients[i].focused ||
			  strcmp(got[i].title, clients[i].title) != 0;
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

/* ------------------------------------------------------------ tray actions */

static void tray_hide(int idx) {
	if (idx < 0 || idx >= nclients || ntray >= MAX_TRAY) return;
	struct trayent *t = &tray[ntray];
	memset(t, 0, sizeof *t);
	t->id = clients[idx].id;
	t->tag = clients[idx].tag ? clients[idx].tag : 1;
	t->x = clients[idx].x; t->y = clients[idx].y;
	t->w = clients[idx].w; t->h = clients[idx].h;
	snprintf(t->appid, sizeof t->appid, "%s", clients[idx].appid);
	snprintf(t->title, sizeof t->title, "%s", clients[idx].title);
	ntray++;

	char cmd[256];
	snprintf(cmd, sizeof cmd,
		 "/usr/local/bin/mango-dispatch dispatch tag,client,%d,9 >/dev/null 2>&1",
		 t->id);
	spawn(cmd);
	dirty = 1;
}

static void tray_restore(int idx) {
	if (idx < 0 || idx >= ntray) return;
	struct trayent *t = &tray[idx];
	char cmd[512];
	snprintf(cmd, sizeof cmd,
		 "/usr/local/bin/mango-dispatch dispatch tag,client,%d,%d >/dev/null 2>&1; "
		 "/usr/local/bin/mango-dispatch dispatch view,%d >/dev/null 2>&1; "
		 "/usr/local/bin/mango-dispatch dispatch focusid client,%d >/dev/null 2>&1",
		 t->id, t->tag, t->tag, t->id);
	spawn(cmd);
	memmove(&tray[idx], &tray[idx + 1], sizeof(struct trayent) * (size_t)(ntray - idx - 1));
	ntray--;
	dirty = 1;
}

/* long press on a tray icon: restore / launch entries / close */
static void tray_menu(int idx) {
	if (idx < 0 || idx >= ntray) return;
	struct trayent *t = &tray[idx];
	char cmd[1024];
	snprintf(cmd, sizeof cmd,
		 "printf '重新显示\\n关闭应用\\n' | rofi -dmenu -p '%s' -theme-str 'window{width:320px;}' "
		 "| xargs -r -I{} sh -c 'case \"{}\" in 重新显示) /usr/local/bin/mango-bar-restore %d;; "
		 "关闭应用) /usr/local/bin/mango-dispatch dispatch killclient client,%d;; esac'",
		 t->appid[0] ? t->appid : "app", t->id, t->id);
	spawn(cmd);
}

static void tray_restore_helper(int id) {
	for (int i = 0; i < ntray; i++)
		if (tray[i].id == id) { tray_restore(i); return; }
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

/* chevron "<>" pointing down: expands / collapses the tray icons */
static void draw_chevron(cairo_t *cr, double cx, double cy, int expanded) {
	set_hex(cr, C_ICON, 1);
	cairo_set_line_width(cr, 1.6);
	double h = expanded ? -2.5 : 2.5;
	cairo_move_to(cr, cx - 6, cy - h);
	cairo_line_to(cr, cx, cy + h);
	cairo_line_to(cr, cx + 6, cy - h);
	cairo_stroke(cr);
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

	/* ---- window pills ---- */
	n_pill_boxes = 0;
	double x = EDGE + 6;
	double right_limit = W - EDGE - 6 - POWER_D - 8 - STATUS_W;
	if (tray_open) right_limit -= (ARROW_W + 8 + ntray * (TRAY_ITEM + TRAY_GAP));
	else right_limit -= (ARROW_W + 8);

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
		if (x + w > right_limit) break;

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

	if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
		cairo_destroy(cr);
		cr = cairo_create(cs);
		cairo_scale(cr, scale, scale);
	}

	/* ---- clock ---- */
	text_at(cr, clock_str, 15, C_CLOCK, W / 2.0, cy + 5.5, 1);

	/* ---- right cluster: power, status pill, tray icons, chevron ---- */
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

	double sx = px - 8 - STATUS_W;
	status_box.x = sx; status_box.y = cy - TRAY_ITEM / 2.0 - 1;
	status_box.w = STATUS_W; status_box.h = TRAY_ITEM + 2;
	set_hex(cr, C_TRAY_BG, 1);
	rounded_rect(cr, status_box.x, status_box.y, STATUS_W, status_box.h, status_box.h / 2);
	cairo_fill_preserve(cr);
	set_hex(cr, C_TRAY_BD, 1);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	double iw = STATUS_W / 3.0;
	for (int i = 0; i < 3; i++) {
		status_icon[i].x = sx + i * iw;
		status_icon[i].y = status_box.y;
		status_icon[i].w = iw;
		status_icon[i].h = status_box.h;
	}
	draw_speaker(cr, status_icon[0].x + iw / 2, cy, 1, vol_mute);
	draw_wifi(cr, status_icon[1].x + iw / 2, cy, wifi_on);
	draw_bt(cr, status_icon[2].x + iw / 2, cy, bt_on);

	/* chevron button, immediately left of the tray icons */
	n_tray_boxes = 0;
	double ax = sx - 8 - ARROW_W;
	arrow_box.x = ax; arrow_box.y = cy - TRAY_ITEM / 2.0;
	arrow_box.w = ARROW_W; arrow_box.h = TRAY_ITEM;
	draw_chevron(cr, ax + ARROW_W / 2, cy, tray_open);

	/* system tray: StatusNotifierItem icons, growing to the left */
	if (tray_open) {
		double tx = ax - 8;
		for (int i = 0; i < nsni && i < MAX_SNI; i++) {
			tx -= TRAY_ITEM;
			tray_box[i].x = tx; tray_box[i].y = cy - TRAY_ITEM / 2.0;
			tray_box[i].w = TRAY_ITEM; tray_box[i].h = TRAY_ITEM;
			n_tray_boxes++;

			set_hex(cr, C_PILL_IDLE, 1);
			rounded_rect(cr, tx, tray_box[i].y, TRAY_ITEM, TRAY_ITEM, 9);
			cairo_fill_preserve(cr);
			set_hex(cr, C_TRAY_BD, 1);
			cairo_set_line_width(cr, 1);
			cairo_stroke(cr);

			if (snis[i].pix && snis[i].pix_w > 0) {
				cairo_surface_t *is = cairo_image_surface_create_for_data(
					(unsigned char *)snis[i].pix, CAIRO_FORMAT_ARGB32,
					snis[i].pix_w, snis[i].pix_h, snis[i].pix_w * 4);
				double sc = fmin((TRAY_ITEM - 10.0) / snis[i].pix_w,
					(TRAY_ITEM - 10.0) / snis[i].pix_h);
				double dw = snis[i].pix_w * sc, dh = snis[i].pix_h * sc;
				cairo_save(cr);
				cairo_translate(cr, tx + (TRAY_ITEM - dw) / 2, tray_box[i].y + (TRAY_ITEM - dh) / 2);
				cairo_scale(cr, sc, sc);
			cairo_set_source_surface(cr, is, 0, 0);
				cairo_paint(cr);
				cairo_restore(cr);
				cairo_surface_destroy(is);
			} else {
				char letter[8] = {0};
				const char *nm = snis[i].id ? snis[i].id : (snis[i].title ? snis[i].title : "?");
				letter[0] = nm[0] ? nm[0] : '?';
				text_at(cr, letter, 15, C_ICON, tx + TRAY_ITEM / 2, cy + 5, 1);
			}
			tx -= TRAY_GAP;
		}
	}

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

static int press_idx = -1;       /* which pill/tray entry is being pressed */
static int press_kind = 0;       /* 1 = pill, 2 = tray, 3 = arrow, 4 = status, 5 = power */
static long long press_at = 0;

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

	double x = pointer_x, y = pointer_y;

	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		press_idx = -1; press_kind = 0; press_at = now_ms();
		for (int i = 0; i < n_pill_boxes; i++)
			if (inside(&pill_box[i], x, y)) { press_kind = 1; press_idx = i; return; }
		for (int i = 0; i < n_tray_boxes; i++)
			if (inside(&tray_box[i], x, y)) { press_kind = 2; press_idx = i; return; }
		if (inside(&arrow_box, x, y)) { press_kind = 3; return; }
		for (int i = 0; i < 3; i++)
			if (inside(&status_icon[i], x, y)) { press_kind = 4; press_idx = i; return; }
		if (inside(&power_box, x, y)) { press_kind = 5; return; }
		return;
	}

	/* release: decide short vs long press */
	long long held = now_ms() - press_at;
	int is_long = held >= LONG_PRESS_MS;

	switch (press_kind) {
	case 1:		/* window pill */
		if (press_idx < 0 || press_idx >= nclients) break;
		if (is_long) {
			tray_hide(press_idx);
		} else {
			char cmd[256];
			snprintf(cmd, sizeof cmd,
				 "/usr/local/bin/mango-dispatch dispatch focusid client,%d >/dev/null 2>&1",
				 clients[press_idx].id);
			spawn(cmd);
		}
		break;
	case 2:		/* system tray icon (StatusNotifierItem) */
		if (press_idx < 0 || press_idx >= nsni) break;
		if (is_long) sni_context(press_idx);
		else sni_activate(press_idx);
		break;
	case 3:		/* chevron: collapse / expand */
		tray_open = !tray_open;
		dirty = 1;
		repaint();
		break;
	case 4:		/* status icons */
		spawn(press_idx == 0 ? cmd_audio : press_idx == 1 ? cmd_network : cmd_bluetooth);
		break;
	case 5: {	/* power */
		time_t now = time(NULL);
		if (power_armed && now - power_armed_at <= 3) {
			power_armed = 0;
			dirty = 1;
			repaint();
			spawn("systemctl poweroff");
			break;
		}
		power_armed = 1;
		power_armed_at = now;
		dirty = 1;
		repaint();
		break;
	}
	default:
		break;
	}
	press_kind = 0;
	press_idx = -1;
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

int main(int argc, char **argv) {
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	signal(SIGCHLD, SIG_IGN);

	/* helper mode used by the tray menu: mango-bar --restore <id> */
	if (argc == 3 && !strcmp(argv[1], "--restore")) {
		tray_restore_helper(atoi(argv[2]));
		return 0;
	}

	load_actions();
	if (sni_bus_init() < 0)
		fprintf(stderr, "mango-bar: StatusNotifierWatcher unavailable\n");

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

		struct pollfd pfds[2] = { { .fd = wfd, .events = POLLIN },
					 { .fd = bus_fd, .events = POLLIN } };
		int nfds = bus_fd >= 0 ? 2 : 1;
		if (poll(pfds, (nfds_t)nfds, 250) > 0) {
			if (pfds[0].revents & POLLIN) {
				if (wl_display_dispatch(display) < 0) break;
			}
			if (nfds == 2 && (pfds[1].revents & POLLIN)) {
				while (sd_bus_process(bus, NULL) > 0) {}
				sd_bus_flush(bus);
			}
		}

		time_t now = time(NULL);
		refresh_clock();
		if (now != last_clients) { refresh_clients(); last_clients = now; }
		if (now - last_status >= 2) { refresh_status(); last_status = now; }
		if (power_armed && now - power_armed_at > 3) { power_armed = 0; dirty = 1; }
		if (sni_dirty) { sni_dirty = 0; dirty = 1; }
		if (dirty) repaint();
	}

	return 0;
}
