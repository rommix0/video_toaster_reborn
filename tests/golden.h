#pragma once
// Reader for the golden JSON-lines files written by tools/make_golden.py.
//
// The files hold one flat JSON object per line. Values are exactly one of:
// integer, string, array of integers, object of string -> integer, and (dve.jsonl
// "frames") array of such objects. Nothing else is parsed.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#ifndef VT_DATA_ROOT
#error "VT_DATA_ROOT must be defined (the Toaster data tree)"
#endif
#ifndef VT_GOLDEN_DIR
#error "VT_GOLDEN_DIR must be defined"
#endif

namespace golden {

struct Value {
    std::variant<int64_t, std::string, std::vector<int64_t>, std::map<std::string, int64_t>,
                 std::vector<std::map<std::string, int64_t>>>
        v;
};
using Record = std::map<std::string, Value>;

}  // namespace golden

// The Toaster data tree (the folder holding 4.2/, 3.1/, converted/).
inline std::filesystem::path dataRoot() { return std::filesystem::path(VT_DATA_ROOT).lexically_normal(); }

inline std::vector<uint8_t> readFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + p.string());
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

namespace golden {

using ::dataRoot;
using ::readFile;

// A "source" field (UTF-8, '/'-separated, relative to the data root) as a path.
inline std::filesystem::path sourcePath(const std::string& utf8) {
    return dataRoot() / std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

namespace detail {

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}

    Record record() {
        Record r;
        ws();
        expect('{');
        ws();
        if (peek() == '}') { ++i_; return r; }
        for (;;) {
            ws();
            std::string key = string();
            ws();
            expect(':');
            r.emplace(std::move(key), value());
            ws();
            if (peek() == ',') { ++i_; continue; }
            expect('}');
            return r;
        }
    }

    bool atEnd() { ws(); return i_ >= s_.size(); }

private:
    const std::string& s_;
    size_t i_ = 0;

    [[noreturn]] void fail(const char* what) const {
        throw std::runtime_error(std::string("golden parse error: ") + what + " at offset " + std::to_string(i_));
    }
    char peek() const { return i_ < s_.size() ? s_[i_] : '\0'; }
    void ws() { while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\r' || s_[i_] == '\n')) ++i_; }
    void expect(char c) { if (peek() != c) fail("unexpected character"); ++i_; }

    int64_t integer() {
        size_t j = i_;
        if (peek() == '-') ++j;
        if (j >= s_.size() || s_[j] < '0' || s_[j] > '9') fail("expected integer");
        int64_t v = 0;
        bool neg = peek() == '-';
        i_ = j;
        while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') v = v * 10 + (s_[i_++] - '0');
        return neg ? -v : v;
    }

    static void utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F));
        }
    }

    uint32_t hex4() {
        if (i_ + 4 > s_.size()) fail("short \\u escape");
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s_[i_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
            else fail("bad \\u escape");
        }
        return v;
    }

    std::string string() {
        expect('"');
        std::string out;
        for (;;) {
            if (i_ >= s_.size()) fail("unterminated string");
            char c = s_[i_++];
            if (c == '"') return out;
            if (c != '\\') { out += c; continue; }
            if (i_ >= s_.size()) fail("unterminated escape");
            switch (char e = s_[i_++]) {
                case '"': case '\\': case '/': out += e; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp = hex4();
                    if (cp >= 0xD800 && cp < 0xDC00 && s_.compare(i_, 2, "\\u") == 0) {
                        i_ += 2;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (hex4() - 0xDC00);
                    }
                    utf8(out, cp);
                    break;
                }
                default: fail("bad escape");
            }
        }
    }

    std::map<std::string, int64_t> intObject() {
        std::map<std::string, int64_t> m;
        expect('{');
        ws();
        if (peek() == '}') { ++i_; return m; }
        for (;;) {
            ws();
            std::string key = string();
            ws();
            expect(':');
            ws();
            m[std::move(key)] = integer();
            ws();
            if (peek() == ',') { ++i_; continue; }
            expect('}');
            return m;
        }
    }

    Value value() {
        ws();
        Value out;
        char c = peek();
        if (c == '"') {
            out.v = string();
        } else if (c == '{') {
            out.v = intObject();
        } else if (c == '[') {
            ++i_;
            ws();
            if (peek() == ']') { ++i_; out.v = std::vector<int64_t>{}; return out; }
            if (peek() == '{') {
                std::vector<std::map<std::string, int64_t>> objs;
                for (;;) {
                    ws();
                    objs.push_back(intObject());
                    ws();
                    if (peek() == ',') { ++i_; continue; }
                    expect(']');
                    break;
                }
                out.v = std::move(objs);
            } else {
                std::vector<int64_t> ints;
                for (;;) {
                    ws();
                    ints.push_back(integer());
                    ws();
                    if (peek() == ',') { ++i_; continue; }
                    expect(']');
                    break;
                }
                out.v = std::move(ints);
            }
        } else {
            out.v = integer();
        }
        return out;
    }
};

}  // namespace detail

// Parse every non-empty line of VT_GOLDEN_DIR/<fileName>.
inline std::vector<Record> readLines(const std::string& fileName) {
    const auto path = std::filesystem::path(VT_GOLDEN_DIR) / fileName;
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open golden file " + path.string());
    std::vector<Record> out;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        detail::Parser p(line);
        out.push_back(p.record());
        if (!p.atEnd()) throw std::runtime_error("golden parse error: trailing data in " + fileName);
    }
    return out;
}

}  // namespace golden
