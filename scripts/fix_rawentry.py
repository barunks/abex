#!/usr/bin/env python3
path = 'include/abex/application/operational_event_writer.hpp'
with open(path, 'r') as f:
    lines = f.readlines()

out = []
for line in lines:
    # Fix the comment block
    if '// category, code, instance_id, client_order_id, request_id are string_view' in line:
        out.append('    // category, code, instance_id are string_view (string literals / permanent).\n')
        out.append('    // message, client_order_id, request_id are std::string (may be temporaries).\n')
        continue
    if '// (always string literals or permanent Order strings' in line:
        continue
    if '// message is std::string because callers sometimes pass constructed strings.' in line:
        continue
    # Fix the field declarations
    if '        std::string_view client_order_id;' in line:
        out.append('        std::string client_order_id;\n')
        continue
    if '        std::string_view request_id;' in line:
        out.append('        std::string request_id;\n')
        continue
    out.append(line)

with open(path, 'w') as f:
    f.writelines(out)
print('Done')
