#include "util/algorithms.h"
#include "util/file.h"
#include "util/format.h"
#include "util/on_scope_exit.h"

#include <Windows.h>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <span>

#include <structopt/app.hpp>

#include <magic_enum.hpp>

enum ExitReason : int32_t
{
    SUCCESS,
    FAILED_PARSING_COMMAND_LINE,
    FAILED_GAME_EXE_DOES_NOT_EXIST,
    FAILED_GAME_EXE_IS_NOT_A_FILE,
    FAILED_LOADING_GAME_EXE,
    FAILED_32BIT_EXE_NOT_SUPPORTED,
    FAILED_NO_TEXT_SECTION_FOUND,
};

struct CommandLineOptions
{
    std::optional<std::string> exe_dir;
    std::optional<int32_t> exit_reason;
    std::optional<bool> verbose{ false };
};
VISITABLE_STRUCT(CommandLineOptions, exe_dir, exit_reason, verbose);

template<class T>
[[nodiscard]] T* get_ptr_and_advance(void*& data, size_t num_elements)
{
    T* ptr{ reinterpret_cast<T*>(data) };
    data = reinterpret_cast<void*>(reinterpret_cast<size_t>(data) + sizeof(T) * num_elements);
    return ptr;
};
template<class T>
[[nodiscard]] T& get_ref_and_advance(void*& data)
{
    return *get_ptr_and_advance<T>(data, 1);
};

size_t get_data_bunde_size(const void* bundle_start)
{
    size_t bundle_start_i{ reinterpret_cast<size_t>(bundle_start) };
    std::size_t bundle_size{ 0 };
    while (true)
    {
        std::size_t asset_size = reinterpret_cast<const std::uint32_t*>(bundle_start_i + bundle_size)[0];
        std::size_t name_size = reinterpret_cast<const std::uint32_t*>(bundle_start_i + bundle_size)[1];
        if (asset_size == 0 && name_size == 0)
        {
            break;
        }
        bundle_size += asset_size + name_size + 2 * sizeof(std::uint32_t);
    };
    return bundle_size;
}

int main(int argc, char* argv[])
{
    try
    {
        CommandLineOptions options = structopt::app("Rip & Tear").parse<CommandLineOptions>(argc, argv);

        if (options.exit_reason.has_value())
        {
            // clang-format off
            fmt::print("Exit Code: {}\n"
                "   Reason: {}\n",
                options.exit_reason.value(),
                magic_enum::enum_name<ExitReason>(static_cast<ExitReason>(options.exit_reason.value())));
            // clang-format on
            return SUCCESS;
        }

        if (!options.exe_dir.has_value())
        {
            fmt::print("Please provide the path to Spel2.exe...\n> ");
            options.exe_dir.emplace();
            std::cin >> options.exe_dir.value();
        }

        struct SVerboseLogData
        {
            size_t segment_depth{};

            [[nodiscard]] auto push_segment()
            {
                ++segment_depth;
                return OnScopeExit([this]()
                                   { --segment_depth; });
            }
        };
        SVerboseLogData verbose_log_data;
        auto verbose_print = [ v = options.verbose, d = &verbose_log_data.segment_depth ]<class... Args>(const char* fmt, Args&&... args)
        {
            if (v)
            {
                fmt::print("{:\t>{}}", "", *d);
                fmt::print(fmt, std::forward<Args>(args)...);
                fmt::print("\n");
            }
        };

        namespace fs = std::filesystem;
        const fs::path user_exe_dir{ options.exe_dir.value() };
        const fs::path exe_path{
            std::string_view{ options.exe_dir.value() }.ends_with(".exe")
                ? user_exe_dir
                : user_exe_dir / "Spel2.exe"
        };

        if (!fs::exists(exe_path))
        {
            fmt::print("Path '{}' does not exist...", exe_path.string());
            return FAILED_GAME_EXE_DOES_NOT_EXIST;
        }

        if (!fs::is_regular_file(exe_path))
        {
            fmt::print("Path '{}' is not a file...", exe_path.string());
            return FAILED_GAME_EXE_IS_NOT_A_FILE;
        }

        std::vector<std::byte> exe_contents{ ReadWholeFile(exe_path) };

        verbose_print("Trying to load executable '{}'...", exe_path.string());
        if (exe_contents.empty())
        {
            fmt::print("File '{}' could not be loaded...", exe_path.string());
            return FAILED_LOADING_GAME_EXE;
        }

        verbose_print("Begin patching image...");
        void* data_current{ exe_contents.data() };

        using namespace std::string_view_literals;
        using DOS_STUB = std::array<std::byte, 56>;
        using IMAGE_NT_HEADERS_SIGNATURE = decltype(IMAGE_NT_HEADERS::Signature);

        IMAGE_DOS_HEADER& image_dos_header = get_ref_and_advance<IMAGE_DOS_HEADER>(data_current);
        (void)get_ref_and_advance<DOS_STUB>(data_current);
        (void)get_ref_and_advance<IMAGE_NT_HEADERS_SIGNATURE>(data_current);
        IMAGE_FILE_HEADER& image_file_header = get_ref_and_advance<IMAGE_FILE_HEADER>(data_current);
        if (image_file_header.SizeOfOptionalHeader == sizeof(IMAGE_OPTIONAL_HEADER32))
        {
            return FAILED_32BIT_EXE_NOT_SUPPORTED;
        }
        IMAGE_OPTIONAL_HEADER& image_optional_header = get_ref_and_advance<IMAGE_OPTIONAL_HEADER>(data_current);
        const WORD num_sections{ image_file_header.NumberOfSections };
        std::span<IMAGE_SECTION_HEADER> image_section_headers{
            get_ptr_and_advance<IMAGE_SECTION_HEADER>(data_current, num_sections), num_sections
        };

        verbose_print("Extracted image headers...");
        auto image_headers_popper = verbose_log_data.push_segment();

        verbose_print("Begin patching headers...");

        // Extract resource bundle and shift all sections
        size_t text_section_start{};
        size_t relocation_offset{};
        std::vector<std::byte> bundle_data{};
        for (IMAGE_SECTION_HEADER& header : image_section_headers)
        {
            if (bundle_data.empty() && (const char*)header.Name == ".text"sv)
            {
                verbose_print("Found .text section...");
                auto text_header_popper = verbose_log_data.push_segment();
                verbose_print("Extracting data bundle...");

                // Compute bundle size
                const size_t bundle_data_offset{ header.PointerToRawData };
                std::byte* const bundle_data_start{ exe_contents.data() + bundle_data_offset };
                const size_t bundle_size{ get_data_bunde_size(bundle_data_start) };

                // Align to the value specified in the header
                relocation_offset = bundle_size & ~(static_cast<size_t>(image_optional_header.FileAlignment) - 1);

                verbose_print("Found data bundle of size {:#x}...", bundle_size);
                verbose_print("Aligning to {:#x} bytes, resulting in {:#x}...", image_optional_header.FileAlignment, relocation_offset);

                // Move bundle into it's own data
                bundle_data.resize(bundle_size);
                std::memmove(bundle_data.data(), bundle_data_start, bundle_size);
                std::memset(bundle_data_start, 0, bundle_size);

                verbose_print("Extracted data bundle...");
                verbose_print("Relocating {} section...", (const char*)header.Name);

                // Move the old data up by size of the bundle
                const size_t remaining_text_size{ header.SizeOfRawData - relocation_offset };
                header.Misc.VirtualSize = static_cast<DWORD>(header.Misc.VirtualSize - relocation_offset);
                header.VirtualAddress = static_cast<DWORD>(header.VirtualAddress - relocation_offset);
                header.SizeOfRawData = static_cast<DWORD>(header.SizeOfRawData - relocation_offset);
                std::memmove(bundle_data_start, bundle_data_start + relocation_offset, remaining_text_size);
                std::memset(bundle_data_start + remaining_text_size, 0, relocation_offset);

                verbose_print("Fixing image optional header...");

                // Fix up the header
                image_optional_header.SizeOfCode = static_cast<DWORD>(image_optional_header.SizeOfCode - relocation_offset);
                image_optional_header.AddressOfEntryPoint = static_cast<DWORD>(image_optional_header.AddressOfEntryPoint - relocation_offset);

                // Remember the start of the .text section
                text_section_start = bundle_data_offset;
            }
            else if (text_section_start > 0)
            {
                verbose_print("Found {} section...", (const char*)header.Name);
                auto section_header_popper = verbose_log_data.push_segment();
                verbose_print("Relocating section...");

                // Move the whole section up by the aligned size of the bundle
                const size_t section_offset{ header.PointerToRawData };
                std::byte* const section_start{ exe_contents.data() + section_offset };
                std::byte* const section_end{ exe_contents.data() + section_offset + header.SizeOfRawData };
                header.VirtualAddress = static_cast<DWORD>(header.VirtualAddress - relocation_offset);
                header.PointerToRawData = static_cast<DWORD>(header.PointerToRawData - relocation_offset);
                std::memmove(section_start - relocation_offset, section_start, header.SizeOfRawData);
                std::byte* const new_section_end{ section_start - relocation_offset + header.SizeOfRawData };
                std::byte* const zero_memory_begin{ std::max(section_start, new_section_end) };
                const size_t zero_memory_size{ static_cast<size_t>(section_end - zero_memory_begin) };
                std::memset(zero_memory_begin, 0, zero_memory_size);
            }

            // TODO: Special treatment for segments
        }

        if (text_section_start == 0)
        {
            return FAILED_NO_TEXT_SECTION_FOUND;
        }

        verbose_print("Fixing data directories...");

        // Fix up the data directories
        for (IMAGE_DATA_DIRECTORY& data_directory : image_optional_header.DataDirectory)
        {
            if (data_directory.VirtualAddress > text_section_start)
            {
                data_directory.VirtualAddress = static_cast<DWORD>(data_directory.VirtualAddress - relocation_offset);
            }
        }

#ifndef DEBUG
        {
            verbose_print("Verifying that all data was moved...");

            const size_t last_section_offset{ image_section_headers.back().PointerToRawData };
            const size_t last_section_size{ image_section_headers.back().SizeOfRawData };
            std::byte* const last_section_start{ exe_contents.data() + last_section_offset };
            std::byte* const last_section_end{ last_section_start + last_section_size };
            std::byte* const exe_end{ exe_contents.data() + exe_contents.size() };
            const ptrdiff_t leftover_size{ exe_end - last_section_end };
            if (leftover_size != static_cast<ptrdiff_t>(relocation_offset))
                ;
            auto memzcmp = [](void* memory, ptrdiff_t size)
            {
                std::byte* const memory_bytes{ static_cast<std::byte*>(memory) };
                for (ptrdiff_t i = 0; i < size; i++)
                {
                    if (memory_bytes[i] != std::byte{})
                    {
                        return false;
                    }
                }
                return true;
                // const size_t zero{};
                // std::byte* const memory_bytes{ static_cast<std::byte*>(memory) };
                // const int32_t first_eight_bytes{ std::memcmp(memory, &zero, 8) };
                // if (first_eight_bytes == 0)
                //{
                //     const int32_t staggered_bytes{ std::memcmp(memory_bytes, memory_bytes + 8, size - 8) };
                //     return staggered_bytes == 0;
                // }
                // return false;
            };
            assert(memzcmp(last_section_end, leftover_size));
        }
#endif

        // TODO: Find absolute addresses in binary and patch those
        // TODO: Check which image dos header members need to change
        // TODO: Patch function reading assets (https://gist.github.com/iojon/0bb1e95bad8b601025bfac14e07d8b25)

        (void)image_dos_header;
        (void)image_optional_header;
    }
    catch (structopt::exception& e)
    {
        fmt::print("{}\n", e.what());
        fmt::print("{}\n", e.help());
        return FAILED_PARSING_COMMAND_LINE;
    }

    return SUCCESS;
}
