#include "PortableAssetCopy.h"
#include "Core/Serialization/Json.h"
#include <pugixml.hpp>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace Engine::Editor
{
namespace
{
namespace fs = std::filesystem;
using Engine::Serialization::JsonValue;

std::string Read(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read dependency: " + path.string());
    return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
}

void Write(const fs::path& path, const std::string& contents)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
    if (!output) throw std::runtime_error("Cannot write imported asset: " + path.string());
}

std::string Trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

fs::path Resolve(const fs::path& owner, const std::string& reference)
{
    const fs::path requested(reference);
    const fs::path local = requested.is_absolute() ? requested : owner.parent_path() / requested;
    if (fs::is_regular_file(local)) return fs::canonical(local);
    // Legacy assets may use a path relative to the project working directory.
    if (!requested.is_absolute() && fs::is_regular_file(requested))
        return fs::canonical(requested);
    throw std::runtime_error("Missing dependency '" + reference + "' in " + owner.string());
}

class Package
{
public:
    explicit Package(const fs::path& destination)
        : root(destination.parent_path() /
            (destination.stem().string() + "_dependencies")) {}

    std::string Copy(const fs::path& owner, const std::string& reference)
    {
        const fs::path original = Resolve(owner, reference);
        auto found = copied.find(original);
        if (found != copied.end()) return found->second;
        fs::create_directories(root);
        fs::path target = root / original.filename();
        for (unsigned index = 2; fs::exists(target); ++index)
            target = root / (original.stem().string() + " " +
                std::to_string(index) + original.extension().string());
        fs::copy_file(original, target);
        return copied.emplace(original, target.filename().generic_string())
            .first->second;
    }

    fs::path root;
    std::map<fs::path, std::string> copied;
};

void RewriteImage(JsonValue& value, const char* key, const fs::path& sheet,
    Package& package)
{
    if (value.Has(key) && value[key].IsString() && !value[key].AsString().empty())
        value.Set(key, JsonValue(package.Copy(sheet, value[key].AsString())));
}

void RewriteFrames(JsonValue& frames, const fs::path& sheet, Package& package)
{
    for (size_t i = 0; i < frames.ArraySize(); ++i)
        RewriteImage(frames.ArrayAt(i), "image", sheet, package);
}

void RewriteSheet(const fs::path& source, const fs::path& destination,
    Package& package)
{
    const std::string contents = Read(source);
    const auto first = contents.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) throw std::runtime_error("Empty sprite sheet");
    if (contents[first] == '{' || contents[first] == '[')
    {
        JsonValue sheet = Engine::Serialization::JsonParse(contents);
        RewriteImage(sheet, "image", source, package);
        RewriteImage(sheet, "texture", source, package);
        RewriteFrames(sheet["frames"], source, package);
        JsonValue& animations = sheet["animations"];
        if (animations.IsArray())
            for (size_t i = 0; i < animations.ArraySize(); ++i)
                RewriteFrames(animations.ArrayAt(i)["frames"], source, package);
        else if (animations.IsObject())
        {
            // JsonValue has no mutable object iterator; update each key.
            for (size_t i = 0; i < animations.ObjectSize(); ++i)
            {
                const std::string key = animations.ObjectKey(i);
                RewriteFrames(animations[key]["frames"], source, package);
            }
        }
        Write(destination, Engine::Serialization::JsonWrite(sheet) + "\n");
        return;
    }
    pugi::xml_document sheet;
    if (!sheet.load_string(contents.c_str()))
        throw std::runtime_error("Invalid sprite sheet: " + source.string());
    auto root = sheet.document_element();
    for (const char* key : { "image", "texture" })
        if (auto attribute = root.attribute(key); attribute && *attribute.value())
            attribute.set_value(package.Copy(source, attribute.value()).c_str());
    for (auto animation : root.children("Animation"))
        for (auto frame : animation.children("Frame"))
            if (auto image = frame.attribute("image"); image && *image.value())
                image.set_value(package.Copy(source, image.value()).c_str());
    if (!sheet.save_file(destination.string().c_str()))
        throw std::runtime_error("Cannot write sprite sheet: " + destination.string());
}

std::string RewriteMaterial(const fs::path& source, Package& package)
{
    std::istringstream input(Read(source));
    std::ostringstream output;
    std::string line;
    while (std::getline(input, line))
    {
        const std::string trimmed = Trim(line);
        const auto split = trimmed.find_first_of(" \t");
        const std::string key = trimmed.substr(0, split);
        if (split != std::string::npos &&
            (key.rfind("map_", 0) == 0 || key == "bump" ||
             key == "disp" || key == "decal" || key == "norm" ||
             key == "refl"))
        {
            const std::string arguments = Trim(trimmed.substr(split));
            // MTL options precede the filename. Preserve them and replace the
            // final path. A declaration without options may contain spaces.
            const auto pathStart = arguments.find_last_of(" \t");
            const bool hasOptions = !arguments.empty() && arguments.front() == '-';
            const std::string name = !hasOptions || pathStart == std::string::npos
                ? arguments : arguments.substr(pathStart + 1);
            const std::string prefix = !hasOptions || pathStart == std::string::npos
                ? std::string{} : arguments.substr(0, pathStart + 1);
            line = key + " " + prefix + package.Copy(source, name);
        }
        output << line << '\n';
    }
    return output.str();
}

void CopyObj(const fs::path& source, const fs::path& destination, Package& package)
{
    std::istringstream input(Read(source));
    std::ostringstream output;
    std::string line;
    while (std::getline(input, line))
    {
        const std::string trimmed = Trim(line);
        if (trimmed.rfind("mtllib ", 0) == 0)
        {
            std::istringstream names(trimmed.substr(7));
            std::string name;
            line = "mtllib";
            while (names >> name)
            {
                const fs::path material = Resolve(source, name);
                const std::string copiedName = package.Copy(source, name);
                Write(package.root / copiedName, RewriteMaterial(material, package));
                line += " " + package.root.filename().generic_string() +
                    "/" + copiedName;
            }
        }
        output << line << '\n';
    }
    Write(destination, output.str());
}
}

void CopyPortableAsset(const fs::path& source, const fs::path& destination)
{
    std::string extension = source.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension != ".obj" && extension != ".spriteanim")
        throw std::invalid_argument("Unsupported portable asset: " + source.string());
    if (fs::exists(destination))
        throw std::runtime_error("Import destination exists: " + destination.string());
    Package package(destination);
    if (fs::exists(package.root))
        throw std::runtime_error("Import dependency directory exists: " + package.root.string());
    try
    {
        if (extension == ".obj") CopyObj(source, destination, package);
        else
        {
            JsonValue animation = Engine::Serialization::JsonParseFile(source.string());
            const std::string sheetName = animation.Has("spriteSheet")
                ? animation["spriteSheet"].AsString()
                : animation["spriteSheetFile"].AsString();
            if (sheetName.empty()) throw std::runtime_error("Sprite animation has no sheet");
            const fs::path sheet = Resolve(source, sheetName);
            const std::string copiedName = package.Copy(source, sheetName);
            RewriteSheet(sheet, package.root / copiedName, package);
            animation.Set("spriteSheet", JsonValue(
                package.root.filename().generic_string() + "/" + copiedName));
            animation.Erase("spriteSheetFile");
            Write(destination, Engine::Serialization::JsonWrite(animation) + "\n");
        }
    }
    catch (...)
    {
        std::error_code ignored;
        fs::remove(destination, ignored);
        fs::remove_all(package.root, ignored);
        throw;
    }
}
}
