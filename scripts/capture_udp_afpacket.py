#!/usr/bin/env python3
"""Passive Linux Ethernet capture using AF_PACKET and a kernel cBPF filter."""
import argparse
import ctypes
import ipaddress
import json
import os
import signal
import socket
import struct
import sys
import time

SO_ATTACH_FILTER = 26
SO_TIMESTAMPNS_NEW = 64
SOL_PACKET = 263
PACKET_STATISTICS = 6
PACKET_OUTGOING = 4
SNAPLEN = 65535


def packet_filter(destination, packet_type=PACKET_OUTGOING):
    """Outgoing IPv4 UDP to one destination, including fragments/0..2 VLAN tags."""
    instructions, labels = [], {}
    def mark(name):
        labels[name] = len(instructions)
    def emit(code, value):
        instructions.append((code, 0, 0, value))
    def branch(value, yes, no):
        instructions.append((0x15, yes, no, value))  # JEQ K
    emit(0x20, 0xfffff004)  # LD W ABS SKF_AD_OFF + SKF_AD_PKTTYPE
    branch(packet_type, 'ethernet', 'drop')
    mark('ethernet')
    emit(0x01, 14)  # LDX IMM: IP header offset
    emit(0x28, 12)  # LD H ABS: EtherType
    branch(0x0800, 'ip', 'tag1type')
    mark('tag1type')
    branch(0x8100, 'tag1', 'tag1ad')
    mark('tag1ad')
    branch(0x88a8, 'tag1', 'drop')
    mark('tag1')
    emit(0x01, 18)
    emit(0x28, 16)
    branch(0x0800, 'ip', 'tag2type')
    mark('tag2type')
    branch(0x8100, 'tag2', 'tag2ad')
    mark('tag2ad')
    branch(0x88a8, 'tag2', 'drop')
    mark('tag2')
    emit(0x01, 22)
    emit(0x28, 20)
    branch(0x0800, 'ip', 'drop')
    mark('ip')
    emit(0x50, 9)  # LD B IND: IPv4 protocol
    branch(17, 'destination', 'drop')
    mark('destination')
    emit(0x40, 16)  # LD W IND: IPv4 destination
    branch(int(ipaddress.IPv4Address(destination)), 'accept', 'drop')
    mark('accept')
    emit(0x06, SNAPLEN)
    mark('drop')
    emit(0x06, 0)
    return [(code, labels[yes] - index - 1 if isinstance(yes, str) else yes,
             labels[no] - index - 1 if isinstance(no, str) else no, value)
            for index, (code, yes, no, value) in enumerate(instructions)]


def attach_filter(sock, instructions):
    class SockFilter(ctypes.Structure):
        _fields_ = [('code', ctypes.c_ushort), ('jt', ctypes.c_ubyte),
                    ('jf', ctypes.c_ubyte), ('k', ctypes.c_uint32)]
    class SockFprog(ctypes.Structure):
        _fields_ = [('length', ctypes.c_ushort), ('filters', ctypes.POINTER(SockFilter))]
    filters = (SockFilter * len(instructions))(*(SockFilter(*item) for item in instructions))
    program = SockFprog(len(instructions), filters)
    sock.setsockopt(socket.SOL_SOCKET, SO_ATTACH_FILTER, bytes(program))


def receive_to_pcap(sock, output, stop, stats):
    output.write(struct.pack('<IHHIIII', 0xa1b23c4d, 2, 4, 0, 0, SNAPLEN, 1))
    output.flush()
    buffer = bytearray(SNAPLEN)
    view = memoryview(buffer)
    last_flush = time.monotonic()
    while not stop():
        try:
            wire_length, controls, flags, _ = sock.recvmsg_into([buffer], 256, socket.MSG_TRUNC)
        except socket.timeout:
            continue
        timestamp = None
        for level, kind, data in controls:
            if level == socket.SOL_SOCKET and kind == SO_TIMESTAMPNS_NEW and len(data) >= 16:
                seconds, nanos = struct.unpack_from('=qq', data)
                timestamp = seconds * 1000000000 + nanos
        if timestamp is None:
            timestamp = time.time_ns()
            stats['timestamp_fallbacks'] += 1
        captured = min(wire_length, SNAPLEN)
        seconds, nanos = divmod(timestamp, 1000000000)
        output.write(struct.pack('<IIII', seconds, nanos, captured, wire_length))
        output.write(view[:captured])
        stats['records'] += 1
        stats['captured_bytes'] += captured
        stats['truncated_frames'] += int(wire_length > SNAPLEN or bool(flags & socket.MSG_TRUNC))
        if time.monotonic() - last_flush >= 1:
            output.flush()
            last_flush = time.monotonic()
    output.flush()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--interface', required=True)
    parser.add_argument('--destination', required=True, type=ipaddress.IPv4Address)
    parser.add_argument('--output', required=True)
    parser.add_argument('--uid', type=int, default=os.getuid())
    parser.add_argument('--gid', type=int, default=os.getgid())
    args = parser.parse_args()
    if not args.interface or args.interface == 'any' or min(args.uid, args.gid) < 0:
        parser.error('AF_PACKET requires one physical Ethernet interface and nonnegative uid/gid')
    stats = dict(engine='afpacket', interface=args.interface, destination=str(args.destination),
                 records=0, captured_bytes=0, timestamp_fallbacks=0, truncated_frames=0,
                 kernel_packets=None, kernel_drops=None)
    stopping = False
    def stop(*_):
        nonlocal stopping
        stopping = True
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    status = 0
    try:
        # Protocol=0 receives nothing until bind, after the kernel filter is attached.
        with socket.socket(socket.AF_PACKET, socket.SOCK_RAW, 0) as capture:
            capture.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 * 1024 * 1024)
            capture.setsockopt(socket.SOL_SOCKET, SO_TIMESTAMPNS_NEW, 1)
            attach_filter(capture, packet_filter(args.destination))
            capture.bind((args.interface, 3))  # ETH_P_ALL; no promiscuous membership
            if capture.getsockname()[3] != 1:  # ARPHRD_ETHER
                raise ValueError('AF_PACKET capture requires an Ethernet interface')
            stats['effective_rcvbuf_bytes'] = capture.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)
            capture.settimeout(.25)
            with open(args.output, 'xb') as output:
                if os.geteuid() == 0:
                    os.fchown(output.fileno(), args.uid, args.gid)
                    os.setgroups([])
                    os.setgid(args.gid)
                    os.setuid(args.uid)
                receive_to_pcap(capture, output, lambda: stopping, stats)
            packet_stats = capture.getsockopt(SOL_PACKET, PACKET_STATISTICS, 8)
            stats['kernel_packets'], stats['kernel_drops'] = struct.unpack('=II', packet_stats)
    except (OSError, ValueError) as error:
        stats['error'] = str(error)
        status = 1
    print('AF_PACKET_CAPTURE ' + json.dumps(stats), flush=True)
    return status


if __name__ == '__main__':
    sys.exit(main())
