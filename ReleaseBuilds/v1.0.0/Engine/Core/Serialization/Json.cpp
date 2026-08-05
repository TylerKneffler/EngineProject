#include "Core/Serialization/Json.h"
#include <vector>
#include <utility>
#include <stdexcept>
#include <fstream>
#include <sstream>
#include <cassert>
#include <iomanip>

// ---- Concrete storage types (only visible in this TU) ----------------------
using JArray  = std::vector<JsonValue>;
using JKV     = std::pair<std::string, JsonValue>;
using JObject = std::vector<JKV>;

// Type-erased casts
static       JArray*  asArr(const std::shared_ptr<void>& d) { return static_cast<JArray*> (d.get()); }
static       JObject* asObj(const std::shared_ptr<void>& d) { return static_cast<JObject*>(d.get()); }

// ---- Constructors ----------------------------------------------------------
JsonValue::JsonValue()                      = default;
JsonValue::JsonValue(bool b)                : m_type(Type::Bool),   m_bool(b)   {}
JsonValue::JsonValue(double d)              : m_type(Type::Number), m_number(d) {}
JsonValue::JsonValue(float  f)              : m_type(Type::Number), m_number(static_cast<double>(f)) {}
JsonValue::JsonValue(int    i)              : m_type(Type::Number), m_number(static_cast<double>(i)) {}
JsonValue::JsonValue(const std::string& s)  : m_type(Type::String), m_string(s) {}
JsonValue::JsonValue(std::string&&      s)  : m_type(Type::String), m_string(std::move(s)) {}
JsonValue::JsonValue(const char* s)         : m_type(Type::String), m_string(s ? s : "") {}

// Deep copy
JsonValue::JsonValue(const JsonValue& o)
    : m_type(o.m_type), m_bool(o.m_bool), m_number(o.m_number), m_string(o.m_string)
{
    if (m_type == Type::Array)
        m_data = std::make_shared<JArray>(*asArr(o.m_data));
    else if (m_type == Type::Object)
        m_data = std::make_shared<JObject>(*asObj(o.m_data));
}

JsonValue::JsonValue(JsonValue&& o) noexcept
    : m_type(o.m_type), m_bool(o.m_bool), m_number(o.m_number),
      m_string(std::move(o.m_string)), m_data(std::move(o.m_data))
{
    o.m_type = Type::Null;
}

void JsonValue::swap(JsonValue& o) noexcept
{
    std::swap(m_type,   o.m_type);
    std::swap(m_bool,   o.m_bool);
    std::swap(m_number, o.m_number);
    m_string.swap(o.m_string);
    m_data.swap(o.m_data);
}

JsonValue& JsonValue::operator=(JsonValue other) { swap(other); return *this; }
JsonValue::~JsonValue() = default;

// ---- Scalar accessors -------------------------------------------------------
bool               JsonValue::AsBool()   const { return m_bool; }
double             JsonValue::AsDouble() const { return m_number; }
float              JsonValue::AsFloat()  const { return static_cast<float>(m_number); }
int                JsonValue::AsInt()    const { return static_cast<int>(m_number); }
const std::string& JsonValue::AsString() const { return m_string; }

// ---- Array accessors --------------------------------------------------------
std::size_t JsonValue::ArraySize() const
{
    return (m_type == Type::Array && m_data) ? asArr(m_data)->size() : 0;
}

const JsonValue& JsonValue::ArrayAt(std::size_t i) const
{
    return (*asArr(m_data))[i];
}

// ---- Object accessors -------------------------------------------------------
std::size_t JsonValue::ObjectSize() const
{
    return (m_type == Type::Object && m_data) ? asObj(m_data)->size() : 0;
}

const std::string& JsonValue::ObjectKey(std::size_t i) const
{
    return (*asObj(m_data))[i].first;
}

const JsonValue& JsonValue::ObjectValue(std::size_t i) const
{
    return (*asObj(m_data))[i].second;
}

bool JsonValue::Has(const std::string& key) const
{
    if (m_type != Type::Object || !m_data) return false;
    for (const auto& [k, v] : *asObj(m_data))
        if (k == key) return true;
    return false;
}

const JsonValue& JsonValue::operator[](const std::string& key) const
{
    static const JsonValue kNull;
    if (m_type != Type::Object || !m_data) return kNull;
    for (const auto& [k, v] : *asObj(m_data))
        if (k == key) return v;
    return kNull;
}

// ---- Mutation ---------------------------------------------------------------
JsonValue& JsonValue::Set(const std::string& key, JsonValue val)
{
    if (m_type == Type::Null)
    {
        m_type = Type::Object;
        m_data = std::make_shared<JObject>();
    }
    assert(m_type == Type::Object);
    auto& obj = *asObj(m_data);
    for (auto& [k, v] : obj)
        if (k == key) { v = std::move(val); return *this; }
    obj.emplace_back(key, std::move(val));
    return *this;
}

JsonValue& JsonValue::Push(JsonValue val)
{
    if (m_type == Type::Null)
    {
        m_type = Type::Array;
        m_data = std::make_shared<JArray>();
    }
    assert(m_type == Type::Array);
    asArr(m_data)->push_back(std::move(val));
    return *this;
}

// ---- Factories ---------------------------------------------------------------
JsonValue JsonValue::MakeObject()
{
    JsonValue v;
    v.m_type = Type::Object;
    v.m_data = std::make_shared<JObject>();
    return v;
}

JsonValue JsonValue::MakeArray()
{
    JsonValue v;
    v.m_type = Type::Array;
    v.m_data = std::make_shared<JArray>();
    return v;
}

// ============================================================================
// Writer
// ============================================================================
namespace
{

void WriteEscaped(std::string& out, const std::string& s)
{
    out += '"';
    for (char c : s)
    {
        switch (c)
        {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:   out += c;      break;
        }
    }
    out += '"';
}

void Indent(std::string& out, int level)
{
    for (int i = 0; i < level; ++i) out += "    ";
}

void WriteImpl(const JsonValue& v, std::string& out, int indent)
{
    switch (v.GetType())
    {
    case JsonValue::Type::Null:
        out += "null";
        break;

    case JsonValue::Type::Bool:
        out += v.AsBool() ? "true" : "false";
        break;

    case JsonValue::Type::Number:
    {
        double d = v.AsDouble();
        // Emit integer form when the value is a whole number in safe range.
        if (d == static_cast<double>(static_cast<long long>(d)) &&
            d >= -1e15 && d <= 1e15)
        {
            out += std::to_string(static_cast<long long>(d));
        }
        else
        {
            std::ostringstream ss;
            ss << std::setprecision(7) << d;
            out += ss.str();
        }
        break;
    }

    case JsonValue::Type::String:
        WriteEscaped(out, v.AsString());
        break;

    case JsonValue::Type::Array:
    {
        std::size_t n = v.ArraySize();
        if (n == 0) { out += "[]"; break; }
        out += "[\n";
        for (std::size_t i = 0; i < n; ++i)
        {
            Indent(out, indent + 1);
            WriteImpl(v.ArrayAt(i), out, indent + 1);
            if (i + 1 < n) out += ',';
            out += '\n';
        }
        Indent(out, indent);
        out += ']';
        break;
    }

    case JsonValue::Type::Object:
    {
        std::size_t n = v.ObjectSize();
        if (n == 0) { out += "{}"; break; }
        out += "{\n";
        for (std::size_t i = 0; i < n; ++i)
        {
            Indent(out, indent + 1);
            WriteEscaped(out, v.ObjectKey(i));
            out += ": ";
            WriteImpl(v.ObjectValue(i), out, indent + 1);
            if (i + 1 < n) out += ',';
            out += '\n';
        }
        Indent(out, indent);
        out += '}';
        break;
    }
    }
}

} // anonymous namespace

std::string JsonWrite(const JsonValue& v, int startIndent)
{
    std::string out;
    out.reserve(4096);
    WriteImpl(v, out, startIndent);
    out += '\n';
    return out;
}

// ============================================================================
// Parser
// ============================================================================
namespace
{

struct Parser
{
    const char* cur;
    const char* end;

    void skipWs()
    {
        while (cur < end && (*cur == ' ' || *cur == '\t' || *cur == '\n' || *cur == '\r'))
            ++cur;
    }

    char peek() const { return cur < end ? *cur : '\0'; }

    void expect(char c)
    {
        if (cur >= end || *cur != c)
            throw std::runtime_error(std::string("JSON parse error: expected '") + c + "' near '" + (cur < end ? std::string(cur, std::min(cur + 20, end)) : "<eof>") + "'");
        ++cur;
    }

    std::string parseStringBody()
    {
        std::string s;
        while (cur < end)
        {
            char c = *cur++;
            if (c == '"') return s;
            if (c == '\\')
            {
                if (cur >= end) break;
                char e = *cur++;
                switch (e)
                {
                case '"':  s += '"';  break;
                case '\\': s += '\\'; break;
                case '/':  s += '/';  break;
                case 'n':  s += '\n'; break;
                case 'r':  s += '\r'; break;
                case 't':  s += '\t'; break;
                default:   s += e;    break;
                }
            }
            else
            {
                s += c;
            }
        }
        throw std::runtime_error("JSON parse error: unterminated string");
    }

    JsonValue parseValue()
    {
        skipWs();
        if (cur >= end) throw std::runtime_error("JSON parse error: unexpected end of input");
        char c = peek();
        if (c == '"')  { ++cur; return JsonValue(parseStringBody()); }
        if (c == '{')  return parseObject();
        if (c == '[')  return parseArray();
        if (c == 't')  { cur += 4; return JsonValue(true);  }
        if (c == 'f')  { cur += 5; return JsonValue(false); }
        if (c == 'n')  { cur += 4; return JsonValue();      }

        // Number
        const char* start = cur;
        if (c == '-') ++cur;
        while (cur < end && ((*cur >= '0' && *cur <= '9') || *cur == '.'))
            ++cur;
        if (cur < end && (*cur == 'e' || *cur == 'E'))
        {
            ++cur;
            if (cur < end && (*cur == '+' || *cur == '-')) ++cur;
            while (cur < end && *cur >= '0' && *cur <= '9') ++cur;
        }
        if (cur == start)
            throw std::runtime_error(std::string("JSON parse error: unexpected char '") + c + "'");
        return JsonValue(std::stod(std::string(start, cur)));
    }

    JsonValue parseObject()
    {
        expect('{');
        JsonValue obj = JsonValue::MakeObject();
        skipWs();
        if (peek() == '}') { ++cur; return obj; }

        while (true)
        {
            skipWs();
            expect('"');
            std::string key = parseStringBody();
            skipWs();
            expect(':');
            obj.Set(key, parseValue());
            skipWs();
            if (peek() == ',') { ++cur; continue; }
            if (peek() == '}') { ++cur; break; }
            throw std::runtime_error("JSON parse error: expected ',' or '}' in object");
        }
        return obj;
    }

    JsonValue parseArray()
    {
        expect('[');
        JsonValue arr = JsonValue::MakeArray();
        skipWs();
        if (peek() == ']') { ++cur; return arr; }

        while (true)
        {
            arr.Push(parseValue());
            skipWs();
            if (peek() == ',') { ++cur; continue; }
            if (peek() == ']') { ++cur; break; }
            throw std::runtime_error("JSON parse error: expected ',' or ']' in array");
        }
        return arr;
    }
};

} // anonymous namespace

JsonValue JsonParse(const std::string& src)
{
    Parser p;
    p.cur = src.data();
    p.end = src.data() + src.size();
    return p.parseValue();
}

JsonValue JsonParseFile(const std::string& path)
{
    std::ifstream f(path);
    if (!f) throw std::runtime_error("JsonParseFile: cannot open '" + path + "'");
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return JsonParse(src);
}
