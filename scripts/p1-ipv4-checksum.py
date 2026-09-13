#!/usr/bin/env python3
import socket
import struct
import sys
import time

IDENT = 0x5453
BAD_SEQ = 0x1001
GOOD_SEQ = 0x1002
PAYLOAD = b"tcp-shift-p1-checksum"


def checksum(data: bytes) -> int:
    if len(data) & 1:
        data += b"\x00"
    total = 0
    for offset in range(0, len(data), 2):
        total += (data[offset] << 8) | data[offset + 1]
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def ipv4_header(source: str, destination: str, payload_len: int) -> bytes:
    version_ihl = (4 << 4) | 5
    total_len = 20 + payload_len
    return struct.pack(
        "!BBHHHBBH4s4s",
        version_ihl,
        0,
        total_len,
        0x5453,
        0x4000,
        64,
        socket.IPPROTO_ICMP,
        0,
        socket.inet_aton(source),
        socket.inet_aton(destination),
    )


def icmp_echo(sequence: int, valid: bool) -> bytes:
    header = struct.pack("!BBHHH", 8, 0, 0, IDENT, sequence)
    good = checksum(header + PAYLOAD)
    value = good if valid else (good ^ 0xFFFF)
    if value == good:
        value ^= 0x0001
    return struct.pack("!BBHHH", 8, 0, value, IDENT, sequence) + PAYLOAD


def receive_reply(sock: socket.socket, sequence: int, timeout: float) -> bool:
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return False
        sock.settimeout(remaining)
        try:
            packet, _ = sock.recvfrom(65535)
        except socket.timeout:
            return False
        if len(packet) < 28:
            continue
        ihl = (packet[0] & 0x0F) * 4
        if ihl < 20 or len(packet) < ihl + 8:
            continue
        icmp_type, code, _, ident, seq = struct.unpack(
            "!BBHHH", packet[ihl : ihl + 8]
        )
        if icmp_type == 0 and code == 0 and ident == IDENT and seq == sequence:
            return True


def send_echo(sock: socket.socket, source: str, destination: str,
              sequence: int, valid: bool) -> None:
    icmp = icmp_echo(sequence, valid)
    packet = ipv4_header(source, destination, len(icmp)) + icmp
    sock.sendto(packet, (destination, 0))


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <source-ipv4> <destination-ipv4>", file=sys.stderr)
        return 2

    source, destination = sys.argv[1:]
    sock = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_ICMP)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_HDRINCL, 1)

    send_echo(sock, source, destination, BAD_SEQ, valid=False)
    if receive_reply(sock, BAD_SEQ, 0.35):
        print("bad checksum unexpectedly received echo reply", file=sys.stderr)
        return 1

    send_echo(sock, source, destination, GOOD_SEQ, valid=True)
    if not receive_reply(sock, GOOD_SEQ, 1.0):
        print("valid checksum did not receive echo reply", file=sys.stderr)
        return 1

    print("icmp_checksum_bad_reply=none icmp_checksum_good_reply=received")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
