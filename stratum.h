#pragma once

#include "sha256.h"
#include <array>
#include <map>
#include <stdexcept>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace stratum {

using Hash = std::array<uint8_t, 32>;

struct Json {
    enum class Type { Null, Boolean, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string text;
    std::vector<Json> array;
    std::map<std::string, Json> object;

    const Json& at(const std::string& key) const {
        if (type != Type::Object || !object.count(key))
            throw std::runtime_error("Missing Stratum JSON field: " + key);
        return object.at(key);
    }
    const Json& at(size_t index) const {
        if (type != Type::Array || index >= array.size())
            throw std::runtime_error("Missing Stratum array element");
        return array[index];
    }
    std::string string() const {
        if (type != Type::String) throw std::runtime_error("Expected a Stratum string");
        return text;
    }
    double numeric() const {
        if (type != Type::Number) throw std::runtime_error("Expected a Stratum number");
        return number;
    }
    bool flag() const {
        if (type != Type::Boolean) throw std::runtime_error("Expected a Stratum boolean");
        return boolean;
    }
    bool null() const { return type == Type::Null; }
};

class JsonReader {
    const std::string& input;
    size_t pos = 0;
    size_t nodes = 0;

    void space() {
        while (pos < input.size() && (input[pos] == ' ' || input[pos] == '\r' ||
               input[pos] == '\n' || input[pos] == '\t')) ++pos;
    }
    bool take(char c) {
        space();
        if (pos < input.size() && input[pos] == c) { ++pos; return true; }
        return false;
    }
    [[noreturn]] void invalid() const { throw std::runtime_error("Malformed Stratum JSON"); }
    std::string string() {
        if (!take('"')) invalid();
        std::string out;
        while (pos < input.size()) {
            unsigned char c = input[pos++];
            if (c == '"') return out;
            if (c < 32) invalid();
            if (c == '\\') {
                if (pos == input.size()) invalid();
                c = input[pos++];
                switch (c) {
                case '"': case '\\': case '/': break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': {
                    unsigned value = 0;
                    for (int i = 0; i < 4; ++i) {
                        if (pos == input.size()) invalid();
                        char digit = input[pos++];
                        unsigned nibble;
                        if (digit >= '0' && digit <= '9') nibble = digit - '0';
                        else if (digit >= 'a' && digit <= 'f') nibble = digit - 'a' + 10;
                        else if (digit >= 'A' && digit <= 'F') nibble = digit - 'A' + 10;
                        else { invalid(); }
                        value = value * 16 + nibble;
                    }
                    // Stratum identifiers and hexadecimal fields are ASCII.
                    if (value > 127) throw std::runtime_error("Non-ASCII Stratum identifier is unsupported");
                    c = static_cast<unsigned char>(value);
                    break;
                }
                default: invalid();
                }
            }
            out += static_cast<char>(c);
        }
        invalid();
    }
    Json value(int depth) {
        if (depth > 24 || ++nodes > 20000) throw std::runtime_error("Stratum JSON limit exceeded");
        space();
        if (pos == input.size()) invalid();
        Json out;
        char c = input[pos];
        if (c == '"') {
            out.type = Json::Type::String;
            out.text = string();
        } else if (c == '[' || c == '{') {
            ++pos;
            bool obj = c == '{';
            out.type = obj ? Json::Type::Object : Json::Type::Array;
            char close = obj ? '}' : ']';
            if (take(close)) return out;
            do {
                if (obj) {
                    std::string key = string();
                    if (!take(':') || out.object.count(key)) invalid();
                    out.object.emplace(key, value(depth + 1));
                } else out.array.push_back(value(depth + 1));
            } while (take(','));
            if (!take(close)) invalid();
        } else if (c == 'n' || c == 't' || c == 'f') {
            std::string literal = c == 'n' ? "null" : c == 't' ? "true" : "false";
            if (input.compare(pos, literal.size(), literal) != 0) invalid();
            pos += literal.size();
            if (c != 'n') {
                out.type = Json::Type::Boolean;
                out.boolean = c == 't';
            }
        } else {
            size_t start = pos;
            if (input[pos] == '-') ++pos;
            if (pos == input.size()) invalid();
            if (input[pos] == '0') ++pos;
            else {
                if (input[pos] < '1' || input[pos] > '9') invalid();
                while (pos < input.size() && input[pos] >= '0' && input[pos] <= '9') ++pos;
            }
            if (pos < input.size() && input[pos] == '.') {
                size_t first = ++pos;
                while (pos < input.size() && input[pos] >= '0' && input[pos] <= '9') ++pos;
                if (first == pos) invalid();
            }
            if (pos < input.size() && (input[pos] == 'e' || input[pos] == 'E')) {
                ++pos;
                if (pos < input.size() && (input[pos] == '+' || input[pos] == '-')) ++pos;
                size_t first = pos;
                while (pos < input.size() && input[pos] >= '0' && input[pos] <= '9') ++pos;
                if (first == pos) invalid();
            }
            std::string token = input.substr(start, pos - start);
            out.type = Json::Type::Number;
            out.number = std::strtod(token.c_str(), nullptr);
            if (!std::isfinite(out.number)) invalid();
        }
        return out;
    }
public:
    explicit JsonReader(const std::string& text) : input(text) {}
    Json parse() {
        if (input.size() > 262144) throw std::runtime_error("Stratum message is too large");
        Json out = value(0);
        space();
        if (pos != input.size()) invalid();
        return out;
    }
};

inline Json parse(const std::string& text) { return JsonReader(text).parse(); }

inline std::string quote(const std::string& text) {
    static const char* hex = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c < 32) {
            out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15];
        } else out += c;
    }
    return out + '"';
}

inline std::vector<uint8_t> hex(const std::string& text, size_t maxBytes = 65536) {
    if (text.size() % 2 || text.size() / 2 > maxBytes)
        throw std::runtime_error("Invalid Stratum hex length");
    auto nibble = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw std::runtime_error("Invalid Stratum hex character");
    };
    std::vector<uint8_t> out;
    out.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2)
        out.push_back(static_cast<uint8_t>((nibble(text[i]) << 4) | nibble(text[i + 1])));
    return out;
}

inline std::string hexText(const uint8_t* bytes, size_t size) {
    static const char* digits = "0123456789abcdef";
    std::string text;
    text.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        text += digits[bytes[i] >> 4];
        text += digits[bytes[i] & 15];
    }
    return text;
}

inline uint32_t word(const std::string& text) {
    auto bytes = hex(text, 4);
    if (bytes.size() != 4) throw std::runtime_error("Stratum header word must be 8 hex digits");
    uint32_t value = 0;
    for (uint8_t b : bytes) value = (value << 8) | b;
    return value;
}

inline void writeLE(uint8_t* out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(value >> (i * 8));
}

inline std::string wordText(uint32_t value) {
    uint8_t bytes[4];
    for (int i = 0; i < 4; ++i) bytes[i] = static_cast<uint8_t>(value >> ((3 - i) * 8));
    return hexText(bytes, 4);
}

inline Hash doubleHash(const uint8_t* bytes, size_t length) {
    Hash first{}, out{};
    sha256(bytes, length, first.data());
    sha256(first.data(), first.size(), out.data());
    return out;
}

inline Hash compactTarget(uint32_t compact) {
    unsigned size = compact >> 24;
    uint32_t mantissa = compact & 0x7fffff;
    if ((compact & 0x800000) || !mantissa || size > 34 ||
        (size > 33 && mantissa > 0xff) || (size > 32 && mantissa > 0xffff))
        throw std::runtime_error("Invalid or overflowing Bitcoin target");
    Hash target{};
    if (size <= 3) {
        mantissa >>= 8 * (3 - size);
        for (int i = 0; i < 4; ++i) target[31 - i] = static_cast<uint8_t>(mantissa >> (8 * i));
    } else {
        for (int i = 0; i < 3; ++i) {
            int index = 32 - static_cast<int>(size) + i;
            if (index >= 0) target[index] = static_cast<uint8_t>(mantissa >> (8 * (2 - i)));
        }
    }
    if (std::all_of(target.begin(), target.end(), [](uint8_t b) { return b == 0; }))
        throw std::runtime_error("Bitcoin target is zero");
    return target;
}

inline Hash difficultyTarget(double difficulty) {
    if (!std::isfinite(difficulty) || difficulty < 1e-12 || difficulty > 1e30)
        throw std::runtime_error("Unsupported Stratum share difficulty");
    int exponent;
    uint64_t divisor = static_cast<uint64_t>(std::ldexp(std::frexp(difficulty, &exponent), 53));
    int shift = 53 - exponent;
    Hash base = compactTarget(0x1d00ffff), out{};
    uint64_t remainder = 0;
    // Binary long division preserves all 256 target bits, including fractional difficulties.
    for (int bit = 255 + shift; bit >= 0; --bit) {
        int source = bit - shift;
        unsigned value = source >= 0 && source < 256 ? (base[31 - source / 8] >> (source % 8)) & 1 : 0;
        remainder = (remainder << 1) | value;
        if (remainder >= divisor) {
            remainder -= divisor;
            if (bit >= 256) { out.fill(0xff); return out; }
            out[31 - bit / 8] |= static_cast<uint8_t>(1 << (bit % 8));
        }
    }
    return out;
}

struct Subscription {
    std::vector<uint8_t> extra1;
    size_t extra2Size = 0;
};

inline Subscription subscription(const Json& result) {
    if (result.type != Json::Type::Array || result.array.size() != 3 ||
        result.at(0).type != Json::Type::Array)
        throw std::runtime_error("Invalid subscription response");
    Subscription out;
    out.extra1 = hex(result.at(1).string(), 32);
    double size = result.at(2).numeric();
    if (size < 1 || size > 16 || std::floor(size) != size)
        throw std::runtime_error("Unsupported extranonce2 size");
    out.extra2Size = static_cast<size_t>(size);
    return out;
}

struct Job {
    std::string id, time;
    std::vector<uint8_t> prev, prefix, suffix;
    std::vector<Hash> branches;
    uint32_t version = 0, bits = 0;
    bool clean = false;
    Hash shareTarget{}, networkTarget{};
};

inline Job job(const Json& params, double difficulty) {
    if (params.type != Json::Type::Array || params.array.size() != 9)
        throw std::runtime_error("Invalid mining.notify job");
    Job out;
    out.id = params.at(0).string();
    if (out.id.empty() || out.id.size() > 256) throw std::runtime_error("Invalid job identifier");
    out.prev = hex(params.at(1).string(), 32);
    if (out.prev.size() != 32) throw std::runtime_error("Invalid previous block hash");
    out.prefix = hex(params.at(2).string());
    out.suffix = hex(params.at(3).string());
    const Json& branches = params.at(4);
    if (branches.type != Json::Type::Array || branches.array.size() > 32)
        throw std::runtime_error("Invalid merkle branches");
    for (const Json& branch : branches.array) {
        auto bytes = hex(branch.string(), 32);
        if (bytes.size() != 32) throw std::runtime_error("Invalid merkle branch hash");
        Hash hash{};
        std::copy(bytes.begin(), bytes.end(), hash.begin());
        out.branches.push_back(hash);
    }
    out.version = word(params.at(5).string());
    out.bits = word(params.at(6).string());
    out.time = params.at(7).string();
    word(out.time);
    out.clean = params.at(8).flag();
    out.networkTarget = compactTarget(out.bits);
    if (out.networkTarget > compactTarget(0x1d00ffff))
        throw std::runtime_error("Job has a target above Bitcoin mainnet's proof-of-work limit");
    out.shareTarget = difficultyTarget(difficulty);
    return out;
}

inline std::array<uint8_t, 80> header(const Job& job, const Subscription& sub,
                                      const std::vector<uint8_t>& extra2) {
    if (extra2.size() != sub.extra2Size) throw std::runtime_error("Incorrect extranonce2 length");
    std::vector<uint8_t> coinbase = job.prefix;
    coinbase.insert(coinbase.end(), sub.extra1.begin(), sub.extra1.end());
    coinbase.insert(coinbase.end(), extra2.begin(), extra2.end());
    coinbase.insert(coinbase.end(), job.suffix.begin(), job.suffix.end());
    Hash root = doubleHash(coinbase.data(), coinbase.size());
    for (const Hash& branch : job.branches) {
        uint8_t pair[64];
        memcpy(pair, root.data(), 32);
        memcpy(pair + 32, branch.data(), 32);
        root = doubleHash(pair, sizeof(pair));
    }
    std::array<uint8_t, 80> out{};
    writeLE(out.data(), job.version);
    // Stratum V1's previous hash uses reversed bytes within each 32-bit word.
    for (int i = 0; i < 32; ++i) out[4 + i] = job.prev[(i / 4) * 4 + 3 - i % 4];
    std::copy(root.begin(), root.end(), out.begin() + 36);
    writeLE(out.data() + 68, word(job.time));
    writeLE(out.data() + 72, job.bits);
    return out;
}

inline bool incrementExtra(std::vector<uint8_t>& extra) {
    for (auto it = extra.rbegin(); it != extra.rend(); ++it)
        if (++*it != 0) return true;
    return false;
}

inline std::string submit(uint32_t id, const std::string& address, const Job& job,
                          const std::vector<uint8_t>& extra2, uint32_t nonce) {
    return "{\"id\":" + std::to_string(id) + ",\"method\":\"mining.submit\",\"params\":[" +
           quote(address) + "," + quote(job.id) + "," + quote(hexText(extra2.data(), extra2.size())) +
           "," + quote(job.time) + "," + quote(wordText(nonce)) + "]}\n";
}

} // namespace stratum
