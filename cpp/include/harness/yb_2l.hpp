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
    T stable_aggregator{T(1)};  // crvUSD in aggregated USD: the LEVAMM oracle's factor
};

template <typename T>
struct Yb2LCosts {
    std::array<T, 2> execution_bps{T(5), T(5)};
    // A fill must clear this net coin0 profit (session yb_min_net_profit_coin0).
    T min_profit_coin0{T(YB_2L_MIN_PROFIT_COIN0)};
};

template <typename T>
struct Yb2LResult {
    bool fired{false};
    size_t direction{0};
    T input{};
    T output{};
    T net_profit{};
    T donation{};
    T price_scale_after_donation{};
    bool donation_committed{false};
    bool donation_price_scale_moved{false};
    size_t fill_adds{0};
    size_t fill_removes{0};
    size_t fill_add_price_scale_moves{0};
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
        state.stable_aggregator = initial.stable_aggregator;
        return Yb2LActor(std::move(state));
    }

    bool enabled() const { return enabled_; }
    const State& state() const { return state_; }

    template <typename Pool>
    T lp_oracle(const Pool& pool) const {
        // CryptopoolLPOracle.price(): the LP price times the seeded aggregator.
        const T sqrt_scale = std::sqrt(pool.cached_price_scale * one());
        return T(2) * pool.get_virtual_price() * sqrt_scale / one() * state_.stable_aggregator / one();
    }

    T projected_debt(uint64_t timestamp) const {
        if (!enabled_ || timestamp < state_.rate_time) return T(0);
        const T next_mul = state_.rate_mul * (
            one() + state_.rate * T(timestamp - state_.rate_time)
        ) / one();
        return state_.debt * next_mul / state_.rate_mul;
    }

    // Conservative no-trade band: dropping nonnegative fixed/add costs widens
    // both executable opportunities. Uncertain inputs must reach full sizing.
    template <typename Pool>
    bool may_trade(const Pool& pool, const T& external_price, uint64_t timestamp,
                   const Costs& costs) const {
        if (!enabled_ || state_.killed || timestamp < state_.rate_time) return false;
        const auto q = marginal_quote(pool, external_price, timestamp, costs);
        if (q.hard_abstain) return false;
        const T raw = q.x / state_.collateral;
        const T factor = one() - state_.fee;
        if (!std::isfinite(raw) || !std::isfinite(q.fair_bid) || !std::isfinite(q.fair_ask)) return true;
        return raw / factor < q.fair_bid || raw * factor > q.fair_ask;
    }

    // Prices where may_trade is false at every timestamp in [from, to] for the current pool
    // and actor state. Debt only grows with time, so the LEVAMM price x / collateral only
    // falls: its value at `from` bounds the ask side and at `to` the bid side. The band is
    // shrunk by a relative margin so that rounding near an edge still reaches may_trade.
    struct NoTradeBand {
        bool valid{false};
        uint64_t from{0}, to{0};
        T lo{}, hi{};
    };
    template <typename Pool>
    NoTradeBand no_trade_band(const Pool& pool, const T& price, uint64_t from, uint64_t to,
                              const Costs& costs) const {
        NoTradeBand band{false, from, to, T(0), T(0)};
        if (!enabled_ || state_.killed || from < state_.rate_time || to < from) return band;
        const auto early = marginal_quote(pool, T(1), from, costs);
        const auto late = marginal_quote(pool, T(1), to, costs);
        if (early.hard_abstain || late.hard_abstain) return band;
        const T factor = one() - state_.fee;
        // At unit external price the quote's bid and ask are the per-price execution factors.
        const T coin1 = pool.balances[1], supply = pool.totalSupply;
        T hi = (supply * (late.x / state_.collateral) / factor - pool.balances[0]) / (coin1 * early.bid);
        T lo = (supply * (early.x / state_.collateral) * factor - pool.balances[0]) / (coin1 * early.ask);
        if (!(coin1 > T(0)) || !(early.bid > T(0)) || !(early.ask > T(0)) || !std::isfinite(lo) || !std::isfinite(hi))
            return band;
        band.lo = lo + std::fabs(lo) * T(1e-9);
        band.hi = hi - std::fabs(hi) * T(1e-9);
        band.valid = band.lo < band.hi;
        return band;
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
            return result;
        }

        Decision decision = compute_decision(
            pool, external_cex_price, timestamp, costs
        );
        if (decision.hard_abstain || decision.chosen_idx < 0) return result;
        FillProposal chosen = decision.chosen_idx == 0
            ? decision.p2 : decision.p1;

        const T real_circulating = pool.totalSupply - pool.donation_shares
            - PoolTraits::MINIMUM_LIQUIDITY();
        const T expected_collateral = real_circulating;
        const T gap = state_.collateral - expected_collateral;
        const T abs_gap = std::fabs(gap);
        if (abs_gap > T(1e-2)) return result;

        Yb2LActor pre_attempt_actor = *this;
        PoolTransactionSnapshot<Pool> pre_attempt_pool(pool);

        const auto rollback_route = [&] {
            pre_attempt_pool.restore(pool);
            *this = std::move(pre_attempt_actor);
            result.fired = false;
        };

        if (chosen.direction == 1) {
            const auto first_add = add_liquidity_to_mint(
                pool, chosen.input
            );
            if (!first_add.has_value()) {
                rollback_route();
                return result;
            }
            ++result.fill_adds;
            result.fill_add_price_scale_moves += first_add->price_scale_moved;
            T ledgered_lp = first_add->minted;
            if (std::fabs(ledgered_lp - chosen.input) >
                lp_reconciliation_tolerance(chosen.input)) {
                rollback_route();
                return result;
            }

            Decision postadd = compute_decision(
                pool, external_cex_price, timestamp, costs
            );
            if (postadd.hard_abstain ||
                postadd.chosen_idx != 1 || !postadd.p1.valid) {
                rollback_route();
                return result;
            }
            chosen = postadd.p1;

            const T delta = chosen.input - ledgered_lp;
            const T target_tolerance = lp_reconciliation_tolerance(chosen.input);
            if (delta > target_tolerance) {
                const auto correction = add_liquidity_to_mint(
                    pool, delta
                );
                if (!correction.has_value()) {
                    rollback_route();
                    return result;
                }
                ++result.fill_adds;
                result.fill_add_price_scale_moves +=
                    correction->price_scale_moved;
                ledgered_lp += correction->minted;
            } else if (delta < -target_tolerance) {
                if (!remove_liquidity_leg(pool, -delta)) {
                    rollback_route();
                    return result;
                }
                ++result.fill_removes;
                ledgered_lp += delta;
            }
            if (std::fabs(ledgered_lp - chosen.input) >
                lp_reconciliation_tolerance(chosen.input)) {
                rollback_route();
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

        Yb2LActor donation_candidate = *this;
        if (!donate_interest(pool, timestamp, result, donation_candidate)) {
            rollback_route();
            return result;
        }

        if (result.direction == 0) {
            if (!remove_liquidity_leg(pool, result.output)) {
                rollback_route();
                result.donation_committed = false;
                result.donation_price_scale_moved = false;
                return result;
            }
            ++result.fill_removes;
        }

        *this = std::move(donation_candidate);
        return result;
    }

    // LEVAMM get_p: (x0 - debt) / collateral at the pool's LP oracle.
    template <typename Pool>
    T levamm_price(const Pool& pool, uint64_t timestamp) const {
        const T debt = projected_debt(timestamp);
        const auto value = x0(lp_oracle(pool), state_.collateral, debt, false);
        return value && state_.collateral > T(0) ? (*value - debt) / state_.collateral : T(0);
    }

private:
    struct MarginalQuote {
        bool hard_abstain{false};
        T oracle{}, debt{}, old_x0{}, x{}, fair_bid{}, fair_ask{}, bid{}, ask{};
    };

    struct FillProposal {
        bool valid{false};
        size_t direction{0};
        T input{};
        T output{};
        T net_profit{};
        T collateral{};
        T debt{};
        T stable_balance{};
        T minted{};
        T redeemed{};
    };

    struct Decision {
        bool hard_abstain{false};
        int chosen_idx{-1};
        FillProposal p2;
        FillProposal p1;
    };

    struct AddLegResult {
        T minted{};
        bool price_scale_moved{false};
    };

    State state_{};
    bool enabled_{false};

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
    MarginalQuote marginal_quote(
        const Pool& pool,
        const T& external_cex_price,
        uint64_t timestamp,
        const Costs& costs
    ) const {
        MarginalQuote decision;
        const T debt = projected_debt(timestamp);
        const T oracle = lp_oracle(pool);
        decision.oracle = oracle;
        const auto old_x0 = x0(oracle, state_.collateral, debt, false);
        if (!old_x0.has_value()) {
            decision.hard_abstain = true;
            return decision;
        }
        const T x = *old_x0 - debt;
        if (!(x > T(0)) || !(state_.collateral > T(0)) ||
            !(pool.totalSupply > T(0)) || !(state_.fee >= T(0)) ||
            !(state_.fee < one())) {
            decision.hard_abstain = true;
            return decision;
        }

        const T bid = external_cex_price * (
            T(1) - costs.execution_bps[0] / T(10000)
        );
        const T ask = external_cex_price * (
            T(1) + costs.execution_bps[1] / T(10000)
        );
        if (!(bid > T(0)) || !(ask > T(0))) {
            decision.hard_abstain = true;
            return decision;
        }

        const T fair_lp_bid = (
            pool.balances[0] + bid * pool.balances[1]
        ) / pool.totalSupply;
        const T fair_lp_ask = (
            pool.balances[0] + ask * pool.balances[1]
        ) / pool.totalSupply;
        decision.debt = debt;
        decision.old_x0 = *old_x0;
        decision.x = x;
        decision.fair_bid = fair_lp_bid;
        decision.fair_ask = fair_lp_ask;
        decision.bid = bid;
        decision.ask = ask;
        return decision;
    }

    template <typename Pool>
    Decision compute_decision(
        const Pool& pool, const T& external_cex_price, uint64_t timestamp,
        const Costs& costs
    ) const {
        const auto quote = marginal_quote(pool, external_cex_price, timestamp, costs);
        Decision decision;
        decision.hard_abstain = quote.hard_abstain;
        if (decision.hard_abstain) return decision;
        const T debt = quote.debt, oracle = quote.oracle, x = quote.x;
        const T fair_lp_bid = quote.fair_bid, fair_lp_ask = quote.fair_ask;
        const T fee_factor = one() - state_.fee;
        const T add_fee = expected_p1_add_fee(pool, timestamp);
        const T effective_p1_fair = add_fee < one()
            ? fair_lp_ask / (one() - add_fee)
            : std::numeric_limits<T>::infinity();
        const T reference0 = pool.balances[0] * T(0.02L);
        const T reference1 = pool.balances[1] * quote.ask * T(0.02L);
        const T fixed_frac0 = reference0 > T(0)
            ? costs.min_profit_coin0 / reference0 : T(0);
        const T fixed_frac1 = reference1 > T(0)
            ? costs.min_profit_coin0 / reference1 : T(0);
        const T bid_edge = fair_lp_bid * std::max(T(0), one() - fixed_frac0);
        const T ask_edge = effective_p1_fair * (one() + fixed_frac1);

        const T levamm_raw = x / state_.collateral;
        const T executable_p2 = levamm_raw / fee_factor;
        const T executable_p1 = levamm_raw * fee_factor;
        if (executable_p2 < bid_edge) {
            decision.p2 = propose_fill(
                0, oracle, quote.old_x0, debt, x, bid_edge * fee_factor,
                fair_lp_bid, costs.min_profit_coin0
            );
        }
        if (executable_p1 > ask_edge) {
            decision.p1 = propose_fill(
                1, oracle, quote.old_x0, debt, x, ask_edge / fee_factor,
                effective_p1_fair, costs.min_profit_coin0
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

    // Accrue interest into donation_candidate and donate it to the pool. A
    // rejected donation is reported; the caller rolls its route back.
    template <typename Pool>
    bool donate_interest(Pool& pool, uint64_t timestamp, Yb2LResult<T>& result,
                         Yb2LActor& donation_candidate) const {
        donation_candidate.accrue_interest(timestamp);
        const T donation = donation_candidate.state_.lt_stable_balance;
        // LT donates any positive integer balance. A second distribution in the same block collects no interest:
        // its float residue (far below 1e-6 crvUSD; real per-block interest is >= ~0.05) is that zero, kept.
        if (!(donation > T(1e-6))) return true;
        result.donation = donation;
        // The minimum mint keeps a 1e-6 relative margin: the float pool reproduces the chain's state to ~3e-7.
        const T min_mint = (one() - T(1e-6)) * (one() - state_.lt_donation_discount) / one()
            * donation / pool.lp_price_at(timestamp);  // the registered binaries' expression, bit for bit
        const T price_scale_before_donation = pool.cached_price_scale;
        try {
            if (!pool.try_add_donation({donation, T(0)}, min_mint).has_value()) return false;
        } catch (...) {
            return false;
        }
        donation_candidate.state_.lt_stable_balance = T(0);
        result.donation_committed = true;
        result.price_scale_after_donation = pool.cached_price_scale;
        result.donation_price_scale_moved =
            result.price_scale_after_donation != price_scale_before_donation;
        return true;
    }

    explicit Yb2LActor(State state)
        : state_(std::move(state)), enabled_(true) {}

    static T one() { return PoolTraits::PRECISION(); }

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
        const T& min_profit
    ) const {
        FillProposal out;
        out.direction = direction;
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
                return out;
            }
            out.input = x_target - x;
            out.output = (state_.collateral - c_target) * fee_factor / one();
            if (out.input > debt) {
                return out;
            }
            out.debt = debt - out.input;
            out.collateral = state_.collateral - out.output;
            out.redeemed = state_.redeemed + out.input;
            out.stable_balance = state_.stable_balance + out.input;
            out.net_profit = out.output * fair_lp - out.input;
        } else {
            if (!(x_target < x) || !(c_target > state_.collateral)) {
                return out;
            }
            out.input = c_target - state_.collateral;
            out.output = (x - x_target) * fee_factor / one();
            if (out.output > state_.stable_balance) {
                return out;
            }
            out.debt = debt + out.output;
            out.collateral = state_.collateral + out.input;
            out.minted = state_.minted + out.output;
            out.stable_balance = state_.stable_balance - out.output;
            out.net_profit = out.output - out.input * fair_lp;
        }

        out.minted = out.debt + out.redeemed - pending;
        if (!(out.input > T(0)) || !(out.output > T(0)) ||
            !(out.net_profit > min_profit)) {
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
            return out;
        }
        if (*final_x0 < old_x0) {
            return out;
        }
        out.valid = true;
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
        if (interest > state_.stable_balance) interest = std::max(T(0), state_.stable_balance);
        state_.minted += interest;
        state_.stable_balance -= interest;
        state_.lt_stable_balance += interest;
    }

};

} // namespace arb::harness
