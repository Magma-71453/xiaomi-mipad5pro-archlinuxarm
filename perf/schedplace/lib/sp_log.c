/* schedplace — logging and structured decision lines. */
#include "sp_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *g_ident = "schedplace";
static int g_level = SP_LOG_INFO;

void sp_log_init(const char *ident, int level) {
    if (ident && *ident) g_ident = ident;
    g_level = level;
}

void sp_log_set_level(int level) {
    g_level = level;
}

int sp_log_level(void) {
    return g_level;
}

const char *sp_level_name(int level) {
    switch (level) {
    case SP_LOG_ERROR:
        return "error";
    case SP_LOG_WARN:
        return "warn";
    case SP_LOG_INFO:
        return "info";
    case SP_LOG_DEBUG:
        return "debug";
    default:
        return "log";
    }
}

void sp_logf(int level, const char *fmt, ...) {
    if (level > g_level) return;
    char message[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof message, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s: %s: %s\n", g_ident, sp_level_name(level), message);
    fflush(stderr);
}

static void append(char *buf, size_t size, size_t *off, const char *fmt, ...) {
    if (*off >= size) return;
    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(buf + *off, size - *off, fmt, ap);
    va_end(ap);
    if (written < 0) return;
    size_t add = (size_t)written;
    if (add >= size - *off) {
        *off = size - 1;
        return;
    }
    *off += add;
}

void sp_log_decision(const struct sp_decision_log *d) {
    if (!d || g_level < SP_LOG_INFO) return;
    char line[2048];
    char nice_field[24];
    char busy_field[48];
    char reason[256];
    size_t off = 0;

    if (d->has_nice) snprintf(nice_field, sizeof nice_field, "%+d", d->nice);
    else snprintf(nice_field, sizeof nice_field, "%s", "-");
    if (d->busy_known) snprintf(busy_field, sizeof busy_field, "%.1f%%", d->busy_pct);
    else snprintf(busy_field, sizeof busy_field, "%s", "-");
    snprintf(reason, sizeof reason, "%s", d->reason ? d->reason : "");
    for (char *p = reason; *p; ++p)
        if (*p == '"') *p = '\'';

    line[0] = '\0';
    append(line, sizeof line, &off,
           "%s: decision tid=%ld pid=%ld comm=%s pcomm=%s uid=%lu exe=%s rule=%s"
           " profile=%s cluster=%s old_mask=%s new_mask=%s cpuset=%s nice=%s busy=%s"
           " result=%s err=%d reason=\"%s\"",
           g_ident, (long)d->tid, (long)d->pid,
           d->tcomm && *d->tcomm ? d->tcomm : "-",
           d->pcomm && *d->pcomm ? d->pcomm : "-", (unsigned long)d->uid,
           d->exe && *d->exe ? d->exe : "-",
           d->rule && *d->rule ? d->rule : "-",
           d->profile && *d->profile ? d->profile : "-",
           d->cluster && *d->cluster ? d->cluster : "-",
           d->old_mask && *d->old_mask ? d->old_mask : "-",
           d->new_mask && *d->new_mask ? d->new_mask : "-",
           d->cpuset && *d->cpuset ? d->cpuset : "-", nice_field, busy_field,
           d->result ? d->result : "-", d->err, reason);
    fprintf(stderr, "%s\n", line);
    fflush(stderr);
}
