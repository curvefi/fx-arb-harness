// Trading costs configuration (templated)
#pragma once

#include <type_traits>

namespace arb {
namespace trading {

template <typename T>
struct Costs {
    T arb_fee_bps{static_cast<T>(0.0)};    // exchange fee in bps of the arb flow
    T gas_coin0{static_cast<T>(0.0)};      // fixed gas cost denominated in coin0
    T report_coin0{static_cast<T>(0.0)};   // Extra fixed cost only when submitting a report.
    // Native arbitrage enters only when the first unit's fee-inclusive edge against
    // the event price exceeds this (bps); sizing still charges arb_fee_bps. 0 = off.
    T entry_edge_bps{static_cast<T>(1.5)};
};

} // namespace trading
} // namespace arb
