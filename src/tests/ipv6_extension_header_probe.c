#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef IPV6_HDRINCL
#define IPV6_HDRINCL 36
#endif

#define IPV6_HEADER_BYTES 40U
#define HOP_BY_HOP_BYTES 8U
#define TCP_HEADER_BYTES 20U
#define PACKET_BYTES (IPV6_HEADER_BYTES + HOP_BY_HOP_BYTES + TCP_HEADER_BYTES)

static void put_u16(unsigned char *p, uint16_t value)
{
    p[0] = (unsigned char)(value >> 8);
    p[1] = (unsigned char)(value & 0xffU);
}

static void put_u32(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)(value >> 24);
    p[1] = (unsigned char)((value >> 16) & 0xffU);
    p[2] = (unsigned char)((value >> 8) & 0xffU);
    p[3] = (unsigned char)(value & 0xffU);
}

static uint32_t checksum_add(uint32_t sum,
                             const unsigned char *data,
                             size_t length)
{
    while (length >= 2U) {
        sum += ((uint32_t)data[0] << 8) | data[1];
        data += 2;
        length -= 2U;
    }
    if (length != 0U) {
        sum += (uint32_t)data[0] << 8;
    }
    return sum;
}

static uint16_t checksum_finish(uint32_t sum)
{
    while ((sum >> 16) != 0U) {
        sum = (sum & 0xffffU) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static uint16_t tcp6_checksum(const struct in6_addr *source,
                              const struct in6_addr *destination,
                              const unsigned char *tcp,
                              size_t tcp_length)
{
    unsigned char pseudo_tail[8] = {0};
    uint32_t sum = 0U;

    put_u32(pseudo_tail, (uint32_t)tcp_length);
    pseudo_tail[7] = IPPROTO_TCP;
    sum = checksum_add(sum, source->s6_addr, sizeof(source->s6_addr));
    sum = checksum_add(sum, destination->s6_addr,
                       sizeof(destination->s6_addr));
    sum = checksum_add(sum, pseudo_tail, sizeof(pseudo_tail));
    sum = checksum_add(sum, tcp, tcp_length);
    return checksum_finish(sum);
}

static int parse_port(const char *text, uint16_t *port)
{
    char *end = NULL;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0UL ||
        value > 65535UL) {
        return -1;
    }
    *port = (uint16_t)value;
    return 0;
}

int main(int argc, char **argv)
{
    struct sockaddr_in6 peer;
    struct in6_addr source;
    struct in6_addr destination;
    unsigned char packet[PACKET_BYTES] = {0};
    unsigned char *tcp = packet + IPV6_HEADER_BYTES + HOP_BY_HOP_BYTES;
    uint16_t source_port;
    uint16_t destination_port;
    uint16_t checksum;
    int fd;
    int enabled = 1;
    ssize_t sent;

    if (argc != 5) {
        fprintf(stderr,
                "usage: %s <source-ipv6> <destination-ipv6> "
                "<source-port> <destination-port>\n",
                argv[0]);
        return EXIT_FAILURE;
    }
    if (inet_pton(AF_INET6, argv[1], &source) != 1 ||
        inet_pton(AF_INET6, argv[2], &destination) != 1 ||
        parse_port(argv[3], &source_port) < 0 ||
        parse_port(argv[4], &destination_port) < 0) {
        fprintf(stderr, "invalid IPv6 extension-header probe arguments\n");
        return EXIT_FAILURE;
    }

    packet[0] = 0x60U;
    put_u16(packet + 4U, (uint16_t)(HOP_BY_HOP_BYTES + TCP_HEADER_BYTES));
    packet[6] = IPPROTO_HOPOPTS;
    packet[7] = 64U;
    memcpy(packet + 8U, source.s6_addr, sizeof(source.s6_addr));
    memcpy(packet + 24U, destination.s6_addr, sizeof(destination.s6_addr));

    /* One 8-byte Hop-by-Hop header: TCP follows, and six Pad1 options fill it. */
    packet[IPV6_HEADER_BYTES] = IPPROTO_TCP;
    packet[IPV6_HEADER_BYTES + 1U] = 0U;

    put_u16(tcp, source_port);
    put_u16(tcp + 2U, destination_port);
    put_u32(tcp + 4U, 1U);
    tcp[12] = 0x50U;
    tcp[13] = 0x02U;
    put_u16(tcp + 14U, 65535U);
    checksum = tcp6_checksum(&source, &destination, tcp, TCP_HEADER_BYTES);
    put_u16(tcp + 16U, checksum);

    memset(&peer, 0, sizeof(peer));
    peer.sin6_family = AF_INET6;
    peer.sin6_addr = destination;

    fd = socket(AF_INET6, SOCK_RAW, IPPROTO_RAW);
    if (fd < 0) {
        perror("socket(AF_INET6, SOCK_RAW)");
        return EXIT_FAILURE;
    }
    if (setsockopt(fd, IPPROTO_IPV6, IPV6_HDRINCL, &enabled,
                   sizeof(enabled)) < 0) {
        perror("setsockopt(IPV6_HDRINCL)");
        close(fd);
        return EXIT_FAILURE;
    }

    sent = sendto(fd, packet, sizeof(packet), 0,
                  (const struct sockaddr *)&peer, sizeof(peer));
    if (sent != (ssize_t)sizeof(packet)) {
        if (sent < 0) {
            perror("sendto extension-header probe");
        } else {
            fprintf(stderr, "short raw IPv6 send: %zd/%zu\n", sent,
                    sizeof(packet));
        }
        close(fd);
        return EXIT_FAILURE;
    }
    close(fd);

    printf("ipv6_extension_header_syn_sent src=[%s]:%u dst=[%s]:%u bytes=%zu\n",
           argv[1], (unsigned)source_port, argv[2],
           (unsigned)destination_port, sizeof(packet));
    return EXIT_SUCCESS;
}
