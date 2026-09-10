#include <cmath>
#include <iostream>
#include "harness/event_loop.hpp"
#include "harness/state_reconciliation.hpp"
using namespace arb;
using namespace arb::harness;
using Pool = pools::twocrypto_fx::TwoCryptoPool<double>;
using Row = events::ObservedState<double>;
using Tape = events::ObservedStateTape<double>;
constexpr uint64_t TS = 1'779'753'600, NS = 1'000'000'000ULL;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
Pool seed() {
    Pool p({1.0,1.0},50000.,1.1111111111e-8,0.0146,0.0170,0.054202748,
        1e-10,5e-3,865.,77235.68,0.301010101,0.0);
    p.set_block_timestamp(TS-1); p.add_liquidity({43596754.65,564.46165},0.0);
    return p;
}
Row checkpoint(const Pool& p, const YbReference2LMarket<double>& yb, uint64_t ts) {
    Row r; r.source_timestamp = r.observed_through_timestamp = ts;
    r.source_block = ts; r.available_ns = (ts+1)*NS;
    r.coverage_start_timestamp = TS; r.coverage_end_timestamp = TS+600;
    auto& i = r.pool_init; auto& h = i.historical_state;
    i.precisions=p.precisions; i.A=p.A; i.gamma=p.gamma; i.mid_fee=p.mid_fee;
    i.out_fee=p.out_fee; i.fee_gamma=p.fee_gamma; i.adjustment_step_min=p.adjustment_step_min;
    i.adjustment_step_max=p.adjustment_step_max; i.ma_time=p.ma_time;
    i.reserved_profit_fraction=p.reserved_profit_fraction; i.admin_fee=p.admin_fee;
    i.donation_duration=p.donation_duration;
    h.enabled=true; h.source_block=r.source_block; h.source_timestamp=ts;
    h.balances=p.balances; h.admin_balances=p.admin_balances; h.D=p.D; h.total_supply=p.totalSupply;
    h.price_scale=p.cached_price_scale; h.price_oracle=p.cached_price_oracle;
    h.last_prices=p.last_prices; h.last_timestamp=p.last_timestamp;
    h.virtual_price=p.virtual_price; h.xcp_profit=p.xcp_profit; h.lp_xcp_profit=p.lp_xcp_profit;
    h.last_admin_fee_claim_timestamp=p.last_admin_fee_claim_timestamp;
    h.donation_shares=p.donation_shares; h.last_donation_release_ts=p.last_donation_release_ts;
    h.donation_protection_expiry_ts=p.donation_protection_expiry_ts;
    h.donation_protection_period=p.donation_protection_period;
    h.donation_protection_lp_threshold=p.donation_protection_lp_threshold;
    h.donation_protection_extension_remainder=p.donation_protection_extension_remainder;
    h.donation_shares_max_ratio=p.donation_shares_max_ratio;
    const auto& s=yb.state();
    r.yb_initial_state={r.source_block,ts,"0x"+std::string(64,'0'),s.leverage,s.fee,
        s.collateral,s.debt,s.rate,s.rate_mul,s.rate_time,s.minted,s.redeemed,
        s.stable_balance,s.lt_stable_balance,s.flash_max_loan,s.stable_aggregator,
        s.rounding_discount,s.lt_donation_discount,s.killed};
    return r;
}
void restore_and_next_operation() {
    auto expected=seed(); expected.set_block_timestamp(TS);
    expected.admin_balances={4.0,0.001}; expected.last_admin_fee_claim_timestamp=TS-1000;
    expected.donation_shares=expected.totalSupply*1e-5;
    expected.last_donation_release_ts=TS-120; expected.donation_protection_expiry_ts=TS+30;
    expected.donation_protection_period=601; expected.donation_protection_lp_threshold=.21;
    expected.donation_protection_extension_remainder=.25; expected.donation_shares_max_ratio=.12;
    auto expected_yb=YbReference2LMarket<double>::fresh_2l(expected,.0145,.012,TS,3.0);
    auto row=checkpoint(expected,expected_yb,TS);
    const Tape tape({row}); StateReconciliation<double> c(tape,StateReconciliationMode::OnPriceScaleDetach,100.,0);
    auto actual=seed(); auto actual_yb=YbReference2LMarket<double>::fresh_2l(actual,.02,.01,TS,2.0);
    actual.cached_ema_alpha_valid=true; actual.cached_ema_dt=100; actual.cached_ema_alpha=.3;
    const auto old_interest=actual_yb.projected_interest_summary(TS+1);
    std::vector<StateReconciliationAction<double>> log;
    auto emit=[&](auto a){log.push_back(std::move(a));};
    c.advance(TS*NS);
    c.check_price_scale_detachment(actual.cached_price_scale*1.1,TS*NS,emit);
    require(c.summary.observations==0 && !c.draining(),"future checkpoint leaked");
    c.advance((TS+1)*NS);
    c.check_price_scale_detachment(actual.cached_price_scale*1.1,(TS+1)*NS,emit);
    require(c.apply_if_ready((TS+1)*NS,actual,actual_yb,emit),"zero-delay reset did not apply");
    require(public_pool_values(actual)==public_pool_values(expected),"native public restore incomplete");
    require(public_yb_values(actual_yb.state())==public_yb_values(expected_yb.state()),"YB public restore incomplete");
    require(actual.block_timestamp==TS && actual.last_timestamp==TS-1 && !actual.cached_ema_alpha_valid,
            "restore substituted wall time or retained stale EMA cache");
    const auto interest=actual_yb.projected_interest_summary(TS+1);
    require(std::abs(interest.accrued-old_interest.accrued)<1e-7 && interest.donated==old_interest.donated,
            "copied debt credited simulated interest");
    actual.set_block_timestamp(TS+12); expected.set_block_timestamp(TS+12);
    const auto a=actual.exchange(0,1,1000.,0.), b=expected.exchange(0,1,1000.,0.);
    require(a==b && public_pool_values(actual)==public_pool_values(expected) &&
        actual.cached_ema_dt==expected.cached_ema_dt && actual.cached_ema_alpha==expected.cached_ema_alpha &&
        actual.cached_ema_alpha_valid==expected.cached_ema_alpha_valid,"next native operation differs");
    actual.set_block_timestamp(TS+24); expected.set_block_timestamp(TS+24);
    const auto va=actual_yb.apply_atomic(actual,1,.001,0.,TS+24);
    const auto vb=expected_yb.apply_atomic(expected,1,.001,0.,TS+24);
    require(va.committed && vb.committed && va.output==vb.output &&
        public_pool_values(actual)==public_pool_values(expected) &&
        public_yb_values(actual_yb.state())==public_yb_values(expected_yb.state()),"next VP operation differs");

    // The checkpoint predates the last committed route; reset and repeat at a
    // later wall time. Stored public clocks stay old, private accrual does not.
    const auto checkpoint_yb=YbReference2LMarket<double>::from_state(row.yb_initial_state);
    for (uint64_t offset : {60,90}) {
        const auto before=actual_yb.projected_interest_summary(TS+offset);
        actual_yb.restore_public_state(row.yb_initial_state,TS+offset);
        const auto after=actual_yb.projected_interest_summary(TS+offset);
        require(before.accrued>0 && before.donated>0 &&
            std::abs(after.accrued-before.accrued)<1e-7 && after.donated==before.donated &&
            std::abs(after.conservation_residual)<1e-7,"older checkpoint changed private interest history");
        require(public_yb_values(actual_yb.state())==public_yb_values(checkpoint_yb.state()),
            "accounting rebase changed checkpoint fields");
        const auto later=actual_yb.projected_interest_summary(TS+offset+12);
        const double increment=checkpoint_yb.projected_debt(TS+offset+12)
            - checkpoint_yb.projected_debt(TS+offset);
        require(std::abs(later.accrued-after.accrued-increment)<1e-7,
            "post-reset accrual repeated an old interval");
    }
}
void price_scale_detach_uses_detection_deadline() {
    auto p=seed(); p.cached_price_scale=100.;
    auto yb=YbReference2LMarket<double>::fresh_2l(p,.0145,.012,TS,3.0);
    auto first=checkpoint(p,yb,TS), latest=checkpoint(p,yb,TS+10);
    latest.pool_init.historical_state.price_scale=102.;
    const Tape tape({first,latest});
    for (auto [threshold,delay] : {std::pair{100.,60U},std::pair{50.,20U}}) {
        StateReconciliation<double> c(tape,StateReconciliationMode::OnPriceScaleDetach,threshold,delay);
        std::vector<StateReconciliationAction<double>> logs; auto log=[&](auto a){logs.push_back(a);};
        c.advance((TS+1)*NS);
        c.check_price_scale_detachment(100.+threshold/100.-.001,(TS+1)*NS,log);
        require(!c.draining(),"sub-threshold difference triggered reset");
        c.check_price_scale_detachment(100.-threshold/100.,(TS+1)*NS,log);
        require(c.draining() && logs.back().phase=="request" &&
            logs.back().deadline_ns==(TS+1+delay)*NS,"threshold boundary or deadline wrong");
        c.check_price_scale_detachment(100.,(TS+10)*NS,log);
        c.advance((TS+delay)*NS);
        require(!c.apply_if_ready((TS+delay)*NS,p,yb,log),"reset applied before deadline");
        c.advance((TS+1+delay)*NS);
        c.apply_if_ready((TS+1+delay)*NS,p,yb,log);
        require(!c.draining() && p.cached_price_scale==102. &&
            logs.back().source_timestamp==TS+10 && logs.back().apply_ns==(TS+1+delay)*NS &&
            c.summary.episodes==1,"latched reset failed to copy latest published checkpoint");
    }
}
void reset_at_runtime_cadence_and_off_is_inert() {
    auto original=seed();
    auto yb=YbReference2LMarket<double>::fresh_2l(original,.0145,.012,TS,3.0);
    auto row=checkpoint(original,yb,TS); row.pool_init.historical_state.price_scale*=1.02;
    const Tape tape({row});
    const auto events=EventSoA::from_events({{TS,77235.,0.,0,1.,0},
        {TS+10,77235.,0.,0,1.,0},{TS+20,77235.,0.,0,1.,0},{TS+30,77235.,0.,0,1.,0}});
    const events::CexDepthTape depth({{TS*NS,{{70000.,1.}},{{80000.,1.}}}});
    RunConfig<double> cfg; cfg.actor_timing_mode=ActorTimingMode::MinuteSequential;
    cfg.yb_mode=YbMode::Reference2l; cfg.yb_cash_multiplier=3.; cfg.dustswap_freq_s=0;
    cfg.cex_depth=&depth; cfg.observation_interval_s=10; cfg.equalization_delay_s=20;
    trading::Costs<double> costs; costs.gas_coin0=1e12;
    DonationCfg<double> donation; donation.apy=.0145; IdleTickCfg<double> idle; idle.freq_s=0;
    UserSwapCfg<double> user;
    auto baseline=original;
    const auto base=run_event_loop(baseline,events,costs,donation,idle,user,cfg);
    cfg.observed_state=&tape;
    auto off=original;
    const auto disabled=run_event_loop(off,events,costs,donation,idle,user,cfg);
    require(public_pool_values(off)==public_pool_values(baseline) && disabled.reconciliation.resets==0 &&
        disabled.metrics.trades==base.metrics.trades,"off mode changed execution");
    cfg.state_reconciliation_mode=StateReconciliationMode::OnPriceScaleDetach;
    auto active=original; std::vector<Action<double>> actions;
    const auto reset=run_event_loop(active,events,costs,donation,idle,user,cfg,nullptr,0,&actions);
    require(reset.reconciliation.resets==1 && active.cached_price_scale==row.pool_init.historical_state.price_scale,
        "nondefault cadence or delay failed to reset");
    bool applied=false;
    for (const auto& a:actions) if (const auto* r=std::get_if<StateReconciliationAction<double>>(&a); r && r->phase=="apply")
        applied=r->detection_ns==(TS+10)*NS && r->apply_ns==(TS+30)*NS;
    require(applied,"runtime reset applied at wrong observation");
}
int main() {
    restore_and_next_operation();
    price_scale_detach_uses_detection_deadline();
    reset_at_runtime_cadence_and_off_is_inert();
    std::cout<<"test_state_reconciliation: PASSED\n";
}
