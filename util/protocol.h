#ifndef SYSINFO_PROTOCOL_H
#define SYSINFO_PROTOCOL_H

#include <stdint.h>
#include "common.h"

#define SYSINFO_PROTOCOL_MAGIC UINT32_C(0x53494E46) /* "SINF" */
#define SYSINFO_PROTOCOL_VERSION 1U
#define SYSINFO_HEADER_SIZE 12U

enum sysinfo_message_type {
    SYSINFO_REQUEST = 1,
    SYSINFO_RESPONSE = 2,
    SYSINFO_ERROR_RESPONSE = 3
};

enum sysinfo_command {
    SYSINFO_CMD_HOSTNAME = 1,
    SYSINFO_CMD_UNAME = 2,
    SYSINFO_CMD_UPTIME = 3,
    SYSINFO_CMD_MEMORY = 4,
    SYSINFO_CMD_LOADAVG = 5,
    SYSINFO_CMD_HOLD = 6
};

/* In-memory representation only: never send this struct directly.
 * Wire layout: magic(4), version(1), type(1), flags(2), payload_length(4).
 * Multi-byte fields use network byte order. Version 1 requires flags == 0.
 */
struct sysinfo_header {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint16_t flags;
    uint32_t payload_length;
};

/* Return SYSINFO_OK or SYSINFO_ERROR. Validate magic, version, type,
 * flags and payload_length <= SYSINFO_MAX_PAYLOAD in both directions.
 * Buffers must contain at least SYSINFO_HEADER_SIZE bytes.
 */
int sysinfo_header_encode(uint8_t out[SYSINFO_HEADER_SIZE],
                          const struct sysinfo_header *header);
int sysinfo_header_decode(struct sysinfo_header *header,
                          const uint8_t in[SYSINFO_HEADER_SIZE]);

#endif /* SYSINFO_PROTOCOL_H */
