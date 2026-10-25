/* schedplace — CLI entry point. */
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sp_apply.h"
#include "sp_conf.h"
#include "sp_daemon.h"
#include "sp_log.h"
#include "sp_topology.h"
#include "sp_util.h"

#define EXIT_USAGE 1
#define EXIT_SAFETY 2
#define EXIT_RUNTIME 3

static void usage(FILE *out, const char *program) {
    fprintf(out,
            "schedplace %s — rule based thread placement daemon (placement only)\n"
            "\n"
            "Usage: %s [options]\n"
            "\n"
            "  --config PATH              configuration file\n"
            "  --interval-ms N            rescan period, 200..2000 (default 500)\n"
            "  --cpuset-root PATH         cgroup v2 cpuset root for managed groups\n"
            "  --sysfs-root PATH          CPU topology root (default /sys/devices/system/cpu)\n"
            "  --foreground-file PATH     marker file written by the compositor\n"
            "  --log-level LEVEL          error|warn|info|debug\n"
            "  --dry-run                  log decisions, never write anything\n"
            "  --once                     one scan (with a short warm-up for busy%% rules)\n"
            "  --show-topology            print the discovered clusters and exit\n"
            "  --check-config             parse and validate the configuration, then exit\n"
            "  --no-cgroup                affinity only, do not touch cpuset groups\n"
            "  --allow-profile-performance\n"
            "                             opt in to the legacy [legacy-performance] profile\n"
            "  --version, --help\n"
            "\n"
            "Exit codes: 0 ok, 1 usage/config, 2 safety refusal, 3 runtime failure.\n",
            SP_VERSION, program);
}

static const char *find_default_config(void) {
    static const char *const candidates[] = {
        "/etc/schedplace/schedplace.conf",
        "/usr/share/schedplace/schedplace.conf",
        "configs/perf/schedplace.conf",
    };
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; ++i)
        if (access(candidates[i], R_OK) == 0) return candidates[i];
    return NULL;
}

static int require_value(int argc, char **argv, int *index, const char *name,
                         const char *inline_value, const char **out) {
    if (inline_value) {
        *out = inline_value;
        return 0;
    }
    if (*index + 1 >= argc) {
        fprintf(stderr, "schedplace: option %s needs a value\n", name);
        return -1;
    }
    *out = argv[++(*index)];
    return 0;
}

int main(int argc, char **argv) {
    sp_log_init("schedplace", SP_LOG_INFO);
    const char *config_path = NULL;
    const char *value = NULL;
    bool once = false;
    bool show_topology = false;
    bool check_config = false;
    bool cli_dry_run = false;
    bool cli_no_cgroup = false;
    bool cli_allow_profile = false;
    long cli_interval = 0;
    const char *cli_cpuset_root = NULL;
    const char *cli_sysfs_root = NULL;
    const char *cli_foreground = NULL;
    int cli_log_level = -1;

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        const char *eq = strchr(arg, '=');
        size_t name_len = eq ? (size_t)(eq - arg) : strlen(arg);
        const char *inline_value = eq ? eq + 1 : NULL;
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage(stdout, argv[0]);
            return 0;
        }
        if (strcmp(arg, "--version") == 0) {
            printf("schedplace %s\n", SP_VERSION);
            return 0;
        }
        if (name_len == 8 && strncmp(arg, "--config", name_len) == 0) {
            if (require_value(argc, argv, &i, "--config", inline_value, &value) != 0)
                return EXIT_USAGE;
            config_path = value;
        } else if (name_len == 13 && strncmp(arg, "--interval-ms", name_len) == 0) {
            if (require_value(argc, argv, &i, "--interval-ms", inline_value, &value) != 0)
                return EXIT_USAGE;
            if (!sp_parse_long(value, &cli_interval)) {
                fprintf(stderr, "schedplace: invalid --interval-ms value\n");
                return EXIT_USAGE;
            }
        } else if (name_len == 13 && strncmp(arg, "--cpuset-root", name_len) == 0) {
            if (require_value(argc, argv, &i, "--cpuset-root", inline_value, &value) != 0)
                return EXIT_USAGE;
            cli_cpuset_root = value;
        } else if (name_len == 12 && strncmp(arg, "--sysfs-root", name_len) == 0) {
            if (require_value(argc, argv, &i, "--sysfs-root", inline_value, &value) != 0)
                return EXIT_USAGE;
            cli_sysfs_root = value;
        } else if (name_len == 17 && strncmp(arg, "--foreground-file", name_len) == 0) {
            if (require_value(argc, argv, &i, "--foreground-file", inline_value, &value) != 0)
                return EXIT_USAGE;
            cli_foreground = value;
        } else if (name_len == 11 && strncmp(arg, "--log-level", name_len) == 0) {
            if (require_value(argc, argv, &i, "--log-level", inline_value, &value) != 0)
                return EXIT_USAGE;
            if (sp_streq(value, "error")) cli_log_level = SP_LOG_ERROR;
            else if (sp_streq(value, "warn")) cli_log_level = SP_LOG_WARN;
            else if (sp_streq(value, "info")) cli_log_level = SP_LOG_INFO;
            else if (sp_streq(value, "debug")) cli_log_level = SP_LOG_DEBUG;
            else {
                fprintf(stderr, "schedplace: invalid --log-level '%s'\n", value);
                return EXIT_USAGE;
            }
        } else if (strcmp(arg, "--dry-run") == 0) {
            cli_dry_run = true;
        } else if (strcmp(arg, "--once") == 0) {
            once = true;
        } else if (strcmp(arg, "--show-topology") == 0) {
            show_topology = true;
        } else if (strcmp(arg, "--check-config") == 0) {
            check_config = true;
        } else if (strcmp(arg, "--no-cgroup") == 0) {
            cli_no_cgroup = true;
        } else if (strcmp(arg, "--allow-profile-performance") == 0) {
            cli_allow_profile = true;
        } else {
            fprintf(stderr, "schedplace: unknown option '%s' (try --help)\n", arg);
            return EXIT_USAGE;
        }
    }

    struct sp_config cfg;
    const char *resolved = config_path ? config_path : find_default_config();
    if (resolved) {
        char err[256];
        err[0] = '\0';
        if (sp_conf_load(&cfg, resolved, err, sizeof err) != 0) {
            fprintf(stderr, "schedplace: config error: %s\n", err[0] ? err : "unknown");
            return EXIT_USAGE;
        }
    } else {
        sp_conf_defaults(&cfg);
        sp_logf(SP_LOG_WARN,
                "no configuration file found (--config, /etc/schedplace, "
                "/usr/share/schedplace); running with built-in defaults and no rules");
    }
    if (cli_interval) cfg.interval_ms = cli_interval;
    if (cli_cpuset_root) sp_strlcpy(cfg.cpuset_root, cli_cpuset_root, sizeof cfg.cpuset_root);
    if (cli_sysfs_root) sp_strlcpy(cfg.sysfs_root, cli_sysfs_root, sizeof cfg.sysfs_root);
    if (cli_foreground) sp_strlcpy(cfg.foreground_file, cli_foreground, sizeof cfg.foreground_file);
    if (cli_log_level >= 0) cfg.log_level = cli_log_level;
    if (cli_dry_run) cfg.dry_run = true;
    if (cli_no_cgroup) cfg.enable_cgroup = false;
    cfg.allow_profile_performance = cli_allow_profile;
    sp_log_set_level(cfg.log_level);

    char err[256];
    err[0] = '\0';
    if (sp_conf_validate(&cfg, cfg.allow_profile_performance, err, sizeof err) != 0) {
        fprintf(stderr, "schedplace: refusing to start: %s\n", err[0] ? err : "invalid configuration");
        sp_ruleset_free(&cfg.rules);
        return EXIT_SAFETY;
    }
    if (check_config) {
        size_t enabled = 0;
        for (size_t i = 0; i < cfg.rules.count; ++i)
            if (cfg.rules.rules[i].enabled) ++enabled;
        printf("schedplace: config ok: %zu rule(s), %zu enabled, cpuset_root=%s\n",
               cfg.rules.count, enabled, cfg.cpuset_root);
        sp_ruleset_free(&cfg.rules);
        return 0;
    }

    struct sp_topology topo;
    if (sp_topo_discover(&topo, cfg.sysfs_root, err, sizeof err) != 0) {
        fprintf(stderr, "schedplace: topology discovery failed: %s\n", err[0] ? err : "unknown");
        sp_ruleset_free(&cfg.rules);
        return EXIT_RUNTIME;
    }
    char summary[SP_TEXT_MAX * 2];
    sp_topo_summary(&topo, summary, sizeof summary);
    sp_logf(SP_LOG_INFO, "version=%s config=%s dry_run=%d interval_ms=%ld",
            SP_VERSION, cfg.path[0] ? cfg.path : "(defaults)", cfg.dry_run ? 1 : 0,
            cfg.interval_ms);
    sp_logf(SP_LOG_INFO, "topology %s", summary);
    if (show_topology) {
        printf("topology %s\n", summary);
        sp_ruleset_free(&cfg.rules);
        return 0;
    }
    if (cfg.dry_run)
        sp_logf(SP_LOG_INFO, "dry-run mode: decisions are logged, nothing is written");
    if (cfg.legacy_enabled)
        sp_logf(SP_LOG_WARN,
                "legacy performance profile is enabled: it fixes the governor and pins all "
                "steamwebhelper threads to little cores; this is the old gamemod behaviour, "
                "kept opt-in only");

    int rc;
    if (once) {
        rc = sp_daemon_once(&cfg, &topo);
    } else {
        rc = sp_daemon_run(&cfg, &topo);
        if (cfg.restore_on_exit) {
            struct sp_stats stats;
            memset(&stats, 0, sizeof stats);
            sp_apply_restore_all(&cfg, &stats, &stats);
        }
    }
    sp_ruleset_free(&cfg.rules);
    return rc;
}
