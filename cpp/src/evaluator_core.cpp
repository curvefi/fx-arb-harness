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
    const double apy = (vp_end > 0.0 && exponent > 0.0) ? std::pow(vp_end, exponent) - 1.0 : -1.0;

    const double donation_apy = static_cast<double>(res.donation_apy);
    const double donation_freq_s = static_cast<double>(res.donation_frequency);
    const double don_growth = arb::harness::donation_growth<double>(donation_apy, donation_freq_s, duration_s);

    const double lp_xcp_end = static_cast<double>(res.lp_xcp_profit);
    double apy_net = -1.0;
    if (lp_xcp_end > 0.0 && don_growth > 0.0 && exponent > 0.0) {
        const double net_growth = lp_xcp_end / don_growth;
        if (net_growth > 0.0) {
            apy_net = std::pow(net_growth, exponent) - 1.0;
        }
    }

    const auto& rm = res.metrics;

    m["vp"] = vp_end;
    m["xcp_profit"] = static_cast<double>(res.xcp_profit);
    m["lp_xcp_profit"] = lp_xcp_end;
    m["apy"] = apy;
    m["apy_net"] = apy_net;
    m["apy_net_gm"] = res.apy_net_gm;

    m["avg_rel_price_diff"] = tw.avg_rel_price_diff;
    m["max_rel_price_diff"] = tw.max_rel_price_diff;
    m["max_7d_rel_price_diff"] = tw.max_7d_rel_price_diff;
    m["final_rel_price_diff"] = tw.final_rel_price_diff;
    m["max_7d_skew"] = tw.max_7d_skew;
    m["min_price_scale"] = tw.min_price_scale;
    m["max_price_scale"] = tw.max_price_scale;
    m["tw_avg_pool_fee"] = tw.tw_avg_pool_fee;
    m["min_pool_fee"] = tw.min_pool_fee;
    m["max_pool_fee"] = tw.max_pool_fee;

    m["tw_real_slippage_1pct"] = res.slippage_probes.tw_slippage(0);
    m["tw_real_slippage_5pct"] = res.slippage_probes.tw_slippage(1);
    m["tw_real_slippage_10pct"] = res.slippage_probes.tw_slippage(2);

    m["trades"] = static_cast<double>(rm.trades);
    m["n_rebalances"] = static_cast<double>(rm.n_rebalances);
    m["dynamic_keeper_attempts"] = static_cast<double>(rm.dynamic_keeper_attempts);
    m["dynamic_keeper_commits"] = static_cast<double>(rm.dynamic_keeper_commits);
    m["dynamic_keeper_gap_checks"] = static_cast<double>(rm.dynamic_keeper_gap_checks);
    m["dynamic_keeper_gap_fires"] = static_cast<double>(rm.dynamic_keeper_gap_fires);
    m["dynamic_keeper_gap_threshold_fires"] = static_cast<double>(rm.dynamic_keeper_gap_threshold_fires);
    m["dynamic_keeper_heartbeat_fires"] = static_cast<double>(rm.dynamic_keeper_heartbeat_fires);
    m["dynamic_keeper_commit_clock_fires"] = static_cast<double>(rm.dynamic_keeper_commit_clock_fires);

    const double days = (duration_s > 0.0) ? (duration_s / 86400.0) : 0.0;
    m["dynamic_keeper_attempts_per_day"] = (days > 0.0) ? static_cast<double>(rm.dynamic_keeper_attempts) / days : 0.0;
    m["dynamic_keeper_commits_per_day"] = (days > 0.0) ? static_cast<double>(rm.dynamic_keeper_commits) / days : 0.0;
    m["dynamic_keeper_gap_checks_per_day"] = (days > 0.0) ? static_cast<double>(rm.dynamic_keeper_gap_checks) / days : 0.0;
    m["dynamic_keeper_gap_fires_per_day"] = (days > 0.0) ? static_cast<double>(rm.dynamic_keeper_gap_fires) / days : 0.0;
    m["dynamic_keeper_gap_threshold_fires_per_day"] = (days > 0.0) ? static_cast<double>(rm.dynamic_keeper_gap_threshold_fires) / days : 0.0;
    m["dynamic_keeper_heartbeat_fires_per_day"] = (days > 0.0) ? static_cast<double>(rm.dynamic_keeper_heartbeat_fires) / days : 0.0;
    m["dynamic_keeper_commit_clock_fires_per_day"] = (days > 0.0) ? static_cast<double>(rm.dynamic_keeper_commit_clock_fires) / days : 0.0;

    m["dynamic_keeper_step_bps_avg"] = rm.dynamic_keeper_commits > 0
        ? rm.dynamic_keeper_step_bps_sum / static_cast<double>(rm.dynamic_keeper_commits) : 0.0;
    m["dynamic_keeper_step_bps_max"] = rm.dynamic_keeper_step_bps_max;

    m["policy_keeper_checks"] = static_cast<double>(rm.policy_keeper_checks);
    m["policy_keeper_reject_clock"] = static_cast<double>(rm.policy_keeper_reject_clock);
    m["policy_keeper_reject_target_unavailable"] = static_cast<double>(rm.policy_keeper_reject_target_unavailable);
    m["policy_keeper_reject_deadband"] = static_cast<double>(rm.policy_keeper_reject_deadband);
    m["policy_keeper_reject_step_min"] = static_cast<double>(rm.policy_keeper_reject_step_min);
    m["policy_keeper_reject_below_threshold"] = static_cast<double>(rm.policy_keeper_reject_below_threshold);
    m["policy_keeper_reject_block"] = static_cast<double>(rm.policy_keeper_reject_block);
    m["policy_keeper_reject_outer_profit"] = static_cast<double>(rm.policy_keeper_reject_outer_profit);
    m["policy_keeper_raw_gap_candidates"] = static_cast<double>(rm.policy_keeper_raw_gap_candidates);
    m["policy_keeper_submissions"] = static_cast<double>(rm.policy_keeper_submissions);
    m["policy_keeper_submitted_commits"] = static_cast<double>(rm.policy_keeper_submitted_commits);
    m["policy_keeper_final_lp_rejects"] = static_cast<double>(rm.policy_keeper_final_lp_rejects);
    m["policy_keeper_unexpected_step_rejects"] = static_cast<double>(rm.policy_keeper_unexpected_step_rejects);
    m["policy_keeper_exceptions"] = static_cast<double>(rm.policy_keeper_exceptions);
    m["policy_keeper_lp_below_precision"] = static_cast<double>(rm.policy_keeper_lp_below_precision);
    m["policy_keeper_lp_below_floor"] = static_cast<double>(rm.policy_keeper_lp_below_floor);
    m["policy_keeper_lp_burn_cap_exhausted"] = static_cast<double>(rm.policy_keeper_lp_burn_cap_exhausted);
    m["policy_keeper_direction_up"] = static_cast<double>(rm.policy_keeper_direction_up);
    m["policy_keeper_direction_down"] = static_cast<double>(rm.policy_keeper_direction_down);
    m["policy_keeper_submissions_per_day"] = (days > 0.0) ? static_cast<double>(rm.policy_keeper_submissions) / days : 0.0;
    m["policy_keeper_final_lp_rejects_per_day"] = (days > 0.0) ? static_cast<double>(rm.policy_keeper_final_lp_rejects) / days : 0.0;
    m["policy_keeper_fire_to_commit_ratio"] = rm.policy_keeper_submissions > 0
        ? static_cast<double>(rm.policy_keeper_submitted_commits) / static_cast<double>(rm.policy_keeper_submissions) : 0.0;

    m["arb_edge_candidates"] = static_cast<double>(rm.arb_edge_candidates);
    m["arb_invalid_size_rejections"] = static_cast<double>(rm.arb_invalid_size_rejections);
    m["arb_nonpositive_profit_rejections"] = static_cast<double>(rm.arb_nonpositive_profit_rejections);
    m["arb_guarded_loss_coin0"] = static_cast<double>(rm.arb_guarded_loss_coin0);
    m["events_total"] = static_cast<double>(rm.events_total);
    m["geometry_refreshes"] = static_cast<double>(rm.geometry_refreshes);
    m["floor_gate_passes"] = static_cast<double>(rm.floor_gate_passes);
    m["actual_fee_calls"] = static_cast<double>(rm.actual_fee_calls);

    m["yb_enabled"] = res.yb_releverage_enabled ? 1.0 : 0.0;
    m["yb_apy"] = res.yb_releverage_apy;
    m["yb_apy_gm"] = res.yb_releverage_apy_gm;
    m["yb_final_growth"] = res.yb_releverage_final_growth;
    m["yb_fee"] = static_cast<double>(res.yb_releverage_fee);
    m["yb_releverage_trades"] = static_cast<double>(res.yb_releverage_trades);
    m["yb_gm_windows"] = static_cast<double>(res.yb_releverage_gm_windows);
    m["yb_gm_floored_windows"] = static_cast<double>(res.yb_releverage_gm_floored_windows);
    m["yb_gm_floor_share"] = res.yb_releverage_gm_floor_share;

    m["elapsed_ms"] = res.elapsed_ms;
    m["duration_s"] = duration_s;
    m["total_notional_coin0"] = static_cast<double>(rm.notional);
    m["lp_fee_coin0"] = static_cast<double>(rm.lp_fee_coin0);
    m["arb_pnl_coin0"] = static_cast<double>(rm.arb_pnl_coin0);

    const double gross_lvr = static_cast<double>(rm.lp_fee_coin0 + rm.arb_pnl_coin0);
    m["fee_capture_rate"] = (gross_lvr > 0.0) ? static_cast<double>(rm.lp_fee_coin0) / gross_lvr : -1.0;

    m["donations"] = static_cast<double>(rm.donations);
    m["donation_coin0_total"] = static_cast<double>(rm.donation_coin0_total);
    m["avg_imbalance"] = tw.avg_imbalance;
    m["max_episode_gap_energy"] = tw.max_episode_gap_energy;
    m["detach_energy"] = tw.detach_energy;
    m["detach_energy_ungated"] = tw.detach_energy_ungated;
    m["detach_energy_ungated_3pct"] = tw.detach_energy_ungated_3pct;
    m["detach_energy_ungated_5pct"] = tw.detach_energy_ungated_5pct;
    m["detach_energy_short3h"] = tw.detach_energy_short3h;

    const double tvl_start = static_cast<double>(res.tvl_start);
    const double tvl_end = static_cast<double>(res.balances[0] + res.balances[1] * res.price_scale);
    m["tvl_growth"] = arb::harness::tvl_growth(tvl_start, tvl_end);

    m["keeper_successful_submissions"] = static_cast<double>(rm.keeper_successful_submissions);
    m["fixed_keeper_ticks"] = static_cast<double>(rm.fixed_keeper_ticks);
}

void execute_scenario_job(
    const EvaluationCandidate<RealT>& cand,
    const Scenario<RealT>& scen,
    const SessionConfig<RealT>& session_cfg,
    const ObservationSpec& obs_spec,
    const arb::pools::PoolOverride<RealT>* pool_override,
    const std::string& pool_override_error,
    ScenarioEvaluationResult& sc_res
) {
    sc_res.scenario_id = scen.id;

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

        if (curve_fx::identity::HAS_COMPILED_POLICY) {
            if (pool_init.policy_kind != arb::pools::twocrypto_fx::PolicyKind::Compiled &&
                pool_init.policy_kind != arb::pools::twocrypto_fx::PolicyKind::None) {
                throw std::invalid_argument(
                    "compiled-policy build prohibits a non-compiled policy kind"
                );
            }
            pool_init.policy_kind = arb::pools::twocrypto_fx::PolicyKind::Compiled;
            pool_init.policy_config.kind = arb::pools::twocrypto_fx::PolicyKind::Compiled;
        } else {
            if (pool_init.policy_kind != arb::pools::twocrypto_fx::PolicyKind::TwocryptoPolicy &&
                pool_init.policy_kind != arb::pools::twocrypto_fx::PolicyKind::None) {
                throw std::invalid_argument(
                    "Native-default build enforces the native policy; requested non-native policy kind is prohibited"
                );
            }
        }

        const std::size_t expected_policy_params =
            curve_fx::identity::HAS_COMPILED_POLICY
                ? arb::pools::twocrypto_fx::ChallengeFeePolicy<RealT>::PARAM_COUNT
                : 0;
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
        run_cfg.dustswap_random = session_cfg.dustswap_random;
        run_cfg.dustswap_dynamic_freq_s = session_cfg.dustswap_dynamic_freq_s;
        run_cfg.dustswap_dynamic_gap_enabled = session_cfg.dustswap_dynamic_gap_enabled;
        run_cfg.dustswap_dynamic_gap_bps = session_cfg.dustswap_dynamic_gap_bps;
        run_cfg.dustswap_dynamic_heartbeat_s = session_cfg.dustswap_dynamic_heartbeat_s;
        run_cfg.dustswap_commit_clock_freq_s = session_cfg.dustswap_commit_clock_freq_s;
        run_cfg.policy_keeper_enabled = session_cfg.policy_keeper_enabled;
        run_cfg.allow_hybrid_keeper = session_cfg.allow_hybrid_keeper;
        run_cfg.user_swap_freq_s = session_cfg.user_swap_freq_s;
        run_cfg.user_swap_size_frac = session_cfg.user_swap_size_frac;
        run_cfg.user_swap_thresh = session_cfg.user_swap_thresh;
        run_cfg.enable_slippage_probes = session_cfg.enable_slippage_probes;
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
    const auto& scenarios = store.scenarios();
    const size_t n_scenarios = scenarios.size();

    BatchEvaluationResult batch_result;
    batch_result.candidate_results.resize(n_candidates);

    for (size_t cand_idx = 0; cand_idx < n_candidates; ++cand_idx) {
        auto& cand_res = batch_result.candidate_results[cand_idx];
        cand_res.ordinal = candidates[cand_idx].ordinal;
        cand_res.candidate_id = candidates[cand_idx].candidate_id;
        cand_res.success = true;
        cand_res.scenario_results.resize(n_scenarios);
    }

    // Materialize each candidate override exactly once. The typed overlay is
    // immutable while workers apply it to their scenario-local pool values.
    std::vector<std::optional<arb::pools::PoolOverride<RealT>>> parsed_overrides(n_candidates);
    std::vector<std::string> override_errors(n_candidates);
    for (size_t cand_idx = 0; cand_idx < n_candidates; ++cand_idx) {
        if (candidates[cand_idx].pool_overrides.empty()) continue;
        try {
            parsed_overrides[cand_idx] =
                arb::pools::parse_pool_override<RealT>(candidates[cand_idx].pool_overrides);
        } catch (const std::exception& e) {
            override_errors[cand_idx] = e.what();
        }
    }


    const size_t total_jobs = n_candidates * n_scenarios;

    if (total_jobs > 0) {
        // One fixed process-lifetime pool. Both summary and full-trace jobs
        // are submitted through the same pool: TraceArena::acquire() holds the
        // arena mutex for the duration of a full-trace run, which keeps at most
        // one trace job executing at any instant (one in flight); each job
        // still writes its canonical candidate/scenario result slot below.
        auto& pool = WorkerPool::global();
        pool.run_jobs(total_jobs, [&](size_t job_idx) {
            const size_t cand_idx = job_idx / n_scenarios;
            const size_t sc_idx = job_idx % n_scenarios;
            execute_scenario_job(
                candidates[cand_idx],
                scenarios[sc_idx],
                session_cfg,
                obs_spec,
                parsed_overrides[cand_idx]
                    ? &*parsed_overrides[cand_idx]
                    : nullptr,
                override_errors[cand_idx],
                batch_result.candidate_results[cand_idx].scenario_results[sc_idx]
            );
        });
    }

    // Deterministic post-join aggregation across scenarios for each candidate
    for (size_t cand_idx = 0; cand_idx < n_candidates; ++cand_idx) {
        auto& cand_res = batch_result.candidate_results[cand_idx];
        std::map<std::string, double> sum_metrics;
        size_t successful_scenarios = 0;

        for (size_t sc_idx = 0; sc_idx < n_scenarios; ++sc_idx) {
            const auto& sc_res = cand_res.scenario_results[sc_idx];
            if (!sc_res.success) {
                cand_res.success = false;
                if (cand_res.error_message.empty()) {
                    cand_res.error_message = "Scenario '" + sc_res.scenario_id + "' failed: " + sc_res.error_message;
                }
            } else {
                for (const auto& [k, v] : sc_res.metrics) {
                    sum_metrics[k] += v;
                }
                ++successful_scenarios;
            }
        }

        if (successful_scenarios > 0) {
            for (const auto& [k, v] : sum_metrics) {
                cand_res.aggregate_metrics[k] = v / static_cast<double>(successful_scenarios);
            }
        }

    }

    auto t_end = std::chrono::high_resolution_clock::now();
    batch_result.elapsed_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();

    return batch_result;
}

} // namespace curve_fx::evaluator
