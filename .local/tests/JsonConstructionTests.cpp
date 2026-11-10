#include "Core/Serialization/Json.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace Engine::Serialization;
using Clock=std::chrono::steady_clock;

void Check(bool ok,const char* message)
{
    if(!ok) throw std::runtime_error(message);
}

JsonValue Records(size_t count,bool reserveArrays,bool reserveObjects=true)
{
    auto records=reserveArrays ? JsonValue::MakeArray(count) : JsonValue::MakeArray();
    for(size_t i=0;i<count;++i)
    {
        auto record=reserveObjects ? JsonValue::MakeObject(6) : JsonValue::MakeObject();
        auto position=reserveArrays ? JsonValue::MakeArray(3) : JsonValue::MakeArray();
        auto normal=reserveArrays ? JsonValue::MakeArray(3) : JsonValue::MakeArray();
        position.Push(JsonValue(float(i))).Push(JsonValue(0.f)).Push(JsonValue(-2.f));
        normal.Push(JsonValue(0.f)).Push(JsonValue(1.f)).Push(JsonValue(0.f));
        record.Set("id",JsonValue(std::to_string(i+1)))
            .Set("asset",JsonValue("Engine/Core/Assets/Prefabs/Woodland/Grass.prefab"))
            .Set("position",std::move(position)).Set("normal",std::move(normal))
            .Set("yawDegrees",JsonValue(15.f)).Set("scale",JsonValue(1.f));
        records.Push(std::move(record));
    }
    return records;
}

void Benchmark()
{
    std::vector<double> construction[2],destruction[2];
    size_t observed=0;
    for(int round=0;round<9;++round)
        for(int item=0;item<2;++item)
        {
            const int reserve=(round+item)%2; // Alternate which path goes first.
            const auto start=Clock::now();
            auto records=Records(65536,reserve!=0,true);
            const auto built=Clock::now();
            observed+=records.ArraySize();
            records=JsonValue();
            const auto destroyed=Clock::now();
            if(round>=2)
            {
                construction[reserve].push_back(std::chrono::duration<double,std::milli>(built-start).count());
                destruction[reserve].push_back(std::chrono::duration<double,std::milli>(destroyed-built).count());
            }
        }
    for(int reserve=0;reserve<2;++reserve)
    {
        auto& build=construction[reserve]; auto& destroy=destruction[reserve];
        std::sort(build.begin(),build.end()); std::sort(destroy.begin(),destroy.end());
        std::cout << (reserve ? "reserved" : "default") << " records=65536 samples=" << build.size()
            << " construction_median_ms=" << build[build.size()/2]
            << " construction_min_ms=" << build.front() << " construction_max_ms=" << build.back()
            << " destruction_median_ms=" << destroy[destroy.size()/2] << '\n';
    }
    Check(observed==18*65536,"benchmark lost records");
}

int main(int argc,char** argv)
{
    try
    {
        for(size_t capacity:{size_t(0),size_t(1),size_t(6),size_t(32)})
        {
            auto object=JsonValue::MakeObject(capacity);
            Check(object.IsObject() && object.ObjectSize()==0 && JsonWriteCompact(object)=="{}",
                "member capacity created serialized fields");
            object.Set("first",JsonValue(1)).Set("second",JsonValue::MakeArray());
            object["second"].Push(JsonValue("owned"));
            object.Set("first",JsonValue(2));
            Check(JsonWriteCompact(object)=="{\"first\":2,\"second\":[\"owned\"]}",
                "reserved object changed key order or replacement semantics");
            auto copy=object;
            copy["second"].ArrayAt(0)=JsonValue("edited");
            Check(object["second"].ArrayAt(0).AsString()=="owned","reserved object lost deep-copy ownership");
            Check(copy.Erase("first") && !copy.Has("first"),"reserved object erase failed");
            for(int i=0;i<40;++i) copy.Set(std::to_string(i),JsonValue(i));
            Check(copy.ObjectSize()==41 && copy["39"].AsInt()==39,"reserved object failed to grow beyond hint");
            auto moved=std::move(copy);
            Check(moved.ObjectSize()==41 && copy.IsNull(),"reserved object move changed ownership");

            auto array=JsonValue::MakeArray(capacity);
            Check(array.IsArray() && array.ArraySize()==0 && JsonWriteCompact(array)=="[]",
                "element capacity created serialized values");
            for(size_t i=0;i<capacity+2;++i) array.Push(JsonValue(int(i)));
            Check(array.ArraySize()==capacity+2 && array.ArrayAt(capacity+1).AsInt()==int(capacity+1),
                "reserved array failed to grow beyond hint");
        }
        const auto normal=Records(31,false),reserved=Records(31,true);
        Check(JsonWrite(normal)==JsonWrite(reserved) && JsonWriteCompact(normal)==JsonWriteCompact(reserved),
            "reserved placement construction changed serialized output");
        std::cout << "JSON object and array construction compatibility passed\n";
        if(argc>1 && std::string(argv[1])=="--benchmark") Benchmark();
    }
    catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
