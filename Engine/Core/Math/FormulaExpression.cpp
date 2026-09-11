#include "Core/Math/FormulaExpression.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <utility>
#include <vector>

namespace Engine::Math
{
namespace
{
class Parser
{
public:
    Parser(std::string_view source, const FormulaVariables& variables)
        : m_source(source), m_variables(variables) {}

    bool Parse(double& result)
    {
        if (m_source.empty() || m_source.size() > 1024)
            return Fail("formula is empty or too long");
        result = ParseExpression();
        SkipWhitespace();
        if (!m_ok || m_position != m_source.size())
            return Fail("unexpected token");
        if (!std::isfinite(result))
            return Fail("formula produced a non-finite value");
        return true;
    }

    const std::string& Error() const { return m_error; }

private:
    double ParseExpression()
    {
        DepthGuard guard(*this);
        double value = ParseTerm();
        while (m_ok)
        {
            SkipWhitespace();
            if (Consume('+')) value += ParseTerm();
            else if (Consume('-')) value -= ParseTerm();
            else break;
        }
        return value;
    }

    double ParseTerm()
    {
        double value = ParsePower();
        while (m_ok)
        {
            SkipWhitespace();
            if (Consume('*')) value *= ParsePower();
            else if (Consume('/')) value /= ParsePower();
            else if (Consume('%')) value = std::fmod(value, ParsePower());
            else break;
        }
        return value;
    }

    double ParsePower()
    {
        double value = ParseUnary();
        SkipWhitespace();
        if (Consume('^'))
            value = std::pow(value, ParsePower());
        return value;
    }

    double ParseUnary()
    {
        SkipWhitespace();
        if (Consume('+')) return ParseUnary();
        if (Consume('-')) return -ParseUnary();
        return ParsePrimary();
    }

    double ParsePrimary()
    {
        SkipWhitespace();
        if (Consume('('))
        {
            const double value = ParseExpression();
            SkipWhitespace();
            if (!Consume(')')) Fail("missing closing parenthesis");
            return value;
        }

        if (m_position < m_source.size() &&
            (std::isdigit(static_cast<unsigned char>(m_source[m_position])) ||
                m_source[m_position] == '.'))
            return ParseNumber();

        const std::string identifier = ParseIdentifier();
        if (identifier.empty())
        {
            Fail("expected a number, variable, or function");
            return 0.0;
        }

        SkipWhitespace();
        if (!Consume('('))
            return ResolveVariable(identifier);

        std::vector<double> arguments;
        SkipWhitespace();
        if (!Consume(')'))
        {
            while (m_ok)
            {
                arguments.push_back(ParseExpression());
                SkipWhitespace();
                if (Consume(')')) break;
                if (!Consume(','))
                {
                    Fail("expected comma or closing parenthesis");
                    break;
                }
            }
        }
        return Call(identifier, arguments);
    }

    double ParseNumber()
    {
        const std::string tail(m_source.substr(m_position));
        char* end = nullptr;
        const double value = std::strtod(tail.c_str(), &end);
        if (!end || end == tail.c_str())
        {
            Fail("invalid number");
            return 0.0;
        }
        m_position += static_cast<size_t>(end - tail.c_str());
        return value;
    }

    std::string ParseIdentifier()
    {
        SkipWhitespace();
        const size_t start = m_position;
        while (m_position < m_source.size())
        {
            const unsigned char value = static_cast<unsigned char>(m_source[m_position]);
            if (!std::isalnum(value) && value != '_') break;
            ++m_position;
        }
        std::string result(m_source.substr(start, m_position - start));
        std::transform(result.begin(), result.end(), result.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        return result;
    }

    double ResolveVariable(const std::string& name)
    {
        if (name == "x") return m_variables.x;
        if (name == "y") return m_variables.y;
        if (name == "z") return m_variables.z;
        if (name == "a") return m_variables.a;
        if (name == "b") return m_variables.b;
        if (name == "c") return m_variables.c;
        if (name == "d") return m_variables.d;
        if (name == "r") return std::sqrt(m_variables.x * m_variables.x +
            m_variables.y * m_variables.y + m_variables.z * m_variables.z);
        if (name == "rho") return std::sqrt(m_variables.x * m_variables.x +
            m_variables.z * m_variables.z);
        if (name == "theta") return std::atan2(m_variables.z, m_variables.x);
        if (name == "phi") return std::atan2(m_variables.y,
            std::sqrt(m_variables.x * m_variables.x + m_variables.z * m_variables.z));
        if (name == "pi") return 3.14159265358979323846;
        if (name == "e") return 2.71828182845904523536;
        Fail("unknown variable: " + name);
        return 0.0;
    }

    double Call(const std::string& name, const std::vector<double>& v)
    {
        const auto unary = [&](auto function) -> double
        {
            if (v.size() != 1) { Fail(name + " expects one argument"); return 0.0; }
            return function(v[0]);
        };
        const auto binary = [&](auto function) -> double
        {
            if (v.size() != 2) { Fail(name + " expects two arguments"); return 0.0; }
            return function(v[0], v[1]);
        };

        if (name == "sin") return unary([](double x) { return std::sin(x); });
        if (name == "cos") return unary([](double x) { return std::cos(x); });
        if (name == "tan") return unary([](double x) { return std::tan(x); });
        if (name == "asin") return unary([](double x) { return std::asin(x); });
        if (name == "acos") return unary([](double x) { return std::acos(x); });
        if (name == "atan") return unary([](double x) { return std::atan(x); });
        if (name == "sqrt") return unary([](double x) { return std::sqrt(x); });
        if (name == "abs") return unary([](double x) { return std::abs(x); });
        if (name == "exp") return unary([](double x) { return std::exp(x); });
        if (name == "log") return unary([](double x) { return std::log(x); });
        if (name == "log10") return unary([](double x) { return std::log10(x); });
        if (name == "floor") return unary([](double x) { return std::floor(x); });
        if (name == "ceil") return unary([](double x) { return std::ceil(x); });
        if (name == "round") return unary([](double x) { return std::round(x); });
        if (name == "sign") return unary([](double x) { return x < 0.0 ? -1.0 : x > 0.0 ? 1.0 : 0.0; });
        if (name == "min") return binary([](double x, double y) { return std::min(x, y); });
        if (name == "max") return binary([](double x, double y) { return std::max(x, y); });
        if (name == "pow") return binary([](double x, double y) { return std::pow(x, y); });
        if (name == "atan2") return binary([](double y, double x) { return std::atan2(y, x); });
        if (name == "mod") return binary([](double x, double y) { return std::fmod(x, y); });
        if (name == "clamp")
        {
            if (v.size() != 3) { Fail("clamp expects three arguments"); return 0.0; }
            return std::clamp(v[0], v[1], v[2]);
        }
        if (name == "mix")
        {
            if (v.size() != 3) { Fail("mix expects three arguments"); return 0.0; }
            return v[0] + (v[1] - v[0]) * v[2];
        }
        Fail("unknown function: " + name);
        return 0.0;
    }

    void SkipWhitespace()
    {
        while (m_position < m_source.size() &&
            std::isspace(static_cast<unsigned char>(m_source[m_position])))
            ++m_position;
    }

    bool Consume(char expected)
    {
        if (m_position < m_source.size() && m_source[m_position] == expected)
        {
            ++m_position;
            return true;
        }
        return false;
    }

    bool Fail(std::string message)
    {
        if (m_ok)
        {
            m_ok = false;
            m_error = std::move(message) + " at character " +
                std::to_string(m_position + 1);
        }
        return false;
    }

    class DepthGuard
    {
    public:
        explicit DepthGuard(Parser& parser) : m_parser(parser)
        {
            if (++m_parser.m_depth > 64)
                m_parser.Fail("formula nesting is too deep");
        }
        ~DepthGuard() { --m_parser.m_depth; }
    private:
        Parser& m_parser;
    };

    std::string_view m_source;
    const FormulaVariables& m_variables;
    size_t m_position = 0;
    int m_depth = 0;
    bool m_ok = true;
    std::string m_error;
};
}

bool EvaluateFormula(std::string_view expression,
    const FormulaVariables& variables, double& result, std::string* error)
{
    Parser parser(expression, variables);
    const bool success = parser.Parse(result);
    if (error)
        *error = success ? std::string() : parser.Error();
    return success;
}
}
