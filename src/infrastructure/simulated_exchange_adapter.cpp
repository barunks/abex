#include "abex/infrastructure/simulated_exchange_adapter.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <thread>
#include <utility>

namespace abex {

SimulatedExchangeAdapter::SimulatedExchangeAdapter(Venue venue)
    : SimulatedExchangeAdapter(venue, Config{}) {}

SimulatedExchangeAdapter::SimulatedExchangeAdapter(Venue venue, Config config)
    : SimulatedExchangeAdapter(venue, config, {}) {}

SimulatedExchangeAdapter::SimulatedExchangeAdapter(
    Venue venue,
    Config config,
    std::shared_ptr<MarketDataBook> market_data)
    : venue_(venue), config_(std::move(config)),
      rate_limiter_(config_.request_burst, config_.requests_per_second),
      market_data_(std::move(market_data)), initial_balances_(config_.initial_balances) {
    if (initial_balances_.empty()) {
        initial_balances_ = {
            {"BTC", Decimal::parse("10")},
            {"ETH", Decimal::parse("100")},
            {"USDT", Decimal::parse("1000000")},
        };
    }
}

SimulatedExchangeAdapter::~SimulatedExchangeAdapter() { stop(); }

void SimulatedExchangeAdapter::start(ExecutionCallback execution_callback,
                                     ConnectionCallback connection_callback) {
    {
        std::scoped_lock lock(mutex_);
        execution_callback_ = std::move(execution_callback);
        connection_callback_ = std::move(connection_callback);
    }
    connected_.store(true);
    if (connection_callback_) connection_callback_(venue_, true, {});
    if (market_data_) {
        market_observer_token_ = market_data_->add_observer(
            [this](const MarketQuote& quote) { match_orders(quote); });
        for (const auto& quote : market_data_->snapshot()) match_orders(quote);
    }
}

void SimulatedExchangeAdapter::stop() noexcept {
    if (market_data_ && market_observer_token_ != 0) {
        market_data_->remove_observer(market_observer_token_);
        market_observer_token_ = 0;
    }
    const bool was_connected = connected_.exchange(false);
    if (was_connected && connection_callback_) {
        connection_callback_(venue_, false, "simulated exchange disconnected");
    }
}

static std::pair<std::string, std::string> split_symbol(const std::string& symbol) {
    const auto sep = symbol.find('-');
    if (sep == std::string::npos || sep == 0 || sep + 1 >= symbol.size()) return {};
    return {symbol.substr(0, sep), symbol.substr(sep + 1)};
}

void SimulatedExchangeAdapter::restore(std::span<const Order> recovered_orders) {
    std::scoped_lock lock(mutex_);
    std::uint64_t next_sequence = next_sequence_.load();
    std::uint64_t next_exchange_id = next_exchange_id_.load();
    const std::string prefix = venue_ == Venue::Okx ? "OKX-SIM-" : "BINANCE-SIM-";
    const auto observe_exchange_id = [&](const std::string& exchange_order_id) {
        if (!exchange_order_id.starts_with(prefix)) return;
        const auto suffix = std::string_view(exchange_order_id).substr(prefix.size());
        std::uint64_t parsed{};
        const auto [end, error] =
            std::from_chars(suffix.data(), suffix.data() + suffix.size(), parsed);
        if (error == std::errc{} && end == suffix.data() + suffix.size()) {
            next_exchange_id = std::max(next_exchange_id, parsed + 1);
        }
    };
    for (const auto& order : recovered_orders) {
        if (order.venue == venue_ && !is_terminal(order.status)) {
            orders_[order.client_order_id] = order;
            observe_exchange_id(order.exchange_order_id);
            for (const auto& alias : order.exchange_order_id_aliases) {
                observe_exchange_id(alias);
            }
            if (order.last_sequence) {
                next_sequence = std::max(next_sequence, *order.last_sequence + 1);
            }
            // Restore incremental balance state for recovered open orders.
            Decimal ep;
            if (order.price) ep = *order.price;
            else if (order.average_fill_price) ep = *order.average_fill_price;
            apply_balance_place_locked(order, ep);
            // Reflect already-filled portion in fill_adjustments_.
            if (order.filled_quantity.is_positive()) {
                apply_balance_fill_locked(order, order.filled_quantity,
                                          order.average_fill_price.value_or(ep));
                // apply_balance_place_locked froze the full quantity; restore only remaining.
                // The fill helper already released the filled portion's frozen amount,
                // but place froze the full quantity, so we need to re-freeze only remaining.
                // Simpler: rebuild frozen from scratch for this order.
                const auto [base, quote] = split_symbol(order.symbol);
                if (!base.empty()) {
                    const auto remaining = order.quantity - order.filled_quantity;
                    if (order.side == Side::Buy) {
                        auto& f = frozen_[quote];
                        f = f > (order.quantity * ep) ? f - (order.quantity * ep) : Decimal{};
                        f += remaining * ep;
                    } else {
                        auto& f = frozen_[base];
                        f = f > order.quantity ? f - order.quantity : Decimal{};
                        f += remaining;
                    }
                }
            }
        } else if (order.venue == venue_ && is_terminal(order.status) &&
                   order.filled_quantity.is_positive()) {
            // Terminal orders with fills: reflect realized P&L.
            const auto ep = order.average_fill_price.value_or(
                order.price.value_or(Decimal{}));
            apply_balance_fill_locked(order, order.filled_quantity, ep);
        }
    }
    next_sequence_.store(next_sequence);
    next_exchange_id_.store(next_exchange_id);
}

AdapterResult SimulatedExchangeAdapter::rate_limited_result() {
    return {.accepted = false,
            .code = "RATE_LIMITED",
            .message = "simulated venue request rate exceeded"};
}

AdapterResult SimulatedExchangeAdapter::place(const Order& order) {
    if (!connected_.load()) {
        return {.accepted = false,
                .outcome_uncertain = true,
                .code = "DISCONNECTED",
                .message = "venue connection is unavailable"};
    }
    if (config_.throw_on_place) throw std::runtime_error("injected place failure");
    if (!rate_limiter_.try_acquire()) return rate_limited_result();

    Order stored = order;
    {
        std::scoped_lock lock(mutex_);
        if (const auto found = orders_.find(order.client_order_id); found != orders_.end()) {
            return {.accepted = true,
                    .exchange_order_id = found->second.exchange_order_id,
                    .exchange_client_order_id = found->second.client_order_id};
        }
        stored.exchange_order_id = (venue_ == Venue::Okx ? "OKX-SIM-" : "BINANCE-SIM-") +
                                   std::to_string(next_exchange_id_.fetch_add(1));
        stored.status = OrderStatus::Live;
        stored.pending_action = PendingAction::None;
        orders_[stored.client_order_id] = stored;
        Decimal ep;
        if (stored.price) ep = *stored.price;
        else if (market_data_) {
            if (const auto q = market_data_->latest(venue_, stored.symbol)) ep = executable_price(*q, stored.side);
        }
        apply_balance_place_locked(stored, ep);
    }

    if (config_.report_before_ack) {
        publish_or_buffer(report_for(stored, OrderStatus::Live, Decimal{}, std::nullopt, {},
                                     std::nullopt));
    }
    if (market_data_) {
        if (const auto quote = market_data_->latest(venue_, stored.symbol); quote &&
            market_data_->fresh(*quote)) {
            match_orders(*quote);
        }
    }
    return {.accepted = true,
            .exchange_order_id = stored.exchange_order_id,
            .exchange_client_order_id = stored.client_order_id};
}

AdapterResult SimulatedExchangeAdapter::cancel(const Order& order) {
    if (!connected_.load()) {
        return {.accepted = false,
                .outcome_uncertain = true,
                .code = "DISCONNECTED",
                .message = "venue connection is unavailable"};
    }
    if (!rate_limiter_.try_acquire()) return rate_limited_result();

    Order stored;
    {
        std::scoped_lock lock(mutex_);
        const auto found = orders_.find(order.client_order_id);
        if (found == orders_.end()) {
            return {.accepted = false, .code = "UNKNOWN_ORDER", .message = "order not found"};
        }
        if (found->second.status == OrderStatus::Filled) {
            return {.accepted = false,
                    .code = "ORDER_FILLED",
                    .message = "filled order cannot be canceled"};
        }
        apply_balance_release_locked(found->second,
            found->second.price ? *found->second.price : Decimal{});
        found->second.status = OrderStatus::Canceled;
        ++found->second.version;
        stored = found->second;
    }
    publish_or_buffer(report_for(stored, OrderStatus::Canceled, stored.filled_quantity,
                                 std::nullopt, {}, std::nullopt));
    return {.accepted = true,
            .exchange_order_id = stored.exchange_order_id,
            .exchange_client_order_id = stored.client_order_id};
}

AdapterResult SimulatedExchangeAdapter::amend(const Order& order,
                                              std::optional<Decimal> new_price,
                                              std::optional<Decimal> new_quantity) {
    if (!connected_.load()) {
        return {.accepted = false,
                .outcome_uncertain = true,
                .code = "DISCONNECTED",
                .message = "venue connection is unavailable"};
    }
    if (!rate_limiter_.try_acquire()) return rate_limited_result();

    Order stored;
    Order superseded;
    bool replacement = false;
    Decimal replacement_quantity;
    {
        std::scoped_lock lock(mutex_);
        const auto found = orders_.find(order.client_order_id);
        if (found == orders_.end()) {
            return {.accepted = false, .code = "UNKNOWN_ORDER", .message = "order not found"};
        }
        if (is_terminal(found->second.status)) {
            return {.accepted = false,
                    .code = "ORDER_TERMINAL",
                    .message = "terminal order cannot be amended"};
        }
        if (venue_ == Venue::Binance) {
            replacement = true;
            superseded = found->second;
            const auto target_quantity = new_quantity.value_or(found->second.quantity);
            replacement_quantity = target_quantity - found->second.filled_quantity;
            if (replacement_quantity <= Decimal{}) {
                return {.accepted = false,
                        .code = "INVALID_REPLACEMENT_QUANTITY",
                        .message = "replacement quantity must exceed the already-filled quantity"};
            }
            const auto old_exchange_id = found->second.exchange_order_id;
            found->second.exchange_order_id_aliases.insert(old_exchange_id);
            found->second.exchange_fill_offsets.try_emplace(old_exchange_id, Decimal{});
            found->second.exchange_quote_offsets.try_emplace(old_exchange_id, Decimal{});
            found->second.exchange_order_id = "BINANCE-SIM-" +
                                              std::to_string(next_exchange_id_.fetch_add(1));
            found->second.exchange_fill_offsets[found->second.exchange_order_id] =
                found->second.filled_quantity;
            found->second.exchange_quote_offsets[found->second.exchange_order_id] =
                found->second.cumulative_quote;
            found->second.quantity = target_quantity;
            found->second.status = found->second.filled_quantity > Decimal{}
                                       ? OrderStatus::PartiallyFilled
                                       : OrderStatus::Live;
        } else if (new_quantity) {
            found->second.quantity = *new_quantity;
        }
        if (new_price) found->second.price = new_price;
        ++found->second.version;
        stored = found->second;
    }
    if (replacement) {
        auto canceled = report_for(superseded, OrderStatus::Canceled,
                                   superseded.filled_quantity, std::nullopt, {}, std::nullopt);
        auto replacement_report = report_for(stored, stored.status, stored.filled_quantity,
                                             std::nullopt, {}, std::nullopt);
        replacement_report.order_quantity = replacement_quantity;
        if (config_.amend_reports_before_ack) {
            publish_or_buffer(std::move(canceled));
            publish_or_buffer(std::move(replacement_report));
            // This simulation mode deliberately models the private stream being
            // observed before the compound request response is delivered.
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
            return {.accepted = true,
                    .replacement = true,
                    .original_order_canceled = true,
                    .exchange_order_id = stored.exchange_order_id,
                    .exchange_client_order_id = stored.client_order_id};
        }
        if (config_.fill_before_replace) {
            // Simulate a fill racing the cancel leg: inject a partial fill on the
            // old generation so final_old_fill > the quantity the adapter used to
            // compute replacement_quantity, triggering REPLACEMENT_QUANTITY_DRIFT.
            auto race_fill = report_for(superseded, OrderStatus::PartiallyFilled,
                                        superseded.filled_quantity + Decimal::parse("0.01"),
                                        Decimal::parse("50000"), {}, std::nullopt);
            return {.accepted = true,
                    .replacement = true,
                    .original_order_canceled = true,
                    .exchange_order_id = stored.exchange_order_id,
                    .exchange_client_order_id = stored.client_order_id,
                    .authoritative_reports = {std::move(race_fill),
                                              std::move(canceled),
                                              std::move(replacement_report)}};
        }
        return {.accepted = true,
                .replacement = true,
                .original_order_canceled = true,
                .exchange_order_id = stored.exchange_order_id,
                .exchange_client_order_id = stored.client_order_id,
                .authoritative_reports = {std::move(canceled),
                                          std::move(replacement_report)}};
    }
    publish_or_buffer(report_for(stored, stored.status, stored.filled_quantity, std::nullopt, {},
                                 std::nullopt));
    if (market_data_) {
        if (const auto quote = market_data_->latest(venue_, stored.symbol); quote &&
            market_data_->fresh(*quote)) {
            match_orders(*quote);
        }
    }
    return {.accepted = true,
            .exchange_order_id = stored.exchange_order_id,
            .exchange_client_order_id = stored.client_order_id};
}

std::optional<ExecutionReport> SimulatedExchangeAdapter::query(const Order& order) {
    if (!connected_.load() || !rate_limiter_.try_acquire()) return std::nullopt;
    std::scoped_lock lock(mutex_);
    const auto found = orders_.find(order.client_order_id);
    if (found == orders_.end()) return std::nullopt;
    const auto offset = found->second.exchange_fill_offsets.contains(
                            found->second.exchange_order_id)
                            ? found->second.exchange_fill_offsets.at(
                                  found->second.exchange_order_id)
                            : Decimal{};
    return ExecutionReport{
        .client_order_id = found->second.client_order_id,
        .exchange_order_id = found->second.exchange_order_id,
        .status = found->second.status,
        .cumulative_filled = found->second.filled_quantity - offset,
        .order_price = found->second.price,
        .order_quantity = found->second.quantity - offset,
        .event_time_ms = unix_time_ms(),
    };
}

// ── incremental balance helpers ──────────────────────────────────────────

void SimulatedExchangeAdapter::apply_balance_place_locked(
    const Order& order, Decimal execution_price) {
    const auto [base, quote] = split_symbol(order.symbol);
    if (base.empty()) return;
    fill_adjustments_.try_emplace(base, Decimal{});
    fill_adjustments_.try_emplace(quote, Decimal{});
    frozen_.try_emplace(base, Decimal{});
    frozen_.try_emplace(quote, Decimal{});
    const auto remaining = order.quantity - order.filled_quantity;
    if (order.side == Side::Buy) frozen_[quote] += remaining * execution_price;
    else frozen_[base] += remaining;
}

void SimulatedExchangeAdapter::apply_balance_release_locked(
    const Order& order, Decimal execution_price) {
    const auto [base, quote] = split_symbol(order.symbol);
    if (base.empty()) return;
    const auto remaining = order.quantity - order.filled_quantity;
    if (order.side == Side::Buy) {
        auto& f = frozen_[quote];
        const auto delta = remaining * execution_price;
        f = f > delta ? f - delta : Decimal{};
    } else {
        auto& f = frozen_[base];
        f = f > remaining ? f - remaining : Decimal{};
    }
}

void SimulatedExchangeAdapter::apply_balance_fill_locked(
    const Order& order, Decimal delta_filled, Decimal fill_price) {
    const auto [base, quote] = split_symbol(order.symbol);
    if (base.empty()) return;
    fill_adjustments_.try_emplace(base, Decimal{});
    fill_adjustments_.try_emplace(quote, Decimal{});
    frozen_.try_emplace(base, Decimal{});
    frozen_.try_emplace(quote, Decimal{});
    const auto notional = delta_filled * fill_price;
    if (order.side == Side::Buy) {
        fill_adjustments_[base] += delta_filled;
        fill_adjustments_[quote] -= notional;
        // Release the frozen quote that was reserved for this fill.
        auto& f = frozen_[quote];
        f = f > notional ? f - notional : Decimal{};
    } else {
        fill_adjustments_[base] -= delta_filled;
        fill_adjustments_[quote] += notional;
        auto& f = frozen_[base];
        f = f > delta_filled ? f - delta_filled : Decimal{};
    }
}

BalanceQueryResult
SimulatedExchangeAdapter::query_balances(std::optional<std::string> currency) {
    if (!connected_.load()) {
        return {.code = "DISCONNECTED",
                .message = "simulated venue connection is unavailable",
                .snapshot = {.venue = venue_}};
    }
    if (!rate_limiter_.try_acquire()) {
        return {.code = "RATE_LIMITED",
                .message = "simulated venue request rate exceeded",
                .snapshot = {.venue = venue_}};
    }
    if (currency) {
        std::ranges::transform(*currency, currency->begin(), [](unsigned char character) {
            return static_cast<char>(std::toupper(character));
        });
    }

    std::scoped_lock lock(mutex_);
    AccountBalanceSnapshot snapshot{
        .venue = venue_,
        .account_id = "SIM-" + std::string(to_string(venue_)),
        .main_account_id = "SIM-" + std::string(to_string(venue_)),
        .observed_at_ms = unix_time_ms(),
    };
    // Merge initial_balances_ with fill_adjustments_ — both are O(currencies), not O(orders).
    auto totals = initial_balances_;
    for (const auto& [asset, delta] : fill_adjustments_) {
        totals.try_emplace(asset, Decimal{});
        totals[asset] += delta;
    }
    for (const auto& [asset, total] : totals) {
        if (currency && asset != *currency) continue;
        const auto it = frozen_.find(asset);
        const auto reserved = it != frozen_.end() ? it->second : Decimal{};
        const auto available = total > reserved ? total - reserved : Decimal{};
        snapshot.balances.push_back({
            .currency = asset,
            .total = total.to_string(),
            .available = available.to_string(),
            .frozen = reserved.to_string(),
            .order_frozen = reserved.to_string(),
        });
    }
    std::ranges::sort(snapshot.balances, {}, &AccountBalance::currency);
    return {.ok = true, .snapshot = std::move(snapshot)};
}

InstrumentRulesQueryResult
SimulatedExchangeAdapter::query_instrument_rules(std::string symbol) {
    if (!connected_.load()) {
        return {.code = "DISCONNECTED",
                .message = "simulated venue connection is unavailable",
                .rules = {.venue = venue_, .symbol = std::move(symbol)}};
    }
    std::ranges::transform(symbol, symbol.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    if (symbol != "BTC-USDT" && symbol != "ETH-USDT") {
        return {.code = "INSTRUMENT_NOT_FOUND",
                .message = "simulator supports BTC-USDT and ETH-USDT",
                .rules = {.venue = venue_, .symbol = std::move(symbol)}};
    }

    InstrumentRules rules{
        .venue = venue_,
        .symbol = symbol,
        .status = venue_ == Venue::Okx ? "live" : "TRADING",
        .trading = true,
        .minimum_price = Decimal::parse("0.01"),
        .maximum_price = venue_ == Venue::Binance
                             ? std::optional(Decimal::parse("1000000")) : std::nullopt,
        .price_tick = venue_ == Venue::Okx && symbol == "BTC-USDT"
                          ? Decimal::parse("0.1") : Decimal::parse("0.01"),
        .minimum_quantity = symbol == "BTC-USDT"
                                ? Decimal::parse("0.00001") : Decimal::parse("0.0001"),
        .maximum_quantity = venue_ == Venue::Binance
                                ? std::optional(Decimal::parse("9000")) : std::nullopt,
        .quantity_step = venue_ == Venue::Okx
                             ? (symbol == "BTC-USDT" ? Decimal::parse("0.00000001")
                                                     : Decimal::parse("0.000001"))
                             : (symbol == "BTC-USDT" ? Decimal::parse("0.00001")
                                                     : Decimal::parse("0.0001")),
        .market_maximum_quantity = venue_ == Venue::Okx
                                       ? std::optional(Decimal::parse("1000000"))
                                       : std::nullopt,
        .maximum_notional = venue_ == Venue::Okx
                                ? std::optional(Decimal::parse("20000000"))
                                : std::optional(Decimal::parse("9000000")),
        .market_minimum_notional = venue_ == Venue::Binance
                                       ? std::optional(Decimal::parse("5")) : std::nullopt,
        .market_maximum_notional = venue_ == Venue::Okx
                                       ? std::optional(Decimal::parse("1000000")) : std::nullopt,
        .observed_at_ms = unix_time_ms(),
    };
    if (venue_ == Venue::Binance) rules.minimum_notional = Decimal::parse("5");
    return {.ok = true, .rules = std::move(rules)};
}

std::optional<std::vector<ExecutionReport>>
SimulatedExchangeAdapter::query_open_orders() {
    if (!connected_.load() || !rate_limiter_.try_acquire()) return std::nullopt;
    std::vector<ExecutionReport> reports;
    std::scoped_lock lock(mutex_);
    for (const auto& [client_id, order] : orders_) {
        (void)client_id;
        if (is_terminal(order.status) && !config_.report_terminal_orders_as_open) continue;
        const auto offset = order.exchange_fill_offsets.contains(order.exchange_order_id)
                                ? order.exchange_fill_offsets.at(order.exchange_order_id)
                                : Decimal{};
        reports.push_back(ExecutionReport{
            .client_order_id = order.client_order_id,
            .exchange_order_id = order.exchange_order_id,
            .status = is_terminal(order.status) ? OrderStatus::Live : order.status,
            .cumulative_filled = order.filled_quantity - offset,
            .order_price = order.price,
            .order_quantity = order.quantity - offset,
            .event_time_ms = unix_time_ms(),
        });
    }
    return reports;
}

bool SimulatedExchangeAdapter::emit(std::string_view client_order_id,
                                    OrderStatus status,
                                    Decimal cumulative_filled,
                                    std::optional<Decimal> last_fill_price,
                                    std::string event_id,
                                    std::optional<std::uint64_t> sequence) {
    Order stored;
    {
        std::scoped_lock lock(mutex_);
        const auto found = orders_.find(client_order_id);
        if (found == orders_.end()) return false;
        found->second.filled_quantity = std::max(found->second.filled_quantity, cumulative_filled);
        found->second.status = status;
        ++found->second.version;
        stored = found->second;
    }
    publish_or_buffer(report_for(stored, status, cumulative_filled, last_fill_price,
                                 std::move(event_id), sequence));
    return true;
}

void SimulatedExchangeAdapter::disconnect() {
    const bool was_connected = connected_.exchange(false);
    if (was_connected && connection_callback_) {
        connection_callback_(venue_, false, "simulated exchange disconnected");
    }
}

void SimulatedExchangeAdapter::reconnect() {
    const bool was_connected = connected_.exchange(true);
    if (!was_connected && connection_callback_) connection_callback_(venue_, true, {});

    std::deque<ExecutionReport> buffered;
    ExecutionCallback callback;
    {
        std::scoped_lock lock(mutex_);
        buffered.swap(buffered_reports_);
        callback = execution_callback_;
    }
    if (callback) {
        std::scoped_lock emit_lock(execution_emit_mutex_);
        for (auto& report : buffered) callback(venue_, std::move(report));
    }
}

void SimulatedExchangeAdapter::synchronize_rate_limiter(
    double capacity, double available, double tokens_per_second) {
    rate_limiter_.synchronize(capacity, available, tokens_per_second);
}

ExecutionReport SimulatedExchangeAdapter::report_for(
    const Order& order,
    OrderStatus status,
    Decimal cumulative_filled,
    std::optional<Decimal> last_fill_price,
    std::string event_id,
    std::optional<std::uint64_t> sequence) {
    if (event_id.empty()) {
        event_id = "sim-" + std::string(to_string(venue_)) + '-' + order.client_order_id + '-' +
                   std::string(to_string(status)) + '-' + cumulative_filled.to_string() + "-v" +
                   std::to_string(order.version) + '-' +
                   std::to_string(next_event_id_.fetch_add(1));
    }
    if (!sequence) sequence = next_sequence_.fetch_add(1);
    const auto fill_offset = order.exchange_fill_offsets.contains(order.exchange_order_id)
                                 ? order.exchange_fill_offsets.at(order.exchange_order_id)
                                 : Decimal{};
    return ExecutionReport{
        .event_id = std::move(event_id),
        .client_order_id = order.client_order_id,
        .exchange_order_id = order.exchange_order_id,
        .status = status,
        .cumulative_filled = cumulative_filled - fill_offset,
        .last_fill_price = last_fill_price,
        .order_price = order.price,
        .order_quantity = order.quantity - fill_offset,
        .sequence = sequence,
        .event_time_ms = unix_time_ms(),
    };
}

void SimulatedExchangeAdapter::publish_or_buffer(ExecutionReport report) {
    ExecutionCallback callback;
    {
        std::scoped_lock lock(mutex_);
        if (!connected_.load()) {
            buffered_reports_.push_back(std::move(report));
            return;
        }
        callback = execution_callback_;
    }
    if (callback) {
        std::scoped_lock emit_lock(execution_emit_mutex_);
        callback(venue_, std::move(report));
    }
}

void SimulatedExchangeAdapter::match_orders(const MarketQuote& quote) {
    if (quote.venue != venue_ || !connected_.load()) return;
    std::vector<ExecutionReport> reports;
    {
        std::scoped_lock lock(mutex_);
        for (auto& [client_order_id, order] : orders_) {
            (void)client_order_id;
            if (is_terminal(order.status) || order.symbol != quote.symbol) continue;
            const auto fill_price = executable_price(quote, order.side);
            const bool marketable = order.type == OrderType::Market ||
                                    (order.side == Side::Buy && order.price &&
                                     *order.price >= quote.ask_price) ||
                                    (order.side == Side::Sell && order.price &&
                                     *order.price <= quote.bid_price);
            if (!marketable) continue;
            const auto prev_filled = order.filled_quantity;
            order.status = OrderStatus::Filled;
            order.filled_quantity = order.quantity;
            ++order.version;
            apply_balance_fill_locked(order, order.quantity - prev_filled, fill_price);
            reports.push_back(report_for(order, OrderStatus::Filled, order.quantity, fill_price,
                                         {}, std::nullopt));
        }
    }
    for (auto& report : reports) publish_or_buffer(std::move(report));
}

} // namespace abex
