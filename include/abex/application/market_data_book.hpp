#pragma once

#include "abex/domain/market_data.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace abex {

struct MarketDataStatus {
    bool ring_mapped{false};
    bool ring_connected{false};
    std::uint64_t generation{0};
    std::uint64_t last_sequence{0};
    std::int64_t last_update_ms{0};
    std::string last_error;
    std::string transport{"PUBLIC_REST"};
};

class MarketDataBook final {
public:
    using ObserverToken = std::uint64_t;
    using QuoteObserver = std::function<void(const MarketQuote&)>;

    explicit MarketDataBook(std::chrono::milliseconds maximum_age = std::chrono::seconds{5});

    void publish(MarketQuote quote);
    [[nodiscard]] std::vector<MarketQuote> snapshot() const;
    [[nodiscard]] std::optional<MarketQuote> latest(Venue venue,
                                                     std::string_view symbol) const;
    [[nodiscard]] std::optional<Decimal> price(Venue venue,
                                                std::string_view symbol,
                                                Side side) const;
    [[nodiscard]] bool fresh(const MarketQuote& quote,
                             std::int64_t now_ms = unix_time_ms()) const noexcept;
    [[nodiscard]] std::chrono::milliseconds maximum_age() const noexcept {
        return maximum_age_;
    }

    [[nodiscard]] ObserverToken add_observer(QuoteObserver observer);
    void remove_observer(ObserverToken token) noexcept;

    void set_ring_status(bool mapped,
                         std::uint64_t generation,
                         std::uint64_t last_sequence,
                         std::string error = {},
                         std::string transport = {});
    [[nodiscard]] MarketDataStatus status() const;

private:
    struct alignas(64) QuoteSlot {
        std::atomic_flag writer = ATOMIC_FLAG_INIT;
        std::atomic<std::uint64_t> version{0};
        std::atomic<std::int64_t> bid_raw{0};
        std::atomic<std::int64_t> ask_raw{0};
        std::atomic<std::int64_t> source_time_ms{0};
        std::atomic<std::int64_t> published_at_ms{0};
        std::atomic<std::uint64_t> sequence{0};
    };
    using ObserverEntries = std::vector<std::pair<ObserverToken, QuoteObserver>>;

    [[nodiscard]] static std::optional<std::size_t> slot_index(Venue venue,
                                                                std::string_view symbol) noexcept;
    [[nodiscard]] static std::string_view slot_symbol(std::size_t index) noexcept;
    [[nodiscard]] std::optional<MarketQuote> read_slot(std::size_t index) const;
    void clear_slots() noexcept;

    const std::chrono::milliseconds maximum_age_;
    std::array<QuoteSlot, 4> quotes_;
    mutable std::mutex observer_update_mutex_;
    std::atomic<std::shared_ptr<const ObserverEntries>> observers_;
    std::atomic<ObserverToken> next_observer_token_{1};
    mutable std::mutex status_mutex_;
    MarketDataStatus status_;
};

} // namespace abex
