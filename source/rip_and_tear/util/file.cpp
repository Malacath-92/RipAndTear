#include "file.h"

#include "on_scope_exit.h"

#include <functional>

std::vector<std::byte> ReadWholeFile(const std::filesystem::path& file_path)
{
    FILE* file{ nullptr };
    errno_t error = _wfopen_s(&file, file_path.c_str(), L"rb");
    if (error == 0 && file != nullptr)
    {
        auto close_file = OnScopeExit{ std::bind_front(fclose, file) };

        fseek(file, 0, SEEK_END);
        const size_t file_size = ftell(file);
        fseek(file, 0, SEEK_SET);

        std::vector<std::byte> code(file_size, std::byte{});

        const auto size_read = fread(code.data(), 1, file_size, file);
        if (size_read != file_size)
        {
            code.clear();
            return code;
        }

        return code;
    }
    return {};
}

bool WriteWholeFile(const std::filesystem::path& file_path, const std::vector<std::byte>& data)
{
    FILE* file{ nullptr };
    errno_t error = _wfopen_s(&file, file_path.c_str(), L"wb");
    if (error == 0 && file != nullptr)
    {
        auto close_file = OnScopeExit{ std::bind_front(fclose, file) };

        fwrite(data.data(), 1, data.size(), file);
        return true;
    }
    return false;
}
