#!/usr/bin/env python3
path = 'include/abex/application/operational_event_writer.hpp'
with open(path, 'r') as f:
    src = f.read()

src = src.replace(
    'return push({occurred_at_ms, severity, category, code, std::move(message),\n'
    '                     instance_id, venue,\n'
    '                     std::string(client_order_id), std::string(request_id),\n'
    '                     std::move(order_ctx)});',
    'return push({occurred_at_ms, severity,\n'
    '                     std::string(category), std::string(code), std::move(message),\n'
    '                     std::string(instance_id), venue,\n'
    '                     std::string(client_order_id), std::string(request_id),\n'
    '                     std::move(order_ctx)});'
)

src = src.replace(
    'const bool a = push({occurred_at_ms, sev_a, cat_a, code_a, std::move(msg_a),\n'
    '                              instance_id, venue,\n'
    '                              std::string(client_order_id), std::string(request_id),\n'
    '                              order_ctx});\n'
    '        const bool b = push({occurred_at_ms, sev_b, cat_b, code_b, std::move(msg_b),\n'
    '                              instance_id, venue,\n'
    '                              std::string(client_order_id), std::string(request_id),\n'
    '                              std::move(order_ctx)});',
    'const bool a = push({occurred_at_ms, sev_a,\n'
    '                              std::string(cat_a), std::string(code_a), std::move(msg_a),\n'
    '                              std::string(instance_id), venue,\n'
    '                              std::string(client_order_id), std::string(request_id),\n'
    '                              order_ctx});\n'
    '        const bool b = push({occurred_at_ms, sev_b,\n'
    '                              std::string(cat_b), std::string(code_b), std::move(msg_b),\n'
    '                              std::string(instance_id), venue,\n'
    '                              std::string(client_order_id), std::string(request_id),\n'
    '                              std::move(order_ctx)});'
)

# Also fix the worker process() which still does std::string(entry.category) etc.
# Now that they're already std::string, use std::move instead.
src = src.replace(
    '            .category        = std::string(entry.category),\n'
    '            .code            = std::string(entry.code),\n'
    '            .message         = std::move(entry.message),\n'
    '            .instance_id     = std::string(entry.instance_id),',
    '            .category        = std::move(entry.category),\n'
    '            .code            = std::move(entry.code),\n'
    '            .message         = std::move(entry.message),\n'
    '            .instance_id     = std::move(entry.instance_id),'
)

with open(path, 'w') as f:
    f.write(src)
print('Done')
