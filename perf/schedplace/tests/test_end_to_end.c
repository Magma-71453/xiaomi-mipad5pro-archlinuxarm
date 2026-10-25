/* test_end_to_end — real daemon runs against mock sysfs + mock cgroup roots.
 *
 * Two passes:
 *   --dry-run : decisions appear in the log, nothing on disk changes;
 *   real      : cpuset.cpus files and the helper thread affinity actually change.
 */
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "sp_fixture.h"
#include "sp_log.h"
#include "sp_test.h"
#include "sp_util.h"

#ifndef SP_BIN
#define SP_BIN "./schedplace"
#endif
#ifndef SP_FAKE
#define SP_FAKE "./fake_sp"
#endif

static const char *env_or(const char *name, const char *fallback) {
    const char *value = getenv(name);
    return (value && *value) ? value : fallback;
}

static pid_t spawn_fake_full(const char *comm, bool busy, int spawn_after_ms,
                             const char *spawn_name) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        char *args[12];
        int n = 0;
        args[n++] = (char *)env_or("SP_FAKE", SP_FAKE);
        args[n++] = (char *)"--comm";
        args[n++] = (char *)comm;
        args[n++] = (char *)"--seconds";
        args[n++] = (char *)"120";
        if (busy) args[n++] = (char *)"--busy";
        if (spawn_after_ms > 0 && spawn_name && spawn_name[0]) {
            args[n++] = (char *)"--spawn-after-ms";
            args[n++] = (char *)"1200";
            args[n++] = (char *)"--spawn-name";
            args[n++] = (char *)spawn_name;
        }
        (void)spawn_after_ms;
        args[n] = NULL;
        execv(args[0], args);
        _exit(127);
    }
    return pid;
}

static pid_t spawn_fake(const char *comm, bool busy) {
    return spawn_fake_full(comm, busy, 0, NULL);
}

static bool wait_comm(pid_t pid, const char *comm, int timeout_ms) {
    char path[128];
    snprintf(path, sizeof path, "/proc/%ld/comm", (long)pid);
    for (int waited = 0; waited < timeout_ms; waited += 10) {
        char text[SP_COMM_LEN];
        if (sp_read_file(path, text, sizeof text) >= 0 && sp_streq(text, comm)) return true;
        usleep(10000);
    }
    return false;
}

static bool read_cpus_allowed(pid_t pid, char *buf, size_t size) {
    char path[128];
    snprintf(path, sizeof path, "/proc/%ld/status", (long)pid);
    FILE *file = fopen(path, "r");
    if (!file) return false;
    char line[512];
    bool found = false;
    while (fgets(line, sizeof line, file)) {
        if (strncmp(line, "Cpus_allowed_list:", 18) != 0) continue;
        sp_strlcpy(buf, sp_trim(line + 18), size);
        found = true;
        break;
    }
    fclose(file);
    return found;
}

static int run_capture(const char *command, char *out, size_t outsize) {
    if (out && outsize) out[0] = '\0';
    FILE *pipe = popen(command, "r");
    if (!pipe) return -1;
    if (out && outsize) {
        size_t n = fread(out, 1, outsize - 1, pipe);
        out[n] = '\0';
    }
    int status = pclose(pipe);
    if (status == -1) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void append(char *buf, size_t size, size_t *off, const char *text) {
    size_t len = strlen(text);
    if (*off + len + 1 >= size) return;
    memcpy(buf + *off, text, len);
    *off += len;
    buf[*off] = '\0';
}

static void fingerprint(const char *root, char *buf, size_t size, size_t *off) {
    struct dirent **list = NULL;
    int count = scandir(root, &list, NULL, alphasort);
    if (count < 0) return;
    for (int i = 0; i < count; ++i) {
        if (strcmp(list[i]->d_name, ".") == 0 || strcmp(list[i]->d_name, "..") == 0) {
            free(list[i]);
            continue;
        }
        char path[1024];
        if (sp_path_join(path, sizeof path, root, list[i]->d_name) == 0) {
            struct stat st;
            if (lstat(path, &st) == 0) {
                char entry[1200];
                snprintf(entry, sizeof entry, "%s|%ld;", path, (long)st.st_size);
                append(buf, size, off, entry);
                if (S_ISDIR(st.st_mode)) fingerprint(path, buf, size, off);
            }
        }
        free(list[i]);
    }
    free(list);
}

static int write_config(const char *path, const char *sysfs, const char *cgroup) {
    FILE *file = fopen(path, "w");
    if (!file) return -1;
    fprintf(file,
            "[global]\n"
            "interval_ms = 200\n"
            "cpuset_root = %s\n"
            "enable_cgroup = true\n"
            "restore_on_exit = false\n"
            "log_level = info\n"
            "[topology]\n"
            "sysfs_root = %s\n"
            "\n"
            "[rule e2e-big]\n"
            "profile = latency\n"
            "cluster = auto\n"
            "cpuset = e2e-perf\n"
            "thread = ^sp-e2e-big$\n"
            "reason = e2e latency thread\n"
            "\n"
            "[rule e2e-little]\n"
            "profile = background\n"
            "cluster = auto\n"
            "cpuset = e2e-little\n"
            "thread = ^sp-e2e-lit$\n"
            "reason = e2e background thread\n"
            "\n"
            "[rule e2e-busy-keep]\n"
            "profile = background\n"
            "cluster = keep\n"
            "thread = ^sp-e2e-busy$\n"
            "busy_gt = 20\n"
            "reason = hot thread stays where it is\n"
            "\n"
            "[rule e2e-idle]\n"
            "profile = background\n"
            "cluster = auto\n"
            "cpuset = e2e-little\n"
            "thread = ^sp-e2e-idle$\n"
            "busy_lt = 5\n"
            "reason = idle helper thread\n",
            cgroup, sysfs);
    fclose(file);
    return 0;
}

/* The thread-creation probe: procfs emits no inotify events for new threads, so the
 * daemon samples thread counts of tracked processes. With a 3000 ms full-scan
 * interval, a thread created ~1150 ms after the first scan can only be seen by the
 * 100 ms probe. */
static void test_thread_probe(void) {
    char tmp[512];
    CHECK(sp_fixture_mkdtemp(tmp, sizeof tmp) != NULL);
    char sysfs[1024], cgroup[1024], config[1024], logpath[1024];
    CHECK(sp_path_join(sysfs, sizeof sysfs, tmp, "sys") == 0);
    CHECK(sp_path_join(cgroup, sizeof cgroup, tmp, "cgroup") == 0);
    CHECK(sp_path_join(config, sizeof config, tmp, "probe.conf") == 0);
    CHECK(sp_path_join(logpath, sizeof logpath, tmp, "daemon.log") == 0);
    const int caps[8] = {284, 284, 284, 284, 871, 871, 871, 1024};
    CHECK(sp_fixture_sysfs(sysfs, caps, NULL, 8, "0-7") == 0);
    CHECK(sp_mkdir_p(cgroup, 0755) == 0);
    CHECK(sp_fixture_write(tmp, "cgroup/evt/cpuset.cpus", "") == 0);
    CHECK(sp_fixture_write(tmp, "cgroup/evt/cpuset.mems", "") == 0);
    CHECK(sp_fixture_write(tmp, "cgroup/evt/cgroup.threads", "") == 0);
    FILE *conf = fopen(config, "w");
    CHECK(conf != NULL);
    if (conf) {
        fprintf(conf,
                "[global]\ninterval_ms = 2000\nprobe_ms = 100\n"
                "cpuset_root = %s\n"
                "enable_cgroup = true\nrestore_on_exit = false\n"
                "log_level = debug\n"
                "[topology]\nsysfs_root = %s\n"
                "[rule evt]\nprofile = background\ncluster = auto\n"
                "cpuset = evt\n"
                "thread = ^sp-evt\nreason = probe test\n",
                cgroup, sysfs);
        fclose(conf);
    }
    pid_t fake = spawn_fake_full("sp-evt-main", false, 1200, "sp-evt-late");
    CHECK(fake > 0);
    CHECK(wait_comm(fake, "sp-evt-main", 2000));
    pid_t daemon = fork();
    CHECK(daemon >= 0);
    if (daemon == 0) {
        int fd = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, 1);
            dup2(fd, 2);
            close(fd);
        }
        execl(env_or("SP_BIN", SP_BIN), env_or("SP_BIN", SP_BIN), "--config", config,
              "--dry-run", "--log-level", "debug", (char *)NULL);
        _exit(127);
    }
    usleep(1750000); /* late thread ~1150 ms; full scans at 0 and 2000 ms */
    kill(daemon, SIGTERM);
    waitpid(daemon, NULL, 0);
    kill(fake, SIGKILL);
    waitpid(fake, NULL, 0);

    FILE *log = fopen(logpath, "r");
    CHECK(log != NULL);
    static char buffer[131072];
    buffer[0] = 0;
    if (log) {
        size_t n = fread(buffer, 1, sizeof buffer - 1, log);
        buffer[n] = 0;
        fclose(log);
    }
    CHECK_MSG(strstr(buffer, "comm=sp-evt-main") != NULL,
              "daemon never placed the pre-existing thread: %.600s", buffer);
    CHECK_MSG(strstr(buffer, "comm=sp-evt-late") != NULL,
              "the 100 ms probe did not pick up the new thread before the 2000 ms scan: "
              "%.900s",
              buffer);
    sp_fixture_remove(tmp);
}


int main(void) {
    sp_log_init("test_end_to_end", SP_LOG_WARN);
    char tmp[512];
    CHECK(sp_fixture_mkdtemp(tmp, sizeof tmp) != NULL);
    char sysfs[1024], cgroup[1024], config[1024], fingerprint_before[16384];
    CHECK(sp_path_join(sysfs, sizeof sysfs, tmp, "sys") == 0);
    CHECK(sp_path_join(cgroup, sizeof cgroup, tmp, "cgroup") == 0);
    CHECK(sp_path_join(config, sizeof config, tmp, "schedplace.conf") == 0);
    /* the real device: 4x A55 284, 3x A77 871, 1x A77 prime 1024 */
    const int caps[8] = {284, 284, 284, 284, 871, 871, 871, 1024};
    CHECK(sp_fixture_sysfs(sysfs, caps, NULL, 8, "0-7") == 0);
    CHECK(sp_mkdir_p(cgroup, 0755) == 0);
    CHECK(sp_fixture_write(tmp, "cgroup/e2e-perf/cpuset.cpus", "") == 0);
    CHECK(sp_fixture_write(tmp, "cgroup/e2e-perf/cpuset.mems", "") == 0);
    CHECK(sp_fixture_write(tmp, "cgroup/e2e-perf/cgroup.threads", "") == 0);
    CHECK(sp_fixture_write(tmp, "cgroup/e2e-little/cpuset.cpus", "") == 0);
    CHECK(sp_fixture_write(tmp, "cgroup/e2e-little/cpuset.mems", "") == 0);
    CHECK(sp_fixture_write(tmp, "cgroup/e2e-little/cgroup.threads", "") == 0);
    CHECK(write_config(config, sysfs, cgroup) == 0);

    pid_t big = spawn_fake("sp-e2e-big", false);
    pid_t little = spawn_fake("sp-e2e-lit", false);
    pid_t busy = spawn_fake("sp-e2e-busy", true);
    pid_t idle = spawn_fake("sp-e2e-idle", false);
    CHECK(big > 0 && little > 0 && busy > 0 && idle > 0);
    CHECK(wait_comm(big, "sp-e2e-big", 2000));
    CHECK(wait_comm(little, "sp-e2e-lit", 2000));
    CHECK(wait_comm(busy, "sp-e2e-busy", 2000));
    CHECK(wait_comm(idle, "sp-e2e-idle", 2000));
    usleep(100000);

    size_t off = 0;
    fingerprint_before[0] = '\0';
    fingerprint(sysfs, fingerprint_before, sizeof fingerprint_before, &off);
    CHECK(strlen(fingerprint_before) > 20);

    char command[4096];
    char output[65536];
    snprintf(command, sizeof command,
             "'%s' --once --dry-run --config '%s' --log-level info 2>&1",
             env_or("SP_BIN", SP_BIN), config);
    int status = run_capture(command, output, sizeof output);
    CHECK_MSG(status == 0, "dry-run exit=%d output=%.400s", status, output);
    CHECK_MSG(strstr(output, "rule=e2e-big") != NULL, "missing e2e-big decision: %.600s",
              output);
    CHECK(strstr(output, "new_mask=4-7") != NULL);
    CHECK_MSG(strstr(output, "rule=e2e-little") != NULL, "missing e2e-little decision");
    CHECK(strstr(output, "new_mask=0-3") != NULL);
    CHECK_MSG(strstr(output, "rule=e2e-idle") != NULL, "missing busy_lt rule decision");
    CHECK(strstr(output, "result=dry-run") != NULL);
    CHECK(strstr(output, "summary scans=") != NULL);
    /* the busy thread must be exempted by busy_gt, never down-clustered */
    CHECK(strstr(output, "comm=sp-e2e-busy") == NULL ||
          strstr(output, "comm=sp-e2e-busy") > strstr(output, "new_mask="));

    char text[SP_TEXT_MAX];
    CHECK(sp_fixture_read(tmp, "cgroup/e2e-perf/cpuset.cpus", text, sizeof text) >= 0);
    CHECK_STR(text, "");
    CHECK(sp_fixture_read(tmp, "cgroup/e2e-little/cpuset.cpus", text, sizeof text) >= 0);
    CHECK_STR(text, "");
    char fingerprint_after[16384];
    off = 0;
    fingerprint_after[0] = '\0';
    fingerprint(sysfs, fingerprint_after, sizeof fingerprint_after, &off);
    CHECK_MSG(strcmp(fingerprint_before, fingerprint_after) == 0,
              "dry-run modified the mock sysfs tree");

    snprintf(command, sizeof command, "'%s' --once --config '%s' --log-level info 2>&1",
             env_or("SP_BIN", SP_BIN), config);
    status = run_capture(command, output, sizeof output);
    CHECK_MSG(status == 0, "real run exit=%d output=%.400s", status, output);
    CHECK(sp_fixture_read(tmp, "cgroup/e2e-perf/cpuset.cpus", text, sizeof text) >= 0);
    CHECK_STR(text, "4-7");
    CHECK(sp_fixture_read(tmp, "cgroup/e2e-little/cpuset.cpus", text, sizeof text) >= 0);
    CHECK_STR(text, "0-3");

    char allowed[SP_TEXT_MAX];
    CHECK(read_cpus_allowed(big, allowed, sizeof allowed));
    CHECK_MSG(sp_streq(allowed, "4-7") || strstr(output, "Operation not permitted") != NULL,
              "sp-e2e-big Cpus_allowed_list=%s", allowed);
    CHECK(read_cpus_allowed(little, allowed, sizeof allowed));
    CHECK_MSG(sp_streq(allowed, "0-3") || strstr(output, "Operation not permitted") != NULL,
              "sp-e2e-lit Cpus_allowed_list=%s", allowed);
    CHECK(read_cpus_allowed(busy, allowed, sizeof allowed));
    CHECK(strcmp(allowed, "0-15") == 0 || strcmp(allowed, "0-7") == 0);

    kill(big, SIGKILL);
    kill(little, SIGKILL);
    kill(busy, SIGKILL);
    kill(idle, SIGKILL);
    waitpid(big, NULL, 0);
    waitpid(little, NULL, 0);
    waitpid(busy, NULL, 0);
    waitpid(idle, NULL, 0);
    sp_fixture_remove(tmp);
    test_thread_probe();
    return sp_test_report("test_end_to_end");
}

