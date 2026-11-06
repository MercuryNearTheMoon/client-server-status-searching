#include <arpa/inet.h>
#include <inttypes.h>
#include <string.h>

#include "logger.h"
#include "protocol.h"

int sysinfo_header_encode(uint8_t out[SYSINFO_HEADER_SIZE],
                          const struct sysinfo_header *header)
{
    if (out == NULL || header == NULL) {
        SYSINFO_ERROR_LOG("encode: NULL output or header");
        return SYSINFO_ERROR;
    }

    SYSINFO_DEBUG("encode: magic=0x%08" PRIX32
                  " version=%u type=%u flags=%u payload=%" PRIu32,
                  header->magic, (unsigned)header->version, (unsigned)header->type,
                  (unsigned)header->flags, header->payload_length);

    /* Validate host-order fields before converting or writing output. */
    const char *invalid = NULL;
    if (header->magic != SYSINFO_PROTOCOL_MAGIC)
        invalid = "invalid magic";
    else if (header->version != SYSINFO_PROTOCOL_VERSION)
        invalid = "unsupported version";
    else if (header->type < SYSINFO_REQUEST || header->type > SYSINFO_ERROR_RESPONSE)
        invalid = "type must be 1..3";
    else if (header->flags != 0)
        invalid = "flags must be zero";
    else if (header->payload_length > SYSINFO_MAX_PAYLOAD)
        invalid = "payload exceeds maximum";
    if (invalid != NULL) {
        SYSINFO_WARN_LOG("encode: rejected header: %s", invalid);
        return SYSINFO_ERROR;
    }

    uint32_t magic = htonl(header->magic);
    memcpy(out, &magic, sizeof(magic));

    out[4] = header->version;
    out[5] = header->type;

    uint16_t flag = htons(header->flags);
    memcpy(out + 6, &flag, sizeof(flag));

    uint32_t payload_length = htonl(header->payload_length);
    memcpy(out + 8, &payload_length, sizeof(payload_length));

    SYSINFO_INFO_LOG("encode: wrote %u-byte header", SYSINFO_HEADER_SIZE);
    return SYSINFO_OK;
}

int sysinfo_header_decode(struct sysinfo_header *header,
                          const uint8_t in[SYSINFO_HEADER_SIZE])
{
    if (header == NULL || in == NULL) {
        SYSINFO_ERROR_LOG("decode: NULL output or input");
        return SYSINFO_ERROR;
    }

    uint32_t magic;
    memcpy(&magic, in, sizeof(magic));
    magic = ntohl(magic);

    uint8_t version = in[4];
    uint8_t type = in[5];

    uint16_t flags;
    memcpy(&flags, in + 6, sizeof(flags));
    flags = ntohs(flags);

    uint32_t payload_length;
    memcpy(&payload_length, in + 8, sizeof(payload_length));
    payload_length = ntohl(payload_length);

    const char *invalid = NULL;
    if (magic != SYSINFO_PROTOCOL_MAGIC)
        invalid = "invalid magic";
    else if (version != SYSINFO_PROTOCOL_VERSION)
        invalid = "unsupported version";
    else if (type < SYSINFO_REQUEST || type > SYSINFO_ERROR_RESPONSE)
        invalid = "type must be 1..3";
    else if (flags != 0)
        invalid = "flags must be zero";
    else if (payload_length > SYSINFO_MAX_PAYLOAD)
        invalid = "payload exceeds maximum";
    if (invalid != NULL) {
        SYSINFO_WARN_LOG("decode: rejected header: %s", invalid);
        return SYSINFO_ERROR;
    }

    header->magic = magic;
    header->version = version;
    header->type = type;
    header->flags = flags;
    header->payload_length = payload_length;

    return SYSINFO_OK;
}
