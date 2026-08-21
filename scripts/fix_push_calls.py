#!/usr/bin/env python3
path = 'include/abex/application/operational_event_writer.hpp'
with open(path, 'r') as f:
    src = f.read()

src = src.replace(
    'return push({occurred_at_ms, severity, category, code, std::move(message),\n'
    '                     instance_id, venue, client_order_id, request_id,\n'
    '                     std::move(order_ctx)});',
    'return push({occurred_at_ms, severity, category, code, std::move(message),\n'
    '                     instance_id, venue,\n'
    '                     std::string(client_order_id), std::string(request_id),\n'
    '                     std::move(order_ctx)});'
)

src = src.replace(
    'const bool a = push({occurred_at_ms, sev_a, cat_a, code_a, std::move(msg_a),\n'
    '                              instance_id, venue, client_order_id, request_id, order_ctx});\n'
    '        const bool b = push({occurred_at_ms, sev_b, cat_b, code_b, std::move(msg_b),\n'
    '                              instance_id, venue, client_order_id, request_id,\n'
    '                              std::move(order_ctx)});',
    'const bool a = push({occurred_at_ms, sev_a, cat_a, code_a, std::move(msg_a),\n'
    '                              instance_id, venue,\n'
    '                              std::string(client_order_id), std::string(request_id),\n'
    '                              order_ctx});\n'
    '        const bool b = push({occurred_at_ms, sev_b, cat_b, code_b, std::move(msg_b),\n'
    '                              instance_id, venue,\n'
    '                              std::string(client_order_id), std::string(request_id),\n'
    '                              std::move(order_ctx)});'
)

with open(path, 'w') as f:
    f.write(src)
print('Done')
