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
    'inline constexpr const char* kProgramVersion = "10.8.118";',
    'inline constexpr const char* kProgramVersion = "10.8.119";',
)

replace_once(
    "src/media/SharedDvbInputPool.cpp",
    '''        bool dropped = false;\n        std::uint64_t droppedCount = 0;\n        {\n            std::lock_guard<std::mutex> lock(subscriber->mutex);\n''',
    '''        bool dropped = false;\n        bool notify = false;\n        std::uint64_t droppedCount = 0;\n        {\n            std::lock_guard<std::mutex> lock(subscriber->mutex);\n''',
)

replace_once(
    "src/media/SharedDvbInputPool.cpp",
    '''            if (chunk->size() > kSubscriberQueueBytes) {\n                ++subscriber->droppedChunks;\n                dropped = true;\n            } else {\n                subscriber->queuedBytes += chunk->size();\n                subscriber->queue.push_back(chunk);\n            }\n            droppedCount = subscriber->droppedChunks;\n        }\n\n        if (dropped && logCounter(droppedCount)) {\n''',
    '''            if (chunk->size() > kSubscriberQueueBytes) {\n                ++subscriber->droppedChunks;\n                dropped = true;\n            } else {\n                // V10.8.119: only wake the subscriber when the queue transitions\n                // from empty to non-empty. Once awake, runSubscriber drains the\n                // queue without sleeping again while data remains, so notifying\n                // on every 64 KiB tuner chunk only creates redundant futex wakeups.\n                notify = subscriber->queue.empty();\n                subscriber->queuedBytes += chunk->size();\n                subscriber->queue.push_back(chunk);\n            }\n            droppedCount = subscriber->droppedChunks;\n        }\n\n        if (dropped && logCounter(droppedCount)) {\n''',
)

replace_once(
    "src/media/SharedDvbInputPool.cpp",
    '''        subscriber->condition.notify_one();\n    }\n\n    static void finishSource(\n''',
    '''        if (notify) subscriber->condition.notify_one();\n    }\n\n    static void finishSource(\n''',
)

print("V10.8.119 shared DVB wakeup optimization applied")
