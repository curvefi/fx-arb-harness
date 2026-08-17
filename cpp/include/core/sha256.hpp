#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>

#if defined(ARB_SHA256_COMMONCRYPTO)
#include <CommonCrypto/CommonDigest.h>
#elif defined(ARB_SHA256_OPENSSL)
#include <openssl/evp.h>
#endif

namespace arb {
namespace crypto {

using Sha256Digest = std::array<unsigned char, 32>;

namespace detail {

inline uint32_t rotr(uint32_t value, unsigned amount) {
    return (value >> amount) | (value << (32U - amount));
}

struct PortableSha256 {
    static constexpr uint32_t K[64] = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
        0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
        0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
        0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
    };

    uint32_t state[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };
    uint64_t bit_length = 0;
    uint8_t buffer[64]{};
    size_t buffer_length = 0;

    void update(const uint8_t* data, size_t length) {
        bit_length += static_cast<uint64_t>(length) * 8ULL;
        while (length != 0) {
            const size_t take = std::min(length, sizeof(buffer) - buffer_length);
            std::memcpy(buffer + buffer_length, data, take);
            buffer_length += take;
            data += take;
            length -= take;
            if (buffer_length == sizeof(buffer)) {
                transform(buffer);
                buffer_length = 0;
            }
        }
    }

    Sha256Digest finish() {
        buffer[buffer_length++] = 0x80U;
        if (buffer_length > 56) {
            std::memset(buffer + buffer_length, 0, sizeof(buffer) - buffer_length);
            transform(buffer);
            buffer_length = 0;
        }
        std::memset(buffer + buffer_length, 0, 56 - buffer_length);
        for (size_t index = 0; index < 8; ++index) {
            buffer[56 + index] = static_cast<uint8_t>(
                (bit_length >> ((7U - index) * 8U)) & 0xffULL
            );
        }
        transform(buffer);

        Sha256Digest digest{};
        for (size_t word = 0; word < 8; ++word) {
            for (size_t byte = 0; byte < 4; ++byte) {
                digest[word * 4 + byte] = static_cast<unsigned char>(
                    (state[word] >> ((3U - byte) * 8U)) & 0xffU
                );
            }
        }
        return digest;
    }

    void transform(const uint8_t* chunk) {
        uint32_t w[64]{};
        for (size_t index = 0; index < 16; ++index) {
            w[index] = (static_cast<uint32_t>(chunk[index * 4]) << 24) |
                (static_cast<uint32_t>(chunk[index * 4 + 1]) << 16) |
                (static_cast<uint32_t>(chunk[index * 4 + 2]) << 8) |
                static_cast<uint32_t>(chunk[index * 4 + 3]);
        }
        for (size_t index = 16; index < 64; ++index) {
            const uint32_t s0 = rotr(w[index - 15], 7) ^
                rotr(w[index - 15], 18) ^ (w[index - 15] >> 3);
            const uint32_t s1 = rotr(w[index - 2], 17) ^
                rotr(w[index - 2], 19) ^ (w[index - 2] >> 10);
            w[index] = w[index - 16] + s0 + w[index - 7] + s1;
        }

        uint32_t a = state[0];
        uint32_t b = state[1];
        uint32_t c = state[2];
        uint32_t d = state[3];
        uint32_t e = state[4];
        uint32_t f = state[5];
        uint32_t g = state[6];
        uint32_t h = state[7];

        for (size_t index = 0; index < 64; ++index) {
            const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t temp1 = h + s1 + ch + K[index] + w[index];
            const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = s0 + maj;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }
};

} // namespace detail

inline Sha256Digest sha256(const void* bytes, std::size_t size) {
    Sha256Digest digest{};
#if defined(ARB_SHA256_COMMONCRYPTO)
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    const auto* cursor = static_cast<const unsigned char*>(bytes);
    while (size != 0) {
        const CC_LONG chunk = static_cast<CC_LONG>(
            std::min<std::size_t>(size, std::numeric_limits<CC_LONG>::max())
        );
        CC_SHA256_Update(&context, cursor, chunk);
        cursor += chunk;
        size -= chunk;
    }
    CC_SHA256_Final(digest.data(), &context);
#elif defined(ARB_SHA256_OPENSSL)
    unsigned int digest_size = 0;
    if (EVP_Digest(
            bytes, size, digest.data(), &digest_size, EVP_sha256(), nullptr
        ) != 1 || digest_size != digest.size()) {
        return {};
    }
#else
    detail::PortableSha256 state;
    state.update(static_cast<const unsigned char*>(bytes), size);
    digest = state.finish();
#endif
    return digest;
}

inline Sha256Digest sha256(const std::string& str) {
    return sha256(str.data(), str.size());
}

inline std::string sha256_hex(const Sha256Digest& digest) {
    std::ostringstream oss;
    for (unsigned char b : digest) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    return oss.str();
}

inline std::string sha256_hex(const void* bytes, std::size_t size) {
    return sha256_hex(sha256(bytes, size));
}

inline std::string sha256_hex(const std::string& str) {
    return sha256_hex(str.data(), str.size());
}

} // namespace crypto

namespace core {

// File content SHA-256 (hex) with robust read handling.
inline std::string sha256_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::string(64, '0');
    }
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    return crypto::sha256_hex(content.data(), content.size());
}

// Bytes/string SHA-256 (hex).
inline std::string sha256_bytes(const std::string& bytes) {
    return crypto::sha256_hex(bytes.data(), bytes.size());
}

} // namespace core
} // namespace arb
