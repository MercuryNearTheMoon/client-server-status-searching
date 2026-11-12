#ifndef SYSINFO_IO_H
#define SYSINFO_IO_H

#include <stddef.h>

enum sysinfo_io_result {
    SYSINFO_IO_OK = 0,
    SYSINFO_IO_EOF = 1,       /* Peer closed before any byte was received. */
    SYSINFO_IO_TRUNCATED = 2, /* Peer closed after a partial receive. */
    SYSINFO_IO_ERROR = -1
};

/* Blocking socket helpers. Retry EINTR and handle short reads/writes.
 * A zero length succeeds immediately. These interfaces impose no deadline;
 * callers must configure socket timeouts when bounded waits are required.
 * A NULL buffer with nonzero length returns ERROR with errno=EINVAL.
 * On ERROR, errno describes the failure; partial transfer is possible.
 */

/* Send exactly length bytes; return OK or ERROR. Suppress SIGPIPE. */
int sysinfo_send_all(int fd, const void *buffer, size_t length);

/* Receive exactly length bytes; return one of sysinfo_io_result. */
int sysinfo_recv_all(int fd, void *buffer, size_t length);

#endif /* SYSINFO_IO_H */
