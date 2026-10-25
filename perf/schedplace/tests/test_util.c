/* test_util — cpu lists, path helpers, write guard. */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "sp_fixture.h"
#include "sp_log.h"
#include "sp_test.h"
#include "sp_util.h"

static void test_cpulist(void) {
    cpu_set_t set;
    CHECK(sp_parse_cpulist("0-3", &set));
    CHECK(sp_cpuset_count(&set) == 4);
    CHECK(CPU_ISSET(0, &set) && CPU_ISSET(3, &set) && !CPU_ISSET(4, &set));
    CHECK(sp_parse_cpulist("0,2,4", &set));
    CHECK(sp_cpuset_count(&set) == 3);
    CHECK(CPU_ISSET(2, &set) && CPU_ISSET(4, &set) && !CPU_ISSET(1, &set));
    CHECK(sp_parse_cpulist("0-1,4-5", &set));
    CHECK(sp_cpuset_count(&set) == 4);
    CHECK(!sp_parse_cpulist("3-1", &set));
    CHECK(!sp_parse_cpulist("a", &set));
    CHECK(!sp_parse_cpulist("", &set));
    CHECK(!sp_parse_cpulist("0-2000", &set));
    CHECK(!sp_parse_cpulist("1,,2", &set));

    CPU_ZERO(&set);
    CPU_SET(0, &set);
    CPU_SET(1, &set);
    CPU_SET(2, &set);
    CPU_SET(3, &set);
    CPU_SET(6, &set);
    CPU_SET(8, &set);
    CPU_SET(9, &set);
    char text[SP_TEXT_MAX];
    CHECK(sp_format_cpulist(&set, text, sizeof text));
    CHECK_STR(text, "0-3,6,8-9");
    cpu_set_t round_trip;
    CHECK(sp_parse_cpulist(text, &round_trip));
    CHECK(sp_cpuset_equal(&set, &round_trip));
    CPU_ZERO(&set);
    CHECK(!sp_format_cpulist(&set, text, sizeof text));

    cpu_set_t a, b;
    CHECK(sp_parse_cpulist("0-3", &a));
    CHECK(sp_parse_cpulist("2-5", &b));
    cpu_set_t result;
    sp_cpuset_and(&result, &a, &b);
    CHECK(sp_cpuset_count(&result) == 2);
    CHECK(CPU_ISSET(2, &result) && CPU_ISSET(3, &result));
    sp_cpuset_or(&result, &a, &b);
    CHECK(sp_cpuset_count(&result) == 6);
    CHECK(sp_cpuset_subset(&a, &result));
    CHECK(!sp_cpuset_subset(&b, &a));
    CHECK(!sp_cpuset_equal(&a, &b));
    CHECK(sp_cpuset_any(&a));
    CPU_ZERO(&result);
    CHECK(!sp_cpuset_any(&result));
}

static void test_strings(void) {
    char text[64];
    CHECK_STR(sp_trim(strcpy(text, "   abc  ")), "abc");
    CHECK(sp_parse_long("12", NULL));
    long value = 0;
    CHECK(sp_parse_long("-20", &value) && value == -20);
    CHECK(!sp_parse_long("12x", &value));
    CHECK(!sp_parse_long("", &value));
    bool flag = false;
    CHECK(sp_parse_bool("yes", &flag) && flag);
    CHECK(sp_parse_bool("OFF", &flag) && !flag);
    CHECK(!sp_parse_bool("maybe", &flag));
    bool ok = false;
    double parsed = sp_parse_double("12.5", &ok);
    CHECK(ok && parsed > 12.4 && parsed < 12.6);
    CHECK(!sp_parse_double("nan", &ok));
    CHECK(!sp_parse_double("abc", &ok));
    char path[128];
    CHECK(sp_path_join(path, sizeof path, "/a", "b") == 0);
    CHECK_STR(path, "/a/b");
    CHECK(sp_path_join(path, sizeof path, "/a/", "b") == 0);
    CHECK_STR(path, "/a/b");
    CHECK(sp_path_join(path, sizeof path, "/a", "") == 0);
    CHECK_STR(path, "/a");
    char small[4];
    CHECK(sp_path_join(small, sizeof small, "/aaaa", "bbbb") != 0);
    char base[32];
    CHECK(sp_path_basename("/sys/fs/cgroup/x/cpuset.cpus", base, sizeof base) == 0);
    CHECK_STR(base, "cpuset.cpus");
    CHECK(sp_streq("a", "a") && !sp_streq("a", "b"));
    CHECK(sp_ends_with("abc/task", "/task") && !sp_ends_with("abc", "/task"));
    CHECK(sp_has_space("a b") && !sp_has_space("ab"));
}

static void test_write_guard(void) {
    char root[512];
    CHECK(sp_fixture_dir_for_write(root, sizeof root) != NULL);
    char path[1024];
    CHECK(sp_path_join(path, sizeof path, root, "cpuset.cpus") == 0);
    CHECK(sp_fixture_write(root, "cpuset.cpus", "0-3") == 0);
    struct sp_stats stats;
    memset(&stats, 0, sizeof stats);
    CHECK(sp_safe_write(path, "0-1", false, &stats) == 0);
    char text[64];
    CHECK(sp_fixture_read(root, "cpuset.cpus", text, sizeof text) >= 0);
    CHECK_STR(text, "0-1");

    char governor[1024];
    CHECK(sp_path_join(governor, sizeof governor, root, "scaling_governor") == 0);
    CHECK(sp_fixture_write(root, "scaling_governor", "schedutil") == 0);
    errno = 0;
    CHECK(sp_safe_write(governor, "performance", false, &stats) == -1);
    CHECK(errno == EPERM);
    CHECK(stats.refused_writes == 1);
    CHECK(sp_fixture_read(root, "scaling_governor", text, sizeof text) >= 0);
    CHECK_STR(text, "schedutil");

    char online[1024];
    CHECK(sp_path_join(online, sizeof online, root, "online") == 0);
    CHECK(sp_fixture_write(root, "online", "1") == 0);
    CHECK(sp_safe_write(online, "0", false, &stats) == -1);
    CHECK(stats.refused_writes == 2);

    char max_freq[1024];
    CHECK(sp_path_join(max_freq, sizeof max_freq, root, "scaling_max_freq") == 0);
    CHECK(sp_fixture_write(root, "scaling_max_freq", "1000000") == 0);
    CHECK(sp_safe_write(max_freq, "2000000", false, &stats) == -1);

    /* the explicit legacy exception may write the governor, nothing else */
    CHECK(sp_safe_write(governor, "performance", true, &stats) == 0);
    CHECK(sp_fixture_read(root, "scaling_governor", text, sizeof text) >= 0);
    CHECK_STR(text, "performance");
    CHECK(sp_safe_write(online, "0", true, &stats) == -1);

    /* a missing allowlisted node must fail instead of being created */
    char missing[1024];
    CHECK(sp_path_join(missing, sizeof missing, root, "cgroup.threads") == 0);
    errno = 0;
    CHECK(sp_safe_write(missing, "1", false, &stats) == -1);
    CHECK(errno == ENOENT);
    CHECK(sp_fixture_read(root, "cgroup.threads", text, sizeof text) == -1);

    CHECK(sp_path_is_forbidden("/sys/devices/system/cpu/cpufreq/policy0/scaling_governor"));
    CHECK(sp_path_is_forbidden("/sys/devices/system/cpu/cpu3/online"));
    CHECK(sp_path_is_forbidden("/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq"));
    CHECK(sp_path_is_forbidden("/proc/sys/kernel/sched_rt_runtime_us"));
    CHECK(sp_path_is_forbidden("/sys/kernel/sched_boost"));
    CHECK(sp_path_is_forbidden("/sys/devices/system/cpu/cpu0/core_ctl"));
    CHECK(!sp_path_is_forbidden("/sys/fs/cgroup/schedplace/x/cpuset.cpus"));
    CHECK(!sp_path_is_forbidden("/sys/fs/cgroup/schedplace/x/cpuset.mems"));
    CHECK(!sp_path_is_forbidden("/sys/fs/cgroup/schedplace/x/cgroup.threads"));
    CHECK(sp_path_is_forbidden("/tmp/whatever"));
    CHECK(sp_path_classify("/sys/devices/system/cpu/cpu3/online") == SP_FORBID_CPU_ONLINE);
    CHECK(sp_path_classify("/a/b/scaling_governor") == SP_FORBID_GOVERNOR);
    CHECK(strlen(sp_forbidden_reason(SP_FORBID_GOVERNOR)) > 10);
    sp_fixture_remove(root);
}

int main(void) {
    test_cpulist();
    test_strings();
    test_write_guard();
    return sp_test_report("test_util");
}
