#pragma once

#include "abex/application/market_data_book.hpp"
#include "abex/domain/string_lookup.hpp"
#include "abex/infrastructure/rate_limiter.hpp"
#include "abex/ports/exchange_adapter.hpp"

#include <atomic>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace abex {

class SimulatedExchangeAdapter final : public IExchangeAdapter {
public:
    struct Config {
        bool report_before_ack{false};
        bool amend_reports_before_ack{false};
        bool fill_before_replace{false};
        bool report_terminal_orders_as_open{false};
        bool throw_on_place{false};
        double request_burst{100.0};
        double requests_per_second{100.0};
        std::unordered_map<std::string, Decimal> initial_balances;
    };

    explicit SimulatedExchangeAdapter(Venue venue);
    SimulatedExchangeAdapter(Venue venue, Config config);
    SimulatedExchangeAdapter(Venue venue,
                             Config config,
                             std::shared_ptr<MarketDataBook> market_data);
    ~SimulatedExchangeAdapter() override;

    [[nodiscard]] Venue venue() const noexcept override { return venue_; }
    void start(ExecutionCallback execution_callback,
               ConnectionCallback connection_callback) override;
    void stop() noexcept override;
    void restore(std::span<const Order> recovered_orders) override;

    [[nodiscard]] AdapterResult place(const Order& order) override;
    [[nodiscard]] AdapterResult cancel(const Order& order) override;
    [[nodiscard]] AdapterResult amend(const Order& order,
                                      std::optional<Decimal> new_price,
                                      std::optional<Decimal> new_quantity) override;
    [[nodiscard]] std::optional<ExecutionReport> query(const Order& order) override;
    [[nodiscard]] BalanceQueryResult
    query_balances(std::optional<std::string> currency = std::nullopt) override;
    [[nodiscard]] InstrumentRulesQueryResult
    query_instrument_rules(std::string symbol) override;
    [[nodiscard]] std::optional<std::vector<ExecutionReport>> query_open_orders() override;

    [[nodiscard]] bool emit(std::string_view client_order_id,
                            OrderStatus status,
                            Decimal cumulative_filled,
                            std::optional<Decimal> last_fill_price = std::nullopt,
                            std::string event_id = {},
                            std::optional<std::uint64_t> sequence = std::nullopt);
    void disconnect();
    void reconnect();
    void synchronize_rate_limiter(double capacity, double available, double tokens_per_second);
    [[nodiscard]] bool connected() const noexcept { return connected_.load(); }

private:
    [[nodiscard]] AdapterResult rate_limited_result();
    [[nodiscard]] ExecutionReport report_for(const Order& order,
                                             OrderStatus status,
                                             Decimal cumulative_filled,
                                             std::optional<Decimal> last_fill_price,
                                             std::string event_id,
                                             std::optional<std::uint64_t> sequence);
    void publish_or_buffer(ExecutionReport report);
    void match_orders(const MarketQuote& quote);

    // Incremental balance accounting — updated on place/cancel/fill so that
    // query_balances() never needs to iterate orders_.
    // fill_adjustments_[currency] = net realized change from initial_balances_.
    // frozen_[currency]           = total quantity reserved by open orders.
    void apply_balance_place_locked(const Order& order, Decimal execution_price);
    void apply_balance_release_locked(const Order& order, Decimal execution_price);
    void apply_balance_fill_locked(const Order& order,
                                   Decimal delta_filled,
                                   Decimal fill_price);

    Venue venue_;
    Config config_;
    TokenBucket rate_limiter_;
    std::shared_ptr<MarketDataBook> market_data_;
    std::unordered_map<std::string, Decimal> initial_balances_;
    MarketDataBook::ObserverToken market_observer_token_{0};
    mutable std::mutex mutex_;
    // Tests, operation calls, and quote matching can originate on different
    // threads. They form one logical producer at the adapter boundary.
    mutable std::mutex execution_emit_mutex_;
    StringMap<Order> orders_;
    std::deque<ExecutionReport> buffered_reports_;
    ExecutionCallback execution_callback_;
    ConnectionCallback connection_callback_;
    std::atomic<bool> connected_{false};
    std::atomic<std::uint64_t> next_exchange_id_{1};
    std::atomic<std::uint64_t> next_event_id_{1};
    std::atomic<std::uint64_t> next_sequence_{1};
    // Incremental balance state (all under mutex_).
    std::unordered_map<std::string, Decimal> fill_adjustments_; // net realized delta
    std::unordered_map<std::string, Decimal> frozen_;           // reserved by open orders
};

} // namespace abex
