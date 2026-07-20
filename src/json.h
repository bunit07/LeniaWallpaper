// Minimal JSON parser (DOM style). Supports the full JSON grammar; enough for
// the species catalog and config files. No external dependencies.
#pragma once
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace json {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object };
    Type type = Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    const Value* find(const char* key) const {
        if (type != Object) return nullptr;
        for (const auto& kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    double numOr(double def) const { return type == Number ? num : def; }
    bool boolOr(bool def) const { return type == Bool ? b : def; }
    const std::string& strOr(const std::string& def) const { return type == String ? str : def; }
    double fieldNum(const char* key, double def) const {
        const Value* v = find(key);
        return v ? v->numOr(def) : def;
    }
    bool fieldBool(const char* key, bool def) const {
        const Value* v = find(key);
        return v ? v->boolOr(def) : def;
    }
    std::string fieldStr(const char* key, const std::string& def) const {
        const Value* v = find(key);
        return v ? v->strOr(def) : def;
    }
};

class Parser {
public:
    // Returns true on success; on failure *err holds a short description.
    static bool parse(const char* text, size_t len, Value* out, std::string* err) {
        Parser p(text, len);
        if (!p.parseValue(*out)) { if (err) *err = p.err_; return false; }
        p.skipWs();
        if (p.pos_ != p.len_) { if (err) *err = "trailing characters"; return false; }
        return true;
    }

private:
    Parser(const char* t, size_t n) : t_(t), len_(n) {}

    const char* t_;
    size_t len_, pos_ = 0;
    std::string err_;

    bool fail(const char* m) { err_ = m; return false; }
    char peek() const { return pos_ < len_ ? t_[pos_] : '\0'; }
    void skipWs() {
        while (pos_ < len_) {
            char c = t_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') pos_++;
            else break;
        }
    }

    bool parseValue(Value& v) {
        skipWs();
        switch (peek()) {
        case '{': return parseObject(v);
        case '[': return parseArray(v);
        case '"': v.type = Value::String; return parseString(v.str);
        case 't':
            if (len_ - pos_ >= 4 && !memcmp(t_ + pos_, "true", 4)) { pos_ += 4; v.type = Value::Bool; v.b = true; return true; }
            return fail("bad literal");
        case 'f':
            if (len_ - pos_ >= 5 && !memcmp(t_ + pos_, "false", 5)) { pos_ += 5; v.type = Value::Bool; v.b = false; return true; }
            return fail("bad literal");
        case 'n':
            if (len_ - pos_ >= 4 && !memcmp(t_ + pos_, "null", 4)) { pos_ += 4; v.type = Value::Null; return true; }
            return fail("bad literal");
        default: return parseNumber(v);
        }
    }

    bool parseNumber(Value& v) {
        char* end = nullptr;
        double d = strtod(t_ + pos_, &end);
        if (end == t_ + pos_) return fail("bad number");
        pos_ = static_cast<size_t>(end - t_);
        if (pos_ > len_) return fail("number past end");
        v.type = Value::Number;
        v.num = d;
        return true;
    }

    bool parseString(std::string& out) {
        if (peek() != '"') return fail("expected string");
        pos_++;
        out.clear();
        while (pos_ < len_) {
            char c = t_[pos_++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (pos_ >= len_) break;
            char e = t_[pos_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                if (len_ - pos_ < 4) return fail("bad \\u escape");
                unsigned cp = 0;
                for (int i = 0; i < 4; i++) {
                    char h = t_[pos_++];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= h - '0';
                    else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                    else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                    else return fail("bad \\u escape");
                }
                // Encode as UTF-8 (surrogate pairs collapse to '?' - not needed for this data).
                if (cp < 0x80) out += static_cast<char>(cp);
                else if (cp < 0x800) {
                    out += static_cast<char>(0xC0 | (cp >> 6));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else if (cp >= 0xD800 && cp <= 0xDFFF) out += '?';
                else {
                    out += static_cast<char>(0xE0 | (cp >> 12));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }

    bool parseArray(Value& v) {
        pos_++;  // '['
        v.type = Value::Array;
        skipWs();
        if (peek() == ']') { pos_++; return true; }
        for (;;) {
            v.arr.emplace_back();
            if (!parseValue(v.arr.back())) return false;
            skipWs();
            char c = peek();
            if (c == ',') { pos_++; continue; }
            if (c == ']') { pos_++; return true; }
            return fail("expected , or ] in array");
        }
    }

    bool parseObject(Value& v) {
        pos_++;  // '{'
        v.type = Value::Object;
        skipWs();
        if (peek() == '}') { pos_++; return true; }
        for (;;) {
            skipWs();
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (peek() != ':') return fail("expected : in object");
            pos_++;
            v.obj.emplace_back(std::move(key), Value{});
            if (!parseValue(v.obj.back().second)) return false;
            skipWs();
            char c = peek();
            if (c == ',') { pos_++; continue; }
            if (c == '}') { pos_++; return true; }
            return fail("expected , or } in object");
        }
    }
};

inline bool parse(const std::string& text, Value* out, std::string* err) {
    return Parser::parse(text.c_str(), text.size(), out, err);
}

}  // namespace json
