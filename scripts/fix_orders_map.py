#!/usr/bin/env python3
import sys

with open('src/application/order_gateway.cpp', 'r') as f:
    src = f.read()

replacements = [
    # place() outbound declaration
    ('    std::shared_ptr<const Order> outbound;\n    std::optional<RiskDecision> local_rejection;',
     '    std::shared_ptr<Order> outbound;\n    std::optional<RiskDecision> local_rejection;'),

    # amend() outbound declaration
    ('    std::shared_ptr<const Order> outbound;\n    const auto request_key = "AMEND:',
     '    std::shared_ptr<Order> outbound;\n    const auto request_key = "AMEND:'),

    # amend() first lock: found->second -> *found->second (the one with processed_requests)
    ('            auto& order = found->second;\n            if (const auto replay = order.processed_requests',
     '            auto& order = *found->second;\n            if (const auto replay = order.processed_requests'),

    # amend() outbound = make_shared -> map copy
    ('            outbound = std::make_shared<const Order>(order);\n        }\n        publish_positions_if_dirty_locked();\n        pp = prepare_persist(*outbound);\n    }\n    commit_persist(outbound, pp.first, pp.second);\n    notify_order_observers(outbound);\n    record_event2(OperationalSeverity::Info, "PERSISTENCE", "AMEND_INTENT_PERSISTED"',
     '            outbound = found->second; // ref-count copy\n        }\n        publish_positions_if_dirty_locked();\n        pp = prepare_persist(*outbound);\n    }\n    commit_persist(outbound, pp.first, pp.second);\n    notify_order_observers(outbound);\n    record_event2(OperationalSeverity::Info, "PERSISTENCE", "AMEND_INTENT_PERSISTED"'),

    # amend() intent record_event2: drop order_event_context(*outbound) for the AMEND_SENT_TO_EXCHANGE pair
    ('                  outbound->venue, outbound->client_order_id, request.request_id,\n                  order_event_context(*outbound));\n\n    AdapterResult result;\n    try {\n        result = adapter_for(outbound->venue)->amend(',
     '                  outbound->venue, outbound->client_order_id, request.request_id,\n                  {});\n\n    AdapterResult result;\n    try {\n        result = adapter_for(outbound->venue)->amend('),

    # amend() persisted declaration
    ('    std::shared_ptr<const Order> persisted;\n    std::string replacement_warning_to_log;',
     '    std::shared_ptr<Order> persisted;\n    std::string replacement_warning_to_log;'),

    # amend() second lock: orders_.at dereference
    ('            auto& order = orders_.at(request.client_order_id);\n            const auto previous_position = position_contribution(order);\n            active_operations_.erase(request.client_order_id);\n            if (result.accepted) {\n            std::string replacement_warning;',
     '            auto& order = *orders_.at(request.client_order_id);\n            const auto previous_position = position_contribution(order);\n            active_operations_.erase(request.client_order_id);\n            if (result.accepted) {\n            std::string replacement_warning;'),

    # amend() persisted = make_shared -> map copy
    ('            persisted = std::make_shared<const Order>(order);\n        }\n        publish_positions_if_dirty_locked();\n        pp = prepare_persist(*persisted);\n    }\n    commit_persist(persisted, pp.first, pp.second);\n    notify_order_observers(persisted);\n\n    if (!replacement_warning_to_log',
     '            persisted = orders_.at(request.client_order_id);\n        }\n        publish_positions_if_dirty_locked();\n        pp = prepare_persist(*persisted);\n    }\n    commit_persist(persisted, pp.first, pp.second);\n    notify_order_observers(persisted);\n\n    if (!replacement_warning_to_log'),

    # apply_execution() persisted declaration
    ('    std::shared_ptr<const Order> persisted;\n    bool should_persist = false;',
     '    std::shared_ptr<Order> persisted;\n    bool should_persist = false;'),

    # apply_execution() persisted = make_shared -> map copy
    ('        persisted = std::make_shared<const Order>(*order);\n        publish_positions_if_dirty_locked();\n        pp = prepare_persist(*persisted);\n    } // mutex_ released here',
     '        persisted = orders_.at(order->client_order_id);\n        publish_positions_if_dirty_locked();\n        pp = prepare_persist(*persisted);\n    } // mutex_ released here'),

    # reconcile() candidate_ids loop
    ('        for (const auto& [client_order_id, order] : orders_) {\n            if (order.venue == venue) candidate_ids.push_back(client_order_id);\n        }',
     '        for (const auto& [client_order_id, sp] : orders_) {\n            if (sp->venue == venue) candidate_ids.push_back(client_order_id);\n        }'),

    # reconcile() query_order = found->second
    ('            query_order = found->second;\n            if (is_terminal(query_order.status)',
     '            query_order = *found->second;\n            if (is_terminal(query_order.status)'),

    # reconcile() inner persisted declaration and current dereference
    ('                std::shared_ptr<const Order> persisted;\n                {\n                    std::scoped_lock lk(mutex_);\n                    auto& current = orders_.at(client_order_id);',
     '                std::shared_ptr<Order> persisted;\n                {\n                    std::scoped_lock lk(mutex_);\n                    auto& current = *orders_.at(client_order_id);'),

    # reconcile() persisted = make_shared -> map copy
    ('                    persisted = std::make_shared<const Order>(current);\n                    auto persist6 = prepare_persist(*persisted);',
     '                    persisted = orders_.at(client_order_id);\n                    auto persist6 = prepare_persist(*persisted);'),

    # get() return dereference
    ('    return found == orders_.end() ? std::nullopt : std::optional(found->second);',
     '    return found == orders_.end() ? std::nullopt : std::optional(*found->second);'),

    # get_snapshot() order dereference
    ('    const auto& order = found->second;\n    return OrderSnapshot{',
     '    const auto& order = *found->second;\n    return OrderSnapshot{'),

    # list() loop
    ('    for (const auto& [id, order] : orders_) {\n        (void)id;\n        if (venue && order.venue != *venue) continue;\n        if (status && order.status != *status) continue;\n        result.push_back(order);\n    }',
     '    for (const auto& [id, sp] : orders_) {\n        (void)id;\n        if (venue && sp->venue != *venue) continue;\n        if (status && sp->status != *status) continue;\n        result.push_back(*sp);\n    }'),

    # list_snapshots() loop header
    ('    for (const auto& [id, order] : orders_) {\n        (void)id;\n        if (venue && order.venue != *venue) continue;\n        if (status && order.status != *status) continue;\n        result.push_back(OrderSnapshot{',
     '    for (const auto& [id, sp] : orders_) {\n        (void)id;\n        if (venue && sp->venue != *venue) continue;\n        if (status && sp->status != *status) continue;\n        result.push_back(OrderSnapshot{'),

    # list_snapshots() body fields
    ('            .client_order_id = order.client_order_id,\n            .exchange_order_id = order.exchange_order_id,\n            .venue = order.venue,\n            .symbol = order.symbol,\n            .side = order.side,\n            .type = order.type,\n            .price = order.price,\n            .quantity = order.quantity,\n            .time_in_force = order.time_in_force,\n            .status = order.status,\n            .pending_action = order.pending_action,\n            .pending_amend_price = order.pending_amend_price,\n            .pending_amend_quantity = order.pending_amend_quantity,\n            .filled_quantity = order.filled_quantity,\n            .average_fill_price = order.average_fill_price,\n            .rejection_reason = order.rejection_reason,\n            .version = order.version,\n            .last_sequence = order.last_sequence,\n            .created_at_ms = order.created_at_ms,\n            .updated_at_ms = order.updated_at_ms,\n        });\n    }\n    std::ranges::sort(result, [](const OrderSnapshot& lhs, const OrderSnapshot& rhs)',
     '            .client_order_id = sp->client_order_id,\n            .exchange_order_id = sp->exchange_order_id,\n            .venue = sp->venue,\n            .symbol = sp->symbol,\n            .side = sp->side,\n            .type = sp->type,\n            .price = sp->price,\n            .quantity = sp->quantity,\n            .time_in_force = sp->time_in_force,\n            .status = sp->status,\n            .pending_action = sp->pending_action,\n            .pending_amend_price = sp->pending_amend_price,\n            .pending_amend_quantity = sp->pending_amend_quantity,\n            .filled_quantity = sp->filled_quantity,\n            .average_fill_price = sp->average_fill_price,\n            .rejection_reason = sp->rejection_reason,\n            .version = sp->version,\n            .last_sequence = sp->last_sequence,\n            .created_at_ms = sp->created_at_ms,\n            .updated_at_ms = sp->updated_at_ms,\n        });\n    }\n    std::ranges::sort(result, [](const OrderSnapshot& lhs, const OrderSnapshot& rhs)'),

    # rebuild_indexes_locked loop
    ('    for (const auto& [client_id, order] : orders_) {\n        if (!order.exchange_order_id.empty())\n            exchange_id_index_[order.exchange_order_id] = client_id;\n        for (const auto& alias : order.exchange_order_id_aliases)\n            exchange_id_index_[alias] = client_id;\n        exchange_client_id_index_[client_id] = client_id;\n        for (const auto& alias : order.exchange_client_id_aliases)\n            exchange_client_id_index_[alias] = client_id;\n        const auto id = symbol_id_rt(order.symbol);\n        if (id != SymbolId::Unknown)\n            conservative_positions_[to_slot(id)] += position_contribution(order);\n    }',
     '    for (const auto& [client_id, sp] : orders_) {\n        if (!sp->exchange_order_id.empty())\n            exchange_id_index_[sp->exchange_order_id] = client_id;\n        for (const auto& alias : sp->exchange_order_id_aliases)\n            exchange_id_index_[alias] = client_id;\n        exchange_client_id_index_[client_id] = client_id;\n        for (const auto& alias : sp->exchange_client_id_aliases)\n            exchange_client_id_index_[alias] = client_id;\n        const auto id = symbol_id_rt(sp->symbol);\n        if (id != SymbolId::Unknown)\n            conservative_positions_[to_slot(id)] += position_contribution(*sp);\n    }'),

    # locate_order_locked direct lookup
    ('        if (auto direct = orders_.find(report.client_order_id); direct != orders_.end()) {\n            if (auto* order = owned_by_venue(&direct->second)) return order;\n        }',
     '        if (auto direct = orders_.find(report.client_order_id); direct != orders_.end()) {\n            if (auto* order = owned_by_venue(direct->second.get())) return order;\n        }'),

    # locate_order_locked alias lookups
    ('            if (auto* order = owned_by_venue(&orders_.at(alias->second))) return order;',
     '            if (auto* order = owned_by_venue(orders_.at(alias->second).get())) return order;'),
    ('            if (auto* order = owned_by_venue(&orders_.at(exchange->second))) return order;',
     '            if (auto* order = owned_by_venue(orders_.at(exchange->second).get())) return order;'),

    # conservative_position_locked excluded order
    ('        if (excluded != orders_.end() && excluded->second.symbol == symbol)\n            result -= position_contribution(excluded->second);',
     '        if (excluded != orders_.end() && excluded->second->symbol == symbol)\n            result -= position_contribution(*excluded->second);'),
]

for old, new in replacements:
    if old not in src:
        print(f"NOT FOUND: {repr(old[:60])}", file=sys.stderr)
    else:
        src = src.replace(old, new, 1)
        print(f"OK: {repr(old[:60])}")

with open('src/application/order_gateway.cpp', 'w') as f:
    f.write(src)
print("Done")
