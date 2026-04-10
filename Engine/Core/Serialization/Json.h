#pragma once
#include <string>
#include <memory>

// ---------------------------------------------------------------------------
// JsonValue  — lightweight, value-semantic JSON node.
//
// Supports: null | bool | number (double) | string | array | object
//
// Array and object data is heap-allocated behind shared_ptr<void> so the
// class never depends on an incomplete self-referential template
// specialisation.  The concrete storage types (vector<JsonValue> etc.)
// live only in Json.cpp and are never exposed in this header.
//
// Building values:
//   JsonValue obj = JsonValue::MakeObject();
//   obj.Set("name", JsonValue("Cube"));
//   obj.Set("x",    JsonValue(1.0f));
//
//   JsonValue arr = JsonValue::MakeArray();
//   arr.Push(JsonValue(1.0f)).Push(JsonValue(2.0f)).Push(JsonValue(3.0f));
//
// Reading values:
//   float x  = obj["x"].AsFloat();
//   bool  ok  = obj.Has("name");
// ---------------------------------------------------------------------------
class JsonValue
{
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    // ---- Constructors -------------------------------------------------------
    JsonValue();                               // null
    explicit JsonValue(bool b);
    JsonValue(double  d);
    JsonValue(float   f);
    JsonValue(int     i);
    JsonValue(const std::string& s);
    JsonValue(std::string&&      s);
    JsonValue(const char* s);

    JsonValue(const JsonValue&);
    JsonValue(JsonValue&&) noexcept;
    JsonValue& operator=(JsonValue other);
    ~JsonValue();

    // ---- Type queries -------------------------------------------------------
    Type GetType() const  { return m_type; }
    bool IsNull()   const { return m_type == Type::Null;   }
    bool IsBool()   const { return m_type == Type::Bool;   }
    bool IsNumber() const { return m_type == Type::Number; }
    bool IsString() const { return m_type == Type::String; }
    bool IsArray()  const { return m_type == Type::Array;  }
    bool IsObject() const { return m_type == Type::Object; }

    // ---- Scalar accessors ---------------------------------------------------
    bool               AsBool()   const;
    double             AsDouble() const;
    float              AsFloat()  const;
    int                AsInt()    const;
    const std::string& AsString() const;

    // ---- Array accessors (safe no-op when not an array) ---------------------
    std::size_t      ArraySize()             const;
    const JsonValue& ArrayAt(std::size_t i)  const;

    // ---- Object accessors (safe no-op when not an object) -------------------
    std::size_t        ObjectSize()               const;
    const std::string& ObjectKey(std::size_t i)   const;
    const JsonValue&   ObjectValue(std::size_t i) const;

    bool             Has(const std::string& key) const;
    // Returns a null JsonValue when the key is absent — never throws.
    const JsonValue& operator[](const std::string& key) const;

    // ---- Mutation -----------------------------------------------------------
    // Calling Set() on a null value promotes it to an object automatically.
    JsonValue& Set(const std::string& key, JsonValue val);
    // Calling Push() on a null value promotes it to an array automatically.
    JsonValue& Push(JsonValue val);

    // ---- Factory helpers ----------------------------------------------------
    static JsonValue MakeObject();
    static JsonValue MakeArray();

private:
    void swap(JsonValue& o) noexcept;

    Type        m_type   = Type::Null;
    bool        m_bool   = false;
    double      m_number = 0.0;
    std::string m_string;

    // Heap-allocated array / object storage (type-erased to avoid cross-TU
    // incomplete-template issues).  See Json.cpp for the concrete layout.
    std::shared_ptr<void> m_data;
};

// ---- Free functions ---------------------------------------------------------

// Pretty-print a JsonValue tree to a JSON string.
std::string JsonWrite(const JsonValue& v, int startIndent = 0);

// Parse a JSON string; throws std::runtime_error on parse error.
JsonValue JsonParse(const std::string& src);

// Read a file and parse it; throws on I/O or parse error.
JsonValue JsonParseFile(const std::string& path);
