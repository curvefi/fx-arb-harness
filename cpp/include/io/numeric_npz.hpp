// Strict numeric C-order NPY arrays in ZIP/DEFLATE archives. No Python runtime.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
namespace arb::io {
struct Array {
    std::vector<unsigned char> bytes;
    std::string dtype;
    std::vector<size_t> shape;
    size_t offset{};
    const unsigned char* data() const { return bytes.data()+offset; }
};
uint64_t little(const unsigned char* p, size_t width);
std::map<std::string, Array> read_arrays(const std::string& path,
                                          const std::vector<std::string>& allowed_names);
} // namespace arb::io
