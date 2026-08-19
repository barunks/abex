#include "abex/application/order_gateway.hpp"

#include "abex/domain/order_state_machine.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <stdexcept>
#include <unistd.h>

namespace abex {
namespace {

[[nodiscard]] OperationResult failure(std::string code,
                                      std::string message,
                                      std::optional<Order> order = std::nullopt) {
    return {.ok = false,
            .code = std::move(code),
            .message = std::move(message),
            .order = std::move(order)};
}

[[nodiscard]] OperationResult success(Order order, bool replay = false) {
    return {.ok = true, .idempotent_replay = replay, .order = std::move(order)};
}

constexpr char outcome_separator = '\x1f';

[[nodiscard]] std::string gateway_instance_id(std::int64_t started_at_ms) {
    return "gateway-" + std::to_string(::getpid()) + '-' + std::to_string(started_at_ms);
}

[[nodiscard]] std::uint64_t stable_hash(std::string_view value) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char character : value) {
        hash ^= character;
        hash *= 1099511628211ULL;
    }
    return hash;
}

struct FundingRequirement {
    std::string currency;
    Decimal amount;
};

[[nodiscard]] std::optional<FundingRequirement>
funding_requirement(const OrderRequest& request,
                    std::optional<Decimal> market_price) {
    const auto separator = request.symbol.find('-');
    if (separator == std::string::npos || separator == 0 ||
        separator + 1 >= request.symbol.size()) {
        return std::nullopt;
    }
    auto base = request.symbol.substr(0, separator);
    auto quote = request.symbol.substr(separator + 1);
    std::ranges::transform(base, base.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    std::ranges::transform(quote, quote.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    if (request.side == Side::Sell) return FundingRequirement{base, request.quantity};
    const auto price = request.type == OrderType::Market ? market_price : request.price;
    if (!price) return std::nullopt;
    return FundingRequirement{quote, request.quantity * *price};
}

[[nodiscard]] Decimal balance_decimal_floor(std::string text) {
    const auto exponent = text.find_first_of("eE");
    if (exponent != std::string::npos) {
        throw std::invalid_argument("scientific notation is not supported for balances");
    }
    if (const auto dot = text.find('.'); dot != std::string::npos) {
        const auto maximum_size = dot + 1 + static_cast<std::size_t>(Decimal::precision);
        if (text.size() > maximum_size) text.resize(maximum_size);
    }
    return Decimal::parse(text.empty() ? "0" : text);
}

[[nodiscard]] RiskDecision funding_decision(const OrderRequest& request,
                                            const FundingRequirement& requirement,
                                            const BalanceQueryResult& balances) {
    if (!balances.ok) {
        const auto detail = balances.message.empty() ? balances.code : balances.message;
        return RiskDecision::reject(
            "BALANCE_UNAVAILABLE",
            std::string(to_string(request.venue)) + " available balance could not be verified" +
                (detail.empty() ? std::string{} : ": " + detail));
    }

    const auto found = std::ranges::find_if(
        balances.snapshot.balances, [&](const AccountBalance& balance) {
            return balance.currency == requirement.currency;
        });
    Decimal available;
    try {
        if (found != balances.snapshot.balances.end()) {
            available = balance_decimal_floor(found->available);
        }
    } catch (const std::exception& error) {
        return RiskDecision::reject(
            "BALANCE_UNAVAILABLE",
            std::string(to_string(request.venue)) + " returned an invalid " + requirement.currency +
                " available balance: " + error.what());
    }
    if (available >= requirement.amount) return RiskDecision::accept();

    const auto frozen = found == balances.snapshot.balances.end()
                            ? std::string{"0"}
                            : found->frozen;
    auto reason = std::string(to_string(request.venue)) + " available " + requirement.currency +
                  " balance " + available.to_string() + " is below required " +
                  requirement.amount.to_string() + " " + requirement.currency +
                  " (frozen " + frozen + ')';
    if (!balances.snapshot.account_id.empty()) {
        reason += " on account " + balances.snapshot.account_id;
    }
    return RiskDecision::reject("INSUFFICIENT_AVAILABLE_BALANCE", std::move(reason));
}

[[nodiscard]] bool violates_step(Decimal value, const std::optional<Decimal>& step) {
    return step && step->is_positive() && value.raw() % step->raw() != 0;
}

[[nodiscard]] RiskDecision instrument_rules_decision(
    const OrderRequest& request,
    std::optional<Decimal> executable_price,
    const InstrumentRulesQueryResult& query) {
    if (!query.ok) {
        const auto detail = query.message.empty() ? query.code : query.message;
        return RiskDecision::reject(
            "INSTRUMENT_RULES_UNAVAILABLE",
            std::string(to_string(request.venue)) + " trading rules for " + request.symbol +
                " could not be verified" +
                (detail.empty() ? std::string{} : ": " + detail));
    }
    const auto& rules = query.rules;
    if (rules.symbol != request.symbol) {
        return RiskDecision::reject(
            "INSTRUMENT_RULES_UNAVAILABLE",
            std::string(to_string(request.venue)) + " returned rules for " + rules.symbol +
                " while " + request.symbol + " was requested");
    }
    if (!rules.trading) {
        return RiskDecision::reject(
            "INSTRUMENT_NOT_TRADING",
            request.symbol + " is not tradable on " + std::string(to_string(request.venue)) +
                " (venue status " + rules.status + ')');
    }

    const auto check_quantity_range = [&](const std::optional<Decimal>& minimum,
                                          const std::optional<Decimal>& maximum,
                                          const std::optional<Decimal>& step,
                                          std::string_view scope) -> std::optional<RiskDecision> {
        if (minimum && request.quantity < *minimum) {
            return RiskDecision::reject(
                "MIN_ORDER_QUANTITY",
                request.symbol + ' ' + std::string(scope) + " quantity " +
                    request.quantity.to_string() + " is below the venue minimum " +
                    minimum->to_string());
        }
        if (maximum && request.quantity > *maximum) {
            return RiskDecision::reject(
                "MAX_VENUE_ORDER_QUANTITY",
                request.symbol + ' ' + std::string(scope) + " quantity " +
                    request.quantity.to_string() + " exceeds the venue maximum " +
                    maximum->to_string());
        }
        if (violates_step(request.quantity, step)) {
            return RiskDecision::reject(
                "INVALID_QUANTITY_STEP",
                request.symbol + ' ' + std::string(scope) + " quantity " +
                    request.quantity.to_string() + " must be an exact multiple of " +
                    step->to_string());
        }
        return std::nullopt;
    };

    if (const auto violation = check_quantity_range(
            rules.minimum_quantity, rules.maximum_quantity, rules.quantity_step, "order")) {
        return *violation;
    }
    if (request.type == OrderType::Market) {
        if (const auto violation = check_quantity_range(
                rules.market_minimum_quantity, rules.market_maximum_quantity,
                rules.market_quantity_step, "market-order")) {
            return *violation;
        }
    }

    if (request.type == OrderType::Limit && request.price) {
        if (rules.minimum_price && *request.price < *rules.minimum_price) {
            return RiskDecision::reject(
                "PRICE_OUT_OF_RANGE",
                request.symbol + " price " + request.price->to_string() +
                    " is below the venue minimum " + rules.minimum_price->to_string());
        }
        if (rules.maximum_price && *request.price > *rules.maximum_price) {
            return RiskDecision::reject(
                "PRICE_OUT_OF_RANGE",
                request.symbol + " price " + request.price->to_string() +
                    " exceeds the venue maximum " + rules.maximum_price->to_string());
        }
        if (violates_step(*request.price, rules.price_tick)) {
            return RiskDecision::reject(
                "INVALID_PRICE_TICK",
                request.symbol + " price " + request.price->to_string() +
                    " must be an exact multiple of tick size " +
                    rules.price_tick->to_string());
        }
    }

    const auto price = request.type == OrderType::Limit ? request.price : executable_price;
    if (price) {
        const auto notional = request.quantity * *price;
        const auto& minimum = request.type == OrderType::Market
                                  ? rules.market_minimum_notional
                                  : rules.minimum_notional;
        const auto& maximum = request.type == OrderType::Market
                                  ? rules.market_maximum_notional
                                  : rules.maximum_notional;
        if (minimum && notional < *minimum) {
            return RiskDecision::reject(
                "MIN_ORDER_NOTIONAL",
                request.symbol + " estimated notional " + notional.to_string() +
                    " is below the venue minimum " + minimum->to_string());
        }
        if (maximum && notional > *maximum) {
            return RiskDecision::reject(
                "MAX_VENUE_ORDER_NOTIONAL",
                request.symbol + " estimated notional " + notional.to_string() +
                    " exceeds the venue maximum " + maximum->to_string());
        }
    }
    return RiskDecision::accept();
}

[[nodiscard]] std::string failed_outcome(std::string_view code, std::string_view message) {
    return "ERROR" + std::string(1, outcome_separator) + std::string(code) + outcome_separator +
           std::string(message);
}

[[nodiscard]] std::string pending_outcome(std::string_view code, std::string_view message) {
    return "PENDING" + std::string(1, outcome_separator) + std::string(code) +
           outcome_separator + std::string(message);
}

[[nodiscard]] OperationResult replay_operation(const Order& order, std::string_view outcome) {
    if (outcome == "OK") return success(order, true);
    if (outcome.starts_with("PENDING")) {
        const auto first = outcome.find(outcome_separator);
        const auto second = first == std::string_view::npos
                                ? std::string_view::npos
                                : outcome.find(outcome_separator, first + 1);
        return {.ok = false,
                .idempotent_replay = true,
                .code = first == std::string_view::npos
                            ? "OUTCOME_UNKNOWN"
                            : std::string(outcome.substr(first + 1, second - first - 1)),
                .message = second == std::string_view::npos
                               ? "the original operation requires reconciliation"
                               : std::string(outcome.substr(second + 1)),
                .order = order};
    }
    if (outcome.starts_with("ERROR")) {
        const auto first = outcome.find(outcome_separator);
        const auto second = first == std::string_view::npos
                                ? std::string_view::npos
                                : outcome.find(outcome_separator, first + 1);
        return {.ok = false,
                .idempotent_replay = true,
                .code = first == std::string_view::npos
                            ? "PREVIOUS_REQUEST_FAILED"
                            : std::string(outcome.substr(first + 1, second - first - 1)),
                .message = second == std::string_view::npos
                               ? "the original operation failed"
                               : std::string(outcome.substr(second + 1)),
                .order = order};
    }
    return success(order, true); // backward compatibility with schema-v1 journals
}

[[nodiscard]] OrderEventContext order_event_context(
    const Order& order,
    std::optional<std::uint64_t> venue_sequence = std::nullopt,
    std::int64_t exchange_time_ms = 0) {
    return {
        .exchange_order_id = order.exchange_order_id,
        .symbol = order.symbol,
        .side = std::string(to_string(order.side)),
        .type = std::string(to_string(order.type)),
        .price = order.price ? order.price->to_string() : std::string{},
        .quantity = order.quantity.to_string(),
        .filled_quantity = order.filled_quantity.to_string(),
        .status = std::string(to_string(order.status)),
        .pending_action = std::string(to_string(order.pending_action)),
        .rejection_reason = order.rejection_reason,
        .version = order.version,
        .venue_sequence = venue_sequence,
        .exchange_time_ms = exchange_time_ms,
    };
}

} // namespace

OrderGateway::OrderGateway(std::vector<std::shared_ptr<IExchangeAdapter>> adapters,
                           RiskManager risk_manager,
                           std::shared_ptr<IOrderStore> order_store)
    : OrderGateway(std::move(adapters), std::move(risk_manager), std::move(order_store),
                   Options{}) {}

OrderGateway::OrderGateway(std::vector<std::shared_ptr<IExchangeAdapter>> adapters,
                           RiskManager risk_manager,
                           std::shared_ptr<IOrderStore> order_store,
                           Options options,
                           std::shared_ptr<MarketDataBook> market_data)
    : risk_manager_(std::move(risk_manager)), order_store_(std::move(order_store)),
      market_data_(std::move(market_data)), options_(options),
      dispatcher_(options.event_queue_capacity,
                  [this](Venue venue, const ExecutionReport& report) {
                      apply_execution(venue, report);
                  }),
      started_at_ms_(unix_time_ms()), instance_id_(gateway_instance_id(started_at_ms_)) {
    if (!order_store_) throw std::invalid_argument("order store is required");
    for (auto& adapter : adapters) {
        if (!adapter) throw std::invalid_argument("exchange adapter is null");
        if (!adapters_.emplace(adapter->venue(), adapter).second) {
            throw std::invalid_argument("duplicate adapter for " +
                                        std::string(to_string(adapter->venue())));
        }
        health_[adapter->venue()] = {};
    }
    if (!adapters_.contains(Venue::Okx) || !adapters_.contains(Venue::Binance)) {
        throw std::invalid_argument("both OKX and Binance adapters are required");
    }

    const auto prior_events = order_store_->load_events(200);
    previous_instance_present_ = std::ranges::any_of(prior_events, [](const auto& event) {
        return event.code == "GATEWAY_STARTED" || event.code == "GATEWAY_RESTARTED";
    });
    operational_events_.insert(operational_events_.end(), prior_events.begin(), prior_events.end());

    auto recovered = order_store_->load_latest();
    recovered_orders_ = recovered.size();
    for (auto& order : recovered) {
        orders_[order.client_order_id] = std::move(order);
    }
    rebuild_indexes_locked();
}

OrderGateway::~OrderGateway() { stop(); }

void OrderGateway::start() {
    if (started_.exchange(true)) return;

    start_reconciliation_worker();

    const auto recovered = list();
    record_event(OperationalSeverity::Info, "LIFECYCLE",
                 previous_instance_present_ ? "GATEWAY_RESTARTED" : "GATEWAY_STARTED",
                 previous_instance_present_
                     ? "Gateway restarted and recovered " + std::to_string(recovered.size()) +
                           " order snapshots"
                     : "Gateway started with " + std::to_string(recovered.size()) +
                           " recovered order snapshots");
    try {
        for (auto& [venue, adapter] : adapters_) {
            adapter->restore(recovered);
            adapter->start(
                [this](Venue source, ExecutionReport report) {
                    receive_execution(source, std::move(report));
                },
                [this](Venue source, bool connected, std::string reason) {
                    connection_changed(source, connected, std::move(reason));
                });
            (void)venue;
        }
        if (options_.reconcile_on_start) {
            for (const auto& [venue, adapter] : adapters_) {
                (void)adapter;
                (void)reconcile(venue);
            }
        }
    } catch (...) {
        record_event(OperationalSeverity::Critical, "LIFECYCLE", "GATEWAY_START_FAILED",
                     "Gateway startup failed while initializing venue adapters");
        stop();
        throw;
    }
}

void OrderGateway::stop() noexcept {
    if (!started_.exchange(false)) return;
    stop_reconciliation_worker();
    for (auto& [venue, adapter] : adapters_) {
        (void)venue;
        adapter->stop();
    }
    dispatcher_.flush();
    record_event(OperationalSeverity::Info, "LIFECYCLE", "GATEWAY_STOPPED",
                 "Gateway stopped cleanly");
}

OperationResult OrderGateway::place(const OrderRequest& request) {
    const auto replay_if_present = [&]() -> std::optional<OperationResult> {
        const auto found = orders_.find(request.client_order_id);
        if (found == orders_.end()) return std::nullopt;
        if (found->second.create_fingerprint == fingerprint(request)) {
            if (found->second.status == OrderStatus::Rejected) {
                record_event(OperationalSeverity::Info, "RETRY", "IDEMPOTENT_REPLAY",
                             "Repeated create returned the persisted rejection",
                             request.venue, request.client_order_id, {},
                             order_event_context(found->second));
                return OperationResult{.ok = false,
                                       .idempotent_replay = true,
                                       .code = "ORDER_REJECTED",
                                       .message = found->second.rejection_reason,
                                       .order = found->second};
            }
            if (found->second.status == OrderStatus::Unknown &&
                found->second.pending_action == PendingAction::Reconcile) {
                record_event(OperationalSeverity::Warning, "RETRY", "IDEMPOTENT_REPLAY",
                             "Repeated create returned the persisted unknown outcome",
                             request.venue, request.client_order_id, {},
                             order_event_context(found->second));
                return OperationResult{.ok = false,
                                       .idempotent_replay = true,
                                       .code = "OUTCOME_UNKNOWN",
                                       .message = found->second.rejection_reason,
                                       .order = found->second};
            }
            record_event(OperationalSeverity::Info, "RETRY", "IDEMPOTENT_REPLAY",
                         "Repeated create returned the existing order without venue I/O",
                         request.venue, request.client_order_id, {},
                         order_event_context(found->second));
            return success(found->second, true);
        }
        record_event(OperationalSeverity::Warning, "RETRY", "IDEMPOTENCY_CONFLICT",
                     "clientOrderId was reused with a different order payload",
                     request.venue, request.client_order_id, {},
                     order_event_context(found->second));
        return failure("IDEMPOTENCY_CONFLICT",
                       "clientOrderId already exists with a different request",
                       found->second);
    };

    {
        std::scoped_lock lock(mutex_);
        if (auto replay = replay_if_present()) return *std::move(replay);
    }

    const auto current_market_price = market_data_
                                          ? market_data_->price(request.venue, request.symbol,
                                                                request.side)
                                          : std::nullopt;
    const auto adapter = adapter_for(request.venue);
    const auto instrument_check = instrument_rules_decision(
        request, current_market_price, adapter->query_instrument_rules(request.symbol));
    std::optional<RiskDecision> balance_check;
    if (instrument_check.accepted) {
        if (const auto requirement = funding_requirement(request, current_market_price)) {
            balance_check = funding_decision(
                request, *requirement,
                adapter->query_balances(requirement->currency));
        }
    }

    Order outbound;
    {
        std::scoped_lock lock(mutex_);
        if (auto replay = replay_if_present()) return *std::move(replay);

        auto decision = request.type == OrderType::Market && market_data_ &&
                                !current_market_price
                            ? RiskDecision::reject(
                                  "MARKET_DATA_UNAVAILABLE",
                                  "MARKET order requires a fresh executable quote")
                            : risk_manager_.check_new_with_position(
                                  request, conservative_position_locked(request.symbol),
                                  current_market_price);
        if (decision.accepted && !instrument_check.accepted) decision = instrument_check;
        if (decision.accepted && balance_check && !balance_check->accepted) {
            decision = *balance_check;
        }
        auto order = make_order(request);
        if (!decision.accepted) {
            order.status = OrderStatus::Rejected;
            order.pending_action = PendingAction::None;
            order.rejection_reason = decision.code + ": " + decision.reason;
            ++order.version;
            persist_locked(order);
            orders_[order.client_order_id] = order;
            record_event(OperationalSeverity::Warning, "RISK", "ORDER_REJECTED",
                         decision.code + ": " + decision.reason, request.venue,
                         request.client_order_id, {}, order_event_context(order));
            return failure(decision.code, decision.reason, std::move(order));
        }
        persist_locked(order); // intent is durable before it can reach an exchange
        orders_[order.client_order_id] = order;
        active_operations_.insert(order.client_order_id);
        outbound = order;
    }
    record_event(OperationalSeverity::Info, "PERSISTENCE", "ORDER_INTENT_PERSISTED",
                 "New-order intent was durably journaled before venue I/O", request.venue,
                 request.client_order_id, {}, order_event_context(outbound));
    record_event(OperationalSeverity::Info, "PIPELINE", "ORDER_SENT_TO_EXCHANGE",
                 "Persisted new order was sent to the venue adapter", request.venue,
                 request.client_order_id, {}, order_event_context(outbound));

    AdapterResult adapter_result;
    try {
        adapter_result = adapter->place(outbound);
    } catch (const std::exception& error) {
        adapter_result = {.outcome_uncertain = true,
                          .code = "ADAPTER_EXCEPTION",
                          .message = error.what()};
    }

    std::scoped_lock lock(mutex_);
    auto& order = orders_.at(request.client_order_id);
    active_operations_.erase(request.client_order_id);
    if (adapter_result.accepted) {
        if (!adapter_result.exchange_order_id.empty()) {
            order.exchange_order_id = adapter_result.exchange_order_id;
            exchange_id_index_[order.exchange_order_id] = order.client_order_id;
        }
        if (!adapter_result.exchange_client_order_id.empty()) {
            order.exchange_client_id_aliases.insert(adapter_result.exchange_client_order_id);
            exchange_client_id_index_[adapter_result.exchange_client_order_id] =
                order.client_order_id;
        }
        if (order.status == OrderStatus::Unknown) order.status = OrderStatus::Live;
        order.pending_action = PendingAction::None;
        order.updated_at_ms = unix_time_ms();
        ++order.version;
        persist_locked(order);
        record_event(OperationalSeverity::Info, "ORDER", "ORDER_ACKNOWLEDGED",
                     "Venue acknowledged the persisted new-order intent", order.venue,
                     order.client_order_id, {}, order_event_context(order));
        return success(order);
    }

    if (adapter_result.outcome_uncertain) {
        order.status = OrderStatus::Unknown;
        order.pending_action = PendingAction::Reconcile;
        order.rejection_reason = adapter_result.message;
        health_[order.venue].reconciliation_required = true;
        health_[order.venue].last_error = adapter_result.message;
        ++order.version;
        persist_locked(order);
        record_event(OperationalSeverity::Critical, "ORDER", "ORDER_OUTCOME_UNKNOWN",
                     adapter_result.message, order.venue, order.client_order_id, {},
                     order_event_context(order));
        return failure(adapter_result.code.empty() ? "OUTCOME_UNKNOWN" : adapter_result.code,
                       adapter_result.message, order);
    }

    ExecutionReport rejection{
        .event_id = "place-reject-" + order.client_order_id + '-' +
                    std::to_string(order.version),
        .client_order_id = order.client_order_id,
        .exchange_order_id = adapter_result.exchange_order_id,
        .status = OrderStatus::Rejected,
        .cumulative_filled = order.filled_quantity,
        .event_time_ms = unix_time_ms(),
        .reason = adapter_result.code + ": " + adapter_result.message,
    };
    (void)OrderStateMachine::apply(order, rejection);
    persist_locked(order);
    record_event(OperationalSeverity::Warning, "ORDER", "VENUE_ORDER_REJECTED",
                 adapter_result.code + ": " + adapter_result.message, order.venue,
                 order.client_order_id, {}, order_event_context(order));
    return failure(adapter_result.code, adapter_result.message, order);
}

OperationResult OrderGateway::cancel(CancelRequest request) {
    if (request.request_id.empty()) request.request_id = "cancel:" + request.client_order_id;
    Order outbound;
    const auto request_key = "CANCEL:" + request.request_id;
    const auto request_fingerprint = fingerprint(request);
    {
        std::scoped_lock lock(mutex_);
        const auto found = orders_.find(request.client_order_id);
        if (found == orders_.end()) {
            record_event(OperationalSeverity::Warning, "REQUEST", "ORDER_NOT_FOUND",
                         "Cancel referenced an unknown clientOrderId", std::nullopt,
                         request.client_order_id, request.request_id);
            return failure("ORDER_NOT_FOUND", "order does not exist");
        }
        auto& order = found->second;
        if (const auto replay = order.processed_requests.find(request_key);
            replay != order.processed_requests.end()) {
            if (replay->second != request_fingerprint) {
                record_event(OperationalSeverity::Warning, "RETRY", "IDEMPOTENCY_CONFLICT",
                             "requestId was reused with a different cancel payload", order.venue,
                             order.client_order_id, request.request_id,
                             order_event_context(order));
                return failure("IDEMPOTENCY_CONFLICT",
                               "requestId was reused with a different cancel request", order);
            }
            const auto outcome = order.processed_request_outcomes.find(request_key);
            record_event(OperationalSeverity::Info, "RETRY", "IDEMPOTENT_REPLAY",
                         "Repeated cancel returned its persisted outcome without venue I/O",
                         order.venue, order.client_order_id, request.request_id,
                         order_event_context(order));
            return replay_operation(order, outcome == order.processed_request_outcomes.end()
                                               ? std::string_view{}
                                               : std::string_view(outcome->second));
        }
        if (order.status == OrderStatus::Canceled) {
            record_event(OperationalSeverity::Info, "RETRY", "IDEMPOTENT_REPLAY",
                         "Cancel replay found the order already canceled", order.venue,
                         order.client_order_id, request.request_id,
                         order_event_context(order));
            return success(order, true);
        }
        if (is_terminal(order.status)) {
            record_event(OperationalSeverity::Warning, "ORDER", "CANCEL_REJECTED",
                         "terminal order cannot be canceled", order.venue,
                         order.client_order_id, request.request_id, order_event_context(order));
            return failure("ORDER_TERMINAL", "terminal order cannot be canceled", order);
        }
        if (active_operations_.contains(order.client_order_id) ||
            order.pending_action != PendingAction::None) {
            return failure("OPERATION_PENDING",
                           "another order operation is still pending", order);
        }
        order.processed_requests[request_key] = request_fingerprint;
        order.processed_request_outcomes[request_key] = "PENDING";
        order.pending_action = PendingAction::Cancel;
        order.updated_at_ms = unix_time_ms();
        ++order.version;
        persist_locked(order);
        active_operations_.insert(order.client_order_id);
        outbound = order;
    }
    record_event(OperationalSeverity::Info, "PERSISTENCE", "CANCEL_INTENT_PERSISTED",
                 "Cancel intent and requestId were durably journaled before venue I/O",
                 outbound.venue, outbound.client_order_id, request.request_id,
                 order_event_context(outbound));
    record_event(OperationalSeverity::Info, "PIPELINE", "CANCEL_SENT_TO_EXCHANGE",
                 "Persisted cancel request was sent to the venue adapter", outbound.venue,
                 outbound.client_order_id, request.request_id, order_event_context(outbound));

    AdapterResult result;
    try {
        result = adapter_for(outbound.venue)->cancel(outbound);
    } catch (const std::exception& error) {
        result = {.outcome_uncertain = true,
                  .code = "ADAPTER_EXCEPTION",
                  .message = error.what()};
    }

    std::scoped_lock lock(mutex_);
    auto& order = orders_.at(request.client_order_id);
    active_operations_.erase(request.client_order_id);
    if (result.accepted) {
        order.processed_request_outcomes[request_key] = "OK";
        order.rejection_reason.clear();
        persist_locked(order);
        record_event(OperationalSeverity::Info, "ORDER", "CANCEL_ACKNOWLEDGED",
                     "Venue acknowledged the persisted cancel request", order.venue,
                     order.client_order_id, request.request_id, order_event_context(order));
        return success(order);
    }
    order.pending_action = result.outcome_uncertain ? PendingAction::Cancel : PendingAction::None;
    if (result.outcome_uncertain) {
        order.status = OrderStatus::Unknown;
        health_[order.venue].reconciliation_required = true;
    }
    order.rejection_reason = result.message;
    order.processed_request_outcomes[request_key] = result.outcome_uncertain
                                                       ? pending_outcome(result.code, result.message)
                                                       : failed_outcome(result.code, result.message);
    ++order.version;
    persist_locked(order);
    record_event(result.outcome_uncertain ? OperationalSeverity::Critical
                                          : OperationalSeverity::Warning,
                 "ORDER", result.outcome_uncertain ? "CANCEL_OUTCOME_UNKNOWN"
                                                    : "CANCEL_REJECTED",
                 result.message, order.venue, order.client_order_id, request.request_id,
                 order_event_context(order));
    return failure(result.code, result.message, order);
}

OperationResult OrderGateway::amend(AmendRequest request) {
    if (request.request_id.empty()) {
        request.request_id = "amend:" +
                             std::to_string(stable_hash(fingerprint(request)));
    }
    Order outbound;
    const auto request_key = "AMEND:" + request.request_id;
    const auto request_fingerprint = fingerprint(request);
    {
        std::scoped_lock lock(mutex_);
        const auto found = orders_.find(request.client_order_id);
        if (found == orders_.end()) {
            record_event(OperationalSeverity::Warning, "REQUEST", "ORDER_NOT_FOUND",
                         "Amend referenced an unknown clientOrderId", std::nullopt,
                         request.client_order_id, request.request_id);
            return failure("ORDER_NOT_FOUND", "order does not exist");
        }
        auto& order = found->second;
        if (const auto replay = order.processed_requests.find(request_key);
            replay != order.processed_requests.end()) {
            if (replay->second != request_fingerprint) {
                record_event(OperationalSeverity::Warning, "RETRY", "IDEMPOTENCY_CONFLICT",
                             "requestId was reused with a different amend payload", order.venue,
                             order.client_order_id, request.request_id,
                             order_event_context(order));
                return failure("IDEMPOTENCY_CONFLICT",
                               "requestId was reused with a different amend request", order);
            }
            const auto outcome = order.processed_request_outcomes.find(request_key);
            record_event(OperationalSeverity::Info, "RETRY", "IDEMPOTENT_REPLAY",
                         "Repeated amend returned its persisted outcome without venue I/O",
                         order.venue, order.client_order_id, request.request_id,
                         order_event_context(order));
            return replay_operation(order, outcome == order.processed_request_outcomes.end()
                                               ? std::string_view{}
                                               : std::string_view(outcome->second));
        }
        if (active_operations_.contains(order.client_order_id)) {
            return failure("OPERATION_PENDING",
                           "another order operation is still in venue I/O", order);
        }
        const auto decision = risk_manager_.check_amend_with_position(
            order, request.new_price, request.new_quantity,
            conservative_position_locked(order.symbol, order.client_order_id));
        if (!decision.accepted) {
            record_event(OperationalSeverity::Warning, "RISK", "AMEND_REJECTED",
                         decision.code + ": " + decision.reason, order.venue,
                         order.client_order_id, request.request_id, order_event_context(order));
            return failure(decision.code, decision.reason, order);
        }

        order.processed_requests[request_key] = request_fingerprint;
        order.processed_request_outcomes[request_key] = "PENDING";
        order.pending_action = PendingAction::Amend;
        order.pending_amend_price = request.new_price;
        order.pending_amend_quantity = request.new_quantity;
        order.updated_at_ms = unix_time_ms();
        ++order.version;
        persist_locked(order);
        active_operations_.insert(order.client_order_id);
        outbound = order;
    }
    record_event(OperationalSeverity::Info, "PERSISTENCE", "AMEND_INTENT_PERSISTED",
                 "Amend intent and requestId were durably journaled before venue I/O",
                 outbound.venue, outbound.client_order_id, request.request_id,
                 order_event_context(outbound));
    record_event(OperationalSeverity::Info, "PIPELINE", "AMEND_SENT_TO_EXCHANGE",
                 "Persisted amend request was sent to the venue adapter", outbound.venue,
                 outbound.client_order_id, request.request_id, order_event_context(outbound));

    AdapterResult result;
    try {
        result = adapter_for(outbound.venue)->amend(
            outbound, request.new_price, request.new_quantity);
    } catch (const std::exception& error) {
        result = {.outcome_uncertain = true,
                  .code = "ADAPTER_EXCEPTION",
                  .message = error.what()};
    }

    OperationResult response;
    std::vector<ExecutionReport> reports_to_apply = result.authoritative_reports;
    {
        std::scoped_lock lock(mutex_);
        auto& order = orders_.at(request.client_order_id);
        active_operations_.erase(request.client_order_id);
        if (result.accepted) {
            std::string replacement_warning;
            if (result.replacement) {
                const auto old_exchange_id = order.exchange_order_id;
                Decimal final_old_fill = order.filled_quantity;
                Decimal final_old_quote = order.cumulative_quote;
                const auto old_fill_offset = order.exchange_fill_offsets.contains(old_exchange_id)
                                                 ? order.exchange_fill_offsets.at(old_exchange_id)
                                                 : Decimal{};
                const auto old_quote_offset = order.exchange_quote_offsets.contains(old_exchange_id)
                                                  ? order.exchange_quote_offsets.at(old_exchange_id)
                                                  : Decimal{};
                for (const auto& report : result.authoritative_reports) {
                    if (report.exchange_order_id != old_exchange_id) continue;
                    final_old_fill = std::max(
                        final_old_fill, old_fill_offset + report.cumulative_filled);
                    if (report.cumulative_quote) {
                        final_old_quote = std::max(
                            final_old_quote, old_quote_offset + *report.cumulative_quote);
                    }
                }

                if (!old_exchange_id.empty()) {
                    order.exchange_order_id_aliases.insert(old_exchange_id);
                    order.exchange_fill_offsets.try_emplace(old_exchange_id, old_fill_offset);
                    order.exchange_quote_offsets.try_emplace(old_exchange_id, old_quote_offset);
                    exchange_id_index_[old_exchange_id] = order.client_order_id;
                }
                order.filled_quantity = final_old_fill;
                order.cumulative_quote = final_old_quote;
                order.exchange_order_id = result.exchange_order_id;
                if (!result.exchange_order_id.empty()) {
                    order.exchange_fill_offsets[result.exchange_order_id] = final_old_fill;
                    order.exchange_quote_offsets[result.exchange_order_id] = final_old_quote;
                    exchange_id_index_[result.exchange_order_id] = order.client_order_id;
                }
                order.status = final_old_fill > Decimal{} ? OrderStatus::PartiallyFilled
                                                          : OrderStatus::Live;

                // If the original filled while cancel-replace was executing, the actual
                // canonical quantity is old final fill plus the replacement quantity.
                // Record that venue reality rather than pretending the requested target
                // was achieved exactly.
                for (const auto& report : result.authoritative_reports) {
                    if (report.exchange_order_id == result.exchange_order_id &&
                        report.order_quantity && report.status != OrderStatus::Canceled) {
                        const auto actual_quantity = final_old_fill + *report.order_quantity;
                        const auto requested_quantity =
                            request.new_quantity.value_or(outbound.quantity);
                        order.pending_amend_quantity = actual_quantity;
                        if (actual_quantity != requested_quantity) {
                            replacement_warning =
                                "old order filled during cancel-replace; authoritative total quantity is " +
                                actual_quantity.to_string() + " instead of requested " +
                                requested_quantity.to_string();
                        }
                    }
                }
            }
            if (!result.exchange_client_order_id.empty()) {
                order.exchange_client_id_aliases.insert(result.exchange_client_order_id);
                exchange_client_id_index_[result.exchange_client_order_id] =
                    order.client_order_id;
            }
            order.processed_request_outcomes[request_key] = "OK";
            order.rejection_reason = replacement_warning;
            ++order.version;
            persist_locked(order);
            if (!replacement_warning.empty()) {
                record_event(OperationalSeverity::Critical, "ORDER",
                             "REPLACEMENT_QUANTITY_DRIFT", replacement_warning,
                             order.venue, order.client_order_id, request.request_id,
                             order_event_context(order));
            }
            record_event(OperationalSeverity::Info, "ORDER", "AMEND_ACKNOWLEDGED",
                         result.replacement
                             ? "Venue accepted cancel-replace; authoritative generation reports are being merged"
                             : "Venue accepted the amend request; final terms await the order stream or query",
                         order.venue, order.client_order_id, request.request_id,
                         order_event_context(order));
            response = success(order);
        } else {
            order.pending_action = result.outcome_uncertain ? PendingAction::Amend
                                                            : PendingAction::None;
            if (result.outcome_uncertain) {
                order.status = OrderStatus::Unknown;
                health_[order.venue].reconciliation_required = true;
                order.processed_request_outcomes[request_key] =
                    pending_outcome(result.code, result.message);
            } else {
                order.pending_amend_price.reset();
                order.pending_amend_quantity.reset();
                order.processed_request_outcomes[request_key] =
                    failed_outcome(result.code, result.message);
            }
            order.rejection_reason = result.message;
            ++order.version;
            persist_locked(order);
            record_event(result.outcome_uncertain ? OperationalSeverity::Critical
                                                  : OperationalSeverity::Warning,
                         "ORDER", result.outcome_uncertain ? "AMEND_OUTCOME_UNKNOWN"
                                                            : "AMEND_REJECTED",
                         result.message, order.venue, order.client_order_id,
                         request.request_id, order_event_context(order));
            response = failure(result.code, result.message, order);
        }

        if (auto deferred = deferred_amend_reports_.find(order.client_order_id);
            deferred != deferred_amend_reports_.end()) {
            reports_to_apply.insert(reports_to_apply.end(),
                                    std::make_move_iterator(deferred->second.begin()),
                                    std::make_move_iterator(deferred->second.end()));
            deferred_amend_reports_.erase(deferred);
        }
    }

    for (const auto& report : reports_to_apply) apply_execution(outbound.venue, report);
    if (const auto current = get(request.client_order_id)) response.order = current;
    return response;
}

OperationResult OrderGateway::reconcile(Venue venue) {
    record_event(OperationalSeverity::Info, "RECONCILIATION", "RECONCILIATION_STARTED",
                 "Venue reconciliation started", venue);
    const auto adapter = adapter_for(venue);
    const auto candidates = list(venue);
    std::size_t reconciled = 0;
    std::size_t unresolved = 0;
    std::unordered_set<std::string> observed_open_orders;

    try {
        if (auto open_reports = adapter->query_open_orders()) {
            for (auto& report : *open_reports) {
                std::string canonical_client_id;
                bool terminal_conflict = false;
                {
                    std::scoped_lock lock(mutex_);
                    if (auto* order = locate_order_locked(venue, report)) {
                        canonical_client_id = order->client_order_id;
                        terminal_conflict = is_terminal(order->status);
                    }
                }
                if (canonical_client_id.empty()) {
                    // Open-order snapshots are account-wide. The durable journal is the
                    // gateway ownership boundary, so orders created by another application,
                    // another journal, or the venue UI are outside this reconciliation run.
                    // Silently ignore them: they must not degrade this gateway's health or
                    // be adopted/canceled without an explicit ownership transfer.
                    continue;
                }
                if (terminal_conflict) {
                    ++unresolved;
                    record_event(OperationalSeverity::Critical, "RECONCILIATION",
                                 "TERMINAL_ORDER_STILL_OPEN",
                                 "Venue reports an order open while the journal records a terminal state",
                                 venue, canonical_client_id);
                    continue;
                }
                report.client_order_id = canonical_client_id;
                observed_open_orders.insert(canonical_client_id);
                apply_execution(venue, report);
                if (const auto current = get(canonical_client_id);
                    current && current->pending_action != PendingAction::None) {
                    ++unresolved;
                } else {
                    ++reconciled;
                }
            }
        } else {
            ++unresolved;
            record_event(OperationalSeverity::Warning, "RECONCILIATION",
                         "OPEN_ORDER_SNAPSHOT_UNAVAILABLE",
                         "Venue did not provide an authoritative open-order snapshot", venue);
        }
    } catch (const std::exception& error) {
        ++unresolved;
        std::scoped_lock lock(mutex_);
        health_[venue].last_error = error.what();
    }

    for (const auto& order : candidates) {
        if (is_terminal(order.status) || observed_open_orders.contains(order.client_order_id)) {
            continue;
        }
        Order query_order;
        {
            std::scoped_lock lock(mutex_);
            if (active_operations_.contains(order.client_order_id)) continue;
            query_order = orders_.at(order.client_order_id);
            if (is_terminal(query_order.status)) continue;
        }
        try {
            if (auto report = adapter->query(query_order)) {
                apply_execution(venue, *report);
                if (const auto current = get(query_order.client_order_id);
                    current && current->pending_action != PendingAction::None) {
                    ++unresolved;
                } else {
                    ++reconciled;
                }
            } else {
                std::scoped_lock lock(mutex_);
                auto& current = orders_.at(order.client_order_id);
                current.status = OrderStatus::Unknown;
                current.pending_action = PendingAction::Reconcile;
                ++current.version;
                persist_locked(current);
                ++unresolved;
            }
        } catch (const std::exception& error) {
            std::scoped_lock lock(mutex_);
            health_[venue].last_error = error.what();
            ++unresolved;
        }
    }
    {
        std::scoped_lock lock(mutex_);
        health_[venue].reconciliation_required = unresolved != 0;
        if (unresolved == 0) sequence_trackers_[venue].clear_gap();
    }
    if (unresolved != 0) {
        record_event(OperationalSeverity::Warning, "RECONCILIATION",
                     "RECONCILIATION_INCOMPLETE",
                     std::to_string(reconciled) + " reconciled, " +
                         std::to_string(unresolved) + " unresolved",
                     venue);
        return failure("RECONCILIATION_INCOMPLETE",
                       std::to_string(reconciled) + " reconciled, " +
                           std::to_string(unresolved) + " unresolved");
    }
    record_event(OperationalSeverity::Info, "RECONCILIATION", "RECONCILIATION_SUCCEEDED",
                 std::to_string(reconciled) + " orders reconciled against venue state", venue);
    return {.ok = true,
            .message = std::to_string(reconciled) + " orders reconciled against venue state"};
}

std::optional<Order> OrderGateway::get(std::string_view client_order_id) const {
    std::scoped_lock lock(mutex_);
    const auto found = orders_.find(client_order_id);
    return found == orders_.end() ? std::nullopt : std::optional(found->second);
}

std::vector<Order> OrderGateway::list(std::optional<Venue> venue,
                                      std::optional<OrderStatus> status) const {
    std::scoped_lock lock(mutex_);
    std::vector<Order> result;
    for (const auto& [id, order] : orders_) {
        (void)id;
        if (venue && order.venue != *venue) continue;
        if (status && order.status != *status) continue;
        result.push_back(order);
    }
    std::ranges::sort(result, [](const Order& lhs, const Order& rhs) {
        if (lhs.created_at_ms != rhs.created_at_ms) return lhs.created_at_ms < rhs.created_at_ms;
        return lhs.client_order_id < rhs.client_order_id;
    });
    return result;
}

std::unordered_map<std::string, Decimal> OrderGateway::positions() const {
    std::scoped_lock lock(mutex_);
    std::unordered_map<std::string, Decimal> result;
    result.reserve(risk_manager_.limits().size());
    for (const auto& [id, order] : orders_) {
        (void)id;
        const auto exposure = is_terminal(order.status) ? order.filled_quantity : order.quantity;
        result[order.symbol] += order.side == Side::Buy ? exposure : -exposure;
    }
    return result;
}

BalanceQueryResult
OrderGateway::balances(Venue venue, std::optional<std::string> currency) const {
    return adapter_for(venue)->query_balances(std::move(currency));
}

InstrumentRulesQueryResult
OrderGateway::instrument_rules(Venue venue, std::string symbol) const {
    return adapter_for(venue)->query_instrument_rules(std::move(symbol));
}

std::unordered_map<Venue, VenueHealth> OrderGateway::health() const {
    std::scoped_lock lock(mutex_);
    return health_;
}

GatewayStability OrderGateway::stability() const {
    GatewayStability result;
    {
        std::scoped_lock lock(operational_mutex_);
        result.instance_id = instance_id_;
        result.started_at_ms = started_at_ms_;
        result.recovered_orders = recovered_orders_;
        result.idempotent_replays = idempotent_replays_;
        result.reconciliations = reconciliations_;
        result.alerts = alerts_;
        result.logging_failures = logging_failures_;
        result.last_logging_error = last_logging_error_;
    }
    result.journal = order_store_->status();
    return result;
}

std::vector<OperationalEvent> OrderGateway::operational_events(std::size_t limit) const {
    std::scoped_lock lock(operational_mutex_);
    const auto count = std::min(limit, operational_events_.size());
    return {operational_events_.end() - static_cast<std::ptrdiff_t>(count),
            operational_events_.end()};
}

std::vector<OperationalEvent>
OrderGateway::order_events(std::string_view client_order_id, std::size_t limit) const {
    return order_store_->load_order_events(client_order_id, limit);
}

OrderGateway::ObserverToken OrderGateway::add_order_observer(OrderObserver observer) {
    if (!observer) throw std::invalid_argument("order observer is empty");
    std::scoped_lock lock(mutex_);
    const auto token = next_observer_token_++;
    order_observers_.emplace(token, std::move(observer));
    return token;
}

void OrderGateway::remove_order_observer(ObserverToken token) noexcept {
    std::scoped_lock lock(mutex_);
    order_observers_.erase(token);
}

OrderGateway::ObserverToken
OrderGateway::add_operational_observer(OperationalObserver observer) {
    if (!observer) throw std::invalid_argument("operational observer is empty");
    std::scoped_lock lock(operational_mutex_);
    const auto token = next_operational_observer_token_++;
    operational_observers_.emplace(token, std::move(observer));
    return token;
}

void OrderGateway::remove_operational_observer(ObserverToken token) noexcept {
    std::scoped_lock lock(operational_mutex_);
    operational_observers_.erase(token);
}

void OrderGateway::flush_events() { dispatcher_.flush(); }

std::shared_ptr<IExchangeAdapter> OrderGateway::adapter_for(Venue venue) const {
    const auto found = adapters_.find(venue);
    if (found == adapters_.end()) throw std::logic_error("venue adapter is unavailable");
    return found->second;
}

Decimal OrderGateway::conservative_position_locked(
    std::string_view symbol,
    std::string_view excluded_client_order_id) const {
    Decimal result;
    for (const auto& [id, order] : orders_) {
        (void)id;
        if (order.symbol != symbol || order.client_order_id == excluded_client_order_id) continue;
        const auto exposure = is_terminal(order.status) ? order.filled_quantity : order.quantity;
        result += order.side == Side::Buy ? exposure : -exposure;
    }
    return result;
}

void OrderGateway::rebuild_indexes_locked() {
    exchange_id_index_.clear();
    exchange_client_id_index_.clear();
    for (const auto& [client_id, order] : orders_) {
        if (!order.exchange_order_id.empty()) {
            exchange_id_index_[order.exchange_order_id] = client_id;
        }
        for (const auto& alias : order.exchange_order_id_aliases) {
            exchange_id_index_[alias] = client_id;
        }
        exchange_client_id_index_[client_id] = client_id;
        for (const auto& alias : order.exchange_client_id_aliases) {
            exchange_client_id_index_[alias] = client_id;
        }
    }
}

void OrderGateway::persist_locked(const Order& order) {
    order_store_->append(order);
    for (const auto& [token, observer] : order_observers_) {
        (void)token;
        try {
            observer(order);
        } catch (...) {
            // Observability must never change order processing semantics.
        }
    }
}

void OrderGateway::record_event(OperationalSeverity severity,
                                std::string category,
                                std::string code,
                                std::string message,
                                std::optional<Venue> venue,
                                std::string client_order_id,
                                std::string request_id,
                                std::optional<OrderEventContext> order) noexcept {
    OperationalEvent event{
        .occurred_at_ms = unix_time_ms(),
        .severity = severity,
        .category = std::move(category),
        .code = std::move(code),
        .message = std::move(message),
        .instance_id = instance_id_,
        .venue = venue,
        .client_order_id = std::move(client_order_id),
        .request_id = std::move(request_id),
        .order = std::move(order),
    };
    std::vector<OperationalObserver> observers;
    {
        std::scoped_lock lock(operational_mutex_);
        // Serialize append and publication so concurrent adapter callbacks cannot expose
        // operational events in an order different from their durable journal sequence.
        try {
            event = order_store_->append_event(std::move(event));
        } catch (const std::exception& error) {
            ++logging_failures_;
            last_logging_error_ = error.what();
            return;
        } catch (...) {
            ++logging_failures_;
            last_logging_error_ = "unknown operational logging failure";
            return;
        }
        operational_events_.push_back(event);
        while (operational_events_.size() > 200) operational_events_.pop_front();
        if (event.code == "IDEMPOTENT_REPLAY") ++idempotent_replays_;
        if (event.code == "RECONCILIATION_STARTED") ++reconciliations_;
        if (event.severity != OperationalSeverity::Info) ++alerts_;
        observers.reserve(operational_observers_.size());
        for (const auto& [token, observer] : operational_observers_) {
            (void)token;
            observers.push_back(observer);
        }
    }
    for (const auto& observer : observers) {
        try {
            observer(event);
        } catch (...) {
            // Operational observers cannot alter order processing.
        }
    }
}

void OrderGateway::receive_execution(Venue venue, ExecutionReport report) {
    if (!dispatcher_.submit(venue, std::move(report))) {
        std::scoped_lock lock(mutex_);
        ++health_[venue].dropped_events;
        health_[venue].reconciliation_required = true;
        health_[venue].last_error = "execution event queue remained full";
        record_event(OperationalSeverity::Critical, "BACKPRESSURE", "EXECUTION_EVENT_DROPPED",
                     health_[venue].last_error, venue);
    }
}

void OrderGateway::apply_execution(Venue venue, const ExecutionReport& report) {
    std::scoped_lock lock(mutex_);

    auto* order = locate_order_locked(venue, report);
    if (!order) {
        // Private order streams are account-wide on the supported venues. A report that
        // cannot be correlated with the durable journal belongs outside this gateway's
        // ownership scope and must not create a false alert or request reconciliation.
        return;
    }

    if (report.sequence) {
        const auto observed = sequence_trackers_[venue].observe(*report.sequence);
        if (observed.observation == SequenceObservation::Gap) {
            ++health_[venue].sequence_gaps;
            health_[venue].reconciliation_required = true;
            health_[venue].last_error = "execution sequence gap: expected " +
                                        std::to_string(*observed.expected) + ", received " +
                                        std::to_string(observed.received);
            record_event(OperationalSeverity::Critical, "SEQUENCING", "EXECUTION_SEQUENCE_GAP",
                         health_[venue].last_error, venue, report.client_order_id);
        }
    }
    const bool unexpected_generation =
        !report.exchange_order_id.empty() && !order->exchange_order_id.empty() &&
        report.exchange_order_id != order->exchange_order_id &&
        !order->exchange_order_id_aliases.contains(report.exchange_order_id);
    const bool canceling_generation = report.status == OrderStatus::Canceled &&
                                      report.exchange_order_id == order->exchange_order_id;
    if (venue == Venue::Binance && order->pending_action == PendingAction::Amend &&
        (unexpected_generation || canceling_generation)) {
        deferred_amend_reports_[order->client_order_id].push_back(report);
        record_event(OperationalSeverity::Info, "PIPELINE", "AMEND_REPORT_DEFERRED",
                     "Binance cancel-replace report was held until the compound response established the active generation",
                     venue, order->client_order_id);
        return;
    }

    const auto previous_pending_action = order->pending_action;
    const auto result = OrderStateMachine::apply(*order, report);
    if (result.disposition == ApplyDisposition::Invalid) {
        health_[venue].last_error = result.reason;
        health_[venue].reconciliation_required = true;
        record_event(OperationalSeverity::Critical, "ORDER", "INVALID_EXECUTION_TRANSITION",
                     result.reason, venue, order->client_order_id);
        return;
    }
    if (!order->exchange_order_id.empty()) {
        exchange_id_index_[order->exchange_order_id] = order->client_order_id;
    }
    if (!report.client_order_id.empty() && report.client_order_id != order->client_order_id) {
        order->exchange_client_id_aliases.insert(report.client_order_id);
        exchange_client_id_index_[report.client_order_id] = order->client_order_id;
    }
    if (result.disposition != ApplyDisposition::Duplicate &&
        (result.state_changed || !report.event_id.empty())) {
        persist_locked(*order);
        std::string event_code = "ORDER_EXECUTION_UPDATED";
        if (result.disposition == ApplyDisposition::Stale) {
            event_code = "STALE_EXECUTION_MERGED";
        } else if (order->status == OrderStatus::Filled) {
            event_code = "ORDER_FILLED";
        } else if (order->status == OrderStatus::Canceled) {
            event_code = "ORDER_CANCELED";
        } else if (order->status == OrderStatus::Rejected) {
            event_code = "VENUE_ORDER_REJECTED";
        } else if (previous_pending_action == PendingAction::Amend &&
                   order->pending_action == PendingAction::None) {
            event_code = "AMEND_CONFIRMED";
        } else if (order->status == OrderStatus::PartiallyFilled) {
            event_code = "ORDER_PARTIALLY_FILLED";
        } else if (order->status == OrderStatus::Live) {
            event_code = "NEW_ORDER_CONFIRMED";
        }
        const auto message = report.reason.empty()
                                 ? "Venue execution update: " + std::string(to_string(order->status)) +
                                       ", filled " + order->filled_quantity.to_string() + "/" +
                                       order->quantity.to_string()
                                 : report.reason;
        record_event(order->status == OrderStatus::Rejected
                         ? OperationalSeverity::Warning
                         : OperationalSeverity::Info,
                     "PIPELINE", std::move(event_code), message, venue,
                     order->client_order_id, {},
                     order_event_context(*order, report.sequence, report.event_time_ms));
    }
}

void OrderGateway::connection_changed(Venue venue, bool connected, std::string reason) {
    bool reconnect = false;
    {
        std::scoped_lock lock(mutex_);
        auto& state = health_[venue];
        const bool was_connected = state.connected;
        reconnect = connected && !state.connected && state.ever_connected;
        state.connected = connected;
        if (!connected || reconnect) state.reconciliation_required = true;
        if (connected) {
            state.ever_connected = true;
            state.last_error.clear();
            record_event(reconnect ? OperationalSeverity::Warning : OperationalSeverity::Info,
                         "CONNECTIVITY", reconnect ? "VENUE_RECONNECTED" : "VENUE_CONNECTED",
                         reconnect ? "Venue connection recovered; reconciliation was scheduled"
                                   : "Venue connection established",
                         venue);
        } else if (was_connected || state.last_error.empty()) {
            state.last_error = reason.empty() ? "venue connection disconnected" : std::move(reason);
            record_event(started_.load() ? OperationalSeverity::Critical
                                         : OperationalSeverity::Info,
                         "CONNECTIVITY",
                         started_.load() ? "VENUE_DISCONNECTED" : "VENUE_STOPPED",
                         state.last_error, venue);
        }
    }
    if (reconnect && options_.reconcile_on_reconnect) schedule_reconciliation(venue);
}

void OrderGateway::start_reconciliation_worker() {
    std::scoped_lock lock(reconciliation_mutex_);
    if (reconciliation_worker_.joinable()) return;
    reconciliation_queue_.clear();
    reconciliation_worker_ =
        std::jthread([this](std::stop_token token) { run_reconciliation_worker(token); });
}

void OrderGateway::stop_reconciliation_worker() noexcept {
    if (!reconciliation_worker_.joinable()) return;
    reconciliation_worker_.request_stop();
    reconciliation_condition_.notify_all();
    reconciliation_worker_.join();
    std::scoped_lock lock(reconciliation_mutex_);
    reconciliation_queue_.clear();
}

void OrderGateway::schedule_reconciliation(Venue venue) {
    {
        std::scoped_lock lock(reconciliation_mutex_);
        if (std::ranges::find(reconciliation_queue_, venue) == reconciliation_queue_.end()) {
            reconciliation_queue_.push_back(venue);
        }
    }
    reconciliation_condition_.notify_one();
}

void OrderGateway::run_reconciliation_worker(std::stop_token token) {
    auto next_periodic = std::chrono::steady_clock::now() +
                         options_.reconciliation_interval;
    while (!token.stop_requested()) {
        Venue venue{};
        {
            std::unique_lock lock(reconciliation_mutex_);
            if (options_.reconciliation_interval > std::chrono::milliseconds::zero()) {
                (void)reconciliation_condition_.wait_until(
                    lock, token, next_periodic,
                    [this] { return !reconciliation_queue_.empty(); });
            } else {
                reconciliation_condition_.wait(
                    lock, token, [this] { return !reconciliation_queue_.empty(); });
            }
            if (token.stop_requested()) return;
            if (reconciliation_queue_.empty() &&
                options_.reconciliation_interval > std::chrono::milliseconds::zero() &&
                std::chrono::steady_clock::now() >= next_periodic) {
                for (const auto& [scheduled_venue, adapter] : adapters_) {
                    (void)adapter;
                    reconciliation_queue_.push_back(scheduled_venue);
                }
                next_periodic = std::chrono::steady_clock::now() +
                                options_.reconciliation_interval;
            }
            if (reconciliation_queue_.empty()) continue;
            venue = reconciliation_queue_.front();
            reconciliation_queue_.pop_front();
        }
        if (!started_.load()) continue;
        try {
            (void)reconcile(venue);
        } catch (const std::exception& error) {
            record_event(OperationalSeverity::Critical, "RECONCILIATION",
                         "RECONCILIATION_FAILED", error.what(), venue);
        } catch (...) {
            record_event(OperationalSeverity::Critical, "RECONCILIATION",
                         "RECONCILIATION_FAILED", "unknown reconciliation failure", venue);
        }
    }
}

Order* OrderGateway::locate_order_locked(Venue venue, const ExecutionReport& report) {
    const auto owned_by_venue = [venue](Order* order) {
        return order != nullptr && order->venue == venue ? order : nullptr;
    };
    if (!report.client_order_id.empty()) {
        if (auto direct = orders_.find(report.client_order_id); direct != orders_.end()) {
            if (auto* order = owned_by_venue(&direct->second)) return order;
        }
        if (auto alias = exchange_client_id_index_.find(report.client_order_id);
            alias != exchange_client_id_index_.end()) {
            if (auto* order = owned_by_venue(&orders_.at(alias->second))) return order;
        }
    }
    if (!report.exchange_order_id.empty()) {
        if (auto exchange = exchange_id_index_.find(report.exchange_order_id);
            exchange != exchange_id_index_.end()) {
            if (auto* order = owned_by_venue(&orders_.at(exchange->second))) return order;
        }
    }
    return nullptr;
}

} // namespace abex
