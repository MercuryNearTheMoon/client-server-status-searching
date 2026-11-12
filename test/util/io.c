#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "util/io.h"

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #expr, errno); \
        return 1; \
    } \
} while (0)

static int wait_success(pid_t pid)
{
    int status;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR)
            return 0;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* Independent peer operations: do not test send_all against recv_all only. */
static int raw_transfer(int fd, unsigned char *data, size_t length, int writing)
{
    size_t done = 0;
    while (done < length) {
        ssize_t n = writing ? write(fd, data + done, length - done)
                            : read(fd, data + done, length - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return 0;
        done += (size_t)n;
    }
    return 1;
}

static int zero_send(void)
{
    CHECK(sysinfo_send_all(-1, NULL, 0) == SYSINFO_IO_OK);
    return 0;
}

static int zero_recv(void)
{
    CHECK(sysinfo_recv_all(-1, NULL, 0) == SYSINFO_IO_OK);
    return 0;
}

static int invalid_send(void)
{
    char byte = 0;
    errno = 0;
    CHECK(sysinfo_send_all(-1, &byte, 1) == SYSINFO_IO_ERROR);
    CHECK(errno == EBADF);
    return 0;
}

static int invalid_recv(void)
{
    char byte = 0;
    errno = 0;
    CHECK(sysinfo_recv_all(-1, &byte, 1) == SYSINFO_IO_ERROR);
    CHECK(errno == EBADF);
    return 0;
}

static int send_bytes(void)
{
    int sv[2];
    unsigned char input[] = {0, 1, 0x80, 0xFF, 0, 42};
    unsigned char output[sizeof(input)];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    CHECK(sysinfo_send_all(sv[0], input, sizeof(input)) == SYSINFO_IO_OK);
    CHECK(raw_transfer(sv[1], output, sizeof(output), 0));
    CHECK(memcmp(input, output, sizeof(input)) == 0);
    /* The helper must not close the caller-owned socket. */
    CHECK(shutdown(sv[0], SHUT_WR) == 0);
    CHECK(read(sv[1], output, 1) == 0);
    close(sv[0]);
    close(sv[1]);
    return 0;
}

static int recv_exact(void)
{
    int sv[2];
    unsigned char input[] = {0, 0x80, 0xFF, 42};
    unsigned char output[] = {0xA5, 0xA5, 0xA5, 0xA5, 0xA5};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    CHECK(raw_transfer(sv[1], input, sizeof(input), 1));
    CHECK(sysinfo_recv_all(sv[0], output + 1, 3) == SYSINFO_IO_OK);
    CHECK(output[0] == 0xA5 && output[4] == 0xA5);
    CHECK(memcmp(output + 1, input, 3) == 0);
    CHECK(sysinfo_recv_all(sv[0], output + 1, 1) == SYSINFO_IO_OK);
    CHECK(output[1] == 42); /* No over-read on the previous call. */
    close(sv[0]);
    close(sv[1]);
    return 0;
}

static int receive_end(size_t sent, size_t wanted, int expected)
{
    int sv[2];
    unsigned char input[] = {1, 2, 3, 4};
    unsigned char output[4] = {0};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    CHECK(raw_transfer(sv[1], input, sent, 1));
    CHECK(shutdown(sv[1], SHUT_WR) == 0);
    CHECK(sysinfo_recv_all(sv[0], output, wanted) == expected);
    CHECK(memcmp(input, output, sent) == 0);
    close(sv[0]);
    close(sv[1]);
    return 0;
}

static int initial_eof(void) { return receive_end(0, 4, SYSINFO_IO_EOF); }
static int partial_eof(void) { return receive_end(2, 4, SYSINFO_IO_TRUNCATED); }
static int complete_then_eof(void) { return receive_end(4, 4, SYSINFO_IO_OK); }

static int disconnected_send(void)
{
    int sv[2];
    char byte = 42;
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    CHECK(signal(SIGPIPE, SIG_DFL) != SIG_ERR);
    close(sv[1]);
    CHECK(sysinfo_send_all(sv[0], &byte, 1) == SYSINFO_IO_ERROR);
    CHECK(errno == EPIPE || errno == ECONNRESET);
    close(sv[0]);
    return 0;
}

static int receive_timeout(void)
{
    int sv[2];
    char byte;
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 100000};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    CHECK(setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    CHECK(sysinfo_recv_all(sv[0], &byte, 1) == SYSINFO_IO_ERROR);
    CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
    close(sv[0]);
    close(sv[1]);
    return 0;
}

static int null_buffers(void)
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    CHECK(sysinfo_send_all(sv[0], NULL, 1) == SYSINFO_IO_ERROR);
    CHECK(errno == EINVAL);
    CHECK(sysinfo_recv_all(sv[0], NULL, 1) == SYSINFO_IO_ERROR);
    CHECK(errno == EINVAL);
    close(sv[0]);
    close(sv[1]);
    return 0;
}

static int send_timeout(void)
{
    int sv[2];
    enum { LENGTH = 1024 * 1024 };
    unsigned char *data = calloc(LENGTH, 1);
    CHECK(data != NULL);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    int small_buffer = 4096;
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 100000};
    CHECK(setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &small_buffer, sizeof(small_buffer)) == 0);
    CHECK(setsockopt(sv[0], SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
    CHECK(sysinfo_send_all(sv[0], data, LENGTH) == SYSINFO_IO_ERROR);
    CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
    free(data);
    close(sv[0]);
    close(sv[1]);
    return 0;
}

static int partial_timeout(void)
{
    int sv[2];
    unsigned char input = 42;
    unsigned char output[2] = {0, 0xA5};
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 100000};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    CHECK(setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    CHECK(write(sv[1], &input, 1) == 1);
    CHECK(sysinfo_recv_all(sv[0], output, sizeof(output)) == SYSINFO_IO_ERROR);
    CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
    CHECK(output[0] == 42 && output[1] == 0xA5);
    close(sv[0]);
    close(sv[1]);
    return 0;
}

static volatile sig_atomic_t interrupted;
static void on_signal(int signo)
{
    (void)signo;
    interrupted = 1;
}

static int interrupted_receive(void)
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    struct sigaction action = {0};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    /* No SA_RESTART: the receive should handle EINTR itself. */
    CHECK(sigaction(SIGALRM, &action, NULL) == 0);
    pid_t peer = fork();
    CHECK(peer >= 0);
    if (peer == 0) {
        signal(SIGALRM, SIG_DFL);
        alarm(5);
        close(sv[0]);
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000000};
        unsigned char bytes[] = {1, 2, 3, 4};
        for (size_t i = 0; i < sizeof(bytes); ++i) {
            while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
            delay.tv_nsec = 100000000;
            if (write(sv[1], bytes + i, 1) != 1)
                _exit(1);
        }
        close(sv[1]);
        _exit(0);
    }
    close(sv[1]);
    struct itimerval timer = {
        .it_interval = {.tv_sec = 0, .tv_usec = 10000},
        .it_value = {.tv_sec = 0, .tv_usec = 10000}
    };
    CHECK(setitimer(ITIMER_REAL, &timer, NULL) == 0);
    unsigned char output[4];
    int result = sysinfo_recv_all(sv[0], output, sizeof(output));
    struct itimerval stopped = {0};
    CHECK(setitimer(ITIMER_REAL, &stopped, NULL) == 0);
    CHECK(signal(SIGALRM, SIG_DFL) != SIG_ERR);
    alarm(5);
    CHECK(wait_success(peer));
    CHECK(result == SYSINFO_IO_OK && interrupted);
    CHECK(memcmp(output, "\1\2\3\4", 4) == 0);
    close(sv[0]);
    return 0;
}

static int large_send(void)
{
    int sv[2];
    enum { LENGTH = 256 * 1024 };
    unsigned char *data = malloc(LENGTH);
    CHECK(data != NULL);
    for (size_t i = 0; i < LENGTH; ++i)
        data[i] = (unsigned char)(i % 251);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    int small_buffer = 4096;
    CHECK(setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &small_buffer, sizeof(small_buffer)) == 0);
    pid_t peer = fork();
    CHECK(peer >= 0);
    if (peer == 0) {
        alarm(5);
        close(sv[0]);
        unsigned char chunk[1024];
        size_t done = 0;
        while (done < LENGTH) {
            if (!raw_transfer(sv[1], chunk, sizeof(chunk), 0))
                _exit(1);
            if (memcmp(chunk, data + done, sizeof(chunk)) != 0)
                _exit(1);
            done += sizeof(chunk);
        }
        close(sv[1]);
        _exit(0);
    }
    close(sv[1]);
    int result = sysinfo_send_all(sv[0], data, LENGTH);
    close(sv[0]);
    CHECK(wait_success(peer));
    CHECK(result == SYSINFO_IO_OK);
    free(data);
    return 0;
}

int main(void)
{
    const struct {
        const char *name;
        int (*run)(void);
    } tests[] = {
        {"zero-length send", zero_send},
        {"zero-length receive", zero_recv},
        {"invalid send fd", invalid_send},
        {"invalid receive fd", invalid_recv},
        {"binary send and socket ownership", send_bytes},
        {"exact receive and buffer boundaries", recv_exact},
        {"EOF before any bytes", initial_eof},
        {"EOF after partial data", partial_eof},
        {"complete data before EOF", complete_then_eof},
        {"disconnected send suppresses SIGPIPE", disconnected_send},
        {"receive timeout", receive_timeout},
        {"NULL buffers", null_buffers},
        {"send timeout under backpressure", send_timeout},
        {"receive timeout after partial data", partial_timeout},
        {"fragmented receive with signals", interrupted_receive},
        {"large send with small socket buffer", large_send}
    };
    size_t total = sizeof(tests) / sizeof(tests[0]);
    size_t failed = 0;
    for (size_t i = 0; i < total; ++i) {
        fflush(NULL);
        pid_t child = fork();
        if (child < 0) {
            perror("fork");
            return EXIT_FAILURE;
        }
        if (child == 0) {
            setpgid(0, 0);
            signal(SIGALRM, SIG_DFL);
            alarm(5);
            _exit(tests[i].run());
        }
        /* Parent watchdog also bounds tests that temporarily replace SIGALRM. */
        setpgid(child, child);
        int status = 0;
        int finished = 0;
        for (int tick = 0; tick < 600; ++tick) {
            pid_t result = waitpid(child, &status, WNOHANG);
            if (result == child) { finished = 1; break; }
            if (result < 0 && errno != EINTR) break;
            struct timespec delay = {.tv_sec = 0, .tv_nsec = 10000000};
            nanosleep(&delay, NULL);
        }
        /* Clean up the isolated test's process group, including helper peers. */
        kill(-child, SIGKILL);
        if (!finished) {
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        }
        int passed = finished && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        printf("[%s] %s", passed ? "PASS" : "FAIL", tests[i].name);
        if (!finished) printf(" (timeout/wait failure)");
        else if (WIFSIGNALED(status)) printf(" (signal %d)", WTERMSIG(status));
        putchar('\n');
        failed += !passed;
    }
    printf("\n%zu/%zu passed; %zu failed\n", total - failed, total, failed);
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
