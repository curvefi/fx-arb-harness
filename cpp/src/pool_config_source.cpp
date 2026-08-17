#include "pools/pool_config_source.hpp"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

#include <boost/json.hpp>

namespace arb {
namespace pools {

PoolConfigDocument PoolConfigDocument::from_json(const boost::json::value& root) {
    PoolConfigDocument doc;
    if (root.is_array()) {
        doc.explicit_pools_ = root.as_array();
        return doc;
    }

    if (!root.is_object()) {
        throw std::runtime_error("Invalid pools json root type");
    }

    const auto& obj = root.as_object();
    if (auto* pools = obj.if_contains("pools")) {
        if (!pools->is_array()) {
            throw std::runtime_error("Invalid pools json: 'pools' must be an array");
        }
        doc.explicit_pools_ = pools->as_array();
        return doc;
    }
    if (auto* pool = obj.if_contains("pool")) {
        doc.explicit_pools_.push_back(*pool);
        return doc;
    }
    if (obj.if_contains("meta") || obj.if_contains("metadata")) {
        throw std::runtime_error(
            "compact pool grids are not supported by the evaluator; materialize pools in the orchestrator"
        );
    }

    // A bare object is one direct pool entry.
    doc.explicit_pools_.push_back(root);
    return doc;
}

PoolConfigDocument PoolConfigDocument::from_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Cannot open pool config file: " + path);
    }
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return from_json(boost::json::parse(content));
}

size_t PoolConfigDocument::size() const {
    return explicit_pools_.size();
}

} // namespace pools
} // namespace arb
