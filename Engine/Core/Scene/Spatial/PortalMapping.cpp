#include "PortalMapping.h"

#include "PortalAperture.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace Engine::Scene::Spatial
{
namespace
{
glm::vec3 SafeNormalize(const glm::vec3& value, const glm::vec3& fallback)
{
    const float lengthSquared = glm::dot(value, value);
    return lengthSquared <= 1e-8f ? fallback : value / std::sqrt(lengthSquared);
}
}

PortalMapping::PortalMapping(
    const std::vector<glm::vec3>& sourceWorldPoints,
    const std::vector<glm::vec3>& targetWorldPoints,
    const glm::mat4& sourceFrame, const glm::mat4& targetFrame)
    : m_sourceFrame(sourceFrame), m_targetFrame(targetFrame)
{
    m_scaleRatio = ComputeScaleRatio(sourceWorldPoints, targetWorldPoints);
    m_affineTransform = BuildAffineTransform(sourceWorldPoints,
        targetWorldPoints, sourceFrame, targetFrame);

    if (sourceWorldPoints.size() < 3u ||
        sourceWorldPoints.size() != targetWorldPoints.size())
    {
        return;
    }
    const glm::mat4 sourceInverse = glm::inverse(sourceFrame);
    const glm::mat4 targetInverse = glm::inverse(targetFrame);
    std::vector<glm::vec2> targetAuthored;
    m_source.reserve(sourceWorldPoints.size());
    targetAuthored.reserve(targetWorldPoints.size());
    for (const glm::vec3& point : sourceWorldPoints)
    {
        const glm::vec3 local(sourceInverse * glm::vec4(point, 1.f));
        m_source.emplace_back(local.x, local.y);
    }
    for (const glm::vec3& point : targetWorldPoints)
    {
        const glm::vec3 local(targetInverse * glm::vec4(point, 1.f));
        targetAuthored.emplace_back(local.x, local.y);
    }

    size_t bestOffset = 0u;
    float bestError = std::numeric_limits<float>::max();
    for (size_t offset = 0; offset < targetAuthored.size(); ++offset)
    {
        float error = 0.f;
        for (size_t index = 0; index < m_source.size(); ++index)
        {
            const glm::vec3 predictedWorld(m_affineTransform *
                glm::vec4(sourceWorldPoints[index], 1.f));
            const glm::vec3 predictedLocal(targetInverse *
                glm::vec4(predictedWorld, 1.f));
            const size_t targetIndex = (offset + targetAuthored.size() - index) %
                targetAuthored.size();
            const glm::vec2 delta = glm::vec2(predictedLocal) -
                targetAuthored[targetIndex];
            error += glm::dot(delta, delta);
        }
        if (error < bestError)
        {
            bestError = error;
            bestOffset = offset;
        }
    }

    m_target.resize(targetAuthored.size());
    float extent = 0.f;
    float maximumError = 0.f;
    for (size_t index = 0; index < m_source.size(); ++index)
    {
        const size_t targetIndex = (bestOffset + targetAuthored.size() - index) %
            targetAuthored.size();
        m_target[index] = targetAuthored[targetIndex];
        m_sourceCenter += m_source[index];
        m_targetCenter += m_target[index];
        const glm::vec3 predictedWorld(m_affineTransform *
            glm::vec4(sourceWorldPoints[index], 1.f));
        maximumError = std::max(maximumError,
            glm::length(predictedWorld - targetWorldPoints[targetIndex]));
        extent = std::max(extent, glm::length(
            targetWorldPoints[(targetIndex + 1u) % targetWorldPoints.size()] -
            targetWorldPoints[targetIndex]));
    }
    m_sourceCenter /= static_cast<float>(m_source.size());
    m_targetCenter /= static_cast<float>(m_target.size());
    m_valid = true;
    m_piecewise = maximumError > std::max(0.0005f, extent * 0.0005f);
}

float PortalMapping::ComputeScaleRatio(
    const std::vector<glm::vec3>& sourceWorldPoints,
    const std::vector<glm::vec3>& targetWorldPoints)
{
    const float sourceArea = PortalAperture::Area(sourceWorldPoints);
    const float targetArea = PortalAperture::Area(targetWorldPoints);
    if (!std::isfinite(sourceArea) || !std::isfinite(targetArea) ||
        sourceArea <= 1e-8f || targetArea <= 1e-8f)
    {
        return 1.f;
    }
    return std::sqrt(targetArea / sourceArea);
}

glm::mat4 PortalMapping::BuildAffineTransform(
    const std::vector<glm::vec3>& sourceWorldPoints,
    const std::vector<glm::vec3>& targetWorldPoints,
    const glm::mat4& sourceFrame, const glm::mat4& targetFrame)
{
    const float scaleRatio = ComputeScaleRatio(sourceWorldPoints,
        targetWorldPoints);
    glm::mat4 crossing(1.f);
    crossing[0][0] = -scaleRatio;
    crossing[1][1] = scaleRatio;
    crossing[2][2] = -scaleRatio;
    return targetFrame * crossing * glm::inverse(sourceFrame);
}

bool PortalMapping::IsValid() const { return m_valid; }
bool PortalMapping::IsPiecewise() const { return m_valid && m_piecewise; }
float PortalMapping::ScaleRatio() const { return m_scaleRatio; }
const glm::mat4& PortalMapping::AffineTransform() const
{
    return m_affineTransform;
}

glm::vec3 PortalMapping::MapPoint(const glm::vec3& worldPoint) const
{
    if (!m_valid || !m_piecewise)
        return glm::vec3(m_affineTransform * glm::vec4(worldPoint, 1.f));
    const glm::vec3 sourceLocal(glm::inverse(m_sourceFrame) *
        glm::vec4(worldPoint, 1.f));
    const glm::vec2 point(sourceLocal.x, sourceLocal.y);
    size_t selected = 0u;
    glm::vec3 selectedWeights(1.f, 0.f, 0.f);
    float bestWedgeScore = -std::numeric_limits<float>::max();
    for (size_t index = 0; index < m_source.size(); ++index)
    {
        const size_t next = (index + 1u) % m_source.size();
        const glm::vec2 a = m_sourceCenter;
        const glm::vec2 b = m_source[index];
        const glm::vec2 c = m_source[next];
        const float denominator = (b.y - c.y) * (a.x - c.x) +
            (c.x - b.x) * (a.y - c.y);
        if (std::abs(denominator) <= 1e-8f)
            continue;
        const float wa = ((b.y - c.y) * (point.x - c.x) +
            (c.x - b.x) * (point.y - c.y)) / denominator;
        const float wb = ((c.y - a.y) * (point.x - c.x) +
            (a.x - c.x) * (point.y - c.y)) / denominator;
        const float wc = 1.f - wa - wb;
        const float wedgeScore = std::min(wb, wc);
        if (wedgeScore > bestWedgeScore)
        {
            bestWedgeScore = wedgeScore;
            selected = index;
            selectedWeights = glm::vec3(wa, wb, wc);
        }
    }
    const size_t next = (selected + 1u) % m_target.size();
    const glm::vec2 targetPoint = selectedWeights.x * m_targetCenter +
        selectedWeights.y * m_target[selected] +
        selectedWeights.z * m_target[next];
    const glm::vec2 sourceFirst = m_source[selected] - m_sourceCenter;
    const glm::vec2 sourceSecond = m_source[next] - m_sourceCenter;
    const glm::vec2 targetFirst = m_target[selected] - m_targetCenter;
    const glm::vec2 targetSecond = m_target[next] - m_targetCenter;
    const float sourceDeterminant = sourceFirst.x * sourceSecond.y -
        sourceFirst.y * sourceSecond.x;
    const float targetDeterminant = targetFirst.x * targetSecond.y -
        targetFirst.y * targetSecond.x;
    const float depthScale = std::abs(sourceDeterminant) > 1e-8f
        ? std::sqrt(std::abs(targetDeterminant / sourceDeterminant)) : 1.f;
    return glm::vec3(m_targetFrame * glm::vec4(targetPoint,
        -sourceLocal.z * depthScale, 1.f));
}

glm::vec3 PortalMapping::MapDirection(const glm::vec3& origin,
    const glm::vec3& direction) const
{
    const float length = glm::length(direction);
    if (length <= 1e-8f)
        return glm::vec3(0.f);
    const float sampleDistance = std::max(0.001f, length * 0.001f);
    return (MapPoint(origin + direction * (sampleDistance / length)) -
        MapPoint(origin)) * (length / sampleDistance);
}

glm::vec3 PortalMapping::MapNormal(const glm::vec3& origin,
    const glm::vec3& normal) const
{
    glm::mat3 jacobian(1.f);
    for (int column = 0; column < 3; ++column)
    {
        jacobian[column] = MapDirection(origin, glm::vec3(column == 0,
            column == 1, column == 2));
    }
    const float determinant = glm::determinant(jacobian);
    if (!std::isfinite(determinant) || std::abs(determinant) <= 1e-8f)
        return normal;
    return SafeNormalize(glm::transpose(glm::inverse(jacobian)) * normal,
        normal);
}
}
