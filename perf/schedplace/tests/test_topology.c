/* test_topology — clustering on mock sysfs trees, including real SM8250 data. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "sp_fixture.h"
#include "sp_test.h"
#include "sp_topology.h"

static void expect_mask(const cpu_set_t *mask, const char *wanted, int line) {
    cpu_set_t want;
    if (!sp_parse_cpulist(wanted, &want)) {
        ++sp_test_checks;
        fprintf(stderr, "FAIL line %d: bad expectation '%s'\n", line, wanted);
        ++sp_test_failures;
        return;
    }
    char got[SP_TEXT_MAX];
    char expected[SP_TEXT_MAX];
    if (!sp_format_cpulist(mask, got, sizeof got)) sp_strlcpy(got, "(empty)", sizeof got);
    if (!sp_format_cpulist(&want, expected, sizeof expected))
        sp_strlcpy(expected, "(empty)", sizeof expected);
    CHECK_MSG(sp_cpuset_equal(mask, &want), "line %d: cluster=%s expected=%s", line, got,
              expected);
}

/* Real device (Xiaomi Mi Pad 5 Pro / SM8250): 4x A55 284, 3x A77 871, 1x A77 prime 1024. */
static void test_sm8250(void) {
    char tmp[512];
    CHECK(sp_fixture_mkdtemp(tmp, sizeof tmp) != NULL);
    char root[1024];
    CHECK(sp_path_join(root, sizeof root, tmp, "sys") == 0);
    const int caps[8] = {284, 284, 284, 284, 871, 871, 871, 1024};
    CHECK(sp_fixture_sysfs(root, caps, NULL, 8, "0-7") == 0);

    struct sp_topology topo;
    char err[256];
    CHECK(sp_topo_discover(&topo, root, err, sizeof err) == 0);
    CHECK(topo.levels == 3);
    CHECK(!topo.degraded);
    CHECK_STR(topo.source, "cpu_capacity");
    expect_mask(&topo.cluster[0], "0-3", __LINE__);
    expect_mask(&topo.cluster[1], "4-6", __LINE__);
    expect_mask(&topo.cluster[2], "7", __LINE__);
    CHECK(topo.capacity[0] == 284);
    CHECK(topo.capacity[1] == 871);
    CHECK(topo.capacity[2] == 1024);

    cpu_set_t mask;
    CHECK(sp_topo_resolve(&topo, SP_CLUSTER_LITTLE, SP_PROFILE_DEFAULT, &mask));
    expect_mask(&mask, "0-3", __LINE__);
    CHECK(sp_topo_resolve(&topo, SP_CLUSTER_BIG, SP_PROFILE_DEFAULT, &mask));
    expect_mask(&mask, "4-6", __LINE__);
    CHECK(sp_topo_resolve(&topo, SP_CLUSTER_PRIME, SP_PROFILE_DEFAULT, &mask));
    expect_mask(&mask, "7", __LINE__);
    CHECK(sp_topo_resolve(&topo, SP_CLUSTER_PERF, SP_PROFILE_DEFAULT, &mask));
    expect_mask(&mask, "4-7", __LINE__);
    CHECK(sp_topo_resolve(&topo, SP_CLUSTER_AUTO, SP_PROFILE_LATENCY, &mask));
    expect_mask(&mask, "4-7", __LINE__);
    CHECK(sp_topo_resolve(&topo, SP_CLUSTER_AUTO, SP_PROFILE_BACKGROUND, &mask));
    expect_mask(&mask, "0-3", __LINE__);
    CHECK(!sp_topo_resolve(&topo, SP_CLUSTER_AUTO, SP_PROFILE_DEFAULT, &mask));
    CHECK(!sp_topo_resolve(&topo, SP_CLUSTER_KEEP, SP_PROFILE_DEFAULT, &mask));

    char summary[SP_TEXT_MAX * 2];
    sp_topo_summary(&topo, summary, sizeof summary);
    CHECK(strstr(summary, "levels=3") != NULL);
    CHECK(strstr(summary, "little=0-3/284") != NULL);
    CHECK(strstr(summary, "prime=7/1024") != NULL);
    sp_fixture_remove(tmp);
}

static void test_two_levels(void) {
    char tmp[512];
    CHECK(sp_fixture_mkdtemp(tmp, sizeof tmp) != NULL);
    char root[1024];
    CHECK(sp_path_join(root, sizeof root, tmp, "sys") == 0);
    const int caps[8] = {512, 512, 512, 512, 1024, 1024, 1024, 1024};
    CHECK(sp_fixture_sysfs(root, caps, NULL, 8, "0-7") == 0);
    struct sp_topology topo;
    char err[256];
    CHECK(sp_topo_discover(&topo, root, err, sizeof err) == 0);
    CHECK(topo.levels == 2);
    CHECK(!topo.degraded);
    expect_mask(&topo.cluster[0], "0-3", __LINE__);
    expect_mask(&topo.cluster[1], "4-7", __LINE__);
    expect_mask(&topo.cluster[2], "4-7", __LINE__);
    CHECK(strstr(topo.warning, "aliases") != NULL);
    cpu_set_t mask;
    CHECK(sp_topo_resolve(&topo, SP_CLUSTER_PERF, SP_PROFILE_DEFAULT, &mask));
    expect_mask(&mask, "4-7", __LINE__);
    sp_fixture_remove(tmp);
}

static void test_single_level(void) {
    char tmp[512];
    CHECK(sp_fixture_mkdtemp(tmp, sizeof tmp) != NULL);
    char root[1024];
    CHECK(sp_path_join(root, sizeof root, tmp, "sys") == 0);
    const int caps[8] = {1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024};
    CHECK(sp_fixture_sysfs(root, caps, NULL, 8, "0-7") == 0);
    struct sp_topology topo;
    char err[256];
    CHECK(sp_topo_discover(&topo, root, err, sizeof err) == 0);
    CHECK(topo.levels == 1);
    CHECK(topo.degraded);
    CHECK(strlen(topo.warning) > 0);
    expect_mask(&topo.cluster[0], "0-7", __LINE__);
    expect_mask(&topo.cluster[1], "0-7", __LINE__);
    expect_mask(&topo.cluster[2], "0-7", __LINE__);
    cpu_set_t mask;
    CHECK(sp_topo_resolve(&topo, SP_CLUSTER_LITTLE, SP_PROFILE_DEFAULT, &mask));
    expect_mask(&mask, "0-7", __LINE__);
    sp_fixture_remove(tmp);
}

static void test_frequency_fallback(void) {
    char tmp[512];
    CHECK(sp_fixture_mkdtemp(tmp, sizeof tmp) != NULL);
    char root[1024];
    CHECK(sp_path_join(root, sizeof root, tmp, "sys") == 0);
    const int caps[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    const int freqs[8] = {1000000, 1000000, 1000000, 1000000,
                          1800000, 1800000, 1800000, 2400000};
    CHECK(sp_fixture_sysfs(root, caps, freqs, 8, "0-7") == 0);
    struct sp_topology topo;
    char err[256];
    CHECK(sp_topo_discover(&topo, root, err, sizeof err) == 0);
    CHECK(topo.levels == 3);
    CHECK_STR(topo.source, "cpufreq_max_freq");
    expect_mask(&topo.cluster[0], "0-3", __LINE__);
    expect_mask(&topo.cluster[1], "4-6", __LINE__);
    expect_mask(&topo.cluster[2], "7", __LINE__);
    CHECK(topo.capacity[2] == 2400000);
    sp_fixture_remove(tmp);
}

static void test_missing_data(void) {
    char tmp[512];
    CHECK(sp_fixture_mkdtemp(tmp, sizeof tmp) != NULL);
    char root[1024];
    CHECK(sp_path_join(root, sizeof root, tmp, "sys") == 0);
    const int none[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(sp_fixture_sysfs(root, none, none, 8, "0-7") == 0);
    struct sp_topology topo;
    char err[256];
    CHECK(sp_topo_discover(&topo, root, err, sizeof err) == 0);
    CHECK(topo.degraded);
    CHECK(topo.levels == 1);
    CHECK_STR(topo.source, "none");
    CHECK(strstr(topo.warning, "0 of 8") != NULL);
    expect_mask(&topo.cluster[0], "0-7", __LINE__);
    sp_fixture_remove(tmp);

    /* unreadable topology root: fall back to the kernel's online CPU count */
    struct sp_topology fallback;
    char missing[1024];
    CHECK(sp_path_join(missing, sizeof missing, tmp, "does-not-exist") == 0);
    CHECK(sp_topo_discover(&fallback, missing, err, sizeof err) == 0);
    CHECK(fallback.degraded);
    CHECK_STR(fallback.source, "fallback");
    CHECK(sp_cpuset_any(&fallback.online));
    CHECK(sp_cpuset_any(&fallback.cluster[0]));
}

static void test_offline_cpus(void) {
    char tmp[512];
    CHECK(sp_fixture_mkdtemp(tmp, sizeof tmp) != NULL);
    char root[1024];
    CHECK(sp_path_join(root, sizeof root, tmp, "sys") == 0);
    const int caps[8] = {284, 284, 1024, 871, 1024, 1024, 1024, 1024};
    CHECK(sp_fixture_sysfs(root, caps, NULL, 8, "0,1,3-7") == 0);
    struct sp_topology topo;
    char err[256];
    CHECK(sp_topo_discover(&topo, root, err, sizeof err) == 0);
    CHECK(topo.levels == 3);
    CHECK(!CPU_ISSET(2, &topo.online));
    expect_mask(&topo.cluster[0], "0-1", __LINE__);
    expect_mask(&topo.cluster[1], "3", __LINE__);
    expect_mask(&topo.cluster[2], "4-7", __LINE__);
    sp_fixture_remove(tmp);
}

static void test_partial_data(void) {
    char tmp[512];
    CHECK(sp_fixture_mkdtemp(tmp, sizeof tmp) != NULL);
    char root[1024];
    CHECK(sp_path_join(root, sizeof root, tmp, "sys") == 0);
    const int caps[8] = {284, 871, 0, 0, 0, 0, 0, 0};
    const int none[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(sp_fixture_sysfs(root, caps, none, 8, "0-7") == 0);
    struct sp_topology topo;
    char err[256];
    CHECK(sp_topo_discover(&topo, root, err, sizeof err) == 0);
    CHECK(topo.degraded);
    CHECK(topo.levels == 1);
    CHECK(strstr(topo.warning, "2 of 8") != NULL);
    expect_mask(&topo.cluster[0], "0-7", __LINE__);
    sp_fixture_remove(tmp);
}

int main(void) {
    test_sm8250();
    test_two_levels();
    test_single_level();
    test_frequency_fallback();
    test_missing_data();
    test_offline_cpus();
    test_partial_data();
    return sp_test_report("test_topology");
}
