/* schedplace — rule model: match keys, actions, ordered evaluation. */
#ifndef SP_RULES_H
#define SP_RULES_H

#include <regex.h>
#include <stdbool.h>
#include <sys/types.h>

#include "sp_util.h"

enum sp_cluster {
    SP_CLUSTER_LITTLE = 0, /* slowest capacity level */
    SP_CLUSTER_BIG = 1,    /* middle level (== prime when only two levels exist) */
    SP_CLUSTER_PRIME = 2,  /* fastest level */
    SP_CLUSTER_PERF = 3,   /* big | prime: the whole "performance" side */
    SP_CLUSTER_KEEP = 4,   /* explicit exemption: matched, deliberately untouched */
    SP_CLUSTER_AUTO = 5    /* resolve from the rule profile */
};

enum sp_profile {
    SP_PROFILE_DEFAULT = 0,
    SP_PROFILE_LATENCY = 1,
    SP_PROFILE_BACKGROUND = 2
};

struct sp_pattern {
    bool set;
    bool icase;
    regex_t re;
    char src[128];
};

struct sp_rule {
    char name[SP_NAME_LEN];
    enum sp_profile profile;
    bool enabled;
    bool legacy;
    struct sp_pattern process_comm; /* /proc/<pid>/comm */
    struct sp_pattern thread_comm;  /* /proc/<pid>/task/<tid>/comm */
    struct sp_pattern exe;          /* /proc/<pid>/exe */
    struct sp_pattern cgroup;       /* cgroup v2 path */
    bool has_uid;
    uid_t uid;
    bool uid_exclude;
    bool has_busy_lt;
    double busy_lt;
    bool has_busy_gt;
    double busy_gt;
    bool cluster_set;
    enum sp_cluster cluster;
    bool set_affinity;
    char cpuset[SP_NAME_LEN];
    bool has_nice;
    int nice;
    char reason[192];
};

struct sp_ruleset {
    struct sp_rule rules[SP_MAX_RULES];
    size_t count;
    bool needs_busy;
    bool needs_cgroup;
    bool needs_exe;
};

struct sp_match_input {
    const char *pcomm;
    const char *tcomm;
    const char *exe;
    const char *cgroup;
    bool has_uid;
    uid_t uid;
    double busy_pct;
    bool busy_known;
};

const char *sp_cluster_name(enum sp_cluster cluster);
bool sp_cluster_from_name(const char *name, enum sp_cluster *out);
const char *sp_profile_name(enum sp_profile profile);
bool sp_profile_from_name(const char *name, enum sp_profile *out);

int sp_pattern_compile(struct sp_pattern *pattern, const char *src, bool icase,
                       char *err, size_t errlen);
bool sp_pattern_match(const struct sp_pattern *pattern, const char *text);

int sp_rule_finalize(struct sp_rule *rule, char *err, size_t errlen);
bool sp_rule_has_process_filter(const struct sp_rule *rule);
bool sp_rule_process_possible(const struct sp_rule *rule, const struct sp_match_input *in);
int sp_rules_match(const struct sp_ruleset *set, const struct sp_match_input *in);
bool sp_rules_process_candidate(const struct sp_ruleset *set, const struct sp_match_input *in);
void sp_ruleset_refresh_flags(struct sp_ruleset *set);
void sp_ruleset_free(struct sp_ruleset *set);

#endif
