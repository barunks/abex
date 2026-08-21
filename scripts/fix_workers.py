#!/usr/bin/env python3
import sys

files = {
    'include/abex/application/async_journal_lane.hpp': (
        # old
        '    void run(std::stop_token token) {\n'
        '        while (!token.stop_requested()) {\n'
        '            // Park until an item arrives or a stop wake is sent.\n'
        '            Entry entry;\n'
        '            if (!ring_.try_pop(entry)) {\n'
        '                // Nothing ready yet \xe2\x80\x94 block, then re-check stop.\n'
        '                entry = ring_.pop_item();\n'
        '                if (token.stop_requested() &&\n'
        '                    pending_.load(std::memory_order_acquire) == 0) break;\n'
        '            }\n'
        '\n'
        '            try {\n'
        '                std::string payload;\n'
        '                payload.reserve(512);\n'
        '                JsonSerializer::write_order(payload, *entry.order, entry.intent_only);\n'
        '                store_->commit_order(*entry.order, std::move(payload), entry.sequence);\n'
        '            } catch (...) {}\n'
        '\n'
        '            if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1)\n'
        '                pending_.notify_all();\n'
        '        }\n'
        '        // Drain any items that arrived before stop was observed.\n'
        '        Entry entry;\n'
        '        while (ring_.try_pop(entry)) {\n'
        '            try {\n'
        '                std::string payload;\n'
        '                payload.reserve(512);\n'
        '                JsonSerializer::write_order(payload, *entry.order, entry.intent_only);\n'
        '                store_->commit_order(*entry.order, std::move(payload), entry.sequence);\n'
        '            } catch (...) {}\n'
        '            if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1)\n'
        '                pending_.notify_all();\n'
        '        }\n'
        '        pending_.notify_all();\n'
        '    }',
        # new
        '    void run(std::stop_token token) {\n'
        '        while (!token.stop_requested()) {\n'
        '            auto [ok, entry] = ring_.pop_item();\n'
        '            if (!ok) continue; // spurious wake (stop sentinel)\n'
        '            process(std::move(entry));\n'
        '        }\n'
        '        Entry entry;\n'
        '        while (ring_.try_pop(entry)) process(std::move(entry));\n'
        '        pending_.notify_all();\n'
        '    }\n'
        '\n'
        '    void process(Entry entry) noexcept {\n'
        '        try {\n'
        '            std::string payload;\n'
        '            payload.reserve(512);\n'
        '            JsonSerializer::write_order(payload, *entry.order, entry.intent_only);\n'
        '            store_->commit_order(*entry.order, std::move(payload), entry.sequence);\n'
        '        } catch (...) {}\n'
        '        if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1)\n'
        '            pending_.notify_all();\n'
        '    }'
    ),
    'include/abex/application/async_order_observer_queue.hpp': (
        # old
        '    void run(std::stop_token token) {\n'
        '        while (!token.stop_requested()) {\n'
        '            std::shared_ptr<Order> order;\n'
        '            if (!ring_.try_pop(order)) {\n'
        '                order = ring_.pop_item();\n'
        '                if (token.stop_requested() &&\n'
        '                    pending_.load(std::memory_order_acquire) == 0) break;\n'
        '            }\n'
        '\n'
        '            const auto snapshot = observers_.load(std::memory_order_acquire);\n'
        '            for (const auto& [tok, observer] : *snapshot) {\n'
        '                (void)tok;\n'
        '                try { observer(*order); } catch (...) {}\n'
        '            }\n'
        '            if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1)\n'
        '                pending_.notify_all();\n'
        '        }\n'
        '        // Drain remaining.\n'
        '        std::shared_ptr<Order> order;\n'
        '        while (ring_.try_pop(order)) {\n'
        '            const auto snapshot = observers_.load(std::memory_order_acquire);\n'
        '            for (const auto& [tok, observer] : *snapshot) {\n'
        '                (void)tok;\n'
        '                try { observer(*order); } catch (...) {}\n'
        '            }\n'
        '            if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1)\n'
        '                pending_.notify_all();\n'
        '        }\n'
        '        pending_.notify_all();\n'
        '    }',
        # new
        '    void run(std::stop_token token) {\n'
        '        while (!token.stop_requested()) {\n'
        '            auto [ok, order] = ring_.pop_item();\n'
        '            if (!ok) continue;\n'
        '            dispatch(std::move(order));\n'
        '        }\n'
        '        std::shared_ptr<Order> order;\n'
        '        while (ring_.try_pop(order)) dispatch(std::move(order));\n'
        '        pending_.notify_all();\n'
        '    }\n'
        '\n'
        '    void dispatch(std::shared_ptr<Order> order) noexcept {\n'
        '        const auto snapshot = observers_.load(std::memory_order_acquire);\n'
        '        for (const auto& [tok, observer] : *snapshot) {\n'
        '            (void)tok;\n'
        '            try { observer(*order); } catch (...) {}\n'
        '        }\n'
        '        if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1)\n'
        '            pending_.notify_all();\n'
        '    }'
    ),
}

for path, (old, new) in files.items():
    with open(path, 'r') as f:
        src = f.read()
    if old not in src:
        print(f'NOT FOUND in {path}', file=sys.stderr)
        sys.exit(1)
    src = src.replace(old, new, 1)
    with open(path, 'w') as f:
        f.write(src)
    print(f'OK: {path}')

print('Done')
