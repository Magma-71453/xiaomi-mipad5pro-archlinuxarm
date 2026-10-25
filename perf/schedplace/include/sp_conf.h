/* schedplace — configuration model and INI parser. */
#ifndef SP_CONF_H
#define SP_CONF_H

#include <stdbool.h>

#include "sp_rules.h"
#include "sp_util.h"

struct sp_config {
    char path[SP_PATH_MAX];
    long interval_ms;
    long probe_ms;
    char cpuset_root[SP_PATH_MAX];
    char sysfs_root[SP_PATH_MAX];
    char foreground_file[SP_PATH_MAX];
    bool enable_cgroup;
    bool restore_on_exit;
    bool dry_run;
    bool allow_profile_performance; /* CLI --allow-profile-performance, default false */
    int log_level;
    long watch_limit;
    /* Optional legacy gamemod profile; default off. */
    bool legacy_enabled;
    char legacy_governor[32];
    bool legacy_pin_webhelper_all;
    struct sp_ruleset rules;
};

void sp_conf_defaults(struct sp_config *cfg);
int sp_conf_load(struct sp_config *cfg, const char *path, char *err, size_t errlen);
int sp_conf_validate(const struct sp_config *cfg, bool allow_performance_profile, char *err,
                     size_t errlen);
int sp_conf_rule_index(const struct sp_config *cfg, const char *name);

#endif
