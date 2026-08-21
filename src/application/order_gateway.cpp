#include "abex/application/order_gateway.hpp"

#include "abex/domain/order_state_machine.hpp"
#include "abex/infrastructure/journal_serializer.hpp"

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
    std::string result;
    result.reserve(48);
    result += "gateway-";
    result += std::to_string(::getpid());
    result += '-';
    result += std::to_string(started_at_ms);
    return result;
}

[[nodiscard]] std::uint64_t stable_hash(std::string_view value) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char character : value) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ULL;
    }
    return hash;
}

[[nodiscard]] constexpr std::size_t venue_slot(Venue venue) noexcept {
    return venue == Venue::Okx ? 0U : 1U;
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
    std::string result;
    result.reserve(7 + code.size() + message.size());
    result += "ERROR";
    result += outcome_separator;
    result += code;
    result += outcome_separator;
    result += message;
    return result;
}

[[nodiscard]] std::string pending_outcome(std::string_view code, std::string_view message) {
    std::string result;
    result.reserve(9 + code.size() + message.size());
    result += "PENDING";
    result += outcome_separator;
    result += code;
    result += outcome_separator;
    result += message;
    return result;
}

[[nodiscard]] std::string place_rejection_event_id(std::string_view client_order_id,
                                                   std::uint64_t version) {
    std::string result;
    result.reserve(24 + client_order_id.size());
    result += "place-reject-";
    result += client_order_id;
    result += '-';
    result += std::to_string(version);
    return result;
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
        .side = order.side,
        .type = order.type,
        .price = order.price,
        .average_fill_price = order.average_fill_price,
        .quantity = order.quantity,
        .filled_quantity = order.filled_quantity,
        .status = order.status,
        .pending_action = order.pending_action,
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
      started_at_ms_(unix_time_ms()), instance_id_(gateway_instance_id(started_at_ms_)) {
    if (!order_store_) throw std::invalid_argument("order store is required");
    for (auto& adapter : adapters) {
        if (!adapter) throw std::invalid_argument("exchange adapter is null");
        if (!adapters_.emplace(adapter->venue(), adapter).second) {
            throw std::invalid_argument("duplicate adapter for " +
                                        std::string(to_string(adapter->venue())));
        }
        health_.try_emplace(adapter->venue());
    }
    if (!adapters_.contains(Venue::Okx) || !adapters_.contains(Venue::Binance)) {
        throw std::invalid_argument("both OKX and Binance adapters are required");
    }
    // Initialize atomic snapshot with zeroed positions.
    positions_snap_.store(std::make_shared<PositionSnapshot>(),
                          std::memory_order_release);
    {
        auto errors = std::make_shared<std::unordered_map<Venue, std::string>>();
        for (const auto& [v, _] : adapters_) errors->emplace(v, std::string{});
        health_errors_snap_.store(std::move(errors), std::memory_order_release);
    }
    for (const auto venue : {Venue::Okx, Venue::Binance}) {
        execution_lanes_[venue_slot(venue)] = std::make_unique<SpscExecutionLane>(
            options_.event_queue_capacity,
            [this, venue](const ExecutionReport& report) { apply_execution(venue, report); });
    }

    const auto prior_events = order_store_->load_events(200);
    previous_instance_present_ = std::ranges::any_of(prior_events, [](const auto& event) {
        return event.code == "GATEWAY_STARTED" || event.code == "GATEWAY_RESTARTED";
    });
    operational_events_.insert(operational_events_.end(), prior_events.begin(), prior_events.end());

    auto recovered = order_store_->load_latest();
    recovered_orders_ = recovered.size();
    for (auto& order : recovered) {
        auto key = order.client_order_id;
        orders_[key] = std::make_shared<Order>(std::move(order));
    }
    rebuild_indexes_locked(); // also publishes positions_snap_

    // P2: async journal writes — post() is the only call on the critical path.
    journal_lane_ = std::make_unique<AsyncJournalLane>(order_store_);

    // P3: async observer dispatch.
    order_observer_queue_ = std::make_unique<AsyncOrderObserverQueue>();

    // P1: per-venue TTL caches — critical path reads are lock-free atomic loads.
    for (const auto& [venue, adapter] : adapters_) {
        venue_caches_.emplace(venue, std::make_unique<VenueCache>(*adapter));
    }

    operational_event_writer_ = std::make_unique<OperationalEventWriter>(
        order_store_, [this](std::optional<OperationalEvent> event, std::string error) {
            complete_operational_event(std::move(event), std::move(error));
        });
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
    for (const auto& lane : execution_lanes_) lane->flush();
    journal_lane_->flush();
    order_observer_queue_->flush();
    record_event(OperationalSeverity::Info, "LIFECYCLE", "GATEWAY_STOPPED",
                 "Gateway stopped cleanly");
    operational_event_writer_->flush();
}

OperationResult OrderGateway::place(const OrderRequest& request) {
    // Defer fingerprint string construction until we know the clientOrderId
    // already exists (replay path). New orders compute it once inside the lock.
    std::string request_fingerprint;
    const auto get_fingerprint = [&]() -> const std::string& {
        if (request_fingerprint.empty()) request_fingerprint = fingerprint(request);
        return request_fingerprint;
    };
    const auto replay_if_present = [&]() -> std::optional<OperationResult> {
        const auto found = orders_.find(request.client_order_id);
        if (found == orders_.end()) return std::nullopt;
        const Order& o = *found->second;
        const auto& stored_fp = o.create_fingerprint;
        if (stored_fp == get_fingerprint() ||
            fingerprint_matches(stored_fp, request)) {
            if (o.status == OrderStatus::Rejected) {
                record_event(OperationalSeverity::Info, "RETRY", "IDEMPOTENT_REPLAY",
                             "Repeated create returned the persisted rejection",
                             request.venue, request.client_order_id, {},
                             order_event_context(o));
                return OperationResult{.ok = false,
                                       .idempotent_replay = true,
                                       .code = "ORDER_REJECTED",
                                       .message = o.rejection_reason,
                                       .order = o};
            }
            if (o.status == OrderStatus::Unknown &&
                o.pending_action == PendingAction::Reconcile) {
                record_event(OperationalSeverity::Warning, "RETRY", "IDEMPOTENT_REPLAY",
                             "Repeated create returned the persisted unknown outcome",
                             request.venue, request.client_order_id, {},
                             order_event_context(o));
                return OperationResult{.ok = false,
                                       .idempotent_replay = true,
                                       .code = "OUTCOME_UNKNOWN",
                                       .message = o.rejection_reason,
                                       .order = o};
            }
            record_event(OperationalSeverity::Info, "RETRY", "IDEMPOTENT_REPLAY",
                         "Repeated create returned the existing order without venue I/O",
                         request.venue, request.client_order_id, {},
                         order_event_context(o));
            return success(o, true);
        }
        record_event(OperationalSeverity::Warning, "RETRY", "IDEMPOTENCY_CONFLICT",
                     "clientOrderId was reused with a different order payload",
                     request.venue, request.client_order_id, {},
                     order_event_context(o));
        return failure("IDEMPOTENCY_CONFLICT",
                       "clientOrderId already exists with a different request",
                       o);
    };

    {
        std::scoped_lock lk(mutex_);
        if (auto replay = replay_if_present()) return *std::move(replay);
    }

    const auto current_market_price = market_data_
                                          ? market_data_->price(request.venue, request.symbol,
                                                                request.side)
                                          : std::nullopt;
    const auto adapter = adapter_for(request.venue);
    // P1: instrument rules and balance served from TTL cache — zero network I/O
    // on the critical path. warm() pre-populates the cache for new symbols.
    auto& cache = *venue_caches_.at(request.venue);
    cache.warm(request.symbol);
    const auto instrument_check = instrument_rules_decision(
        request, current_market_price, cache.instrument_rules(request.symbol));
    std::optional<RiskDecision> balance_check;
    if (instrument_check.accepted) {
        if (const auto requirement = funding_requirement(request, current_market_price)) {
            balance_check = funding_decision(
                request, *requirement,
                cache.balances(requirement->currency));
        }
    }

    std::shared_ptr<Order> outbound;
    std::optional<RiskDecision> local_rejection;
    std::tuple<std::shared_ptr<const Order>, bool, std::uint64_t> pp0;
    {
        const auto now_ms = unix_time_ms();
        std::scoped_lock lk(mutex_);
        {
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
            auto order = make_order(request, get_fingerprint(), now_ms);
            if (!decision.accepted) {
                order.status = OrderStatus::Rejected;
                order.pending_action = PendingAction::None;
                order.rejection_reason = decision.code + ": " + decision.reason;
                ++order.version;
                local_rejection = decision;
            } else {
                active_operations_.insert(order.client_order_id);
            }
            adjust_position_locked(order.symbol, Decimal{}, position_contribution(order));
            auto sp = std::make_shared<Order>(std::move(order));
            orders_[sp->client_order_id] = sp;
            outbound = std::move(sp);
        }
        publish_positions_if_dirty_locked();
        pp0 = prepare_persist(*outbound, !local_rejection.has_value());
    }
    commit_persist(std::get<0>(pp0), std::get<1>(pp0), std::get<2>(pp0));
    notify_order_observers(outbound);
    if (local_rejection) {
        record_event(OperationalSeverity::Warning, "RISK", "ORDER_REJECTED",
                     local_rejection->code + ": " + local_rejection->reason, request.venue,
                     request.client_order_id, {}, order_event_context(*outbound));
        return failure(local_rejection->code, local_rejection->reason, *outbound);
    }
    record_event2(OperationalSeverity::Info, "PERSISTENCE", "ORDER_INTENT_PERSISTED",
                  "New-order intent was durably journaled before venue I/O",
                  OperationalSeverity::Info, "PIPELINE", "ORDER_SENT_TO_EXCHANGE",
                  "Persisted new order was sent to the venue adapter",
                  request.venue, request.client_order_id, {}, order_event_context(*outbound));

    AdapterResult adapter_result;
    try {
        adapter_result = adapter->place(*outbound);
    } catch (const std::exception& error) {
        adapter_result = {.outcome_uncertain = true,
                          .code = "ADAPTER_EXCEPTION",
                          .message = error.what()};
    }

    OperationResult response;
    std::shared_ptr<Order> persisted;
    OperationalSeverity result_severity{OperationalSeverity::Info};
    std::string result_code;
    std::string result_message;
    std::tuple<std::shared_ptr<const Order>, bool, std::uint64_t> pp1;
    {
        const auto now_ms = unix_time_ms(); // one syscall for the entire lock section
        std::scoped_lock lk(mutex_);
        {
            auto& order = *orders_.at(request.client_order_id);
            const auto previous_position = position_contribution(order);
            active_operations_.erase(request.client_order_id);
            if (adapter_result.accepted) {
                if (!adapter_result.exchange_order_id.empty()) {
                    order.exchange_order_id = adapter_result.exchange_order_id;
                    exchange_id_index_[order.exchange_order_id] = order.client_order_id;
                }
                if (!adapter_result.exchange_client_order_id.empty()) {
                    order.exchange_client_id_aliases.insert(
                        adapter_result.exchange_client_order_id);
                    exchange_client_id_index_[adapter_result.exchange_client_order_id] =
                        order.client_order_id;
                }
                if (order.status == OrderStatus::Unknown) order.status = OrderStatus::Live;
                order.pending_action = PendingAction::None;
                order.updated_at_ms = now_ms;
                ++order.version;
                result_code = "ORDER_ACKNOWLEDGED";
                result_message = "Venue acknowledged the persisted new-order intent";
                persisted = orders_.at(request.client_order_id);
                response = success(*persisted);
            } else if (adapter_result.outcome_uncertain) {
                order.status = OrderStatus::Unknown;
                order.pending_action = PendingAction::Reconcile;
                order.rejection_reason = adapter_result.message;
                health_[order.venue].reconciliation_required.store(true, std::memory_order_relaxed);
                publish_health_error_locked(order.venue, adapter_result.message);
                ++order.version;
                result_severity = OperationalSeverity::Critical;
                result_code = "ORDER_OUTCOME_UNKNOWN";
                result_message = adapter_result.message;
                persisted = orders_.at(request.client_order_id);
                response = failure(
                    adapter_result.code.empty() ? "OUTCOME_UNKNOWN" : adapter_result.code,
                    adapter_result.message, *persisted);
            } else {
                ExecutionReport rejection{
                    .event_id = place_rejection_event_id(
                        order.client_order_id, order.version),
                    .client_order_id = order.client_order_id,
                    .exchange_order_id = adapter_result.exchange_order_id,
                    .status = OrderStatus::Rejected,
                    .cumulative_filled = order.filled_quantity,
                    .event_time_ms = now_ms,
                    .reason = adapter_result.code + ": " + adapter_result.message,
                };
                (void)OrderStateMachine::apply(order, rejection);
                result_severity = OperationalSeverity::Warning;
                result_code = "VENUE_ORDER_REJECTED";
                result_message = adapter_result.code + ": " + adapter_result.message;
                persisted = orders_.at(request.client_order_id);
                response = failure(adapter_result.code, adapter_result.message, *persisted);
            }
            adjust_position_locked(order.symbol, previous_position,
                                   position_contribution(order));
        }
        publish_positions_if_dirty_locked();
        pp1 = prepare_persist(*persisted);
    }
    commit_persist(std::get<0>(pp1), std::get<1>(pp1), std::get<2>(pp1));
    notify_order_observers(persisted);
    record_event(result_severity, "ORDER", result_code, result_message,
                 persisted->venue, persisted->client_order_id, {},
                 order_event_context(*persisted));

    // Synchronous venue queries are returned explicitly rather than injected into
    // the private-stream SPSC lane from an operation thread.
    for (const auto& report : adapter_result.authoritative_reports) {
        apply_execution(request.venue, report);
    }
    if (!adapter_result.authoritative_reports.empty()) {
        if (const auto current = get(request.client_order_id)) response.order = current;
    }
    return response;
}

OperationResult OrderGateway::cancel(CancelRequest request) {
    if (request.request_id.empty()) request.request_id = "cancel:" + request.client_order_id;
    std::shared_ptr<Order> outbound;
    const auto request_key = "CANCEL:" + request.request_id;
    const auto request_fingerprint = fingerprint(request);
    std::tuple<std::shared_ptr<const Order>, bool, std::uint64_t> pp;
    {
        const auto now_ms = unix_time_ms();
        std::scoped_lock lk(mutex_);
        {
            const auto found = orders_.find(request.client_order_id);
            if (found == orders_.end()) {
                record_event(OperationalSeverity::Warning, "REQUEST", "ORDER_NOT_FOUND",
                             "Cancel referenced an unknown clientOrderId", std::nullopt,
                             request.client_order_id, request.request_id);
                return failure("ORDER_NOT_FOUND", "order does not exist");
            }
            auto& order = *found->second;
            if (const auto replay = order.processed_requests.find(request_key);
                replay != order.processed_requests.end()) {
                if (replay->second != request_fingerprint &&
                    !fingerprint_matches(replay->second, request)) {
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
                             order.client_order_id, request.request_id,
                             order_event_context(order));
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
            order.updated_at_ms = now_ms;
            ++order.version;
            active_operations_.insert(order.client_order_id);
            outbound = found->second; // ref-count copy — no deep copy
        }
        publish_positions_if_dirty_locked();
        pp = prepare_persist(*outbound);
    }
    commit_persist(std::get<0>(pp), std::get<1>(pp), std::get<2>(pp));
    notify_order_observers(outbound);
    record_event2(OperationalSeverity::Info, "PERSISTENCE", "CANCEL_INTENT_PERSISTED",
                  "Cancel intent and requestId were durably journaled before venue I/O",
                  OperationalSeverity::Info, "PIPELINE", "CANCEL_SENT_TO_EXCHANGE",
                  "Persisted cancel request was sent to the venue adapter",
                  outbound->venue, outbound->client_order_id, request.request_id,
                  {});

    AdapterResult result;
    try {
        result = adapter_for(outbound->venue)->cancel(*outbound);
    } catch (const std::exception& error) {
        result = {.outcome_uncertain = true,
                  .code = "ADAPTER_EXCEPTION",
                  .message = error.what()};
    }

    OperationResult response;
    std::shared_ptr<Order> persisted;
    OperationalSeverity severity{OperationalSeverity::Info};
    std::string event_code;
    std::string event_message;
    std::tuple<std::shared_ptr<const Order>, bool, std::uint64_t> pp_ca;
    {
        std::scoped_lock lk(mutex_);
        {
            auto& order = *orders_.at(request.client_order_id);
            const auto previous_position = position_contribution(order);
            active_operations_.erase(request.client_order_id);
            if (result.accepted) {
                order.processed_request_outcomes[request_key] = "OK";
                order.rejection_reason.clear();
                ++order.version;
                event_code = "CANCEL_ACKNOWLEDGED";
                event_message = "Venue acknowledged the persisted cancel request";
                persisted = orders_.at(request.client_order_id);
                response = success(*persisted);
            } else {
                order.pending_action = result.outcome_uncertain ? PendingAction::Cancel
                                                                : PendingAction::None;
                if (result.outcome_uncertain) {
                    order.status = OrderStatus::Unknown;
                    health_[order.venue].reconciliation_required.store(true, std::memory_order_relaxed);
                }
                order.rejection_reason = result.message;
                order.processed_request_outcomes[request_key] =
                    result.outcome_uncertain ? pending_outcome(result.code, result.message)
                                             : failed_outcome(result.code, result.message);
                ++order.version;
                severity = result.outcome_uncertain ? OperationalSeverity::Critical
                                                    : OperationalSeverity::Warning;
                event_code = result.outcome_uncertain ? "CANCEL_OUTCOME_UNKNOWN"
                                                      : "CANCEL_REJECTED";
                event_message = result.message;
                persisted = orders_.at(request.client_order_id);
                response = failure(result.code, result.message, *persisted);
            }
            adjust_position_locked(order.symbol, previous_position,
                                   position_contribution(order));
        }
        publish_positions_if_dirty_locked();
        pp = prepare_persist(*persisted);
    }
    commit_persist(std::get<0>(pp), std::get<1>(pp), std::get<2>(pp));
    notify_order_observers(persisted);
    record_event(severity, "ORDER", event_code, event_message,
                 persisted->venue, persisted->client_order_id, request.request_id,
                 order_event_context(*persisted));
    return response;
}

OperationResult OrderGateway::amend(AmendRequest request) {
    if (request.request_id.empty()) {
        request.request_id = "amend:" +
                             std::to_string(stable_hash(fingerprint(request)));
    }
    std::shared_ptr<Order> outbound;
    const auto request_key = "AMEND:" + request.request_id;
    const auto request_fingerprint = fingerprint(request);
    std::tuple<std::shared_ptr<const Order>, bool, std::uint64_t> pp;
    {
        const auto now_ms = unix_time_ms();
        std::scoped_lock lk(mutex_);
        {
            const auto found = orders_.find(request.client_order_id);
            if (found == orders_.end()) {
                record_event(OperationalSeverity::Warning, "REQUEST", "ORDER_NOT_FOUND",
                             "Amend referenced an unknown clientOrderId", std::nullopt,
                             request.client_order_id, request.request_id);
                return failure("ORDER_NOT_FOUND", "order does not exist");
            }
            auto& order = *found->second;
            if (const auto replay = order.processed_requests.find(request_key);
                replay != order.processed_requests.end()) {
                if (replay->second != request_fingerprint &&
                    !fingerprint_matches(replay->second, request)) {
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
                             order.client_order_id, request.request_id,
                             order_event_context(order));
                return failure(decision.code, decision.reason, order);
            }

            order.processed_requests[request_key] = request_fingerprint;
            order.processed_request_outcomes[request_key] = "PENDING";
            order.pending_action = PendingAction::Amend;
            order.pending_amend_price = request.new_price;
            order.pending_amend_quantity = request.new_quantity;
            order.updated_at_ms = now_ms;
            ++order.version;
            active_operations_.insert(order.client_order_id);
            outbound = found->second; // ref-count copy
        }
        publish_positions_if_dirty_locked();
        pp = prepare_persist(*outbound);
    }
    commit_persist(std::get<0>(pp), std::get<1>(pp), std::get<2>(pp));
    notify_order_observers(outbound);
    record_event2(OperationalSeverity::Info, "PERSISTENCE", "AMEND_INTENT_PERSISTED",
                  "Amend intent and requestId were durably journaled before venue I/O",
                  OperationalSeverity::Info, "PIPELINE", "AMEND_SENT_TO_EXCHANGE",
                  "Persisted amend request was sent to the venue adapter",
                  outbound->venue, outbound->client_order_id, request.request_id,
                  {});

    AdapterResult result;
    try {
        result = adapter_for(outbound->venue)->amend(
            *outbound, request.new_price, request.new_quantity);
    } catch (const std::exception& error) {
        result = {.outcome_uncertain = true,
                  .code = "ADAPTER_EXCEPTION",
                  .message = error.what()};
    }

    OperationResult response;
    std::vector<ExecutionReport> reports_to_apply = result.authoritative_reports;
    std::shared_ptr<Order> persisted;
    std::string replacement_warning_to_log;
    OperationalSeverity result_severity{OperationalSeverity::Info};
    std::string result_event_code;
    std::string result_event_message;
    std::tuple<std::shared_ptr<const Order>, bool, std::uint64_t> pp_aa;
    {
        std::scoped_lock lk(mutex_);
        {
            auto& order = *orders_.at(request.client_order_id);
            const auto previous_position = position_contribution(order);
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
                            request.new_quantity.value_or(outbound->quantity);
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
            replacement_warning_to_log = replacement_warning;
            result_event_code = "AMEND_ACKNOWLEDGED";
            result_event_message =
                result.replacement
                    ? "Venue accepted cancel-replace; authoritative generation reports are being merged"
                    : "Venue accepted the amend request; final terms await the order stream or query";
            response = success(order);
        } else {
            order.pending_action = result.outcome_uncertain ? PendingAction::Amend
                                                            : PendingAction::None;
            if (result.outcome_uncertain) {
                order.status = OrderStatus::Unknown;
                health_[order.venue].reconciliation_required.store(true, std::memory_order_relaxed);
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
            result_severity = result.outcome_uncertain ? OperationalSeverity::Critical
                                                       : OperationalSeverity::Warning;
            result_event_code = result.outcome_uncertain ? "AMEND_OUTCOME_UNKNOWN"
                                                         : "AMEND_REJECTED";
            result_event_message = result.message;
            response = failure(result.code, result.message, order);
        }

            if (auto deferred = deferred_amend_reports_.find(order.client_order_id);
            deferred != deferred_amend_reports_.end()) {
            reports_to_apply.insert(reports_to_apply.end(),
                                    std::make_move_iterator(deferred->second.begin()),
                                    std::make_move_iterator(deferred->second.end()));
                deferred_amend_reports_.erase(deferred);
            }
            adjust_position_locked(order.symbol, previous_position,
                                   position_contribution(order));
            persisted = orders_.at(request.client_order_id);
        }
        publish_positions_if_dirty_locked();
        pp = prepare_persist(*persisted);
    }
    commit_persist(std::get<0>(pp), std::get<1>(pp), std::get<2>(pp));
    notify_order_observers(persisted);

    if (!replacement_warning_to_log.empty()) {
        record_event(OperationalSeverity::Critical, "ORDER", "REPLACEMENT_QUANTITY_DRIFT",
                     replacement_warning_to_log, persisted->venue,
                     persisted->client_order_id, request.request_id,
                     order_event_context(*persisted));
    }
    record_event(result_severity, "ORDER", result_event_code,
                 result_event_message, persisted->venue,
                 persisted->client_order_id, request.request_id,
                 order_event_context(*persisted));

    for (const auto& report : reports_to_apply) apply_execution(outbound->venue, report);
    if (const auto current = get(request.client_order_id)) response.order = current;
    return response;
}

OperationResult OrderGateway::reconcile(Venue venue) {
    record_event(OperationalSeverity::Info, "RECONCILIATION", "RECONCILIATION_STARTED",
                 "Venue reconciliation started", venue);
    const auto adapter = adapter_for(venue);
    std::vector<std::string> candidate_ids;
    {
        std::scoped_lock lk(mutex_);
        candidate_ids.reserve(orders_.size());
        for (const auto& [client_order_id, sp] : orders_) {
            if (sp->venue == venue) candidate_ids.push_back(client_order_id);
        }
    }
    std::size_t reconciled = 0;
    std::size_t unresolved = 0;
    std::unordered_set<std::string> observed_open_orders;

    try {
        if (auto open_reports = adapter->query_open_orders()) {
            for (auto& report : *open_reports) {
                std::string canonical_client_id;
                bool terminal_conflict = false;
                {
                    std::scoped_lock lk(mutex_);
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
        { std::scoped_lock lk(mutex_); publish_health_error_locked(venue, error.what()); }
    }

    for (const auto& client_order_id : candidate_ids) {
        if (observed_open_orders.contains(client_order_id)) continue;
        Order query_order;
        {
            std::scoped_lock lk(mutex_);
            const auto found = orders_.find(client_order_id);
            if (found == orders_.end() || active_operations_.contains(client_order_id)) continue;
            query_order = *found->second;
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
                std::shared_ptr<Order> persisted;
                std::tuple<std::shared_ptr<const Order>, bool, std::uint64_t> persist6;
                {
                    std::scoped_lock lk(mutex_);
                    auto& current = *orders_.at(client_order_id);
                    current.status = OrderStatus::Unknown;
                    current.pending_action = PendingAction::Reconcile;
                    ++current.version;
                    persisted = orders_.at(client_order_id);
                    persist6 = prepare_persist(*persisted);
                }
                commit_persist(std::get<0>(persist6), std::get<1>(persist6), std::get<2>(persist6));
                notify_order_observers(persisted);
                ++unresolved;
            }
        } catch (const std::exception& error) {
            { std::scoped_lock lk(mutex_); publish_health_error_locked(venue, error.what()); }
            ++unresolved;
        }
    }
    health_[venue].reconciliation_required.store(unresolved != 0, std::memory_order_relaxed);
    if (unresolved == 0) {
        std::scoped_lock lk(mutex_);
        sequence_trackers_[venue].clear_gap();
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
    std::scoped_lock lk(mutex_);
    const auto found = orders_.find(client_order_id);
    return found == orders_.end() ? std::nullopt : std::optional(*found->second);
}

std::optional<OrderSnapshot>
OrderGateway::get_snapshot(std::string_view client_order_id) const {
    std::scoped_lock lk(mutex_);
    const auto found = orders_.find(client_order_id);
    if (found == orders_.end()) return std::nullopt;
    const auto& order = *found->second;
    return OrderSnapshot{
        .client_order_id = order.client_order_id,
        .exchange_order_id = order.exchange_order_id,
        .venue = order.venue,
        .symbol = order.symbol,
        .side = order.side,
        .type = order.type,
        .price = order.price,
        .quantity = order.quantity,
        .time_in_force = order.time_in_force,
        .status = order.status,
        .pending_action = order.pending_action,
        .pending_amend_price = order.pending_amend_price,
        .pending_amend_quantity = order.pending_amend_quantity,
        .filled_quantity = order.filled_quantity,
        .average_fill_price = order.average_fill_price,
        .rejection_reason = order.rejection_reason,
        .version = order.version,
        .last_sequence = order.last_sequence,
        .created_at_ms = order.created_at_ms,
        .updated_at_ms = order.updated_at_ms,
    };
}

std::vector<Order> OrderGateway::list(std::optional<Venue> venue,
                                      std::optional<OrderStatus> status) const {
    std::scoped_lock lk(mutex_);
    std::vector<Order> result;
    for (const auto& [id, sp] : orders_) {
        (void)id;
        if (venue && sp->venue != *venue) continue;
        if (status && sp->status != *status) continue;
        result.push_back(*sp);
    }
    std::ranges::sort(result, [](const Order& lhs, const Order& rhs) {
        if (lhs.created_at_ms != rhs.created_at_ms) return lhs.created_at_ms < rhs.created_at_ms;
        return lhs.client_order_id < rhs.client_order_id;
    });
    return result;
}

std::vector<OrderSnapshot>
OrderGateway::list_snapshots(std::optional<Venue> venue,
                             std::optional<OrderStatus> status) const {
    std::scoped_lock lk(mutex_);
    std::vector<OrderSnapshot> result;
    result.reserve(orders_.size());
    for (const auto& [id, sp] : orders_) {
        (void)id;
        if (venue && sp->venue != *venue) continue;
        if (status && sp->status != *status) continue;
        result.push_back(OrderSnapshot{
            .client_order_id = sp->client_order_id,
            .exchange_order_id = sp->exchange_order_id,
            .venue = sp->venue,
            .symbol = sp->symbol,
            .side = sp->side,
            .type = sp->type,
            .price = sp->price,
            .quantity = sp->quantity,
            .time_in_force = sp->time_in_force,
            .status = sp->status,
            .pending_action = sp->pending_action,
            .pending_amend_price = sp->pending_amend_price,
            .pending_amend_quantity = sp->pending_amend_quantity,
            .filled_quantity = sp->filled_quantity,
            .average_fill_price = sp->average_fill_price,
            .rejection_reason = sp->rejection_reason,
            .version = sp->version,
            .last_sequence = sp->last_sequence,
            .created_at_ms = sp->created_at_ms,
            .updated_at_ms = sp->updated_at_ms,
        });
    }
    std::ranges::sort(result, [](const OrderSnapshot& lhs, const OrderSnapshot& rhs) {
        if (lhs.created_at_ms != rhs.created_at_ms) return lhs.created_at_ms < rhs.created_at_ms;
        return lhs.client_order_id < rhs.client_order_id;
    });
    return result;
}

std::shared_ptr<const OrderGateway::PositionSnapshot> OrderGateway::positions() const {
    // One atomic load — zero copy, zero allocation, no mutex.
    return positions_snap_.load(std::memory_order_acquire);
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
    // Lock-free: one atomic load for the error strings, then atomic reads of
    // the per-venue health fields. No mutex_ acquisition needed.
    const auto errors = health_errors_snap_.load(std::memory_order_acquire);
    std::unordered_map<Venue, VenueHealth> result;
    for (const auto& [venue, h] : health_) {
        std::string last_error;
        if (errors) {
            if (const auto it = errors->find(venue); it != errors->end())
                last_error = it->second;
        }
        result[venue] = {
            .connected = h.connected.load(std::memory_order_relaxed),
            .ever_connected = h.ever_connected.load(std::memory_order_relaxed),
            .reconciliation_required = h.reconciliation_required.load(std::memory_order_relaxed),
            .sequence_gaps = h.sequence_gaps.load(std::memory_order_relaxed),
            .dropped_events = h.dropped_events.load(std::memory_order_relaxed),
            .last_error = std::move(last_error),
        };
    }
    return result;
}

GatewayStability OrderGateway::stability() const {
    operational_event_writer_->flush();
    GatewayStability result;
    result.instance_id = instance_id_;
    result.started_at_ms = started_at_ms_;
    result.recovered_orders = recovered_orders_;
    result.idempotent_replays = idempotent_replays_.load(std::memory_order_relaxed);
    result.reconciliations = reconciliations_.load(std::memory_order_relaxed);
    result.alerts = alerts_.load(std::memory_order_relaxed);
    result.logging_failures = logging_failures_.load(std::memory_order_relaxed);
    {
        std::scoped_lock lock(logging_error_mutex_);
        result.last_logging_error = last_logging_error_;
    }
    result.journal = order_store_->status();
    return result;
}

std::vector<OperationalEvent> OrderGateway::operational_events(std::size_t limit) const {
    operational_event_writer_->flush();
    std::scoped_lock lock(operational_mutex_);
    const auto count = std::min(limit, operational_events_.size());
    return {operational_events_.end() - static_cast<std::ptrdiff_t>(count),
            operational_events_.end()};
}

std::vector<OperationalEvent>
OrderGateway::order_events(std::string_view client_order_id, std::size_t limit) const {
    operational_event_writer_->flush();
    return order_store_->load_order_events(client_order_id, limit);
}

OrderGateway::ObserverToken OrderGateway::add_order_observer(OrderObserver observer) {
    return order_observer_queue_->add(std::move(observer));
}

void OrderGateway::remove_order_observer(ObserverToken token) noexcept {
    order_observer_queue_->remove(token);
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

void OrderGateway::flush_events() {
    for (const auto& lane : execution_lanes_) lane->flush();
    journal_lane_->flush();
    order_observer_queue_->flush();
    operational_event_writer_->flush();
}

std::shared_ptr<IExchangeAdapter> OrderGateway::adapter_for(Venue venue) const {
    const auto found = adapters_.find(venue);
    if (found == adapters_.end()) throw std::logic_error("venue adapter is unavailable");
    return found->second;
}

Decimal OrderGateway::conservative_position_locked(
    std::string_view symbol,
    std::string_view excluded_client_order_id) const {
    // Caller already holds mutex_. Compile-time slot lookup — no heap, no hash.
    const auto id = symbol_id_rt(symbol);
    auto result = id != SymbolId::Unknown ? conservative_positions_[to_slot(id)] : Decimal{};
    if (!excluded_client_order_id.empty()) {
        const auto excluded = orders_.find(excluded_client_order_id);
        if (excluded != orders_.end() && excluded->second->symbol == symbol)
            result -= position_contribution(*excluded->second);
    }
    return result;
}

Decimal OrderGateway::position_contribution(const Order& order) {
    const auto exposure = is_terminal(order.status) ? order.filled_quantity : order.quantity;
    return order.side == Side::Buy ? exposure : -exposure;
}

void OrderGateway::adjust_position_locked(std::string_view symbol,
                                          Decimal previous,
                                          Decimal current) {
    const auto id = symbol_id_rt(symbol);
    if (id != SymbolId::Unknown)
        conservative_positions_[to_slot(id)] += current - previous;
    mark_positions_dirty_locked();
}

void OrderGateway::rebuild_indexes_locked() {
    exchange_id_index_.clear();
    exchange_client_id_index_.clear();
    conservative_positions_.fill(Decimal{});
    for (const auto& [client_id, sp] : orders_) {
        if (!sp->exchange_order_id.empty())
            exchange_id_index_[sp->exchange_order_id] = client_id;
        for (const auto& alias : sp->exchange_order_id_aliases)
            exchange_id_index_[alias] = client_id;
        exchange_client_id_index_[client_id] = client_id;
        for (const auto& alias : sp->exchange_client_id_aliases)
            exchange_client_id_index_[alias] = client_id;
        const auto id = symbol_id_rt(sp->symbol);
        if (id != SymbolId::Unknown)
            conservative_positions_[to_slot(id)] += position_contribution(*sp);
    }
    positions_dirty_ = true;
    publish_positions_if_dirty_locked();
}

void OrderGateway::publish_positions_if_dirty_locked() {
    if (!positions_dirty_) return;
    positions_dirty_ = false;
    auto snap = std::make_shared<PositionSnapshot>();
    snap->values = conservative_positions_;
    positions_snap_.store(std::move(snap), std::memory_order_release);
}

void OrderGateway::publish_health_error_locked(Venue venue, std::string error) {
    // Caller holds mutex_. Build a new error map with the updated entry and
    // publish it atomically. health() reads it with one atomic load.
    const auto current = health_errors_snap_.load(std::memory_order_acquire);
    auto updated = current ? std::make_shared<std::unordered_map<Venue, std::string>>(*current)
                           : std::make_shared<std::unordered_map<Venue, std::string>>();
    (*updated)[venue] = std::move(error);
    health_errors_snap_.store(std::move(updated), std::memory_order_release);
}

void OrderGateway::persist_order(const Order& order, bool intent_only) {
    order_store_->append_order(order, intent_only);
}

std::tuple<std::shared_ptr<const Order>, bool, std::uint64_t>
OrderGateway::prepare_persist(const Order& order, bool intent_only) {
    // Inside mutex_: snapshot the Order and reserve a sequence number.
    return {std::make_shared<const Order>(order),
            intent_only,
            order_store_->reserve_sequence()};
}

void OrderGateway::commit_persist(std::shared_ptr<const Order> snapshot,
                                   bool intent_only,
                                   std::uint64_t sequence) {
    if (!journal_lane_->post(std::move(snapshot), intent_only, sequence)) {
        // Lane full: serialize synchronously.
        std::string fallback;
        fallback.reserve(512);
        JsonSerializer::write_order(fallback, *snapshot, intent_only);
        order_store_->commit_order(*snapshot, std::move(fallback), sequence);
    }
}

void OrderGateway::notify_order_observers(std::shared_ptr<Order> order) noexcept {
    (void)order_observer_queue_->post(std::move(order));
}

void OrderGateway::record_event(OperationalSeverity severity,
                                std::string_view category,
                                std::string_view code,
                                std::string message,
                                std::optional<Venue> venue,
                                std::string_view client_order_id,
                                std::string_view request_id,
                                std::optional<OrderEventContext> order) noexcept {
    if (!operational_event_writer_->submit(
            unix_time_ms(), severity, category, code, std::move(message),
            instance_id_, venue, client_order_id, request_id, std::move(order))) {
        logging_failures_.fetch_add(1, std::memory_order_relaxed);
        std::scoped_lock lock(logging_error_mutex_);
        last_logging_error_ = "operational event queue is full";
    }
}

void OrderGateway::record_event2(OperationalSeverity sev_a, std::string_view cat_a,
                                  std::string_view code_a, std::string msg_a,
                                  OperationalSeverity sev_b, std::string_view cat_b,
                                  std::string_view code_b, std::string msg_b,
                                  std::optional<Venue> venue,
                                  std::string_view client_order_id,
                                  std::string_view request_id,
                                  std::optional<OrderEventContext> order) noexcept {
    if (!operational_event_writer_->submit2(
            unix_time_ms(),
            sev_a, cat_a, code_a, std::move(msg_a),
            sev_b, cat_b, code_b, std::move(msg_b),
            instance_id_, venue, client_order_id, request_id, std::move(order))) {
        logging_failures_.fetch_add(1, std::memory_order_relaxed);
        std::scoped_lock lock(logging_error_mutex_);
        last_logging_error_ = "operational event queue is full";
    }
}

void OrderGateway::complete_operational_event(std::optional<OperationalEvent> event,
                                              std::string error) noexcept {
    if (!event) {
        logging_failures_.fetch_add(1, std::memory_order_relaxed);
        std::scoped_lock lock(logging_error_mutex_);
        last_logging_error_ = std::move(error);
        return;
    }
    std::vector<OperationalObserver> observers;
    {
        std::scoped_lock lock(operational_mutex_);
        operational_events_.push_back(*event);
        if (operational_events_.size() > 200)
            operational_events_.erase(operational_events_.begin());
        observers.reserve(operational_observers_.size());
        for (const auto& [token, observer] : operational_observers_) {
            (void)token;
            observers.push_back(observer);
        }
    }
    if (event->code == "IDEMPOTENT_REPLAY") idempotent_replays_.fetch_add(1, std::memory_order_relaxed);
    if (event->code == "RECONCILIATION_STARTED") reconciliations_.fetch_add(1, std::memory_order_relaxed);
    if (event->severity != OperationalSeverity::Info) alerts_.fetch_add(1, std::memory_order_relaxed);
    for (const auto& observer : observers) {
        try {
            observer(*event);
        } catch (...) {
            // Operational observers cannot alter order processing.
        }
    }
}

void OrderGateway::receive_execution(Venue venue, ExecutionReport report) {
    auto& lane = *execution_lanes_[venue_slot(venue)];
    if (!lane.submit(std::move(report), options_.event_submit_timeout)) {
        ++health_[venue].dropped_events;
        health_[venue].reconciliation_required.store(true, std::memory_order_relaxed);
        const auto drop_error = lane.producer_violations() != 0
                                    ? "adapter violated its serialized execution-callback contract"
                                    : "venue execution lane remained full";
        { std::scoped_lock lk(mutex_); publish_health_error_locked(venue, drop_error); }
        record_event(OperationalSeverity::Critical, "BACKPRESSURE", "EXECUTION_EVENT_DROPPED",
                     drop_error, venue);
    }
}

void OrderGateway::apply_execution(Venue venue, const ExecutionReport& report) {
    std::shared_ptr<Order> persisted;
    bool should_persist = false;
    std::string event_code;
    std::string event_message;
    OperationalSeverity event_severity{OperationalSeverity::Info};
    std::tuple<std::shared_ptr<const Order>, bool, std::uint64_t> pp;
    {
        std::scoped_lock lk(mutex_);
        auto* order = locate_order_locked(venue, report);
        if (!order) return;

        if (report.sequence) {
            const auto observed = sequence_trackers_[venue].observe(*report.sequence);
            if (observed.observation == SequenceObservation::Gap) {
                ++health_[venue].sequence_gaps;
                health_[venue].reconciliation_required.store(true, std::memory_order_relaxed);
                const auto gap_error = "execution sequence gap: expected " +
                                       std::to_string(*observed.expected) + ", received " +
                                       std::to_string(observed.received);
                publish_health_error_locked(venue, gap_error);
                record_event(OperationalSeverity::Critical, "SEQUENCING",
                             "EXECUTION_SEQUENCE_GAP", gap_error, venue,
                             report.client_order_id);
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
        const auto previous_position = position_contribution(*order);
        const auto result = OrderStateMachine::apply(*order, report);
        if (result.disposition == ApplyDisposition::Invalid) {
            health_[venue].reconciliation_required.store(true, std::memory_order_relaxed);
            publish_health_error_locked(venue, std::string(result.reason));
            record_event(OperationalSeverity::Critical, "ORDER",
                         "INVALID_EXECUTION_TRANSITION", std::string(result.reason), venue,
                         order->client_order_id);
            return;
        }
        if (!order->exchange_order_id.empty()) {
            exchange_id_index_[order->exchange_order_id] = order->client_order_id;
        }
        if (!report.client_order_id.empty() && report.client_order_id != order->client_order_id) {
            order->exchange_client_id_aliases.insert(report.client_order_id);
            exchange_client_id_index_[report.client_order_id] = order->client_order_id;
        }
        adjust_position_locked(order->symbol, previous_position,
                               position_contribution(*order));
        should_persist = result.disposition != ApplyDisposition::Duplicate &&
                         (result.state_changed || !report.event_id.empty());
        if (!should_persist) return;

        event_code = "ORDER_EXECUTION_UPDATED";
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
        if (report.reason.empty()) {
            event_message.reserve(96);
            event_message += "Venue execution update: ";
            event_message += to_string(order->status);
            event_message += ", filled ";
            order->filled_quantity.append_to(event_message);
            event_message += '/';
            order->quantity.append_to(event_message);
        } else {
            event_message = report.reason;
        }
        event_severity = order->status == OrderStatus::Rejected
                             ? OperationalSeverity::Warning
                             : OperationalSeverity::Info;
        persisted = orders_.at(order->client_order_id);
        publish_positions_if_dirty_locked();
        pp = prepare_persist(*persisted);
    } // mutex_ released here
    commit_persist(std::get<0>(pp), std::get<1>(pp), std::get<2>(pp));
    notify_order_observers(persisted);
    record_event(event_severity, "PIPELINE", event_code,
                 event_message, venue, persisted->client_order_id, {},
                 order_event_context(*persisted, report.sequence, report.event_time_ms));
}

void OrderGateway::connection_changed(Venue venue, bool connected, std::string reason) {
    auto& state = health_[venue];
    const bool was_connected = state.connected.load(std::memory_order_relaxed);
    const bool reconnect = connected && !was_connected &&
                           state.ever_connected.load(std::memory_order_relaxed);
    state.connected.store(connected, std::memory_order_relaxed);
    if (!connected || reconnect)
        state.reconciliation_required.store(true, std::memory_order_relaxed);
    if (connected) {
        state.ever_connected.store(true, std::memory_order_relaxed);
        { std::scoped_lock lk(mutex_); publish_health_error_locked(venue, {}); }
        record_event(reconnect ? OperationalSeverity::Warning : OperationalSeverity::Info,
                     "CONNECTIVITY", reconnect ? "VENUE_RECONNECTED" : "VENUE_CONNECTED",
                     reconnect ? "Venue connection recovered; reconciliation was scheduled"
                               : "Venue connection established",
                     venue);
    } else {
        const auto cur_errors = health_errors_snap_.load(std::memory_order_acquire);
        const bool no_error = !cur_errors || !cur_errors->count(venue) ||
                              cur_errors->at(venue).empty();
        if (was_connected || no_error) {
            const auto msg = reason.empty() ? "venue connection disconnected" : std::move(reason);
            { std::scoped_lock lk(mutex_); publish_health_error_locked(venue, msg); }
            record_event(started_.load() ? OperationalSeverity::Critical : OperationalSeverity::Info,
                         "CONNECTIVITY",
                         started_.load() ? "VENUE_DISCONNECTED" : "VENUE_STOPPED",
                         msg, venue);
        }
    }
    if (reconnect && options_.reconcile_on_reconnect) schedule_reconciliation(venue);
}

void OrderGateway::start_reconciliation_worker() {
    std::scoped_lock lock(reconciliation_mutex_);
    if (reconciliation_worker_.joinable()) return;
    reconciliation_pending_.store(0, std::memory_order_release);
    reconciliation_worker_ =
        std::jthread([this](std::stop_token token) { run_reconciliation_worker(token); });
}

void OrderGateway::stop_reconciliation_worker() noexcept {
    if (!reconciliation_worker_.joinable()) return;
    reconciliation_worker_.request_stop();
    reconciliation_condition_.notify_all();
    reconciliation_worker_.join();
    reconciliation_pending_.store(0, std::memory_order_release);
}

void OrderGateway::schedule_reconciliation(Venue venue) {
    const auto bit = venue == Venue::Okx ? std::uint8_t{1} : std::uint8_t{2};
    reconciliation_pending_.fetch_or(bit, std::memory_order_release);
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
                    [this] { return reconciliation_pending_.load(std::memory_order_acquire) != 0; });
            } else {
                reconciliation_condition_.wait(
                    lock, token, [this] {
                        return reconciliation_pending_.load(std::memory_order_acquire) != 0;
                    });
            }
            if (token.stop_requested()) return;
            if (reconciliation_pending_.load(std::memory_order_acquire) == 0 &&
                options_.reconciliation_interval > std::chrono::milliseconds::zero() &&
                std::chrono::steady_clock::now() >= next_periodic) {
                reconciliation_pending_.fetch_or(std::uint8_t{3}, std::memory_order_release);
                next_periodic = std::chrono::steady_clock::now() +
                                options_.reconciliation_interval;
            }
            const auto pending = reconciliation_pending_.load(std::memory_order_acquire);
            if (pending == 0) continue;
            const auto bit = (pending & std::uint8_t{1}) != 0 ? std::uint8_t{1}
                                                              : std::uint8_t{2};
            venue = bit == 1 ? Venue::Okx : Venue::Binance;
            reconciliation_pending_.fetch_and(static_cast<std::uint8_t>(~bit),
                                              std::memory_order_acq_rel);
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
            if (auto* order = owned_by_venue(direct->second.get())) return order;
        }
        if (auto alias = exchange_client_id_index_.find(report.client_order_id);
            alias != exchange_client_id_index_.end()) {
            if (auto* order = owned_by_venue(orders_.at(alias->second).get())) return order;
        }
    }
    if (!report.exchange_order_id.empty()) {
        if (auto exchange = exchange_id_index_.find(report.exchange_order_id);
            exchange != exchange_id_index_.end()) {
            if (auto* order = owned_by_venue(orders_.at(exchange->second).get())) return order;
        }
    }
    return nullptr;
}

} // namespace abex
