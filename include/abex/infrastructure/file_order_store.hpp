#pragma once

#include "abex/ports/order_store.hpp"
#include "abex/domain/string_lookup.hpp"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <deque>
#include <mutex>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace abex {

class FileOrderStore final : public IOrderStore {
public:
    explicit FileOrderStore(std::filesystem::path path, bool durable_writes = true);
    ~FileOrderStore() override;

    void append(const Order& order) override;
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
    [[nodiscard]] std::uint64_t append_record(std::string_view type,
                                              nlohmann::json payload);

    std::filesystem::path path_;
    bool durable_writes_{true};
    mutable std::mutex mutex_;
    std::atomic<std::uint64_t> next_sequence_{1};
    int lock_descriptor_{-1};
    StringMap<std::pair<std::uint64_t, Order>> latest_orders_;
    std::deque<OperationalEvent> recent_events_;
    StringMap<std::deque<OperationalEvent>> recent_order_events_;
};

// Useful for deterministic tests and embedding the gateway without local storage.
class MemoryOrderStore final : public IOrderStore {
public:
    void append(const Order& order) override;
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
