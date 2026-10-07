// Shared numeric C-order NPZ decoder. MiniZip handles ZIP/DEFLATE/CRC.
#include "io/numeric_npz.hpp"

#include <array>
#include <algorithm>
#include <charconv>
#include <cstring>
#include <map>
#include <memory>
#include <limits>
#include <regex>
#include <sstream>
#include <minizip/unzip.h>

namespace arb::io {
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);

[[noreturn]] static void invalid() {
    throw std::runtime_error("Invalid numeric NPZ");
}

uint64_t little(const unsigned char* p, size_t n) {
    uint64_t value = 0;
    for (size_t i = 0; i < n; ++i) value |= uint64_t(p[i]) << (8*i);
    return value;
}



static Array parse(std::vector<unsigned char> bytes) {
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

std::map<std::string, Array> read_arrays(const std::string& path, const std::vector<std::string>& names) {
    const auto close = [](void* file) { if (file) unzClose(file); };
    std::unique_ptr<void, decltype(close)> file(unzOpen64(path.c_str()), close);
    if (!file) throw std::runtime_error("Cannot open numeric NPZ: "+path);
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
    if (status != UNZ_END_OF_LIST_OF_FILE) invalid();
    return arrays;
}

} // namespace arb::io
