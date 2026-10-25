/* test_apply — mask generation, cpuset writes, dry-run and restore. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "sp_apply.h"
#include "sp_fixture.h"
#include "sp_log.h"
#include "sp_test.h"

static struct sp_rule *add_rule(struct sp_config *cfg, const char *name,
                                enum sp_cluster cluster, const char *group) {
    struct sp_rule *rule = &cfg->rules.rules[cfg->rules.count++];
    memset(rule, 0, sizeof *rule);
    sp_strlcpy(rule->name, name, sizeof rule->name);
    rule->enabled = true;
    rule->set_affinity = true;
    rule->cluster = cluster;
    rule->cluster_set = true;
    rule->profile = SP_PROFILE_LATENCY;
    if (group) sp_strlcpy(rule->cpuset, group, sizeof rule->cpuset);
    sp_strlcpy(rule->reason, "unit test rule", sizeof rule->reason);
    char err[256];
    CHECK(sp_rule_finalize(rule, err, sizeof err) == 0);
    return rule;
}

static void test_cpuset_files(void) {
    char tmp[512];
    CHECK(sp_fixture_mkdtemp(tmp, sizeof tmp) != NULL);
    struct sp_config cfg;
    sp_conf_defaults(&cfg);
    char root[1024];
    CHECK(sp_path_join(root, sizeof root, tmp, "cgroup") == 0);
    CHECK(sp_mkdir_p(root, 0755) == 0);
    sp_strlcpy(cfg.cpuset_root, root, sizeof cfg.cpuset_root);
    cfg.enable_cgroup = true;

    char group_path[1024];
    CHECK(sp_cpuset_group_path(&cfg, "little", group_path, sizeof group_path) == 0);
    CHECK(strstr(group_path, "/little") != NULL);

    /* emulate the files a real cgroup v2 directory exposes */
    CHECK(sp_fixture_write(tmp, "cgroup/little/cpuset.cpus", "") == 0);
    CHECK(sp_fixture_write(tmp, "cgroup/little/cpuset.mems", "") == 0);
    CHECK(sp_fixture_write(tmp, "cgroup/little/cgroup.threads", "") == 0);

    cpu_set_t mask;
    CHECK(sp_parse_cpulist("0-3", &mask));
    struct sp_stats stats;
    memset(&stats, 0, sizeof stats);
    CHECK(sp_cpuset_ensure(&cfg, "little", &mask, &stats) == 0);
    char text[SP_TEXT_MAX];
    CHECK(sp_fixture_read(tmp, "cgroup/little/cpuset.cpus", text, sizeof text) >= 0);
    CHECK_STR(text, "0-3");
    CHECK(sp_fixture_read(tmp, "cgroup/little/cpuset.mems", text, sizeof text) >= 0);
    CHECK_STR(text, "0");

    CHECK(sp_cpuset_join(&cfg, "little", getpid(), &stats) == 0);
    CHECK(sp_fixture_read(tmp, "cgroup/little/cgroup.threads", text, sizeof text) >= 0);
    char expected[32];
    snprintf(expected, sizeof expected, "%ld", (long)getpid());
    CHECK_STR(text, expected);

    CHECK(sp_parse_cpulist("0-1", &mask));
    CHECK(sp_cpuset_ensure(&cfg, "little", &mask, &stats) == 0);
    CHECK(sp_fixture_read(tmp, "cgroup/little/cpuset.cpus", text, sizeof text) >= 0);
    CHECK_STR(text, "0-1");

    /* a group created by us but without cpuset files (no cpuset controller) is reported */
    CHECK(sp_cpuset_ensure(&cfg, "plain", &mask, &stats) == -1);
    CHECK(sp_cpuset_join(&cfg, "plain", getpid(), &stats) == 0); /* disabled: no-op */
    sp_apply_reset_for_tests();
    sp_fixture_remove(tmp);
}

static void test_dry_run_and_apply(void) {
    char tmp[512];
    CHECK(sp_fixture_mkdtemp(tmp, sizeof tmp) != NULL);
    char sysfs[1024];
    CHECK(sp_path_join(sysfs, sizeof sysfs, tmp, "sys") == 0);
    const int caps[8] = {284, 284, 284, 284, 871, 871, 871, 1024};
    CHECK(sp_fixture_sysfs(sysfs, caps, NULL, 8, "0-7") == 0);
    char cgroup_root[1024];
    CHECK(sp_path_join(cgroup_root, sizeof cgroup_root, tmp, "cgroup") == 0);
    CHECK(sp_mkdir_p(cgroup_root, 0755) == 0);

    struct sp_topology topo;
    char err[256];
    CHECK(sp_topo_discover(&topo, sysfs, err, sizeof err) == 0);

    struct sp_config cfg;
    sp_conf_defaults(&cfg);
    sp_strlcpy(cfg.cpuset_root, cgroup_root, sizeof cfg.cpuset_root);
    sp_strlcpy(cfg.sysfs_root, sysfs, sizeof cfg.sysfs_root);
    cfg.enable_cgroup = true;
    cfg.restore_on_exit = true;
    add_rule(&cfg, "test-little", SP_CLUSTER_LITTLE, "test-little");

    struct sp_proc_info proc;
    memset(&proc, 0, sizeof proc);
    proc.pid = getpid();
    proc.uid = getuid();
    sp_strlcpy(proc.comm, "test-proc", sizeof proc.comm);
    sp_strlcpy(proc.exe, "/usr/bin/test", sizeof proc.exe);
    sp_strlcpy(proc.cgroup, "/user.slice/test.scope", sizeof proc.cgroup);

    cpu_set_t original;
    CHECK(sched_getaffinity(0, sizeof original, &original) == 0);
    struct sp_thread_info thread;
    memset(&thread, 0, sizeof thread);
    thread.pid = getpid();
    thread.tid = getpid();
    thread.proc = &proc;
    sp_strlcpy(thread.comm, "test-thread", sizeof thread.comm);
    thread.affinity = original; /* the real mask schedplace would have read */
    thread.affinity_valid = true;

    struct sp_stats stats;
    memset(&stats, 0, sizeof stats);
    struct sp_decision decision;
    cfg.dry_run = true;
    CHECK(sp_apply_thread(&cfg, &topo, &thread, 0, 1, &stats, &decision, err,
                          sizeof err) == 0);
    CHECK(decision.result == SP_RESULT_DRY_RUN);
    CHECK(sp_format_cpulist(&decision.mask, err, sizeof err));
    CHECK_STR(err, "0-3");
    CHECK(access(cgroup_root, F_OK) == 0);
    char group_dir[1024];
    CHECK(sp_path_join(group_dir, sizeof group_dir, cgroup_root, "test-little") == 0);
    CHECK(access(group_dir, F_OK) != 0);
    CHECK(stats.affinity_applied == 0);

    /* real cgroups expose these files as soon as the directory exists */
    CHECK(sp_fixture_write(tmp, "cgroup/test-little/cpuset.cpus", "") == 0);
    CHECK(sp_fixture_write(tmp, "cgroup/test-little/cpuset.mems", "") == 0);
    CHECK(sp_fixture_write(tmp, "cgroup/test-little/cgroup.threads", "") == 0);
    cfg.dry_run = false;
    CHECK(sp_apply_thread(&cfg, &topo, &thread, 0, 2, &stats, &decision, err,
                          sizeof err) == 0);
    CHECK_MSG(decision.result == SP_RESULT_APPLIED, "result=%s err=%d",
              sp_result_name(decision.result), decision.err);
    CHECK(stats.affinity_applied == 1);
    CHECK(access(group_dir, F_OK) == 0);
    char text[SP_TEXT_MAX];
    CHECK(sp_fixture_read(tmp, "cgroup/test-little/cpuset.cpus", text, sizeof text) >= 0);
    CHECK_STR(text, "0-3");

    cpu_set_t now;
    CHECK(sched_getaffinity(0, sizeof now, &now) == 0);
    CHECK(sp_cpuset_subset(&now, &decision.mask));

    /* second pass with the same mask is reported as unchanged */
    thread.affinity = decision.mask;
    CHECK(sp_apply_thread(&cfg, &topo, &thread, 0, 3, &stats, &decision, err,
                          sizeof err) == 0);
    CHECK(decision.result == SP_RESULT_UNCHANGED);
    CHECK(stats.skipped_unchanged == 1);

    sp_apply_restore_all(&cfg, &stats, &stats);
    cpu_set_t restored;
    CHECK(sched_getaffinity(0, sizeof restored, &restored) == 0);
    CHECK(sp_cpuset_equal(&restored, &original));
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    cpu_set_t everything;
    CPU_ZERO(&everything);
    for (long cpu = 0; cpu < online && cpu < CPU_SETSIZE; ++cpu) CPU_SET((int)cpu, &everything);
    char expected_all[SP_TEXT_MAX];
    CHECK(sp_format_cpulist(&everything, expected_all, sizeof expected_all));
    CHECK(sp_fixture_read(tmp, "cgroup/test-little/cpuset.cpus", text, sizeof text) >= 0);
    CHECK_STR(text, expected_all);
    sp_fixture_remove(tmp);
}

int main(void) {
    sp_log_init("test_apply", SP_LOG_ERROR);
    test_cpuset_files();
    test_dry_run_and_apply();
    return sp_test_report("test_apply");
}
