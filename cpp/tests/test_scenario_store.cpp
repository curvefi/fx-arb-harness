#include <iostream>
#include <string>

#include "curve_fx_evaluator/evaluator.hpp"

namespace {

void require(bool condition, const char* msg) {
    if (!condition) {
        std::cerr << "FAIL: " << msg << std::endl;
        std::exit(1);
    }
}

} // namespace

int main() {
    using namespace curve_fx::evaluator;

    ScenarioStore<RealT> store;
    std::string bin_sha = "0000000000000000000000000000000000000000000000000000000000000000";
    std::string pol_sha = "1111111111111111111111111111111111111111111111111111111111111111";
    std::string cfg_sha = "2222222222222222222222222222222222222222222222222222222222222222";

    std::string fp1 = store.compute_session_fingerprint(
        bin_sha, pol_sha, cfg_sha, 0);
    require(fp1.size() == 64, "session fingerprint must be 64-hex chars");
    std::string fp2 = store.compute_session_fingerprint(
        bin_sha, pol_sha, cfg_sha, 0);
    require(fp1 == fp2, "session fingerprint must be deterministic");
    std::string other_pool = store.compute_session_fingerprint(
        bin_sha, pol_sha, cfg_sha, 1);
    require(fp1 != other_pool, "pool_index must change the session fingerprint");

    std::string sc_set = store.compute_scenario_set_sha256();
    require(sc_set.size() == 64, "scenario set hash must be 64-hex chars");

    std::cout << "test_scenario_store: PASSED\n";
    return 0;
}
