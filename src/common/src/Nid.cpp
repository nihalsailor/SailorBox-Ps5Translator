#include "common/Nid.hpp"
#include <array>
#include <vector>
#include <format>
#include <stdexcept>
#include <cstring>

namespace Sailor {

namespace {

constexpr std::string_view NID_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";

constexpr u8 SONY_NID_SALT[16] = {
    0x51, 0x8D, 0x64, 0xA6, 0x35, 0xDE, 0xD8, 0xC1,
    0xE6, 0xB0, 0x39, 0xB1, 0xC3, 0xE5, 0x52, 0x30
};

int DecodeChar(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '_') return 62;
    if (c == '-' || c == '/') return 63;
    return -1;
}

// Lightweight self-contained SHA-1 implementation
inline u32 LeftRotate(u32 value, u32 count) {
    return (value << count) | (value >> (32 - count));
}

std::array<u8, 20> Sha1(const u8* data, size_t length) {
    u32 h0 = 0x67452301;
    u32 h1 = 0xEFCDAB89;
    u32 h2 = 0x98BADCFE;
    u32 h3 = 0x10325476;
    u32 h4 = 0xC3D2E1F0;

    // Pad data
    std::vector<u8> msg(data, data + length);
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) {
        msg.push_back(0x00);
    }
    const u64 bitLength = static_cast<u64>(length) * 8;
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<u8>((bitLength >> (i * 8)) & 0xFF));
    }

    // Process blocks
    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        u32 w[80]{};
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<u32>(msg[chunk + i * 4]) << 24) |
                   (static_cast<u32>(msg[chunk + i * 4 + 1]) << 16) |
                   (static_cast<u32>(msg[chunk + i * 4 + 2]) << 8) |
                   (static_cast<u32>(msg[chunk + i * 4 + 3]));
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = LeftRotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        u32 a = h0;
        u32 b = h1;
        u32 c = h2;
        u32 d = h3;
        u32 e = h4;

        for (int i = 0; i < 80; ++i) {
            u32 f = 0, k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }

            u32 temp = LeftRotate(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = LeftRotate(b, 30);
            b = a;
            a = temp;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    std::array<u8, 20> digest{};
    const u32 h[5] = { h0, h1, h2, h3, h4 };
    for (int i = 0; i < 5; ++i) {
        digest[i * 4]     = static_cast<u8>((h[i] >> 24) & 0xFF);
        digest[i * 4 + 1] = static_cast<u8>((h[i] >> 16) & 0xFF);
        digest[i * 4 + 2] = static_cast<u8>((h[i] >> 8) & 0xFF);
        digest[i * 4 + 3] = static_cast<u8>(h[i] & 0xFF);
    }
    return digest;
}

} // namespace

Nid::Nid(std::string_view encodedString) {
    if (encodedString.size() != 11) {
        if (encodedString.starts_with("0x") || encodedString.starts_with("0X")) {
            m_value = std::stoull(std::string(encodedString), nullptr, 16);
            return;
        }
        throw std::invalid_argument(std::format("Invalid NID string length: {}", encodedString.size()));
    }

    u8 rawBytes[8]{};
    u64 accumulator = 0;
    int bits = 0;
    int bytesWritten = 0;

    for (char c : encodedString) {
        int val = DecodeChar(c);
        if (val < 0) {
            throw std::invalid_argument(std::format("Invalid character in NID: '{}'", c));
        }
        accumulator = (accumulator << 6) | static_cast<u64>(val);
        bits += 6;
        while (bits >= 8 && bytesWritten < 8) {
            bits -= 8;
            rawBytes[bytesWritten++] = static_cast<u8>((accumulator >> bits) & 0xFF);
        }
    }

    // Big-endian u64 representation from the decoded 8 bytes
    u64 result = 0;
    for (int i = 0; i < 8; ++i) {
        result |= (static_cast<u64>(rawBytes[i]) << ((7 - i) * 8));
    }
    m_value = result;
}

std::string Nid::ToString() const {
    std::string out;
    out.reserve(11);

    // Extract bytes in big-endian order (digest byte 0 to 7)
    u8 rawBytes[8]{};
    for (int i = 0; i < 8; ++i) {
        rawBytes[i] = static_cast<u8>((m_value >> ((7 - i) * 8)) & 0xFF);
    }

    // Stream the 8 bytes big-endian into 6-bit Base64 characters
    u64 accumulator = 0;
    int bits = 0;
    for (int i = 0; i < 8; ++i) {
        accumulator = (accumulator << 8) | rawBytes[i];
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out.push_back(NID_CHARS[(accumulator >> bits) & 0x3F]);
        }
    }
    if (bits > 0) {
        out.push_back(NID_CHARS[(accumulator << (6 - bits)) & 0x3F]);
    }
    return out;
}

Nid Nid::Compute(std::string_view symbolName) {
    // Sony algorithm: SHA-1(symbolName + SONY_NID_SALT)
    std::vector<u8> input(symbolName.begin(), symbolName.end());
    input.insert(input.end(), std::begin(SONY_NID_SALT), std::end(SONY_NID_SALT));

    const auto digest = Sha1(input.data(), input.size());

    // First 8 bytes of the SHA-1 digest as little-endian u64
    u64 val = 0;
    for (int i = 0; i < 8; ++i) {
        val |= (static_cast<u64>(digest[i]) << (i * 8));
    }
    return Nid(val);
}

} // namespace Sailor
