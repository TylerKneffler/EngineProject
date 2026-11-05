#include "Engine/Editor/Core/View/Views/Scene/SurfacePlacement.h"
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <stdexcept>

using namespace Engine::Editor;
static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

int main()
{
    try
    {
        double distance = 100.0;
        glm::dvec3 normal;
        const glm::dvec3 a(0, 0, 0), b(2, 2, 0), c(0, 0, 2);
        const glm::dvec3 origin(.5, 10, .5), down(0, -1, 0);
        Require(IntersectPlacementTriangle(origin, down, a, b, c, distance, normal), "slope missed");
        Require(std::abs(distance - 9.5) < 1.e-9, "height is not on triangle");
        Require(std::abs(glm::dot(normal, b - a)) < 1.e-9, "normal not perpendicular");
        Require(!IntersectPlacementTriangle(origin, down, a, b, c, distance, normal), "farther hit replaced nearest");
        distance = 100.0;
        Require(!IntersectPlacementTriangle({3, 10, 3}, down, a, b, c, distance, normal), "outside triangle accepted");
        Require(!IntersectPlacementTriangle(origin, {1, 1, 0}, a, b, c, distance, normal), "parallel ray accepted");
        Require(!IntersectPlacementTriangle(origin, down, a, a, c, distance, normal), "degenerate triangle accepted");
        Require(!IntersectPlacementTriangle(origin, -down, a, b, c, distance, normal), "hit behind ray accepted");

        const auto world = glm::translate(glm::dmat4(1), glm::dvec3(5, 3, -2)) *
            glm::rotate(glm::dmat4(1), .4, glm::dvec3(0, 0, 1)) *
            glm::scale(glm::dmat4(1), glm::dvec3(2, 3, .5));
        const auto point = [&](glm::dvec3 p) { return glm::dvec3(world * glm::dvec4(p, 1)); };
        const auto expected = point({.5, .5, .5});
        distance = 100.0;
        Require(IntersectPlacementTriangle(expected + glm::dvec3(0, 10, 0), down,
            point(a), point(b), point(c), distance, normal), "transformed terrain missed");
        Require(std::abs(distance - 10.0) < 1.e-9, "transformed height wrong");
        if (normal.y < 0) normal = -normal;
        const auto yaw = glm::angleAxis(.7f, glm::vec3(0, 1, 0));
        const auto rotation = SurfacePlacementRotation(glm::vec3(normal), yaw);
        Require(glm::length(rotation * glm::vec3(0, 1, 0) - glm::vec3(normal)) < 1.e-5f,
            "painted up axis does not match slope");
        Require(glm::length(SurfacePlacementRotation({0, 1, 0}, yaw) * glm::vec3(1, 0, 0) -
            yaw * glm::vec3(1, 0, 0)) < 1.e-5f, "flat terrain changed authored yaw");
        std::cout << "Surface placement tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
