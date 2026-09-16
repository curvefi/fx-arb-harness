// Numeric C-order depth NPZ interchange. MiniZip handles ZIP/DEFLATE/CRC.
#include "events/cex_depth.hpp"

#include <array>
#include <algorithm>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <limits>
#include <regex>
#include <sstream>
#include <minizip/unzip.h>

namespace arb::events {
namespace {
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);

[[noreturn]] void invalid() {
    throw std::runtime_error("Invalid CEX depth NPZ");
}

uint64_t little(const unsigned char* p, size_t n) {
    uint64_t value = 0;
    for (size_t i = 0; i < n; ++i) value |= uint64_t(p[i]) << (8*i);
    return value;
}

struct Array {
    std::vector<unsigned char> bytes;
    std::string dtype;
    std::vector<size_t> shape;
    size_t offset{};
    const unsigned char* data() const { return bytes.data()+offset; }
};

Array parse(std::vector<unsigned char> bytes) {
    if (bytes.size() < 10 || std::memcmp(bytes.data(), "\x93NUMPY", 6) != 0
            || bytes[7] != 0 || (bytes[6] != 1 && bytes[6] != 2)) invalid();
    const size_t prefix = bytes[6] == 1 ? 10 : 12;
    if (bytes.size() < prefix) invalid();
    const size_t length = little(bytes.data()+8, prefix-8);
    if (length > bytes.size()-prefix) invalid();
    const std::string header(reinterpret_cast<const char*>(bytes.data()+prefix), length);
    // np.save writes these sorted dictionary fields; no Python evaluation.
    static const std::regex pattern(
        R"(^\{'descr': '([^']+)', 'fortran_order': False, 'shape': \(([0-9, ]*)\), \} *\n$)");
    std::smatch match;
    if (!std::regex_match(header, match, pattern)) invalid();
    Array result;
    result.dtype = match[1];
    std::istringstream dimensions(match[2]);
    std::string part;
    size_t elements = 1;
    while (std::getline(dimensions, part, ',')) {
        const auto start = part.find_first_not_of(' ');
        if (start == std::string::npos) continue;
        const auto end = part.find_last_not_of(' ');
        part = part.substr(start, end-start+1);
        size_t value = 0;
        const auto [parsed_end, error] = std::from_chars(part.data(), part.data()+part.size(), value);
        if (error != std::errc{} || parsed_end != part.data()+part.size() ||
                !value || value > bytes.size() || elements > bytes.size()/value) invalid();
        elements *= value;
        result.shape.push_back(value);
        if (result.shape.size() > 4) invalid();
    }
    size_t width = 0;
    if (result.dtype == "<f8" || result.dtype == "<i8") width = 8;
    else if (result.dtype == "|u1") width = 1;
    else if (result.dtype == "<u4") width = 4;
    else invalid();
    result.offset = prefix+length;
    if (elements > (bytes.size()-result.offset)/width ||
            elements*width != bytes.size()-result.offset) invalid();
    result.bytes = std::move(bytes);
    return result;
}
} // namespace

CexDepthTape load_cex_depth(const std::string& path) {
    if (std::filesystem::path(path).extension() != ".npz")
        throw std::invalid_argument("CEX depth requires NPZ; convert JSONL with fxopt.depth_archive");
    const auto close = [](void* file) { if (file) unzClose(file); };
    std::unique_ptr<void, decltype(close)> file(unzOpen64(path.c_str()), close);
    if (!file) throw std::runtime_error("Cannot open CEX depth NPZ: "+path);
    const std::array<std::string, 5> names = {
        "format_version.npy", "depth.npy", "timestamps.npy", "counts.npy", "interpolation.npy"};
    std::map<std::string, Array> arrays;
    int status = unzGoToFirstFile(file.get());
    while (status == UNZ_OK) {
        unz_file_info64 info{};
        std::array<char, 128> name{};
        if (unzGetCurrentFileInfo64(file.get(), &info, name.data(), name.size(),
                                   nullptr, 0, nullptr, 0) != UNZ_OK
                || info.size_filename >= name.size() || (info.flag & 1)) invalid();
        const std::string key(name.data());
        if (std::find(names.begin(), names.end(), key) == names.end() || arrays.count(key)
                || info.uncompressed_size > std::numeric_limits<size_t>::max()
                || unzOpenCurrentFile(file.get()) != UNZ_OK) invalid();
        std::vector<unsigned char> bytes(static_cast<size_t>(info.uncompressed_size));
        size_t offset = 0;
        while (offset < bytes.size()) {
            const auto size = static_cast<unsigned>(std::min(size_t(1 << 20), bytes.size()-offset));
            const int got = unzReadCurrentFile(file.get(), bytes.data()+offset, size);
            if (got <= 0) invalid();
            offset += static_cast<size_t>(got);
        }
        unsigned char extra{};
        const int tail = unzReadCurrentFile(file.get(), &extra, 1);
        const int crc = unzCloseCurrentFile(file.get());
        if (tail != 0 || crc != UNZ_OK) invalid();
        arrays.emplace(key, parse(std::move(bytes)));
        status = unzGoToNextFile(file.get());
    }
    if (status != UNZ_END_OF_LIST_OF_FILE || (arrays.size() != 4 && arrays.size() != 5)) invalid();
    const auto& version = arrays.at("format_version.npy");
    const auto& depth = arrays.at("depth.npy");
    const auto& times = arrays.at("timestamps.npy");
    const auto& counts = arrays.at("counts.npy");
    if (version.dtype != "<u4" || !version.shape.empty()) invalid();
    const auto revision = little(version.data(), 4);
    if (revision != 1 && revision != 2) invalid();
    if (depth.dtype != "<f8" || depth.shape.size() != 4 || depth.shape[1] != 2
            || depth.shape[2] > 4096 || depth.shape[3] != 2) invalid();
    const size_t rows = depth.shape[0], width = depth.shape[2];
    const size_t count_bytes = revision == 1 ? 1 : 4;
    if (times.dtype != "<i8" || times.shape != std::vector<size_t>{rows}
            || counts.dtype != (revision == 1 ? "|u1" : "<u4")
            || counts.shape != std::vector<size_t>{rows, 2}) invalid();
    const Array* modes = nullptr;
    if (revision == 1) {
        if (arrays.size() != 4 || width != 16) invalid();
    } else {
        if (arrays.size() != 5 || !arrays.count("interpolation.npy")) invalid();
        modes = &arrays.at("interpolation.npy");
        if (modes->dtype != "|u1" || modes->shape != std::vector<size_t>{rows}) invalid();
    }
    std::vector<trading::CexDepthSnapshot> snapshots;
    snapshots.reserve(rows);
    uint64_t previous = 0;
    for (size_t row = 0; row < rows; ++row) {
        const auto timestamp = little(times.data()+row*8, 8);
        if (!timestamp || timestamp > INT64_MAX || (row &&
                (timestamp <= previous || (revision == 1 && timestamp-previous != 10'000'000'000ULL)))) invalid();
        previous = timestamp;
        trading::CexDepthSnapshot snapshot;
        snapshot.available_ns = timestamp;
        if (modes) {
            if (modes->data()[row] > 1) invalid();
            snapshot.interpolation = modes->data()[row] == 0
                ? trading::DepthInterpolation::Step : trading::DepthInterpolation::Linear;
        }
        for (size_t side = 0; side < 2; ++side) {
            const auto count = little(counts.data()+(row*2+side)*count_bytes, count_bytes);
            if (count < 1 || count > width) invalid();
            auto& levels = side == 0 ? snapshot.bids : snapshot.asks;
            levels.reserve(count);
            for (size_t level = 0; level < width; ++level) {
                std::array<double, 2> pair{};
                for (size_t component = 0; component < 2; ++component) {
                    const size_t index = ((row*2+side)*width+level)*2+component;
                    const auto bits = little(depth.data()+index*8, 8);
                    std::memcpy(&pair[component], &bits, 8);
                }
                if (level < count) levels.push_back({pair[0], pair[1]});
                else if (pair[0] != 0 || pair[1] != 0) invalid();
            }
        }
        if (revision == 1 && snapshot.bids.front().price >= snapshot.asks.front().price) invalid();
        snapshots.push_back(std::move(snapshot));
    }
    return CexDepthTape(std::move(snapshots));
}
} // namespace arb::events
