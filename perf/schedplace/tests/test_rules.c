/* test_rules — match keys, ordering and busy% double conditions. */
#include <stdio.h>
#include <string.h>

#include "sp_rules.h"
#include "sp_test.h"

static struct sp_rule *add_rule(struct sp_ruleset *set, const char *name,
                                enum sp_cluster cluster) {
    struct sp_rule *rule = &set->rules[set->count++];
    memset(rule, 0, sizeof *rule);
    sp_strlcpy(rule->name, name, sizeof rule->name);
    rule->enabled = true;
    rule->set_affinity = true;
    rule->profile = SP_PROFILE_DEFAULT;
    rule->cluster = cluster;
    rule->cluster_set = true;
    return rule;
}

static int finalize_all(struct sp_ruleset *set) {
    char err[256];
    for (size_t i = 0; i < set->count; ++i) {
        if (sp_rule_finalize(&set->rules[i], err, sizeof err) != 0) {
            fprintf(stderr, "finalize failed: %s\n", err);
            return -1;
        }
    }
    return 0;
}

static void test_patterns(void) {
    struct sp_pattern pattern;
    char err[256];
    CHECK(sp_pattern_compile(&pattern, "^schedplace-e2e-t$", false, err, sizeof err) == 0);
    CHECK(sp_pattern_match(&pattern, "schedplace-e2e-t"));
    CHECK(!sp_pattern_match(&pattern, "schedplace-e2e-thread"));
    regfree(&pattern.re);

    CHECK(sp_pattern_compile(&pattern, "fex|FEX|JIT", true, err, sizeof err) == 0);
    CHECK(sp_pattern_match(&pattern, "rexjit"));
    CHECK(sp_pattern_match(&pattern, "FEX-0"));
    regfree(&pattern.re);

    CHECK(sp_pattern_compile(&pattern, "([", false, err, sizeof err) != 0);
    CHECK(strlen(err) > 0);
}

static void test_matching(void) {
    struct sp_ruleset set;
    memset(&set, 0, sizeof set);
    struct sp_rule *jit = add_rule(&set, "fex-jit", SP_CLUSTER_BIG);
    sp_strlcpy(jit->thread_comm.src, "^(FEX|JIT|CPUThread)", sizeof jit->thread_comm.src);
    jit->profile = SP_PROFILE_LATENCY;
    struct sp_rule *render = add_rule(&set, "wine-render", SP_CLUSTER_PERF);
    sp_strlcpy(render->exe.src, "^/usr/(bin|lib)/wine", sizeof render->exe.src);
    struct sp_rule *web = add_rule(&set, "steamwebhelper-idle", SP_CLUSTER_LITTLE);
    sp_strlcpy(web->process_comm.src, "^steamwebhelper$", sizeof web->process_comm.src);
    web->has_busy_lt = true;
    web->busy_lt = 20.0;
    struct sp_rule *web_busy = add_rule(&set, "steamwebhelper-busy-keep", SP_CLUSTER_KEEP);
    sp_strlcpy(web_busy->process_comm.src, "^steamwebhelper$",
               sizeof web_busy->process_comm.src);
    web_busy->has_busy_gt = true;
    web_busy->busy_gt = 30.0;
    struct sp_rule *idx = add_rule(&set, "indexer", SP_CLUSTER_LITTLE);
    sp_strlcpy(idx->process_comm.src, "^(updatedb|baloo_file)$",
               sizeof idx->process_comm.src);
    sp_strlcpy(idx->cgroup.src, "^/user.slice/", sizeof idx->cgroup.src);
    struct sp_rule *uid_rule = add_rule(&set, "uid-1000", SP_CLUSTER_LITTLE);
    uid_rule->has_uid = true;
    uid_rule->uid = 1000;
    CHECK(finalize_all(&set) == 0);

    struct sp_match_input input;
    memset(&input, 0, sizeof input);
    input.pcomm = "fex";
    input.tcomm = "CPUThread-3";
    input.exe = "/usr/bin/fex";
    input.cgroup = "/user.slice/user-1000.slice";
    input.has_uid = true;
    input.uid = 1000;
    input.busy_pct = 12.0;
    input.busy_known = true;
    CHECK(sp_rules_match(&set, &input) == 0);

    struct sp_match_input other = input;
    other.tcomm = "worker";
    /* exe/process rules do not match, so the uid rule (index 5) wins */
    CHECK(sp_rules_match(&set, &other) == 5);

    struct sp_match_input wine = other;
    wine.exe = "/usr/lib/wine/wine64";
    CHECK(sp_rules_match(&set, &wine) == 1);

    struct sp_match_input web_input = other;
    web_input.pcomm = "steamwebhelper";
    web_input.tcomm = "steamwebhelper";
    web_input.busy_pct = 5.0;
    CHECK(sp_rules_match(&set, &web_input) == 2);
    web_input.busy_pct = 45.0;
    CHECK(sp_rules_match(&set, &web_input) == 3);
    web_input.busy_pct = 10.0;
    web_input.busy_known = false;
    CHECK(sp_rules_match(&set, &web_input) == 5); /* busy rules need a known sample */

    struct sp_match_input indexer = other;
    indexer.pcomm = "updatedb";
    indexer.uid = 0;
    CHECK(sp_rules_match(&set, &indexer) == 4);
    indexer.cgroup = "/system.slice";
    CHECK(sp_rules_match(&set, &indexer) == -1);
    indexer.uid = 1000;
    indexer.cgroup = "/user.slice/user-1000.slice/session.scope";
    CHECK(sp_rules_match(&set, &indexer) == 4);

    set.rules[0].enabled = false;
    CHECK(sp_rules_match(&set, &input) == 5);
    set.rules[0].enabled = true;

    /* process-level candidate filtering: another ruleset with no thread-only rules */
    struct sp_ruleset process_only;
    memset(&process_only, 0, sizeof process_only);
    struct sp_rule *exe_rule = add_rule(&process_only, "exe-only", SP_CLUSTER_BIG);
    sp_strlcpy(exe_rule->exe.src, "^/opt/steam/", sizeof exe_rule->exe.src);
    struct sp_rule *uid_only = add_rule(&process_only, "uid-only", SP_CLUSTER_LITTLE);
    uid_only->has_uid = true;
    uid_only->uid = 1000;
    CHECK(finalize_all(&process_only) == 0);
    struct sp_match_input unknown = other;
    unknown.pcomm = "bash";
    unknown.exe = "/usr/bin/bash";
    unknown.cgroup = "/system.slice";
    unknown.uid = 4242;
    CHECK(!sp_rules_process_candidate(&process_only, &unknown));
    unknown.uid = 1000;
    CHECK(sp_rules_process_candidate(&process_only, &unknown));
    unknown.uid = 4242;
    unknown.exe = "/opt/steam/steam";
    CHECK(sp_rules_process_candidate(&process_only, &unknown));
    sp_ruleset_free(&process_only);

    sp_ruleset_free(&set);
}

static void test_rule_validation(void) {
    struct sp_ruleset set;
    memset(&set, 0, sizeof set);
    char err[256];
    struct sp_rule *rule = add_rule(&set, "no-cluster", SP_CLUSTER_BIG);
    rule->cluster_set = false;
    CHECK(sp_rule_finalize(rule, err, sizeof err) != 0);
    rule->cluster_set = true;
    rule->cluster = SP_CLUSTER_AUTO;
    rule->profile = SP_PROFILE_DEFAULT;
    CHECK(sp_rule_finalize(rule, err, sizeof err) != 0);
    rule->profile = SP_PROFILE_LATENCY;
    CHECK(sp_rule_finalize(rule, err, sizeof err) == 0);
    sp_ruleset_free(&set);

    CHECK_STR(sp_cluster_name(SP_CLUSTER_PERF), "perf");
    CHECK_STR(sp_profile_name(SP_PROFILE_BACKGROUND), "background");
    enum sp_cluster cluster;
    CHECK(sp_cluster_from_name("prime", &cluster) && cluster == SP_CLUSTER_PRIME);
    CHECK(!sp_cluster_from_name("medium", &cluster));
    enum sp_profile profile;
    CHECK(sp_profile_from_name("latency", &profile) && profile == SP_PROFILE_LATENCY);
    CHECK(!sp_profile_from_name("realtime", &profile));
}

int main(void) {
    test_patterns();
    test_matching();
    test_rule_validation();
    return sp_test_report("test_rules");
}
