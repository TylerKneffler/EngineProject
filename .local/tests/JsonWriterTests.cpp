#include "Core/Serialization/Json.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace Engine::Serialization;

void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// Compatibility reference: preserve the writer's existing byte treatment,
// including raw control bytes that are outside this performance change's scope.
std::string Reference(const std::string& text)
{
    std::string out="\"";
    for (char c:text)
        switch (c)
        {
        case '\\': out+="\\\\"; break;
        case '"': out+="\\\""; break;
        case '\n': out+="\\n"; break;
        case '\r': out+="\\r"; break;
        case '\t': out+="\\t"; break;
        default: out+=c; break;
        }
    return out+'"';
}

void Verify(const std::string& text)
{
    const auto expected=Reference(text);
    Check(JsonWriteCompact(JsonValue(text))==expected,"compact string bytes changed");
    Check(JsonWrite(JsonValue(text))==expected+'\n',"pretty string bytes changed");
    auto object=JsonValue::MakeObject();
    object.Set(text,JsonValue(text));
    Check(JsonWriteCompact(object)=="{"+expected+":"+expected+"}","object key escaping changed");
}

void Benchmark(const char* name,const JsonValue& value,int repeats)
{
    size_t bytes=0;
    const auto start=std::chrono::steady_clock::now();
    for(int i=0;i<repeats;++i) bytes+=JsonWriteCompact(value).size();
    std::cout << name << " ms_per_write=" <<
        std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/repeats
        << " output_bytes=" << bytes/repeats << '\n';
}

int main(int argc,char** argv)
{
    try
    {
        Check(JsonWriteCompact(JsonValue("\"\\\n\r\t"))=="\"\\\"\\\\\\n\\r\\t\"","escape golden changed");
        for(const auto& text:{std::string{},std::string("plain"),std::string("\"start"),
            std::string("end\\"),std::string("a\n\r\t\\\"b"),std::string("UTF8 \xc3\xa9 \xe6\xa3\xae")})
            Verify(text);
        std::string bytes;
        for(int i=0;i<256;++i) bytes+=static_cast<char>(i);
        Verify(bytes);
        std::string large(1024*1024,'a');
        Verify(large);
        for(size_t index:{size_t(0),size_t(15),size_t(4095),large.size()-1}) large[index]='"';
        Verify(large);
        Check(JsonParse(JsonWriteCompact(JsonValue(large))).AsString()==large,"large escaped string roundtrip failed");
        std::cout << "JSON string byte compatibility and ownership checks passed\n";
        if(argc>1 && std::string(argv[1])=="--benchmark")
        {
            Benchmark("baked_hex_16MiB",JsonValue(std::string(16*1024*1024,'a')),8);
            auto paths=JsonValue::MakeArray();
            for(int i=0;i<32768;++i) paths.Push(JsonValue("Engine/Core/Assets/Prefabs/Woodland/Grass.prefab"));
            Benchmark("placement_paths",paths,8);
            std::string dense;
            for(int i=0;i<65536;++i) dense+="a\n\\\"\t";
            Benchmark("dense_escapes",JsonValue(std::move(dense)),8);
        }
    }
    catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
