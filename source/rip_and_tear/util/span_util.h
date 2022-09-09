#pragma once

#include <span>

namespace span
{
template<class T, class U>
std::span<T> bit_cast(std::span<U> data)
{
    if (data.size() % sizeof(T) == 0)
    {
        return { reinterpret_cast<T*>(data.data()), data.size() / sizeof(T) };
    }
    return {};
}
template<class T, class U>
std::span<T> bit_cast_allow_size_mismatch(std::span<U> data)
{
    return std::span<T>{ reinterpret_cast<T*>(data.data()), data.size() / sizeof(T) };
}
} // namespace span
