/* schedplace — /proc scanner. */
#include "sp_proc.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SP_MAX_SCAN_THREADS 20000

struct sp_proc_entry {
    pid_t pid;
    unsigned long long starttime;
    uid_t uid;
    char comm[SP_COMM_LEN];
    char exe[SP_PATH_MAX];
    char cgroup[SP_PATH_MAX];
    unsigned long long seen;
};

struct sp_busy_sample {
    pid_t tid;
    unsigned long long starttime;
    unsigned long long ticks;
    long long ms;
    unsigned long long seen;
};

struct sp_scanner {
    struct sp_proc_entry *procs;
    size_t nprocs;
    size_t pcap;
    struct sp_proc_entry *next;
    size_t nnext;
    size_t ncap;
    struct sp_busy_sample *samples;
    size_t nsamples;
    size_t scap;
    long hz;
    unsigned long long pass;
};

long long sp_scanner_now_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

int sp_scanner_init(struct sp_scanner **out) {
    if (!out) return -1;
    struct sp_scanner *scanner = calloc(1, sizeof *scanner);
    if (!scanner) return -1;
    scanner->hz = sysconf(_SC_CLK_TCK);
    if (scanner->hz <= 0) scanner->hz = 100;
    *out = scanner;
    return 0;
}

void sp_scanner_free(struct sp_scanner *scanner) {
    if (!scanner) return;
    free(scanner->procs);
    free(scanner->next);
    free(scanner->samples);
    free(scanner);
}

static int read_comm(pid_t pid, char *buf, size_t size) {
    char path[SP_PATH_MAX];
    char dir[32];
    snprintf(dir, sizeof dir, "%ld", (long)pid);
    char base[SP_PATH_MAX];
    if (sp_path_join(base, sizeof base, "/proc", dir) != 0) return -1;
    if (sp_path_join(path, sizeof path, base, "comm") != 0) return -1;
    if (sp_read_file(path, buf, size) < 0) return -1;
    return 0;
}

static int read_thread_stat(const char *stat_path, unsigned long long *starttime,
                            unsigned long long *utime, unsigned long long *stime) {
    char text[4096];
    int len = sp_read_file(stat_path, text, sizeof text);
    if (len < 0) return -1;
    char *close = strrchr(text, ')');
    if (!close) return -1;
    char *p = close + 1;
    unsigned long long parsed_start = 0;
    unsigned long long parsed_utime = 0;
    unsigned long long parsed_stime = 0;
    for (int field = 3; field <= 22; ++field) {
        while (*p && isspace((unsigned char)*p)) ++p;
        if (!*p) return -1;
        char *end = p;
        while (*end && !isspace((unsigned char)*end)) ++end;
        char saved = *end;
        *end = '\0';
        if (field == 14) parsed_utime = strtoull(p, NULL, 10);
        else if (field == 15) parsed_stime = strtoull(p, NULL, 10);
        else if (field == 22) parsed_start = strtoull(p, NULL, 10);
        *end = saved;
        p = end;
    }
    if (!parsed_start) return -1;
    if (starttime) *starttime = parsed_start;
    if (utime) *utime = parsed_utime;
    if (stime) *stime = parsed_stime;
    return 0;
}

static int read_uid(pid_t pid, uid_t *out) {
    char path[SP_PATH_MAX];
    char dir[32];
    snprintf(dir, sizeof dir, "%ld", (long)pid);
    if (sp_path_join(path, sizeof path, "/proc", dir) != 0) return -1;
    char status_path[SP_PATH_MAX];
    if (sp_path_join(status_path, sizeof status_path, path, "status") != 0) return -1;
    FILE *file = fopen(status_path, "r");
    if (!file) return -1;
    char line[256];
    int rc = -1;
    while (fgets(line, sizeof line, file)) {
        if (strncmp(line, "Uid:", 4) != 0) continue;
        unsigned long real = 0;
        if (sscanf(line + 4, "%lu", &real) == 1) {
            *out = (uid_t)real;
            rc = 0;
        }
        break;
    }
    fclose(file);
    return rc;
}

static void read_exe(pid_t pid, char *buf, size_t size) {
    char path[SP_PATH_MAX];
    char dir[32];
    buf[0] = '\0';
    snprintf(dir, sizeof dir, "%ld", (long)pid);
    if (sp_path_join(path, sizeof path, "/proc", dir) != 0) return;
    char exe_path[SP_PATH_MAX];
    if (sp_path_join(exe_path, sizeof exe_path, path, "exe") != 0) return;
    sp_read_link(exe_path, buf, size);
}

static void read_cgroup(pid_t pid, char *buf, size_t size) {
    char path[SP_PATH_MAX];
    char dir[32];
    buf[0] = '\0';
    snprintf(dir, sizeof dir, "%ld", (long)pid);
    if (sp_path_join(path, sizeof path, "/proc", dir) != 0) return;
    char cgroup_path[SP_PATH_MAX];
    if (sp_path_join(cgroup_path, sizeof cgroup_path, path, "cgroup") != 0) return;
    char text[SP_PATH_MAX];
    if (sp_read_file(cgroup_path, text, sizeof text) < 0) return;
    /* cgroup v2 unified hierarchy: "0::/user.slice/..." */
    const char *sep = strstr(text, "::");
    if (sep) {
        sp_strlcpy(buf, sep + 2, size);
        return;
    }
    /* cgroup v1 fallback: take the first non-empty path */
    const char *colon = strchr(text, ':');
    if (colon) {
        const char *second = strchr(colon + 1, ':');
        if (second) sp_strlcpy(buf, second + 1, size);
    }
}

static int cmp_pid(const void *a, const void *b) {
    pid_t x = *(const pid_t *)a;
    pid_t y = *(const pid_t *)b;
    return (x > y) - (x < y);
}

static bool numeric_name(const char *name, pid_t *out) {
    if (!*name) return false;
    for (const char *p = name; *p; ++p)
        if (!isdigit((unsigned char)*p)) return false;
    long value = strtol(name, NULL, 10);
    if (value <= 0) return false;
    if (out) *out = (pid_t)value;
    return true;
}

static int collect_pids(pid_t **out, size_t *count) {
    DIR *dir = opendir("/proc");
    if (!dir) return -1;
    size_t cap = 256;
    size_t used = 0;
    pid_t *pids = malloc(cap * sizeof *pids);
    if (!pids) {
        closedir(dir);
        return -1;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        pid_t pid = 0;
        if (!numeric_name(entry->d_name, &pid)) continue;
        if (used == cap) {
            size_t grown = cap * 2;
            pid_t *resized = realloc(pids, grown * sizeof *pids);
            if (!resized) {
                free(pids);
                closedir(dir);
                return -1;
            }
            pids = resized;
            cap = grown;
        }
        pids[used++] = pid;
    }
    closedir(dir);
    qsort(pids, used, sizeof *pids, cmp_pid);
    *out = pids;
    *count = used;
    return 0;
}

static struct sp_proc_entry *proc_lookup(struct sp_scanner *scanner, pid_t pid) {
    size_t low = 0;
    size_t high = scanner->nprocs;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (scanner->procs[mid].pid < pid) low = mid + 1;
        else high = mid;
    }
    if (low < scanner->nprocs && scanner->procs[low].pid == pid) return &scanner->procs[low];
    return NULL;
}

static struct sp_proc_entry *proc_push(struct sp_scanner *scanner) {
    if (scanner->nnext == scanner->ncap) {
        size_t grown = scanner->ncap ? scanner->ncap * 2 : 256;
        struct sp_proc_entry *resized = realloc(scanner->next, grown * sizeof *resized);
        if (!resized) return NULL;
        scanner->next = resized;
        scanner->ncap = grown;
    }
    struct sp_proc_entry *entry = &scanner->next[scanner->nnext++];
    memset(entry, 0, sizeof *entry);
    return entry;
}

static struct sp_busy_sample *sample_lookup(struct sp_scanner *scanner, pid_t tid) {
    size_t low = 0;
    size_t high = scanner->nsamples;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (scanner->samples[mid].tid < tid) low = mid + 1;
        else high = mid;
    }
    if (low < scanner->nsamples && scanner->samples[low].tid == tid)
        return &scanner->samples[low];
    return NULL;
}

static struct sp_busy_sample *sample_insert(struct sp_scanner *scanner, pid_t tid) {
    size_t low = 0;
    size_t high = scanner->nsamples;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (scanner->samples[mid].tid < tid) low = mid + 1;
        else high = mid;
    }
    if (low < scanner->nsamples && scanner->samples[low].tid == tid)
        return &scanner->samples[low];
    if (scanner->nsamples == scanner->scap) {
        size_t grown = scanner->scap ? scanner->scap * 2 : 256;
        struct sp_busy_sample *resized =
            realloc(scanner->samples, grown * sizeof *resized);
        if (!resized) return NULL;
        scanner->samples = resized;
        scanner->scap = grown;
    }
    if (low < scanner->nsamples)
        memmove(&scanner->samples[low + 1], &scanner->samples[low],
                (scanner->nsamples - low) * sizeof scanner->samples[0]);
    ++scanner->nsamples;
    memset(&scanner->samples[low], 0, sizeof scanner->samples[0]);
    scanner->samples[low].tid = tid;
    return &scanner->samples[low];
}

static double compute_busy(struct sp_scanner *scanner, pid_t tid,
                           unsigned long long starttime, unsigned long long ticks,
                           bool *known) {
    *known = false;
    struct sp_busy_sample *sample = sample_lookup(scanner, tid);
    long long now = sp_scanner_now_ms();
    double busy = 0.0;
    if (sample && sample->starttime == starttime && sample->ticks <= ticks &&
        now > sample->ms) {
        double delta_ticks = (double)(ticks - sample->ticks);
        double delta_seconds = (double)(now - sample->ms) / 1000.0;
        if (delta_seconds > 0.0) {
            busy = delta_ticks / ((double)scanner->hz * delta_seconds) * 100.0;
            if (busy < 0.0) busy = 0.0;
            *known = true;
        }
    }
    if (!sample) sample = sample_insert(scanner, tid);
    if (sample) {
        sample->starttime = starttime;
        sample->ticks = ticks;
        sample->ms = now;
        sample->seen = scanner->pass;
    }
    return busy;
}

static int enumerate_threads(struct sp_scanner *scanner, const struct sp_proc_info *proc,
                             bool need_busy, sp_thread_cb cb, void *ctx, long *delivered,
                             struct sp_stats *stats) {
    char base[SP_PATH_MAX];
    char dir[32];
    snprintf(dir, sizeof dir, "%ld", (long)proc->pid);
    if (sp_path_join(base, sizeof base, "/proc", dir) != 0) return 0;
    char task_path[SP_PATH_MAX];
    if (sp_path_join(task_path, sizeof task_path, base, "task") != 0) return 0;
    DIR *dirp = opendir(task_path);
    if (!dirp) return 0;
    struct dirent *entry;
    while ((entry = readdir(dirp)) != NULL) {
        pid_t tid = 0;
        if (!numeric_name(entry->d_name, &tid)) continue;
        if (*delivered >= SP_MAX_SCAN_THREADS) break;
        char tid_dir[32];
        snprintf(tid_dir, sizeof tid_dir, "%ld", (long)tid);
        char thread_dir[SP_PATH_MAX];
        if (sp_path_join(thread_dir, sizeof thread_dir, task_path, tid_dir) != 0) continue;
        char stat_path[SP_PATH_MAX];
        if (sp_path_join(stat_path, sizeof stat_path, thread_dir, "stat") != 0) continue;
        unsigned long long starttime = 0;
        unsigned long long utime = 0;
        unsigned long long stime = 0;
        if (read_thread_stat(stat_path, &starttime, &utime, &stime) != 0) continue;
        char comm_path[SP_PATH_MAX];
        if (sp_path_join(comm_path, sizeof comm_path, thread_dir, "comm") != 0) continue;
        char tcomm[SP_COMM_LEN];
        if (sp_read_file(comm_path, tcomm, sizeof tcomm) < 0) continue;

        struct sp_thread_info info;
        memset(&info, 0, sizeof info);
        info.pid = proc->pid;
        info.tid = tid;
        info.starttime = starttime;
        info.proc = proc;
        sp_strlcpy(info.comm, tcomm, sizeof info.comm);
        bool known = false;
        double busy = 0.0;
        if (need_busy) busy = compute_busy(scanner, tid, starttime, utime + stime, &known);
        else {
            struct sp_busy_sample *sample = sample_lookup(scanner, tid);
            if (sample) sample->seen = scanner->pass;
        }
        info.busy_pct = busy;
        info.busy_known = known;
        CPU_ZERO(&info.affinity);
        info.affinity_valid = sched_getaffinity(tid, sizeof info.affinity, &info.affinity) == 0;
        if (stats) ++stats->threads_seen;
        ++*delivered;
        if (cb && cb(ctx, proc, &info) != 0) {
            closedir(dirp);
            return -1;
        }
    }
    closedir(dirp);
    return 0;
}

long sp_scanner_scan(struct sp_scanner *scanner, const struct sp_ruleset *rules,
                     bool need_busy, sp_thread_cb cb, void *ctx, struct sp_stats *stats,
                     char *err, size_t errlen) {
    if (!scanner || !rules) {
        if (err) snprintf(err, errlen, "scanner or rules missing");
        return -1;
    }
    pid_t *pids = NULL;
    size_t pid_count = 0;
    if (collect_pids(&pids, &pid_count) != 0) {
        if (err) snprintf(err, errlen, "cannot enumerate /proc: %s", strerror(errno));
        return -1;
    }
    ++scanner->pass;
    scanner->nnext = 0;
    if (stats) ++stats->scans;
    for (size_t i = 0; i < pid_count; ++i) {
        pid_t pid = pids[i];
        char dir[32];
        snprintf(dir, sizeof dir, "%ld", (long)pid);
        char proc_dir[SP_PATH_MAX];
        if (sp_path_join(proc_dir, sizeof proc_dir, "/proc", dir) != 0) continue;
        char stat_path[SP_PATH_MAX];
        if (sp_path_join(stat_path, sizeof stat_path, proc_dir, "stat") != 0) continue;
        unsigned long long starttime = 0;
        if (read_thread_stat(stat_path, &starttime, NULL, NULL) != 0) continue;
        char comm[SP_COMM_LEN];
        if (read_comm(pid, comm, sizeof comm) != 0) continue;
        struct sp_proc_entry *cached = proc_lookup(scanner, pid);
        struct sp_proc_entry *entry = proc_push(scanner);
        if (!entry) break;
        entry->pid = pid;
        entry->starttime = starttime;
        sp_strlcpy(entry->comm, comm, sizeof entry->comm);
        entry->seen = scanner->pass;
        if (cached && cached->starttime == starttime) {
            entry->uid = cached->uid;
            sp_strlcpy(entry->exe, cached->exe, sizeof entry->exe);
            sp_strlcpy(entry->cgroup, cached->cgroup, sizeof entry->cgroup);
        } else {
            entry->uid = (uid_t)-1;
            read_uid(pid, &entry->uid);
            read_exe(pid, entry->exe, sizeof entry->exe);
            if (rules->needs_cgroup) read_cgroup(pid, entry->cgroup, sizeof entry->cgroup);
        }
        if (stats) ++stats->processes_seen;
    }
    free(pids);
    if (scanner->nnext == 0) {
        if (err) snprintf(err, errlen, "/proc produced no readable processes");
        return -1;
    }
    /* swap the freshly built list in; the old buffer becomes the scratch area */
    struct sp_proc_entry *old_procs = scanner->procs;
    size_t old_cap = scanner->pcap;
    scanner->procs = scanner->next;
    scanner->nprocs = scanner->nnext;
    scanner->pcap = scanner->ncap;
    scanner->next = old_procs;
    scanner->nnext = 0;
    scanner->ncap = old_cap;

    long delivered = 0;
    for (size_t i = 0; i < scanner->nprocs; ++i) {
        struct sp_proc_entry *entry = &scanner->procs[i];
        struct sp_proc_info proc;
        memset(&proc, 0, sizeof proc);
        proc.pid = entry->pid;
        proc.starttime = entry->starttime;
        proc.uid = entry->uid;
        sp_strlcpy(proc.comm, entry->comm, sizeof proc.comm);
        sp_strlcpy(proc.exe, entry->exe, sizeof proc.exe);
        sp_strlcpy(proc.cgroup, entry->cgroup, sizeof proc.cgroup);
        struct sp_match_input input;
        memset(&input, 0, sizeof input);
        input.pcomm = proc.comm;
        input.exe = proc.exe;
        input.cgroup = proc.cgroup;
        input.has_uid = true;
        input.uid = proc.uid;
        if (!sp_rules_process_candidate(rules, &input)) continue;
        if (enumerate_threads(scanner, &proc, need_busy, cb, ctx, &delivered, stats) != 0)
            return delivered;
    }

    /* Drop busy samples that have not been observed for five passes. */
    size_t kept = 0;
    for (size_t i = 0; i < scanner->nsamples; ++i) {
        if (scanner->pass > scanner->samples[i].seen + 5) continue;
        if (kept != i) scanner->samples[kept] = scanner->samples[i];
        ++kept;
    }
    scanner->nsamples = kept;
    return delivered;
}
