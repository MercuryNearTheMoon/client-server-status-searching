#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>

#include "io.h"
#include "logger.h"

int sysinfo_send_all(int fd, const void *buffer, size_t length){
    if (length == 0) return SYSINFO_IO_OK;
    if (buffer == NULL) {
        SYSINFO_ERROR_LOG("send: NULL buffer");
        errno = EINVAL;
        return SYSINFO_IO_ERROR;
    }

    int flags = 0;
#if defined(MSG_NOSIGNAL)
    flags = MSG_NOSIGNAL;
#elif defined(SO_NOSIGPIPE)
    int enabled = 1;
    int result;
    do {
        result = setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        int saved_errno = errno;
        SYSINFO_ERROR_LOG("send: SO_NOSIGPIPE fd=%d errno=%d (%s)",
                         fd, saved_errno, strerror(saved_errno));
        errno = saved_errno;
        return SYSINFO_IO_ERROR;
    }
#else
#error "This platform needs a socket-local SIGPIPE suppression mechanism"
#endif

    const char *ptr = (const char *)buffer;
    size_t total_sent = 0;

    while (total_sent < length){
        ssize_t bytes_sent = send(fd, ptr + total_sent, length - total_sent, flags);

        if (bytes_sent < 0) {
            int saved_errno = errno;
            if (saved_errno == EINTR) continue;
            SYSINFO_ERROR_LOG("send: fd=%d progress=%zu/%zu errno=%d (%s)",
                             fd, total_sent, length, saved_errno, strerror(saved_errno));
            errno = saved_errno;
            return SYSINFO_IO_ERROR;
        }
        else if (bytes_sent == 0) {
            SYSINFO_ERROR_LOG("send: fd=%d no progress at %zu/%zu", fd, total_sent, length);
            errno = EIO;
            return SYSINFO_IO_ERROR;
        }
        total_sent += (size_t)bytes_sent;
    }

    SYSINFO_DEBUG("send: fd=%d completed %zu bytes", fd, total_sent);
    return SYSINFO_IO_OK;
}

int sysinfo_recv_all(int fd, void *buffer, size_t length){
    if (length == 0) return SYSINFO_IO_OK;
    if (buffer == NULL) {
        SYSINFO_ERROR_LOG("read: NULL buffer");
        errno = EINVAL;
        return SYSINFO_IO_ERROR;
    }

    char *ptr = (char *)buffer;
    size_t total_read = 0;

    while (total_read < length){
        ssize_t bytes_read = read(fd, ptr + total_read, length - total_read);

        if (bytes_read < 0) {
            int saved_errno = errno;
            if (saved_errno == EINTR) continue;
            SYSINFO_ERROR_LOG("read: fd=%d progress=%zu/%zu errno=%d (%s)",
                             fd, total_read, length, saved_errno, strerror(saved_errno));
            errno = saved_errno;
            return SYSINFO_IO_ERROR;
        }
        else if (bytes_read == 0) {
            if (total_read != 0) {
                SYSINFO_WARN_LOG("read: fd=%d truncated at %zu/%zu", fd, total_read, length);
            }
            return total_read == 0 ? SYSINFO_IO_EOF : SYSINFO_IO_TRUNCATED;
        }
        total_read += (size_t)bytes_read;
    }

    SYSINFO_DEBUG("read: fd=%d completed %zu bytes", fd, total_read);
    return SYSINFO_IO_OK;
}
