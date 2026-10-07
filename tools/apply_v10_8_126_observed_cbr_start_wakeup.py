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
    'inline constexpr const char* kProgramVersion = "10.8.125";',
    'inline constexpr const char* kProgramVersion = "10.8.126";',
)

replace_once(
    "src/media/NativeUdpRelay.cpp",
    '''        if (observedCbrPacer) {\n            {\n                std::lock_guard<std::mutex> lock(observedCbrMutex);\n                for (const auto& packet : observedPackets) {\n                    if (!observedCbrPacer->enqueue(packet)) {\n                        std::lock_guard<std::mutex> errorLock(errorMutex_);\n                        lastError_ =\n                            "observed CBR input exceeded the bounded 2 MiB pacing queue; "\n                            "increase target bitrate";\n                        running_.store(false, std::memory_order_release);\n                        httpQueueCondition_.notify_all();\n                        return false;\n                    }\n                }\n            }\n            observedCbrCondition.notify_one();\n            return true;\n        }\n''',
    '''        if (observedCbrPacer) {\n            bool wakeForStart = false;\n            {\n                std::lock_guard<std::mutex> lock(observedCbrMutex);\n                const bool wasStarted = observedCbrPacer->started();\n                for (const auto& packet : observedPackets) {\n                    if (!observedCbrPacer->enqueue(packet)) {\n                        std::lock_guard<std::mutex> errorLock(errorMutex_);\n                        lastError_ =\n                            "observed CBR input exceeded the bounded 2 MiB pacing queue; "\n                            "increase target bitrate";\n                        running_.store(false, std::memory_order_release);\n                        httpQueueCondition_.notify_all();\n                        return false;\n                    }\n                }\n                // V10.8.126: once the observed pacer has started, its worker\n                // sleeps on nextDeadline() and new input does not move that\n                // deadline. Repeated notify_one() calls only wake wait_until()\n                // early, creating avoidable futex/scheduler churn. The first\n                // successful enqueue still wakes the worker immediately.\n                wakeForStart = !wasStarted && observedCbrPacer->started();\n            }\n            if (wakeForStart) observedCbrCondition.notify_one();\n            return true;\n        }\n''',
)

print("V10.8.126 observed CBR startup-only wakeup applied")
