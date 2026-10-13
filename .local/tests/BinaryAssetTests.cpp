#include "Core/Serialization/Json.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using Engine::Serialization::AssetParseFile;
using Engine::Serialization::AssetWriteBinary;
using Engine::Serialization::JsonParseFile;
using Engine::Serialization::JsonValue;

int main(int argc, char** argv)
{
    if (argc == 3)
    {
        try
        {
            const auto source = JsonParseFile(argv[1]);
            const auto binary = AssetWriteBinary(source);
            std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
            output.write(binary.data(), static_cast<std::streamsize>(binary.size()));
            output.close();
            if (Engine::Serialization::JsonWrite(AssetParseFile(argv[2])) !=
                Engine::Serialization::JsonWrite(source))
                throw std::runtime_error("Converted asset changed values");
            return 0;
        }
        catch (const std::exception& error)
        {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    const auto path = std::filesystem::temp_directory_path() /
        "engine-binary-asset-regression.anim";
    try
    {
        JsonValue root = JsonValue::MakeObject()
            .Set("version", JsonValue(1))
            .Set("name", JsonValue("0.0"))
            .Set("enabled", JsonValue(true));
        JsonValue samples = JsonValue::MakeArray();
        samples.Push(JsonValue(-42));
        samples.Push(JsonValue(0.125f));
        samples.Push(JsonValue(0.1));
        samples.Push(JsonValue(-2147483648.0));
        samples.Push(JsonValue(-0.0));
        root.Set("samples", std::move(samples));
        const std::string binary = AssetWriteBinary(root);
        if (binary.substr(0, 4) != "EBIN")
            throw std::runtime_error("Missing binary header");
        {
            std::ofstream output(path, std::ios::binary);
            output.write(binary.data(), static_cast<std::streamsize>(binary.size()));
        }
        const auto loaded = AssetParseFile(path.string());
        if (loaded["name"].AsString() != "0.0" ||
            !loaded["enabled"].AsBool() ||
            loaded["samples"].ArraySize() != 5 ||
            loaded["samples"].ArrayAt(0).AsInt() != -42 ||
            loaded["samples"].ArrayAt(1).AsDouble() != 0.125 ||
            loaded["samples"].ArrayAt(2).AsDouble() != 0.1 ||
            loaded["samples"].ArrayAt(3).AsDouble() != -2147483648.0 ||
            !std::signbit(loaded["samples"].ArrayAt(4).AsDouble()))
            throw std::runtime_error("Binary round trip changed values");
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << "{\"version\":1,\"name\":\"legacy\"}";
        }
        if (AssetParseFile(path.string())["name"].AsString() != "legacy" ||
            JsonParseFile(path.string())["name"].AsString() != "legacy")
            throw std::runtime_error("Legacy JSON asset failed to load");
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output.write(binary.data(), static_cast<std::streamsize>(binary.size() - 1));
        }
        bool rejected = false;
        try { (void)AssetParseFile(path.string()); }
        catch (const std::exception&) { rejected = true; }
        if (!rejected)
            throw std::runtime_error("Truncated binary asset was accepted");
        std::filesystem::remove(path);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::filesystem::remove(path);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
