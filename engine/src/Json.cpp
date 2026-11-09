#include "pulp/Json.h"
#include <charconv>
#include <cmath>
#include <cstdlib>

namespace pulp {

static const Json kNullJson;

const Json& Json::operator[](const char* key) const {
    if (type_ == Type::Obj)
        for (auto& kv : obj_) if (kv.first == key) return kv.second;
    return kNullJson;
}
bool Json::has(const char* key) const {
    if (type_ == Type::Obj)
        for (auto& kv : obj_) if (kv.first == key) return true;
    return false;
}
Json& Json::set(const std::string& key, Json v) {
    if (type_ != Type::Obj) { *this = object(); }
    for (auto& kv : obj_) if (kv.first == key) { kv.second = std::move(v); return *this; }
    obj_.emplace_back(key, std::move(v));
    return *this;
}
const Json& Json::at(size_t i) const {
    if (type_ == Type::Arr && i < arr_.size()) return arr_[i];
    return kNullJson;
}
Json& Json::push(Json v) {
    if (type_ != Type::Arr) { *this = array(); }
    arr_.push_back(std::move(v));
    return *this;
}

bool Json::operator==(const Json& o) const {
    if (type_ != o.type_) return false;
    switch (type_) {
        case Type::Null: return true;
        case Type::Bool: case Type::Num: return num_ == o.num_;
        case Type::Str: return str_ == o.str_;
        case Type::Arr: return arr_ == o.arr_;
        case Type::Obj: return obj_ == o.obj_;
    }
    return false;
}

static void dumpString(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
                else out += (char)c;
        }
    }
    out += '"';
}

void Json::dumpTo(std::string& out, int indent, int depth) const {
    auto nl = [&](int d) { if (indent > 0) { out += '\n'; out.append((size_t)(indent * d), ' '); } };
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += num_ != 0.0 ? "true" : "false"; break;
        case Type::Num: {
            if (!std::isfinite(num_)) { out += "0"; break; }
            char b[40];
            if (num_ == std::floor(num_) && std::fabs(num_) < 9.0e15) {
                auto r = std::to_chars(b, b + sizeof b, (long long)num_);
                out.append(b, r.ptr);
            } else if (isFloat_) {
                auto r = std::to_chars(b, b + sizeof b, (float)num_);
                out.append(b, r.ptr);
            } else {
                auto r = std::to_chars(b, b + sizeof b, num_);
                out.append(b, r.ptr);
            }
            break;
        }
        case Type::Str: dumpString(out, str_); break;
        case Type::Arr: {
            if (arr_.empty()) { out += "[]"; break; }
            bool flat = true;
            for (auto& e : arr_) if (e.type_ == Type::Arr || e.type_ == Type::Obj) { flat = false; break; }
            out += '[';
            for (size_t i = 0; i < arr_.size(); ++i) {
                if (i) out += flat && indent > 0 ? ", " : ",";
                if (!flat) nl(depth + 1);
                arr_[i].dumpTo(out, indent, depth + 1);
            }
            if (!flat) nl(depth);
            out += ']';
            break;
        }
        case Type::Obj: {
            if (obj_.empty()) { out += "{}"; break; }
            out += '{';
            for (size_t i = 0; i < obj_.size(); ++i) {
                if (i) out += ',';
                nl(depth + 1);
                dumpString(out, obj_[i].first);
                out += indent > 0 ? ": " : ":";
                obj_[i].second.dumpTo(out, indent, depth + 1);
            }
            nl(depth);
            out += '}';
            break;
        }
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

namespace {
struct Parser {
    const char* p;
    const char* end;
    std::string err;

    void ws() { while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p; }
    bool fail(const char* m) { if (err.empty()) err = m; return false; }

    bool parseString(std::string& out) {
        if (p >= end || *p != '"') return fail("expected string");
        ++p;
        while (p < end && *p != '"') {
            if (*p == '\\') {
                if (++p >= end) return fail("bad escape");
                switch (*p) {
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        if (end - p < 5) return fail("bad unicode escape");
                        unsigned cp = (unsigned)std::strtoul(std::string(p + 1, p + 5).c_str(), nullptr, 16);
                        p += 4;
                        if (cp < 0x80) out += (char)cp;
                        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
                        else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
                        break;
                    }
                    default: out += *p; break;
                }
                ++p;
            } else out += *p++;
        }
        if (p >= end) return fail("unterminated string");
        ++p;
        return true;
    }

    bool parseValue(Json& out, int depth) {
        if (depth > 200) return fail("too deep");
        ws();
        if (p >= end) return fail("unexpected end");
        const char c = *p;
        if (c == '{') {
            ++p; out = Json::object(); ws();
            if (p < end && *p == '}') { ++p; return true; }
            for (;;) {
                ws();
                std::string key;
                if (!parseString(key)) return false;
                ws();
                if (p >= end || *p != ':') return fail("expected ':'");
                ++p;
                Json v;
                if (!parseValue(v, depth + 1)) return false;
                out.set(key, std::move(v));
                ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++p; out = Json::array(); ws();
            if (p < end && *p == ']') { ++p; return true; }
            for (;;) {
                Json v;
                if (!parseValue(v, depth + 1)) return false;
                out.push(std::move(v));
                ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == ']') { ++p; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') { std::string s; if (!parseString(s)) return false; out = Json(s); return true; }
        if (end - p >= 4 && std::string(p, p + 4) == "true") { p += 4; out = Json(true); return true; }
        if (end - p >= 5 && std::string(p, p + 5) == "false") { p += 5; out = Json(false); return true; }
        if (end - p >= 4 && std::string(p, p + 4) == "null") { p += 4; out = Json(); return true; }
        double d = 0.0;
        auto r = std::from_chars(p, end, d);
        if (r.ec != std::errc() || r.ptr == p) return fail("bad value");
        p = r.ptr;
        out = Json(d);
        return true;
    }
};
}

Json Json::parse(const std::string& text, std::string* error) {
    Parser ps{text.data(), text.data() + text.size(), {}};
    Json out;
    if (!ps.parseValue(out, 0)) { if (error) *error = ps.err; return Json(); }
    ps.ws();
    if (ps.p != ps.end) { if (error) *error = "trailing characters"; return Json(); }
    if (error) error->clear();
    return out;
}

} // namespace pulp
