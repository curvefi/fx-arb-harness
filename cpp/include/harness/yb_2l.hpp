// State-mutating YieldBasis 2L contract model.
//
// Every fill executes proportional pool legs, every interest donation mutates
// the pool, and the complete route rolls back atomically on any failed leg.
// Each causal event admits at most one closed-form XYK fill.
#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "harness/pool_snapshot.hpp"
#include "harness/yb_initial_state.hpp"
#include "pools/twocrypto_fx/twocrypto.hpp"

namespace arb::harness {

inline constexpr long double YB_2L_MIN_PROFIT_COIN0 = 1.0L;

template <typename T>
struct Yb2LState {
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
    T lt_donation_discount{};
    T lt_stable_balance{};
    bool killed{false};
};

template <typename T>
struct Yb2LCosts {
    T transaction_coin0{};
    std::array<T, 2> leg_coin0{T(0), T(0)};
    T market_basis_bps{};
    std::array<T, 2> execution_bps{T(0), T(0)};
    T notional_cap_coin0{};
};

template <typename T>
struct Yb2LInterestSummary {
    T accrued{};
    T donated{};
    T pending_interest{};
    T pending_donation{};
    T conservation_residual{};
};

enum class Yb2LAbstainReason : uint8_t {
    None = 0,
    NoBandEdge,
    MinFill,
    InvalidState,
    NegativeDiscriminant,
    DebtFloor,
    StableCash,
    UnsafeDebt,
    BadFinalState,
    Count,
};

inline constexpr size_t YB_2L_ABSTAIN_REASON_COUNT =
    static_cast<size_t>(Yb2LAbstainReason::Count);

enum class Yb2LDonationReject : uint8_t {
    None = 0,
    Cap,
    MinMint,
    NothingMinted,
    TweakThrow,
    Count,
};

inline constexpr size_t YB_2L_DONATION_REJECT_COUNT =
    static_cast<size_t>(Yb2LDonationReject::Count);

template <typename T>
struct Yb2LResult {
    bool fired{false};
    size_t direction{0};
    Yb2LAbstainReason abstain{Yb2LAbstainReason::None};
    Yb2LDonationReject donation_reject{Yb2LDonationReject::None};
    T input{};
    T output{};
    T net_profit{};
    T lp_oracle{};
    T fair_lp{};
    T levamm_price_before{};
    T levamm_price_after{};
    T target_price{};
    T donation{};
    T donation_min_mint{};
    T price_scale_before_donation{};
    T price_scale_after_donation{};
    T virtual_price_before_donation{};
    T virtual_price_after_donation{};
    T xcp_profit_before_donation{};
    T xcp_profit_after_donation{};
    bool donation_committed{false};
    bool donation_price_scale_moved{false};
    size_t fill_adds{0};
    size_t fill_removes{0};
    size_t fill_add_price_scale_moves{0};
    bool fill_leg_aborted{false};
    bool postadd_aborted{false};
};

template <typename T>
class Yb2LActor {
public:
    using State = Yb2LState<T>;
    using Costs = Yb2LCosts<T>;
    using PoolTraits = pools::twocrypto_fx::PoolTraits<T>;

    Yb2LActor() = default;

    template <typename Pool>
    static Yb2LActor fresh_2l(
        const Pool& pool,
        const T& target_donation_apy,
        const T& levamm_fee,
        uint64_t timestamp,
        const T& stable_cash_multiplier = T(1)
    ) {
        static_assert(
            std::is_floating_point_v<T>,
            "YieldBasis 2L is available only on floating-point runtimes"
        );
        const T precision = PoolTraits::PRECISION();
        const T tvl = pool.balances[0]
            + pool.balances[1] * pool.cached_price_scale / precision;
        const T collateral = pool.totalSupply - pool.donation_shares
            - PoolTraits::MINIMUM_LIQUIDITY();
        if (!(tvl > T(0)) || !(collateral > T(0))) {
            throw std::runtime_error("fresh 2L state requires a funded pool");
        }
        if (!(levamm_fee >= T(0)) || !(levamm_fee <= precision)) {
            throw std::runtime_error("fresh 2L fee must be in [0, 1]");
        }
        if (!(stable_cash_multiplier > T(0))) {
            throw std::runtime_error("fresh 2L cash multiplier must be positive");
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
        state.lt_donation_discount = precision / T(100);
        return Yb2LActor(std::move(state));
    }

    static Yb2LActor from_state(const YbInitialState<T>& initial) {
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
        state.lt_donation_discount = initial.lt_donation_discount;
        state.lt_stable_balance = initial.lt_stable_balance;
        state.killed = initial.killed;
        Yb2LActor actor(std::move(state));
        const T debt_at_checkpoint = actor.projected_debt(initial.source_timestamp);
        actor.initial_unsettled_interest_ = std::max(
            T(0), debt_at_checkpoint + actor.state_.redeemed - actor.state_.minted
        ) + actor.state_.lt_stable_balance;
        // advance_debt starts from stored rate_time; offset its pre-checkpoint
        // portion so cumulative accrual begins at the authoritative checkpoint.
        actor.accrued_interest_total_ = actor.state_.debt - debt_at_checkpoint;
        return actor;
    }

    bool enabled() const { return enabled_; }
    const State& state() const { return state_; }
    uint64_t fires() const { return fires_; }
    const T& shadow_gap_max() const { return shadow_gap_max_; }
    const T& shadow_gap_last() const { return shadow_gap_last_; }
    uint64_t shadow_checks() const { return shadow_checks_; }
    uint64_t shadow_violations() const { return shadow_violations_; }
    uint64_t shadow_abstains() const { return shadow_abstains_; }

    template <typename Pool>
    T lp_oracle(const Pool& pool) const {
        const T sqrt_scale = std::sqrt(pool.cached_price_scale * one());
        return T(2) * pool.get_virtual_price() * sqrt_scale / one();
    }

    T projected_debt(uint64_t timestamp) const {
        if (!enabled_ || timestamp < state_.rate_time) return T(0);
        const T next_mul = state_.rate_mul * (
            one() + state_.rate * T(timestamp - state_.rate_time)
        ) / one();
        return state_.debt * next_mul / state_.rate_mul;
    }

    Yb2LInterestSummary<T> projected_interest_summary(
        uint64_t timestamp
    ) const {
        Yb2LActor projected = *this;
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
    Yb2LResult<T> try_fire(
        Pool& pool,
        const T& external_cex_price,
        uint64_t timestamp,
        const Costs& costs
    ) {
        static_assert(
            std::is_floating_point_v<T>,
            "YieldBasis 2L is available only on floating-point runtimes"
        );
        Yb2LResult<T> result;
        if (!enabled_ || state_.killed || !(external_cex_price > T(0)) ||
            timestamp < state_.rate_time) {
            result.abstain = Yb2LAbstainReason::InvalidState;
            return result;
        }

        Decision decision = compute_decision(
            pool, external_cex_price, timestamp, costs
        );
        result.lp_oracle = decision.oracle;
        if (decision.hard_abstain != Yb2LAbstainReason::None) {
            result.abstain = decision.hard_abstain;
            return result;
        }
        if (decision.chosen_idx < 0) {
            result.abstain = strongest_reason(
                decision.p2.reason, decision.p1.reason
            );
            return result;
        }
        FillProposal chosen = decision.chosen_idx == 0
            ? decision.p2 : decision.p1;

        const T real_circulating = pool.totalSupply - pool.donation_shares
            - PoolTraits::MINIMUM_LIQUIDITY();
        const T expected_collateral = real_circulating;
        const T gap = state_.collateral - expected_collateral;
        const T abs_gap = std::fabs(gap);
        ++shadow_checks_;
        shadow_gap_last_ = gap;
        shadow_gap_max_ = std::max(shadow_gap_max_, abs_gap);
        const T report_tolerance = T(1e-6)
            * std::max(T(1), std::fabs(state_.collateral));
        if (abs_gap > report_tolerance) ++shadow_violations_;
        if (abs_gap > T(1e-2)) {
            ++shadow_abstains_;
            result.abstain = Yb2LAbstainReason::InvalidState;
            return result;
        }

        Yb2LActor pre_attempt_actor = *this;
        PoolTransactionSnapshot<Pool> pre_attempt_pool(pool);

        const auto rollback_route = [&] (
            bool fill_leg_aborted,
            bool postadd_aborted
        ) {
            pre_attempt_pool.restore(pool);
            *this = std::move(pre_attempt_actor);
            result.fired = false;
            result.fill_leg_aborted = fill_leg_aborted;
            result.postadd_aborted = postadd_aborted;
        };

        if (chosen.direction == 1) {
            const auto first_add = add_liquidity_to_mint(
                pool, chosen.input
            );
            if (!first_add.has_value()) {
                rollback_route(true, false);
                result.abstain = Yb2LAbstainReason::MinFill;
                return result;
            }
            ++result.fill_adds;
            result.fill_add_price_scale_moves += first_add->price_scale_moved;
            T ledgered_lp = first_add->minted;
            if (std::fabs(ledgered_lp - chosen.input) >
                lp_reconciliation_tolerance(chosen.input)) {
                rollback_route(true, false);
                result.abstain = Yb2LAbstainReason::MinFill;
                return result;
            }

            Decision postadd = compute_decision(
                pool, external_cex_price, timestamp, costs
            );
            if (postadd.hard_abstain != Yb2LAbstainReason::None ||
                postadd.chosen_idx != 1 || !postadd.p1.valid) {
                rollback_route(true, true);
                result.abstain = Yb2LAbstainReason::MinFill;
                return result;
            }
            decision = postadd;
            chosen = postadd.p1;
            result.lp_oracle = decision.oracle;

            const T delta = chosen.input - ledgered_lp;
            const T target_tolerance = lp_reconciliation_tolerance(chosen.input);
            if (delta > target_tolerance) {
                const auto correction = add_liquidity_to_mint(
                    pool, delta
                );
                if (!correction.has_value()) {
                    rollback_route(true, true);
                    result.abstain = Yb2LAbstainReason::MinFill;
                    return result;
                }
                ++result.fill_adds;
                result.fill_add_price_scale_moves +=
                    correction->price_scale_moved;
                ledgered_lp += correction->minted;
            } else if (delta < -target_tolerance) {
                if (!remove_liquidity_leg(pool, -delta)) {
                    rollback_route(true, true);
                    result.abstain = Yb2LAbstainReason::MinFill;
                    return result;
                }
                ++result.fill_removes;
                ledgered_lp += delta;
            }
            if (std::fabs(ledgered_lp - chosen.input) >
                lp_reconciliation_tolerance(chosen.input)) {
                rollback_route(true, true);
                result.abstain = Yb2LAbstainReason::MinFill;
                return result;
            }
        }

        advance_debt(timestamp);
        const T debt_before_fill = state_.debt;
        const T cash_before_fill = state_.stable_balance;
        state_.debt = chosen.debt;
        state_.collateral = chosen.collateral;
        state_.stable_balance = chosen.stable_balance;
        state_.minted = chosen.minted;
        state_.redeemed = chosen.redeemed;
        const T debt_delta = std::fabs(state_.debt - debt_before_fill);
        const T cash_delta = std::fabs(state_.stable_balance - cash_before_fill);
        const T ledger_tol = T(32) * std::numeric_limits<T>::epsilon()
            * std::max({T(1), debt_delta, cash_delta});
        assert(std::fabs(debt_delta - cash_delta) <= ledger_tol);
        (void)ledger_tol;

        result.fired = true;
        result.direction = chosen.direction;
        result.input = chosen.input;
        result.output = chosen.output;
        result.net_profit = chosen.net_profit;
        result.fair_lp = chosen.fair_lp;
        result.levamm_price_before = decision.levamm_raw;
        result.target_price = chosen.target_price;
        if (const auto final_x0 = x0(
                decision.oracle, state_.collateral, state_.debt, false
            )) {
            result.levamm_price_after =
                (*final_x0 - state_.debt) / state_.collateral;
        }
        ++fires_;


        Yb2LActor donation_candidate = *this;
        donation_candidate.accrue_interest(timestamp);
        const T donation = donation_candidate.state_.lt_stable_balance;
        if (donation > T(0)) {
            result.donation = donation;
            result.donation_min_mint = (one() - state_.lt_donation_discount)
                * donation / pool.lp_price_at(timestamp);
            result.price_scale_before_donation = pool.cached_price_scale;
            result.virtual_price_before_donation = pool.get_virtual_price();
            result.xcp_profit_before_donation = pool.xcp_profit;
            std::string rejection;
            try {
                const auto minted = pool.try_add_donation(
                    {donation, T(0)}, result.donation_min_mint, rejection
                );
                if (!minted.has_value()) {
                    result.donation_reject =
                        classify_donation_reject(rejection);
                    rollback_route(false, false);
                    return result;
                }
            } catch (...) {
                result.donation_reject =
                    Yb2LDonationReject::TweakThrow;
                rollback_route(false, false);
                return result;
            }

            donation_candidate.state_.lt_stable_balance = T(0);
            donation_candidate.donated_interest_total_ += donation;
            result.donation_committed = true;
            result.price_scale_after_donation = pool.cached_price_scale;
            result.virtual_price_after_donation = pool.get_virtual_price();
            result.xcp_profit_after_donation = pool.xcp_profit;
            result.donation_price_scale_moved =
                result.price_scale_after_donation !=
                    result.price_scale_before_donation;
        }

        if (result.direction == 0) {
            if (!remove_liquidity_leg(pool, result.output)) {
                rollback_route(true, false);
                result.donation_committed = false;
                result.donation_price_scale_moved = false;
                result.abstain = Yb2LAbstainReason::MinFill;
                return result;
            }
            ++result.fill_removes;
        }

        *this = std::move(donation_candidate);
        return result;
    }

private:
    struct FillProposal {
        bool valid{false};
        size_t direction{0};
        Yb2LAbstainReason reason{Yb2LAbstainReason::NoBandEdge};
        T input{};
        T output{};
        T net_profit{};
        T fair_lp{};
        T target_price{};
        T collateral{};
        T debt{};
        T stable_balance{};
        T minted{};
        T redeemed{};
    };

    struct Decision {
        Yb2LAbstainReason hard_abstain{
            Yb2LAbstainReason::None
        };
        int chosen_idx{-1};
        T oracle{};
        T levamm_raw{};
        FillProposal p2;
        FillProposal p1;
    };

    struct AddLegResult {
        T minted{};
        bool price_scale_moved{false};
    };

    State state_{};
    bool enabled_{false};
    uint64_t fires_{0};
    T initial_unsettled_interest_{};
    T accrued_interest_total_{};
    T donated_interest_total_{};
    T shadow_gap_max_{};
    T shadow_gap_last_{};
    uint64_t shadow_checks_{0};
    uint64_t shadow_violations_{0};
    uint64_t shadow_abstains_{0};

    template <typename Pool>
    std::optional<AddLegResult> add_liquidity_to_mint(
        Pool& pool,
        const T& lp_target
    ) const {
        if (!(lp_target > T(0)) || !(pool.totalSupply > T(0))) {
            return std::nullopt;
        }
        try {
            const T fraction = lp_target / pool.totalSupply;
            std::array<T, 2> amounts{
                pool.balances[0] * fraction,
                pool.balances[1] * fraction,
            };
            Pool probe = pool;
            const T probe_minted =
                probe.add_liquidity(amounts, T(0), false);
            if (!(probe_minted > T(0))) return std::nullopt;
            const T scale = lp_target / probe_minted;
            amounts[0] *= scale;
            amounts[1] *= scale;
            const T ps_before = pool.cached_price_scale;
            const T minted = pool.add_liquidity(amounts, T(0), false);
            if (!(minted > T(0))) return std::nullopt;
            return AddLegResult{
                minted,
                pool.cached_price_scale != ps_before,
            };
        } catch (...) {
            return std::nullopt;
        }
    }

    template <typename Pool>
    bool remove_liquidity_leg(Pool& pool, const T& lp_amount) const {
        if (!(lp_amount > T(0))) return true;
        const T burnable = pool.totalSupply - pool.donation_shares
            - PoolTraits::MINIMUM_LIQUIDITY();
        if (lp_amount > burnable) return false;
        try {
            (void)pool.remove_liquidity(lp_amount, {T(0), T(0)});
            return true;
        } catch (...) {
            return false;
        }
    }

    static T lp_reconciliation_tolerance(const T& target) {
        return std::max(
            T(1e-9L) * target,
            T(64) * std::numeric_limits<T>::epsilon()
                * std::max(T(1), target)
        );
    }

    template <typename Pool>
    Decision compute_decision(
        const Pool& pool,
        const T& external_cex_price,
        uint64_t timestamp,
        const Costs& costs
    ) const {
        Decision decision;
        const T debt = projected_debt(timestamp);
        const T oracle = lp_oracle(pool);
        decision.oracle = oracle;
        const auto old_x0 = x0(oracle, state_.collateral, debt, false);
        if (!old_x0.has_value()) {
            decision.hard_abstain =
                Yb2LAbstainReason::NegativeDiscriminant;
            return decision;
        }
        const T x = *old_x0 - debt;
        if (!(x > T(0)) || !(state_.collateral > T(0)) ||
            !(pool.totalSupply > T(0)) || !(state_.fee >= T(0)) ||
            !(state_.fee < one())) {
            decision.hard_abstain = Yb2LAbstainReason::InvalidState;
            return decision;
        }

        const T fair_cex = external_cex_price * (
            T(1) + costs.market_basis_bps / T(10000)
        );
        const T bid = fair_cex * (
            T(1) - costs.execution_bps[0] / T(10000)
        );
        const T ask = fair_cex * (
            T(1) + costs.execution_bps[1] / T(10000)
        );
        if (!(bid > T(0)) || !(ask > T(0))) {
            decision.hard_abstain = Yb2LAbstainReason::InvalidState;
            return decision;
        }

        const T fair_lp_bid = (
            pool.balances[0] + bid * pool.balances[1]
        ) / pool.totalSupply;
        const T fair_lp_ask = (
            pool.balances[0] + ask * pool.balances[1]
        ) / pool.totalSupply;
        const T fee_factor = one() - state_.fee;
        const T add_fee = expected_p1_add_fee(pool, timestamp);
        const T effective_p1_fair = add_fee < one()
            ? fair_lp_ask / (one() - add_fee)
            : std::numeric_limits<T>::infinity();
        const T fixed0 = costs.transaction_coin0 + costs.leg_coin0[0];
        const T fixed1 = costs.transaction_coin0 + costs.leg_coin0[1];
        const T band_cost0 = fixed0 + T(YB_2L_MIN_PROFIT_COIN0);
        const T band_cost1 = fixed1 + T(YB_2L_MIN_PROFIT_COIN0);
        const T reference0 = pool.balances[0] * T(0.02L);
        const T reference1 = pool.balances[1] * ask * T(0.02L);
        const T fixed_frac0 = reference0 > T(0)
            ? band_cost0 / reference0 : T(0);
        const T fixed_frac1 = reference1 > T(0)
            ? band_cost1 / reference1 : T(0);
        const T bid_edge = fair_lp_bid * std::max(T(0), one() - fixed_frac0);
        const T ask_edge = effective_p1_fair * (one() + fixed_frac1);

        const T levamm_raw = x / state_.collateral;
        decision.levamm_raw = levamm_raw;
        const T executable_p2 = levamm_raw / fee_factor;
        const T executable_p1 = levamm_raw * fee_factor;
        if (executable_p2 < bid_edge) {
            decision.p2 = propose_fill(
                0, oracle, *old_x0, debt, x, bid_edge * fee_factor,
                fair_lp_bid, fixed0
            );
        }
        if (executable_p1 > ask_edge) {
            decision.p1 = propose_fill(
                1, oracle, *old_x0, debt, x, ask_edge / fee_factor,
                effective_p1_fair, fixed1
            );
        }
        if (decision.p2.valid && decision.p1.valid) {
            decision.chosen_idx =
                decision.p2.net_profit >= decision.p1.net_profit ? 0 : 1;
        } else if (decision.p2.valid) {
            decision.chosen_idx = 0;
        } else if (decision.p1.valid) {
            decision.chosen_idx = 1;
        }
        return decision;
    }

    explicit Yb2LActor(State state)
        : state_(std::move(state)), enabled_(true) {
        initial_unsettled_interest_ = pending_interest()
            + state_.lt_stable_balance;
    }

    static T one() { return PoolTraits::PRECISION(); }

    static Yb2LAbstainReason strongest_reason(
        Yb2LAbstainReason a,
        Yb2LAbstainReason b
    ) {
        if (a != Yb2LAbstainReason::NoBandEdge &&
            a != Yb2LAbstainReason::None) return a;
        if (b != Yb2LAbstainReason::NoBandEdge &&
            b != Yb2LAbstainReason::None) return b;
        return Yb2LAbstainReason::NoBandEdge;
    }

    std::optional<T> x0(
        const T& oracle,
        const T& collateral,
        const T& debt,
        bool safe
    ) const {
        const T coll_value = oracle * collateral / one();
        if (!(coll_value > T(0)) || !(collateral > T(0)) || !(debt >= T(0))) {
            return std::nullopt;
        }
        if (safe && (
            debt < coll_value * state_.min_safe_debt_ratio / one() ||
            debt > coll_value * state_.max_safe_debt_ratio / one()
        )) {
            return std::nullopt;
        }
        const T disc = coll_value * coll_value
            - T(4) * coll_value * state_.lev_ratio / one() * debt;
        if (!(disc >= T(0))) return std::nullopt;
        return (coll_value + std::sqrt(disc)) * one()
            / (T(2) * state_.lev_ratio);
    }

    FillProposal propose_fill(
        size_t direction,
        const T& oracle,
        const T& old_x0,
        const T& debt,
        const T& x,
        const T& target_price,
        const T& fair_lp,
        const T& fixed_cost
    ) const {
        FillProposal out;
        out.direction = direction;
        out.reason = Yb2LAbstainReason::InvalidState;
        out.target_price = target_price;
        out.fair_lp = fair_lp;
        if (!(target_price > T(0)) || !(fair_lp > T(0))) return out;
        const T inv = x * state_.collateral;
        const T x_target = std::sqrt(inv * target_price);
        const T c_target = x_target / target_price;
        const T fee_factor = one() - state_.fee;
        const T pending = debt + state_.redeemed > state_.minted
            ? debt + state_.redeemed - state_.minted
            : T(0);

        out.debt = debt;
        out.collateral = state_.collateral;
        out.stable_balance = state_.stable_balance;
        out.minted = state_.minted;
        out.redeemed = state_.redeemed;
        if (direction == 0) {
            if (!(x_target > x) || !(c_target < state_.collateral)) {
                out.reason = Yb2LAbstainReason::NoBandEdge;
                return out;
            }
            out.input = x_target - x;
            out.output = (state_.collateral - c_target) * fee_factor / one();
            if (out.input > debt) {
                out.reason = Yb2LAbstainReason::DebtFloor;
                return out;
            }
            out.debt = debt - out.input;
            out.collateral = state_.collateral - out.output;
            out.redeemed = state_.redeemed + out.input;
            out.stable_balance = state_.stable_balance + out.input;
            out.net_profit = out.output * fair_lp - out.input - fixed_cost;
        } else {
            if (!(x_target < x) || !(c_target > state_.collateral)) {
                out.reason = Yb2LAbstainReason::NoBandEdge;
                return out;
            }
            out.input = c_target - state_.collateral;
            out.output = (x - x_target) * fee_factor / one();
            if (out.output > state_.stable_balance) {
                out.reason = Yb2LAbstainReason::StableCash;
                return out;
            }
            out.debt = debt + out.output;
            out.collateral = state_.collateral + out.input;
            out.minted = state_.minted + out.output;
            out.stable_balance = state_.stable_balance - out.output;
            out.net_profit = out.output - out.input * fair_lp - fixed_cost;
        }

        out.minted = out.debt + out.redeemed - pending;
        if (!(out.input > T(0)) || !(out.output > T(0)) ||
            !(out.net_profit > T(YB_2L_MIN_PROFIT_COIN0))) {
            out.reason = Yb2LAbstainReason::MinFill;
            return out;
        }

        const T before_ratio = debt > T(0)
            ? oracle * state_.collateral / debt
            : std::numeric_limits<T>::max();
        const T after_ratio = out.debt > T(0)
            ? oracle * out.collateral / out.debt
            : std::numeric_limits<T>::max();
        bool check_state = true;
        if ((after_ratio > T(2) * one() && before_ratio > after_ratio) ||
            (after_ratio <= T(2) * one() && before_ratio < after_ratio)) {
            check_state = false;
        }
        const auto final_x0 = x0(
            oracle, out.collateral, out.debt, check_state
        );
        if (!final_x0.has_value()) {
            out.reason = Yb2LAbstainReason::UnsafeDebt;
            return out;
        }
        if (*final_x0 < old_x0) {
            out.reason = Yb2LAbstainReason::BadFinalState;
            return out;
        }
        out.valid = true;
        out.reason = Yb2LAbstainReason::None;
        return out;
    }

    template <typename Pool>
    T expected_p1_add_fee(const Pool& pool, uint64_t timestamp) const {
        const T fee_prime = pool.fee({
            pool.balances[0] * pool.precisions[0],
            pool.balances[1] * pool.precisions[1]
                * pool.cached_price_scale / one(),
        }) / T(2);
        T spam{};
        if (pool.donation_protection_expiry_ts > T(timestamp) &&
            pool.donation_protection_period > T(0) &&
            pool.totalSupply > T(0) &&
            pool.donation_shares_max_ratio > T(0)) {
            const T protection = std::min(
                (pool.donation_protection_expiry_ts - T(timestamp))
                    / pool.donation_protection_period,
                one()
            );
            spam = std::min(
                fee_prime,
                protection * fee_prime * pool.donation_shares
                    / pool.totalSupply / pool.donation_shares_max_ratio
            );
        }
        return PoolTraits::NOISE_FEE() + spam;
    }

    void advance_debt(uint64_t timestamp) {
        if (timestamp < state_.rate_time) return;
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

    static Yb2LDonationReject classify_donation_reject(
        const std::string& rejection
    ) {
        if (rejection == "donation above cap") {
            return Yb2LDonationReject::Cap;
        }
        if (rejection == "slippage") {
            return Yb2LDonationReject::MinMint;
        }
        if (rejection == "nothing minted") {
            return Yb2LDonationReject::NothingMinted;
        }
        return Yb2LDonationReject::TweakThrow;
    }
};

} // namespace arb::harness
