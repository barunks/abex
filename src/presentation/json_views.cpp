#include "abex/presentation/json_views.hpp"

#include <algorithm>

namespace abex {

namespace {

template <typename OrderLike>
nlohmann::json order_view_impl(const OrderLike& order) {
    nlohmann::json json{
        {"clientOrderId", order.client_order_id},
        {"exchangeOrderId", order.exchange_order_id},
        {"venue", to_string(order.venue)},
        {"symbol", order.symbol},
        {"side", to_string(order.side)},
        {"type", to_string(order.type)},
        {"quantity", order.quantity.to_string()},
        {"timeInForce", to_string(order.time_in_force)},
        {"status", to_string(order.status)},
        {"pendingAction", to_string(order.pending_action)},
        {"filledQuantity", order.filled_quantity.to_string()},
        {"rejectionReason", order.rejection_reason},
        {"version", order.version},
        {"createdAt", order.created_at_ms},
        {"updatedAt", order.updated_at_ms},
    };
    if (order.price) json["price"] = order.price->to_string();
    if (order.pending_amend_price) {
        json["pendingAmendPrice"] = order.pending_amend_price->to_string();
    }
    if (order.pending_amend_quantity) {
        json["pendingAmendQuantity"] = order.pending_amend_quantity->to_string();
    }
    if (order.average_fill_price) {
        json["averageFillPrice"] = order.average_fill_price->to_string();
    }
    if (order.last_sequence) json["lastSequence"] = *order.last_sequence;
    return json;
}

} // namespace

nlohmann::json order_view(const Order& order) { return order_view_impl(order); }

nlohmann::json order_view(const OrderSnapshot& order) { return order_view_impl(order); }

nlohmann::json operation_view(const OperationResult& result) {
    nlohmann::json json{
        {"ok", result.ok},
        {"idempotentReplay", result.idempotent_replay},
    };
    if (!result.code.empty()) json["code"] = result.code;
    if (!result.message.empty()) json["message"] = result.message;
    if (result.order) json["order"] = order_view(*result.order);
    return json;
}

nlohmann::json positions_view(const std::unordered_map<std::string, Decimal>& positions) {
    auto values = nlohmann::json::object();
    for (const auto& [symbol, position] : positions) {
        values[symbol] = position.to_string();
    }
    return values;
}

nlohmann::json health_view(const std::unordered_map<Venue, VenueHealth>& health) {
    auto venues = nlohmann::json::object();
    for (const auto& [venue, state] : health) {
        venues[to_string(venue)] = {
            {"connected", state.connected},
            {"everConnected", state.ever_connected},
            {"reconciliationRequired", state.reconciliation_required},
            {"sequenceGaps", state.sequence_gaps},
            {"droppedEvents", state.dropped_events},
            {"lastError", state.last_error},
        };
    }
    return venues;
}

nlohmann::json balance_view(const BalanceQueryResult& result) {
    nlohmann::json json{
        {"ok", result.ok},
        {"venue", to_string(result.snapshot.venue)},
        {"accountId", result.snapshot.account_id},
        {"mainAccountId", result.snapshot.main_account_id},
        {"observedAt", result.snapshot.observed_at_ms},
    };
    auto balances = nlohmann::json::array();
    for (const auto& balance : result.snapshot.balances) {
        balances.push_back({
            {"currency", balance.currency},
            {"total", balance.total},
            {"available", balance.available},
            {"frozen", balance.frozen},
            {"orderFrozen", balance.order_frozen},
        });
    }
    json["balances"] = std::move(balances);
    if (!result.code.empty()) json["code"] = result.code;
    if (!result.message.empty()) json["message"] = result.message;
    return json;
}

nlohmann::json instrument_rules_view(const InstrumentRulesQueryResult& result) {
    const auto& rules = result.rules;
    nlohmann::json json{
        {"ok", result.ok},
        {"venue", to_string(rules.venue)},
        {"symbol", rules.symbol},
        {"status", rules.status},
        {"trading", rules.trading},
        {"observedAt", rules.observed_at_ms},
    };
    const auto add = [&](std::string_view key, const std::optional<Decimal>& value) {
        if (value) json[std::string(key)] = value->to_string();
    };
    add("minimumPrice", rules.minimum_price);
    add("maximumPrice", rules.maximum_price);
    add("priceTick", rules.price_tick);
    add("minimumQuantity", rules.minimum_quantity);
    add("maximumQuantity", rules.maximum_quantity);
    add("quantityStep", rules.quantity_step);
    add("marketMinimumQuantity", rules.market_minimum_quantity);
    add("marketMaximumQuantity", rules.market_maximum_quantity);
    add("marketQuantityStep", rules.market_quantity_step);
    add("minimumNotional", rules.minimum_notional);
    add("maximumNotional", rules.maximum_notional);
    add("marketMinimumNotional", rules.market_minimum_notional);
    add("marketMaximumNotional", rules.market_maximum_notional);
    if (!result.code.empty()) json["code"] = result.code;
    if (!result.message.empty()) json["message"] = result.message;
    return json;
}

nlohmann::json operational_event_view(const OperationalEvent& event) {
    nlohmann::json json{
        {"sequence", event.sequence},
        {"occurredAt", event.occurred_at_ms},
        {"severity", to_string(event.severity)},
        {"category", event.category},
        {"code", event.code},
        {"message", event.message},
        {"instanceId", event.instance_id},
        {"clientOrderId", event.client_order_id},
        {"requestId", event.request_id},
    };
    if (event.venue) json["venue"] = to_string(*event.venue);
    if (event.order) json["order"] = *event.order;
    return json;
}

nlohmann::json system_view(const OrderGateway& gateway, std::size_t event_limit) {
    const auto stability = gateway.stability();
    auto events = nlohmann::json::array();
    for (const auto& event : gateway.operational_events(event_limit)) {
        events.push_back(operational_event_view(event));
    }
    return {
        {"ok", true},
        {"transport", {
             {"commands", "REST API"},
             {"commandsDetail", "POST/PATCH/DELETE /api/v1/orders"},
             {"snapshots", "REST API"},
             {"snapshotsDetail", "Authoritative recovery and 15-second UI fallback"},
             {"updates", "WebSocket"},
             {"updatesDetail", "/ws/v1/orders streams order, market, and system events"},
             {"marketIngress", "Memory-mapped ring"},
             {"marketIngressDetail", "abex_market_data -> state/market-data.ring -> gateway"},
         }},
        {"exchangeConnectivity", {
             {"orderGatewayIsolation", false},
             {"processModel", "IN_PROCESS"},
             {"restartDomain", "abex_server (OMS and both order adapters)"},
             {"isolationDetail",
              "Market data is process-isolated; OKX and Binance order adapters currently are not"},
             {"venues", {
                  {"OKX", {
                       {"commands", "Authenticated REST API"},
                       {"commandOperations", "place · amend · cancel · query"},
                       {"updates", "Private WebSocket"},
                       {"updateDetail", "Authenticated orders channel"},
                       {"heartbeat", "Application ping/pong · 20 s idle · 8 s timeout"},
                       {"reconnect", "Exponential backoff · 250 ms to 10 s"},
                   }},
                  {"BINANCE", {
                       {"commands", "Signed WebSocket API"},
                       {"commandOperations",
                        "order.place · order.cancel · order.cancelReplace · order.status"},
                       {"updates", "Signed user-data WebSocket"},
                       {"updateDetail", "Execution reports after server-time synchronization"},
                       {"heartbeat", "WebSocket control ping/pong"},
                       {"reconnect", "Exponential backoff · 250 ms to 10 s"},
                   }},
              }},
         }},
        {"recoveryPolicy", {
             {"intentPersistence", "Intent and requestId are durable before exchange I/O"},
             {"requestReplay", "Duplicate client/request IDs replay the persisted result"},
             {"transportFailure",
              "Unknown outcomes are not blindly resent; the order is marked UNKNOWN"},
             {"reconnectRecovery", "Authenticated reconnect schedules automatic reconciliation"},
             {"startupRecovery", "Journal reload plus venue open-order enumeration and point queries restore authoritative state"},
             {"periodicRecovery", "Venue reconciliation repeats on the configured interval"},
             {"marketOrderFinality", "Placement ACK may be LIVE; only execution/query evidence is terminal"},
         }},
        {"persistence", {
             {"model", "Checksummed append-only JSONL"},
             {"location", stability.journal.location},
             {"durableWrites", stability.journal.durable_writes},
             {"recordSequence", stability.journal.record_sequence},
             {"singleWriterLock", true},
             {"writeOrdering", "Intent and requestId are persisted before exchange I/O"},
             {"recovery", "Latest order snapshots and request outcomes reload on startup"},
         }},
        {"stability", {
             {"instanceId", stability.instance_id},
             {"startedAt", stability.started_at_ms},
             {"recoveredOrders", stability.recovered_orders},
             {"idempotentReplays", stability.idempotent_replays},
             {"reconciliations", stability.reconciliations},
             {"alerts", stability.alerts},
             {"loggingFailures", stability.logging_failures},
             {"lastLoggingError", stability.last_logging_error},
         }},
        {"events", std::move(events)},
        {"serverTime", unix_time_ms()},
    };
}

nlohmann::json market_quote_view(const MarketDataBook& book, const MarketQuote& quote) {
    const auto now = unix_time_ms();
    const auto age = now - quote.published_at_ms;
    return {
        {"venue", to_string(quote.venue)},
        {"symbol", quote.symbol},
        {"bid", quote.bid_price.to_string()},
        {"ask", quote.ask_price.to_string()},
        {"sourceTime", quote.source_time_ms},
        {"publishedAt", quote.published_at_ms},
        {"sequence", quote.sequence},
        {"ageMs", std::max<std::int64_t>(0, age)},
        {"fresh", book.fresh(quote, now)},
    };
}

nlohmann::json market_data_view(const MarketDataBook& book) {
    struct BestPrices {
        std::optional<MarketQuote> buy;
        std::optional<MarketQuote> sell;
    };

    const auto now = unix_time_ms();
    auto quotes = nlohmann::json::array();
    std::unordered_map<std::string, BestPrices> best;
    struct SourceHealth {
        std::size_t fresh_symbols{0};
        std::int64_t last_update_ms{0};
    };
    std::unordered_map<Venue, SourceHealth> sources{{Venue::Okx, {}}, {Venue::Binance, {}}};
    for (const auto& quote : book.snapshot()) {
        quotes.push_back(market_quote_view(book, quote));
        auto& source = sources[quote.venue];
        source.last_update_ms = std::max(source.last_update_ms, quote.published_at_ms);
        if (!book.fresh(quote, now)) continue;
        ++source.fresh_symbols;
        auto& prices = best[quote.symbol];
        if (!prices.buy || quote.ask_price < prices.buy->ask_price) prices.buy = quote;
        if (!prices.sell || quote.bid_price > prices.sell->bid_price) prices.sell = quote;
    }

    auto best_json = nlohmann::json::object();
    for (const auto& [symbol, prices] : best) {
        auto item = nlohmann::json::object();
        if (prices.buy) {
            item["buy"] = {
                {"venue", to_string(prices.buy->venue)},
                {"price", prices.buy->ask_price.to_string()},
            };
        }
        if (prices.sell) {
            item["sell"] = {
                {"venue", to_string(prices.sell->venue)},
                {"price", prices.sell->bid_price.to_string()},
            };
        }
        best_json[symbol] = std::move(item);
    }

    auto sources_json = nlohmann::json::object();
    const auto status = book.status();
    for (const auto& [venue, source] : sources) {
        const auto age = source.last_update_ms == 0 ? std::int64_t{0}
                                                    : now - source.last_update_ms;
        sources_json[to_string(venue)] = {
            {"connected", source.fresh_symbols == 2},
            {"transport", status.transport},
            {"freshSymbols", source.fresh_symbols},
            {"expectedSymbols", 2},
            {"lastUpdate", source.last_update_ms},
            {"ageMs", std::max<std::int64_t>(0, age)},
        };
    }

    const auto ring_age = status.last_update_ms == 0 ? std::int64_t{0}
                                                     : now - status.last_update_ms;
    return {
        {"ok", true},
        {"healthy", status.ring_connected &&
                        sources.at(Venue::Okx).fresh_symbols == 2 &&
                        sources.at(Venue::Binance).fresh_symbols == 2},
        {"ring", {
             {"mapped", status.ring_mapped},
             {"connected", status.ring_connected},
             {"status", status.ring_connected ? "LIVE"
                                               : status.ring_mapped ? "STALE" : "UNAVAILABLE"},
             {"generation", status.generation},
             {"lastSequence", status.last_sequence},
             {"lastUpdate", status.last_update_ms},
             {"ageMs", std::max<std::int64_t>(0, ring_age)},
             {"lastError", status.last_error},
         }},
        {"maximumAgeMs", book.maximum_age().count()},
        {"sources", std::move(sources_json)},
        {"quotes", std::move(quotes)},
        {"best", std::move(best_json)},
        {"serverTime", now},
    };
}

} // namespace abex
