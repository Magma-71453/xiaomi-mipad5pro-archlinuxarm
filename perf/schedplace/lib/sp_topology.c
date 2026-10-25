/* schedplace — runtime CPU topology discovery.
 *
 * Nothing here is hard-coded: the clusters come from cpu_capacity (preferred) or
 * cpufreq/cpuinfo_max_freq, grouped by distinct value and sorted from fastest to
 * slowest. Incomplete or missing data degrades to a single cluster plus a warning.
 */
#include "sp_topology.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sp_log.h"

static int cmp_desc(const void *a, const void *b) {
    unsigned long x = *(const unsigned long *)a;
    unsigned long y = *(const unsigned long *)b;
    if (x < y) return 1;
    if (x > y) return -1;
    return 0;
}

static bool read_online_file(const char *root, cpu_set_t *online) {
    char path[SP_PATH_MAX];
    char text[SP_TEXT_MAX];
    if (sp_path_join(path, sizeof path, root, "online") != 0) return false;
    if (sp_read_file(path, text, sizeof text) < 0) return false;
    return sp_parse_cpulist(text, online);
}

static bool scan_cpu_dirs(const char *root, cpu_set_t *online) {
    DIR *dir = opendir(root);
    if (!dir) return false;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "cpu", 3) != 0) continue;
        const char *digits = entry->d_name + 3;
        if (!*digits) continue;
        bool numeric = true;
        for (const char *p = digits; *p; ++p)
            if (*p < '0' || *p > '9') numeric = false;
        if (!numeric) continue;
        long cpu = strtol(digits, NULL, 10);
        if (cpu < 0 || cpu >= CPU_SETSIZE) continue;
        char path[SP_PATH_MAX];
        char value[16];
        if (sp_path_join(path, sizeof path, root, entry->d_name) != 0) continue;
        char online_path[SP_PATH_MAX];
        if (sp_path_join(online_path, sizeof online_path, path, "online") != 0) continue;
        if (sp_read_file(online_path, value, sizeof value) >= 0) {
            unsigned long flag = 0;
            if (!sp_parse_ulong(value, &flag) || flag == 0) continue;
        }
        CPU_SET((int)cpu, online);
    }
    closedir(dir);
    return sp_cpuset_any(online);
}

static bool read_cpu_value(const char *root, int cpu, const char *leaf, unsigned long *out) {
    char path[SP_PATH_MAX];
    char cpu_dir[32];
    char nested[SP_PATH_MAX];
    snprintf(cpu_dir, sizeof cpu_dir, "cpu%d", cpu);
    if (sp_path_join(path, sizeof path, root, cpu_dir) != 0) return false;
    if (sp_path_join(nested, sizeof nested, path, leaf) != 0) return false;
    return sp_read_ulong_file(nested, out);
}

static int collect_values(const char *root, const cpu_set_t *online, const char *leaf,
                          unsigned long *values) {
    int found = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        values[cpu] = 0;
        if (!CPU_ISSET(cpu, online)) continue;
        if (read_cpu_value(root, cpu, leaf, &values[cpu])) ++found;
    }
    return found;
}

static void set_single_cluster(struct sp_topology *topo, unsigned long capacity,
                               const char *reason) {
    for (int level = 0; level < 3; ++level) {
        topo->cluster[level] = topo->online;
        topo->capacity[level] = capacity;
    }
    topo->levels = 1;
    topo->degraded = true;
    sp_strlcpy(topo->warning, reason, sizeof topo->warning);
    sp_logf(SP_LOG_WARN, "topology degraded to a single cluster: %s", reason);
}

int sp_topo_discover(struct sp_topology *topo, const char *sysfs_root, char *err,
                     size_t errlen) {
    if (!topo) {
        if (err) snprintf(err, errlen, "null topology");
        return -1;
    }
    memset(topo, 0, sizeof *topo);
    sp_strlcpy(topo->sysfs_root, (sysfs_root && *sysfs_root) ? sysfs_root
                                                            : "/sys/devices/system/cpu",
               sizeof topo->sysfs_root);
    CPU_ZERO(&topo->online);

    if (!read_online_file(topo->sysfs_root, &topo->online) &&
        !scan_cpu_dirs(topo->sysfs_root, &topo->online)) {
        long count = sysconf(_SC_NPROCESSORS_ONLN);
        if (count < 1 || count > CPU_SETSIZE) {
            if (err)
                snprintf(err, errlen,
                         "cannot determine the online CPU set from %s (run with --sysfs-root "
                         "when testing)",
                         topo->sysfs_root);
            return -1;
        }
        for (long cpu = 0; cpu < count; ++cpu) CPU_SET((int)cpu, &topo->online);
        char reason[SP_PATH_MAX + 128];
        snprintf(reason, sizeof reason,
                 "online CPU list unreadable under %s; fell back to %ld online CPUs",
                 topo->sysfs_root, count);
        set_single_cluster(topo, 0, reason);
        sp_strlcpy(topo->source, "fallback", sizeof topo->source);
        return 0;
    }

    int online_count = sp_cpuset_count(&topo->online);
    if (online_count <= 0) {
        if (err) snprintf(err, errlen, "no online CPUs under %s", topo->sysfs_root);
        return -1;
    }

    unsigned long values[CPU_SETSIZE];
    int found = collect_values(topo->sysfs_root, &topo->online, "cpu_capacity", values);
    const char *source = "cpu_capacity";
    if (found != online_count) {
        int capacity_found = found;
        int frequency_found = collect_values(topo->sysfs_root, &topo->online,
                                             "cpufreq/cpuinfo_max_freq", values);
        source = "cpufreq_max_freq";
        found = frequency_found;
        /* report the better-covered source so the warning names the real gap */
        if (capacity_found > frequency_found) {
            found = collect_values(topo->sysfs_root, &topo->online, "cpu_capacity", values);
            source = "cpu_capacity";
        }
    }
    if (found != online_count) {
        char reason[SP_PATH_MAX + 128];
        snprintf(reason, sizeof reason,
                 "capacity/frequency data found for %d of %d online CPUs under %s",
                 found, online_count, topo->sysfs_root);
        set_single_cluster(topo, 0, reason);
        sp_strlcpy(topo->source, "none", sizeof topo->source);
        return 0;
    }
    sp_strlcpy(topo->source, source, sizeof topo->source);

    unsigned long unique[CPU_SETSIZE];
    int nunique = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &topo->online)) continue;
        bool seen = false;
        for (int i = 0; i < nunique; ++i)
            if (unique[i] == values[cpu]) seen = true;
        if (!seen) unique[nunique++] = values[cpu];
    }
    qsort(unique, (size_t)nunique, sizeof unique[0], cmp_desc);

    if (nunique < 2) {
        char reason[SP_PATH_MAX + 128];
        snprintf(reason, sizeof reason,
                 "single capacity level (%lu) for all %d online CPUs; placement cannot "
                 "differentiate clusters",
                 nunique > 0 ? unique[0] : 0UL, online_count);
        set_single_cluster(topo, nunique > 0 ? unique[0] : 0UL, reason);
        return 0;
    }

    int levels = nunique >= 3 ? 3 : 2;
    CPU_ZERO(&topo->cluster[0]);
    CPU_ZERO(&topo->cluster[1]);
    CPU_ZERO(&topo->cluster[2]);
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &topo->online)) continue;
        int rank = 0;
        for (int i = 0; i < nunique; ++i) {
            if (values[cpu] == unique[i]) {
                rank = i;
                break;
            }
        }
        if (levels == 2) {
            if (rank == 0) {
                CPU_SET(cpu, &topo->cluster[2]); /* prime */
                CPU_SET(cpu, &topo->cluster[1]); /* big aliases prime */
            } else {
                CPU_SET(cpu, &topo->cluster[0]); /* little */
            }
        } else if (rank == 0) {
            CPU_SET(cpu, &topo->cluster[2]); /* prime */
        } else if (rank == 1) {
            CPU_SET(cpu, &topo->cluster[1]); /* big */
        } else {
            CPU_SET(cpu, &topo->cluster[0]); /* little: every slower level */
        }
    }
    if (levels == 2) {
        topo->capacity[2] = unique[0];
        topo->capacity[1] = unique[0];
        topo->capacity[0] = unique[1];
        snprintf(topo->warning, sizeof topo->warning,
                 "two capacity levels (%lu/%lu); prime aliases the big cluster", unique[0],
                 unique[1]);
        sp_logf(SP_LOG_WARN, "%s", topo->warning);
    } else {
        topo->capacity[2] = unique[0];
        topo->capacity[1] = unique[1];
        topo->capacity[0] = unique[nunique - 1];
    }
    topo->levels = levels;
    topo->degraded = false;
    return 0;
}

const cpu_set_t *sp_topo_cluster(const struct sp_topology *topo, enum sp_cluster cluster) {
    if (!topo) return NULL;
    switch (cluster) {
    case SP_CLUSTER_LITTLE:
        return &topo->cluster[0];
    case SP_CLUSTER_BIG:
        return &topo->cluster[1];
    case SP_CLUSTER_PRIME:
        return &topo->cluster[2];
    default:
        return NULL;
    }
}

bool sp_topo_resolve(const struct sp_topology *topo, enum sp_cluster cluster,
                     enum sp_profile profile, cpu_set_t *out) {
    if (!topo || !out) return false;
    cpu_set_t mask;
    CPU_ZERO(&mask);
    switch (cluster) {
    case SP_CLUSTER_LITTLE:
        sp_cpuset_and(&mask, &topo->cluster[0], &topo->online);
        break;
    case SP_CLUSTER_BIG:
        sp_cpuset_and(&mask, &topo->cluster[1], &topo->online);
        break;
    case SP_CLUSTER_PRIME:
        sp_cpuset_and(&mask, &topo->cluster[2], &topo->online);
        break;
    case SP_CLUSTER_PERF:
        sp_cpuset_or(&mask, &topo->cluster[1], &topo->cluster[2]);
        sp_cpuset_and(&mask, &mask, &topo->online);
        break;
    case SP_CLUSTER_AUTO:
        if (profile == SP_PROFILE_LATENCY) {
            sp_cpuset_or(&mask, &topo->cluster[1], &topo->cluster[2]);
            sp_cpuset_and(&mask, &mask, &topo->online);
        } else if (profile == SP_PROFILE_BACKGROUND) {
            sp_cpuset_and(&mask, &topo->cluster[0], &topo->online);
        } else {
            return false;
        }
        break;
    case SP_CLUSTER_KEEP:
        return false;
    }
    if (!sp_cpuset_any(&mask)) return false;
    *out = mask;
    return true;
}

void sp_topo_summary(const struct sp_topology *topo, char *buf, size_t size) {
    if (!topo || !buf || size < 2) return;
    char online[SP_TEXT_MAX], little[SP_TEXT_MAX], big[SP_TEXT_MAX], prime[SP_TEXT_MAX];
    sp_format_cpulist(&topo->online, online, sizeof online);
    if (!sp_format_cpulist(&topo->cluster[0], little, sizeof little))
        sp_strlcpy(little, "-", sizeof little);
    if (!sp_format_cpulist(&topo->cluster[1], big, sizeof big))
        sp_strlcpy(big, "-", sizeof big);
    if (!sp_format_cpulist(&topo->cluster[2], prime, sizeof prime))
        sp_strlcpy(prime, "-", sizeof prime);
    snprintf(buf, size,
             "online=%s source=%s levels=%d little=%s/%lu big=%s/%lu prime=%s/%lu "
             "degraded=%d warning=\"%s\"",
             online[0] ? online : "-", topo->source, topo->levels, little, topo->capacity[0],
             big, topo->capacity[1], prime, topo->capacity[2], topo->degraded ? 1 : 0,
             topo->warning);
}
