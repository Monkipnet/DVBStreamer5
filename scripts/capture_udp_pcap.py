#!/usr/bin/env python3
"""Supervise tcpdump without propagating its fatal signal to the caller."""
import argparse
import ipaddress
import json
import signal
import subprocess
import sys
import time


def capture(command, seconds, grace=5):
    started = time.monotonic()
    result = dict(command=command, requested_seconds=seconds, duration_reached=False,
                  forced_kill=False, interrupted=False, child_returncode=None,
                  completed=False)
    try:
        child = subprocess.Popen(command, stdout=sys.stderr, stderr=sys.stderr)
    except OSError as error:
        result.update(error=str(error), elapsed_seconds=round(time.monotonic() - started, 6))
        return result, 127
    try:
        try:
            child.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            result['duration_reached'] = True
            child.send_signal(signal.SIGINT)
            try:
                child.wait(timeout=grace)
            except subprocess.TimeoutExpired:
                result['forced_kill'] = True
                child.kill()
                child.wait()
        except KeyboardInterrupt:
            result['interrupted'] = True
    finally:
        if child.poll() is None:
            child.kill()
            child.wait()
            result['forced_kill'] = True
    code = child.returncode
    result.update(child_returncode=code, elapsed_seconds=round(time.monotonic() - started, 6))
    result['completed'] = (result['duration_reached'] and not result['forced_kill']
                           and not result['interrupted'] and code in (0, -signal.SIGINT))
    if result['completed']:
        return result, 0
    if result['interrupted']:
        return result, 130
    return result, (128 - code if code < 0 else code or 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--interface', required=True)
    parser.add_argument('--destination', required=True, type=ipaddress.IPv4Address)
    parser.add_argument('--output', required=True)
    parser.add_argument('--user', required=True)
    parser.add_argument('--seconds', required=True, type=int)
    args = parser.parse_args()
    if not args.interface or not 5 <= args.seconds <= 900:
        parser.error('interface must be nonempty and seconds must be 5..900')
    command = ['tcpdump', '-p', '-i', args.interface, '-Q', 'out', '-nn', '-s', '0',
               '-B', '8192', '-U', '-Z', args.user, '-w', args.output,
               f'ip proto 17 and dst host {args.destination}']
    result, status = capture(command, args.seconds)
    json.dump(result, sys.stdout, indent=2)
    print()
    return status


if __name__ == '__main__':
    sys.exit(main())
