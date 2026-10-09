#!/usr/bin/env python3
from pathlib import Path

p = Path('src/media/NativeUdpRelay.cpp')
s = p.read_text()

old_file_wait = '''            if (received == 0) {
                std::chrono::steady_clock::time_point nextDeadline {};
                bool hasDeadline = false;
                for (const auto& output : outputs) {
                    if (output.cbrWorker && output.cbrWorker->started()) {
                        const auto outputDeadline = output.cbrWorker->nextDeadline();
                        if (!hasDeadline || outputDeadline < nextDeadline) {
                            nextDeadline = outputDeadline;
                            hasDeadline = true;
                        }
                    }
                }
                if (hasDeadline &&
                    nextDeadline > std::chrono::steady_clock::now()) {
                    std::this_thread::sleep_until(nextDeadline);
                }
            }
'''
new_file_wait = '''            if (received == 0) {
                // The dedicated WISI sender owns all CBR deadlines. The input
                // relay must not wake once per UDP slot merely to watch them.
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
'''
if old_file_wait not in s:
    raise SystemExit('file wait block not found')
s = s.replace(old_file_wait, new_file_wait, 1)

old_http_header = '''        } else if (httpInput) {
            int receiveTimeoutMs = 250;
            auto nextDeadline = std::chrono::steady_clock::time_point {};
            bool hasDeadline = false;
            for (const auto& output : outputs) {
                if (output.cbrWorker && output.cbrWorker->started()) {
                    const auto deadline = output.cbrWorker->nextDeadline();
                    if (!hasDeadline || deadline < nextDeadline) {
                        nextDeadline = deadline;
                        hasDeadline = true;
                    }
                }
            }
            std::unique_lock<std::mutex> lock(httpQueueMutex_);
'''
new_http_header = '''        } else if (httpInput) {
            std::unique_lock<std::mutex> lock(httpQueueMutex_);
'''
if old_http_header not in s:
    raise SystemExit('HTTP deadline header not found')
s = s.replace(old_http_header, new_http_header, 1)

old_http_wait = '''                if (hasDeadline) {
                    if (httpFinished_) {
                        httpQueueCondition_.wait_until(lock, nextDeadline);
                    } else {
                        httpQueueCondition_.wait_until(
                            lock, nextDeadline, [this] {
                                return !httpQueue_.empty() || httpFinished_ ||
                                    !running_.load(std::memory_order_acquire);
                            });
                    }
                } else if (!httpFinished_) {
                    httpQueueCondition_.wait_for(
                        lock, std::chrono::milliseconds(receiveTimeoutMs), [this] {
                            return !httpQueue_.empty() || httpFinished_ ||
                                !running_.load(std::memory_order_acquire);
                        });
                }
'''
new_http_wait = '''                if (httpFinished_) {
                    // Source is done but the dedicated WISI queue may still be
                    // draining. Avoid a busy loop without coupling to its CBR clock.
                    httpQueueCondition_.wait_for(
                        lock, std::chrono::milliseconds(20), [this] {
                            return !httpQueue_.empty() ||
                                !running_.load(std::memory_order_acquire);
                        });
                } else {
                    httpQueueCondition_.wait_for(
                        lock, std::chrono::milliseconds(250), [this] {
                            return !httpQueue_.empty() || httpFinished_ ||
                                !running_.load(std::memory_order_acquire);
                        });
                }
'''
if old_http_wait not in s:
    raise SystemExit('HTTP wait block not found')
s = s.replace(old_http_wait, new_http_wait, 1)

old_network_timeout = '''        } else {
            int receiveTimeoutMs = 250;
            for (const auto& output : outputs) {
                if (!output.cbrWorker || !output.cbrWorker->started()) {
                    continue;
                }
                const auto remaining = output.cbrWorker->nextDeadline() -
                    std::chrono::steady_clock::now();
                const int outputTimeout =
                    remaining <= std::chrono::steady_clock::duration::zero()
                    ? 0
                    : static_cast<int>((std::min)(
                        std::int64_t{250},
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            remaining + std::chrono::milliseconds(1)).count()));
                receiveTimeoutMs = (std::min)(receiveTimeoutMs, outputTimeout);
            }
'''
new_network_timeout = '''        } else {
            // Input reads no longer share the WISI output clock. A fixed timeout
            // avoids thousands of unnecessary wakeups per second across channels.
            int receiveTimeoutMs = 250;
'''
if old_network_timeout not in s:
    raise SystemExit('network timeout block not found')
s = s.replace(old_network_timeout, new_network_timeout, 1)

p.write_text(s)
Path('tools/patch_wisi_v164_input_wake.py').unlink(missing_ok=True)
Path('.github/workflows/wisi-cbr-v164-wake.yml').unlink(missing_ok=True)
