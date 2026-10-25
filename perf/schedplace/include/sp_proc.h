/* schedplace — /proc scanner (process cache + per-thread sampling). */
#ifndef SP_PROC_H
#define SP_PROC_H

#include <sched.h>
#include <stdbool.h>
#include <sys/types.h>

#include "sp_log.h"
#include "sp_rules.h"
#include "sp_util.h"

struct sp_proc_info {
    pid_t pid;
    unsigned long long starttime;
    uid_t uid;
    char comm[SP_COMM_LEN];
    char exe[SP_PATH_MAX];
    char cgroup[SP_PATH_MAX];
};

struct sp_thread_info {
    pid_t pid;
    pid_t tid;
    unsigned long long starttime;
    const struct sp_proc_info *proc;
    char comm[SP_COMM_LEN];
    double busy_pct;
    bool busy_known;
    cpu_set_t affinity;
    bool affinity_valid;
};

struct sp_scanner;

typedef int (*sp_thread_cb)(void *ctx, const struct sp_proc_info *proc,
                            const struct sp_thread_info *thread);

int sp_scanner_init(struct sp_scanner **out);
void sp_scanner_free(struct sp_scanner *scanner);
long long sp_scanner_now_ms(void);
/* Returns the number of candidate threads delivered to cb, or -1 on error.
 * cb may be NULL, which only refreshes the busy% samples (warm-up pass). */
long sp_scanner_scan(struct sp_scanner *scanner, const struct sp_ruleset *rules,
                     bool need_busy, sp_thread_cb cb, void *ctx, struct sp_stats *stats,
                     char *err, size_t errlen);

#endif
