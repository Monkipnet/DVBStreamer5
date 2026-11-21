#!/usr/bin/env python3
"""Ensure the tested DVR buffer request is wired into the real Linux input."""
from pathlib import Path

source = Path("src/media/LinuxDvbInput.cpp").read_text()
open_fn = source.split("bool LinuxDvbInput::open(", 1)[1].split(
    "bool LinuxDvbInput::read(", 1
)[0]
read_fn = source.split("bool LinuxDvbInput::read(", 1)[1].split(
    "void LinuxDvbInput::close(", 1
)[0]

assert '#include "media/DvbDvrBuffer.h"' in source
assert open_fn.index('dvrFd_ = ::open(') < open_fn.index(
    'if (dvrFd_ < 0)') < open_fn.index('configureDvbDvrBuffer(')
assert 'dvrFd_, [](int fd, unsigned long request, unsigned long bytes)' in open_fn
assert 'dvrBuffer.success ? "ok" : "warning"' in open_fn
assert 'requested_bytes=' in open_fn
assert 'std::strerror(dvrBuffer.error)' in open_fn
assert 'NATIVE DVB DVR overflow recovered count=' in read_fn
assert '<< " adapter=" << frontendAdapter_' in read_fn
assert '<< " frontend=" << frontendIndex_' in read_fn
print("PASS: V617 real DVR open/ioctl and adapter-attributed overflow diagnostics")
