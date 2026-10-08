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
    'inline constexpr const char* kProgramVersion = "10.8.137";',
    'inline constexpr const char* kProgramVersion = "10.8.138";',
)

replace_once(
    "src/media/NativeUdpRelay.h",
    "    std::deque<std::vector<std::uint8_t>> httpQueue_;\n    std::size_t httpQueuedBytes_ = 0;",
    "    std::deque<std::vector<std::uint8_t>> httpQueue_;\n"
    "    // V10.8.138: recycle a small number of consumed queue buffers so the\n"
    "    // shared-DVB/external hot path does not malloc/free one vector per chunk.\n"
    "    std::vector<std::vector<std::uint8_t>> httpQueueBufferPool_;\n"
    "    std::size_t httpQueuedBytes_ = 0;",
)

replace_exact_count(
    "src/media/NativeUdpRelay.cpp",
    "        httpQueue_.clear();\n        httpQueuedBytes_ = 0;",
    "        httpQueue_.clear();\n        httpQueueBufferPool_.clear();\n        httpQueuedBytes_ = 0;",
    2,
)

replace_once(
    "src/media/NativeUdpRelay.cpp",
    "        std::vector<std::uint8_t> chunk(\n"
    "            data + offset, data + offset + chunkSize);\n"
    "        std::unique_lock<std::mutex> lock(httpQueueMutex_);\n"
    "        httpQueueCondition_.wait(lock, [this, chunkSize, kMaximumHttpQueueBytes] {\n"
    "            return !running_.load(std::memory_order_acquire) ||\n"
    "                httpQueuedBytes_ + chunkSize <= kMaximumHttpQueueBytes;\n"
    "        });\n"
    "        if (!running_.load(std::memory_order_acquire)) {\n"
    "            return false;\n"
    "        }",
    "        std::unique_lock<std::mutex> lock(httpQueueMutex_);\n"
    "        httpQueueCondition_.wait(lock, [this, chunkSize, kMaximumHttpQueueBytes] {\n"
    "            return !running_.load(std::memory_order_acquire) ||\n"
    "                httpQueuedBytes_ + chunkSize <= kMaximumHttpQueueBytes;\n"
    "        });\n"
    "        if (!running_.load(std::memory_order_acquire)) {\n"
    "            return false;\n"
    "        }\n"
    "\n"
    "        std::vector<std::uint8_t> chunk;\n"
    "        if (!httpQueueBufferPool_.empty()) {\n"
    "            chunk = std::move(httpQueueBufferPool_.back());\n"
    "            httpQueueBufferPool_.pop_back();\n"
    "        } else {\n"
    "            chunk.reserve(kMaximumChunkBytes);\n"
    "        }\n"
    "        chunk.resize(chunkSize);\n"
    "        std::memcpy(chunk.data(), data + offset, chunkSize);",
)

replace_once(
    "src/media/NativeUdpRelay.cpp",
    "                received = chunk.size();\n"
    "                std::copy(chunk.begin(), chunk.end(), datagram.begin());\n"
    "                inputBytes_.fetch_add(received, std::memory_order_relaxed);\n"
    "                if (config_.trustedAlignedExternalInput) {\n"
    "                    inputFramer.pushTrustedAligned(datagram.data(), received, packets);\n"
    "                } else {\n"
    "                    inputFramer.push(datagram.data(), received, packets);\n"
    "                }",
    "                received = chunk.size();\n"
    "                inputBytes_.fetch_add(received, std::memory_order_relaxed);\n"
    "                // V10.8.138: the dequeued vector already owns a stable contiguous\n"
    "                // byte range for the duration of framing. Feed it directly instead\n"
    "                // of copying every chunk through the 64 KiB datagram scratch buffer.\n"
    "                if (config_.trustedAlignedExternalInput) {\n"
    "                    inputFramer.pushTrustedAligned(chunk.data(), received, packets);\n"
    "                } else {\n"
    "                    inputFramer.push(chunk.data(), received, packets);\n"
    "                }\n"
    "\n"
    "                {\n"
    "                    std::lock_guard<std::mutex> recycleLock(httpQueueMutex_);\n"
    "                    constexpr std::size_t kMaximumRecycledQueueBuffers = 2;\n"
    "                    if (httpQueueBufferPool_.size() < kMaximumRecycledQueueBuffers) {\n"
    "                        chunk.clear();\n"
    "                        httpQueueBufferPool_.push_back(std::move(chunk));\n"
    "                    }\n"
    "                }",
)
