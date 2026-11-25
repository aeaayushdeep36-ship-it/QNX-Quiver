#include <errno.h>
#include <fcntl.h>
#include <secpol/secpol.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define CHILD_COUNT 4u
#define PATH_CAPACITY 512u
#define INITIAL_RESTART_MS 250u
#define MAX_RESTART_MS 30000u
#define STABLE_RUN_NS UINT64_C(5000000000)

typedef struct {
    const char *name;
    const char *secpol_type;
    char path[PATH_CAPACITY];
    char *argv[4];
    pid_t pid;
    unsigned restart_delay_ms;
    uint64_t restart_at_ns;
    uint64_t started_at_ns;
    int wait_status;
    int wait_observed;
} child_t;

static volatile sig_atomic_t stopping;
static void stop_handler(int signal_number) { (void)signal_number; stopping = 1; }

static uint64_t monotonic_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static void sleep_ms(unsigned ms)
{
    struct timespec delay = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};
    while (!stopping && nanosleep(&delay, &delay) == -1 && errno == EINTR) { }
}

static int start_child(child_t *child)
{
    posix_spawnattr_t attr;
    int rc = posix_spawnattr_init(&attr);
    if (rc != 0) {
        fprintf(stderr, "QNX-Quiver supervisor: spawnattr_init %s: %s\n",
                child->name, strerror(rc));
        return -1;
    }
    if (child->secpol_type != NULL &&
        secpol_posix_spawnattr_settypeid(NULL, &attr, child->secpol_type,
                                         SECPOL_TYPE_NAME) == -1) {
        fprintf(stderr, "QNX-Quiver supervisor: secpol type %s for %s: %s\n",
                child->secpol_type, child->name, strerror(errno));
        (void)posix_spawnattr_destroy(&attr);
        return -1;
    }
    pid_t pid = -1;
    rc = posix_spawn(&pid, child->path, NULL, &attr, child->argv, environ);
    (void)posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        fprintf(stderr, "QNX-Quiver supervisor: posix_spawn %s (%s): %s\n",
                child->name, child->path, strerror(rc));
        return -1;
    }
    child->pid = pid;
    child->started_at_ns = monotonic_ns();
    child->wait_status = 0;
    child->wait_observed = 0;
    printf("QNX-Quiver supervisor: started %s pid=%ld type=%s\n",
           child->name, (long)pid,
           child->secpol_type != NULL ? child->secpol_type : "inherited");
    return 0;
}

static int wait_for_quiver_node(unsigned timeout_ms)
{
    unsigned elapsed = 0;
    while (!stopping && elapsed < timeout_ms) {
        if (access("/dev/quiver", R_OK) == 0) return 0;
        sleep_ms(100);
        elapsed += 100;
    }
    return -1;
}

static int process_exists(pid_t pid)
{
    if (pid <= 0) return 0;
    if (kill(pid, 0) == 0) return 1;
    return errno == EPERM;
}

static int select_trace_path(char *path, size_t capacity)
{
    static const char *directories[] = {"/tmp", "/dev/shmem"};
    for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); ++i) {
        struct stat directory;
        if (stat(directories[i], &directory) == -1 ||
            !S_ISDIR(directory.st_mode) || access(directories[i], W_OK) == -1) {
            continue;
        }
        int length = snprintf(path, capacity, "%s/%s", directories[i],
                              "quiver-supervisor-trace.csv");
        if (length < 0 || (size_t)length >= capacity) {
            errno = ENAMETOOLONG;
            return -1;
        }
        return 0;
    }
    errno = EACCES;
    return -1;
}

/* Some QNX DDK workers can outlive the thread whose status waitpid observes.
 * Do not launch a replacement or forget a PID until the process is gone. */
static int child_exited(child_t *child)
{
    if (child->pid <= 0) return 1;
    if (!child->wait_observed) {
        pid_t result = waitpid(child->pid, &child->wait_status, WNOHANG);
        if (result == child->pid || (result == -1 && errno == ECHILD)) {
            child->wait_observed = 1;
        } else if (result == 0 || (result == -1 && errno == EINTR)) {
            return 0;
        } else if (result == -1) {
            fprintf(stderr, "QNX-Quiver supervisor: waitpid %s: %s\n",
                    child->name, strerror(errno));
            return 0;
        }
    }
    if (process_exists(child->pid)) return 0;

    uint64_t now = monotonic_ns();
    if (now >= child->started_at_ns &&
        now - child->started_at_ns >= STABLE_RUN_NS) {
        child->restart_delay_ms = INITIAL_RESTART_MS;
    }
    child->restart_at_ns = now +
        (uint64_t)child->restart_delay_ms * UINT64_C(1000000);
    fprintf(stderr, "QNX-Quiver supervisor: %s exited (status=%d); retry in %u ms\n",
            child->name, child->wait_status, child->restart_delay_ms);
    if (child->restart_delay_ms < MAX_RESTART_MS / 2u) {
        child->restart_delay_ms *= 2u;
    } else {
        child->restart_delay_ms = MAX_RESTART_MS;
    }
    child->pid = -1;
    child->wait_observed = 0;
    return 1;
}

static int any_children_running(child_t children[], size_t count)
{
    int running = 0;
    for (size_t i = 0; i < count; ++i) {
        if (children[i].pid > 0 && !child_exited(&children[i])) running = 1;
    }
    return running;
}

static void stop_children(child_t children[], size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (children[i].pid > 0) (void)kill(children[i].pid, SIGTERM);
    }
    uint64_t deadline = monotonic_ns() + UINT64_C(5000000000);
    while (monotonic_ns() < deadline && any_children_running(children, count)) {
        sleep_ms(50);
    }
    for (size_t i = 0; i < count; ++i) {
        if (children[i].pid > 0 && process_exists(children[i].pid)) {
            (void)kill(children[i].pid, SIGKILL);
        }
    }
    deadline = monotonic_ns() + UINT64_C(2000000000);
    for (;;) {
        int running = 0;
        for (size_t i = 0; i < count; ++i) {
            if (children[i].pid > 0 && process_exists(children[i].pid)) {
                running = 1;
                break;
            }
        }
        if (!running || monotonic_ns() >= deadline) break;
        sleep_ms(20);
    }
    for (size_t i = 0; i < count; ++i) {
        if (children[i].pid > 0 && process_exists(children[i].pid)) {
            fprintf(stderr, "QNX-Quiver supervisor: failed to stop %s pid=%ld\n",
                    children[i].name, (long)children[i].pid);
            continue;
        }
        if (children[i].pid > 0 && !children[i].wait_observed)
            (void)waitpid(children[i].pid, NULL, WNOHANG);
        children[i].pid = -1;
        children[i].wait_observed = 0;
    }
}

int main(int argc, char **argv)
{
    const char *bin_dir = argc == 2 ? argv[1] : "/opt/qnx-quiver/bin";
    if (argc > 2) {
        fprintf(stderr, "usage: %s [binary-directory]\n", argv[0]);
        return EXIT_FAILURE;
    }
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = stop_handler;
    (void)sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) == -1 ||
        sigaction(SIGTERM, &action, NULL) == -1) {
        perror("QNX-Quiver supervisor: sigaction");
        return EXIT_FAILURE;
    }
    char trace_path[PATH_CAPACITY];
    if (select_trace_path(trace_path, sizeof(trace_path)) == -1) {
        fprintf(stderr,
                "QNX-Quiver supervisor: no writable trace directory (/tmp or /dev/shmem)\n");
        return EXIT_FAILURE;
    }
    child_t pci = {.name = "PCI resource manager", .secpol_type = "quiver_pci_t",
                   .pid = -1, .restart_delay_ms = INITIAL_RESTART_MS};
    child_t children[CHILD_COUNT] = {
        {.name = "CAN RX", .secpol_type = "quiver_can_t", .pid = -1,
         .restart_delay_ms = INITIAL_RESTART_MS},
        {.name = "CAN TX", .secpol_type = "quiver_can_t", .pid = -1,
         .restart_delay_ms = INITIAL_RESTART_MS},
        {.name = "IMU consumer", .secpol_type = "quiver_imu_t", .pid = -1,
         .restart_delay_ms = INITIAL_RESTART_MS},
        {.name = "trace capture", .secpol_type = "quiver_trace_t", .pid = -1,
         .restart_delay_ms = INITIAL_RESTART_MS}
    };
    (void)snprintf(pci.path, sizeof(pci.path), "%s/quiver-pci-rm", bin_dir);
    pci.argv[0] = pci.path;
    (void)snprintf(children[0].path, PATH_CAPACITY, "%s/dev-can-quiver", bin_dir);
    (void)snprintf(children[1].path, PATH_CAPACITY, "%s/dev-can-quiver", bin_dir);
    (void)snprintf(children[2].path, PATH_CAPACITY, "%s/quiver-imu", bin_dir);
    (void)snprintf(children[3].path, PATH_CAPACITY, "%s/quiver-trace", bin_dir);
    children[0].argv[0] = children[0].path; children[0].argv[1] = "rx";
    children[1].argv[0] = children[1].path; children[1].argv[1] = "tx";
    children[2].argv[0] = children[2].path;
    children[3].argv[0] = children[3].path;
    children[3].argv[1] = trace_path;
    for (size_t i = 0; i < CHILD_COUNT; ++i) children[i].restart_at_ns = 0;

    if (start_child(&pci) == -1) return EXIT_FAILURE;
    printf("QNX-Quiver supervisor: trace output %s\n", trace_path);
    while (!stopping && wait_for_quiver_node(30000u) == -1) {
        if (child_exited(&pci)) {
            sleep_ms(pci.restart_delay_ms);
            if (!stopping && start_child(&pci) == -1) break;
        } else {
            fprintf(stderr, "QNX-Quiver supervisor: /dev/quiver not ready; retrying\n");
        }
    }
    int services_active = 0;
    if (!stopping && access("/dev/quiver", R_OK) == 0) {
        for (size_t i = 0; i < CHILD_COUNT; ++i) (void)start_child(&children[i]);
        services_active = 1;
    }
    while (!stopping) {
        uint64_t now = monotonic_ns();
        int quiver_ready = access("/dev/quiver", R_OK) == 0;
        if (pci.pid > 0) {
            (void)child_exited(&pci);
        } else if (now >= pci.restart_at_ns) {
            (void)start_child(&pci);
        }
        if (!quiver_ready && services_active) {
            fprintf(stderr, "QNX-Quiver supervisor: /dev/quiver withdrawn; stopping dependent services\n");
            stop_children(children, CHILD_COUNT);
            services_active = 0;
        } else if (quiver_ready && !services_active) {
            fprintf(stdout, "QNX-Quiver supervisor: /dev/quiver ready; starting dependent services\n");
            for (size_t i = 0; i < CHILD_COUNT; ++i) {
                if (children[i].pid <= 0) (void)start_child(&children[i]);
            }
            services_active = 1;
        }
        for (size_t i = 0; i < CHILD_COUNT; ++i) {
            child_t *child = &children[i];
            if (!services_active) {
                continue;
            } else if (child->pid > 0) {
                (void)child_exited(child);
            } else if (now >= child->restart_at_ns && quiver_ready) {
                (void)start_child(child);
            }
        }
        sleep_ms(100);
    }
    fprintf(stdout, "QNX-Quiver supervisor: shutting down\n");
    stop_children(children, CHILD_COUNT);
    stop_children(&pci, 1);
    return EXIT_SUCCESS;
}
