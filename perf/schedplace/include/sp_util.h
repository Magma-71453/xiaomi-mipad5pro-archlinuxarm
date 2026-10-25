/* schedplace — small string, cpu-mask and safety helpers. */
#ifndef SP_UTIL_H
#define SP_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <sched.h>
#include <sys/types.h>

#define SP_VERSION "0.1.0"
#define SP_MAX_RULES 64
#define SP_PATH_MAX 512
#define SP_TEXT_MAX 256
#define SP_COMM_LEN 16      /* TASK_COMM_LEN - 1: comm is truncated by the kernel */
#define SP_NAME_LEN 64

struct sp_stats;

/* ---- file IO ---------------------------------------------------------- */
int sp_read_file(const char *path, char *buf, size_t size);
int sp_read_link(const char *path, char *buf, size_t size);
bool sp_read_ulong_file(const char *path, unsigned long *out);

/* ---- parsing ---------------------------------------------------------- */
bool sp_parse_ulong(const char *s, unsigned long *out);
bool sp_parse_long(const char *s, long *out);
double sp_parse_double(const char *s, bool *ok);
bool sp_parse_bool(const char *s, bool *out);
bool sp_parse_cpulist(const char *text, cpu_set_t *set);
bool sp_format_cpulist(const cpu_set_t *set, char *buf, size_t size);

/* ---- cpu masks -------------------------------------------------------- */
void sp_cpuset_and(cpu_set_t *dst, const cpu_set_t *a, const cpu_set_t *b);
void sp_cpuset_or(cpu_set_t *dst, const cpu_set_t *a, const cpu_set_t *b);
bool sp_cpuset_equal(const cpu_set_t *a, const cpu_set_t *b);
bool sp_cpuset_subset(const cpu_set_t *sub, const cpu_set_t *sup);
bool sp_cpuset_any(const cpu_set_t *set);
int sp_cpuset_count(const cpu_set_t *set);

/* ---- strings / paths -------------------------------------------------- */
char *sp_trim(char *s);
size_t sp_strlcpy(char *dst, const char *src, size_t size);
bool sp_streq(const char *a, const char *b);
bool sp_ends_with(const char *s, const char *tail);
bool sp_has_space(const char *s);
int sp_path_join(char *dst, size_t size, const char *a, const char *b);
int sp_path_basename(const char *path, char *out, size_t size);
int sp_mkdir_p(const char *path, unsigned int mode);

/* ---- write safety ----------------------------------------------------- */
enum sp_forbidden {
    SP_FORBID_ALLOWED = 0,
    SP_FORBID_GOVERNOR,
    SP_FORBID_FREQUENCY,
    SP_FORBID_CPU_ONLINE,
    SP_FORBID_SCHED_TUNABLE,
    SP_FORBID_VENDOR_BOOST,
    SP_FORBID_NOT_ALLOWLISTED
};

enum sp_forbidden sp_path_classify(const char *path);
const char *sp_forbidden_reason(enum sp_forbidden kind);
bool sp_path_is_forbidden(const char *path);
int sp_safe_write(const char *path, const char *text, bool allow_frequency_writes,
                  struct sp_stats *stats);

#endif
