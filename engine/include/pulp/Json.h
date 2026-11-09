#pragma once
// Small JSON document type used for project files, plugin state and presets.
#include <string>
#include <utility>
#include <vector>

namespace pulp {

class Json {
public:
    enum class Type { Null, Bool, Num, Str, Arr, Obj };
    using Array = std::vector<Json>;
    using Object = std::vector<std::pair<std::string, Json>>;   // keeps insertion order

    Json() = default;
    Json(bool b) : type_(Type::Bool), num_(b ? 1.0 : 0.0) {}
    Json(int v) : type_(Type::Num), num_(v) {}
    Json(unsigned v) : type_(Type::Num), num_(v) {}
    Json(long long v) : type_(Type::Num), num_((double)v) {}
    Json(float v) : type_(Type::Num), num_(v), isFloat_(true) {}
    Json(double v) : type_(Type::Num), num_(v) {}
    Json(const char* s) : type_(Type::Str), str_(s) {}
    Json(const std::string& s) : type_(Type::Str), str_(s) {}

    static Json array() { Json j; j.type_ = Type::Arr; return j; }
    static Json object() { Json j; j.type_ = Type::Obj; return j; }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isObject() const { return type_ == Type::Obj; }
    bool isArray() const { return type_ == Type::Arr; }
    bool isNumber() const { return type_ == Type::Num; }
    bool isString() const { return type_ == Type::Str; }

    double num(double def = 0.0) const { return type_ == Type::Num || type_ == Type::Bool ? num_ : def; }
    float f(float def = 0.0f) const { return (float)num(def); }
    int i(int def = 0) const { return type_ == Type::Num || type_ == Type::Bool ? (int)num_ : def; }
    long long i64(long long def = 0) const { return type_ == Type::Num ? (long long)num_ : def; }
    bool b(bool def = false) const { return type_ == Type::Bool || type_ == Type::Num ? num_ != 0.0 : def; }
    const std::string& str() const { return str_; }
    std::string str(const std::string& def) const { return type_ == Type::Str ? str_ : def; }

    // Object access. Reading a missing key gives a null Json.
    const Json& operator[](const char* key) const;
    const Json& operator[](const std::string& key) const { return (*this)[key.c_str()]; }
    bool has(const char* key) const;
    Json& set(const std::string& key, Json v);
    const Object& items() const { return obj_; }

    // Array access.
    size_t size() const { return type_ == Type::Arr ? arr_.size() : (type_ == Type::Obj ? obj_.size() : 0); }
    const Json& at(size_t i) const;
    Json& push(Json v);
    const Array& elements() const { return arr_; }

    std::string dump(int indent = 0) const;
    static Json parse(const std::string& text, std::string* error = nullptr);

    bool operator==(const Json& o) const;

private:
    void dumpTo(std::string& out, int indent, int depth) const;
    Type type_ = Type::Null;
    double num_ = 0.0;
    bool isFloat_ = false;
    std::string str_;
    Array arr_;
    Object obj_;
};

} // namespace pulp
