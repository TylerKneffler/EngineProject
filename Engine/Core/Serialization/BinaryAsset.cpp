#include "Core/Serialization/Json.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace Engine::Serialization
{
namespace
{
// EBIN + storage version. The asset's own "version" field remains independent.
constexpr char Magic[] = {'E', 'B', 'I', 'N', 1};
constexpr unsigned MaxDepth = 128;
enum Tag : uint8_t
{
    Null, False, True, Integer, Float32, Float64, String, Array, Object
};

void WriteVar(std::string& output, uint64_t value)
{
    while (value >= 128)
    {
        output.push_back(static_cast<char>((value & 127) | 128));
        value >>= 7;
    }
    output.push_back(static_cast<char>(value));
}

void WriteString(std::string& output, const std::string& value)
{
    WriteVar(output, value.size());
    output.append(value);
}

template<typename T>
void WriteBits(std::string& output, T value)
{
    static_assert(sizeof(T) == 4 || sizeof(T) == 8);
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(T));
    for (size_t i = 0; i < sizeof(T); ++i)
        output.push_back(static_cast<char>(bits >> (i * 8)));
}

void CollectKeys(const JsonValue& value,
    std::vector<std::string>& keys,
    std::unordered_map<std::string, size_t>& indices,
    unsigned depth)
{
    if (depth > MaxDepth) throw std::runtime_error("Asset tree is too deep");
    if (value.IsObject())
        for (size_t i = 0; i < value.ObjectSize(); ++i)
        {
            const auto& key = value.ObjectKey(i);
            if (indices.emplace(key, keys.size()).second)
                keys.push_back(key);
            CollectKeys(value.ObjectValue(i), keys, indices, depth + 1);
        }
    else if (value.IsArray())
        for (size_t i = 0; i < value.ArraySize(); ++i)
            CollectKeys(value.ArrayAt(i), keys, indices, depth + 1);
}

void WriteValue(std::string& output, const JsonValue& value,
    const std::unordered_map<std::string, size_t>& keys, unsigned depth)
{
    if (depth > MaxDepth) throw std::runtime_error("Asset tree is too deep");
    switch (value.GetType())
    {
    case JsonValue::Type::Null: output.push_back(Null); break;
    case JsonValue::Type::Bool:
        output.push_back(value.AsBool() ? True : False);
        break;
    case JsonValue::Type::Number:
    {
        const double number = value.AsDouble();
        if (std::isfinite(number) && !(number == 0.0 && std::signbit(number)) &&
            number >= -2147483648.0 &&
            number <= 2147483647.0 && std::trunc(number) == number)
        {
            output.push_back(Integer);
            const auto integer = static_cast<int32_t>(number);
            WriteVar(output, (static_cast<uint64_t>(
                static_cast<int64_t>(integer)) << 1) ^
                static_cast<uint64_t>(integer >> 31));
        }
        else if (std::isfinite(number) &&
            static_cast<double>(static_cast<float>(number)) == number)
        {
            output.push_back(Float32);
            WriteBits(output, static_cast<float>(number));
        }
        else
        {
            output.push_back(Float64);
            WriteBits(output, number);
        }
        break;
    }
    case JsonValue::Type::String:
        output.push_back(String);
        WriteString(output, value.AsString());
        break;
    case JsonValue::Type::Array:
        output.push_back(Array);
        WriteVar(output, value.ArraySize());
        for (size_t i = 0; i < value.ArraySize(); ++i)
            WriteValue(output, value.ArrayAt(i), keys, depth + 1);
        break;
    case JsonValue::Type::Object:
        output.push_back(Object);
        WriteVar(output, value.ObjectSize());
        for (size_t i = 0; i < value.ObjectSize(); ++i)
        {
            WriteVar(output, keys.at(value.ObjectKey(i)));
            WriteValue(output, value.ObjectValue(i), keys, depth + 1);
        }
        break;
    }
}

class Reader
{
public:
    explicit Reader(const std::string& bytes)
        : current(reinterpret_cast<const uint8_t*>(bytes.data())),
          end(current + bytes.size()) {}

    size_t Remaining() const { return static_cast<size_t>(end - current); }
    bool Empty() const { return current == end; }

    uint8_t Byte()
    {
        if (Empty()) throw std::runtime_error("Truncated binary asset");
        return *current++;
    }

    uint64_t Var()
    {
        uint64_t result = 0;
        for (unsigned i = 0; i < 10; ++i)
        {
            const uint8_t byte = Byte();
            if (i == 9 && byte > 1)
                throw std::runtime_error("Invalid binary asset length");
            result |= static_cast<uint64_t>(byte & 127) << (i * 7);
            if (!(byte & 128)) return result;
        }
        throw std::runtime_error("Invalid binary asset length");
    }

    std::string StringValue()
    {
        const uint64_t length = Var();
        if (length > Remaining())
            throw std::runtime_error("Truncated binary asset string");
        std::string result(reinterpret_cast<const char*>(current),
            static_cast<size_t>(length));
        current += length;
        return result;
    }

    template<typename T>
    T Bits()
    {
        if (Remaining() < sizeof(T))
            throw std::runtime_error("Truncated binary asset number");
        uint64_t bits = 0;
        for (size_t i = 0; i < sizeof(T); ++i)
            bits |= static_cast<uint64_t>(Byte()) << (i * 8);
        T result;
        std::memcpy(&result, &bits, sizeof(T));
        return result;
    }

    JsonValue Value(const std::vector<std::string>& keys, unsigned depth)
    {
        if (depth > MaxDepth) throw std::runtime_error("Asset tree is too deep");
        switch (Byte())
        {
        case Null: return JsonValue();
        case False: return JsonValue(false);
        case True: return JsonValue(true);
        case Integer:
        {
            const uint64_t encoded = Var();
            const int64_t integer = static_cast<int64_t>(encoded >> 1) ^
                -static_cast<int64_t>(encoded & 1);
            if (integer < std::numeric_limits<int32_t>::min() ||
                integer > std::numeric_limits<int32_t>::max())
                throw std::runtime_error("Invalid binary asset integer");
            return JsonValue(static_cast<int>(integer));
        }
        case Float32: return JsonValue(Bits<float>());
        case Float64: return JsonValue(Bits<double>());
        case String: return JsonValue(StringValue());
        case Array:
        {
            const uint64_t count = Var();
            if (count > Remaining())
                throw std::runtime_error("Invalid binary asset array size");
            JsonValue array = JsonValue::MakeArray();
            for (uint64_t i = 0; i < count; ++i)
                array.Push(Value(keys, depth + 1));
            return array;
        }
        case Object:
        {
            const uint64_t count = Var();
            if (count > Remaining() / 2)
                throw std::runtime_error("Invalid binary asset object size");
            JsonValue object = JsonValue::MakeObject();
            for (uint64_t i = 0; i < count; ++i)
            {
                const uint64_t index = Var();
                if (index >= keys.size())
                    throw std::runtime_error("Invalid binary asset field");
                object.Set(keys[static_cast<size_t>(index)],
                    Value(keys, depth + 1));
            }
            return object;
        }
        default: throw std::runtime_error("Invalid binary asset tag");
        }
    }

private:
    const uint8_t* current;
    const uint8_t* end;
};
}

std::string AssetWriteBinary(const JsonValue& value)
{
    std::vector<std::string> keys;
    std::unordered_map<std::string, size_t> indices;
    CollectKeys(value, keys, indices, 0);
    std::string output(std::begin(Magic), std::end(Magic));
    WriteVar(output, keys.size());
    for (const auto& key : keys) WriteString(output, key);
    WriteValue(output, value, indices, 0);
    return output;
}

JsonValue AssetParseFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open asset: " + path);
    const std::string bytes((std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());
    if (bytes.size() < 4 || std::memcmp(bytes.data(), Magic, 4) != 0)
        return JsonParse(bytes);
    if (bytes.size() < sizeof(Magic) || bytes[4] != Magic[4])
        throw std::runtime_error("Unsupported binary asset version");
    const std::string payload(bytes.data() + sizeof(Magic),
        bytes.size() - sizeof(Magic));
    Reader reader(payload);
    const uint64_t count = reader.Var();
    if (count > reader.Remaining())
        throw std::runtime_error("Invalid binary asset field table");
    std::vector<std::string> keys;
    keys.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i)
        keys.push_back(reader.StringValue());
    JsonValue result = reader.Value(keys, 0);
    if (!reader.Empty())
        throw std::runtime_error("Trailing data in binary asset");
    return result;
}
}
