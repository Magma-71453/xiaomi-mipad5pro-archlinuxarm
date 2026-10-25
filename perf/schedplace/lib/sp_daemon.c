/* schedplace — event loop.
 *
 * One thread, no busy waiting: epoll waits on
 *   - timerfd (periodic full rescan, default 500 ms),
 *   - signalfd (SIGUSR1 status dump, SIGTERM/SIGINT shutdown),
 *   - inotify (new thread in a tracked process, foreground marker change).
 * The inotify path is an accelerator only: if the kernel refuses the watches the
 * daemon keeps working on the periodic timer.
 */
#include "sp_daemon.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "sp_apply.h"
#include "sp_log.h"
#include "sp_proc.h"

#define SP_MAX_EVENTS 8

struct sp_ctx {
    struct sp_config *cfg;
    struct sp_topology *topo;
    struct sp_scanner *scanner;
    struct sp_stats stats;
    unsigned long long scan_id;
    pid_t *watch_pids;
    size_t nwatch;
    size_t wcap;
    size_t *probe_counts; /* thread count per watched pid, sampled after each scan */
    size_t probe_cap;
    size_t probe_count;
};

struct sp_watch {
    int wd;
    char path[SP_PATH_MAX];
};

struct sp_loop {
    int epfd;
    int timerfd;
    int probefd;
    int sigfd;
    int inofd;
    bool inotify_ok;
    struct sp_watch *watches;
    size_t nwatches;
    size_t wcap;
};

static void ctx_reset_watch_pids(struct sp_ctx *ctx) {
    ctx->nwatch = 0;
}

static void ctx_add_watch_pid(struct sp_ctx *ctx, pid_t pid) {
    for (size_t i = 0; i < ctx->nwatch; ++i)
        if (ctx->watch_pids[i] == pid) return;
    if (ctx->nwatch == ctx->wcap) {
        size_t grown = ctx->wcap ? ctx->wcap * 2 : 64;
        pid_t *resized = realloc(ctx->watch_pids, grown * sizeof *resized);
        if (!resized) return;
        ctx->watch_pids = resized;
        ctx->wcap = grown;
    }
    ctx->watch_pids[ctx->nwatch++] = pid;
}

static int count_threads(pid_t pid) {
    char dir[64];
    snprintf(dir, sizeof dir, "/proc/%ld/task", (long)pid);
    DIR *task = opendir(dir);
    if (!task) return -1;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(task)) != NULL) {
        bool numeric = entry->d_name[0] != '\0';
        for (const char *p = entry->d_name; *p; ++p)
            if (*p < '0' || *p > '9') numeric = false;
        if (numeric) ++count;
    }
    closedir(task);
    return count;
}

/* Snapshot the thread count of every tracked process; a later probe compares
 * against it. procfs emits no inotify events for new threads on current kernels
 * (measured on 7.2.11), so this bounded probe is what makes a new thread visible
 * well before the next full scan. */
static void probe_snapshot(struct sp_ctx *ctx) {
    ctx->probe_count = ctx->nwatch;
    if (ctx->probe_cap < ctx->probe_count) {
        size_t *grown = realloc(ctx->probe_counts, ctx->probe_count * sizeof *grown);
        if (!grown) {
            ctx->probe_count = 0;
            return;
        }
        ctx->probe_counts = grown;
        ctx->probe_cap = ctx->probe_count;
    }
    for (size_t i = 0; i < ctx->probe_count; ++i) {
        int count = count_threads(ctx->watch_pids[i]);
        ctx->probe_counts[i] = count < 0 ? (size_t)-1 : (size_t)count;
    }
}

static bool probe_changed(const struct sp_ctx *ctx) {
    for (size_t i = 0; i < ctx->probe_count; ++i) {
        int count = count_threads(ctx->watch_pids[i]);
        size_t current = count < 0 ? (size_t)-1 : (size_t)count;
        if (current != ctx->probe_counts[i]) return true;
    }
    return false;
}

static int cmp_pid(const void *a, const void *b) {
    pid_t x = *(const pid_t *)a;
    pid_t y = *(const pid_t *)b;
    return (x > y) - (x < y);
}

static int on_thread(void *vctx, const struct sp_proc_info *proc,
                     const struct sp_thread_info *thread) {
    struct sp_ctx *ctx = vctx;
    struct sp_match_input input;
    memset(&input, 0, sizeof input);
    input.pcomm = proc->comm;
    input.tcomm = thread->comm;
    input.exe = proc->exe;
    input.cgroup = proc->cgroup;
    input.has_uid = true;
    input.uid = proc->uid;
    input.busy_pct = thread->busy_pct;
    input.busy_known = thread->busy_known;
    int index = sp_rules_match(&ctx->cfg->rules, &input);
    if (index < 0) return 0;
    ++ctx->stats.candidates;
    if ((size_t)index < SP_MAX_RULES) ++ctx->stats.rule_hits[index];
    ctx_add_watch_pid(ctx, proc->pid);
    struct sp_decision decision;
    char err[192];
    if (sp_apply_thread(ctx->cfg, ctx->topo, thread, index, ctx->scan_id, &ctx->stats,
                        &decision, err, sizeof err) != 0) {
        sp_logf(SP_LOG_DEBUG, "placement failed for tid %ld: %s", (long)thread->tid, err);
    }
    return 0;
}

static long run_scan(struct sp_ctx *ctx, bool apply) {
    char err[192];
    err[0] = '\0';
    ++ctx->scan_id;
    ctx_reset_watch_pids(ctx);
    long delivered = sp_scanner_scan(ctx->scanner, &ctx->cfg->rules,
                                     ctx->cfg->rules.needs_busy,
                                     apply ? on_thread : NULL, ctx, &ctx->stats, err,
                                     sizeof err);
    if (delivered < 0)
        sp_logf(SP_LOG_ERROR, "scan failed: %s", err[0] ? err : "unknown error");
    else if (sp_log_level() >= SP_LOG_DEBUG)
        sp_logf(SP_LOG_DEBUG,
                "scan %llu: processes=%lu candidate_threads=%ld decisions=%lu applied=%lu "
                "unchanged=%lu errors=%lu",
                ctx->scan_id, ctx->stats.processes_seen, delivered, ctx->stats.decisions,
                ctx->stats.affinity_applied, ctx->stats.skipped_unchanged, ctx->stats.errors);
    return delivered;
}

static void print_summary(const struct sp_stats *stats) {
    fprintf(stderr,
            "schedplace: summary scans=%lu processes=%lu threads=%lu candidates=%lu "
            "decisions=%lu affinity=%lu cgroup=%lu nice=%lu unchanged=%lu errors=%lu "
            "refused_writes=%lu\n",
            stats->scans, stats->processes_seen, stats->threads_seen, stats->candidates,
            stats->decisions, stats->affinity_applied, stats->cgroup_applied,
            stats->nice_applied, stats->skipped_unchanged, stats->errors,
            stats->refused_writes);
    fflush(stderr);
}

static void sleep_ms(long ms) {
    struct timespec delay;
    delay.tv_sec = ms / 1000;
    delay.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

int sp_daemon_once(struct sp_config *cfg, struct sp_topology *topo) {
    struct sp_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.cfg = cfg;
    ctx.topo = topo;
    if (sp_scanner_init(&ctx.scanner) != 0) {
        sp_logf(SP_LOG_ERROR, "cannot initialise the /proc scanner");
        return 3;
    }
    if (cfg->rules.needs_busy) {
        /* busy% needs two samples: warm up, then decide */
        run_scan(&ctx, false);
        sleep_ms(120);
    }
    run_scan(&ctx, true);
    print_summary(&ctx.stats);
    sp_scanner_free(ctx.scanner);
    free(ctx.watch_pids);
    free(ctx.probe_counts);
    return 0;
}

static int loop_add(struct sp_loop *loop, int fd, uint32_t events) {
    struct epoll_event event;
    memset(&event, 0, sizeof event);
    event.events = events;
    event.data.fd = fd;
    return epoll_ctl(loop->epfd, EPOLL_CTL_ADD, fd, &event);
}

static void watches_clear(struct sp_loop *loop) {
    for (size_t i = 0; i < loop->nwatches; ++i)
        if (loop->watches[i].wd >= 0) inotify_rm_watch(loop->inofd, loop->watches[i].wd);
    loop->nwatches = 0;
}

static void watches_sync(struct sp_loop *loop, struct sp_ctx *ctx) {
    if (!loop->inotify_ok) return;
    pid_t pids[512];
    size_t pid_count = 0;
    for (size_t i = 0; i < ctx->nwatch; ++i)
        pids[pid_count++] = ctx->watch_pids[i];
    qsort(pids, pid_count, sizeof pids[0], cmp_pid);

    char paths[512][SP_PATH_MAX];
    size_t path_count = 0;
    size_t limit = (size_t)ctx->cfg->watch_limit;
    if (limit > 512) limit = 512;
    for (size_t i = 0; i < pid_count && path_count < limit; ++i) {
        char dir[32];
        snprintf(dir, sizeof dir, "%ld", (long)pids[i]);
        char proc_dir[SP_PATH_MAX];
        if (sp_path_join(proc_dir, sizeof proc_dir, "/proc", dir) != 0) continue;
        char task_dir[SP_PATH_MAX];
        if (sp_path_join(task_dir, sizeof task_dir, proc_dir, "task") != 0) continue;
        sp_strlcpy(paths[path_count++], task_dir, SP_PATH_MAX);
    }
    if (ctx->cfg->foreground_file[0] && path_count < limit) {
        char parent[SP_PATH_MAX];
        sp_strlcpy(parent, ctx->cfg->foreground_file, sizeof parent);
        char *slash = strrchr(parent, '/');
        if (slash && slash != parent) {
            *slash = '\0';
            if (access(ctx->cfg->foreground_file, F_OK) == 0)
                sp_strlcpy(paths[path_count++], ctx->cfg->foreground_file, SP_PATH_MAX);
            else
                sp_strlcpy(paths[path_count++], parent, SP_PATH_MAX);
        }
    }
    bool same = path_count == loop->nwatches;
    for (size_t i = 0; same && i < path_count; ++i)
        if (!sp_streq(paths[i], loop->watches[i].path)) same = false;
    if (same) return;
    watches_clear(loop);
    if (loop->wcap < path_count) {
        struct sp_watch *resized = realloc(loop->watches, path_count * sizeof *resized);
        if (!resized) return;
        loop->watches = resized;
        loop->wcap = path_count;
    }
    for (size_t i = 0; i < path_count; ++i) {
        uint32_t mask = IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MODIFY | IN_CLOSE_WRITE;
        if (sp_ends_with(paths[i], "/task")) mask |= IN_ONLYDIR;
        int wd = inotify_add_watch(loop->inofd, paths[i], mask);
        if (wd < 0) continue;
        loop->watches[loop->nwatches].wd = wd;
        sp_strlcpy(loop->watches[loop->nwatches].path, paths[i], SP_PATH_MAX);
        ++loop->nwatches;
    }
    sp_logf(SP_LOG_DEBUG, "inotify watches: %zu", loop->nwatches);
}

static void loop_close(struct sp_loop *loop) {
    watches_clear(loop);
    free(loop->watches);
    if (loop->inofd >= 0) close(loop->inofd);
    if (loop->sigfd >= 0) close(loop->sigfd);
    if (loop->probefd >= 0) close(loop->probefd);
    if (loop->timerfd >= 0) close(loop->timerfd);
    if (loop->epfd >= 0) close(loop->epfd);
}

int sp_daemon_run(struct sp_config *cfg, struct sp_topology *topo) {
    struct sp_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.cfg = cfg;
    ctx.topo = topo;
    if (sp_scanner_init(&ctx.scanner) != 0) {
        sp_logf(SP_LOG_ERROR, "cannot initialise the /proc scanner");
        return 3;
    }
    struct sp_loop loop;
    memset(&loop, 0, sizeof loop);
    loop.epfd = loop.timerfd = loop.probefd = loop.sigfd = loop.inofd = -1;

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0)
        sp_logf(SP_LOG_WARN, "cannot block signals: %s", strerror(errno));
    signal(SIGPIPE, SIG_IGN);

    loop.epfd = epoll_create1(EPOLL_CLOEXEC);
    if (loop.epfd < 0) {
        sp_logf(SP_LOG_ERROR, "epoll_create1: %s", strerror(errno));
        sp_scanner_free(ctx.scanner);
        return 3;
    }
    loop.timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (loop.timerfd < 0) {
        sp_logf(SP_LOG_ERROR, "timerfd_create: %s", strerror(errno));
        loop_close(&loop);
        sp_scanner_free(ctx.scanner);
        return 3;
    }
    struct itimerspec timer;
    memset(&timer, 0, sizeof timer);
    timer.it_value.tv_sec = cfg->interval_ms / 1000;
    timer.it_value.tv_nsec = (cfg->interval_ms % 1000) * 1000000L;
    timer.it_interval = timer.it_value;
    if (timerfd_settime(loop.timerfd, 0, &timer, NULL) != 0)
        sp_logf(SP_LOG_WARN, "timerfd_settime: %s", strerror(errno));
    loop.probefd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (loop.probefd >= 0) {
        struct itimerspec probe;
        memset(&probe, 0, sizeof probe);
        probe.it_value.tv_nsec = cfg->probe_ms * 1000000L;
        if (probe.it_value.tv_nsec >= 1000000000L) {
            probe.it_value.tv_sec = probe.it_value.tv_nsec / 1000000000L;
            probe.it_value.tv_nsec %= 1000000000L;
        }
        probe.it_interval = probe.it_value;
        if (timerfd_settime(loop.probefd, 0, &probe, NULL) != 0) {
            sp_logf(SP_LOG_WARN, "thread probe timer unavailable: %s", strerror(errno));
            close(loop.probefd);
            loop.probefd = -1;
        }
    }
    loop.sigfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (loop.sigfd < 0) sp_logf(SP_LOG_WARN, "signalfd: %s", strerror(errno));
    loop.inofd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    loop.inotify_ok = loop.inofd >= 0;
    if (!loop.inotify_ok)
        sp_logf(SP_LOG_WARN,
                "inotify unavailable (%s); falling back to the %ld ms periodic scan only",
                strerror(errno), cfg->interval_ms);

    if (loop_add(&loop, loop.timerfd, EPOLLIN) != 0 ||
        (loop.probefd >= 0 && loop_add(&loop, loop.probefd, EPOLLIN) != 0) ||
        (loop.sigfd >= 0 && loop_add(&loop, loop.sigfd, EPOLLIN) != 0) ||
        (loop.inotify_ok && loop_add(&loop, loop.inofd, EPOLLIN) != 0)) {
        sp_logf(SP_LOG_ERROR, "epoll_ctl: %s", strerror(errno));
        loop_close(&loop);
        sp_scanner_free(ctx.scanner);
        return 3;
    }

    /* first scan immediately so a restart is not blind for one interval */
    run_scan(&ctx, true);
    watches_sync(&loop, &ctx);
    probe_snapshot(&ctx);

    int rc = 0;
    bool stop = false;
    long long last_rescan_ms = 0;
    while (!stop) {
        struct epoll_event events[SP_MAX_EVENTS];
        int ready = epoll_wait(loop.epfd, events, SP_MAX_EVENTS, -1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            sp_logf(SP_LOG_ERROR, "epoll_wait: %s", strerror(errno));
            rc = 3;
            break;
        }
        bool rescan = false;
        bool rescan_is_event = false;
        for (int i = 0; i < ready; ++i) {
            int fd = events[i].data.fd;
            if (fd == loop.timerfd) {
                uint64_t expirations = 0;
                if (read(loop.timerfd, &expirations, sizeof expirations) < 0 &&
                    errno != EAGAIN)
                    sp_logf(SP_LOG_WARN, "timerfd read: %s", strerror(errno));
                rescan = true;
            } else if (fd == loop.probefd && loop.probefd >= 0) {
                uint64_t ticks = 0;
                if (read(loop.probefd, &ticks, sizeof ticks) < 0 && errno != EAGAIN)
                    sp_logf(SP_LOG_WARN, "probe timer read: %s", strerror(errno));
                if (probe_changed(&ctx)) {
                    sp_logf(SP_LOG_DEBUG, "thread set changed in a tracked process; rescanning");
                    rescan = true;
                }
            } else if (fd == loop.sigfd && loop.sigfd >= 0) {
                struct signalfd_siginfo info;
                while (read(loop.sigfd, &info, sizeof info) == (ssize_t)sizeof info) {
                    if (info.ssi_signo == SIGUSR1) {
                        sp_state_report(stderr, cfg, topo, &ctx.stats);
                    } else {
                        sp_logf(SP_LOG_INFO, "received signal %u; shutting down",
                                info.ssi_signo);
                        stop = true;
                    }
                }
            } else if (fd == loop.inofd) {
                char buffer[4096];
                ssize_t got;
                while ((got = read(loop.inofd, buffer, sizeof buffer)) > 0) {
                    for (ssize_t offset = 0; offset < got;) {
                        struct inotify_event *event = (struct inotify_event *)(buffer + offset);
                        sp_logf(SP_LOG_DEBUG, "inotify mask=0x%x name=%s", event->mask,
                                event->len ? event->name : "-");
                        offset += (ssize_t)(sizeof *event + event->len);
                    }
                }
                rescan = true;
                rescan_is_event = true;
            }
        }
        if (stop) break;
        if (rescan) {
            struct timespec now_ts;
            clock_gettime(CLOCK_MONOTONIC, &now_ts);
            long long now_ms = (long long)now_ts.tv_sec * 1000 + now_ts.tv_nsec / 1000000;
            /* Coalesce event driven rescans: the periodic scan picks the work
             * up anyway, so walking every thread for each inotify notification
             * just burns a core on a system with a thousand threads. */
            if (rescan_is_event && last_rescan_ms &&
                now_ms - last_rescan_ms < (long long)cfg->interval_ms) {
                rescan = false;
            }
            last_rescan_ms = now_ms;
        }
        if (rescan) {
            run_scan(&ctx, true);
            watches_sync(&loop, &ctx);
            probe_snapshot(&ctx);
            if (ctx.scan_id % 120 == 0) sp_state_prune(ctx.scan_id);
        }
    }
    loop_close(&loop);
    sp_apply_restore_all(cfg, &ctx.stats, &ctx.stats);
    print_summary(&ctx.stats);
    sp_scanner_free(ctx.scanner);
    free(ctx.watch_pids);
    free(ctx.probe_counts);
    return rc;
}
