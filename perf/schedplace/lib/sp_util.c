/* schedplace — small string, cpu-mask and safety helpers. */
#include "sp_util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sp_log.h"

int sp_read_file(const char *path, char *buf, size_t size) {
    if (!path || !buf || size < 2) {
        errno = EINVAL;
        return -1;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, size - 1);
    int saved = errno;
    close(fd);
    errno = saved;
    if (n < 0)
        return -1;
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';
    return (int)n;
}

int sp_read_link(const char *path, char *buf, size_t size) {
    if (!path || !buf || size < 2) {
        errno = EINVAL;
        return -1;
    }
    ssize_t n = readlink(path, buf, size - 1);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return (int)n;
}

bool sp_read_ulong_file(const char *path, unsigned long *out) {
    char text[64];
    if (sp_read_file(path, text, sizeof text) < 0)
        return false;
    return sp_parse_ulong(text, out);
}

bool sp_parse_ulong(const char *s, unsigned long *out) {
    if (!s) return false;
    while (*s && isspace((unsigned char)*s)) ++s;
    if (!*s) return false;
    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(s, &end, 10);
    if (errno || end == s) return false;
    while (*end && isspace((unsigned char)*end)) ++end;
    if (*end) return false;
    if (out) *out = value;
    return true;
}

bool sp_parse_long(const char *s, long *out) {
    if (!s) return false;
    while (*s && isspace((unsigned char)*s)) ++s;
    if (!*s) return false;
    errno = 0;
    char *end = NULL;
    long value = strtol(s, &end, 10);
    if (errno || end == s) return false;
    while (*end && isspace((unsigned char)*end)) ++end;
    if (*end) return false;
    if (out) *out = value;
    return true;
}

double sp_parse_double(const char *s, bool *ok) {
    bool ignored = false;
    if (!ok) ok = &ignored;
    *ok = false;
    if (!s) return 0.0;
    while (*s && isspace((unsigned char)*s)) ++s;
    if (!*s) return 0.0;
    errno = 0;
    char *end = NULL;
    double value = strtod(s, &end);
    if (errno || end == s) return 0.0;
    while (*end && isspace((unsigned char)*end)) ++end;
    if (*end || !isfinite(value)) return 0.0;
    *ok = true;
    return value;
}

bool sp_parse_bool(const char *s, bool *out) {
    if (!s || !out) return false;
    while (*s && isspace((unsigned char)*s)) ++s;
    char lower[16];
    size_t n = 0;
    while (s[n] && !isspace((unsigned char)s[n]) && n + 1 < sizeof lower) {
        lower[n] = (char)tolower((unsigned char)s[n]);
        ++n;
    }
    lower[n] = '\0';
    if (sp_streq(lower, "1") || sp_streq(lower, "true") || sp_streq(lower, "yes") ||
        sp_streq(lower, "on")) {
        *out = true;
        return true;
    }
    if (sp_streq(lower, "0") || sp_streq(lower, "false") || sp_streq(lower, "no") ||
        sp_streq(lower, "off")) {
        *out = false;
        return true;
    }
    return false;
}

bool sp_parse_cpulist(const char *text, cpu_set_t *set) {
    if (!text || !set) return false;
    CPU_ZERO(set);
    const char *p = text;
    bool any = false;
    for (;;) {
        while (isspace((unsigned char)*p)) ++p;
        if (!*p) break;
        if (any) {
            if (*p != ',') return false;
            ++p;
            while (isspace((unsigned char)*p)) ++p;
            if (!isdigit((unsigned char)*p)) return false;
        }
        if (!isdigit((unsigned char)*p)) return false;
        errno = 0;
        char *end = NULL;
        long first = strtol(p, &end, 10);
        if (errno || end == p || first < 0 || first >= CPU_SETSIZE) return false;
        long last = first;
        p = end;
        if (*p == '-') {
            ++p;
            errno = 0;
            long value = strtol(p, &end, 10);
            if (errno || end == p || value < first || value >= CPU_SETSIZE) return false;
            last = value;
            p = end;
        }
        for (long cpu = first; cpu <= last; ++cpu) CPU_SET((int)cpu, set);
        any = true;
    }
    if (!any) CPU_ZERO(set);
    return any;
}

bool sp_format_cpulist(const cpu_set_t *set, char *buf, size_t size) {
    char tmp[SP_TEXT_MAX];
    size_t used = 0;
    bool first = true;
    if (!set || !buf || size < 2) return false;
    buf[0] = '\0';
    for (int cpu = 0; cpu < CPU_SETSIZE;) {
        if (!CPU_ISSET(cpu, set)) {
            ++cpu;
            continue;
        }
        int start = cpu;
        while (cpu + 1 < CPU_SETSIZE && CPU_ISSET(cpu + 1, set)) ++cpu;
        int end = cpu;
        char piece[32];
        int written = (start == end)
                          ? snprintf(piece, sizeof piece, "%d", start)
                          : snprintf(piece, sizeof piece, "%d-%d", start, end);
        if (written < 0) return false;
        if (used + (size_t)written + 2 >= sizeof tmp) return false;
        if (!first) tmp[used++] = ',';
        memcpy(tmp + used, piece, (size_t)written);
        used += (size_t)written;
        tmp[used] = '\0';
        first = false;
        ++cpu;
    }
    if (first || used + 1 > size) return false;
    memcpy(buf, tmp, used + 1);
    return true;
}

void sp_cpuset_and(cpu_set_t *dst, const cpu_set_t *a, const cpu_set_t *b) {
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, a) && CPU_ISSET(cpu, b))
            CPU_SET(cpu, dst);
        else
            CPU_CLR(cpu, dst);
    }
}

void sp_cpuset_or(cpu_set_t *dst, const cpu_set_t *a, const cpu_set_t *b) {
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, a) || CPU_ISSET(cpu, b))
            CPU_SET(cpu, dst);
        else
            CPU_CLR(cpu, dst);
    }
}

bool sp_cpuset_equal(const cpu_set_t *a, const cpu_set_t *b) {
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        if (CPU_ISSET(cpu, a) != CPU_ISSET(cpu, b)) return false;
    return true;
}

bool sp_cpuset_subset(const cpu_set_t *sub, const cpu_set_t *sup) {
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        if (CPU_ISSET(cpu, sub) && !CPU_ISSET(cpu, sup)) return false;
    return true;
}

bool sp_cpuset_any(const cpu_set_t *set) {
    return sp_cpuset_count(set) > 0;
}

int sp_cpuset_count(const cpu_set_t *set) {
    int count = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        if (CPU_ISSET(cpu, set)) ++count;
    return count;
}

char *sp_trim(char *s) {
    if (!s) return s;
    char *start = s;
    while (*start && isspace((unsigned char)*start)) ++start;
    if (start != s) memmove(s, start, strlen(start) + 1);
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
    return s;
}

size_t sp_strlcpy(char *dst, const char *src, size_t size) {
    if (!dst || !size) return 0;
    size_t len = (src && *src) ? strlen(src) : 0;
    size_t copy = len < size - 1 ? len : size - 1;
    if (copy) memcpy(dst, src, copy);
    dst[copy] = '\0';
    return copy;
}

bool sp_streq(const char *a, const char *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return strcmp(a, b) == 0;
}

bool sp_ends_with(const char *s, const char *tail) {
    if (!s || !tail) return false;
    size_t ls = strlen(s), lt = strlen(tail);
    return ls >= lt && memcmp(s + ls - lt, tail, lt) == 0;
}

bool sp_has_space(const char *s) {
    for (; s && *s; ++s)
        if (isspace((unsigned char)*s)) return true;
    return false;
}

int sp_path_join(char *dst, size_t size, const char *a, const char *b) {
    if (!dst || !size || !a) {
        errno = EINVAL;
        return -1;
    }
    const char *tail = (b && *b) ? b : NULL;
    size_t la = strlen(a);
    size_t lb = tail ? strlen(tail) : 0;
    bool slash = la > 0 && a[la - 1] == '/';
    if (tail && !slash) {
        if (la + 1 + lb + 1 > size) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(dst, a, la);
        dst[la] = '/';
        memcpy(dst + la + 1, tail, lb + 1);
        return 0;
    }
    if (la + lb + 1 > size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(dst, a, la);
    if (tail) memcpy(dst + la, tail, lb + 1);
    else dst[la] = '\0';
    return 0;
}

int sp_path_basename(const char *path, char *out, size_t size) {
    if (!path || !out || size < 2) {
        errno = EINVAL;
        return -1;
    }
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    if (!*name) {
        errno = EINVAL;
        return -1;
    }
    sp_strlcpy(out, name, size);
    return 0;
}

int sp_mkdir_p(const char *path, unsigned int mode) {
    if (!path || !*path) {
        errno = EINVAL;
        return -1;
    }
    char tmp[SP_PATH_MAX];
    if (sp_strlcpy(tmp, path, sizeof tmp) >= sizeof tmp - 1) {
        errno = ENAMETOOLONG;
        return -1;
    }
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') tmp[--len] = '\0';
    for (char *p = tmp + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, (mode_t)mode) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(tmp, (mode_t)mode) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* Files this daemon is allowed to write. Everything else is denied. */
static const char *const k_write_allowlist[] = {
    "cpuset.cpus",
    "cpuset.mems",
    "cgroup.threads",
};

static const struct {
    const char *name;
    enum sp_forbidden kind;
} k_write_denylist[] = {
    {"scaling_governor", SP_FORBID_GOVERNOR},
    {"scaling_max_freq", SP_FORBID_FREQUENCY},
    {"scaling_min_freq", SP_FORBID_FREQUENCY},
    {"scaling_setspeed", SP_FORBID_FREQUENCY},
    {"scaling_boost_freq", SP_FORBID_FREQUENCY},
    {"scaling_cur_freq", SP_FORBID_FREQUENCY},
    {"online", SP_FORBID_CPU_ONLINE},
    {"sched_rt_runtime_us", SP_FORBID_SCHED_TUNABLE},
    {"sched_util_clamp_min", SP_FORBID_SCHED_TUNABLE},
    {"sched_util_clamp_max", SP_FORBID_SCHED_TUNABLE},
    {"sched_autogroup_enabled", SP_FORBID_SCHED_TUNABLE},
    {"sched_energy_aware", SP_FORBID_SCHED_TUNABLE},
    {"sched_boost", SP_FORBID_VENDOR_BOOST},
    {"core_ctl", SP_FORBID_VENDOR_BOOST},
    {"msm_performance", SP_FORBID_VENDOR_BOOST},
    {"task_boost", SP_FORBID_VENDOR_BOOST},
};

enum sp_forbidden sp_path_classify(const char *path) {
    char base[SP_NAME_LEN];
    if (sp_path_basename(path, base, sizeof base) != 0) return SP_FORBID_NOT_ALLOWLISTED;
    for (size_t i = 0; i < sizeof k_write_denylist / sizeof k_write_denylist[0]; ++i)
        if (sp_streq(base, k_write_denylist[i].name)) return k_write_denylist[i].kind;
    for (size_t i = 0; i < sizeof k_write_allowlist / sizeof k_write_allowlist[0]; ++i)
        if (sp_streq(base, k_write_allowlist[i])) return SP_FORBID_ALLOWED;
    return SP_FORBID_NOT_ALLOWLISTED;
}

const char *sp_forbidden_reason(enum sp_forbidden kind) {
    switch (kind) {
    case SP_FORBID_ALLOWED:
        return "allowed";
    case SP_FORBID_GOVERNOR:
        return "governor writes belong to the platform/schedutil policy, not to schedplace";
    case SP_FORBID_FREQUENCY:
        return "frequency limit writes are outside schedplace's placement-only scope";
    case SP_FORBID_CPU_ONLINE:
        return "CPU hotplug is outside schedplace's placement-only scope";
    case SP_FORBID_SCHED_TUNABLE:
        return "scheduler tunables are owned by the kernel/EAS configuration";
    case SP_FORBID_VENDOR_BOOST:
        return "vendor boost/hotplug knobs are audited by perf-audit-noise.sh, never written here";
    case SP_FORBID_NOT_ALLOWLISTED:
        return "path is not one of the cgroup cpuset files schedplace may write";
    }
    return "unknown";
}

bool sp_path_is_forbidden(const char *path) {
    return sp_path_classify(path) != SP_FORBID_ALLOWED;
}

int sp_safe_write(const char *path, const char *text, bool allow_frequency_writes,
                  struct sp_stats *stats) {
    enum sp_forbidden kind = sp_path_classify(path);
    bool permitted = kind == SP_FORBID_ALLOWED;
    if (!permitted && allow_frequency_writes &&
        (kind == SP_FORBID_GOVERNOR || kind == SP_FORBID_FREQUENCY))
        permitted = true;
    if (!permitted) {
        if (stats) ++stats->refused_writes;
        errno = EPERM;
        sp_logf(SP_LOG_WARN, "refusing write to %s: %s", path ? path : "(null)",
                sp_forbidden_reason(kind));
        return -1;
    }
    /* No O_CREAT: a missing sysfs/cgroup node must fail instead of being created. */
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t len = text ? strlen(text) : 0;
    ssize_t written = len ? write(fd, text, len) : 0;
    int saved = errno;
    close(fd);
    errno = saved;
    if (len && written != (ssize_t)len) {
        if (written >= 0) errno = EIO;
        return -1;
    }
    return 0;
}
