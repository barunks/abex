#pragma once

#include "abex/domain/execution_report.hpp"

namespace abex {

class OrderStateMachine final {
public:
    [[nodiscard]] static ApplyResult apply(Order& order, const ExecutionReport& report);
};

} // namespace abex
