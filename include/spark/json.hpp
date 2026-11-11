#pragma once

#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace spark {

// Minimal JSON DOM. Numbers are kept as their exact token text so prices are
// not rounded through double. This is not a general-purpose library; it is
// the parser the venue adapters and tests actually run.
class Json {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Type type = Type::Null;
  bool b = false;
  std::string text;
  std::vector<Json> arr;
  std::vector<std::pair<std::string, Json>> obj;

  const Json* get(std::string_view key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& kv : obj) {
      if (kv.first == key) return &kv.second;
    }
    return nullptr;
  }
  std::string_view as_string() const { return text; }
};

class JsonParser {
 public:
  explicit JsonParser(std::string_view in) : s_(in) {}

  bool parse(Json& out, std::string& err) {
    skip();
    if (!value(out, err)) return false;
    skip();
    if (i_ != s_.size()) {
      err = "trailing junk";
      return false;
    }
    return true;
  }

 private:
  std::string_view s_;
  size_t i_ = 0;

  void skip() {
    while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
  }
  bool value(Json& out, std::string& err) {
    skip();
    if (i_ >= s_.size()) {
      err = "unexpected end";
      return false;
    }
    char c = s_[i_];
    if (c == 'n') return lit("null", out, Json::Type::Null, err);
    if (c == 't') return lit("true", out, Json::Type::Bool, err);
    if (c == 'f') return lit("false", out, Json::Type::Bool, err);
    if (c == '"') return str(out, err);
    if (c == '[') return array(out, err);
    if (c == '{') return object(out, err);
    if (c == '-' || (c >= '0' && c <= '9')) return number(out, err);
    err = "invalid value";
    return false;
  }
  bool lit(const char* w, Json& out, Json::Type t, std::string& err) {
    size_t n = std::char_traits<char>::length(w);
    if (s_.substr(i_, n) != w) {
      err = "bad literal";
      return false;
    }
    i_ += n;
    out = Json{};
    out.type = t;
    if (t == Json::Type::Bool) out.b = (w[0] == 't');
    return true;
  }
  bool str_into(std::string& dest, std::string& err) {
    if (i_ >= s_.size() || s_[i_] != '"') {
      err = "expected string";
      return false;
    }
    ++i_;
    dest.clear();
    while (i_ < s_.size()) {
      char c = s_[i_++];
      if (c == '"') return true;
      if (c == '\\') {
        if (i_ >= s_.size()) {
          err = "bad escape";
          return false;
        }
        char e = s_[i_++];
        switch (e) {
          case '"':
          case '\\':
          case '/':
            dest.push_back(e);
            break;
          case 'b': dest.push_back('\b'); break;
          case 'f': dest.push_back('\f'); break;
          case 'n': dest.push_back('\n'); break;
          case 'r': dest.push_back('\r'); break;
          case 't': dest.push_back('\t'); break;
          case 'u': {
            if (i_ + 4 > s_.size()) {
              err = "bad unicode";
              return false;
            }
            int cp = 0;
            for (int k = 0; k < 4; ++k) {
              char h = s_[i_++];
              cp <<= 4;
              if (h >= '0' && h <= '9') cp += h - '0';
              else if (h >= 'a' && h <= 'f') cp += h - 'a' + 10;
              else if (h >= 'A' && h <= 'F') cp += h - 'A' + 10;
              else {
                err = "bad unicode";
                return false;
              }
            }
            if (cp < 0x80) dest.push_back(static_cast<char>(cp));
            else if (cp < 0x800) {
              dest.push_back(static_cast<char>(0xC0 | (cp >> 6)));
              dest.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
              dest.push_back(static_cast<char>(0xE0 | (cp >> 12)));
              dest.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
              dest.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            break;
          }
          default:
            err = "bad escape";
            return false;
        }
      } else {
        dest.push_back(c);
      }
    }
    err = "unterminated string";
    return false;
  }
  bool str(Json& out, std::string& err) {
    out = Json{};
    out.type = Json::Type::String;
    return str_into(out.text, err);
  }
  bool number(Json& out, std::string& err) {
    size_t start = i_;
    if (s_[i_] == '-') ++i_;
    if (i_ >= s_.size() || s_[i_] < '0' || s_[i_] > '9') {
      err = "bad number";
      return false;
    }
    if (s_[i_] == '0') ++i_;
    else while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
    if (i_ < s_.size() && s_[i_] == '.') {
      ++i_;
      if (i_ >= s_.size() || s_[i_] < '0' || s_[i_] > '9') {
        err = "bad number";
        return false;
      }
      while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
    }
    if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
      err = "exponent not accepted for exact decimals";
      return false;
    }
    out = Json{};
    out.type = Json::Type::Number;
    out.text.assign(s_.data() + start, i_ - start);
    return true;
  }
  bool array(Json& out, std::string& err) {
    ++i_;
    out = Json{};
    out.type = Json::Type::Array;
    skip();
    if (i_ < s_.size() && s_[i_] == ']') {
      ++i_;
      return true;
    }
    while (true) {
      Json el;
      if (!value(el, err)) return false;
      out.arr.push_back(std::move(el));
      skip();
      if (i_ >= s_.size()) {
        err = "unterminated array";
        return false;
      }
      if (s_[i_] == ',') {
        ++i_;
        continue;
      }
      if (s_[i_] == ']') {
        ++i_;
        return true;
      }
      err = "expected comma or ]";
      return false;
    }
  }
  bool object(Json& out, std::string& err) {
    ++i_;
    out = Json{};
    out.type = Json::Type::Object;
    skip();
    if (i_ < s_.size() && s_[i_] == '}') {
      ++i_;
      return true;
    }
    while (true) {
      skip();
      std::string key;
      if (!str_into(key, err)) return false;
      skip();
      if (i_ >= s_.size() || s_[i_] != ':') {
        err = "expected colon";
        return false;
      }
      ++i_;
      Json val;
      if (!value(val, err)) return false;
      out.obj.emplace_back(std::move(key), std::move(val));
      skip();
      if (i_ >= s_.size()) {
        err = "unterminated object";
        return false;
      }
      if (s_[i_] == ',') {
        ++i_;
        continue;
      }
      if (s_[i_] == '}') {
        ++i_;
        return true;
      }
      err = "expected comma or }";
      return false;
    }
  }
};

inline bool parse_json(std::string_view in, Json& out, std::string& err) {
  return JsonParser(in).parse(out, err);
}

}  // namespace spark
