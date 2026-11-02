#pragma once
#include <cstdint>
#include <locale>
#include <sstream>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace Engine::Threading
{
// Immutable token stream: scene owners copy values, writers format them later.
// No references to a renderer, scene, component, or temporary string survive.
class CapturedText
{
    struct Text { size_t first,count; };
    using Token=std::variant<Text,uint64_t,int64_t,double>;
    std::vector<Token> m_tokens;
    std::string m_text;
    CapturedText& Append(const char* data,size_t size)
    {
        const auto first=m_text.size(); m_text.append(data,size);
        if(!m_tokens.empty() && std::holds_alternative<Text>(m_tokens.back())) std::get<Text>(m_tokens.back()).count+=size;
        else m_tokens.emplace_back(Text{first,size});
        return *this;
    }
public:
    CapturedText() { m_tokens.reserve(384); m_text.reserve(4096); }
    CapturedText& operator<<(const std::string& value)
    { return Append(value.data(),value.size()); }
    CapturedText& operator<<(const char* value) { return Append(value,std::char_traits<char>::length(value)); }
    CapturedText& operator<<(char value) { return Append(&value,1); }
    template<class T,std::enable_if_t<std::is_arithmetic_v<T>,int> =0>
    CapturedText& operator<<(T value)
    {
        if constexpr(std::is_floating_point_v<T>) m_tokens.emplace_back(double(value));
        else if constexpr(std::is_signed_v<T>) m_tokens.emplace_back(int64_t(value));
        else m_tokens.emplace_back(uint64_t(value));
        return *this;
    }
    size_t OwnedBytes() const { return sizeof(*this)+m_tokens.capacity()*sizeof(Token)+m_text.capacity(); }
    std::string Format() const
    {
        std::ostringstream output; output.imbue(std::locale::classic());
        for(const auto& token:m_tokens) std::visit([&](const auto& value) {
            if constexpr(std::is_same_v<std::decay_t<decltype(value)>,Text>) output.write(m_text.data()+value.first,std::streamsize(value.count));
            else output<<value;
        },token);
        return output.str();
    }
};
}
