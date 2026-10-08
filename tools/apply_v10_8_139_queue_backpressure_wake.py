from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one match, got {count}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


def replace_exact_count(path: str, old: str, new: str, expected: int) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise SystemExit(f"{path}: expected {expected} matches, got {count}")
    p.write_text(text.replace(old, new), encoding="utf-8")


replace_once(
    "src/AppVersion.h",
    'inline constexpr const char* kProgramVersion = "10.8.138";',
    'inline constexpr const char* kProgramVersion = "10.8.139";',
)

replace_once(
    "src/media/NativeUdpRelay.h",
    "    std::size_t httpQueuedBytes_ = 0;\n    bool httpFinished_ = false;",
    "    std::size_t httpQueuedBytes_ = 0;\n"
    "    // V10.8.139: guarded by httpQueueMutex_. Only signal queue-space\n"
    "    // availability when the single producer is actually blocked.\n"
    "    bool httpQueueProducerWaiting_ = false;\n"
    "    bool httpFinished_ = false;",
)

replace_exact_count(
    "src/media/NativeUdpRelay.cpp",
    "        httpQueueBufferPool_.clear();\n        httpQueuedBytes_ = 0;\n        httpFinished_ = false;",
    "        httpQueueBufferPool_.clear();\n"
    "        httpQueuedBytes_ = 0;\n"
    "        httpQueueProducerWaiting_ = false;\n"
    "        httpFinished_ = false;",
    2,
)

replace_once(
    "src/media/NativeUdpRelay.cpp",
    "        std::unique_lock<std::mutex> lock(httpQueueMutex_);\n"
    "        httpQueueCondition_.wait(lock, [this, chunkSize, kMaximumHttpQueueBytes] {\n"
    "            return !running_.load(std::memory_order_acquire) ||\n"
    "                httpQueuedBytes_ + chunkSize <= kMaximumHttpQueueBytes;\n"
    "        });\n"
    "        if (!running_.load(std::memory_order_acquire)) {\n"
    "            return false;\n"
    "        }",
    "        std::unique_lock<std::mutex> lock(httpQueueMutex_);\n"
    "        const auto hasQueueSpace = [this, chunkSize, kMaximumHttpQueueBytes] {\n"
    "            return !running_.load(std::memory_order_acquire) ||\n"
    "                httpQueuedBytes_ + chunkSize <= kMaximumHttpQueueBytes;\n"
    "        };\n"
    "        if (!hasQueueSpace()) {\n"
    "            httpQueueProducerWaiting_ = true;\n"
    "            httpQueueCondition_.wait(lock, hasQueueSpace);\n"
    "            httpQueueProducerWaiting_ = false;\n"
    "        }\n"
    "        if (!running_.load(std::memory_order_acquire)) {\n"
    "            return false;\n"
    "        }",
)

replace_once(
    "src/media/NativeUdpRelay.cpp",
    "                auto chunk = std::move(httpQueue_.front());\n"
    "                httpQueue_.pop_front();\n"
    "                httpQueuedBytes_ -= chunk.size();\n"
    "                lock.unlock();\n"
    "                // A dequeue can only unblock the single producer waiting for\n"
    "                // bounded-queue space. notify_one avoids a broadcast wakeup.\n"
    "                httpQueueCondition_.notify_one();",
    "                auto chunk = std::move(httpQueue_.front());\n"
    "                httpQueue_.pop_front();\n"
    "                httpQueuedBytes_ -= chunk.size();\n"
    "                // V10.8.139: most dequeues happen while the 2 MiB queue has\n"
    "                // ample space. Avoid a futex wake unless enqueueHttpData()\n"
    "                // actually had to block on backpressure. The flag and queue\n"
    "                // state are observed under the same mutex, so no wake is lost.\n"
    "                const bool notifyProducer = httpQueueProducerWaiting_;\n"
    "                lock.unlock();\n"
    "                if (notifyProducer) httpQueueCondition_.notify_one();",
)
