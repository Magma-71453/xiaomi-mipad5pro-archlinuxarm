/* schedplace — applying placement decisions.
 *
 * Two mechanisms, both placement-only:
 *   - sched_setaffinity(): the actual per-thread CPU mask (always available);
 *   - cgroup v2 cpuset: <cpuset_root>/<group>/cpuset.cpus + cgroup.threads (opt-in,
 *     needs the cpuset controller and a threaded subtree).
 * The scheduler policy (SCHED_*) is never changed and no governor/frequency/hotplug
 * file is ever written; every write funnels through sp_safe_write().
 */
#include "sp_apply.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#include "sp_log.h"

#define SP_MAX_STATES 8192

struct sp_state {
    bool valid;
    pid_t tid;
    unsigned long long starttime;
    cpu_set_t original;
    bool have_original;
    bool affinity_changed;
    bool cgroup_joined;
    char group[SP_NAME_LEN];
    bool have_nice;
    bool nice_changed;
    int original_nice;
    int applied_nice;
    int rule_index;
    unsigned long long last_seen;
};

static struct sp_state *g_states;
static size_t g_state_count;
static size_t g_state_cap;
static bool g_cgroup_disabled;
static bool g_cgroup_warned;
static bool g_join_warned;
static bool g_governor_unsupported;

const char *sp_result_name(enum sp_result result) {
    switch (result) {
    case SP_RESULT_APPLIED:
        return "applied";
    case SP_RESULT_UNCHANGED:
        return "unchanged";
    case SP_RESULT_DRY_RUN:
        return "dry-run";
    case SP_RESULT_KEEP:
        return "keep";
    case SP_RESULT_ERROR:
        return "error";
    }
    return "?";
}

static struct sp_state *state_get(pid_t tid, unsigned long long starttime) {
    for (size_t i = 0; i < g_state_count; ++i)
        if (g_states[i].valid && g_states[i].tid == tid &&
            g_states[i].starttime == starttime)
            return &g_states[i];
    if (g_state_count >= SP_MAX_STATES) {
        sp_logf(SP_LOG_ERROR, "state table full (%d entries); dropping placement tracking",
                SP_MAX_STATES);
        return NULL;
    }
    if (g_state_count == g_state_cap) {
        size_t grown = g_state_cap ? g_state_cap * 2 : 256;
        struct sp_state *resized = realloc(g_states, grown * sizeof *resized);
        if (!resized) return NULL;
        g_states = resized;
        g_state_cap = grown;
    }
    struct sp_state *state = &g_states[g_state_count++];
    memset(state, 0, sizeof *state);
    state->valid = true;
    state->tid = tid;
    state->starttime = starttime;
    state->rule_index = -1;
    return state;
}

static bool thread_alive(pid_t tid, unsigned long long starttime) {
    char path[SP_PATH_MAX];
    char dir[32];
    snprintf(dir, sizeof dir, "%ld", (long)tid);
    if (sp_path_join(path, sizeof path, "/proc", dir) != 0) return false;
    char stat_path[SP_PATH_MAX];
    if (sp_path_join(stat_path, sizeof stat_path, path, "stat") != 0) return false;
    char text[1024];
    if (sp_read_file(stat_path, text, sizeof text) < 0) return false;
    if (starttime == 0) return true; /* caller only knows the tid */
    char *close = strrchr(text, ')');
    if (!close) return false;
    char *p = close + 1;
    unsigned long long parsed = 0;
    for (int field = 3; field <= 22; ++field) {
        while (*p == ' ') ++p;
        if (!*p) return false;
        char *end = p;
        while (*end && *end != ' ') ++end;
        char saved = *end;
        *end = '\0';
        if (field == 22) parsed = strtoull(p, NULL, 10);
        *end = saved;
        p = end;
    }
    return parsed == starttime;
}

size_t sp_state_count(void) {
    size_t count = 0;
    for (size_t i = 0; i < g_state_count; ++i)
        if (g_states[i].valid) ++count;
    return count;
}

void sp_apply_reset_for_tests(void) {
    free(g_states);
    g_states = NULL;
    g_state_count = 0;
    g_state_cap = 0;
    g_cgroup_disabled = false;
    g_cgroup_warned = false;
    g_join_warned = false;
    g_governor_unsupported = false;
}

void sp_state_prune(unsigned long long scan_id) {
    size_t kept = 0;
    for (size_t i = 0; i < g_state_count; ++i) {
        struct sp_state *state = &g_states[i];
        if (!state->valid) continue;
        bool expired = scan_id > state->last_seen + 600;
        if (expired || !thread_alive(state->tid, state->starttime)) {
            state->valid = false;
            continue;
        }
        if (kept != i) g_states[kept] = g_states[i];
        ++kept;
    }
    g_state_count = kept;
}

int sp_cpuset_group_path(const struct sp_config *cfg, const char *group, char *buf,
                         size_t size) {
    if (!cfg || !group || !*group) {
        errno = EINVAL;
        return -1;
    }
    return sp_path_join(buf, size, cfg->cpuset_root, group);
}

int sp_cpuset_ensure(const struct sp_config *cfg, const char *group, const cpu_set_t *mask,
                     struct sp_stats *stats) {
    if (!cfg || !group || !mask) {
        errno = EINVAL;
        return -1;
    }
    if (!cfg->enable_cgroup || g_cgroup_disabled) return 0;
    char dir[SP_PATH_MAX];
    if (sp_cpuset_group_path(cfg, group, dir, sizeof dir) != 0) return -1;
    if (sp_mkdir_p(dir, 0755) != 0) {
        if (!g_cgroup_warned) {
            g_cgroup_warned = true;
            sp_logf(SP_LOG_WARN,
                    "cannot create cpuset group %s (%s); continuing with affinity only",
                    dir, strerror(errno));
        }
        g_cgroup_disabled = true;
        return -1;
    }
    char wanted[SP_TEXT_MAX];
    if (!sp_format_cpulist(mask, wanted, sizeof wanted)) {
        errno = EINVAL;
        return -1;
    }
    char cpus_path[SP_PATH_MAX];
    if (sp_path_join(cpus_path, sizeof cpus_path, dir, "cpuset.cpus") != 0) return -1;
    if (access(cpus_path, F_OK) != 0) {
        if (!g_cgroup_warned) {
            g_cgroup_warned = true;
            sp_logf(SP_LOG_WARN,
                    "%s exists but exposes no cpuset.cpus: the cpuset controller is not "
                    "available here; continuing with affinity only",
                    dir);
        }
        g_cgroup_disabled = true;
        errno = ENODEV;
        return -1;
    }
    char current[SP_TEXT_MAX];
    if (sp_read_file(cpus_path, current, sizeof current) >= 0 &&
        sp_streq(sp_trim(current), wanted))
        return 0;
    char mems_path[SP_PATH_MAX];
    if (sp_path_join(mems_path, sizeof mems_path, dir, "cpuset.mems") != 0) return -1;
    char mems_current[SP_TEXT_MAX];
    if (sp_read_file(mems_path, mems_current, sizeof mems_current) < 0 ||
        !sp_trim(mems_current)[0]) {
        char parent_mems[SP_PATH_MAX];
        if (sp_path_join(parent_mems, sizeof parent_mems, cfg->cpuset_root,
                         "cpuset.mems.effective") == 0) {
            if (sp_read_file(parent_mems, mems_current, sizeof mems_current) < 0)
                sp_strlcpy(mems_current, "0", sizeof mems_current);
        } else {
            sp_strlcpy(mems_current, "0", sizeof mems_current);
        }
        sp_safe_write(mems_path, sp_trim(mems_current), false, stats);
    }
    if (sp_safe_write(cpus_path, wanted, false, stats) != 0) {
        sp_logf(SP_LOG_WARN, "cannot set %s to %s: %s", cpus_path, wanted,
                strerror(errno));
        return -1;
    }
    sp_logf(SP_LOG_DEBUG, "cpuset group %s restricted to %s", group, wanted);
    return 0;
}

int sp_cpuset_join(const struct sp_config *cfg, const char *group, pid_t tid,
                   struct sp_stats *stats) {
    if (!cfg || !group) {
        errno = EINVAL;
        return -1;
    }
    if (!cfg->enable_cgroup || g_cgroup_disabled) return 0;
    char dir[SP_PATH_MAX];
    if (sp_cpuset_group_path(cfg, group, dir, sizeof dir) != 0) return -1;
    char threads_path[SP_PATH_MAX];
    if (sp_path_join(threads_path, sizeof threads_path, dir, "cgroup.threads") != 0)
        return -1;
    if (access(threads_path, F_OK) != 0) {
        if (!g_join_warned) {
            g_join_warned = true;
            sp_logf(SP_LOG_WARN,
                    "%s has no cgroup.threads: this cpuset group is not a threaded "
                    "subtree, so thread-level cgroup migration is unavailable; using "
                    "affinity only",
                    dir);
        }
        return -1;
    }
    char text[32];
    snprintf(text, sizeof text, "%ld", (long)tid);
    if (sp_safe_write(threads_path, text, false, stats) != 0) {
        if (errno == EOPNOTSUPP || errno == ENOTSUP || errno == EINVAL) {
            if (!g_join_warned) {
                g_join_warned = true;
                sp_logf(SP_LOG_WARN,
                        "kernel refused thread-level cgroup migration into %s (%s); "
                        "using affinity only",
                        group, strerror(errno));
            }
            g_cgroup_disabled = true;
        }
        return -1;
    }
    return 0;
}

static void log_decision(const struct sp_config *cfg, const struct sp_topology *topo,
                         const struct sp_thread_info *thread, const struct sp_rule *rule,
                         enum sp_result result, const cpu_set_t *mask, int err) {
    char old_mask[SP_TEXT_MAX];
    char new_mask[SP_TEXT_MAX];
    char cluster[16];
    if (!sp_format_cpulist(&thread->affinity, old_mask, sizeof old_mask))
        sp_strlcpy(old_mask, "-", sizeof old_mask);
    if (mask && sp_format_cpulist(mask, new_mask, sizeof new_mask))
        ;
    else
        new_mask[0] = '\0';
    sp_strlcpy(cluster, sp_cluster_name(rule->cluster), sizeof cluster);
    struct sp_decision_log entry;
    memset(&entry, 0, sizeof entry);
    entry.pid = thread->pid;
    entry.tid = thread->tid;
    entry.pcomm = thread->proc ? thread->proc->comm : "";
    entry.tcomm = thread->comm;
    entry.uid = thread->proc ? thread->proc->uid : 0;
    entry.exe = thread->proc ? thread->proc->exe : "";
    entry.rule = rule->name;
    entry.profile = sp_profile_name(rule->profile);
    entry.cluster = cluster;
    entry.old_mask = old_mask;
    entry.new_mask = new_mask;
    entry.cpuset = rule->cpuset;
    entry.result = sp_result_name(result);
    entry.reason = rule->reason;
    entry.busy_known = thread->busy_known;
    entry.busy_pct = thread->busy_pct;
    entry.has_nice = rule->has_nice;
    entry.nice = rule->nice;
    entry.err = err;
    sp_log_decision(&entry);
    (void)cfg;
    (void)topo;
}

int sp_apply_thread(const struct sp_config *cfg, const struct sp_topology *topo,
                    const struct sp_thread_info *thread, int rule_index,
                    unsigned long long scan_id, struct sp_stats *stats,
                    struct sp_decision *out, char *err, size_t errlen) {
    if (!cfg || !topo || !thread || !out || rule_index < 0 ||
        (size_t)rule_index >= cfg->rules.count) {
        if (err) snprintf(err, errlen, "invalid apply request");
        return -1;
    }
    const struct sp_rule *rule = &cfg->rules.rules[rule_index];
    memset(out, 0, sizeof *out);
    out->thread = thread;
    out->rule = rule;
    out->rule_index = rule_index;
    out->cluster = rule->cluster;
    ++stats->decisions;

    struct sp_state *state = state_get(thread->tid, thread->starttime);
    if (state) {
        state->last_seen = scan_id;
        state->rule_index = rule_index;
    }

    if (rule->cluster == SP_CLUSTER_KEEP) {
        out->result = SP_RESULT_KEEP;
        if (sp_log_level() >= SP_LOG_DEBUG)
            log_decision(cfg, topo, thread, rule, SP_RESULT_KEEP, NULL, 0);
        return 0;
    }
    cpu_set_t desired;
    if (!sp_topo_resolve(topo, rule->cluster, rule->profile, &desired)) {
        out->result = SP_RESULT_ERROR;
        out->err = EINVAL;
        ++stats->errors;
        sp_logf(SP_LOG_ERROR, "rule '%s' resolves to no online CPU for tid %ld", rule->name,
                (long)thread->tid);
        log_decision(cfg, topo, thread, rule, SP_RESULT_ERROR, NULL, EINVAL);
        return -1;
    }
    out->mask = desired;
    out->has_mask = true;
    if (cfg->dry_run) {
        out->result = SP_RESULT_DRY_RUN;
        bool differs = !thread->affinity_valid ||
                       !sp_cpuset_equal(&thread->affinity, &desired);
        if (differs) log_decision(cfg, topo, thread, rule, SP_RESULT_DRY_RUN, &desired, 0);
        return 0;
    }
    if (!state) {
        out->result = SP_RESULT_ERROR;
        out->err = ENOMEM;
        ++stats->errors;
        return -1;
    }

    bool changed = false;
    bool hard_failure = false;
    int last_error = 0;
    if (rule->set_affinity) {
        bool already = thread->affinity_valid &&
                       sp_cpuset_equal(&thread->affinity, &desired);
        if (!already) {
            if (thread->affinity_valid && !state->have_original) {
                state->original = thread->affinity;
                state->have_original = true;
            }
            if (sched_setaffinity(thread->tid, sizeof desired, &desired) != 0) {
                last_error = errno;
                hard_failure = true;
                ++stats->errors;
                sp_logf(SP_LOG_WARN, "sched_setaffinity(tid=%ld) failed: %s",
                        (long)thread->tid, strerror(errno));
            } else {
                state->affinity_changed = true;
                ++stats->affinity_applied;
                changed = true;
            }
        }
    }
    if (!hard_failure && cfg->enable_cgroup && rule->cpuset[0]) {
        if (sp_cpuset_ensure(cfg, rule->cpuset, &desired, stats) == 0) {
            bool joined = state->cgroup_joined && sp_streq(state->group, rule->cpuset);
            if (!joined) {
                if (sp_cpuset_join(cfg, rule->cpuset, thread->tid, stats) == 0) {
                    state->cgroup_joined = true;
                    sp_strlcpy(state->group, rule->cpuset, sizeof state->group);
                    ++stats->cgroup_applied;
                } else {
                    last_error = errno;
                }
            }
        } else {
            last_error = errno;
        }
    }
    if (!hard_failure && rule->has_nice) {
        if (!state->have_nice) {
            errno = 0;
            int current = getpriority(PRIO_PROCESS, (id_t)thread->tid);
            if (!(errno != 0 && current == -1)) {
                state->original_nice = current;
                state->have_nice = true;
            }
        }
        errno = 0;
        int current = getpriority(PRIO_PROCESS, (id_t)thread->tid);
        if (errno == 0 && current == rule->nice) {
            state->applied_nice = rule->nice;
        } else if (setpriority(PRIO_PROCESS, (id_t)thread->tid, rule->nice) == 0) {
            state->nice_changed = true;
            state->applied_nice = rule->nice;
            ++stats->nice_applied;
            changed = true;
        } else {
            last_error = errno;
            sp_logf(SP_LOG_WARN, "setpriority(tid=%ld, %d) failed: %s",
                    (long)thread->tid, rule->nice, strerror(errno));
        }
    }
    if (hard_failure) {
        out->result = SP_RESULT_ERROR;
        out->err = last_error;
        log_decision(cfg, topo, thread, rule, SP_RESULT_ERROR, &desired, last_error);
        return -1;
    }
    out->result = changed ? SP_RESULT_APPLIED : SP_RESULT_UNCHANGED;
    out->err = last_error;
    if (out->result == SP_RESULT_UNCHANGED) ++stats->skipped_unchanged;
    if (out->result == SP_RESULT_APPLIED)
        log_decision(cfg, topo, thread, rule, SP_RESULT_APPLIED, &desired, last_error);
    else if (sp_log_level() >= SP_LOG_DEBUG)
        log_decision(cfg, topo, thread, rule, SP_RESULT_UNCHANGED, &desired, last_error);
    return 0;
}

void sp_apply_restore_all(const struct sp_config *cfg, const struct sp_stats *stats,
                          struct sp_stats *out_stats) {
    (void)stats;
    char groups[SP_MAX_RULES][SP_NAME_LEN];
    size_t group_count = 0;
    if (!cfg->restore_on_exit) return;
    for (size_t i = g_state_count; i > 0; --i) {
        struct sp_state *state = &g_states[i - 1];
        if (!state->valid) continue;
        if (!thread_alive(state->tid, state->starttime)) continue;
        if (state->affinity_changed && state->have_original) {
            if (sched_setaffinity(state->tid, sizeof state->original, &state->original) == 0 &&
                out_stats)
                ++out_stats->restored;
        }
        if (state->nice_changed && state->have_nice) {
            if (setpriority(PRIO_PROCESS, (id_t)state->tid, state->original_nice) == 0 &&
                out_stats)
                ++out_stats->restored;
        }
        if (state->cgroup_joined && cfg->enable_cgroup) {
            bool seen = false;
            for (size_t g = 0; g < group_count; ++g)
                if (sp_streq(groups[g], state->group)) seen = true;
            if (!seen && group_count < SP_MAX_RULES)
                sp_strlcpy(groups[group_count++], state->group, SP_NAME_LEN);
        }
        state->valid = false;
    }
    for (size_t g = 0; g < group_count; ++g) {
        char dir[SP_PATH_MAX];
        if (sp_cpuset_group_path(cfg, groups[g], dir, sizeof dir) != 0) continue;
        char cpus_path[SP_PATH_MAX];
        if (sp_path_join(cpus_path, sizeof cpus_path, dir, "cpuset.cpus") != 0) continue;
        if (access(cpus_path, F_OK) != 0) continue;
        char all[SP_TEXT_MAX];
        char online[SP_TEXT_MAX];
        /* widen the group back to every online CPU instead of leaving it restricted */
        long count = sysconf(_SC_NPROCESSORS_ONLN);
        if (count < 1 || count > CPU_SETSIZE) continue;
        cpu_set_t mask;
        CPU_ZERO(&mask);
        for (long cpu = 0; cpu < count; ++cpu) CPU_SET((int)cpu, &mask);
        if (!sp_format_cpulist(&mask, all, sizeof all)) continue;
        if (sp_safe_write(cpus_path, all, false, out_stats) == 0) {
            sp_logf(SP_LOG_INFO, "widened cpuset group %s back to %s", groups[g], all);
        }
        (void)online;
    }
    g_state_count = 0;
}

void sp_state_report(FILE *out, const struct sp_config *cfg, const struct sp_topology *topo,
                     const struct sp_stats *stats) {
    if (!out || !cfg || !topo || !stats) return;
    char summary[SP_TEXT_MAX * 2];
    sp_topo_summary(topo, summary, sizeof summary);
    fprintf(out, "schedplace: state pid=%ld dry_run=%d version=%s\n", (long)getpid(),
            cfg->dry_run ? 1 : 0, SP_VERSION);
    fprintf(out, "schedplace: topology %s\n", summary);
    fprintf(out,
            "schedplace: config interval_ms=%ld cpuset_root=%s enable_cgroup=%d "
            "restore_on_exit=%d foreground_file=%s legacy_profile=%d\n",
            cfg->interval_ms, cfg->cpuset_root, cfg->enable_cgroup ? 1 : 0,
            cfg->restore_on_exit ? 1 : 0, cfg->foreground_file[0] ? cfg->foreground_file : "-",
            cfg->legacy_enabled ? 1 : 0);
    fprintf(out,
            "schedplace: counters scans=%lu processes=%lu candidate_threads=%lu "
            "decisions=%lu affinity=%lu cgroup=%lu nice=%lu unchanged=%lu errors=%lu "
            "refused_writes=%lu restored=%lu\n",
            stats->scans, stats->processes_seen, stats->threads_seen, stats->decisions,
            stats->affinity_applied, stats->cgroup_applied, stats->nice_applied,
            stats->skipped_unchanged, stats->errors, stats->refused_writes, stats->restored);
    size_t tracked = 0;
    for (size_t i = 0; i < cfg->rules.count; ++i)
        fprintf(out, "schedplace: rule[%zu] name=%s hits=%lu cluster=%s profile=%s\n", i,
                cfg->rules.rules[i].name, stats->rule_hits[i],
                sp_cluster_name(cfg->rules.rules[i].cluster),
                sp_profile_name(cfg->rules.rules[i].profile));
    for (size_t i = 0; i < g_state_count && tracked < 32; ++i) {
        const struct sp_state *state = &g_states[i];
        if (!state->valid) continue;
        char mask[SP_TEXT_MAX];
        if (!sp_format_cpulist(&state->original, mask, sizeof mask))
            sp_strlcpy(mask, "-", sizeof mask);
        fprintf(out,
                "schedplace: tracked tid=%ld start=%llu rule=%s original_mask=%s "
                "affinity_changed=%d nice=%d\n",
                (long)state->tid, state->starttime,
                state->rule_index >= 0 ? cfg->rules.rules[state->rule_index].name : "-", mask,
                state->affinity_changed ? 1 : 0, state->applied_nice);
        ++tracked;
    }
    fflush(out);
}

int sp_legacy_apply_governor(const struct sp_config *cfg, struct sp_stats *stats) {
    if (!cfg || !cfg->legacy_enabled) return 0;
    if (!cfg->allow_profile_performance) {
        errno = EPERM;
        sp_logf(SP_LOG_ERROR,
                "legacy performance profile requested without "
                "--allow-profile-performance; refusing to touch scaling_governor");
        return -1;
    }
    const char *cpu_root = "/sys/devices/system/cpu/cpufreq";
    DIR *dir = opendir(cpu_root);
    if (!dir) {
        if (!g_governor_unsupported) {
            g_governor_unsupported = true;
            sp_logf(SP_LOG_WARN, "%s unavailable (%s); legacy governor not applied",
                    cpu_root, strerror(errno));
        }
        return -1;
    }
    size_t written = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "policy", 6) != 0) continue;
        char policy_dir[SP_PATH_MAX];
        if (sp_path_join(policy_dir, sizeof policy_dir, cpu_root, entry->d_name) != 0)
            continue;
        char governor_path[SP_PATH_MAX];
        if (sp_path_join(governor_path, sizeof governor_path, policy_dir,
                         "scaling_governor") != 0)
            continue;
        if (sp_safe_write(governor_path, cfg->legacy_governor, true, stats) != 0) continue;
        ++written;
        sp_logf(SP_LOG_INFO, "legacy profile: %s = %s", governor_path,
                cfg->legacy_governor);
    }
    closedir(dir);
    return written > 0 ? (int)written : -1;
}
