#pragma once

#include "abex/infrastructure/journal_serializer.hpp"
#include "abex/ports/order_store.hpp"
#include "abex/domain/string_lookup.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace abex {

template <JournalSerializer S = JsonSerializer>
class FileOrderStore final : public IOrderStore {
public:
    explicit FileOrderStore(std::filesystem::path path, bool durable_writes = true);
    ~FileOrderStore() override;

    void append(const Order& order) override;
    void append_order(const Order& order, bool intent_only) override;
    // Two-phase write: reserve_sequence() is called inside the gateway mutex_
    // (so sequence order matches mutation order); commit_order() does the disk
    // write outside the gateway mutex_ — only the store's own mutex is held
    // for the write() syscall.
    [[nodiscard]] std::uint64_t reserve_sequence() override;
    void commit_order(const Order& order, std::string payload,
                      std::uint64_t sequence) override;
    [[nodiscard]] OperationalEvent append_event(OperationalEvent event) override;
    [[nodiscard]] std::vector<Order> load_latest() const override;
    [[nodiscard]] std::vector<OperationalEvent>
    load_events(std::size_t limit = 200) const override;
    [[nodiscard]] std::vector<OperationalEvent>
    load_order_events(std::string_view client_order_id,
                      std::size_t limit = 500) const override;
    [[nodiscard]] OrderJournalStatus status() const override;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    void write_record(std::string_view type,
                      std::string_view payload,
                      std::uint64_t sequence);
    [[nodiscard]] std::uint64_t next_seq() {
        return next_sequence_.fetch_add(1, std::memory_order_relaxed);
    }
    void signal_sync();
    void run_sync_worker();

    std::filesystem::path path_;
    bool durable_writes_{true};
    // Hot path (commit_order): lock-free — O_APPEND write() is kernel-atomic.
    // index_mutex_: guards latest_orders_, recent_events_, recent_order_events_
    // for cold read paths (load_latest, load_events) and append_event.
    mutable std::mutex index_mutex_;
    std::atomic<std::uint64_t> next_sequence_{1};
    int lock_descriptor_{-1};
    StringMap<std::pair<std::uint64_t, Order>> latest_orders_;
    std::deque<OperationalEvent> recent_events_;
    StringMap<std::deque<OperationalEvent>> recent_order_events_;

    std::atomic<std::uint64_t> sync_generation_{0};
    std::atomic<std::uint64_t> synced_generation_{0};
    std::mutex sync_mutex_;
    std::condition_variable sync_cv_;
    bool sync_stop_{false};
    std::thread sync_worker_;
};

using JsonFileOrderStore = FileOrderStore<JsonSerializer>;
using KvFileOrderStore   = FileOrderStore<KvSerializer>;

class MemoryOrderStore final : public IOrderStore {
public:
    void append(const Order& order) override;
    void append_order(const Order& order, bool intent_only) override;
    [[nodiscard]] std::uint64_t reserve_sequence() override;
    void commit_order(const Order& order, std::string payload,
                      std::uint64_t sequence) override;
    [[nodiscard]] OperationalEvent append_event(OperationalEvent event) override;
    [[nodiscard]] std::vector<Order> load_latest() const override;
    [[nodiscard]] std::vector<OperationalEvent>
    load_events(std::size_t limit = 200) const override;
    [[nodiscard]] std::vector<OperationalEvent>
    load_order_events(std::string_view client_order_id,
                      std::size_t limit = 500) const override;
    [[nodiscard]] OrderJournalStatus status() const override;

private:
    mutable std::mutex mutex_;
    StringMap<Order> latest_orders_;
    std::vector<OperationalEvent> events_;
    std::uint64_t record_sequence_{0};
};

} // namespace abex

#include "abex/infrastructure/file_order_store_impl.hpp"
