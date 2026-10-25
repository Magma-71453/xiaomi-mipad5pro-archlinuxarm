/* schedplace — rule model implementation. */
#include "sp_rules.h"

#include <stdio.h>
#include <string.h>

#include "sp_log.h"

const char *sp_cluster_name(enum sp_cluster cluster) {
    switch (cluster) {
    case SP_CLUSTER_LITTLE:
        return "little";
    case SP_CLUSTER_BIG:
        return "big";
    case SP_CLUSTER_PRIME:
        return "prime";
    case SP_CLUSTER_PERF:
        return "perf";
    case SP_CLUSTER_KEEP:
        return "keep";
    case SP_CLUSTER_AUTO:
        return "auto";
    }
    return "?";
}

bool sp_cluster_from_name(const char *name, enum sp_cluster *out) {
    static const struct {
        const char *name;
        enum sp_cluster cluster;
    } table[] = {
        {"little", SP_CLUSTER_LITTLE}, {"big", SP_CLUSTER_BIG},
        {"prime", SP_CLUSTER_PRIME},   {"perf", SP_CLUSTER_PERF},
        {"keep", SP_CLUSTER_KEEP},     {"auto", SP_CLUSTER_AUTO},
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; ++i) {
        if (sp_streq(name, table[i].name)) {
            if (out) *out = table[i].cluster;
            return true;
        }
    }
    return false;
}

const char *sp_profile_name(enum sp_profile profile) {
    switch (profile) {
    case SP_PROFILE_DEFAULT:
        return "default";
    case SP_PROFILE_LATENCY:
        return "latency";
    case SP_PROFILE_BACKGROUND:
        return "background";
    }
    return "?";
}

bool sp_profile_from_name(const char *name, enum sp_profile *out) {
    static const struct {
        const char *name;
        enum sp_profile profile;
    } table[] = {
        {"default", SP_PROFILE_DEFAULT},
        {"latency", SP_PROFILE_LATENCY},
        {"background", SP_PROFILE_BACKGROUND},
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; ++i) {
        if (sp_streq(name, table[i].name)) {
            if (out) *out = table[i].profile;
            return true;
        }
    }
    return false;
}

int sp_pattern_compile(struct sp_pattern *pattern, const char *src, bool icase,
                       char *err, size_t errlen) {
    if (!pattern || !src) return -1;
    char copy[128];
    sp_strlcpy(copy, src, sizeof copy); /* src may alias pattern->src */
    memset(pattern, 0, sizeof *pattern);
    sp_strlcpy(pattern->src, copy, sizeof pattern->src);
    pattern->icase = icase;
    int flags = REG_EXTENDED | REG_NOSUB;
    if (icase) flags |= REG_ICASE;
    int rc = regcomp(&pattern->re, pattern->src, flags);
    if (rc != 0) {
        char detail[128];
        regerror(rc, &pattern->re, detail, sizeof detail);
        if (err) snprintf(err, errlen, "invalid regex '%s': %s", pattern->src, detail);
        return -1;
    }
    pattern->set = true;
    return 0;
}

bool sp_pattern_match(const struct sp_pattern *pattern, const char *text) {
    if (!pattern || !pattern->set) return false;
    const char *subject = text ? text : "";
    return regexec(&pattern->re, subject, 0, NULL, 0) == 0;
}

int sp_rule_finalize(struct sp_rule *rule, char *err, size_t errlen) {
    if (!rule->cluster_set) {
        if (err) snprintf(err, errlen, "rule '%s' has no cluster=", rule->name);
        return -1;
    }
    if (rule->cluster == SP_CLUSTER_AUTO && rule->profile == SP_PROFILE_DEFAULT) {
        if (err)
            snprintf(err, errlen,
                     "rule '%s' uses cluster=auto without a latency/background profile",
                     rule->name);
        return -1;
    }
    if (rule->cluster != SP_CLUSTER_KEEP && !rule->set_affinity && !rule->cpuset[0] &&
        !rule->has_nice) {
        if (err)
            snprintf(err, errlen, "rule '%s' has no action (affinity/cpuset/nice are all off)",
                     rule->name);
        return -1;
    }
    static const char *const groups[] = {"process", "thread", "exe", "cgroup"};
    struct sp_pattern *patterns[] = {&rule->process_comm, &rule->thread_comm, &rule->exe,
                                     &rule->cgroup};
    for (size_t i = 0; i < 4; ++i) {
        struct sp_pattern *pattern = patterns[i];
        if (!pattern->src[0]) continue;
        if (pattern->set) {
            regfree(&pattern->re);
            pattern->set = false;
        }
        char detail[160];
        if (sp_pattern_compile(pattern, pattern->src, pattern->icase, detail,
                               sizeof detail) != 0) {
            if (err)
                snprintf(err, errlen, "rule '%s' field %s: %s", rule->name, groups[i], detail);
            return -1;
        }
    }
    return 0;
}

bool sp_rule_has_process_filter(const struct sp_rule *rule) {
    return rule->process_comm.set || rule->exe.set || rule->cgroup.set || rule->has_uid;
}

bool sp_rule_process_possible(const struct sp_rule *rule, const struct sp_match_input *in) {
    if (rule->process_comm.set && !sp_pattern_match(&rule->process_comm, in->pcomm))
        return false;
    if (rule->exe.set && !sp_pattern_match(&rule->exe, in->exe)) return false;
    if (rule->cgroup.set && !sp_pattern_match(&rule->cgroup, in->cgroup)) return false;
    if (rule->has_uid) {
        if (!in->has_uid) return false;
        bool equal = in->uid == rule->uid;
        if (rule->uid_exclude ? equal : !equal) return false;
    }
    return true;
}

static bool rule_thread_possible(const struct sp_rule *rule, const struct sp_match_input *in) {
    if (rule->thread_comm.set && !sp_pattern_match(&rule->thread_comm, in->tcomm))
        return false;
    if (rule->has_busy_lt) {
        if (!in->busy_known || !(in->busy_pct < rule->busy_lt)) return false;
    }
    if (rule->has_busy_gt) {
        if (!in->busy_known || !(in->busy_pct > rule->busy_gt)) return false;
    }
    return true;
}

int sp_rules_match(const struct sp_ruleset *set, const struct sp_match_input *in) {
    if (!set || !in) return -1;
    for (size_t i = 0; i < set->count; ++i) {
        const struct sp_rule *rule = &set->rules[i];
        if (!rule->enabled) continue;
        if (!sp_rule_process_possible(rule, in)) continue;
        if (!rule_thread_possible(rule, in)) continue;
        return (int)i;
    }
    return -1;
}

bool sp_rules_process_candidate(const struct sp_ruleset *set, const struct sp_match_input *in) {
    if (!set || !in) return false;
    for (size_t i = 0; i < set->count; ++i) {
        const struct sp_rule *rule = &set->rules[i];
        if (!rule->enabled) continue;
        if (!sp_rule_process_possible(rule, in)) continue;
        return true;
    }
    return false;
}

void sp_ruleset_refresh_flags(struct sp_ruleset *set) {
    if (!set) return;
    set->needs_busy = false;
    set->needs_cgroup = false;
    set->needs_exe = false;
    for (size_t i = 0; i < set->count; ++i) {
        const struct sp_rule *rule = &set->rules[i];
        if (!rule->enabled) continue;
        if (rule->has_busy_lt || rule->has_busy_gt) set->needs_busy = true;
        if (rule->cgroup.set) set->needs_cgroup = true;
        if (rule->exe.set) set->needs_exe = true;
    }
}

void sp_ruleset_free(struct sp_ruleset *set) {
    if (!set) return;
    for (size_t i = 0; i < set->count; ++i) {
        struct sp_rule *rule = &set->rules[i];
        struct sp_pattern *patterns[] = {&rule->process_comm, &rule->thread_comm, &rule->exe,
                                         &rule->cgroup};
        for (size_t j = 0; j < 4; ++j) {
            if (patterns[j]->set) {
                regfree(&patterns[j]->re);
                patterns[j]->set = false;
            }
        }
    }
    set->count = 0;
}
