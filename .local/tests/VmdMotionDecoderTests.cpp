#include "Editor/Core/Importers/VmdMotionDecoder.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <cstring>

#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (false)

int main()
{
    using namespace Engine::Editor;
    try
    {
        // One identity bone key and no morph keys; no optional sections.
        std::vector<uint8_t> bytes(50 + 4 + 111 + 4, 0);
        std::memcpy(bytes.data(), "Vocaloid Motion Data 0002", 25);
        std::memset(bytes.data() + 30, 'M', 19);
        bytes[49] = 0x84; // Model name truncated at the 20-byte boundary.
        bytes[50] = 1;
        for (size_t offset = 54; offset < 68; offset += 2)
        {
            bytes[offset] = 0x83;
            bytes[offset + 1] = 0x58; // CP932 Japanese character.
        }
        bytes[68] = 0x81; // Bone name truncated at the 15-byte boundary.
        const float identity = 1.f;
        std::memcpy(bytes.data() + 54 + 15 + 4 + 12 + 12, &identity, 4);
        VmdMotion motion;
        std::string error;
        CHECK(VmdMotionDecoder::DecodeBytes(bytes, motion, error));
        CHECK(motion.modelName == std::string(19, 'M'));
        CHECK(motion.bones.size() == 1);
        CHECK(motion.bones[0].name == std::string("\xe3\x82\xb9\xe3\x82\xb9\xe3\x82\xb9\xe3\x82\xb9\xe3\x82\xb9\xe3\x82\xb9\xe3\x82\xb9"));
        bytes[68] = 'A'; // Complete full-width field retains its final byte.
        CHECK(VmdMotionDecoder::DecodeBytes(bytes, motion, error));
        CHECK(motion.bones[0].name.back() == 'A');
        bytes[66] = 0x81; bytes[67] = 0; // Incomplete before terminator is invalid.
        CHECK(!VmdMotionDecoder::DecodeBytes(bytes, motion, error));
        bytes[66] = 0x83; bytes[67] = 0x58; bytes[68] = 'A';
        bytes[55] = 0x20; // Invalid interior trail byte must not be repaired.
        CHECK(!VmdMotionDecoder::DecodeBytes(bytes, motion, error));
        bytes.resize(60);
        CHECK(!VmdMotionDecoder::DecodeBytes(bytes, motion, error));

        const auto directory = std::filesystem::path("Engine/Core/Assets/Models/mmd/sad cat dance");
        for (const auto* name : { "dancing.vmd", "camera.vmd", "headband.vmd", "praying.vmd" })
        {
            const auto path = directory / name;
            CHECK(std::filesystem::is_regular_file(path));
            if (!VmdMotionDecoder::DecodeFile(path.string(), motion, error))
                throw std::runtime_error(path.string() + ": " + error);
            if (std::string(name) == "dancing.vmd") CHECK(!motion.bones.empty());
            if (std::string(name) == "camera.vmd") CHECK(!motion.cameras.empty());
            std::cout << name << ": " << motion.bones.size() << " bone keys, "
                << motion.morphs.size() << " morph keys, " << motion.cameras.size() << " camera keys\n";
        }
        std::cout << "VMD name boundary and bundled motion regressions passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
