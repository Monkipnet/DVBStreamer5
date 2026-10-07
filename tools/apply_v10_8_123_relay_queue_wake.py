from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one match, got {count}\n--- OLD ---\n{old}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


replace_once(
    "src/AppVersion.h",
    'inline constexpr const char* kProgramVersion = "10.8.122";',
    'inline constexpr const char* kProgramVersion = "10.8.123";',
)

replace_once(
    "src/media/NativeUdpRelay.cpp",
    '''        httpQueuedBytes_ += chunk.size();\n        httpQueue_.push_back(std::move(chunk));\n        lock.unlock();\n        httpQueueCondition_.notify_all();\n        offset += chunkSize;\n''',
    '''        // V10.8.123: there is one producer and one relay consumer for this\n        // bounded queue. Only wake the consumer when the queue transitions from\n        // empty to non-empty; repeated broadcasts while it is already draining\n        // create avoidable futex/scheduler churn on multi-service DVB inputs.\n        const bool notifyConsumer = httpQueue_.empty();\n        httpQueuedBytes_ += chunk.size();\n        httpQueue_.push_back(std::move(chunk));\n        lock.unlock();\n        if (notifyConsumer) httpQueueCondition_.notify_one();\n        offset += chunkSize;\n''',
)

replace_once(
    "src/media/NativeUdpRelay.cpp",
    '''                httpQueuedBytes_ -= chunk.size();\n                lock.unlock();\n                httpQueueCondition_.notify_all();\n\n                if (chunk.empty()) {\n''',
    '''                httpQueuedBytes_ -= chunk.size();\n                lock.unlock();\n                // A dequeue can only unblock the single producer waiting for\n                // bounded-queue space. notify_one avoids a broadcast wakeup.\n                httpQueueCondition_.notify_one();\n\n                if (chunk.empty()) {\n''',
)

print("V10.8.123 relay queue wake optimization applied")
