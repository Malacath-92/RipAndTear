#pragma once

#include <string>
#include <string_view>
#include <variant>

enum ExitReason : int32_t
{
    SUCCESS,
    FAILED_UNCAUGHT_EXCEPTION,
    FAILED_PARSING_COMMAND_LINE,
    FAILED_GAME_EXE_DOES_NOT_EXIST,
    FAILED_GAME_EXE_IS_NOT_A_FILE,
    FAILED_LOADING_GAME_EXE,
    FAILED_32BIT_EXE_NOT_SUPPORTED,
    FAILED_UNEXPECTED_TEXT_SECTION,
    FAILED_NO_TEXT_SECTION_FOUND,
    FAILED_MYSTERY_DATA_DIRECTORY,
    FAILED_UNSUPPORTED_DATA_DIRECTORY_ENTRY,
    FAILED_UNSUPPORTED_IMPORT_FORWARDER_CHAIN,
    FAILED_PATCHING_MAIN_FUNCTION,
    FAILED_PATCHING_BUNDLE_INIT_FUNCTION,
};

struct Error
{
    std::string error_message;
    ExitReason error;
};

class RipException
{
  public:
    RipException(Error error)
        : mError{ error }
    {
    }

    std::string_view what() const
    {
        return mError.error_message;
    }
    ExitReason why() const
    {
        return mError.error;
    }

  private:
    Error mError;
};

template<class T>
using expected = std::variant<Error, T>;
template<class T>
[[nodiscard]] T& as_value(const expected<T>& e)
{
    if (std::holds_alternative<T>(e))
    {
        return std::get<T>(e);
    }
    else
    {
        throw RipException(std::get<Error>(e));
    }
}
template<class T>
[[nodiscard]] T as_value(expected<T>&& e)
{
    if (std::holds_alternative<T>(e))
    {
        return std::get<T>(std::move(e));
    }
    else
    {
        throw RipException(std::get<Error>(std::move(e)));
    }
}
