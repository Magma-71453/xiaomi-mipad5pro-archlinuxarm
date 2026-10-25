/* schedplace — logging and run-time counters. */
#ifndef SP_LOG_H
#define SP_LOG_H

#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>

#include "sp_util.h"

enum sp_log_level {
    SP_LOG_ERROR = 0,
    SP_LOG_WARN = 1,
    SP_LOG_INFO = 2,
    SP_LOG_DEBUG = 3
};

struct sp_stats {
    unsigned long scans;
    unsigned long processes_seen;
    unsigned long threads_seen;
    unsigned long candidates;
    unsigned long decisions;
    unsigned long affinity_applied;
    unsigned long cgroup_applied;
    unsigned long nice_applied;
    unsigned long skipped_unchanged;
    unsigned long errors;
    unsigned long refused_writes;
    unsigned long restored;
    unsigned long rule_hits[SP_MAX_RULES];
};

void sp_log_init(const char *ident, int level);
void sp_log_set_level(int level);
int sp_log_level(void);
const char *sp_level_name(int level);
#if defined(__GNUC__)
void sp_logf(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#else
void sp_logf(int level, const char *fmt, ...);
#endif

/* One structured line per placement decision (see docs/perf.md). */
struct sp_decision_log {
    pid_t pid;
    pid_t tid;
    const char *pcomm;
    const char *tcomm;
    uid_t uid;
    const char *exe;
    const char *rule;
    const char *profile;
    const char *cluster;
    const char *old_mask;
    const char *new_mask;
    const char *cpuset;
    const char *result;
    const char *reason;
    bool busy_known;
    double busy_pct;
    bool has_nice;
    int nice;
    int err;
};

void sp_log_decision(const struct sp_decision_log *d);

#endif
