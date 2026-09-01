// Arbitrage decision logic (floating-point only)
// Currently twocrypto_fx specific - will refactor when adding other pool types
#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

#include "pools/twocrypto_fx/helpers.hpp"
#include "trading/costs.hpp"

namespace arb {
namespace trading {

namespace fx = arb::pools::twocrypto_fx;

// Result of a trade-sizing decision (sole producer: decide_trade below).
template <typename T>
struct Decision {
    bool do_trade{false};
    int i{0};
    int j{1};
    T dx{};
    T dy_after_fee{};
    T profit{};
    T fee_tokens{};
    T notional_coin0{};
};

inline constexpr int NUMERIC_SEARCH_ITERS = 24;   // legacy golden-section iters
inline constexpr int MAX_SIZING_EVALS = 24;       // hard eval budget per decision

template <typename T, typename PoolT>
Decision<T> decide_trade(
    const PoolT& pool,
    T cex_price,
    const Costs<T>& costs,
    T volume_cap,
    T min_swap_frac,
    T max_swap_frac,
    T cex_fee_discount,
    T cex_fee_markup,
    const T* p_now_hint = nullptr,
    const T* fee_hint = nullptr,
    const std::array<T, 2>* xp_hint = nullptr
) {
    static_assert(std::is_floating_point_v<T>, "decide_trade is floating-only");

    Decision<T> d{};

    if (!(cex_price > T(0))) return d;

    T p_now;
    T fee_pool;
    if (p_now_hint != nullptr && fee_hint != nullptr) {
        p_now = *p_now_hint;
        fee_pool = *fee_hint;
    } else {
        const auto xp_now = fx::pool_xp_current(pool);
        p_now = fx::MathOps<T>::get_p(xp_now, pool.D, {pool.A, pool.gamma}) * pool.cached_price_scale;
        fee_pool = pool.fee(xp_now);
    }
    if (!(p_now > T(0))) return d;

    const T one_minus_f0 = std::max(T(1) - fee_pool, T(1e-12));
    const T p_cex_bid = cex_fee_discount * cex_price;
    const T p_cex_ask = cex_fee_markup * cex_price;

    // Per-direction path-floor fee: the lowest fee any size of trade in
    // that direction can pay. For the native fee with the conventional
    // ordering (mid <= out) this is exact: trading away from balance the
    // fee only rises with size (floor = spot fee); toward balance it can
    // dip to min(mid, out). With an inverted config (out < mid) the fee
    // FALLS away from balance, so the spot fee is not a floor in either
    // direction and both sides use the global floor. Policy fees use the
    // config-derived floor both ways.
    const T fee_floor = pool.fee_lower_bound();
    // Callers may provide the exact xp value cached with p_now/fee. The cache
    // is invalidated after every pool mutation, so this avoids only a duplicate
    // pure conversion and does not change the sizing surface.
    const std::array<T, 2> xp_bal = xp_hint != nullptr
        ? *xp_hint
        : fx::pool_xp_current(pool);
    const bool native_fee_mode = pool.uses_native_fee_model();
    const bool fee_ordered = !(pool.out_fee < pool.mid_fee);
    const T away_floor = fee_ordered ? fee_pool : fee_floor;
    const T floor_fee_01 = (!native_fee_mode || xp_bal[0] < xp_bal[1]) ? fee_floor : away_floor;
    const T floor_fee_10 = (!native_fee_mode || xp_bal[1] < xp_bal[0]) ? fee_floor : away_floor;

    // Gate fee: the gate must never exceed the direction's true path floor
    // (or profitable sizes get pre-filtered), so it gates on floor_fee.
    // For native pools this admits toward-balance trades whose only profit
    // is the fee dip at the balance crossing -- on steep fee surfaces
    // (small fee_gamma) those are the dominant arb mode, so admitting them
    // is a correctness requirement for fee research.
    const T gate_fee_01 = floor_fee_01;
    const T gate_fee_10 = floor_fee_10;

    // Required price-move ratios (rho > 1 means a profitable direction
    // exists at that fee level).
    const T rho_01_floor = std::max(T(1) - gate_fee_01, T(1e-12)) * p_cex_bid / p_now;
    const T rho_10_floor = std::max(T(1) - gate_fee_10, T(1e-12)) * p_now / p_cex_ask;

    if (rho_01_floor <= T(1) && rho_10_floor <= T(1)) return d;
    int sel_i = -1, sel_j = -1;
    if (rho_01_floor >= rho_10_floor) { sel_i = 0; sel_j = 1; } else { sel_i = 1; sel_j = 0; }
    d.i = sel_i;
    d.j = sel_j;

    const T avail = pool.balances[static_cast<size_t>(sel_i)];
    if (!(avail > T(0))) return d;

    // Sizing bounds
    T dx_lo = std::max(T(1e-18), avail * std::max(T(1e-12), min_swap_frac));
    T dx_hi = avail * max_swap_frac;

    if (std::isfinite(static_cast<double>(volume_cap)) && volume_cap > T(0)) {
        T cap = volume_cap;
        if (costs.volume_cap_is_coin1) {
            if (sel_i == 0) {
                cap *= cex_price; // coin1 -> coin0
            }
        } else {
            if (sel_i == 1) {
                cap /= cex_price; // coin0 -> coin1
            }
        }
        dx_hi = std::min(dx_hi, cap);
    }
    if (!(dx_hi > dx_lo)) {
        return d;
    }

    struct Candidate {
        T dx{};
        T dy_after_fee{};
        T profit{};
        T fee_tokens{};
    };

    const T value_coeff = (sel_i == 0) ? cex_price * cex_fee_discount : T(1);
    const T cost_coeff  = (sel_i == 0) ? T(1) : cex_price * cex_fee_markup;

    auto evaluate_candidate = [&](T dx) -> Candidate {
        auto sim = fx::simulate_exchange_once(pool, static_cast<size_t>(sel_i), static_cast<size_t>(sel_j), dx);
        const T profit = sim.first * value_coeff - dx * cost_coeff - costs.gas_coin0;
        return Candidate{dx, sim.first, profit, sim.second};
    };

#if defined(ARB_SIZING_GOLDEN)
    // Legacy profit maximizer: golden-section search within sizing bounds.
    constexpr T phi = static_cast<T>(0x1.3c6ef372fe95p-1);  // (sqrt(5) - 1) / 2
    T a = dx_lo;
    T b = dx_hi;
    Candidate c = evaluate_candidate(b - phi * (b - a));
    Candidate e = evaluate_candidate(a + phi * (b - a));
    for (int it = 0; it < NUMERIC_SEARCH_ITERS; ++it) {
        if (c.profit < e.profit) {
            a = c.dx;
            c = e;
            e = evaluate_candidate(a + phi * (b - a));
        } else {
            b = e.dx;
            e = c;
            c = evaluate_candidate(b - phi * (b - a));
        }
    }

    Candidate best = (c.profit > e.profit) ? c : e;
#else
    // ------------------------------------------------------------------
    // Scan-and-refine profit maximizer (derivative-free).
    //
    // The after-fee output is a black box of dx, so any fee structure --
    // xp-dependent, oracle/band-driven, even discontinuous -- is handled
    // without a derivative model:
    //   1. Seed the size scale with the closed-form first-order optimum
    //      for a frozen fee: dx ~ edge / lambda, where lambda is the
    //      relative price-impact rate at the current state.
    //   2. Cap the range with the concave floor-fee envelope (cheap
    //      fee-free samples), then evaluate a geometric ladder anchored at
    //      the seed: multi-modal profit shapes are caught at ladder
    //      resolution (factor-9 gaps).
    //   3. Refine inside the best bracket by successive parabolic
    //      interpolation with golden-section safeguards (Brent-style):
    //      superlinear on smooth fees, still convergent on kinks/jumps.
    // The answer is the best of *all* evaluations; total full evaluations
    // are capped at MAX_SIZING_EVALS = 24 by construction (ladder <= 20,
    // refine consumes the remainder; the legacy golden section spends 26).
    // Fee-free envelope probes are extra but ~half-cost (no policy call).
    // All stepping is multiplicative in dx (no log/exp calls).
    // ------------------------------------------------------------------
    int evals = 0;
    Candidate best{};
    best.profit = -std::numeric_limits<T>::infinity();
    auto eval_at = [&](T dx) -> Candidate {
        dx = std::min(std::max(dx, dx_lo), dx_hi);
        Candidate c = evaluate_candidate(dx);
        ++evals;
        if (c.profit > best.profit) best = c;
        return c;
    };

    // 1. Seed: required relative price move over the impact rate. Use the
    //    spot fee if it leaves an edge, else the direction's gate fee.
    T rho_seed = (sel_i == 0)
        ? one_minus_f0 * p_cex_bid / p_now
        : one_minus_f0 * p_now / p_cex_ask;
    if (!(rho_seed > T(1))) {
        rho_seed = (sel_i == 0) ? rho_01_floor : rho_10_floor;
    }
    const T eps_edge = rho_seed - T(1);
    const T lambda = fx::price_impact_rate(pool, static_cast<size_t>(sel_i), static_cast<size_t>(sel_j));

    T dx_seed = (lambda > T(0) && eps_edge > T(0))
        ? eps_edge / lambda
        : std::sqrt(dx_lo * dx_hi);
    dx_seed = std::min(std::max(dx_seed, dx_lo), dx_hi);

    // 2a. Rigorous size cap. With f the direction's path-floor fee,
    //     P_floor(dx) = (1-f)*value*dy(dx) - cost*dx - gas is concave and
    //     >= P(dx) for every admissible fee, so once a sample beyond the
    //     seed goes nonpositive, all larger sizes are dominated. The walk
    //     uses fee-free evaluations (one sqrt, no policy call).
    const T floor_fee_sel = (sel_i == 0) ? floor_fee_01 : floor_fee_10;
    const T floor_value_coeff =
        std::max(T(1) - floor_fee_sel, T(1e-12)) * value_coeff;
    auto p_floor_at = [&](T dx) -> T {
        dx = std::min(std::max(dx, dx_lo), dx_hi);
        const T b0 = pool.balances[0] + (sel_i == 0 ? dx : T(0));
        const T b1 = pool.balances[1] + (sel_i == 1 ? dx : T(0));
        const std::array<T, 2> xq{
            b0 * pool.precisions[0],
            b1 * pool.precisions[1] * pool.cached_price_scale / fx::PoolTraits<T>::PRECISION()
        };
        const T y = fx::MathOps<T>::get_y_unchecked(
            pool.A, pool.gamma, xq, pool.D, static_cast<size_t>(sel_j));
        const T dy_tokens = fx::xp_to_tokens_j(
            pool, static_cast<size_t>(sel_j), xq[static_cast<size_t>(sel_j)] - y,
            pool.cached_price_scale);
        return dy_tokens * floor_value_coeff - dx * cost_coeff - costs.gas_coin0;
    };

    T dx_cap = dx_hi;
    if (p_floor_at(dx_seed) > T(0)) {
        int guard = 0;
        for (T v = dx_seed * T(9); v < dx_hi && guard < 32; v *= T(9), ++guard) {
            if (!(p_floor_at(v) > T(0))) { dx_cap = v; break; }
        }
    } else {
        // Seed overshot the profitable window: walk down to re-anchor.
        bool found = false;
        int guard = 0;
        for (T v = dx_seed / T(9); guard < 32; v /= T(9), ++guard) {
            const T vq = std::max(v, dx_lo);
            if (p_floor_at(vq) > T(0)) {
                dx_cap = std::min(dx_seed, dx_hi);  // previous sample was <= 0
                dx_seed = vq;
                found = true;
                break;
            }
            if (vq <= dx_lo) break;
        }
        if (!found) {
            dx_cap = dx_hi;  // no positive envelope sample: scan everything
        }
    }

    // 2a'. Native fee-dip probe. The native fee has its minimum at the
    //      balance point; on steep surfaces (small fee_gamma) the
    //      profitable window is centered on the balance-crossing trade
    //      size, far from the frozen-fee seed and narrower than a ladder
    //      rung, so without an explicit probe the search misses it
    //      entirely (orders-of-magnitude shortfalls; see
    //      tests/test_sizing.cpp small-fee_gamma surfaces). The crossing
    //      sits where post-trade xp equalize: dxp ~ (xp_out - xp_in) / 2.
    T dx_dip = T(0);
    if (native_fee_mode && xp_bal[static_cast<size_t>(sel_i)] < xp_bal[static_cast<size_t>(sel_j)]) {
        const T dxp_in_ddx = (sel_i == 0)
            ? pool.precisions[0]
            : pool.precisions[1] * pool.cached_price_scale / fx::PoolTraits<T>::PRECISION();
        if (dxp_in_ddx > T(0)) {
            const T cand = (xp_bal[static_cast<size_t>(sel_j)] - xp_bal[static_cast<size_t>(sel_i)])
                / (T(2) * dxp_in_ddx);
            if (cand > dx_lo && cand < dx_cap) {
                dx_dip = cand;
            }
        }
    }

    // 2b. Ladder: seed/3 and 3*seed neighbors (plus the dip probe and its
    //     neighbors when present) plus factor-9 rungs anchored at the
    //     seed. Coverage below the seed: with ordered fees the native fee
    //     only rises away from its dip, so the optimum sits within a small
    //     factor of the seed or at the dip probe; arbitrary policy fees
    //     may hide discounts at any scale, so they get the full range down
    //     to dx_lo. Rung counts are bounded so the scan plus a minimal
    //     refine always fits the MAX_SIZING_EVALS budget (extreme ranges
    //     get coarser tails).
    const T low_cover = (native_fee_mode && fee_ordered)
        ? std::max(dx_lo, std::min(dx_seed, dx_dip > T(0) ? dx_dip : dx_seed) / T(27))
        : dx_lo;

    T pts[24];
    int n_pts = 0;
    pts[n_pts++] = low_cover;
    pts[n_pts++] = dx_cap;
    pts[n_pts++] = dx_seed;
    if (dx_seed / T(3) > low_cover) pts[n_pts++] = dx_seed / T(3);
    if (dx_seed * T(3) < dx_cap)    pts[n_pts++] = dx_seed * T(3);
    if (dx_dip > T(0)) {
        pts[n_pts++] = dx_dip;
        if (dx_dip / T(3) > low_cover) pts[n_pts++] = dx_dip / T(3);
        if (dx_dip * T(3) < dx_cap)    pts[n_pts++] = dx_dip * T(3);
    }
    for (T v = dx_seed / T(9); v > low_cover && n_pts < 14; v /= T(9)) pts[n_pts++] = v;
    for (T v = dx_seed * T(9); v < dx_cap && n_pts < 20; v *= T(9)) pts[n_pts++] = v;
    std::sort(pts, pts + n_pts);
    int m_pts = 0;
    for (int k = 0; k < n_pts; ++k) {
        if (m_pts == 0 || pts[k] > pts[m_pts - 1] * (T(1) + T(1e-9))) {
            pts[m_pts++] = pts[k];
        }
    }

    // The cap point is provably nonpositive when it came from the envelope
    // (dx_cap < dx_hi), so spend no full evaluation there: use the envelope
    // value itself as its bracket ordinate.
    const bool cap_is_hi = (dx_cap >= dx_hi * (T(1) - T(1e-9)));
    Candidate scan[24];
    int best_idx = 0;
    for (int k = 0; k < m_pts; ++k) {
        if (k == m_pts - 1 && !cap_is_hi && m_pts > 1) {
            scan[k] = Candidate{pts[k], T(0), p_floor_at(pts[k]), T(0)};
        } else {
            scan[k] = eval_at(pts[k]);
        }
        if (scan[k].profit > scan[best_idx].profit) best_idx = k;
    }

    // 3. Refine. Endpoint winners first probe their adjacent gap: if an
    //    interior point beats the endpoint we have a proper bracket,
    //    otherwise the endpoint stands (cap-bound trades, dust rejects).
    constexpr T TOL_REL = T(2e-3);             // relative size resolution
    constexpr T GINV = T(0.3819660112501051);  // 1 - 1/phi
    bool have_triplet = false;
    T ta{}, tb{}, tc{}, Pa{}, Pb{}, Pc{};
    if (best_idx > 0 && best_idx < m_pts - 1) {
        ta = pts[best_idx - 1]; tb = pts[best_idx]; tc = pts[best_idx + 1];
        Pa = scan[best_idx - 1].profit;
        Pb = scan[best_idx].profit;
        Pc = scan[best_idx + 1].profit;
        have_triplet = true;
    } else if (m_pts >= 2) {
        const bool at_hi = (best_idx == m_pts - 1);
        T inner_t = at_hi ? pts[m_pts - 2] : pts[1];
        T inner_P = at_hi ? scan[m_pts - 2].profit : scan[1].profit;
        T end_t = pts[best_idx];
        T end_P = scan[best_idx].profit;
        for (int k = 0; k < 3 && evals < MAX_SIZING_EVALS &&
                        std::fabs(end_t - inner_t) > TOL_REL * std::max(end_t, inner_t); ++k) {
            const T tp = end_t + GINV * (inner_t - end_t);
            Candidate cp = eval_at(tp);
            if (cp.profit > end_P) {
                // interior maximum: bracket it between inner and end
                if (at_hi) { ta = inner_t; Pa = inner_P; tb = tp; Pb = cp.profit; tc = end_t; Pc = end_P; }
                else       { ta = end_t; Pa = end_P; tb = tp; Pb = cp.profit; tc = inner_t; Pc = inner_P; }
                have_triplet = true;
                break;
            }
            inner_t = tp;  // probe lost: tighten toward the endpoint
            inner_P = cp.profit;
        }
    }
    auto spi_refine = [&](T ra, T rb, T rc, T RPa, T RPb, T RPc) {
        T dprev = rc - ra;
        T dprev2 = dprev;
        while (evals < MAX_SIZING_EVALS && rc - ra > TOL_REL * rb) {
            const T d1 = rb - ra;
            const T d2 = rc - rb;
            const T ga = RPb - RPa;   // >= 0
            const T gc = RPb - RPc;   // >= 0
            const T den = d1 * gc + d2 * ga;
            T tnew{};
            bool spi_ok = false;
            const T margin = TOL_REL * rb / T(4);
            if (den > T(0)) {
                const T tv = rb - (d1 * d1 * gc - d2 * d2 * ga) / (T(2) * den);
                const T step_len = std::fabs(tv - rb);
                if (tv > ra + margin && tv < rc - margin &&
                    step_len >= margin && step_len <= dprev2 / T(2)) {
                    tnew = tv;
                    spi_ok = true;
                }
            }
            if (!spi_ok) {
                tnew = (d1 > d2) ? rb - GINV * d1 : rb + GINV * d2;
            }
            dprev2 = dprev;
            dprev = std::fabs(tnew - rb);
            Candidate cn = eval_at(tnew);
            if (tnew < rb) {
                if (cn.profit > RPb) { rc = rb; RPc = RPb; rb = tnew; RPb = cn.profit; }
                else                 { ra = tnew; RPa = cn.profit; }
            } else {
                if (cn.profit > RPb) { ra = rb; RPa = RPb; rb = tnew; RPb = cn.profit; }
                else                 { rc = tnew; RPc = cn.profit; }
            }
        }
    };
    if (have_triplet) {
        spi_refine(ta, tb, tc, Pa, Pb, Pc);
    }

    // The fee-dip mode can be a sharp local peak that loses the SCAN
    // comparison to a broad mode elsewhere yet wins after refinement.
    // If the main refine settled far from the dip probe, spend a few of
    // the remaining evals refining the dip's own basin: shrink the bracket
    // until the dip point is its local max (the basin narrows with
    // fee_gamma, so a fixed bracket factor cannot work for all surfaces).
    if (dx_dip > T(0) && evals + 3 <= MAX_SIZING_EVALS &&
        !(best.dx > dx_dip / T(2) && best.dx < dx_dip * T(2))) {
        // Reuse the scan's value at the dip point.
        T pb_dip = T(0);
        bool have_pb = false;
        for (int k = 0; k < m_pts; ++k) {
            if (std::fabs(pts[k] - dx_dip) <= dx_dip * T(1e-9)) {
                pb_dip = scan[k].profit;
                have_pb = true;
                break;
            }
        }
        if (!have_pb && evals < MAX_SIZING_EVALS) {
            pb_dip = eval_at(dx_dip).profit;
            have_pb = true;
        }
        if (have_pb) {
            for (T factor : {T(1.5), T(1.15), T(1.05)}) {
                if (evals + 2 > MAX_SIZING_EVALS) break;
                const T da = std::max(low_cover, dx_dip / factor);
                const T dc = std::min(dx_cap, dx_dip * factor);
                if (!(da < dx_dip && dx_dip < dc)) continue;
                const T pa_dip = eval_at(da).profit;
                const T pc_dip = eval_at(dc).profit;
                if (pb_dip >= pa_dip && pb_dip >= pc_dip) {
                    spi_refine(da, dx_dip, dc, pa_dip, pb_dip, pc_dip);
                    break;
                }
            }
        }
    }

    // Native ladders skip the bottom of the range; if nothing positive was
    // found, probe the smallest size before rejecting (micro-edges live at
    // dx_lo when the spot edge is positive).
    if (native_fee_mode && !(best.profit > T(0)) && evals < MAX_SIZING_EVALS) {
        Candidate cl = eval_at(dx_lo);
        if (cl.profit > T(0) && low_cover > T(3) * dx_lo && evals < MAX_SIZING_EVALS) {
            eval_at(std::sqrt(dx_lo * low_cover));
        }
    }
#endif

    if (!(best.profit > T(0))) {
        d.dx = best.dx;
        d.dy_after_fee = best.dy_after_fee;
        d.profit = best.profit;
        d.fee_tokens = best.fee_tokens;
        d.notional_coin0 = (sel_i == 0) ? best.dx : best.dx * cex_price;
        return d;
    }

    d.do_trade = true;
    d.dx = best.dx;
    d.dy_after_fee = best.dy_after_fee;
    d.profit = best.profit;
    d.fee_tokens = best.fee_tokens;
    d.notional_coin0 = (sel_i == 0) ? best.dx : best.dx * cex_price;

    return d;
}

} // namespace trading
} // namespace arb
