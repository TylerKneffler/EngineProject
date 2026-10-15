#include "PmxModelDecoder.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <glm/gtc/matrix_transform.hpp>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Engine::Editor
{
namespace
{
class Reader
{
public:
    explicit Reader(std::vector<unsigned char> bytes) : m_bytes(std::move(bytes)) {}

    void Skip(size_t size)
    {
        if (size > m_bytes.size() - m_position)
            throw std::runtime_error("Truncated PMX at byte " + std::to_string(m_position));
        m_position += size;
    }

    template<class T> T Read()
    {
        T value{};
        if (sizeof(T) > m_bytes.size() - m_position)
            throw std::runtime_error("Truncated PMX at byte " + std::to_string(m_position));
        std::memcpy(&value, m_bytes.data() + m_position, sizeof(T));
        m_position += sizeof(T);
        return value;
    }

    int Count(const char* kind, int maximum)
    {
        const int value = Read<int32_t>();
        if (value < 0 || value > maximum)
            throw std::runtime_error(std::string("Invalid PMX ") + kind + " count");
        return value;
    }

    int Index(int width, bool vertex = false)
    {
        if (vertex)
        {
            if (width == 1) return Read<uint8_t>();
            if (width == 2) return Read<uint16_t>();
        }
        else
        {
            if (width == 1) return Read<int8_t>();
            if (width == 2) return Read<int16_t>();
        }
        if (width == 4) return Read<int32_t>();
        throw std::runtime_error("Invalid PMX index width");
    }

    glm::vec3 Vec3()
    {
        const float x = Read<float>(), y = Read<float>(), z = Read<float>();
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
            throw std::runtime_error("Non-finite PMX coordinate");
        return { x, y, -z }; // PMX left-handed to engine right-handed.
    }

    std::string Text(bool utf8)
    {
        const int length = Count("text byte", 4'000'000);
        if (static_cast<size_t>(length) > m_bytes.size() - m_position)
            throw std::runtime_error("Truncated PMX text");
        const char* data = reinterpret_cast<const char*>(m_bytes.data() + m_position);
        m_position += length;
        if (!length) return {};
        if (!utf8 && (length & 1))
            throw std::runtime_error("Odd PMX UTF-16 byte count");
        std::wstring wide;
        if (utf8)
        {
            const int wideLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                data, length, nullptr, 0);
            if (wideLength <= 0) throw std::runtime_error("Invalid PMX UTF-8 text");
            wide.resize(wideLength);
            if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                    data, length, wide.data(), wideLength))
                throw std::runtime_error("Invalid PMX UTF-8 text");
        }
        else
        {
            wide.resize(length / 2);
            for (int i = 0; i < length; i += 2)
                wide[i / 2] = static_cast<wchar_t>(
                    static_cast<unsigned char>(data[i]) |
                    (static_cast<unsigned char>(data[i + 1]) << 8));
        }
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            wide.data(), static_cast<int>(wide.size()),
            nullptr, 0, nullptr, nullptr);
        if (size <= 0) throw std::runtime_error("Invalid PMX text encoding");
        std::string result(size, '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            wide.data(), static_cast<int>(wide.size()),
            result.data(), size, nullptr, nullptr);
        return result;
    }

private:
    std::vector<unsigned char> m_bytes;
    size_t m_position = 0;
};

struct Bone
{
    std::string name;
    glm::vec3 position{};
    int parent = -1;
    int deformLayer = 0;
    uint16_t flags = 0;
    glm::vec3 fixedAxis { 0.f };
    glm::vec3 localAxisX { 0.f };
    glm::vec3 localAxisZ { 0.f };
    int externalParentKey = 0;
    int appendSource = -1;
    float appendRatio = 0.f;
    Engine::Model::ImportedIkChain ik;
};

struct MaterialRange
{
    int begin = 0;
    int count = 0;
};

struct PmxMorph
{
    struct Offset
    {
        glm::vec3 position { 0.f };
        glm::vec4 uv { 0.f };
        std::array<glm::vec4, 5> uvChannels {};
    };
    std::string name;
    int type = -1;
    std::unordered_map<int, Offset> offsets;
    std::vector<Engine::Model::ImportedBoneMorphOffset> boneOffsets;
    std::vector<Engine::Model::ImportedMaterialMorphOffset> materialOffsets;
    std::vector<Engine::Model::ImportedImpulseMorphOffset> impulseOffsets;
    std::vector<std::pair<int, float>> children;
    bool supported = false;
};

void SetInfluences(Engine::Model::AnimationVertex& vertex,
    const std::array<int, 4>& bones, const std::array<float, 4>& weights)
{
    float total = 0.f;
    for (size_t i = 0; i < 4; ++i)
        if (bones[i] >= 0 && std::isfinite(weights[i]) && weights[i] > 0.f)
            total += weights[i];
    if (total <= 0.f) return;
    for (size_t i = 0; i < 4; ++i)
        if (bones[i] >= 0 && std::isfinite(weights[i]) && weights[i] > 0.f)
        {
            vertex.joints0[i] = static_cast<float>(bones[i]);
            vertex.weights0[i] = weights[i] / total;
        }
}

void CheckIndex(int index, int count, const char* label, bool optional = false)
{
    if (index < (optional ? -1 : 0) || index >= count)
        throw std::runtime_error(std::string("Invalid PMX ") + label + " reference");
}
}

bool PmxModelDecoder::Decode(const std::string& sourcePath,
    Engine::Model::ImportedModel& model, std::string& error,
    PmxImportReport* report, PmxImportOptions options)
{
    try
    {
        if (report) *report = {};
        std::ifstream input(sourcePath, std::ios::binary);
        if (!input) throw std::runtime_error("Could not open PMX file");
        input.seekg(0, std::ios::end);
        const auto byteCount = input.tellg();
        if (byteCount < 0) throw std::runtime_error("Could not read PMX file size");
        std::vector<unsigned char> bytes(static_cast<size_t>(byteCount));
        input.seekg(0, std::ios::beg);
        if (!input.read(reinterpret_cast<char*>(bytes.data()), byteCount))
            throw std::runtime_error("Could not read PMX file");
        Reader reader(std::move(bytes));
        const auto signature = reader.Read<std::array<char, 4>>();
        if (signature != std::array<char, 4>{ 'P', 'M', 'X', ' ' })
            throw std::runtime_error("Invalid PMX signature");
        const float version = reader.Read<float>();
        if (std::abs(version - 2.f) > 0.01f && std::abs(version - 2.1f) > 0.01f)
            throw std::runtime_error("Unsupported PMX version");
        if (reader.Read<uint8_t>() != 8)
            throw std::runtime_error("Unsupported PMX header length");
        const int encoding = reader.Read<uint8_t>();
        if (encoding != 0 && encoding != 1)
            throw std::runtime_error("Unsupported PMX text encoding");
        const bool utf8 = encoding == 1;
        const int extraUv = reader.Read<uint8_t>();
        const int vertexWidth = reader.Read<uint8_t>();
        const int textureWidth = reader.Read<uint8_t>();
        const int materialWidth = reader.Read<uint8_t>();
        const int boneWidth = reader.Read<uint8_t>();
        const int morphWidth = reader.Read<uint8_t>();
        const int rigidWidth = reader.Read<uint8_t>();
        const std::array<int, 6> widths{ vertexWidth, textureWidth, materialWidth,
            boneWidth, morphWidth, rigidWidth };
        if (extraUv > 4 ||
            std::any_of(widths.begin(), widths.end(),
                [](int width) { return width != 1 && width != 2 && width != 4; }))
            throw std::runtime_error("Invalid PMX header settings");

        model = {};
        const std::string localName = reader.Text(utf8);
        const std::string englishName = reader.Text(utf8);
        reader.Text(utf8);
        reader.Text(utf8);
        model.name = englishName.empty() ? localName : englishName;
        if (model.name.empty()) model.name = std::filesystem::path(sourcePath).stem().string();
        model.sourceFormat = "pmx";
        // PMX has no encoded unit scale. ModelImporter applies the user's
        // chosen scale at the prefab root after decoding.
        model.unitScaleToMeters = 1.f;

        const int vertexCount = reader.Count("vertex", 5'000'000);
        std::vector<Engine::Model::AnimationVertex> vertices(vertexCount);
        std::vector<std::array<int, 4>> vertexBones(vertexCount);
        for (int i = 0; i < vertexCount; ++i)
        {
            auto& vertex = vertices[i];
            const auto position = reader.Vec3();
            const auto normal = reader.Vec3();
            vertex.pos[0] = position.x; vertex.pos[1] = position.y; vertex.pos[2] = position.z;
            vertex.normal[0] = normal.x; vertex.normal[1] = normal.y; vertex.normal[2] = normal.z;
            vertex.tangent[0] = 1.f; vertex.tangent[3] = 1.f;
            vertex.uv[0] = reader.Read<float>();
            vertex.uv[1] = reader.Read<float>();
            vertex.uvChannels[0][0] = vertex.uv[0];
            vertex.uvChannels[0][1] = vertex.uv[1];
            for (int channel = 0; channel < extraUv; ++channel)
            {
                const float x = reader.Read<float>(), y = reader.Read<float>();
                const float z = reader.Read<float>(), w = reader.Read<float>();
                vertex.uvChannels[channel + 1][0] = x;
                vertex.uvChannels[channel + 1][1] = y;
                vertex.uvChannels[channel + 1][2] = z;
                vertex.uvChannels[channel + 1][3] = w;
                if (channel == 0)
                {
                    vertex.uv1[0] = x;
                    vertex.uv1[1] = y;
                }
            }
            const int deform = reader.Read<uint8_t>();
            std::array<int, 4> bones{ -1, -1, -1, -1 };
            std::array<float, 4> weights{};
            if (deform == 0)
            {
                bones[0] = reader.Index(boneWidth); weights[0] = 1.f;
            }
            else if (deform == 1 || deform == 3)
            {
                bones[0] = reader.Index(boneWidth);
                bones[1] = reader.Index(boneWidth);
                weights[0] = reader.Read<float>(); weights[1] = 1.f - weights[0];
                if (deform == 3)
                {
                    const auto center = reader.Vec3();
                    const auto radius0 = reader.Vec3();
                    const auto radius1 = reader.Vec3();
                    vertex.sdefCenterAndMode[0] = center.x;
                    vertex.sdefCenterAndMode[1] = center.y;
                    vertex.sdefCenterAndMode[2] = center.z;
                    vertex.sdefCenterAndMode[3] = 3.f;
                    vertex.sdefRadius0[0] = radius0.x;
                    vertex.sdefRadius0[1] = radius0.y;
                    vertex.sdefRadius0[2] = radius0.z;
                    vertex.sdefRadius1[0] = radius1.x;
                    vertex.sdefRadius1[1] = radius1.y;
                    vertex.sdefRadius1[2] = radius1.z;
                }
            }
            else if (deform == 2 || deform == 4)
            {
                for (int& bone : bones) bone = reader.Index(boneWidth);
                for (float& weight : weights) weight = reader.Read<float>();
                if (deform == 4)
                {
                    vertex.sdefCenterAndMode[3] = 4.f;
                }
            }
            else throw std::runtime_error("Unsupported PMX vertex deform type");
            vertexBones[i] = bones;
            SetInfluences(vertex, bones, weights);
            reader.Read<float>(); // Edge scale.
        }

        const int indexCount = reader.Count("surface", 30'000'000);
        if (indexCount % 3) throw std::runtime_error("PMX surface count is not divisible by three");
        std::vector<int> indices(indexCount);
        for (int& index : indices)
        {
            index = reader.Index(vertexWidth, true);
            if (index < 0 || index >= vertexCount)
                throw std::runtime_error("PMX surface references an invalid vertex");
        }
        std::vector<std::string> textures;
        for (int i = reader.Count("texture", 100'000); i > 0; --i)
        {
            std::string path = reader.Text(utf8);
            std::replace(path.begin(), path.end(), '\\', '/');
            if (!path.empty())
            {
                const auto normalized = std::filesystem::u8path(path).lexically_normal();
                if (normalized.is_absolute() || normalized.has_root_name() ||
                    (!normalized.empty() && *normalized.begin() == ".."))
                    throw std::runtime_error("PMX texture path escapes the model directory");
            }
            textures.push_back(std::move(path));
        }
        const int materialCount = reader.Count("material", 100'000);
        std::vector<MaterialRange> ranges(materialCount);
        int covered = 0;
        int toonMaterials = 0, outlineMaterials = 0;
        for (int i = 0; i < materialCount; ++i)
        {
            auto& material = model.materials.emplace_back();
            const std::string local = reader.Text(utf8);
            const std::string english = reader.Text(utf8);
            material.name = english.empty() ? local : english;
            if (material.name.empty()) material.name = "Material " + std::to_string(i + 1);
            material.baseColor = { reader.Read<float>(), reader.Read<float>(),
                reader.Read<float>(), reader.Read<float>() };
            material.specularColor = { reader.Read<float>(), reader.Read<float>(),
                reader.Read<float>() };
            material.shininess = reader.Read<float>();
            material.ambientColor = { reader.Read<float>(), reader.Read<float>(),
                reader.Read<float>() };
            material.specularModel = "BlinnPhong";
            const uint8_t flags = reader.Read<uint8_t>();
            material.doubleSided = (flags & 1) != 0;
            material.castsShadows = (flags & 0x04) != 0;
            material.edgeEnabled = (flags & 0x10) != 0;
            if (material.edgeEnabled) ++outlineMaterials;
            material.alphaMode = material.baseColor.a < .999f ? "Blend" : "Opaque";
            for (int channel = 0; channel < 4; ++channel)
                material.edgeColor[channel] = reader.Read<float>();
            material.edgeWidth = reader.Read<float>();
            const int texture = reader.Index(textureWidth);
            const int sphereTexture = reader.Index(textureWidth);
            const int sphereMode = reader.Read<uint8_t>();
            if (sphereMode > 3) throw std::runtime_error("Invalid PMX sphere mode");
            CheckIndex(sphereTexture, static_cast<int>(textures.size()),
                "sphere texture", true);
            if (sphereTexture >= 0 && sphereMode != 0)
                material.sphereTexture = textures[sphereTexture];
            material.sphereMode = sphereMode;
            const int toonReference = reader.Read<uint8_t>();
            if (toonReference == 0)
            {
                const int toonTexture = reader.Index(textureWidth);
                CheckIndex(toonTexture, static_cast<int>(textures.size()),
                    "toon texture", true);
                if (toonTexture >= 0) ++toonMaterials;
                if (toonTexture >= 0)
                    material.toonTexture = textures[toonTexture];
            }
            else if (toonReference == 1)
            {
                material.builtinToon = reader.Read<uint8_t>();
                if (material.builtinToon > 9)
                    throw std::runtime_error("Invalid PMX built-in toon index");
                ++toonMaterials;
            }
            else throw std::runtime_error("Invalid PMX toon reference");
            reader.Text(utf8); // Material memo.
            const int count = reader.Count("material surface", indexCount);
            if (count % 3 || count > indexCount - covered)
                throw std::runtime_error("Invalid PMX material surface range");
            ranges[i] = { covered, count };
            covered += count;
            if (texture >= 0)
            {
                if (texture >= static_cast<int>(textures.size()))
                    throw std::runtime_error("PMX material references an invalid texture");
                material.baseColorTexture = textures[texture];
            }
        }
        if (covered != indexCount)
            throw std::runtime_error("PMX material ranges do not cover all surfaces");

        const int boneCount = reader.Count("bone", 100'000);
        std::vector<Bone> bones(boneCount);
        int inheritedBones = 0, ikChains = 0;
        int fixedAxisBones = 0, localAxisBones = 0;
        int afterPhysicsBones = 0, externalParentBones = 0;
        for (int i = 0; i < boneCount; ++i)
        {
            const std::string local = reader.Text(utf8);
            const std::string english = reader.Text(utf8);
            // VMD bone keys use the local MMD names.
            bones[i].name = local.empty() ? english : local;
            if (bones[i].name.empty()) bones[i].name = "Bone " + std::to_string(i + 1);
            bones[i].position = reader.Vec3();
            bones[i].parent = reader.Index(boneWidth);
            if (bones[i].parent < -1 || bones[i].parent >= boneCount || bones[i].parent == i)
                throw std::runtime_error("Invalid PMX bone parent");
            bones[i].deformLayer = reader.Read<int32_t>();
            const uint16_t flags = reader.Read<uint16_t>();
            bones[i].flags = flags;
            if (flags & 0x0001)
                CheckIndex(reader.Index(boneWidth), boneCount, "bone tail", true);
            else reader.Skip(12);
            if (flags & 0x0300)
            {
                ++inheritedBones;
                bones[i].appendSource = reader.Index(boneWidth);
                CheckIndex(bones[i].appendSource, boneCount, "inherited bone");
                bones[i].appendRatio = reader.Read<float>();
                if (!std::isfinite(bones[i].appendRatio) ||
                    bones[i].appendSource == i)
                    throw std::runtime_error("Invalid PMX inherited bone ratio");
            }
            if (flags & 0x0400)
            {
                ++fixedAxisBones;
                bones[i].fixedAxis = reader.Vec3();
            }
            if (flags & 0x0800)
            {
                ++localAxisBones;
                bones[i].localAxisX = reader.Vec3();
                bones[i].localAxisZ = reader.Vec3();
            }
            if (flags & 0x1000) ++afterPhysicsBones;
            if (flags & 0x2000)
            {
                ++externalParentBones;
                bones[i].externalParentKey = reader.Read<int32_t>();
            }
            if (flags & 0x0020)
            {
                ++ikChains;
                auto& ik = bones[i].ik;
                ik.controllerNodeIndex = static_cast<unsigned>(i);
                ik.effectorNodeIndex = static_cast<unsigned>(reader.Index(boneWidth));
                CheckIndex(static_cast<int>(ik.effectorNodeIndex), boneCount,
                    "IK target");
                ik.iterations = reader.Count("IK iteration", 100'000);
                ik.angleLimit = reader.Read<float>();
                ik.deformLayer = bones[i].deformLayer;
                ik.deformOrder = i;
                ik.afterPhysics = (flags & 0x1000) != 0;
                if (!std::isfinite(ik.angleLimit) || ik.angleLimit < 0.f)
                    throw std::runtime_error("Invalid PMX IK angle limit");
                for (int links = reader.Count("IK link", 1'000); links > 0; --links)
                {
                    Engine::Model::ImportedIkLink link;
                    link.nodeIndex = static_cast<unsigned>(reader.Index(boneWidth));
                    CheckIndex(static_cast<int>(link.nodeIndex), boneCount, "IK link");
                    const int limited = reader.Read<uint8_t>();
                    if (limited > 1) throw std::runtime_error("Invalid PMX IK limit flag");
                    link.limited = limited != 0;
                    if (limited)
                    {
                        const float minX = reader.Read<float>(), minY = reader.Read<float>();
                        const float minZ = reader.Read<float>();
                        const float maxX = reader.Read<float>(), maxY = reader.Read<float>();
                        const float maxZ = reader.Read<float>();
                        link.minimum = { -maxX, -maxY, minZ };
                        link.maximum = { -minX, -minY, maxZ };
                    }
                    ik.links.push_back(link);
                }
            }
        }
        for (const auto& boneIndices : vertexBones)
            for (int bone : boneIndices)
                if (bone < -1 || bone >= boneCount)
                    throw std::runtime_error("PMX vertex references an invalid bone");

        const int morphCount = reader.Count("morph", 100'000);
        std::vector<PmxMorph> morphs(morphCount);
        std::array<int, 11> morphTypeCounts{};
        std::vector<int> impulseRigidReferences;
        for (int i = 0; i < morphCount; ++i)
        {
            PmxMorph& morph = morphs[i];
            const std::string local = reader.Text(utf8);
            const std::string english = reader.Text(utf8);
            reader.Read<uint8_t>(); // MMD display panel.
            const int type = reader.Read<uint8_t>();
            if (type > 10) throw std::runtime_error("Unsupported PMX morph type");
            if (version < 2.05f && type >= 9)
                throw std::runtime_error("PMX 2.1 morph in a PMX 2.0 file");
            ++morphTypeCounts[type];
            morph.type = type;
            morph.name = local.empty() ? english : local;
            if (morph.name.empty()) morph.name = "Morph " + std::to_string(i + 1);
            const int count = reader.Count("morph offset", 5'000'000);
            if (type == 1)
            {
                morph.supported = true;
                for (int j = 0; j < count; ++j)
                {
                    const int index = reader.Index(vertexWidth, true);
                    if (index < 0 || index >= vertexCount)
                        throw std::runtime_error("PMX morph references an invalid vertex");
                    morph.offsets[index].position += reader.Vec3();
                }
            }
            else if (type >= 3 && type <= 7)
            {
                morph.supported = type == 3 || type - 3 <= extraUv;
                for (int j = 0; j < count; ++j)
                {
                    const int index = reader.Index(vertexWidth, true);
                    CheckIndex(index, vertexCount, "UV morph vertex");
                    const float x = reader.Read<float>(), y = reader.Read<float>();
                    const float z = reader.Read<float>(), w = reader.Read<float>();
                    if (!std::isfinite(x) || !std::isfinite(y) ||
                        !std::isfinite(z) || !std::isfinite(w))
                        throw std::runtime_error("Non-finite PMX UV morph offset");
                    if (morph.supported)
                    {
                        auto& offset = morph.offsets[index].uv;
                        if (type == 3) { offset.x += x; offset.y += y; }
                        else if (type == 4) { offset.z += x; offset.w += y; }
                        auto& full = morph.offsets[index].uvChannels[type - 3];
                        full += glm::vec4(x, y, z, w);
                    }
                }
            }
            else if (type == 0 || type == 9)
            {
                for (int j = 0; j < count; ++j)
                {
                    const int child = reader.Index(morphWidth);
                    const float influence = reader.Read<float>();
                    if (child < 0 || child >= morphCount || !std::isfinite(influence))
                        throw std::runtime_error("Invalid PMX group/flip morph reference");
                    morph.children.emplace_back(child, influence);
                }
            }
            else
            {
                if (type == 2 || type == 8 || type == 10) morph.supported = true;
                for (int j = 0; j < count; ++j)
                {
                    if (type == 2)
                    {
                        const int bone = reader.Index(boneWidth);
                        CheckIndex(bone, boneCount, "bone morph bone");
                        Engine::Model::ImportedBoneMorphOffset offset;
                        offset.nodeIndex = static_cast<unsigned>(bone);
                        offset.translation = reader.Vec3();
                        const float x = reader.Read<float>(), y = reader.Read<float>();
                        const float z = reader.Read<float>(), w = reader.Read<float>();
                        if (!std::isfinite(x) || !std::isfinite(y) ||
                            !std::isfinite(z) || !std::isfinite(w) ||
                            x*x + y*y + z*z + w*w < 1e-12f)
                            throw std::runtime_error("Invalid PMX bone morph rotation");
                        offset.rotation = glm::normalize(glm::quat(w, -x, -y, z));
                        morph.boneOffsets.push_back(offset);
                    }
                    else if (type == 8)
                    {
                        Engine::Model::ImportedMaterialMorphOffset offset;
                        offset.materialIndex = reader.Index(materialWidth);
                        CheckIndex(offset.materialIndex, materialCount,
                            "material morph material", true);
                        const int operation = reader.Read<uint8_t>();
                        if (operation > 1)
                            throw std::runtime_error("Invalid PMX material morph operation");
                        offset.additive = operation == 1;
                        for (int channel = 0; channel < 4; ++channel)
                            offset.diffuse[channel] = reader.Read<float>();
                        for (int channel = 0; channel < 3; ++channel)
                            offset.specular[channel] = reader.Read<float>();
                        offset.shininess = reader.Read<float>();
                        for (int channel = 0; channel < 3; ++channel)
                            offset.ambient[channel] = reader.Read<float>();
                        for (int channel = 0; channel < 4; ++channel)
                            offset.edgeColor[channel] = reader.Read<float>();
                        offset.edgeWidth = reader.Read<float>();
                        for (int channel = 0; channel < 4; ++channel)
                            offset.textureTint[channel] = reader.Read<float>();
                        for (int channel = 0; channel < 4; ++channel)
                            offset.sphereTint[channel] = reader.Read<float>();
                        for (int channel = 0; channel < 4; ++channel)
                            offset.toonTint[channel] = reader.Read<float>();
                        morph.materialOffsets.push_back(offset);
                    }
                    else if (type == 10)
                    {
                        Engine::Model::ImportedImpulseMorphOffset offset;
                        offset.bodyIndex = reader.Index(rigidWidth);
                        impulseRigidReferences.push_back(offset.bodyIndex);
                        const int localFlag = reader.Read<uint8_t>();
                        if (localFlag > 1)
                            throw std::runtime_error("Invalid PMX impulse local flag");
                        offset.local = localFlag != 0;
                        offset.velocity = reader.Vec3();
                        offset.torque = -reader.Vec3();
                        morph.impulseOffsets.push_back(offset);
                    }
                    else throw std::runtime_error("Unsupported PMX morph type");
                }
            }
        }

        // Resolve nested groups into the same mesh, bone, and material
        // channels used by direct morphs. An unsupported child invalidates
        // the entire group so no partial animation is silently imported.
        std::vector<int> morphVisits(morphCount, 0);
        std::function<bool(int)> resolveMorph = [&](int index)
        {
            PmxMorph& morph = morphs[index];
            if (morphVisits[index] == 2) return morph.supported;
            if (morphVisits[index] == 1)
                throw std::runtime_error("Cyclic PMX group morph");
            morphVisits[index] = 1;
            if (morph.type == 0 || morph.type == 9)
            {
                morph.supported = true;
                for (const auto& [childIndex, influence] : morph.children)
                    if (!resolveMorph(childIndex)) morph.supported = false;
                if (morph.supported)
                    for (const auto& [childIndex, influence] : morph.children)
                    {
                        const auto& child = morphs[childIndex];
                        for (const auto& [vertex, offset] : child.offsets)
                        {
                            auto& combined = morph.offsets[vertex];
                            combined.position += offset.position * influence;
                            combined.uv += offset.uv * influence;
                            for (size_t channel = 0; channel < combined.uvChannels.size(); ++channel)
                                combined.uvChannels[channel] +=
                                    offset.uvChannels[channel] * influence;
                        }
                        for (auto offset : child.boneOffsets)
                        {
                            offset.translation *= influence;
                            offset.rotation = glm::normalize(glm::slerp(
                                glm::quat(1.f, 0.f, 0.f, 0.f),
                                offset.rotation, influence));
                            morph.boneOffsets.push_back(offset);
                        }
                        for (auto offset : child.materialOffsets)
                        {
                            if (offset.additive)
                            {
                                offset.diffuse *= influence;
                                offset.specular *= influence;
                                offset.shininess *= influence;
                                offset.ambient *= influence;
                                offset.textureTint *= influence;
                                offset.edgeColor *= influence;
                                offset.edgeWidth *= influence;
                                offset.sphereTint *= influence;
                                offset.toonTint *= influence;
                            }
                            else
                            {
                                offset.diffuse = glm::mix(glm::vec4(1.f),
                                    offset.diffuse, influence);
                                offset.specular = glm::mix(glm::vec3(1.f),
                                    offset.specular, influence);
                                offset.shininess = glm::mix(1.f,
                                    offset.shininess, influence);
                                offset.ambient = glm::mix(glm::vec3(1.f),
                                    offset.ambient, influence);
                                offset.textureTint = glm::mix(glm::vec4(1.f),
                                    offset.textureTint, influence);
                                offset.edgeColor = glm::mix(glm::vec4(1.f),
                                    offset.edgeColor, influence);
                                offset.edgeWidth = glm::mix(1.f,
                                    offset.edgeWidth, influence);
                                offset.sphereTint = glm::mix(glm::vec4(1.f),
                                    offset.sphereTint, influence);
                                offset.toonTint = glm::mix(glm::vec4(1.f),
                                    offset.toonTint, influence);
                            }
                            morph.materialOffsets.push_back(offset);
                        }
                        for (auto offset : child.impulseOffsets)
                        {
                            if (influence == 0.f) continue;
                            offset.velocity *= influence;
                            offset.torque *= influence;
                            morph.impulseOffsets.push_back(offset);
                        }
                    }
            }
            morphVisits[index] = 2;
            return morph.supported;
        };
        for (int i = 0; i < morphCount; ++i) resolveMorph(i);
        if (report)
            for (const auto& morph : morphs)
                if ((morph.type == 0 || morph.type == 9) && morph.supported &&
                    (!morph.offsets.empty() || !morph.boneOffsets.empty() ||
                        !morph.materialOffsets.empty() ||
                        !morph.impulseOffsets.empty()))
                    ++report->importedSupportedGroupMorphs;

        const int displayCount = reader.Count("display frame", 100'000);
        for (int i = 0; i < displayCount; ++i)
        {
            reader.Text(utf8);
            reader.Text(utf8);
            reader.Read<uint8_t>();
            for (int entries = reader.Count("display item", 100'000);
                entries > 0; --entries)
            {
                const int kind = reader.Read<uint8_t>();
                if (kind == 0) reader.Index(boneWidth);
                else if (kind == 1) reader.Index(morphWidth);
                else throw std::runtime_error("Invalid PMX display item type");
            }
        }
        const int rigidCount = reader.Count("rigid body", 100'000);
        std::vector<int> importedRigidIndex(rigidCount, -1);
        std::vector<glm::vec3> rigidRestPositions(rigidCount);
        std::vector<glm::quat> rigidRestRotations(rigidCount);
        for (int index : impulseRigidReferences)
            CheckIndex(index, rigidCount, "impulse rigid body");
        int dynamicRigidCount = 0;
        for (int i = 0; i < rigidCount; ++i)
        {
            const std::string local = reader.Text(utf8);
            const std::string english = reader.Text(utf8);
            const int bone = reader.Index(boneWidth);
            CheckIndex(bone, boneCount, "rigid body bone", true);
            const int group = reader.Read<uint8_t>();
            const int mask = reader.Read<uint16_t>();
            const int shape = reader.Read<uint8_t>();
            const float sx = reader.Read<float>(), sy = reader.Read<float>();
            const float sz = reader.Read<float>();
            const glm::vec3 size(sx, sy, sz);
            const glm::vec3 worldPosition = reader.Vec3();
            const float rx = reader.Read<float>(), ry = reader.Read<float>();
            const float rz = reader.Read<float>();
            const float mass = reader.Read<float>();
            const float linearDamping = reader.Read<float>();
            const float angularDamping = reader.Read<float>();
            const float restitution = reader.Read<float>();
            const float friction = reader.Read<float>();
            const int mode = reader.Read<uint8_t>();
            rigidRestPositions[i] = worldPosition;
            rigidRestRotations[i] = glm::quat(glm::vec3(-rx, -ry, rz));
            if (group > 15 || shape > 2 || mode > 2 ||
                !std::isfinite(rx) || !std::isfinite(ry) || !std::isfinite(rz) ||
                !std::isfinite(friction) || !std::isfinite(restitution) ||
                !std::isfinite(mass) || mass < 0.f ||
                !std::isfinite(linearDamping) || linearDamping < 0.f ||
                !std::isfinite(angularDamping) || angularDamping < 0.f ||
                !std::isfinite(size.x) || !std::isfinite(size.y) ||
                !std::isfinite(size.z) || size.x <= 0.f ||
                (shape == 1 && (size.y <= 0.f || size.z <= 0.f)) ||
                (shape == 2 && size.y < 0.f))
                throw std::runtime_error("Invalid PMX rigid body settings at index " +
                    std::to_string(i) + " (group " + std::to_string(group) +
                    ", shape " + std::to_string(shape) + ", mode " +
                    std::to_string(mode) + ", size " +
                    std::to_string(size.x) + "," + std::to_string(size.y) +
                    "," + std::to_string(size.z) + ")");
            if (mode > 0) ++dynamicRigidCount;
            if ((mode == 0 && options.followBoneColliders) ||
                (mode > 0 && options.dynamicPhysics))
            {
                importedRigidIndex[i] = static_cast<int>(model.rigidBodies.size());
                auto& body = model.rigidBodies.emplace_back();
                body.name = local.empty() ? english : local;
                if (body.name.empty()) body.name = "Rigid body " +
                    std::to_string(i + 1);
                body.parentNode = mode == 0 ? bone : -1;
                body.relatedBoneNode = mode > 0 ? bone : -1;
                body.localPosition = mode == 0 ? worldPosition -
                    (bone < 0 ? glm::vec3(0.f) : bones[bone].position) :
                    worldPosition;
                body.localRotation = glm::quat(glm::vec3(-rx, -ry, rz));
                body.boneOffsetPosition = worldPosition -
                    (bone < 0 ? glm::vec3(0.f) : bones[bone].position);
                body.boneOffsetRotation = body.localRotation;
                body.mode = static_cast<Engine::Model::RigidBodyMotion>(mode);
                body.mass = mass;
                body.linearDamping = linearDamping;
                body.angularDamping = angularDamping;
                body.shape = static_cast<Engine::Model::RigidBodyShape>(shape);
                body.size = size;
                body.collisionGroup = group;
                body.nonCollisionMask = mask;
                body.friction = friction;
                body.restitution = restitution;
            }
        }
        int unavailableImpulseOffsets = 0;
        if (options.morphs && options.dynamicPhysics)
            for (const auto& morph : morphs)
                if (morph.supported && !morph.impulseOffsets.empty())
                {
                    auto& imported = model.impulseMorphs.emplace_back();
                    imported.name = morph.name;
                    for (auto offset : morph.impulseOffsets)
                    {
                        if (offset.bodyIndex < 0 ||
                            importedRigidIndex[offset.bodyIndex] < 0)
                        {
                            if (morph.type == 10) ++unavailableImpulseOffsets;
                            continue;
                        }
                        offset.bodyIndex = importedRigidIndex[offset.bodyIndex];
                        imported.offsets.push_back(offset);
                    }
                    if (imported.offsets.empty()) model.impulseMorphs.pop_back();
                }
        const int jointCount = reader.Count("joint", 100'000);
        int unsupportedJointCount = 0;
        for (int i = 0; i < jointCount; ++i)
        {
            const std::string local = reader.Text(utf8);
            const std::string english = reader.Text(utf8);
            const int type = reader.Read<uint8_t>();
            const int bodyA = reader.Index(rigidWidth);
            const int bodyB = reader.Index(rigidWidth);
            CheckIndex(bodyA, rigidCount, "joint body", true);
            CheckIndex(bodyB, rigidCount, "joint body", true);
            const glm::vec3 position = reader.Vec3();
            const glm::vec3 rotation = -reader.Vec3();
            const glm::vec3 rawLinearLower = reader.Vec3();
            const glm::vec3 rawLinearUpper = reader.Vec3();
            const glm::vec3 rawAngularLower = -reader.Vec3();
            const glm::vec3 rawAngularUpper = -reader.Vec3();
            const glm::vec3 linearSpring = reader.Vec3();
            const glm::vec3 angularSpring = reader.Vec3();
            if (type > 5 || (version < 2.05f && type != 0) ||
                (bodyA < 0 && bodyB < 0) || bodyA == bodyB ||
                (bodyA >= 0 && importedRigidIndex[bodyA] < 0) ||
                (bodyB >= 0 && importedRigidIndex[bodyB] < 0) ||
                !options.dynamicPhysics)
            {
                ++unsupportedJointCount;
                continue;
            }
            auto& joint = model.physicsJoints.emplace_back();
            joint.name = local.empty() ? english : local;
            joint.type = static_cast<Engine::Model::PhysicsJointKind>(type);
            joint.bodyA = bodyA < 0 ? -1 : importedRigidIndex[bodyA];
            joint.bodyB = bodyB < 0 ? -1 : importedRigidIndex[bodyB];
            joint.position = position;
            joint.rotation = glm::quat(rotation);
            joint.linearLower = (glm::min)(rawLinearLower, rawLinearUpper);
            joint.linearUpper = (glm::max)(rawLinearLower, rawLinearUpper);
            joint.angularLower = (glm::min)(rawAngularLower, rawAngularUpper);
            joint.angularUpper = (glm::max)(rawAngularLower, rawAngularUpper);
            joint.linearSpring = linearSpring;
            joint.angularSpring = angularSpring;
            joint.frameARotation = bodyA < 0 ? joint.rotation : glm::normalize(
                glm::inverse(rigidRestRotations[bodyA]) * joint.rotation);
            joint.frameBRotation = bodyB < 0 ? joint.rotation : glm::normalize(
                glm::inverse(rigidRestRotations[bodyB]) * joint.rotation);
            joint.frameAPosition = bodyA < 0 ? joint.position :
                glm::inverse(rigidRestRotations[bodyA]) *
                (joint.position - rigidRestPositions[bodyA]);
            joint.frameBPosition = bodyB < 0 ? joint.position :
                glm::inverse(rigidRestRotations[bodyB]) *
                (joint.position - rigidRestPositions[bodyB]);
        }
        int softBodyCount = 0;
        int unavailableSoftBodies = 0;
        int unavailableSoftAttachments = 0;
        std::unordered_set<int> importedSoftMaterials;
        if (version > 2.05f)
        {
            softBodyCount = reader.Count("soft body", 100'000);
            for (int i = 0; i < softBodyCount; ++i)
            {
                const std::string local = reader.Text(utf8);
                const std::string english = reader.Text(utf8);
                Engine::Model::ImportedSoftBody soft;
                soft.name = local.empty() ? english : local;
                const int sourceShape = reader.Read<uint8_t>();
                soft.shape = static_cast<Engine::Model::SoftBodyTopology>(sourceShape);
                soft.materialIndex = reader.Index(materialWidth);
                CheckIndex(soft.materialIndex, materialCount,
                    "soft body material");
                const int group = reader.Read<uint8_t>();
                soft.collisionGroup = group;
                soft.nonCollisionMask = reader.Read<uint16_t>();
                reader.Read<uint8_t>(); // B-link, cluster, link-crossing flags.
                reader.Read<int32_t>(); // B-link distance.
                soft.clusterCount = reader.Read<int32_t>();
                soft.mass = reader.Read<float>();
                soft.collisionMargin = reader.Read<float>();
                reader.Read<int32_t>(); // Aerodynamics model.
                reader.Read<float>(); // Velocity correction.
                soft.damping = reader.Read<float>();
                soft.drag = reader.Read<float>();
                reader.Skip(4 * 3); // Lift, pressure, volume conservation.
                soft.friction = reader.Read<float>();
                reader.Skip(4 * 5); // Pose match and contact hardness values.
                for (int channel = 0; channel < 3; ++channel)
                    soft.clusterHardness[channel] = reader.Read<float>();
                for (int channel = 0; channel < 3; ++channel)
                    soft.clusterImpulseSplit[channel] = reader.Read<float>();
                reader.Read<int32_t>(); // Velocity iterations.
                soft.solverIterations = reader.Read<int32_t>();
                reader.Read<int32_t>(); // Drift iterations.
                soft.clusterIterations = reader.Read<int32_t>();
                soft.linearStiffness = reader.Read<float>();
                soft.bendingStiffness = reader.Read<float>();
                reader.Read<float>(); // Volume stiffness.
                if (sourceShape > 1 || group > 15 ||
                    !std::isfinite(soft.mass) || soft.mass < 0.f ||
                    !std::isfinite(soft.collisionMargin) ||
                    soft.collisionMargin < 0.f ||
                    !std::isfinite(soft.linearStiffness) ||
                    !std::isfinite(soft.bendingStiffness) ||
                    !std::isfinite(soft.damping) ||
                    !std::isfinite(soft.drag) ||
                    !std::isfinite(soft.friction) ||
                    soft.solverIterations < 0 || soft.clusterCount < 0 ||
                    soft.clusterIterations < 0 ||
                    !std::isfinite(soft.clusterHardness.x) ||
                    !std::isfinite(soft.clusterHardness.y) ||
                    !std::isfinite(soft.clusterHardness.z) ||
                    !std::isfinite(soft.clusterImpulseSplit.x) ||
                    !std::isfinite(soft.clusterImpulseSplit.y) ||
                    !std::isfinite(soft.clusterImpulseSplit.z))
                    throw std::runtime_error("Invalid PMX soft body settings");
                const auto vertexInMaterial = [&](int vertex)
                {
                    const auto& range = ranges[soft.materialIndex];
                    return std::find(indices.begin() + range.begin,
                        indices.begin() + range.begin + range.count,
                        vertex) != indices.begin() + range.begin + range.count;
                };
                for (int anchors = reader.Count("soft body anchor", 1'000'000);
                    anchors > 0; --anchors)
                {
                    const int body = reader.Index(rigidWidth);
                    const int vertex = reader.Index(vertexWidth, true);
                    const int nearMode = reader.Read<uint8_t>();
                    CheckIndex(body, rigidCount, "soft body anchor rigid body");
                    CheckIndex(vertex, vertexCount, "soft body anchor vertex");
                    if (nearMode > 1)
                        throw std::runtime_error("Invalid PMX soft body anchor mode");
                    if (importedRigidIndex[body] >= 0 &&
                        vertexInMaterial(vertex))
                    {
                        auto& anchor = soft.anchors.emplace_back();
                        anchor.bodyIndex = importedRigidIndex[body];
                        anchor.vertexPosition = glm::vec3(
                            vertices[vertex].pos[0], vertices[vertex].pos[1],
                            vertices[vertex].pos[2]);
                        anchor.nearMode = nearMode != 0;
                    }
                    else ++unavailableSoftAttachments;
                }
                for (int pins = reader.Count("soft body pin", 1'000'000);
                    pins > 0; --pins)
                {
                    const int vertex = reader.Index(vertexWidth, true);
                    CheckIndex(vertex, vertexCount, "soft body pinned vertex");
                    if (vertexInMaterial(vertex))
                        soft.pinnedVertices.emplace_back(vertices[vertex].pos[0],
                            vertices[vertex].pos[1], vertices[vertex].pos[2]);
                    else ++unavailableSoftAttachments;
                }
                if (options.dynamicPhysics &&
                    ranges[soft.materialIndex].count > 0 &&
                    importedSoftMaterials.insert(soft.materialIndex).second)
                    model.softBodies.push_back(std::move(soft));
                else ++unavailableSoftBodies;
            }
        }
        if (report)
        {
            const auto warn = [&](int count, const char* feature)
            {
                if (count > 0)
                    report->warnings.push_back(std::to_string(count) + " " + feature +
                        " present but not faithfully imported");
            };
            int incompleteGroups = 0;
            for (const auto& morph : morphs)
                if ((morph.type == 0 || morph.type == 9) && !morph.supported)
                    ++incompleteGroups;
            warn(incompleteGroups, "group morphs with unsupported children");
            if (morphTypeCounts[4] && extraUv == 0)
                warn(morphTypeCounts[4], "additional-UV morphs without a UV1 stream");
            if (morphTypeCounts[8])
                report->warnings.push_back("Material morph specular/shininess render "
                    "through a PBR approximation of MMD lighting");
            if (morphTypeCounts[10] && !options.dynamicPhysics)
                warn(morphTypeCounts[10], "impulse morphs (dynamic physics disabled)");
            else
                warn(unavailableImpulseOffsets,
                    "impulse offsets targeting unavailable rigid bodies");
            report->importedKinematicRigidBodies = static_cast<int>(
                std::count_if(model.rigidBodies.begin(), model.rigidBodies.end(),
                    [](const auto& body) { return body.mode ==
                        Engine::Model::RigidBodyMotion::FollowTransform; }));
            report->importedDynamicRigidBodies = static_cast<int>(
                std::count_if(model.rigidBodies.begin(), model.rigidBodies.end(),
                    [](const auto& body) { return body.mode !=
                        Engine::Model::RigidBodyMotion::FollowTransform; }));
            report->importedPhysicsJoints =
                static_cast<int>(model.physicsJoints.size());
            report->importedSoftBodies =
                static_cast<int>(model.softBodies.size());
            if (report->importedPhysicsJoints)
                report->warnings.push_back("PMX joints use Bullet constraints; "
                    "MMD motion parity remains unverified");
            if (!options.dynamicPhysics)
                warn(dynamicRigidCount, "gravity-driven rigid bodies");
            else if (dynamicRigidCount && unsupportedJointCount)
                report->warnings.push_back("Some dynamic PMX bodies lack "
                    "supported joint constraints; motion can differ from MMD");
            if (!options.dynamicPhysics)
                warn(jointCount, "physics joints (dynamic physics disabled)");
            else
                warn(unsupportedJointCount, "unsupported physics joints");
            warn(unavailableSoftBodies,
                "soft bodies (dynamic physics disabled or empty material)");
            warn(unavailableSoftAttachments,
                "soft-body anchors or pins outside imported rigid bodies/material surfaces");
            if (!model.softBodies.empty())
                report->warnings.push_back("PMX soft bodies use Bullet mesh or "
                    "rope links with two-way rigid-body anchors; near-anchor "
                    "semantics and MMD motion parity remain unverified");
            if (std::any_of(model.softBodies.begin(), model.softBodies.end(),
                [](const auto& soft) { return soft.shape ==
                    Engine::Model::SoftBodyTopology::Rope && soft.clusterCount > 0; }))
                report->warnings.push_back("PMX rope cluster settings are not "
                    "applied; Bullet rope simulation uses links only");
            if (toonMaterials)
                report->warnings.push_back(std::to_string(toonMaterials) +
                    " toon materials use a PBR lighting approximation");
            if (outlineMaterials)
                report->warnings.push_back(std::to_string(outlineMaterials) +
                    " outlined materials use approximate mesh-normal extrusion");
            warn(fixedAxisBones,
                "fixed-axis bones (rotation projection approximates MMD behavior)");
            warn(localAxisBones,
                "local-axis bones (authored basis applied to animation; "
                "MMD parity remains unverified)");
            warn(afterPhysicsBones,
                "after-physics bones (append/IK/external-parent constraints run "
                "after physics; full MMD deform timing is approximate)");
            if (externalParentBones)
                report->warnings.push_back(std::to_string(externalParentBones) +
                    " external-parent bones imported; assign target objects "
                    "by PMX key in rig Properties");
            if (inheritedBones || ikChains)
                report->warnings.push_back("PMX append/IK constraints use a "
                    "layer/file-order CCD pass; exact MMD deform timing remains unavailable; "
                    "matching VMD IK toggles can be applied after motion binding");
        }

        // Topological node order is required by ModelAssetWriter. Palette order
        // remains the original PMX bone order for vertex influences.
        std::vector<int> nodeForBone(boneCount, -1), visits(boneCount, 0);
        std::function<void(int)> addBone = [&](int index)
        {
            if (visits[index] == 2) return;
            if (visits[index] == 1) throw std::runtime_error("Cyclic PMX bone hierarchy");
            visits[index] = 1;
            const int parent = bones[index].parent;
            if (parent >= 0) addBone(parent);
            nodeForBone[index] = static_cast<int>(model.nodes.size());
            auto& node = model.nodes.emplace_back();
            node.name = bones[index].name;
            node.parent = parent < 0 ? -1 : nodeForBone[parent];
            node.translation = bones[index].position -
                (parent < 0 ? glm::vec3(0.f) : bones[parent].position);
            visits[index] = 2;
        };
        for (int i = 0; i < boneCount; ++i) addBone(i);
        for (int i = 0; i < boneCount; ++i)
            if ((bones[i].flags & 0x0400) &&
                glm::dot(bones[i].fixedAxis, bones[i].fixedAxis) > 1e-12f)
                model.fixedAxes.push_back({
                    static_cast<unsigned>(nodeForBone[i]),
                    glm::normalize(bones[i].fixedAxis) });
        for (int i = 0; i < boneCount; ++i)
            if ((bones[i].flags & 0x0800) &&
                glm::dot(bones[i].localAxisX, bones[i].localAxisX) > 1e-12f &&
                glm::dot(glm::cross(bones[i].localAxisX,
                    bones[i].localAxisZ),
                    glm::cross(bones[i].localAxisX,
                        bones[i].localAxisZ)) > 1e-12f)
                model.localAxes.push_back({
                    static_cast<unsigned>(nodeForBone[i]),
                    bones[i].localAxisX, bones[i].localAxisZ,
                    model.nodes[nodeForBone[i]].translation });
        for (int i = 0; i < boneCount; ++i)
            if (bones[i].flags & 0x2000)
                model.externalParents.push_back({
                    static_cast<unsigned>(nodeForBone[i]),
                    bones[i].externalParentKey, bones[i].deformLayer, i,
                    (bones[i].flags & 0x1000) != 0, {} });
        for (const auto& morph : morphs)
            if ((morph.type == 2 || morph.type == 0 || morph.type == 9) &&
                morph.supported && !morph.boneOffsets.empty())
            {
                auto& imported = model.boneMorphs.emplace_back();
                imported.name = morph.name;
                imported.offsets = morph.boneOffsets;
                for (auto& offset : imported.offsets)
                    offset.nodeIndex = static_cast<unsigned>(
                        nodeForBone[offset.nodeIndex]);
            }
        for (const auto& morph : morphs)
            if ((morph.type == 8 || morph.type == 0 || morph.type == 9) &&
                morph.supported && !morph.materialOffsets.empty())
            {
                auto& imported = model.materialMorphs.emplace_back();
                imported.name = morph.name;
                imported.offsets = morph.materialOffsets;
            }
        for (int i = 0; i < boneCount; ++i)
        {
            const Bone& bone = bones[i];
            if (bone.appendSource >= 0)
            {
                auto& rule = model.appendRules.emplace_back();
                rule.nodeIndex = static_cast<unsigned>(nodeForBone[i]);
                rule.sourceNodeIndex = static_cast<unsigned>(
                    nodeForBone[bone.appendSource]);
                const int sourceParent = bones[bone.appendSource].parent;
                rule.sourceRestTranslation = bones[bone.appendSource].position -
                    (sourceParent < 0 ? glm::vec3(0.f) : bones[sourceParent].position);
                rule.influence = bone.appendRatio;
                rule.deformLayer = bone.deformLayer;
                rule.deformOrder = i;
                rule.afterPhysics = (bone.flags & 0x1000) != 0;
                rule.rotation = (bone.flags & 0x0100) != 0;
                rule.translation = (bone.flags & 0x0200) != 0;
            }
            if (bone.flags & 0x0020)
            {
                auto chain = bone.ik;
                chain.controllerNodeIndex = static_cast<unsigned>(nodeForBone[i]);
                chain.effectorNodeIndex = static_cast<unsigned>(
                    nodeForBone[chain.effectorNodeIndex]);
                for (auto& link : chain.links)
                {
                    const auto& linkBone = bones[link.nodeIndex];
                    link.fixedAxis = (linkBone.flags & 0x0400) != 0 &&
                        glm::dot(linkBone.fixedAxis, linkBone.fixedAxis) > 1e-12f;
                    if (link.fixedAxis)
                        link.axis = glm::normalize(linkBone.fixedAxis);
                    link.nodeIndex = static_cast<unsigned>(nodeForBone[link.nodeIndex]);
                }
                model.ikChains.push_back(std::move(chain));
            }
        }
        std::stable_sort(model.appendRules.begin(), model.appendRules.end(),
            [](const auto& a, const auto& b)
            { return a.deformLayer < b.deformLayer; });
        std::stable_sort(model.ikChains.begin(), model.ikChains.end(),
            [](const auto& a, const auto& b)
            { return a.deformLayer < b.deformLayer; });
        if (report)
        {
            report->importedAppendRules = static_cast<int>(model.appendRules.size());
            report->importedIkChains = static_cast<int>(model.ikChains.size());
            report->importedBoneMorphs = static_cast<int>(model.boneMorphs.size());
            report->importedMaterialMorphs =
                static_cast<int>(model.materialMorphs.size());
        }
        for (auto& body : model.rigidBodies)
        {
            if (body.parentNode >= 0)
                body.parentNode = nodeForBone[body.parentNode];
            if (body.relatedBoneNode >= 0)
                body.relatedBoneNode = nodeForBone[body.relatedBoneNode];
        }

        if (boneCount)
        {
            auto& skin = model.skins.emplace_back();
            skin.name = "PMX skeleton";
            for (int i = 0; i < boneCount; ++i)
            {
                skin.jointNodes.push_back(static_cast<unsigned>(nodeForBone[i]));
                skin.inverseBindMatrices.push_back(glm::translate(glm::mat4(1.f),
                    -bones[i].position));
            }
        }
        auto& meshNode = model.nodes.emplace_back();
        meshNode.name = "Mesh";
        for (int i = 0; i < materialCount; ++i)
        {
            const auto& range = ranges[i];
            if (!range.count) continue;
            const unsigned primitiveIndex = static_cast<unsigned>(model.primitives.size());
            auto& primitive = model.primitives.emplace_back();
            primitive.name = model.materials[i].name;
            primitive.materialIndex = i;
            primitive.skinIndex = boneCount ? 0 : -1;
            primitive.vertices.reserve(range.count);
            for (int j = range.begin; j < range.begin + range.count; j += 3)
            {
                // Reflection across Z reverses winding.
                primitive.vertices.push_back(vertices[indices[j]]);
                primitive.vertices.push_back(vertices[indices[j + 2]]);
                primitive.vertices.push_back(vertices[indices[j + 1]]);
            }
            if (options.morphs)
            {
            // Link source vertices to the slots in this material's expanded mesh.
            // A morph usually touches few vertices, so visit those slots instead
            // of scanning every triangle once for every morph.
            std::vector<int> firstSlot(vertexCount, -1);
            std::vector<int> nextSlot(range.count, -1);
            for (int slot = 0; slot < range.count; ++slot)
            {
                const int triangleSlot = slot % 3;
                const int source = indices[range.begin + slot - triangleSlot +
                    (triangleSlot == 1 ? 2 : triangleSlot == 2 ? 1 : 0)];
                nextSlot[slot] = firstSlot[source];
                firstSlot[source] = slot;
            }
            for (const auto& morph : morphs)
            {
                if (!morph.supported || morph.offsets.empty()) continue;
                Engine::Model::MorphTarget target;
                target.name = morph.name;
                for (const auto& [source, offset] : morph.offsets)
                {
                    if (firstSlot[source] < 0) continue;
                    if (glm::any(glm::notEqual(offset.position, glm::vec3(0.f))) &&
                        target.positions.empty())
                        target.positions.assign(range.count, glm::vec3(0.f));
                    if (glm::any(glm::notEqual(offset.uv, glm::vec4(0.f))) &&
                        target.uv.empty())
                        target.uv.assign(range.count, glm::vec4(0.f));
                    for (size_t channel = 0; channel < target.uvChannels.size(); ++channel)
                        if (glm::any(glm::notEqual(offset.uvChannels[channel],
                                glm::vec4(0.f))) && target.uvChannels[channel].empty())
                            target.uvChannels[channel].assign(range.count, glm::vec4(0.f));
                    for (int slot = firstSlot[source]; slot >= 0; slot = nextSlot[slot])
                    {
                        if (!target.positions.empty())
                            target.positions[slot] = offset.position;
                        if (!target.uv.empty())
                            target.uv[slot] = offset.uv;
                        for (size_t channel = 0; channel < target.uvChannels.size(); ++channel)
                            if (!target.uvChannels[channel].empty())
                                target.uvChannels[channel][slot] = offset.uvChannels[channel];
                    }
                }
                if (!target.positions.empty() || !target.uv.empty() ||
                    std::any_of(target.uvChannels.begin(), target.uvChannels.end(),
                        [](const auto& channel) { return !channel.empty(); }))
                {
                    primitive.morphTargets.push_back(std::move(target));
                    primitive.morphWeights.push_back(0.f);
                }
            }
            }
            meshNode.primitives.push_back(primitiveIndex);
        }
        if (materialCount == 0 && indexCount != 0)
            throw std::runtime_error("PMX surfaces have no material");
        if (!options.morphs)
        {
            for (auto& primitive : model.primitives)
            {
                primitive.morphTargets.clear();
                primitive.morphWeights.clear();
            }
            model.boneMorphs.clear();
            model.materialMorphs.clear();
            if (report)
            {
                report->importedSupportedGroupMorphs = 0;
                report->importedBoneMorphs = 0;
                report->importedMaterialMorphs = 0;
                auto& warnings = report->warnings;
                warnings.erase(std::remove_if(warnings.begin(), warnings.end(),
                    [](const std::string& warning)
                    {
                        return warning.rfind("Bone morph sliders import", 0) == 0 ||
                            warning.rfind("Material morph sliders import", 0) == 0 ||
                            warning.rfind("Material morph specular/shininess render", 0) == 0;
                    }), warnings.end());
                report->warnings.push_back("Morph import disabled by import option");
            }
        }
        if (!options.rigConstraints)
        {
            model.appendRules.clear();
            model.ikChains.clear();
            model.fixedAxes.clear();
            model.localAxes.clear();
            model.externalParents.clear();
            if (report)
            {
                report->importedAppendRules = 0;
                report->importedIkChains = 0;
                auto& warnings = report->warnings;
                warnings.erase(std::remove_if(warnings.begin(), warnings.end(),
                    [](const std::string& warning)
                    { return warning.rfind("PMX append/IK constraints use", 0) == 0; }),
                    warnings.end());
                report->warnings.push_back("Append and IK import disabled by import option");
            }
        }
        if (!options.followBoneColliders)
        {
            model.rigidBodies.erase(std::remove_if(model.rigidBodies.begin(),
                model.rigidBodies.end(), [](const auto& body)
                { return body.mode == Engine::Model::RigidBodyMotion::FollowTransform; }),
                model.rigidBodies.end());
            if (report)
            {
                report->importedKinematicRigidBodies = 0;
                report->warnings.push_back("Follow-bone collider import disabled by import option");
            }
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        model = {};
        error = exception.what();
        return false;
    }
}
}
