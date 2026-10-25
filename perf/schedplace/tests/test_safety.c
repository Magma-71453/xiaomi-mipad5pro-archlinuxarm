/* test_safety — boundary refusals, config errors, exit codes, symbol audit. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "sp_conf.h"
#include "sp_fixture.h"
#include "sp_log.h"
#include "sp_test.h"

#ifndef SP_BIN
#define SP_BIN "./schedplace"
#endif

static const char *binary(void) {
    const char *env = getenv("SP_BIN");
    return (env && *env) ? env : SP_BIN;
}

static int run_capture(const char *command, char *out, size_t outsize) {
    if (out && outsize) out[0] = '\0';
    FILE *pipe = popen(command, "r");
    if (!pipe) return -1;
    size_t used = 0;
    if (out && outsize) {
        size_t n = fread(out, 1, outsize - 1, pipe);
        used = n;
        out[used] = '\0';
    }
    int status = pclose(pipe);
    if (status == -1) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

static void write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "w");
    if (!file) return;
    fputs(text, file);
    fclose(file);
}

static void test_config_errors(void) {
    char tmp[512];
    CHECK(sp_fixture_mkdtemp(tmp, sizeof tmp) != NULL);
    char path[1024];
    char err[512];
    struct sp_config cfg;

    CHECK(sp_path_join(path, sizeof path, tmp, "unknown-key.conf") == 0);
    write_text(path, "[global]\nbogus = 1\n");
    CHECK(sp_conf_load(&cfg, path, err, sizeof err) != 0);
    CHECK(strstr(err, "line 2") != NULL);

    CHECK(sp_path_join(path, sizeof path, tmp, "bad-regex.conf") == 0);
    write_text(path, "[rule broken]\nthread = ([\ncluster = little\n");
    CHECK(sp_conf_load(&cfg, path, err, sizeof err) != 0);
    CHECK(strstr(err, "invalid regex") != NULL);

    CHECK(sp_path_join(path, sizeof path, tmp, "no-cluster.conf") == 0);
    write_text(path, "[rule nope]\nthread = foo\n");
    CHECK(sp_conf_load(&cfg, path, err, sizeof err) != 0);
    CHECK(strstr(err, "cluster") != NULL);

    CHECK(sp_path_join(path, sizeof path, tmp, "duplicate.conf") == 0);
    write_text(path,
               "[rule dup]\ncluster = little\n[rule dup]\ncluster = big\n");
    CHECK(sp_conf_load(&cfg, path, err, sizeof err) != 0);
    CHECK(strstr(err, "duplicate") != NULL);

    CHECK(sp_path_join(path, sizeof path, tmp, "bad-section.conf") == 0);
    write_text(path, "[nonsense]\nkey = value\n");
    CHECK(sp_conf_load(&cfg, path, err, sizeof err) != 0);
    CHECK(strstr(err, "unknown section") != NULL);

    CHECK(sp_path_join(path, sizeof path, tmp, "bad-interval.conf") == 0);
    write_text(path, "[global]\ninterval_ms = 20\n");
    CHECK(sp_conf_load(&cfg, path, err, sizeof err) != 0);
    sp_fixture_remove(tmp);
}

static void test_legacy_refusal(void) {
    char tmp[512];
    CHECK(sp_fixture_mkdtemp(tmp, sizeof tmp) != NULL);
    char path[1024];
    CHECK(sp_path_join(path, sizeof path, tmp, "legacy.conf") == 0);
    char cgroup[1024];
    CHECK(sp_path_join(cgroup, sizeof cgroup, tmp, "cgroup") == 0);
    CHECK(sp_mkdir_p(cgroup, 0755) == 0);
    char text[2048];
    snprintf(text, sizeof text,
             "[global]\ninterval_ms = 500\ncpuset_root = %s\n"
             "[rule web]\nthread = ^steamwebhelper$\ncluster = little\n"
             "[legacy-performance]\nenabled = true\ngovernor = performance\n",
             cgroup);
    write_text(path, text);

    struct sp_config cfg;
    char err[512];
    CHECK(sp_conf_load(&cfg, path, err, sizeof err) == 0);
    CHECK(cfg.rules.count == 2); /* one rule plus the legacy profile rule */
    CHECK(strcmp(cfg.rules.rules[1].name, "legacy-steamwebhelper-all") == 0);
    CHECK(sp_conf_validate(&cfg, false, err, sizeof err) != 0);
    CHECK(strstr(err, "--allow-profile-performance") != NULL);
    CHECK(sp_conf_validate(&cfg, true, err, sizeof err) == 0);
    sp_ruleset_free(&cfg.rules);

    char command[4096];
    snprintf(command, sizeof command, "'%s' --check-config --config '%s' 2>/dev/null",
             binary(), path);
    CHECK(run_capture(command, NULL, 0) == 2);
    snprintf(command, sizeof command,
             "'%s' --check-config --config '%s' --allow-profile-performance 2>/dev/null",
             binary(), path);
    CHECK(run_capture(command, NULL, 0) == 0);
    snprintf(command, sizeof command, "'%s' --check-config --config '%s' 2>&1", binary(),
             path);
    char output[4096];
    CHECK(run_capture(command, output, sizeof output) == 2);
    CHECK(strstr(output, "refusing to start") != NULL);
    CHECK(strstr(output, "--allow-profile-performance") != NULL);

    snprintf(command, sizeof command, "'%s' --definitely-not-an-option", binary());
    CHECK(run_capture(command, NULL, 0) == 1);
    snprintf(command, sizeof command, "'%s' --check-config --config /nonexistent/x.conf",
             binary());
    CHECK(run_capture(command, NULL, 0) == 1);
    sp_fixture_remove(tmp);
}

/* The daemon must not even link the scheduler-policy API. */
static void test_no_scheduler_policy_api(void) {
    char command[2048];
    snprintf(command, sizeof command, "nm -u '%s' 2>/dev/null", binary());
    FILE *pipe = popen(command, "r");
    if (!pipe) {
        printf("test_safety: nm unavailable, skipping symbol audit\n");
        return;
    }
    char line[256];
    int found_policy = 0;
    int found_any = 0;
    while (fgets(line, sizeof line, pipe)) {
        found_any = 1;
        if (strstr(line, "sched_setscheduler") || strstr(line, "sched_setattr") ||
            strstr(line, "sched_setparam"))
            found_policy = 1;
    }
    int status = pclose(pipe);
    if (!found_any || status != 0) {
        printf("test_safety: nm produced no symbols, skipping symbol audit\n");
        return;
    }
    CHECK_MSG(!found_policy, "the daemon references sched_setscheduler/sched_setattr");
}

int main(void) {
    sp_log_init("test_safety", SP_LOG_ERROR);
    test_config_errors();
    test_legacy_refusal();
    test_no_scheduler_policy_api();
    return sp_test_report("test_safety");
}

