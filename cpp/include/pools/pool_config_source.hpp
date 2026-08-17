// Pool config source facade for explicit pool entries.
#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

#include <boost/json.hpp>

#include "pools/pool_config_parse.hpp"
#include "trading/costs.hpp"

namespace arb {
namespace pools {

class PoolConfigDocument {
public:
    static PoolConfigDocument from_json(const boost::json::value& root);
    static PoolConfigDocument from_file(const std::string& path);

    size_t size() const;

    template <typename T>
    std::pair<PoolInit<T>, arb::trading::Costs<T>> instantiate(size_t index) const {
        if (index >= size()) {
            throw std::out_of_range("pool index out of range");
        }
        if (!explicit_pools_.at(index).is_object()) {
            throw std::runtime_error("pool config entry must be an object");
        }
        PoolInit<T> pool;
        arb::trading::Costs<T> costs;
        pool.global_index = index;
        parse_pool_entry<T>(explicit_pools_.at(index).as_object(), pool, costs);
        return {std::move(pool), std::move(costs)};
    }

private:
    boost::json::array explicit_pools_{};
};

} // namespace pools
} // namespace arb
