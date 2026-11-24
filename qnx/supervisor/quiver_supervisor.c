#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHILD_COUNT 4u
#define PATH_CAPACITY 512u

typedef struct {
    const char *name;
    char path[PATH_CAPACITY];
    char *argv[4];
    pid_t pid;
    unsigned restart_delay_ms;
    uint64_t restart_at_ns;
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
    pid_t pid = fork();
    if (pid == -1) {
        fprintf(stderr, "QNX-Quiver supervisor: fork %s: %s\n",
                child->name, strerror(errno));
        return -1;
    }
    if (pid == 0) {
        execv(child->path, child->argv);
        dprintf(STDERR_FILENO, "QNX-Quiver supervisor: exec %s: %s\n",
                child->path, strerror(errno));
        _exit(127);
    }
    child->pid = pid;
    printf("QNX-Quiver supervisor: started %s pid=%ld\n",
           child->name, (long)pid);
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

static void stop_children(child_t children[], size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (children[i].pid > 0) (void)kill(children[i].pid, SIGTERM);
    }
    uint64_t deadline = monotonic_ns() + UINT64_C(5000000000);
    while (monotonic_ns() < deadline) {
        int running = 0;
        for (size_t i = 0; i < count; ++i) {
            if (children[i].pid > 0 &&
                waitpid(children[i].pid, NULL, WNOHANG) == children[i].pid) {
                children[i].pid = -1;
            }
            if (children[i].pid > 0) running = 1;
        }
        if (!running) return;
        sleep_ms(50);
    }
    for (size_t i = 0; i < count; ++i) {
        if (children[i].pid > 0) {
            (void)kill(children[i].pid, SIGKILL);
            (void)waitpid(children[i].pid, NULL, 0);
            children[i].pid = -1;
        }
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
    child_t pci = {.name = "PCI resource manager", .pid = -1,
                   .restart_delay_ms = 250};
    child_t children[CHILD_COUNT] = {
        {.name = "CAN RX", .pid = -1, .restart_delay_ms = 250},
        {.name = "CAN TX", .pid = -1, .restart_delay_ms = 250},
        {.name = "IMU consumer", .pid = -1, .restart_delay_ms = 250},
        {.name = "trace capture", .pid = -1, .restart_delay_ms = 250}
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
    children[3].argv[1] = "/tmp/quiver-supervisor-trace.csv";
    for (size_t i = 0; i < CHILD_COUNT; ++i) children[i].restart_at_ns = 0;

    if (start_child(&pci) == -1) return EXIT_FAILURE;
    while (!stopping && wait_for_quiver_node(30000u) == -1) {
        int child_status;
        if (waitpid(pci.pid, &child_status, WNOHANG) == pci.pid) {
            fprintf(stderr, "QNX-Quiver supervisor: PCI manager exited; retrying\n");
            sleep_ms(pci.restart_delay_ms);
            pci.restart_delay_ms = pci.restart_delay_ms < 15000u
                                       ? pci.restart_delay_ms * 2u : 30000u;
            if (start_child(&pci) == -1) break;
        } else {
            fprintf(stderr, "QNX-Quiver supervisor: /dev/quiver not ready; retrying\n");
        }
    }
    if (!stopping && access("/dev/quiver", R_OK) == 0) {
        for (size_t i = 0; i < CHILD_COUNT; ++i) (void)start_child(&children[i]);
    }
    while (!stopping) {
        uint64_t now = monotonic_ns();
        if (pci.pid > 0) {
            int pci_status;
            if (waitpid(pci.pid, &pci_status, WNOHANG) == pci.pid) {
                fprintf(stderr, "QNX-Quiver supervisor: PCI manager exited (status=%d)\n",
                        pci_status);
                pci.pid = -1;
                pci.restart_at_ns = now +
                    (uint64_t)pci.restart_delay_ms * 1000000u;
                if (pci.restart_delay_ms < 15000u) pci.restart_delay_ms *= 2u;
                if (pci.restart_delay_ms > 30000u) pci.restart_delay_ms = 30000u;
            }
        } else if (now >= pci.restart_at_ns) {
            if (start_child(&pci) == 0) pci.restart_delay_ms = 250u;
        }
        for (size_t i = 0; i < CHILD_COUNT; ++i) {
            child_t *child = &children[i];
            if (child->pid > 0) {
                int child_status;
                pid_t result = waitpid(child->pid, &child_status, WNOHANG);
                if (result == child->pid) {
                    fprintf(stderr, "QNX-Quiver supervisor: %s exited (status=%d)\n",
                            child->name, child_status);
                    child->pid = -1;
                    child->restart_at_ns = now +
                        (uint64_t)child->restart_delay_ms * 1000000u;
                    if (child->restart_delay_ms < 15000u)
                        child->restart_delay_ms *= 2u;
                    if (child->restart_delay_ms > 30000u)
                        child->restart_delay_ms = 30000u;
                }
            } else if (now >= child->restart_at_ns && access("/dev/quiver", R_OK) == 0) {
                if (start_child(child) == 0) child->restart_delay_ms = 250u;
            }
        }
        sleep_ms(100);
    }
    fprintf(stdout, "QNX-Quiver supervisor: shutting down\n");
    stop_children(children, CHILD_COUNT);
    if (pci.pid > 0) {
        (void)kill(pci.pid, SIGTERM);
        (void)waitpid(pci.pid, NULL, 0);
    }
    return EXIT_SUCCESS;
}
