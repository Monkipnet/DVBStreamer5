from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one match, got {count}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


replace_once(
    "src/AppVersion.h",
    'inline constexpr const char* kProgramVersion = "10.8.135";',
    'inline constexpr const char* kProgramVersion = "10.8.136";',
)

p = Path("src/media/NativeUdpRelay.h")
text = p.read_text(encoding="utf-8")
start = text.index("class NativeTransportObserver {")
end = text.index("\n\nstruct NativeUdpRelayConfig {", start)
old = text[start:end]
new = '''class NativeTransportObserver {
public:
    using Callback = std::function<void(const std::uint8_t*, std::size_t)>;

    NativeTransportObserver()
        : httpHub_(std::make_shared<NativePreviewHub>()) {}

    NativeTransportObserver(const NativeTransportObserver&) = default;
    NativeTransportObserver& operator=(const NativeTransportObserver&) = default;

    NativeTransportObserver& operator=(Callback callback) {
        callback_ = std::move(callback);
        return *this;
    }

    explicit operator bool() const {
        return static_cast<bool>(callback_) ||
            (httpHub_ && httpHub_->subscriberCount() != 0U);
    }

    void operator()(const std::uint8_t* data, std::size_t size) const {
        if (callback_) callback_(data, size);
        if (httpHub_) httpHub_->publish(data, size);
    }

    std::shared_ptr<NativePreviewHub> httpHub() const {
        return httpHub_;
    }

private:
    // V10.8.136: NativeUdpRelayConfig is fully assembled before start(), then
    // copied into NativeUdpRelay before the worker thread is launched.  The
    // observer callback and hub pointer are immutable afterwards, so the hot
    // 1316-byte transport path needs neither a mutex nor an atomic shared_ptr
    // snapshot/refcount operation. NativePreviewHub remains independently
    // thread-safe for subscriber attach/detach and publish.
    Callback callback_;
    std::shared_ptr<NativePreviewHub> httpHub_;
};'''
if "std::atomic_load_explicit" not in old or "std::shared_ptr<const State> state_" not in old:
    raise SystemExit("NativeTransportObserver is not the expected V10.8.135 snapshot implementation")
p.write_text(text[:start] + new + text[end:], encoding="utf-8")
