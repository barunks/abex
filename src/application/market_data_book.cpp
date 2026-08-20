#include "abex/application/market_data_book.hpp"

#include <algorithm>
#include <stdexcept>
#include <thread>
#include <utility>

namespace abex {

MarketDataBook::MarketDataBook(std::chrono::milliseconds maximum_age)
    : maximum_age_(maximum_age), observers_(std::make_shared<const ObserverEntries>()) {
    if (maximum_age_ <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("market-data maximum age must be positive");
    }
}

void MarketDataBook::publish(MarketQuote quote) {
    if (!valid_quote(quote)) throw std::invalid_argument("invalid market quote");
    if (quote.published_at_ms == 0) quote.published_at_ms = unix_time_ms();
    const auto index = slot_index(quote.venue, quote.symbol);
    if (!index) throw std::invalid_argument("unsupported market-data symbol");

    auto& slot = quotes_[*index];
    while (slot.writer.test_and_set(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    const auto current_sequence = slot.sequence.load(std::memory_order_relaxed);
    if (current_sequence > quote.sequence && quote.sequence != 0) {
        slot.writer.clear(std::memory_order_release);
        return;
    }
    slot.version.fetch_add(1, std::memory_order_acq_rel);
    slot.bid_raw.store(quote.bid_price.raw(), std::memory_order_relaxed);
    slot.ask_raw.store(quote.ask_price.raw(), std::memory_order_relaxed);
    slot.source_time_ms.store(quote.source_time_ms, std::memory_order_relaxed);
    slot.published_at_ms.store(quote.published_at_ms, std::memory_order_relaxed);
    slot.sequence.store(quote.sequence, std::memory_order_relaxed);
    slot.version.fetch_add(1, std::memory_order_release);
    slot.writer.clear(std::memory_order_release);

    {
        std::scoped_lock lock(status_mutex_);
        status_.last_update_ms = std::max(status_.last_update_ms, quote.published_at_ms);
        status_.last_sequence = std::max(status_.last_sequence, quote.sequence);
    }
    const auto observers = observers_.load(std::memory_order_acquire);
    for (const auto& [token, observer] : *observers) {
        (void)token;
        observer(quote);
    }
}

std::vector<MarketQuote> MarketDataBook::snapshot() const {
    std::vector<MarketQuote> result;
    result.reserve(quotes_.size());
    for (std::size_t index = 0; index < quotes_.size(); ++index) {
        if (auto quote = read_slot(index)) result.push_back(std::move(*quote));
    }
    std::ranges::sort(result, [](const auto& lhs, const auto& rhs) {
        if (lhs.symbol != rhs.symbol) return lhs.symbol < rhs.symbol;
        return lhs.venue < rhs.venue;
    });
    return result;
}

std::optional<MarketQuote> MarketDataBook::latest(Venue venue, std::string_view symbol) const {
    const auto index = slot_index(venue, symbol);
    return index ? read_slot(*index) : std::nullopt;
}

std::optional<Decimal> MarketDataBook::price(Venue venue,
                                             std::string_view symbol,
                                             Side side) const {
    const auto quote = latest(venue, symbol);
    if (!quote || !fresh(*quote)) return std::nullopt;
    return executable_price(*quote, side);
}

bool MarketDataBook::fresh(const MarketQuote& quote, std::int64_t now_ms) const noexcept {
    const auto age = now_ms - quote.published_at_ms;
    return age >= 0 && age <= maximum_age_.count();
}

MarketDataBook::ObserverToken MarketDataBook::add_observer(QuoteObserver observer) {
    if (!observer) throw std::invalid_argument("market-data observer is required");
    std::scoped_lock lock(observer_update_mutex_);
    const auto token = next_observer_token_.fetch_add(1, std::memory_order_relaxed);
    auto updated = std::make_shared<ObserverEntries>(
        *observers_.load(std::memory_order_acquire));
    updated->emplace_back(token, std::move(observer));
    observers_.store(std::move(updated), std::memory_order_release);
    return token;
}

void MarketDataBook::remove_observer(ObserverToken token) noexcept {
    std::scoped_lock lock(observer_update_mutex_);
    auto updated = std::make_shared<ObserverEntries>(
        *observers_.load(std::memory_order_acquire));
    std::erase_if(*updated, [token](const auto& entry) { return entry.first == token; });
    observers_.store(std::move(updated), std::memory_order_release);
}

void MarketDataBook::set_ring_status(bool mapped,
                                     std::uint64_t generation,
                                     std::uint64_t last_sequence,
                                     std::string error,
                                     std::string transport) {
    std::scoped_lock lock(status_mutex_);
    if (generation != 0 && status_.generation != 0 && generation != status_.generation) {
        clear_slots();
        status_.last_sequence = 0;
        status_.last_update_ms = 0;
    }
    status_.ring_mapped = mapped;
    status_.generation = generation;
    status_.last_sequence = last_sequence;
    status_.last_error = std::move(error);
    if (!transport.empty()) status_.transport = std::move(transport);
}

MarketDataStatus MarketDataBook::status() const {
    std::scoped_lock lock(status_mutex_);
    auto result = status_;
    const auto age = unix_time_ms() - result.last_update_ms;
    result.ring_connected = result.ring_mapped && result.last_update_ms != 0 && age >= 0 &&
                            age <= maximum_age_.count();
    if (result.ring_mapped && !result.ring_connected && result.last_error.empty()) {
        result.last_error = result.last_update_ms == 0
                                ? "waiting for the first publisher update"
                                : "market-data publisher updates are stale";
    }
    return result;
}

std::optional<std::size_t> MarketDataBook::slot_index(Venue venue,
                                                       std::string_view symbol) noexcept {
    const std::size_t venue_offset = venue == Venue::Okx ? 0U : 2U;
    if (symbol == "BTC-USDT") return venue_offset;
    if (symbol == "ETH-USDT") return venue_offset + 1U;
    return std::nullopt;
}

std::string_view MarketDataBook::slot_symbol(std::size_t index) noexcept {
    return index % 2U == 0 ? "BTC-USDT" : "ETH-USDT";
}

std::optional<MarketQuote> MarketDataBook::read_slot(std::size_t index) const {
    const auto& slot = quotes_[index];
    for (;;) {
        const auto before = slot.version.load(std::memory_order_acquire);
        if ((before & 1U) != 0) continue;
        const auto bid = slot.bid_raw.load(std::memory_order_relaxed);
        const auto ask = slot.ask_raw.load(std::memory_order_relaxed);
        const auto source_time = slot.source_time_ms.load(std::memory_order_relaxed);
        const auto published_at = slot.published_at_ms.load(std::memory_order_relaxed);
        const auto sequence = slot.sequence.load(std::memory_order_relaxed);
        const auto after = slot.version.load(std::memory_order_acquire);
        if (before != after || (after & 1U) != 0) continue;
        if (published_at == 0) return std::nullopt;
        return MarketQuote{
            .venue = index < 2U ? Venue::Okx : Venue::Binance,
            .symbol = std::string(slot_symbol(index)),
            .bid_price = Decimal::from_raw(bid),
            .ask_price = Decimal::from_raw(ask),
            .source_time_ms = source_time,
            .published_at_ms = published_at,
            .sequence = sequence,
        };
    }
}

void MarketDataBook::clear_slots() noexcept {
    for (auto& slot : quotes_) {
        while (slot.writer.test_and_set(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        slot.version.fetch_add(1, std::memory_order_acq_rel);
        slot.published_at_ms.store(0, std::memory_order_relaxed);
        slot.sequence.store(0, std::memory_order_relaxed);
        slot.version.fetch_add(1, std::memory_order_release);
        slot.writer.clear(std::memory_order_release);
    }
}

} // namespace abex
