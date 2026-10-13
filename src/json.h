// Minimal single-header JSON reader for the D2R datatable tools.
//
// Goal: load the game's localisation json (<lng>/strings/*.json) which, unlike
// strict JSON, carries C/C++ style comments and trailing commas (e.g.
// shrines.json uses a "// sk11..." annotation line).  The game ships its own
// permissive parser; this one mirrors that tolerance so the same files parse
// here without pre-processing.
//
// It is a small recursive-descent parser producing a Value tree.  We only need
// object/array/string/number/bool/null plus comment skipping; it is deliberately
// not a general-purpose JSON library.
#pragma once
#include <string>
#include <vector>
#include <utility>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace d2json {

struct Value {
  enum T { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
  bool b = false;
  double d = 0.0;
  long long i = 0;       // valid when isInt == true
  bool isInt = false;
  std::string s;
  std::vector<Value> a;
  std::vector<std::pair<std::string, Value>> o;  // object, order preserved

  const Value* find(const std::string& key) const {
    for (const auto& kv : o)
      if (kv.first == key) return &kv.second;
    return nullptr;
  }
};

class Parser {
 public:
  // Parse `text` into `out`.  Returns false on the first structural error and,
  // if `err` is non-null, fills it with a short message.  Comments (// and /* */)
  // and trailing commas are tolerated everywhere.
  bool parse(const std::string& text, Value& out, std::string* err = nullptr) {
    text_ = text.data();
    end_ = text_ + text.size();
    err_ = err;
    if (err) err->clear();
    p_ = text_;
    // Skip a leading UTF-8 BOM (the game's json files carry one).
    if ((size_t)(end_ - p_) >= 3 && (unsigned char)p_[0] == 0xEF &&
        (unsigned char)p_[1] == 0xBB && (unsigned char)p_[2] == 0xBF)
      p_ += 3;
    try {
      skip_ws();
      if (!parse_value(out)) return false;
      skip_ws();
      if (p_ != end_) { fail("trailing characters after value"); return false; }
      return true;
    } catch (...) {
      return false;
    }
  }

 private:
  const char* text_ = nullptr;
  const char* p_ = nullptr;
  const char* end_ = nullptr;
  std::string* err_ = nullptr;

  void fail(const std::string& m) {
    if (err_) *err_ = m;
    throw m;  // parse() catches this to return false
  }

  bool parse_value(Value& v) {
    if (!skip_ws()) return false;
    if (p_ == end_) { fail("unexpected end of input"); return false; }
    char c = *p_;
    switch (c) {
      case '{': return parse_object(v);
      case '[': return parse_array(v);
      case '"': {
        v.t = Value::STR;
        return parse_string(v.s);
      }
      case 't': case 'f': return parse_keyword(v, c == 't' ? "true" : "false", c == 't');
      case 'n': return parse_keyword(v, "null", false);
      default:
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number(v);
        fail("unexpected character");
        return false;
    }
  }

  // Returns false only on EOF; throws on malformed structure.
  bool skip_ws() {
    while (p_ < end_) {
      char c = *p_;
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++p_; }
      else if (c == '/' && p_ + 1 < end_ && p_[1] == '/') {
        p_ += 2;
        while (p_ < end_ && *p_ != '\n') ++p_;
      } else if (c == '/' && p_ + 1 < end_ && p_[1] == '*') {
        p_ += 2;
        while (p_ + 1 < end_ && !(p_[0] == '*' && p_[1] == '/')) ++p_;
        if (p_ + 1 >= end_) { fail("unterminated block comment"); return false; }
        p_ += 2;
      } else {
        break;
      }
    }
    return true;
  }

  bool parse_string(std::string& out) {
    // assumes current char is '"'
    ++p_;
    out.clear();
    while (p_ < end_) {
      char c = *p_++;
      if (c == '"') return true;
      if (c == '\\') {
        if (p_ == end_) { fail("unterminated escape"); return false; }
        char e = *p_++;
        switch (e) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            if (p_ + 4 > end_) { fail("bad \\u escape"); return false; }
            unsigned cp = 0;
            for (int k = 0; k < 4; ++k) {
              char h = *p_++;
              cp <<= 4;
              if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
              else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
              else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
              else { fail("bad \\u hex digit"); return false; }
            }
            if (cp >= 0xD800 && cp <= 0xDBFF && p_ + 6 <= end_ &&
                p_[0] == '\\' && p_[1] == 'u') {
              p_ += 2;
              unsigned lo = 0;
              for (int k = 0; k < 4; ++k) {
                char h = *p_++;
                lo <<= 4;
                if (h >= '0' && h <= '9') lo |= (unsigned)(h - '0');
                else if (h >= 'a' && h <= 'f') lo |= (unsigned)(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') lo |= (unsigned)(h - 'A' + 10);
                else { fail("bad \\u hex digit"); return false; }
              }
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            encode_utf8(cp, out);
            break;
          }
          default: out.push_back(e); break;  // permissive: keep the char
        }
      } else {
        out.push_back(c);  // raw byte (UTF-8 passes through untouched)
      }
    }
    fail("unterminated string");
    return false;
  }

  static void encode_utf8(unsigned cp, std::string& out) {
    if (cp <= 0x7F) {
      out.push_back((char)cp);
    } else if (cp <= 0x7FF) {
      out.push_back((char)(0xC0 | (cp >> 6)));
      out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
      out.push_back((char)(0xE0 | (cp >> 12)));
      out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
      out.push_back((char)(0xF0 | (cp >> 18)));
      out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back((char)(0x80 | (cp & 0x3F)));
    }
  }

  bool parse_number(Value& v) {
    const char* start = p_;
    if (p_ < end_ && *p_ == '-') ++p_;
    bool is_int = true;
    while (p_ < end_ && *p_ >= '0' && *p_ <= '9') ++p_;
    if (p_ < end_ && *p_ == '.') { is_int = false; ++p_; while (p_ < end_ && *p_ >= '0' && *p_ <= '9') ++p_; }
    if (p_ < end_ && (*p_ == 'e' || *p_ == 'E')) {
      is_int = false; ++p_;
      if (p_ < end_ && (*p_ == '+' || *p_ == '-')) ++p_;
      while (p_ < end_ && *p_ >= '0' && *p_ <= '9') ++p_;
    }
    std::string num(start, p_);
    if (num.empty() || num == "-") { fail("invalid number"); return false; }
    if (is_int) {
      v.t = Value::NUM; v.isInt = true;
      v.i = (long long)strtoll(num.c_str(), nullptr, 10);
      v.d = (double)v.i;
    } else {
      v.t = Value::NUM; v.isInt = false;
      v.d = strtod(num.c_str(), nullptr);
      v.i = (long long)v.d;
    }
    return true;
  }

  bool parse_keyword(Value& v, const char* lit, bool val) {
    size_t n = std::strlen(lit);
    if ((size_t)(end_ - p_) < n || std::strncmp(p_, lit, n) != 0) {
      fail("invalid literal");
      return false;
    }
    p_ += n;
    if (lit[0] == 'n') v.t = Value::NUL;
    else { v.t = Value::BOOL; v.b = val; }
    return true;
  }

  bool parse_object(Value& v) {
    v.t = Value::OBJ;
    ++p_;  // consume '{'
    skip_ws();
    if (p_ < end_ && *p_ == '}') { ++p_; return true; }
    for (;;) {
      skip_ws();
      if (p_ == end_ || *p_ != '"') { fail("expected string key"); return false; }
      std::string key;
      if (!parse_string(key)) return false;
      skip_ws();
      if (p_ == end_ || *p_ != ':') { fail("expected ':'"); return false; }
      ++p_;
      Value val;
      if (!parse_value(val)) return false;
      v.o.emplace_back(std::move(key), std::move(val));
      skip_ws();
      if (p_ == end_) { fail("unterminated object"); return false; }
      if (*p_ == ',') {
        ++p_; skip_ws();
        if (p_ < end_ && *p_ == '}') { ++p_; return true; }
        continue;
      }
      if (*p_ == '}') { ++p_; return true; }
      fail("expected ',' or '}'");
      return false;
    }
  }

  bool parse_array(Value& v) {
    v.t = Value::ARR;
    ++p_;  // consume '['
    skip_ws();
    if (p_ < end_ && *p_ == ']') { ++p_; return true; }
    for (;;) {
      Value val;
      if (!parse_value(val)) return false;
      v.a.push_back(std::move(val));
      skip_ws();
      if (p_ == end_) { fail("unterminated array"); return false; }
      if (*p_ == ',') {
        ++p_; skip_ws();
        if (p_ < end_ && *p_ == ']') { ++p_; return true; }
        continue;
      }
      if (*p_ == ']') { ++p_; return true; }
      fail("expected ',' or ']'");
      return false;
    }
  }
};

}  // namespace d2json
