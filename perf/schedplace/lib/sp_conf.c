/* schedplace — configuration model and INI parser. */
#include "sp_conf.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sp_log.h"

enum sp_section {
    SP_SECTION_NONE = 0,
    SP_SECTION_GLOBAL,
    SP_SECTION_TOPOLOGY,
    SP_SECTION_RULE,
    SP_SECTION_LEGACY
};

void sp_conf_defaults(struct sp_config *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof *cfg);
    cfg->interval_ms = 500;
    cfg->probe_ms = 100;
    sp_strlcpy(cfg->cpuset_root, "/sys/fs/cgroup/schedplace", sizeof cfg->cpuset_root);
    sp_strlcpy(cfg->sysfs_root, "/sys/devices/system/cpu", sizeof cfg->sysfs_root);
    sp_strlcpy(cfg->foreground_file, "/run/schedplace/foreground",
               sizeof cfg->foreground_file);
    cfg->enable_cgroup = true;
    cfg->restore_on_exit = true;
    cfg->dry_run = false;
    cfg->allow_profile_performance = false;
    cfg->log_level = SP_LOG_INFO;
    cfg->watch_limit = 64;
    cfg->legacy_enabled = false;
    sp_strlcpy(cfg->legacy_governor, "performance", sizeof cfg->legacy_governor);
    cfg->legacy_pin_webhelper_all = true;
}

static void set_error(char *err, size_t errlen, unsigned line, const char *fmt, ...) {
    if (!err || !errlen) return;
    int offset = snprintf(err, errlen, "line %u: ", line);
    if (offset < 0 || (size_t)offset >= errlen) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err + offset, errlen - (size_t)offset, fmt, ap);
    va_end(ap);
}

static void strip_inline_comment(char *line) {
    bool single = false;
    bool double_quote = false;
    for (char *p = line; *p; ++p) {
        if (*p == '\'' && !double_quote) single = !single;
        else if (*p == '"' && !single) double_quote = !double_quote;
        else if ((*p == '#' || *p == ';') && !single && !double_quote &&
                 (p == line || isspace((unsigned char)p[-1]))) {
            *p = '\0';
            return;
        }
    }
}

static void unquote(char *value) {
    size_t len = strlen(value);
    if (len < 2) return;
    if ((value[0] == '"' && value[len - 1] == '"') ||
        (value[0] == '\'' && value[len - 1] == '\'')) {
        memmove(value, value + 1, len - 2);
        value[len - 2] = '\0';
    }
}

static bool parse_log_level(const char *value, int *out) {
    static const struct {
        const char *name;
        int level;
    } table[] = {
        {"error", SP_LOG_ERROR}, {"warn", SP_LOG_WARN},
        {"info", SP_LOG_INFO},   {"debug", SP_LOG_DEBUG},
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; ++i) {
        if (sp_streq(value, table[i].name)) {
            *out = table[i].level;
            return true;
        }
    }
    return false;
}

static bool valid_group_name(const char *name) {
    if (!*name) return false;
    for (const char *p = name; *p; ++p) {
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' && *p != '-') return false;
    }
    return true;
}

static int handle_global(struct sp_config *cfg, const char *key, const char *value,
                         unsigned line, char *err, size_t errlen) {
    if (sp_streq(key, "interval_ms")) {
        long parsed = 0;
        if (!sp_parse_long(value, &parsed) || parsed < 200 || parsed > 2000) {
            set_error(err, errlen, line, "interval_ms must be an integer in [200, 2000]");
            return -1;
        }
        cfg->interval_ms = parsed;
        return 0;
    }
    if (sp_streq(key, "probe_ms")) {
        long parsed = 0;
        if (!sp_parse_long(value, &parsed) || parsed < 50 || parsed > 1000) {
            set_error(err, errlen, line, "probe_ms must be an integer in [50, 1000]");
            return -1;
        }
        cfg->probe_ms = parsed;
        return 0;
    }
    if (sp_streq(key, "cpuset_root")) {
        if (sp_strlcpy(cfg->cpuset_root, value, sizeof cfg->cpuset_root) == 0) {
            set_error(err, errlen, line, "cpuset_root must not be empty");
            return -1;
        }
        return 0;
    }
    if (sp_streq(key, "sysfs_root")) {
        if (sp_strlcpy(cfg->sysfs_root, value, sizeof cfg->sysfs_root) == 0) {
            set_error(err, errlen, line, "sysfs_root must not be empty");
            return -1;
        }
        return 0;
    }
    if (sp_streq(key, "foreground_file")) {
        sp_strlcpy(cfg->foreground_file, value, sizeof cfg->foreground_file);
        return 0;
    }
    if (sp_streq(key, "watch_limit")) {
        long parsed = 0;
        if (!sp_parse_long(value, &parsed) || parsed < 1 || parsed > 4096) {
            set_error(err, errlen, line, "watch_limit must be an integer in [1, 4096]");
            return -1;
        }
        cfg->watch_limit = parsed;
        return 0;
    }
    if (sp_streq(key, "log_level")) {
        if (!parse_log_level(value, &cfg->log_level)) {
            set_error(err, errlen, line, "log_level must be error|warn|info|debug");
            return -1;
        }
        return 0;
    }
    struct {
        const char *key;
        bool *target;
    } booleans[] = {
        {"enable_cgroup", &cfg->enable_cgroup},
        {"restore_on_exit", &cfg->restore_on_exit},
        {"dry_run", &cfg->dry_run},
    };
    for (size_t i = 0; i < sizeof booleans / sizeof booleans[0]; ++i) {
        if (sp_streq(key, booleans[i].key)) {
            bool value_bool = false;
            if (!sp_parse_bool(value, &value_bool)) {
                set_error(err, errlen, line, "%s must be a boolean", key);
                return -1;
            }
            *booleans[i].target = value_bool;
            return 0;
        }
    }
    set_error(err, errlen, line, "unknown [global] key '%s'", key);
    return -1;
}

static int handle_legacy(struct sp_config *cfg, const char *key, const char *value,
                         unsigned line, char *err, size_t errlen) {
    if (sp_streq(key, "enabled")) {
        if (!sp_parse_bool(value, &cfg->legacy_enabled)) {
            set_error(err, errlen, line, "enabled must be a boolean");
            return -1;
        }
        return 0;
    }
    if (sp_streq(key, "governor")) {
        sp_strlcpy(cfg->legacy_governor, value, sizeof cfg->legacy_governor);
        return 0;
    }
    if (sp_streq(key, "pin_steamwebhelper_all")) {
        if (!sp_parse_bool(value, &cfg->legacy_pin_webhelper_all)) {
            set_error(err, errlen, line, "pin_steamwebhelper_all must be a boolean");
            return -1;
        }
        return 0;
    }
    set_error(err, errlen, line, "unknown [legacy-performance] key '%s'", key);
    return -1;
}

static int handle_rule(struct sp_rule *rule, const char *key, const char *value,
                       unsigned line, char *err, size_t errlen) {
    struct {
        const char *key;
        struct sp_pattern *pattern;
    } patterns[] = {
        {"process", &rule->process_comm},
        {"match_process", &rule->process_comm},
        {"thread", &rule->thread_comm},
        {"match_thread", &rule->thread_comm},
        {"exe", &rule->exe},
        {"match_exe", &rule->exe},
        {"cgroup", &rule->cgroup},
    };
    for (size_t i = 0; i < sizeof patterns / sizeof patterns[0]; ++i) {
        if (!sp_streq(key, patterns[i].key)) continue;
        if (sp_strlcpy(patterns[i].pattern->src, value,
                       sizeof patterns[i].pattern->src) == 0) {
            set_error(err, errlen, line, "%s must not be empty", key);
            return -1;
        }
        return 0;
    }
    if (sp_streq(key, "reason")) {
        sp_strlcpy(rule->reason, value, sizeof rule->reason);
        return 0;
    }
    if (sp_streq(key, "enabled")) {
        if (!sp_parse_bool(value, &rule->enabled)) {
            set_error(err, errlen, line, "enabled must be a boolean");
            return -1;
        }
        return 0;
    }
    if (sp_streq(key, "affinity")) {
        if (!sp_parse_bool(value, &rule->set_affinity)) {
            set_error(err, errlen, line, "affinity must be a boolean");
            return -1;
        }
        return 0;
    }
    if (sp_streq(key, "case_insensitive")) {
        bool flag = false;
        if (!sp_parse_bool(value, &flag)) {
            set_error(err, errlen, line, "case_insensitive must be a boolean");
            return -1;
        }
        struct sp_pattern *all[] = {&rule->process_comm, &rule->thread_comm, &rule->exe,
                                    &rule->cgroup};
        for (size_t i = 0; i < 4; ++i) all[i]->icase = flag;
        return 0;
    }
    if (sp_streq(key, "profile")) {
        if (!sp_profile_from_name(value, &rule->profile)) {
            set_error(err, errlen, line,
                      "profile must be latency|background|default");
            return -1;
        }
        return 0;
    }
    if (sp_streq(key, "cluster")) {
        if (!sp_cluster_from_name(value, &rule->cluster)) {
            set_error(err, errlen, line,
                      "cluster must be little|big|prime|perf|keep|auto");
            return -1;
        }
        rule->cluster_set = true;
        return 0;
    }
    if (sp_streq(key, "cpuset")) {
        if (!valid_group_name(value)) {
            set_error(err, errlen, line,
                      "cpuset group name may only contain [A-Za-z0-9._-]");
            return -1;
        }
        sp_strlcpy(rule->cpuset, value, sizeof rule->cpuset);
        return 0;
    }
    if (sp_streq(key, "nice")) {
        long parsed = 0;
        if (!sp_parse_long(value, &parsed) || parsed < -20 || parsed > 19) {
            set_error(err, errlen, line, "nice must be an integer in [-20, 19]");
            return -1;
        }
        rule->has_nice = true;
        rule->nice = (int)parsed;
        return 0;
    }
    if (sp_streq(key, "uid")) {
        unsigned long parsed = 0;
        if (!sp_parse_ulong(value, &parsed) || parsed > 4294967295UL) {
            set_error(err, errlen, line, "uid must be a non-negative integer");
            return -1;
        }
        rule->has_uid = true;
        rule->uid = (uid_t)parsed;
        rule->uid_exclude = false;
        return 0;
    }
    if (sp_streq(key, "exclude_uid")) {
        unsigned long parsed = 0;
        if (!sp_parse_ulong(value, &parsed) || parsed > 4294967295UL) {
            set_error(err, errlen, line, "exclude_uid must be a non-negative integer");
            return -1;
        }
        rule->has_uid = true;
        rule->uid = (uid_t)parsed;
        rule->uid_exclude = true;
        return 0;
    }
    if (sp_streq(key, "busy_lt") || sp_streq(key, "busy_gt")) {
        bool ok = false;
        double parsed = sp_parse_double(value, &ok);
        if (!ok || parsed < 0.0 || parsed > 100.0) {
            set_error(err, errlen, line, "%s must be a percentage in [0, 100]", key);
            return -1;
        }
        if (sp_streq(key, "busy_lt")) {
            rule->has_busy_lt = true;
            rule->busy_lt = parsed;
        } else {
            rule->has_busy_gt = true;
            rule->busy_gt = parsed;
        }
        return 0;
    }
    set_error(err, errlen, line, "unknown rule key '%s'", key);
    return -1;
}

static void split_key_value(char *line, char **key, char **value, bool *ok) {
    *ok = false;
    char *eq = strchr(line, '=');
    if (!eq) return;
    *eq = '\0';
    *key = sp_trim(line);
    *value = sp_trim(eq + 1);
    if (!**key) return;
    unquote(*value);
    *ok = true;
}

int sp_conf_rule_index(const struct sp_config *cfg, const char *name) {
    if (!cfg || !name) return -1;
    for (size_t i = 0; i < cfg->rules.count; ++i)
        if (sp_streq(cfg->rules.rules[i].name, name)) return (int)i;
    return -1;
}

static int append_legacy_rule(struct sp_config *cfg, char *err, size_t errlen) {
    if (!cfg->legacy_enabled || !cfg->legacy_pin_webhelper_all) return 0;
    if (cfg->rules.count >= SP_MAX_RULES) {
        if (err) snprintf(err, errlen, "too many rules (max %d)", SP_MAX_RULES);
        return -1;
    }
    struct sp_rule *rule = &cfg->rules.rules[cfg->rules.count++];
    memset(rule, 0, sizeof *rule);
    sp_strlcpy(rule->name, "legacy-steamwebhelper-all", sizeof rule->name);
    rule->enabled = true;
    rule->legacy = true;
    rule->profile = SP_PROFILE_BACKGROUND;
    sp_strlcpy(rule->process_comm.src, "^steamwebhelper$", sizeof rule->process_comm.src);
    sp_strlcpy(rule->thread_comm.src, "^steamwebhelper$", sizeof rule->thread_comm.src);
    rule->cluster = SP_CLUSTER_LITTLE;
    rule->cluster_set = true;
    rule->set_affinity = true;
    sp_strlcpy(rule->reason,
               "legacy gamemod profile: every steamwebhelper thread on little cores "
               "(explicitly re-enabled, default off)",
               sizeof rule->reason);
    char detail[160];
    if (sp_rule_finalize(rule, detail, sizeof detail) != 0) {
        if (err) snprintf(err, errlen, "%s", detail);
        return -1;
    }
    return 0;
}

int sp_conf_load(struct sp_config *cfg, const char *path, char *err, size_t errlen) {
    if (!cfg || !path) return -1;
    sp_conf_defaults(cfg);
    sp_strlcpy(cfg->path, path, sizeof cfg->path);
    FILE *file = fopen(path, "r");
    if (!file) {
        if (err) snprintf(err, errlen, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    enum sp_section section = SP_SECTION_NONE;
    struct sp_rule *current = NULL;
    unsigned line_number = 0;
    char line[1024];
    int rc = 0;
    while (fgets(line, sizeof line, file)) {
        ++line_number;
        char *text = sp_trim(line);
        if (!*text || *text == '#' || *text == ';') continue;
        strip_inline_comment(text);
        text = sp_trim(text);
        if (!*text) continue;
        if (*text == '[') {
            char *close = strchr(text, ']');
            if (!close) {
                set_error(err, errlen, line_number, "unterminated section header");
                rc = -1;
                break;
            }
            *close = '\0';
            char *name = sp_trim(text + 1);
            if (current) {
                char detail[160];
                if (sp_rule_finalize(current, detail, sizeof detail) != 0) {
                    set_error(err, errlen, line_number, "%s", detail);
                    rc = -1;
                    break;
                }
                current = NULL;
            }
            if (sp_streq(name, "global")) {
                section = SP_SECTION_GLOBAL;
            } else if (sp_streq(name, "topology")) {
                section = SP_SECTION_TOPOLOGY;
            } else if (sp_streq(name, "legacy-performance")) {
                section = SP_SECTION_LEGACY;
            } else if (strncmp(name, "rule", 4) == 0 && (name[4] == ' ' || name[4] == '\t')) {
                char *rule_name = sp_trim(name + 4);
                if (!*rule_name) {
                    set_error(err, errlen, line_number, "rule section needs a name");
                    rc = -1;
                    break;
                }
                if (cfg->rules.count >= SP_MAX_RULES) {
                    set_error(err, errlen, line_number, "too many rules (max %d)",
                              SP_MAX_RULES);
                    rc = -1;
                    break;
                }
                if (sp_conf_rule_index(cfg, rule_name) >= 0) {
                    set_error(err, errlen, line_number, "duplicate rule '%s'", rule_name);
                    rc = -1;
                    break;
                }
                current = &cfg->rules.rules[cfg->rules.count++];
                memset(current, 0, sizeof *current);
                sp_strlcpy(current->name, rule_name, sizeof current->name);
                current->enabled = true;
                current->set_affinity = true;
                current->profile = SP_PROFILE_DEFAULT;
                section = SP_SECTION_RULE;
            } else {
                set_error(err, errlen, line_number, "unknown section [%s]", name);
                rc = -1;
                break;
            }
            continue;
        }
        char *key = NULL;
        char *value = NULL;
        bool ok = false;
        split_key_value(text, &key, &value, &ok);
        if (!ok) {
            set_error(err, errlen, line_number, "expected 'key = value'");
            rc = -1;
            break;
        }
        int handled = 0;
        switch (section) {
        case SP_SECTION_GLOBAL:
            handled = handle_global(cfg, key, value, line_number, err, errlen);
            break;
        case SP_SECTION_TOPOLOGY:
            if (sp_streq(key, "sysfs_root")) {
                if (sp_strlcpy(cfg->sysfs_root, value, sizeof cfg->sysfs_root) == 0) {
                    set_error(err, errlen, line_number, "sysfs_root must not be empty");
                    handled = -1;
                }
            } else {
                set_error(err, errlen, line_number, "unknown [topology] key '%s'", key);
                handled = -1;
            }
            break;
        case SP_SECTION_LEGACY:
            handled = handle_legacy(cfg, key, value, line_number, err, errlen);
            break;
        case SP_SECTION_RULE:
            handled = handle_rule(current, key, value, line_number, err, errlen);
            break;
        case SP_SECTION_NONE:
        default:
            set_error(err, errlen, line_number, "key '%s' outside of any section", key);
            handled = -1;
            break;
        }
        if (handled != 0) {
            rc = -1;
            break;
        }
    }
    if (rc == 0 && current) {
        char detail[160];
        if (sp_rule_finalize(current, detail, sizeof detail) != 0) {
            set_error(err, errlen, line_number, "%s", detail);
            rc = -1;
        }
    }
    fclose(file);
    if (rc != 0) {
        sp_ruleset_free(&cfg->rules);
        return -1;
    }
    if (append_legacy_rule(cfg, err, errlen) != 0) {
        sp_ruleset_free(&cfg->rules);
        return -1;
    }
    sp_ruleset_refresh_flags(&cfg->rules);
    return 0;
}

int sp_conf_validate(const struct sp_config *cfg, bool allow_performance_profile,
                     char *err, size_t errlen) {
    if (!cfg) return -1;
    if (cfg->interval_ms < 200 || cfg->interval_ms > 2000) {
        if (err) snprintf(err, errlen, "interval_ms %ld outside [200, 2000]",
                          cfg->interval_ms);
        return -1;
    }
    if (cfg->probe_ms < 50 || cfg->probe_ms > 1000) {
        if (err) snprintf(err, errlen, "probe_ms %ld outside [50, 1000]", cfg->probe_ms);
        return -1;
    }
    if (!cfg->cpuset_root[0]) {
        if (err) snprintf(err, errlen, "cpuset_root is empty");
        return -1;
    }
    if (!cfg->sysfs_root[0]) {
        if (err) snprintf(err, errlen, "sysfs_root is empty");
        return -1;
    }
    if (cfg->watch_limit < 1 || cfg->watch_limit > 4096) {
        if (err) snprintf(err, errlen, "watch_limit %ld outside [1, 4096]", cfg->watch_limit);
        return -1;
    }
    if (cfg->legacy_enabled && !allow_performance_profile) {
        if (err)
            snprintf(err, errlen,
                     "config requests the legacy performance profile "
                     "([legacy-performance] enabled = true); re-run with "
                     "--allow-profile-performance to opt in explicitly");
        return -1;
    }
    size_t enabled = 0;
    for (size_t i = 0; i < cfg->rules.count; ++i) {
        const struct sp_rule *rule = &cfg->rules.rules[i];
        if (!rule->enabled) continue;
        ++enabled;
        if (!valid_group_name(rule->cpuset) && rule->cpuset[0]) {
            if (err)
                snprintf(err, errlen, "rule '%s': invalid cpuset group '%s'", rule->name,
                         rule->cpuset);
            return -1;
        }
        if (rule->cpuset[0] && !cfg->enable_cgroup)
            sp_logf(SP_LOG_WARN, "rule '%s' wants cpuset '%s' but enable_cgroup = false",
                    rule->name, rule->cpuset);
        bool has_match = sp_rule_has_process_filter(rule) || rule->thread_comm.set ||
                         rule->has_busy_lt || rule->has_busy_gt;
        if (!has_match)
            sp_logf(SP_LOG_WARN, "rule '%s' has no match key and will match every thread",
                    rule->name);
        if (rule->legacy)
            sp_logf(SP_LOG_INFO, "legacy profile rule '%s' is active", rule->name);
    }
    if (enabled == 0) sp_logf(SP_LOG_WARN, "no enabled rules: schedplace will not place anything");
    return 0;
}
