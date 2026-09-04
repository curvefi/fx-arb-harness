#include "curve_fx_evaluator/compiled_policy_identity.hpp"
#include "curve_fx_evaluator/evaluator.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace curve_fx::evaluator {

namespace {

size_t process_worker_count = 1;
bool worker_pool_initialized = false;

// ============================================================================
// Fixed, bounded, process-lifetime worker pool.
//
// One pool per process (contract: "One fixed worker pool per process").
// Threads are created once and persist for the lifetime of the process. Each
// batch is tagged with a monotonically increasing generation; each active
// worker waits on its own condition variable, captures the batch exactly once
// per generation, and marks itself done once. `workers_remaining_` (not
// total-job subtraction) is the completion barrier, so a fast worker can never
// loop the same batch twice. Jobs claim unique indices from `next_job_`, and
// every job writes only its own preassigned [candidate][scenario] result slot,
// keeping output canonical regardless of schedule.
// ============================================================================
class WorkerPool {
public:
    static WorkerPool& global() {
        static WorkerPool pool;
        return pool;
    }

    ~WorkerPool() {
        {
            std::unique_lock<std::mutex> lock(mu_);
            stop_ = true;
            ++generation_;
        }
        for (auto& cv : cv_start_) cv.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
    }


    // Runs `total_jobs` indices [0, total_jobs) once each. Only the workers
    // participating in this generation are notified.
    void run_jobs(size_t total_jobs, const std::function<void(size_t)>& fn) {
        if (total_jobs == 0) return;
        std::unique_lock<std::mutex> gate(mu_);
        if (workers_.empty()) {
            gate.unlock();
            for (size_t job = 0; job < total_jobs; ++job) fn(job);
            return;
        }

        fn_ = &fn;
        total_ = total_jobs;
        next_job_.store(0, std::memory_order_relaxed);
        active_workers_ = std::min(total_jobs, workers_.size());
        workers_remaining_ = active_workers_;
        const uint64_t gen = ++generation_;
        for (size_t id = 0; id < active_workers_; ++id) {
            cv_start_[id].notify_one();
        }
        cv_done_.wait(gate, [this, gen] {
            return workers_remaining_ == 0 && generation_ == gen;
        });
        fn_ = nullptr;
        active_workers_ = 0;
    }

private:
    WorkerPool() : cv_start_(process_worker_count) {
        worker_pool_initialized = true;
        const size_t n = cv_start_.size();
        workers_.reserve(n);
        for (size_t id = 0; id < n; ++id) {
            workers_.emplace_back([this, id] { worker_loop(id); });
        }
    }

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    void worker_loop(size_t id) {
        uint64_t last_generation = 0;
        while (true) {
            std::unique_lock<std::mutex> lock(mu_);
            cv_start_[id].wait(lock, [this, &last_generation, id] {
                return stop_ || (id < active_workers_ && generation_ != last_generation);
            });
            if (stop_) return;

            last_generation = generation_;
            const std::function<void(size_t)>* fn = fn_;
            lock.unlock();

            for (size_t job = next_job_.fetch_add(1, std::memory_order_relaxed);
                 job < total_;
                 job = next_job_.fetch_add(1, std::memory_order_relaxed)) {
                (*fn)(job);
            }

            lock.lock();
            --workers_remaining_;
            if (workers_remaining_ == 0) {
                cv_done_.notify_one();
            }
        }
    }

    std::mutex mu_;
    std::condition_variable cv_done_;
    // Sized to the configured worker count in the constructor; condition_variable is
    // neither copyable nor movable, so the vector is never resized.
    std::vector<std::condition_variable> cv_start_;
    std::vector<std::thread> workers_;
    const std::function<void(size_t)>* fn_{nullptr};
    size_t total_{0};
    std::atomic<size_t> next_job_{0};
    size_t active_workers_{0};
    uint64_t generation_{0};
    size_t workers_remaining_{0};
    bool stop_{false};
};

void extract_metrics_from_pool_result(
    const arb::harness::PoolResult<RealT>& res,
    const arb::harness::TimeWeightedSummary& tw,
    std::map<std::string, double>& m
) {
    constexpr double SEC_PER_YEAR = 365.0 * 86400.0;
    const double duration_s = res.duration_s();
    const double exponent = (duration_s > 0.0) ? (SEC_PER_YEAR / duration_s) : 0.0;

    const double vp_end = static_cast<double>(res.virtual_price);
    const double vp_start = static_cast<double>(res.initial_virtual_price);
    const double apy = (vp_end > 0.0 && vp_start > 0.0 && exponent > 0.0) ? std::pow(vp_end / vp_start, exponent) - 1.0 : -1.0;

    const double donation_apy = static_cast<double>(res.donation_apy);
    const double donation_freq_s = static_cast<double>(res.donation_frequency);
    const double don_growth = arb::harness::donation_growth<double>(donation_apy, donation_freq_s, duration_s);

    const double lp_xcp_end = static_cast<double>(res.lp_xcp_profit);
    const double lp_xcp_start = static_cast<double>(res.initial_lp_xcp_profit);
    double apy_net = -1.0;
    if (lp_xcp_end > 0.0 && lp_xcp_start > 0.0 && don_growth > 0.0 && exponent > 0.0) {
        const double net_growth = lp_xcp_end / lp_xcp_start / don_growth;
        if (net_growth > 0.0) {
            apy_net = std::pow(net_growth, exponent) - 1.0;
        }
    }

    const auto& rm = res.metrics;

    m["vp"] = vp_end;
    m["lp_xcp_profit"] = lp_xcp_end;
    m["apy"] = apy;
    m["apy_net"] = apy_net;
    m["apy_net_gm"] = res.apy_net_gm;
    m["apy_net_robust_90d"] = res.apy_net_robust_90d;

    m["avg_rel_price_diff"] = tw.avg_rel_price_diff;
    m["max_rel_price_diff"] = tw.max_rel_price_diff;
    m["max_7d_rel_price_diff"] = tw.max_7d_rel_price_diff;
    m["final_rel_price_diff"] = tw.final_rel_price_diff;
    m["detach_energy_ungated"] = tw.detach_energy_ungated;
    m["avg_imbalance"] = tw.avg_imbalance;
    m["tw_avg_pool_fee"] = tw.tw_avg_pool_fee;
    m["min_pool_fee"] = tw.min_pool_fee;
    m["max_pool_fee"] = tw.max_pool_fee;

    m["tw_real_slippage_1pct"] = res.slippage_probes.tw_slippage(0);
    m["tw_real_slippage_5pct"] = res.slippage_probes.tw_slippage(1);
    m["tw_real_slippage_10pct"] = res.slippage_probes.tw_slippage(2);

    m["trades"] = static_cast<double>(rm.trades);
    m["n_rebalances"] = static_cast<double>(rm.n_rebalances);
    m["arb_guarded_loss_coin0"] = static_cast<double>(rm.arb_guarded_loss_coin0);

    m["yb_apy"] = res.yb_releverage_apy;
    m["yb_apy_gm"] = res.yb_releverage_apy_gm;
    m["yb_final_growth"] = res.yb_releverage_final_growth;
    m["yb_fee"] = static_cast<double>(res.yb_releverage_fee);
    m["yb_releverage_trades"] = static_cast<double>(res.yb_releverage_trades);
    m["yb_gm_windows"] = static_cast<double>(res.yb_releverage_gm_windows);
    m["yb_gm_floored_windows"] = static_cast<double>(res.yb_releverage_gm_floored_windows);
    m["yb_gm_floor_share"] = res.yb_releverage_gm_floor_share;

    m["elapsed_ms"] = res.elapsed_ms;
    m["total_notional_coin0"] = static_cast<double>(rm.notional);
    m["lp_fee_coin0"] = static_cast<double>(rm.lp_fee_coin0);
    m["arb_pnl_coin0"] = static_cast<double>(rm.arb_pnl_coin0);

    const double gross_lvr = static_cast<double>(rm.lp_fee_coin0 + rm.arb_pnl_coin0);
    m["fee_capture_rate"] = (gross_lvr > 0.0) ? static_cast<double>(rm.lp_fee_coin0) / gross_lvr : -1.0;

    m["donations"] = static_cast<double>(rm.donations);
    m["donation_coin0_total"] = static_cast<double>(rm.donation_coin0_total);

    const double tvl_start = static_cast<double>(res.tvl_start);
    const double tvl_end = static_cast<double>(res.balances[0] + res.balances[1] * res.price_scale);
    m["tvl_growth"] = arb::harness::tvl_growth(tvl_start, tvl_end);
}

void execute_scenario_job(
    const EvaluationCandidate<RealT>& cand,
    const Scenario<RealT>& scen,
    const SessionConfig<RealT>& session_cfg,
    const ObservationSpec& obs_spec,
    const arb::pools::PoolOverride<RealT>* pool_override,
    const std::string& pool_override_error,
    CandidateEvaluationResult& sc_res
) {

    try {
        if (!pool_override_error.empty()) {
            throw std::invalid_argument(pool_override_error);
        }

        arb::pools::PoolInit<RealT> pool_init = scen.base_pool;
        arb::trading::Costs<RealT> costs = scen.base_costs;

        // Candidate overrides are parsed once before the batch and applied as
        // typed values; only fields present in the override replace scenario
        // defaults.
        if (pool_override != nullptr) {
            pool_override->apply(pool_init, costs);
        }
        if (pool_init.historical_state.enabled &&
            pool_init.start_ts != 0 &&
            pool_init.start_ts < pool_init.historical_state.source_timestamp) {
            throw std::invalid_argument(
                "pool start_timestamp predates historical_state source_timestamp"
            );
        }

#ifdef TWOCRYPTO_POLICY_HEADER
        pool_init.policy_kind = arb::pools::twocrypto_fx::PolicyKind::Compiled;
        pool_init.policy_config.kind =
            arb::pools::twocrypto_fx::PolicyKind::Compiled;
        constexpr std::size_t expected_policy_params =
            arb::pools::twocrypto_fx::ChallengeFeePolicy<RealT>::PARAM_COUNT;
#else
        pool_init.policy_kind = arb::pools::twocrypto_fx::PolicyKind::None;
        pool_init.policy_config.kind = arb::pools::twocrypto_fx::PolicyKind::None;
        constexpr std::size_t expected_policy_params = 0;
#endif
        if (cand.policy_params.size() != expected_policy_params) {
            throw std::invalid_argument(
                "policy parameter count mismatch: expected " +
                std::to_string(expected_policy_params) + ", got " +
                std::to_string(cand.policy_params.size())
            );
        }

        // The admitted vector is exact and dense; no truncation or fallback.
        if (!cand.policy_params.empty()) {
            for (size_t p_i = 0; p_i < cand.policy_params.size(); ++p_i) {
                pool_init.policy_config.params[p_i] = cand.policy_params[p_i];
            }
            pool_init.policy_config.n_params = cand.policy_params.size();
        }

        // Construct RunConfig
        arb::harness::RunConfig<RealT> run_cfg;
        run_cfg.min_swap_frac = session_cfg.min_swap_frac;
        run_cfg.max_swap_frac = session_cfg.max_swap_frac;
        run_cfg.start_ts = session_cfg.start_ts;
        run_cfg.dustswap_freq_s = session_cfg.dustswap_freq_s;
        run_cfg.user_swap_freq_s = session_cfg.user_swap_freq_s;
        run_cfg.user_swap_size_frac = session_cfg.user_swap_size_frac;
        run_cfg.user_swap_thresh = session_cfg.user_swap_thresh;
        run_cfg.enable_slippage_probes = session_cfg.enable_slippage_probes;
        if (session_cfg.event_cursor == "exact_skip") {
            run_cfg.event_cursor = arb::harness::EventCursor::ExactSkip;
        } else if (session_cfg.event_cursor == "scalar") {
            run_cfg.event_cursor = arb::harness::EventCursor::Scalar;
        } else {
            throw std::invalid_argument(
                "unknown event_cursor '" + session_cfg.event_cursor +
                "' (expected 'scalar' or 'exact_skip')"
            );
        }
        if (session_cfg.metric_profile == "grid_core") {
            run_cfg.metric_profile = arb::harness::MetricProfile::GridCore;
        } else if (session_cfg.metric_profile == "full_summary") {
            run_cfg.metric_profile = arb::harness::MetricProfile::FullSummary;
        } else {
            throw std::invalid_argument(
                "unknown metric_profile '" + session_cfg.metric_profile +
                "' (expected 'full_summary' or 'grid_core')"
            );
        }
        if (session_cfg.yb_mode == "active_2l") {
            run_cfg.yb_mode = arb::harness::YbMode::Active2l;
        } else if (session_cfg.yb_mode == "reference_2l") {
            run_cfg.yb_mode = arb::harness::YbMode::Reference2l;
        } else if (session_cfg.yb_mode == "off") {
            run_cfg.yb_mode = arb::harness::YbMode::Off;
        } else {
            throw std::invalid_argument(
                "unknown yb_mode '" + session_cfg.yb_mode +
                "' (expected 'off', 'active_2l', or 'reference_2l')"
            );
        }
        run_cfg.yb_releverage_fee =
            pool_override != nullptr && pool_override->yb_releverage_fee.has_value()
                ? *pool_override->yb_releverage_fee
                : session_cfg.yb_releverage_fee;
        run_cfg.yb_cash_multiplier = session_cfg.yb_cash_multiplier;

        std::vector<arb::harness::Action<RealT>>* actions_ptr = nullptr;
        std::vector<arb::harness::DetailedEntry<RealT>>* detailed_ptr = nullptr;
        std::optional<TraceArena::Lease> trace_lease;

        if (obs_spec.kind == ObservationKind::FullTrace) {
            auto& effective = sc_res.effective_inputs;
            effective["pool.A"] = static_cast<double>(pool_init.A);
            effective["pool.gamma"] = static_cast<double>(pool_init.gamma);
            effective["pool.mid_fee"] = static_cast<double>(pool_init.mid_fee);
            effective["pool.out_fee"] = static_cast<double>(pool_init.out_fee);
            effective["pool.fee_gamma"] = static_cast<double>(pool_init.fee_gamma);
            effective["pool.adjustment_step_min"] =
                static_cast<double>(pool_init.adjustment_step_min);
            effective["pool.adjustment_step_max"] =
                static_cast<double>(pool_init.adjustment_step_max);
            effective["pool.ma_time"] = static_cast<double>(pool_init.ma_time);
            effective["pool.reserved_profit_fraction"] =
                static_cast<double>(pool_init.reserved_profit_fraction);
            effective["pool.admin_fee"] = static_cast<double>(pool_init.admin_fee);
            effective["pool.donation_apy"] =
                static_cast<double>(pool_init.donation_apy);
            effective["pool.donation_frequency"] =
                static_cast<double>(pool_init.donation_frequency);
            effective["pool.donation_duration"] =
                static_cast<double>(pool_init.donation_duration);
            effective["pool.donation_coins_ratio"] =
                static_cast<double>(pool_init.donation_coins_ratio);
            effective["pool.costs.arb_fee_bps"] =
                static_cast<double>(costs.arb_fee_bps);
            effective["pool.costs.gas_coin0"] =
                static_cast<double>(costs.gas_coin0);
            effective["pool.run.yb_releverage_fee"] =
                static_cast<double>(run_cfg.yb_releverage_fee);
            effective["run.yb_cash_multiplier"] =
                static_cast<double>(run_cfg.yb_cash_multiplier);

            trace_lease.emplace(TraceArena::global_instance().acquire());
            run_cfg.detailed_log = true;
            run_cfg.detailed_interval = std::max<size_t>(1, obs_spec.trace_interval);
            run_cfg.save_actions = obs_spec.trace_actions;
            detailed_ptr = &trace_lease->detailed_entries();
            if (run_cfg.save_actions) {
                actions_ptr = &trace_lease->actions();
            }
        } else {
            // Summary mode: keep both output pointers null so the detailed
            // logger stays disabled (enabled() == out_entries_ != nullptr)
            // and the per-worker vectors never fill with per-event entries.
            run_cfg.detailed_log = false;
        }

        auto pool_res = arb::harness::run_single_pool<RealT>(
            pool_init,
            costs,
            scen.events,
            run_cfg,
            &scen.candles,
            actions_ptr,
            detailed_ptr
        );

        if (!pool_res.success) {
            sc_res.success = false;
            sc_res.error_message = pool_res.error_msg;
            return;
        }

        sc_res.success = true;
        auto tw_summary = pool_res.tw_metrics.summarize();
        extract_metrics_from_pool_result(pool_res, tw_summary, sc_res.metrics);

        if (trace_lease.has_value()) {
            sc_res.has_trace = true;
            sc_res.trace_record_count = trace_lease->detailed_entries().size();
            sc_res.action_count = trace_lease->actions().size();
            sc_res.trace_json = serialize_detailed_entries_json(
                trace_lease->detailed_entries());

            if (obs_spec.trace_actions) {
                sc_res.actions_json = serialize_actions_json(
                    trace_lease->actions());
            }
        }

    } catch (const std::exception& e) {
        sc_res.success = false;
        sc_res.error_message = e.what();
    }
}

} // namespace

void configure_worker_count(size_t count) {
    if (count == 0) {
        throw std::invalid_argument("worker count must be positive");
    }
    const size_t hardware = std::thread::hardware_concurrency();
    if (hardware != 0 && count > hardware) {
        throw std::invalid_argument(
            "worker count " + std::to_string(count) +
            " exceeds detected hardware concurrency " +
            std::to_string(hardware)
        );
    }
    if (worker_pool_initialized && count != process_worker_count) {
        throw std::logic_error("worker pool is already initialized");
    }
    process_worker_count = count;
}

size_t configured_worker_count() {
    return process_worker_count;
}

BatchEvaluationResult evaluate_batch_candidates(
    const ScenarioStore<RealT>& store,
    const SessionConfig<RealT>& session_cfg,
    const std::vector<EvaluationCandidate<RealT>>& candidates,
    const ObservationSpec& obs_spec
) {
    auto t_start = std::chrono::high_resolution_clock::now();

    const size_t n_candidates = candidates.size();
    const auto& scenario = store.scenario();

    BatchEvaluationResult batch_result;
    batch_result.candidate_results.resize(n_candidates);

    for (size_t cand_idx = 0; cand_idx < n_candidates; ++cand_idx) {
        auto& cand_res = batch_result.candidate_results[cand_idx];
        cand_res.ordinal = candidates[cand_idx].ordinal;
        cand_res.candidate_id = candidates[cand_idx].candidate_id;
    }

    // Materialize each candidate override exactly once. The typed overlay is
    // immutable while workers apply it to their scenario-local pool values.
    std::vector<std::optional<arb::pools::PoolOverride<RealT>>> parsed_overrides(n_candidates);
    std::vector<std::string> override_errors(n_candidates);
    for (size_t cand_idx = 0; cand_idx < n_candidates; ++cand_idx) {
        if (candidates[cand_idx].typed_pool_override.has_value()) {
            parsed_overrides[cand_idx] = candidates[cand_idx].typed_pool_override;
            continue;
        }
        if (candidates[cand_idx].pool_overrides.empty()) continue;
        try {
            parsed_overrides[cand_idx] =
                arb::pools::parse_pool_override<RealT>(candidates[cand_idx].pool_overrides);
        } catch (const std::exception& e) {
            override_errors[cand_idx] = e.what();
        }
    }


    if (n_candidates > 0) {
        // Each job owns one candidate result. TraceArena serializes full traces.
        WorkerPool::global().run_jobs(n_candidates, [&](size_t cand_idx) {
            execute_scenario_job(
                candidates[cand_idx], scenario, session_cfg, obs_spec,
                parsed_overrides[cand_idx] ? &*parsed_overrides[cand_idx] : nullptr,
                override_errors[cand_idx], batch_result.candidate_results[cand_idx]
            );
        });
    }

    auto t_end = std::chrono::high_resolution_clock::now();
    batch_result.elapsed_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();

    return batch_result;
}

} // namespace curve_fx::evaluator
