// Floating YieldBasis L=2 VirtualPool reference model.
//
// This keeps source-free actor timing and a synthetic fresh-L2 start, but the
// selected route executes the represented production bundle atomically:
// flash-backed native add/remove, LevAMM exchange, accrued-interest donation,
// repayment, rounding discount, capacity, and final user output.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "harness/yb_initial_state.hpp"
#include "pools/twocrypto_fx/twocrypto.hpp"

namespace arb::harness {

inline constexpr long double YB_REFERENCE_2L_MIN_PROFIT_COIN0 = 1.0L;

template <typename T>
struct YbReference2LState {
    T leverage{};
    T lev_ratio{};
    T min_safe_debt_ratio{};
    T max_safe_debt_ratio{};
    T fee{};
    T collateral{};
    T debt{};
    T rate{};
    T rate_mul{};
    uint64_t rate_time{0};
    T minted{};
    T redeemed{};
    T stable_balance{};
    T flash_max_loan{};
    T stable_aggregator{};
    T rounding_discount{};
    T lt_donation_discount{};
    T lt_stable_balance{};
    bool killed{false};
};

template <typename T>
struct YbReference2LCosts {
    T transaction_coin0{};
    std::array<T, 2> leg_coin0{T(0), T(0)};
    T market_basis_bps{};
    std::array<T, 2> execution_bps{T(0), T(0)};
    T notional_cap_coin0{};
};

template <typename T>
struct YbReference2LInterestSummary {
    T accrued{};
    T donated{};
    T pending_interest{};
    T pending_donation{};
    T conservation_residual{};
};

template <typename T>
struct YbReference2LRouteResult {
    bool committed{false};
    size_t direction{0};
    T input{};
    T output{};
    T profit_coin0{};
    T lp_amount{};
    T donation{};
    T donation_min_mint{};
    T flash_amount{};
    T price_scale_after_add{};
    T price_scale_after_donation{};
    T price_scale_after_remove{};
    T virtual_price_before_donation{};
    T virtual_price_after_donation{};
    T xcp_profit_before_donation{};
    T xcp_profit_after_donation{};
    bool emitted_add{false};
    bool emitted_remove{false};
    bool emitted_donation{false};
    bool price_scale_moved{false};
    T search_fraction{};
    bool search_bound_max{false};
    bool search_bound_notional_cap{false};
    std::string rejection;
};

template <typename T>
class YbReference2LMarket {
public:
    using State = YbReference2LState<T>;
    using Costs = YbReference2LCosts<T>;
    using Result = YbReference2LRouteResult<T>;
    using PoolTraits = pools::twocrypto_fx::PoolTraits<T>;

    YbReference2LMarket() = default;

    template <typename Pool>
    static YbReference2LMarket fresh_2l(
        const Pool& pool,
        const T& target_donation_apy,
        const T& levamm_fee,
        uint64_t timestamp,
        const T& stable_cash_multiplier = T(1)
    ) {
        static_assert(
            std::is_floating_point_v<T>,
            "reference_2l is available only on floating-point runtimes"
        );
        const T precision = one();
        const T tvl = pool.balances[0]
            + pool.balances[1] * pool.cached_price_scale / precision;
        const T collateral = pool.totalSupply - pool.donation_shares
            - PoolTraits::MINIMUM_LIQUIDITY();
        if (!(tvl > T(0)) || !(collateral > T(0))) {
            throw std::runtime_error("fresh reference_2l requires a funded pool");
        }
        if (!(levamm_fee >= T(0)) || !(levamm_fee <= precision)) {
            throw std::runtime_error("reference_2l fee must be in [0, 1]");
        }
        if (!(stable_cash_multiplier > T(0))) {
            throw std::runtime_error("reference_2l cash multiplier must be positive");
        }

        State state;
        state.leverage = T(2) * precision;
        state.lev_ratio = T(4) * precision / T(9);
        state.min_safe_debt_ratio = precision / T(16);
        state.max_safe_debt_ratio = T(17) * precision / T(32);
        state.fee = levamm_fee;
        state.collateral = collateral;
        state.debt = tvl / T(2);
        state.rate = target_donation_apy * tvl / state.debt
            / T(365ULL * 86400ULL);
        state.rate_mul = precision;
        state.rate_time = timestamp;
        state.minted = state.debt;
        state.stable_balance = state.debt * stable_cash_multiplier;
        state.flash_max_loan = tvl;
        state.stable_aggregator = precision;
        state.rounding_discount = precision / T(100000000);
        state.lt_donation_discount = precision / T(100);
        return YbReference2LMarket(std::move(state));
    }

    static YbReference2LMarket from_state(const YbInitialState<T>& initial) {
        validate_yb_initial_state(initial);
        const T one = PoolTraits::PRECISION();
        const T denominator = T(2) * initial.leverage - one;
        State state;
        state.leverage = initial.leverage;
        state.lev_ratio = initial.leverage * initial.leverage * one /
            (denominator * denominator);
        state.min_safe_debt_ratio = one * one * one /
            (T(4) * initial.leverage * initial.leverage);
        state.max_safe_debt_ratio = denominator * denominator * one /
            (T(4) * initial.leverage * initial.leverage) -
            one * one * one / (T(8) * initial.leverage * initial.leverage);
        state.fee = initial.fee;
        state.collateral = initial.collateral;
        state.debt = initial.debt;
        state.rate = initial.rate;
        state.rate_mul = initial.rate_mul;
        state.rate_time = initial.rate_time;
        state.minted = initial.minted;
        state.redeemed = initial.redeemed;
        state.stable_balance = initial.stable_balance;
        state.flash_max_loan = initial.flash_max_loan;
        state.stable_aggregator = initial.stable_aggregator;
        state.rounding_discount = initial.rounding_discount;
        state.lt_donation_discount = initial.lt_donation_discount;
        state.lt_stable_balance = initial.lt_stable_balance;
        state.killed = initial.killed;
        YbReference2LMarket market(std::move(state));
        const T debt_at_checkpoint = market.projected_debt(initial.source_timestamp);
        market.initial_unsettled_interest_ = std::max(
            T(0), debt_at_checkpoint + market.state_.redeemed - market.state_.minted
        ) + market.state_.lt_stable_balance;
        // Exclude the stored rate_time-to-checkpoint interval from simulated accrual.
        market.accrued_interest_total_ = market.state_.debt - debt_at_checkpoint;
        return market;
    }

    bool enabled() const { return enabled_; }
    const State& state() const { return state_; }

    template <typename Pool>
    T lp_oracle(const Pool& pool) const {
        const T sqrt_scale = std::sqrt(pool.cached_price_scale * one());
        return T(2) * pool.get_virtual_price() * sqrt_scale / one()
            * state_.stable_aggregator / one();
    }

    T projected_debt(uint64_t timestamp) const {
        if (!enabled_ || timestamp < state_.rate_time) return T(0);
        const T next_mul = state_.rate_mul * (
            one() + state_.rate * T(timestamp - state_.rate_time)
        ) / one();
        return state_.debt * next_mul / state_.rate_mul;
    }

    YbReference2LInterestSummary<T> projected_interest_summary(
        uint64_t timestamp
    ) const {
        YbReference2LMarket projected = *this;
        projected.advance_debt(timestamp);
        const T pending = projected.pending_interest();
        const T sources = projected.initial_unsettled_interest_
            + projected.accrued_interest_total_;
        const T uses = projected.donated_interest_total_ + pending
            + projected.state_.lt_stable_balance;
        return {
            projected.accrued_interest_total_,
            projected.donated_interest_total_,
            pending,
            projected.state_.lt_stable_balance,
            sources - uses,
        };
    }

    template <typename Pool>
    Result apply_atomic(
        Pool& pool,
        size_t direction,
        const T& input,
        const T& min_output,
        uint64_t timestamp
    ) {
        Pool candidate_pool = pool;
        YbReference2LMarket candidate_market = *this;
        auto result = try_apply_in_place(
            candidate_market, candidate_pool, direction, input, min_output,
            timestamp
        );
        if (!result.committed) return result;
        pool = std::move(candidate_pool);
        *this = std::move(candidate_market);
        return result;
    }

    template <typename Pool>
    Result best_route_for_direction(
        const Pool& pool,
        const T& cex_price,
        uint64_t timestamp,
        const Costs& costs,
        bool charge_transaction_cost,
        size_t direction,
        T min_fraction = T(1e-7),
        T max_fraction = T(0.02)
    ) const {
        static_assert(
            std::is_floating_point_v<T>,
            "reference_2l route sizing is floating-only"
        );
        Result best;
        best.rejection = "nonpositive net route";
        if (!enabled_ || !(cex_price > T(0)) || direction > 1) return best;

        bool have_trial = false;
        const T cex_coin0_per_coin1 = cex_price
            * (T(1) + costs.market_basis_bps / T(10000));
        const T basis = direction == 0 ? pool.balances[0] : pool.balances[1];
        for (size_t step = 0; step < 24; ++step) {
            const T q = T(step) / T(23);
            const T fraction = min_fraction
                * std::pow(max_fraction / min_fraction, q);
            T amount = basis * fraction;
            bool cap_bound = false;
            if (costs.notional_cap_coin0 > T(0)) {
                const T cap = direction == 0
                    ? costs.notional_cap_coin0
                    : costs.notional_cap_coin0 / cex_coin0_per_coin1;
                if (amount >= cap) {
                    amount = cap;
                    cap_bound = true;
                }
            }

            Pool trial_pool = pool;
            YbReference2LMarket trial_market = *this;
            auto trial = try_apply_in_place(
                trial_market, trial_pool, direction, amount, T(0), timestamp
            );
            if (!trial.committed) continue;
            trial.search_fraction = fraction;
            trial.search_bound_max = step + 1 == 24;
            trial.search_bound_notional_cap = cap_bound;
            const T execution = costs.execution_bps[direction] / T(10000);
            const T fixed_cost = costs.leg_coin0[direction]
                + (charge_transaction_cost ? costs.transaction_coin0 : T(0));
            trial.profit_coin0 = direction == 0
                ? trial.output * cex_coin0_per_coin1 * (T(1) - execution)
                    - amount - fixed_cost
                : trial.output - amount * cex_coin0_per_coin1
                    * (T(1) + execution) - fixed_cost;
            if (!have_trial || trial.profit_coin0 > best.profit_coin0) {
                best = std::move(trial);
                have_trial = true;
            }
            if (cap_bound) break;
        }
        if (!(best.profit_coin0 > T(YB_REFERENCE_2L_MIN_PROFIT_COIN0))) {
            best.committed = false;
            best.rejection = "nonpositive net route";
        }
        return best;
    }

    template <typename Pool>
    Result execute_best(
        Pool& pool,
        const T& cex_price,
        uint64_t timestamp,
        const Costs& costs,
        bool charge_transaction_cost = true
    ) {
        auto best = best_route_for_direction(
            pool, cex_price, timestamp, costs, charge_transaction_cost, 0
        );
        auto other = best_route_for_direction(
            pool, cex_price, timestamp, costs, charge_transaction_cost, 1
        );
        if (other.profit_coin0 > best.profit_coin0) best = std::move(other);
        if (!best.committed) return best;

        auto result = apply_atomic(
            pool, best.direction, best.input, T(0), timestamp
        );
        result.profit_coin0 = best.profit_coin0;
        result.search_fraction = best.search_fraction;
        result.search_bound_max = best.search_bound_max;
        result.search_bound_notional_cap = best.search_bound_notional_cap;
        return result;
    }

private:
    State state_{};
    bool enabled_{false};
    T initial_unsettled_interest_{};
    T accrued_interest_total_{};
    T donated_interest_total_{};

    explicit YbReference2LMarket(State state)
        : state_(std::move(state)), enabled_(true) {
        initial_unsettled_interest_ = pending_interest()
            + state_.lt_stable_balance;
    }

    static T one() { return PoolTraits::PRECISION(); }

    static std::optional<T> reject_value(
        std::string& rejection,
        const char* reason
    ) {
        rejection = reason;
        return std::nullopt;
    }

    static Result reject_route(
        size_t direction,
        const T& input,
        const char* reason
    ) {
        Result result;
        result.direction = direction;
        result.input = input;
        result.rejection = reason;
        return result;
    }

    template <typename Pool>
    static Result try_apply_in_place(
        YbReference2LMarket& market,
        Pool& pool,
        size_t direction,
        const T& input,
        const T& min_output,
        uint64_t timestamp
    ) {
        Result result;
        result.direction = direction;
        result.input = input;
        if (!market.enabled_) {
            result.rejection = "disabled";
            return result;
        }
        try {
            pool.set_block_timestamp(timestamp);
            return market.apply_in_place(
                pool, direction, input, min_output, timestamp
            );
        } catch (const std::exception& error) {
            result.rejection = error.what();
        } catch (...) {
            result.rejection = "unknown exception";
        }
        return result;
    }

    std::optional<T> x0(
        const T& oracle,
        const T& collateral,
        const T& debt,
        bool safe,
        std::string& rejection
    ) const {
        const T coll_value = oracle * collateral / one();
        if (!(coll_value > T(0)) || !(collateral > T(0)) || !(debt >= T(0))) {
            return reject_value(rejection, "invalid YB state");
        }
        if (safe && (
            debt < coll_value * state_.min_safe_debt_ratio / one() ||
            debt > coll_value * state_.max_safe_debt_ratio / one()
        )) {
            return reject_value(rejection, "unsafe YB debt");
        }
        const T term = T(4) * coll_value * state_.lev_ratio / one() * debt;
        if (term > coll_value * coll_value) {
            return reject_value(rejection, "negative YB discriminant");
        }
        return (coll_value + std::sqrt(coll_value * coll_value - term))
            * one() / (T(2) * state_.lev_ratio);
    }

    void advance_debt(uint64_t timestamp) {
        if (timestamp < state_.rate_time) {
            throw std::runtime_error("YB time reversal");
        }
        const T next_mul = state_.rate_mul * (
            one() + state_.rate * T(timestamp - state_.rate_time)
        ) / one();
        const T next_debt = state_.debt * next_mul / state_.rate_mul;
        accrued_interest_total_ += next_debt - state_.debt;
        state_.debt = next_debt;
        state_.rate_mul = next_mul;
        state_.rate_time = timestamp;
    }

    T pending_interest() const {
        return state_.debt + state_.redeemed > state_.minted
            ? state_.debt + state_.redeemed - state_.minted
            : T(0);
    }

    void accrue_interest(uint64_t timestamp) {
        advance_debt(timestamp);
        T interest = pending_interest();
        if (interest > state_.stable_balance) interest = state_.stable_balance;
        state_.minted += interest;
        state_.stable_balance -= interest;
        state_.lt_stable_balance += interest;
    }

    template <typename Pool>
    bool collect_and_donate(
        Pool& pool,
        uint64_t timestamp,
        Result& result
    ) {
        accrue_interest(timestamp);
        const T donation = state_.lt_stable_balance;
        if (!(donation > T(0))) return true;

        result.donation = donation;
        result.donation_min_mint = (one() - state_.lt_donation_discount)
            * donation / pool.lp_price_at(timestamp);
        result.virtual_price_before_donation = pool.get_virtual_price();
        result.xcp_profit_before_donation = pool.xcp_profit;
        pool.add_liquidity(
            {donation, T(0)}, result.donation_min_mint, true
        );
        state_.lt_stable_balance = T(0);
        donated_interest_total_ += donation;
        result.emitted_donation = true;
        result.price_scale_after_donation = pool.cached_price_scale;
        result.virtual_price_after_donation = pool.get_virtual_price();
        result.xcp_profit_after_donation = pool.xcp_profit;
        return true;
    }

    template <typename Pool>
    std::optional<T> amm_exchange(
        Pool& pool,
        size_t direction,
        const T& input,
        uint64_t timestamp,
        Result& result,
        std::string& rejection
    ) {
        if (state_.killed || !(state_.collateral > T(0))) {
            return reject_value(rejection, "inactive YB AMM");
        }
        const T oracle = lp_oracle(pool);
        advance_debt(timestamp);
        const T pending = pending_interest();
        const auto old_x0 = x0(
            oracle, state_.collateral, state_.debt, false, rejection
        );
        if (!old_x0.has_value()) return std::nullopt;
        const T x_initial = *old_x0 - state_.debt;
        const T before_ratio = state_.debt > T(0)
            ? oracle * state_.collateral / state_.debt
            : std::numeric_limits<T>::max();

        T output{};
        if (direction == 0) {
            if (input > state_.debt) {
                return reject_value(rejection, "stable input exceeds debt");
            }
            const T x = x_initial + input;
            const T y = x_initial * state_.collateral / x;
            output = (state_.collateral - y) * (one() - state_.fee) / one();
            state_.debt -= input;
            state_.collateral -= output;
            state_.redeemed += input;
            state_.stable_balance += input;
        } else {
            const T y = state_.collateral + input;
            const T x = x_initial * state_.collateral / y;
            output = (x_initial - x) * (one() - state_.fee) / one();
            if (output > state_.stable_balance) {
                return reject_value(rejection, "YB stable liquidity");
            }
            state_.debt += output;
            state_.collateral = y;
            state_.stable_balance -= output;
        }
        state_.minted = state_.debt + state_.redeemed - pending;

        const T after_ratio = state_.debt > T(0)
            ? oracle * state_.collateral / state_.debt
            : std::numeric_limits<T>::max();
        bool check_state = true;
        if ((after_ratio > T(2) * one() && before_ratio > after_ratio) ||
            (after_ratio <= T(2) * one() && before_ratio < after_ratio)) {
            check_state = false;
        }
        const auto final_x0 = x0(
            oracle, state_.collateral, state_.debt, check_state, rejection
        );
        if (!final_x0.has_value()) return std::nullopt;
        if (*final_x0 < *old_x0) {
            return reject_value(rejection, "bad YB final state");
        }
        if (!collect_and_donate(pool, timestamp, result)) return std::nullopt;
        return output;
    }

    template <typename Pool>
    Result apply_in_place(
        Pool& pool,
        size_t direction,
        const T& input,
        const T& min_output,
        uint64_t timestamp
    ) {
        if (direction > 1 || !(input > T(0))) {
            return reject_route(direction, input, "invalid VirtualPool input");
        }
        Result result;
        result.direction = direction;
        result.input = input;
        const T price_scale_before = pool.cached_price_scale;
        std::string rejection;

        if (direction == 1) {
            if (!(pool.balances[1] > T(0))) {
                return reject_route(direction, input, "empty crypto balance");
            }
            const T flash = input * pool.balances[0] / pool.balances[1];
            if (flash > state_.flash_max_loan) {
                return reject_route(direction, input, "VirtualPool flash capacity");
            }
            result.flash_amount = flash;
            result.lp_amount = pool.add_liquidity(
                {flash, input}, T(0), false
            );
            result.emitted_add = true;
            result.price_scale_after_add = pool.cached_price_scale;
            const auto stable_out = amm_exchange(
                pool, 1, result.lp_amount, timestamp, result, rejection
            );
            if (!stable_out.has_value()) {
                return reject_route(direction, input, rejection.c_str());
            }
            if (*stable_out < flash) {
                return reject_route(
                    direction, input, "flash repayment shortfall"
                );
            }
            result.output = *stable_out - flash;
            if (!result.emitted_donation) {
                result.price_scale_after_donation = pool.cached_price_scale;
            }
        } else {
            const T r0fee = pool.balances[0] * (one() - state_.fee)
                / pool.totalSupply;
            advance_debt(timestamp);
            const auto state_x0 = x0(
                lp_oracle(pool), state_.collateral, state_.debt,
                false, rejection
            );
            if (!state_x0.has_value()) {
                return reject_route(direction, input, rejection.c_str());
            }
            const T b = *state_x0 - state_.debt + input
                - r0fee * state_.collateral / one();
            const T discriminant = b * b
                + T(4) * state_.collateral * r0fee / one() * input;
            if (!(discriminant >= T(0))) {
                return reject_route(direction, input, "negative flash discriminant");
            }
            const T flash = (std::sqrt(discriminant) - b) / T(2);
            if (flash > state_.flash_max_loan) {
                return reject_route(direction, input, "VirtualPool flash capacity");
            }
            result.flash_amount = flash;
            const T effective = input * (one() - state_.rounding_discount)
                / one();
            const auto lp_out = amm_exchange(
                pool, 0, effective + flash, timestamp, result, rejection
            );
            if (!lp_out.has_value()) {
                return reject_route(direction, input, rejection.c_str());
            }
            result.lp_amount = *lp_out;
            if (!result.emitted_donation) {
                result.price_scale_after_donation = pool.cached_price_scale;
            }
            const auto amounts = pool.remove_liquidity(
                result.lp_amount, {T(0), T(0)}
            );
            result.emitted_remove = true;
            result.price_scale_after_remove = pool.cached_price_scale;
            const T stable_dust = input - effective;
            if (amounts[0] + stable_dust < flash) {
                return reject_route(
                    direction, input, "flash repayment shortfall"
                );
            }
            result.output = amounts[1];
        }
        if (result.output < min_output) {
            return reject_route(direction, input, "VirtualPool slippage");
        }
        result.price_scale_moved = pool.cached_price_scale
            != price_scale_before;
        result.committed = true;
        return result;
    }
};

} // namespace arb::harness
