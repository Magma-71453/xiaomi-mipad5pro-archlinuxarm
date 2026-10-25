/* Shared fixtures: mock sysfs trees and temporary directories. */
#ifndef SP_FIXTURE_H
#define SP_FIXTURE_H

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sp_util.h"

static inline int sp_fixture_write(const char *root, const char *rel, const char *text) {
    char path[1024];
    if (sp_path_join(path, sizeof path, root, rel) != 0) return -1;
    char dir[1024];
    sp_strlcpy(dir, path, sizeof dir);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
        if (sp_mkdir_p(dir, 0755) != 0) return -1;
    }
    FILE *file = fopen(path, "w");
    if (!file) return -1;
    if (text && *text) fputs(text, file);
    fputc('\n', file);
    fclose(file);
    return 0;
}

static inline int sp_fixture_read(const char *root, const char *rel, char *buf,
                                  size_t size) {
    char path[1024];
    if (sp_path_join(path, sizeof path, root, rel) != 0) return -1;
    return sp_read_file(path, buf, size);
}

static inline void sp_fixture_remove(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) {
        unlink(path);
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[1024];
        if (sp_path_join(child, sizeof child, path, entry->d_name) != 0) continue;
        struct stat st;
        if (lstat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) sp_fixture_remove(child);
        else unlink(child);
    }
    closedir(dir);
    rmdir(path);
}

/* Creates "<SP_TEST_TMPDIR>/sp-test-XXXXXX" and returns it, or NULL.
 * SP_TEST_TMPDIR is set by the Makefile and lives outside the repository, so a
 * fixture run never leaves artifacts in the tree. */
#ifndef SP_TEST_TMPDIR
#define SP_TEST_TMPDIR "/tmp"
#endif

static inline char *sp_fixture_mkdtemp(char *buf, size_t size) {
    if (sp_mkdir_p(SP_TEST_TMPDIR, 0755) != 0) return NULL;
    if (sp_path_join(buf, size, SP_TEST_TMPDIR, "sp-test-XXXXXX") != 0) return NULL;
    return mkdtemp(buf);
}

static inline char *sp_fixture_dir_for_write(char *buf, size_t size) {
    return sp_fixture_mkdtemp(buf, size);
}

/* Mock topology root: <root>/online plus per-CPU capacity/frequency files.
 * caps[cpu] == 0 means "no cpu_capacity file"; freqs[cpu] == 0 means none either. */
static inline int sp_fixture_sysfs(const char *root, const int *caps, const int *freqs,
                                   int ncpu, const char *online) {
    if (sp_fixture_write(root, "online", online) != 0) return -1;
    for (int cpu = 0; cpu < ncpu; ++cpu) {
        char rel[128];
        snprintf(rel, sizeof rel, "cpu%d/cpu_capacity", cpu);
        if (caps && caps[cpu] > 0) {
            char value[32];
            snprintf(value, sizeof value, "%d", caps[cpu]);
            if (sp_fixture_write(root, rel, value) != 0) return -1;
        }
        if (freqs && freqs[cpu] > 0) {
            snprintf(rel, sizeof rel, "cpu%d/cpufreq/cpuinfo_max_freq", cpu);
            char value[32];
            snprintf(value, sizeof value, "%d", freqs[cpu]);
            if (sp_fixture_write(root, rel, value) != 0) return -1;
        }
        snprintf(rel, sizeof rel, "cpu%d/online", cpu);
        if (sp_fixture_write(root, rel, "1") != 0) return -1;
    }
    return 0;
}

#endif
