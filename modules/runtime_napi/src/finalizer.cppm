export module mbun.runtime_napi.finalizer;

import std;
import mbun.runtime_napi.env_handle;

namespace mbun::runtime_napi {

export using Finalizer = void (*)(EnvHandle*, void*, void*);

export struct FinalizerEntry {
    Finalizer callback{nullptr};
    void* data{nullptr};
    void* hint{nullptr};
};

export class FinalizerQueue {
private:
    std::vector<FinalizerEntry> entries_{};
    bool finishing_{false};

public:
    [[nodiscard]] bool add(Finalizer callback, void* data, void* hint) {
        if (!callback || finishing_) return false;
        entries_.push_back(FinalizerEntry{callback, data, hint});
        return true;
    }

    [[nodiscard]] bool remove(Finalizer callback, void* data, void* hint) {
        const auto found = std::ranges::find_if(entries_, [=](const FinalizerEntry& entry) {
            return entry.callback == callback && entry.data == data && entry.hint == hint;
        });
        if (found == entries_.end()) return false;
        entries_.erase(found);
        return true;
    }

    void cleanup(EnvHandle& env) {
        finishing_ = true;
        while (!entries_.empty()) {
            const auto entry = entries_.back();
            entries_.pop_back();
            entry.callback(&env, entry.data, entry.hint);
            env.clear_pending_exception();
        }
        finishing_ = false;
    }

    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] bool finishing() const noexcept { return finishing_; }
};

} // namespace mbun::runtime_napi
