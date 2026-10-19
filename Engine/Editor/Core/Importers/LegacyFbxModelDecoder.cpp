#include "LegacyFbxModelDecoder.h"

#include <ufbx.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <glm/gtc/matrix_inverse.hpp>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Engine::Editor
{
namespace
{
namespace fs = std::filesystem;

std::string String(ufbx_string value)
{
    return value.data ? std::string(value.data, value.length) : std::string{};
}

glm::vec3 Vec3(ufbx_vec3 value)
{
    return { static_cast<float>(value.x), static_cast<float>(value.y),
        static_cast<float>(value.z) };
}

glm::mat4 Matrix(ufbx_matrix value)
{
    glm::mat4 result(1.f);
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 3; ++row)
            result[column][row] = static_cast<float>(value.cols[column].v[row]);
    return result;
}

std::string TexturePath(const fs::path& source, const ufbx_texture* texture,
    Engine::Model::ImportedModel& model)
{
    if (!texture || texture->type != UFBX_TEXTURE_FILE) return {};
    if (texture->content.size)
    {
        const std::string key = "*legacy-" + std::to_string(texture->typed_id);
        Engine::Model::ImportedTexture embedded;
        embedded.source = key;
        embedded.name = fs::path(String(texture->filename)).filename().string();
        if (embedded.name.empty()) embedded.name = "Embedded.png";
        const auto* first = static_cast<const std::byte*>(texture->content.data);
        embedded.bytes.assign(first, first + texture->content.size);
        model.textures.push_back(std::move(embedded));
        return key;
    }

    // Older FBX files commonly retain the exporter's absolute texture path.
    // Resolve existing paths first, then look for the same image in the model
    // folder or its sibling textures folder.
    const fs::path authored(String(texture->filename));
    const fs::path relative(String(texture->relative_filename));
    std::vector<fs::path> candidates;
    if (!authored.empty()) candidates.push_back(authored);
    if (!relative.empty()) candidates.push_back(source.parent_path() / relative);
    const fs::path filename = !relative.empty() ? relative.filename() : authored.filename();
    if (!filename.empty())
        for (const fs::path& directory : { source.parent_path(),
                source.parent_path().parent_path() / "textures" })
        {
            candidates.push_back(directory / filename);
            candidates.push_back(directory / (filename.string() + ".png"));
        }
    for (const fs::path& candidate : candidates)
        if (fs::is_regular_file(candidate))
        {
            if (candidate.is_absolute())
                return candidate.lexically_normal().generic_string();
            return candidate.lexically_normal().lexically_relative(
                source.parent_path().lexically_normal()).generic_string();
        }
    return {};
}

void StoreWeights(Engine::Model::AnimationVertex& vertex,
    const ufbx_skin_deformer* skin, uint32_t vertexIndex,
    const std::vector<int>& clusterToPalette)
{
    if (!skin || vertexIndex >= skin->vertices.count) return;
    const ufbx_skin_vertex weights = skin->vertices.data[vertexIndex];
    float total = 0.f;
    size_t used = 0;
    for (size_t i = 0; i < weights.num_weights && used < 8; ++i)
    {
        const ufbx_skin_weight weight = skin->weights.data[weights.weight_begin + i];
        if (weight.cluster_index >= clusterToPalette.size() ||
            clusterToPalette[weight.cluster_index] < 0 || weight.weight <= 0.0)
            continue;
        float* joints = used < 4 ? vertex.joints0 : vertex.joints1;
        float* values = used < 4 ? vertex.weights0 : vertex.weights1;
        joints[used & 3u] = static_cast<float>(clusterToPalette[weight.cluster_index]);
        values[used & 3u] = static_cast<float>(weight.weight);
        total += values[used & 3u];
        ++used;
    }
    if (total > 0.f)
        for (size_t i = 0; i < used; ++i)
        {
            float* values = i < 4 ? vertex.weights0 : vertex.weights1;
            values[i & 3u] /= total;
        }
}
}

bool LegacyFbxModelDecoder::Decode(const std::string& sourcePath,
    Engine::Model::ImportedModel& model, std::string& error)
{
    ufbx_load_opts options{};
    options.generate_missing_normals = true;
    options.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;
    ufbx_error loadError{};
    std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)> scene(
        ufbx_load_file(sourcePath.c_str(), &options, &loadError), ufbx_free_scene);
    if (!scene)
    {
        char message[1024]{};
        ufbx_format_error(message, sizeof(message), &loadError);
        error = std::string("Could not decode legacy FBX: ") + message;
        return false;
    }
    if (scene->anim_stacks.count || scene->blend_deformers.count)
    {
        error = "Legacy FBX animation and blend shapes are not supported";
        return false;
    }
    model = {};
    model.name = fs::path(sourcePath).stem().string();
    model.sourceFormat = "fbx";
    model.unitScaleToMeters = static_cast<float>(scene->settings.unit_meters);
    if (!std::isfinite(model.unitScaleToMeters) || model.unitScaleToMeters <= 0.f)
    {
        error = "Legacy FBX has invalid source units";
        return false;
    }

    std::unordered_map<const ufbx_node*, unsigned> nodeIndices;
    for (size_t i = 0; i < scene->nodes.count; ++i)
    {
        const ufbx_node* source = scene->nodes.data[i];
        Engine::Model::ImportedNode node;
        node.name = String(source->name);
        if (node.name.empty()) node.name = "Node " + std::to_string(i + 1);
        if (source->parent)
        {
            const auto parent = nodeIndices.find(source->parent);
            if (parent == nodeIndices.end())
            {
                error = "Legacy FBX node hierarchy is out of order";
                return false;
            }
            node.parent = static_cast<int>(parent->second);
        }
        node.translation = Vec3(source->local_transform.translation);
        const ufbx_quat rotation = source->local_transform.rotation;
        node.rotation = glm::quat(static_cast<float>(rotation.w),
            static_cast<float>(rotation.x), static_cast<float>(rotation.y),
            static_cast<float>(rotation.z));
        node.scale = Vec3(source->local_transform.scale);
        nodeIndices.emplace(source, static_cast<unsigned>(model.nodes.size()));
        model.nodes.push_back(std::move(node));
    }

    std::unordered_map<const ufbx_material*, int> materialIndices;
    for (size_t i = 0; i < scene->materials.count; ++i)
    {
        const ufbx_material* source = scene->materials.data[i];
        Engine::Model::ImportedMaterial material;
        material.name = String(source->name);
        if (material.name.empty()) material.name = "Material " + std::to_string(i + 1);
        const auto& color = source->pbr.base_color;
        if (color.has_value)
            material.baseColor = { static_cast<float>(color.value_vec3.x),
                static_cast<float>(color.value_vec3.y),
                static_cast<float>(color.value_vec3.z), 1.f };
        material.baseColorTexture = TexturePath(fs::path(sourcePath), color.texture, model);
        material.normalTexture = TexturePath(fs::path(sourcePath),
            source->pbr.normal_map.texture, model);
        material.emissiveTexture = TexturePath(fs::path(sourcePath),
            source->pbr.emission_color.texture, model);
        materialIndices.emplace(source, static_cast<int>(model.materials.size()));
        model.materials.push_back(std::move(material));
    }

    for (size_t i = 0; i < scene->nodes.count; ++i)
    {
        const ufbx_node* sourceNode = scene->nodes.data[i];
        const ufbx_mesh* mesh = sourceNode->mesh;
        if (!mesh || mesh->num_triangles == 0) continue;
        if (mesh->skin_deformers.count > 1)
        {
            error = "Legacy FBX meshes with multiple skin deformers are not supported";
            return false;
        }
        if (mesh->max_face_triangles > (1u << 20))
        {
            error = "Legacy FBX polygon is too large to triangulate";
            return false;
        }
        const ufbx_skin_deformer* skin = mesh->skin_deformers.count
            ? mesh->skin_deformers.data[0] : nullptr;
        std::vector<int> clusterToPalette;
        int skinIndex = -1;
        if (skin)
        {
            Engine::Model::ImportedSkin imported;
            imported.name = String(skin->name);
            if (imported.name.empty()) imported.name = String(mesh->name) + " Skeleton";
            clusterToPalette.assign(skin->clusters.count, -1);
            for (size_t c = 0; c < skin->clusters.count; ++c)
            {
                const ufbx_skin_cluster* cluster = skin->clusters.data[c];
                const auto bone = nodeIndices.find(cluster->bone_node);
                if (bone == nodeIndices.end()) continue;
                clusterToPalette[c] = static_cast<int>(imported.jointNodes.size());
                imported.jointNodes.push_back(bone->second);
                // The runtime uses model-space inverse binds and a separate
                // bindMeshToModel transform, as the modern FBX importer does.
                const glm::mat4 bind = Matrix(cluster->bind_to_world);
                if (!std::isfinite(glm::determinant(bind)) ||
                    std::abs(glm::determinant(bind)) < 1e-8f)
                {
                    error = "Legacy FBX joint has a non-invertible bind transform";
                    return false;
                }
                imported.inverseBindMatrices.push_back(glm::inverse(bind));
            }
            if (!imported.jointNodes.empty())
            {
                skinIndex = static_cast<int>(model.skins.size());
                model.skins.push_back(std::move(imported));
            }
        }

        const glm::mat4 bindMesh = Matrix(sourceNode->node_to_world);
        for (size_t partIndex = 0; partIndex < mesh->material_parts.count; ++partIndex)
        {
            const ufbx_mesh_part& part = mesh->material_parts.data[partIndex];
            if (!part.num_triangles) continue;
            Engine::Model::ImportedPrimitive primitive;
            primitive.name = String(mesh->name);
            if (primitive.name.empty()) primitive.name = "Mesh " + std::to_string(i + 1);
            if (mesh->material_parts.count > 1)
                primitive.name += " " + std::to_string(partIndex + 1);
            primitive.skinIndex = skinIndex;
            primitive.bindMeshToModel = bindMesh;
            if (part.index < sourceNode->materials.count)
            {
                const auto material = materialIndices.find(sourceNode->materials.data[part.index]);
                if (material != materialIndices.end()) primitive.materialIndex = material->second;
            }
            primitive.vertices.reserve(part.num_triangles * 3);
            std::vector<uint32_t> triangleIndices(mesh->max_face_triangles * 3);
            for (size_t f = 0; f < part.num_faces; ++f)
            {
                const ufbx_face face = mesh->faces.data[part.face_indices.data[f]];
                if (face.num_indices < 3) continue;
                const uint32_t triangles = ufbx_triangulate_face(triangleIndices.data(),
                    triangleIndices.size(), mesh, face);
                for (size_t t = 0; t < static_cast<size_t>(triangles) * 3; ++t)
                {
                    const uint32_t index = triangleIndices[t];
                    Engine::Model::AnimationVertex vertex{};
                    const ufbx_vec3 position = ufbx_get_vertex_vec3(&mesh->vertex_position, index);
                    vertex.pos[0] = static_cast<float>(position.x);
                    vertex.pos[1] = static_cast<float>(position.y);
                    vertex.pos[2] = static_cast<float>(position.z);
                    if (mesh->vertex_normal.exists)
                    {
                        const ufbx_vec3 normal = ufbx_get_vertex_vec3(&mesh->vertex_normal, index);
                        vertex.normal[0] = static_cast<float>(normal.x);
                        vertex.normal[1] = static_cast<float>(normal.y);
                        vertex.normal[2] = static_cast<float>(normal.z);
                    }
                    if (mesh->vertex_uv.exists)
                    {
                        const ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, index);
                        vertex.uv[0] = static_cast<float>(uv.x);
                        vertex.uv[1] = 1.f - static_cast<float>(uv.y);
                    }
                    if (mesh->uv_sets.count > 1 && mesh->uv_sets.data[1].vertex_uv.exists)
                    {
                        const ufbx_vec2 uv = ufbx_get_vertex_vec2(
                            &mesh->uv_sets.data[1].vertex_uv, index);
                        vertex.uv1[0] = static_cast<float>(uv.x);
                        vertex.uv1[1] = 1.f - static_cast<float>(uv.y);
                    }
                    if (mesh->vertex_color.exists)
                    {
                        const ufbx_vec4 color = ufbx_get_vertex_vec4(&mesh->vertex_color, index);
                        vertex.color[0] = static_cast<float>(color.x);
                        vertex.color[1] = static_cast<float>(color.y);
                        vertex.color[2] = static_cast<float>(color.z);
                        vertex.color[3] = static_cast<float>(color.w);
                    }
                    if (mesh->vertex_tangent.exists)
                    {
                        const ufbx_vec3 tangent = ufbx_get_vertex_vec3(&mesh->vertex_tangent, index);
                        vertex.tangent[0] = static_cast<float>(tangent.x);
                        vertex.tangent[1] = static_cast<float>(tangent.y);
                        vertex.tangent[2] = static_cast<float>(tangent.z);
                        vertex.tangent[3] = 1.f;
                        if (mesh->vertex_normal.exists && mesh->vertex_bitangent.exists)
                        {
                            const ufbx_vec3 bitangent = ufbx_get_vertex_vec3(
                                &mesh->vertex_bitangent, index);
                            const glm::vec3 normal(vertex.normal[0], vertex.normal[1],
                                vertex.normal[2]);
                            vertex.tangent[3] = glm::dot(glm::cross(normal, Vec3(tangent)),
                                Vec3(bitangent)) < 0.f ? -1.f : 1.f;
                        }
                        if (mesh->vertex_uv.exists) vertex.tangent[3] *= -1.f;
                    }
                    if (skinIndex >= 0)
                        StoreWeights(vertex, skin, mesh->vertex_indices.data[index],
                            clusterToPalette);
                    primitive.vertices.push_back(vertex);
                }
            }
            if (!primitive.vertices.empty())
            {
                model.nodes[i].primitives.push_back(static_cast<unsigned>(model.primitives.size()));
                model.primitives.push_back(std::move(primitive));
            }
        }
    }
    if (model.primitives.empty())
    {
        error = "Legacy FBX contains no polygon mesh";
        return false;
    }
    return true;
}
}
