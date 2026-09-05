#include "curve_fx_evaluator/trace.hpp"

#include <boost/json.hpp>

namespace json = boost::json;

namespace curve_fx::evaluator {

TraceArena& TraceArena::global_instance() {
    static TraceArena instance;
    return instance;
}

namespace {

json::object detailed_entry_to_json(const arb::harness::DetailedEntry<RealT>& e) {
    json::object o;
    o["t"] = e.t;
    o["token0"] = static_cast<double>(e.token0);
    o["token1"] = static_cast<double>(e.token1);
    o["D"] = static_cast<double>(e.D);
    o["xp_0"] = static_cast<double>(e.xp_0);
    o["xp_1"] = static_cast<double>(e.xp_1);
    o["price_oracle"] = static_cast<double>(e.price_oracle);
    o["price_scale"] = static_cast<double>(e.price_scale);
    o["profit"] = static_cast<double>(e.profit);
    o["vp"] = static_cast<double>(e.vp);
    o["vp_boosted"] = static_cast<double>(e.vp_boosted);
    o["xcp"] = static_cast<double>(e.xcp);
    o["lp_xcp_profit"] = static_cast<double>(e.lp_xcp_profit);
    o["total_supply"] = static_cast<double>(e.total_supply);
    o["donation_apy"] = static_cast<double>(e.donation_apy);
    o["donation_shares"] = static_cast<double>(e.donation_shares);
    o["donation_unlocked"] = static_cast<double>(e.donation_unlocked);
    o["last_prices"] = static_cast<double>(e.last_prices);
    o["last_timestamp"] = e.last_timestamp;
    o["open"] = static_cast<double>(e.open);
    o["high"] = static_cast<double>(e.high);
    o["low"] = static_cast<double>(e.low);
    o["close"] = static_cast<double>(e.close);
    o["p_cex"] = static_cast<double>(e.p_cex);
    o["p_price_feed"] = static_cast<double>(e.p_price_feed);
    o["fee"] = static_cast<double>(e.fee);
    o["slippage_1pct_0to1"] = static_cast<double>(e.slippage_1pct_0to1);
    o["slippage_1pct_1to0"] = static_cast<double>(e.slippage_1pct_1to0);
    o["n_trades"] = e.n_trades;
    o["n_rebalances"] = e.n_rebalances;
    o["yb_initialized"] = e.yb_initialized;
    o["yb_growth"] = static_cast<double>(e.yb_growth);
    o["yb_fee"] = static_cast<double>(e.yb_fee);
    o["yb_releverage_trades"] = e.yb_releverage_trades;
    o["yb_stable_balance"] = static_cast<double>(e.yb_stable_balance);
    o["yb_debt"] = static_cast<double>(e.yb_debt);
    o["yb_collateral_lp"] = static_cast<double>(e.yb_collateral_lp);
    o["yb_lp_oracle"] = static_cast<double>(e.yb_lp_oracle);
    o["yb_lp_fair"] = static_cast<double>(e.yb_lp_fair);
    return o;
}

json::object action_to_json(const arb::harness::Action<RealT>& action) {
    json::object o;
    std::visit([&o](auto&& act) {
        using ActionType = std::decay_t<decltype(act)>;
        if constexpr (std::is_same_v<ActionType, arb::harness::DonationAction<RealT>>) {
            o["type"] = "donation";
            o["ts"] = act.ts;
            o["ts_due"] = act.ts_due;
            o["amount0"] = static_cast<double>(act.amounts[0]);
            o["amount1"] = static_cast<double>(act.amounts[1]);
            o["price_scale"] = static_cast<double>(act.price_scale);
            o["donation_ratio1"] = static_cast<double>(act.donation_ratio1);
            o["apy_per_year"] = static_cast<double>(act.apy_per_year);
            o["freq_s"] = act.freq_s;
        } else if constexpr (std::is_same_v<ActionType, arb::harness::TickAction<RealT>>) {
            o["type"] = "tick";
            o["ts"] = act.ts;
            o["p_cex"] = static_cast<double>(act.p_cex);
            o["ps_before"] = static_cast<double>(act.ps_before);
            o["ps_after"] = static_cast<double>(act.ps_after);
            o["oracle_before"] = static_cast<double>(act.oracle_before);
            o["oracle_after"] = static_cast<double>(act.oracle_after);
            o["xcp_profit_before"] = static_cast<double>(act.xcp_profit_before);
            o["xcp_profit_after"] = static_cast<double>(act.xcp_profit_after);
            o["vp_before"] = static_cast<double>(act.vp_before);
            o["vp_after"] = static_cast<double>(act.vp_after);
        } else if constexpr (std::is_same_v<ActionType, arb::harness::ExchangeAction<RealT>>) {
            o["type"] = "exchange";
            o["ts"] = act.ts;
            o["i"] = act.i;
            o["j"] = act.j;
            o["dx"] = static_cast<double>(act.dx);
            o["dy_after_fee"] = static_cast<double>(act.dy_after_fee);
            o["fee_tokens"] = static_cast<double>(act.fee_tokens);
            if (act.synthetic_user) {
                o["actor"] = "user";
            } else {
                o["profit_coin0"] = static_cast<double>(act.profit_coin0);
            }
            o["p_cex"] = static_cast<double>(act.p_cex);
            o["p_pool_before"] = static_cast<double>(act.p_pool_before);
            o["p_pool_after"] = static_cast<double>(act.p_pool_after);
            o["oracle_before"] = static_cast<double>(act.oracle_before);
            o["oracle_after"] = static_cast<double>(act.oracle_after);
            o["ps_before"] = static_cast<double>(act.ps_before);
            o["ps_after"] = static_cast<double>(act.ps_after);
            o["lp_before"] = static_cast<double>(act.lp_before);
            o["lp_after"] = static_cast<double>(act.lp_after);
            o["xcp_profit_before"] = static_cast<double>(act.xcp_profit_before);
            o["xcp_profit_after"] = static_cast<double>(act.xcp_profit_after);
            o["vp_before"] = static_cast<double>(act.vp_before);
            o["vp_after"] = static_cast<double>(act.vp_after);
        }
    }, action);
    return o;
}

} // namespace

std::string serialize_detailed_entries_json(
    const std::vector<arb::harness::DetailedEntry<RealT>>& entries
) {
    json::array array;
    array.reserve(entries.size());
    for (const auto& entry : entries) {
        array.push_back(detailed_entry_to_json(entry));
    }
    return json::serialize(array);
}

std::string serialize_actions_json(
    const std::vector<arb::harness::Action<RealT>>& actions
) {
    json::array array;
    array.reserve(actions.size());
    for (const auto& action : actions) {
        array.push_back(action_to_json(action));
    }
    return json::serialize(array);
}

} // namespace curve_fx::evaluator
