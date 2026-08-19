#pragma once

#include "abex/application/bounded_event_dispatcher.hpp"
#include "abex/application/market_data_book.hpp"
#include "abex/application/risk_manager.hpp"
#include "abex/application/sequence_tracker.hpp"
#include "abex/ports/exchange_adapter.hpp"
#include "abex/ports/order_store.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace abex {

struct OperationResult {
    bool ok{false};
    bool idempotent_replay{false};
    std::string code;
    std::string message;
    std::optional<Order> order;
};

struct VenueHealth {
    bool connected{false};
    bool ever_connected{false};
    bool reconciliation_required{false};
    std::uint64_t sequence_gaps{0};
    std::uint64_t dropped_events{0};
    std::string last_error;
};

struct GatewayStability {
    std::string instance_id;
    std::int64_t started_at_ms{0};
    std::size_t recovered_orders{0};
    std::uint64_t idempotent_replays{0};
    std::uint64_t reconciliations{0};
    std::uint64_t alerts{0};
    std::uint64_t logging_failures{0};
    std::string last_logging_error;
    OrderJournalStatus journal;
};

class OrderGateway final {
public:
    using ObserverToken = std::uint64_t;
    using OrderObserver = std::function<void(const Order&)>;
    using OperationalObserver = std::function<void(const OperationalEvent&)>;

    struct Options {
        std::size_t event_queue_capacity{4096};
        bool reconcile_on_start{true};
        bool reconcile_on_reconnect{true};
        std::chrono::milliseconds reconciliation_interval{30000};
    };

    OrderGateway(std::vector<std::shared_ptr<IExchangeAdapter>> adapters,
                 RiskManager risk_manager,
                 std::shared_ptr<IOrderStore> order_store);
    OrderGateway(std::vector<std::shared_ptr<IExchangeAdapter>> adapters,
                 RiskManager risk_manager,
                 std::shared_ptr<IOrderStore> order_store,
                 Options options,
                 std::shared_ptr<MarketDataBook> market_data = {});
    ~OrderGateway();

    OrderGateway(const OrderGateway&) = delete;
    OrderGateway& operator=(const OrderGateway&) = delete;

    void start();
    void stop() noexcept;

    [[nodiscard]] OperationResult place(const OrderRequest& request);
    [[nodiscard]] OperationResult cancel(CancelRequest request);
    [[nodiscard]] OperationResult amend(AmendRequest request);
    [[nodiscard]] OperationResult reconcile(Venue venue);

    [[nodiscard]] std::optional<Order> get(std::string_view client_order_id) const;
    [[nodiscard]] std::vector<Order> list(std::optional<Venue> venue = std::nullopt,
                                          std::optional<OrderStatus> status = std::nullopt) const;
    [[nodiscard]] std::unordered_map<std::string, Decimal> positions() const;
    [[nodiscard]] BalanceQueryResult
    balances(Venue venue, std::optional<std::string> currency = std::nullopt) const;
    [[nodiscard]] InstrumentRulesQueryResult
    instrument_rules(Venue venue, std::string symbol) const;
    [[nodiscard]] std::unordered_map<Venue, VenueHealth> health() const;
    [[nodiscard]] GatewayStability stability() const;
    [[nodiscard]] std::vector<OperationalEvent>
    operational_events(std::size_t limit = 100) const;
    [[nodiscard]] std::vector<OperationalEvent>
    order_events(std::string_view client_order_id, std::size_t limit = 500) const;
    [[nodiscard]] ObserverToken add_order_observer(OrderObserver observer);
    void remove_order_observer(ObserverToken token) noexcept;
    [[nodiscard]] ObserverToken add_operational_observer(OperationalObserver observer);
    void remove_operational_observer(ObserverToken token) noexcept;
    void flush_events();

private:
    [[nodiscard]] std::shared_ptr<IExchangeAdapter> adapter_for(Venue venue) const;
    [[nodiscard]] std::vector<Order> orders_snapshot_locked() const;
    void rebuild_indexes_locked();
    void persist_locked(const Order& order);
    void record_event(OperationalSeverity severity,
                      std::string category,
                      std::string code,
                      std::string message,
                      std::optional<Venue> venue = std::nullopt,
                      std::string client_order_id = {},
                      std::string request_id = {},
                      std::optional<OrderEventContext> order = std::nullopt) noexcept;
    void receive_execution(Venue venue, ExecutionReport report);
    void apply_execution(Venue venue, const ExecutionReport& report);
    void connection_changed(Venue venue, bool connected, std::string reason);
    void start_reconciliation_worker();
    void stop_reconciliation_worker() noexcept;
    void schedule_reconciliation(Venue venue);
    void run_reconciliation_worker(std::stop_token token);
    [[nodiscard]] Order* locate_order_locked(Venue venue, const ExecutionReport& report);

    std::unordered_map<Venue, std::shared_ptr<IExchangeAdapter>> adapters_;
    RiskManager risk_manager_;
    std::shared_ptr<IOrderStore> order_store_;
    std::shared_ptr<MarketDataBook> market_data_;
    Options options_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Order> orders_;
    std::unordered_map<std::string, std::string> exchange_id_index_;
    std::unordered_map<std::string, std::string> exchange_client_id_index_;
    std::unordered_map<std::string, std::vector<ExecutionReport>> deferred_amend_reports_;
    std::unordered_set<std::string> active_operations_;
    std::unordered_map<Venue, SequenceTracker> sequence_trackers_;
    std::unordered_map<Venue, VenueHealth> health_;
    std::unordered_map<ObserverToken, OrderObserver> order_observers_;
    ObserverToken next_observer_token_{1};
    std::atomic<bool> started_{false};
    BoundedEventDispatcher dispatcher_;
    std::mutex reconciliation_mutex_;
    std::condition_variable_any reconciliation_condition_;
    std::deque<Venue> reconciliation_queue_;
    std::jthread reconciliation_worker_;

    const std::int64_t started_at_ms_;
    const std::string instance_id_;
    std::size_t recovered_orders_{0};
    bool previous_instance_present_{false};
    mutable std::mutex operational_mutex_;
    std::deque<OperationalEvent> operational_events_;
    std::unordered_map<ObserverToken, OperationalObserver> operational_observers_;
    ObserverToken next_operational_observer_token_{1};
    std::uint64_t idempotent_replays_{0};
    std::uint64_t reconciliations_{0};
    std::uint64_t alerts_{0};
    std::uint64_t logging_failures_{0};
    std::string last_logging_error_;
};

} // namespace abex
