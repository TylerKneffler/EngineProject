#pragma once

#include <string>
#include <string_view>

namespace Engine::Math
{
struct FormulaVariables
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double a = 0.0;
    double b = 0.0;
    double c = 0.0;
    double d = 0.0;
};

// Evaluates a bounded, side-effect-free scalar expression. Supported
// variables: x, y, z, a-d, r, rho, theta, phi, pi, e.
bool EvaluateFormula(std::string_view expression,
    const FormulaVariables& variables, double& result,
    std::string* error = nullptr);
}
