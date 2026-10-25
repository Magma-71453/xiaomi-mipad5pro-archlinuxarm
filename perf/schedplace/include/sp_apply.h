/* schedplace — applying placement decisions (cpuset + affinity + nice). */
#ifndef SP_APPLY_H
#define SP_APPLY_H

#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>

#include "sp_conf.h"
#include "sp_proc.h"
#include "sp_topology.h"

enum sp_result {
    SP_RESULT_APPLIED = 0,
    SP_RESULT_UNCHANGED,
    SP_RESULT_DRY_RUN,
    SP_RESULT_KEEP,
    SP_RESULT_ERROR
};

const char *sp_result_name(enum sp_result result);

struct sp_decision {
    const struct sp_thread_info *thread;
    const struct sp_rule *rule;
    int rule_index;
    enum sp_cluster cluster;
    cpu_set_t mask;
    bool has_mask;
    enum sp_result result;
    int err;
};

int sp_apply_thread(const struct sp_config *cfg, const struct sp_topology *topo,
                    const struct sp_thread_info *thread, int rule_index,
                    unsigned long long scan_id, struct sp_stats *stats,
                    struct sp_decision *out, char *err, size_t errlen);

void sp_state_report(FILE *out, const struct sp_config *cfg, const struct sp_topology *topo,
                     const struct sp_stats *stats);
void sp_apply_restore_all(const struct sp_config *cfg, const struct sp_stats *stats,
                          struct sp_stats *out_stats);
void sp_state_prune(unsigned long long scan_id);
size_t sp_state_count(void);
void sp_apply_reset_for_tests(void);

int sp_cpuset_group_path(const struct sp_config *cfg, const char *group, char *buf,
                         size_t size);
int sp_cpuset_ensure(const struct sp_config *cfg, const char *group, const cpu_set_t *mask,
                     struct sp_stats *stats);
int sp_cpuset_join(const struct sp_config *cfg, const char *group, pid_t tid,
                   struct sp_stats *stats);
int sp_legacy_apply_governor(const struct sp_config *cfg, struct sp_stats *stats);

#endif
