#!/usr/bin/env python3
path = 'include/abex/application/operational_event_writer.hpp'
with open(path, 'r') as f:
    lines = f.readlines()

out = []
for line in lines:
    if '// category, code, instance_id are string_view (string literals / permanent).' in line:
        out.append('    // All string fields in RawEntry are owned std::string to prevent\n')
        out.append('    // dangling string_view from caller-thread temporaries.\n')
        out.append('    // instance_id is the only truly permanent string_view but we keep\n')
        out.append('    // it as std::string for uniformity and safety.\n')
        continue
    if '// message, client_order_id, request_id are std::string (may be temporaries).' in line:
        continue
    if '        std::string_view category;' in line:
        out.append('        std::string category;\n')
        continue
    if '        std::string_view code;' in line:
        out.append('        std::string code;\n')
        continue
    if '        std::string_view instance_id;' in line:
        out.append('        std::string instance_id;\n')
        continue
    out.append(line)

with open(path, 'w') as f:
    f.writelines(out)
print('Done')
