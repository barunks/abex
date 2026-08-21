#!/usr/bin/env python3
import re, sys

path = 'src/application/order_gateway.cpp'
with open(path, 'r') as f:
    src = f.read()

# Pattern: variable declarations like:
#   std::pair<bool, std::uint64_t> pp0;
#   std::pair<bool, std::uint64_t> pp;
#   std::pair<bool, std::uint64_t> pp1;
#   std::pair<bool, std::uint64_t> pp_ca;
#   std::pair<bool, std::uint64_t> pp_aa;
#   std::pair<bool, std::uint64_t> persist6;
src = re.sub(
    r'std::pair<bool, std::uint64_t> (pp\w*|persist\w*);',
    r'std::tuple<std::shared_ptr<const Order>, bool, std::uint64_t> \1;',
    src
)

# Pattern: pp0 = prepare_persist(*outbound, !local_rejection.has_value());
# -> auto [snap0, io0, seq0] = prepare_persist(*outbound, !local_rejection.has_value());
# We need to replace each assignment and the subsequent commit_persist call.
# Easier: replace the pair access pp.first/pp.second with get<1>/get<2>,
# and replace commit_persist(x, pp.first, pp.second) with commit_persist(get<0>(pp), get<1>(pp), get<2>(pp))

# Replace pp0.first -> std::get<1>(pp0), pp0.second -> std::get<2>(pp0)
for var in ['pp0', 'pp1', 'pp', 'pp_ca', 'pp_aa', 'persist6']:
    src = src.replace(f'{var}.first', f'std::get<1>({var})')
    src = src.replace(f'{var}.second', f'std::get<2>({var})')

# Replace commit_persist(outbound, std::get<1>(pp0), std::get<2>(pp0))
# -> commit_persist(std::get<0>(pp0), std::get<1>(pp0), std::get<2>(pp0))
# The first arg is now ignored (snapshot is in the tuple), so replace it.
for var in ['pp0', 'pp1', 'pp', 'pp_ca', 'pp_aa', 'persist6']:
    # commit_persist(outbound, std::get<1>(pp0), std::get<2>(pp0))
    src = re.sub(
        rf'commit_persist\(\w+, std::get<1>\({re.escape(var)}\), std::get<2>\({re.escape(var)}\)\)',
        f'commit_persist(std::get<0>({var}), std::get<1>({var}), std::get<2>({var}))',
        src
    )

# Also fix notify_order_observers — it still takes shared_ptr<Order> (the live map ptr)
# No change needed there.

with open(path, 'w') as f:
    f.write(src)
print('Done')
