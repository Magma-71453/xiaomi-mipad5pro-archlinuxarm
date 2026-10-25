/* fake_sp — a harmless thread whose comm and CPU usage the tests control. */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop;

static void *late_thread_main(void *arg) {
    const char *name = arg;
    prctl(PR_SET_NAME, name, 0, 0, 0);
    printf("fake_sp thread=%s ready\n", name);
    fflush(stdout);
    while (!g_stop) usleep(10000);
    return NULL;
}

static void on_signal(int signo) {
    (void)signo;
    g_stop = 1;
}

static long elapsed_ms(const struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000L +
           (now.tv_nsec - start->tv_nsec) / 1000000L;
}

int main(int argc, char **argv) {
    const char *comm = "sp-fake";
    const char *spawn_name = "";
    int spawn_after_ms = 0;
    bool busy = false;
    int seconds = 120;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--comm") == 0 && i + 1 < argc) comm = argv[++i];
        else if (strcmp(argv[i], "--busy") == 0) busy = true;
        else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (strcmp(argv[i], "--spawn-after-ms") == 0 && i + 1 < argc)
            spawn_after_ms = atoi(argv[++i]);
        else if (strcmp(argv[i], "--spawn-name") == 0 && i + 1 < argc)
            spawn_name = argv[++i];
        else {
            fprintf(stderr, "fake_sp: unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    if (prctl(PR_SET_NAME, comm, 0, 0, 0) != 0) {
        perror("prctl(PR_SET_NAME)");
        return 1;
    }
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    printf("fake_sp pid=%ld comm=%s\n", (long)getpid(), comm);
    fflush(stdout);

    bool late_requested = spawn_after_ms > 0 && spawn_name[0];
    bool late_started = false;
    pthread_t late;
    struct timespec started;
    clock_gettime(CLOCK_MONOTONIC, &started);

    volatile unsigned long long accumulator = 0;
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000L};
    for (int i = 0; i < seconds * 1000 && !g_stop; ++i) {
        if (late_requested && !late_started && elapsed_ms(&started) >= spawn_after_ms) {
            if (pthread_create(&late, NULL, late_thread_main, (void *)spawn_name) == 0)
                late_started = true;
        }
        if (busy) {
            for (unsigned long j = 0; j < 200000; ++j) accumulator += j;
            nanosleep(&pause, NULL);
        } else {
            nanosleep(&pause, NULL);
        }
    }
    (void)accumulator;
    if (late_started) pthread_join(late, NULL);
    return 0;
}
