#include "unwind_info.h"
#include "util/algorithms.h"
#include "util/error.h"
#include "util/file.h"
#include "util/format.h"
#include "util/on_scope_exit.h"
#include "util/span_util.h"

#include <Windows.h>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <span>

#include <structopt/app.hpp>

#include <magic_enum.hpp>

struct CommandLineOptions
{
    std::optional<std::string> exe_dir;
    std::optional<std::string> output_name{ "Tear2.exe" };
    std::optional<int32_t> exit_reason;
    std::optional<bool> verbose{ false };
};
VISITABLE_STRUCT(CommandLineOptions, exe_dir, output_name, exit_reason, verbose);

struct Logger
{
    bool is_verbose{};
    size_t segment_depth{};

    [[nodiscard]] auto push_segment()
    {
        ++segment_depth;
        return OnScopeExit([this]()
                           { --segment_depth; });
    }

    template<class... Args>
    void verbose(const char* fmt, Args&&... args)
    {
        if (is_verbose)
        {
            fmt::print("{:\t>{}}", "", segment_depth);
            fmt::print(fmt, std::forward<Args>(args)...);
            fmt::print("\n");
        }
    }
    template<class... Args>
    void warning(const char* fmt, Args&&... args)
    {
        fmt::print(fmt::fg(fmt::color::yellow), "{:\t>{}}", "Warning: ", segment_depth);
        fmt::print(fmt::fg(fmt::color::yellow), fmt, std::forward<Args>(args)...);
        fmt::print("\n");
    }
    template<class... Args>
    void error(const char* fmt, Args&&... args)
    {
        fmt::print(fmt::fg(fmt::color::red), "{:\t>{}}", "Error: ", segment_depth);
        fmt::print(fmt::fg(fmt::color::yellow), fmt, std::forward<Args>(args)...);
        fmt::print("\n");
    }
};

template<class T, class U>
[[nodiscard]] T* get_ptr(U* const data)
{
    return reinterpret_cast<T*>(data);
};
template<class T, class U>
[[nodiscard]] T* get_ptr_and_advance(U*& data, size_t num_elements)
{
    T* ptr{ get_ptr<T>(data) };
    data = reinterpret_cast<U*>(reinterpret_cast<size_t>(data) + sizeof(T) * num_elements);
    return ptr;
};
template<class T, class U>
[[nodiscard]] T& get_ref(U* const data)
{
    return *get_ptr<T>(data);
};
template<class T, class U>
[[nodiscard]] T& get_ref_and_advance(U*& data)
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

expected<std::filesystem::path> get_exe_path(const CommandLineOptions& options)
{
    namespace fs = std::filesystem;

    const fs::path user_exe_dir{ options.exe_dir.value() };
    const fs::path exe_path{
        std::string_view{ options.exe_dir.value() }.ends_with(".exe")
            ? user_exe_dir
            : user_exe_dir / "Spel2.exe"
    };

    if (!fs::exists(exe_path))
    {
        return Error{
            fmt::format("Path '{}' does not exist...", exe_path.string()),
            FAILED_GAME_EXE_DOES_NOT_EXIST,
        };
    }

    if (!fs::is_regular_file(exe_path))
    {
        return Error{
            fmt::format("Path '{}' is not a file...", exe_path.string()),
            FAILED_GAME_EXE_IS_NOT_A_FILE,
        };
    }

    return exe_path;
}

int main(int argc, char* argv[])
{
    try
    {
        CommandLineOptions options = structopt::app("Rip & Tear").parse<CommandLineOptions>(argc, argv);
        Logger logger{ options.verbose.value_or(false) };

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

        namespace fs = std::filesystem;

        const fs::path exe_path = as_value(get_exe_path(options));

        std::vector<std::byte> exe_data{ ReadWholeFile(exe_path) };

        logger.verbose("Trying to load executable '{}'...", exe_path.string());
        if (exe_data.empty())
        {
            fmt::print("File '{}' could not be loaded...", exe_path.string());
            return FAILED_LOADING_GAME_EXE;
        }

        logger.verbose("Begin patching image...");
        void* data_current{ exe_data.data() };

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

        logger.verbose("Extracted image headers...");

        size_t text_section_start{};
        size_t relocation_offset{};
        std::vector<std::byte> bundle_data{};

        {
            logger.verbose("Begin patching headers and relocating sections...");
            auto image_headers_popper = logger.push_segment();

            // Extract resource bundle and shift all sections
            for (IMAGE_SECTION_HEADER& header : image_section_headers)
            {
                if (bundle_data.empty() && (const char*)header.Name == ".text"sv)
                {
                    logger.verbose("Found .text section...");
                    auto text_header_popper = logger.push_segment();
                    logger.verbose("Extracting data bundle...");

                    // Compute bundle size
                    const size_t bundle_data_offset{ header.PointerToRawData };
                    std::byte* const bundle_data_start{ exe_data.data() + bundle_data_offset };
                    const size_t bundle_size{ get_data_bunde_size(bundle_data_start) };

                    // Align to the value specified in the header
                    relocation_offset = bundle_size & ~(static_cast<size_t>(image_optional_header.FileAlignment) - 1);

                    logger.verbose("Found data bundle of size {:#x}...", bundle_size);
                    logger.verbose("Aligning to {:#x} bytes, resulting in {:#x}...", image_optional_header.FileAlignment, relocation_offset);

                    // Move bundle into it's own data
                    bundle_data.resize(bundle_size);
                    std::memmove(bundle_data.data(), bundle_data_start, bundle_size);
                    std::memset(bundle_data_start, 0, bundle_size);

                    logger.verbose("Extracted data bundle...");
                    logger.verbose("Relocating {} section...", (const char*)header.Name);

                    // Move the old data up by size of the bundle
                    const size_t remaining_text_size{ header.SizeOfRawData - relocation_offset };
                    header.Misc.VirtualSize = static_cast<DWORD>(header.Misc.VirtualSize - relocation_offset);
                    header.SizeOfRawData = static_cast<DWORD>(header.SizeOfRawData - relocation_offset);
                    std::memmove(bundle_data_start, bundle_data_start + relocation_offset, remaining_text_size);
                    std::memset(bundle_data_start + remaining_text_size, 0, relocation_offset);

                    logger.verbose("Fixing image optional header...");

                    // Fix up the header
                    image_optional_header.SizeOfImage = static_cast<DWORD>(image_optional_header.SizeOfImage - relocation_offset);
                    image_optional_header.SizeOfCode = static_cast<DWORD>(image_optional_header.SizeOfCode - relocation_offset);
                    image_optional_header.AddressOfEntryPoint = static_cast<DWORD>(image_optional_header.AddressOfEntryPoint - relocation_offset);

                    // Remember the start of the .text section
                    text_section_start = bundle_data_offset;
                }
                else if (text_section_start > 0)
                {
                    logger.verbose("Found {} section...", (const char*)header.Name);
                    auto section_header_popper = logger.push_segment();
                    logger.verbose("Relocating section...");

                    // Move the whole section up by the aligned size of the bundle
                    const size_t section_offset{ header.PointerToRawData };
                    std::byte* const section_start{ exe_data.data() + section_offset };
                    std::byte* const section_end{ exe_data.data() + section_offset + header.SizeOfRawData };
                    header.VirtualAddress = static_cast<DWORD>(header.VirtualAddress - relocation_offset);
                    header.PointerToRawData = static_cast<DWORD>(header.PointerToRawData - relocation_offset);
                    std::memmove(section_start - relocation_offset, section_start, header.SizeOfRawData);
                    std::byte* const new_section_end{ section_start - relocation_offset + header.SizeOfRawData };
                    std::byte* const zero_memory_begin{ std::max(section_start, new_section_end) };
                    const size_t zero_memory_size{ static_cast<size_t>(section_end - zero_memory_begin) };
                    std::memset(zero_memory_begin, 0, zero_memory_size);
                }
                else
                {
                    logger.warning(".text segment is not the first segment, some data may be corrupt...");
                }
            }
        }

        if (text_section_start == 0)
        {
            return FAILED_NO_TEXT_SECTION_FOUND;
        }

        {
            logger.verbose("Patching relative virtual addresses in sections...");
            auto patch_sections_popper = logger.push_segment();
            for (IMAGE_SECTION_HEADER& header : image_section_headers)
            {
                logger.verbose("Patching {}...", (const char*)header.Name);
                auto patch_section_popper = logger.push_segment();

                const size_t section_offset{ header.PointerToRawData };
                const size_t section_size{ header.SizeOfRawData };
                std::byte* section_data = exe_data.data() + section_offset;

                if ((const char*)header.Name == ".pdata"sv)
                {
                    logger.verbose("Patching runtime function entries...");

                    std::span<IMAGE_RUNTIME_FUNCTION_ENTRY> image_runtime_function_array{
                        span::bit_cast<IMAGE_RUNTIME_FUNCTION_ENTRY>(std::span<std::byte>{ section_data, section_size }),
                    };
                    for (IMAGE_RUNTIME_FUNCTION_ENTRY& image_runtime_function_entry : image_runtime_function_array)
                    {
                        if (image_runtime_function_entry.BeginAddress == 0)
                        {
                            break;
                        }

                        image_runtime_function_entry.BeginAddress = static_cast<DWORD>(image_runtime_function_entry.BeginAddress - relocation_offset);
                        image_runtime_function_entry.EndAddress = static_cast<DWORD>(image_runtime_function_entry.EndAddress - relocation_offset);
                        image_runtime_function_entry.UnwindInfoAddress = static_cast<DWORD>(image_runtime_function_entry.UnwindInfoAddress - relocation_offset);

                        const size_t unwind_info_offset{ image_runtime_function_entry.UnwindInfoAddress };
                        std::byte* const unwind_info_data = exe_data.data() + unwind_info_offset;
                        UNWIND_INFO& unwind_info{ get_ref<UNWIND_INFO>(unwind_info_data) };
                        if (unwind_info.Flags & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER))
                        {
                            ULONG& except_handler_offset = *(PULONG)GetLanguageSpecificDataPtr(&unwind_info);
                            except_handler_offset = static_cast<ULONG>(except_handler_offset - relocation_offset);
                        }
                        else if (unwind_info.Flags & UNW_FLAG_CHAININFO)
                        {
                            ULONG& chain_info_offset = *(PULONG)GetLanguageSpecificDataPtr(&unwind_info);
                            chain_info_offset = static_cast<ULONG>(chain_info_offset - relocation_offset);
                        }
                    }

                    // TODO: More to do here?
                }
                else if ((const char*)header.Name == ".rsrc"sv)
                {
                    logger.verbose("Patching resource directory...");
                    auto res_section_popper = logger.push_segment();

                    std::byte* const section_start{ section_data };
                    auto patch_directory_recursive = [&](std::byte* directory_data, auto& self) -> void
                    {
                        IMAGE_RESOURCE_DIRECTORY& resource_directory = get_ref_and_advance<IMAGE_RESOURCE_DIRECTORY>(directory_data);
                        for (size_t i = 0; i < resource_directory.NumberOfNamedEntries + resource_directory.NumberOfIdEntries; i++)
                        {
                            IMAGE_RESOURCE_DIRECTORY_ENTRY& resource_entry = get_ref_and_advance<IMAGE_RESOURCE_DIRECTORY_ENTRY>(directory_data);

                            const bool named{ i < resource_directory.NumberOfNamedEntries };
                            if (named)
                            {
                                if (resource_entry.NameIsString)
                                {
                                    std::byte* const name_address{ section_start + resource_entry.NameOffset };
                                    IMAGE_RESOURCE_DIR_STRING_U& name{ get_ref<IMAGE_RESOURCE_DIR_STRING_U>(name_address) };
                                    const std::string name_utf8{ algo::to_utf8(std::wstring{ name.NameString, name.Length }) };
                                    logger.verbose("Named: {}", name_utf8);
                                }
                                else
                                {
                                    logger.verbose("Named: {}", (DWORD)resource_entry.NameOffset);
                                }
                            }
                            else
                            {
                                logger.verbose("Id: {}", resource_entry.Id);
                            }

                            if (resource_entry.DataIsDirectory)
                            {
                                auto dir_popper = logger.push_segment();
                                self(section_start + resource_entry.OffsetToDirectory, self);
                            }
                            else
                            {
                                std::byte* const data_address{ section_start + resource_entry.OffsetToData };
                                IMAGE_RESOURCE_DATA_ENTRY& data_entry{ get_ref<IMAGE_RESOURCE_DATA_ENTRY>(data_address) };
                                data_entry.OffsetToData = static_cast<DWORD>(data_entry.OffsetToData - relocation_offset);
                            }
                        }
                    };
                    logger.verbose("Root");
                    auto root_popper = logger.push_segment();
                    patch_directory_recursive(section_data, patch_directory_recursive);
                }
                else if ((const char*)header.Name == ".reloc"sv)
                {
                    logger.verbose("Patching relocation table...");

                    while (true)
                    {
                        DWORD& page_offset{ get_ref_and_advance<DWORD>(section_data) };
                        if (page_offset > 0)
                        {
                            page_offset = static_cast<DWORD>(page_offset - relocation_offset);
                            DWORD& reloc_block_size{ get_ref_and_advance<DWORD>(section_data) };
                            section_data += reloc_block_size - sizeof(page_offset) - sizeof(reloc_block_size);
                        }
                        else
                        {
                            break;
                        }
                    }
                }
                else
                {
                    logger.verbose("No work to be done...");
                }
            }
        }

        {
            logger.verbose("Patching absolute addresses in sections...");
            auto patch_sections_popper = logger.push_segment();
            for (IMAGE_SECTION_HEADER& header : image_section_headers)
            {
                logger.verbose("Patching {}...", (const char*)header.Name);
                auto patch_section_popper = logger.push_segment();

                const size_t section_offset{ header.PointerToRawData };
                const size_t section_size{ header.SizeOfRawData };
                std::byte* section_data = exe_data.data() + section_offset;

                constexpr size_t absolute_memory_range_lower{ 0x162000000 };
                constexpr size_t absolute_memory_range_upper{ 0x162ffffff };

                std::span<size_t> image_as_aligned_eight_bytes{
                    span::bit_cast<size_t>(std::span<std::byte>{ section_data, section_size }),
                };
                size_t num_values_patched{};
                for (size_t& value : image_as_aligned_eight_bytes)
                {
                    if (value >= absolute_memory_range_lower && value <= absolute_memory_range_upper)
                    {
                        value = value - relocation_offset;
                        num_values_patched++;
                    }
                }
                logger.verbose("Patched {} addresses...", num_values_patched);
            }
        }

        {
            logger.verbose("Patching main function...");
            auto patch_main_popper = logger.push_segment();
            constexpr std::string_view pattern_before_virtual_lock{ "\xba\x00\x00\x00\x40\x41\xb8\x00\x00\x00\x80"sv };
            for (size_t i = 0; i < exe_data.size(); i++)
            {
                if (i > (exe_data.size() - pattern_before_virtual_lock.size()))
                {
                    fmt::print("Could not patch main function...");
                    return FAILED_PATCHING_MAIN_FUNCTION;
                }

                // Find the pattern
                if (std::memcmp(exe_data.data() + i, pattern_before_virtual_lock.data(), pattern_before_virtual_lock.size()) == 0)
                {
                    // Find next call
                    while (exe_data[i] != std::byte{ 0xff })
                    {
                        ++i;
                    }
                    // Skip the call
                    i += 6;

                    // Expected following instructions:
                    // MOV EDX, dword ptr [g_DataBundleSize]
                    // LEA RCX, [g_DataBundle]
                    // CALL qword ptr[->KERNEL32.DLL::VirtualLock]

                    // Replace with:
                    // XOR RAX, RAX
                    // MOV DWORD PTR [g_DataBundleSize], EAX
                    // MOV QWORD PTR [g_DataBundlePtr], RAX
                    // NOP DWORD PTR [RAX] # 3-byte NOP

                    logger.verbose("Found VirtualLock call in main, replacing with nop...");

                    constexpr std::string_view new_code{
                        "\x48\x31\xC0"
                        "\x89\x05\xCD\x19\x16\x00"
                        "\x48\x89\x05\xCD\x19\x16\x00"
                        "\x0F\x1F\x00"sv
                    };
                    std::memcpy(exe_data.data() + i, new_code.data(), new_code.size());
                    break;
                }
            }
        }

        logger.verbose("Fixing data directories...");

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
            logger.verbose("Verifying that all data was moved...");
            auto debug_popper = logger.push_segment();

            const size_t last_section_offset{ image_section_headers.back().PointerToRawData };
            const size_t last_section_size{ image_section_headers.back().SizeOfRawData };
            std::byte* const last_section_start{ exe_data.data() + last_section_offset };
            std::byte* const last_section_end{ last_section_start + last_section_size };
            std::byte* const exe_end{ exe_data.data() + exe_data.size() };
            const ptrdiff_t leftover_size{ exe_end - last_section_end };
            if (leftover_size != static_cast<ptrdiff_t>(relocation_offset))
            {
                logger.warning("Additional data left at end of executable, leftover {:#x} bytes...", leftover_size - static_cast<ptrdiff_t>(relocation_offset));
            }
            auto memzcmp = [](void* memory, ptrdiff_t size)
            {
                const size_t zero{};
                std::byte* const memory_bytes{ static_cast<std::byte*>(memory) };
                const int32_t first_eight_bytes{ std::memcmp(memory, &zero, 8) };
                if (first_eight_bytes == 0)
                {
                    const int32_t staggered_bytes{ std::memcmp(memory_bytes, memory_bytes + 8, size - 8) };
                    return staggered_bytes == 0;
                }
                return false;
            };
            if (!memzcmp(last_section_end, leftover_size))
            {
                logger.warning("Not all data past the relocated executable is nulled out...");
            }
        }
#endif
        {
            logger.verbose("Resizing binary...");
            auto resize_popper = logger.push_segment();

            const size_t last_section_offset{ image_section_headers.back().PointerToRawData };
            const size_t last_section_size{ image_section_headers.back().SizeOfRawData };
            const size_t total_binary_size{ last_section_offset + last_section_size };
            logger.verbose("Old size: {:#x}", exe_data.size());
            logger.verbose("New size: {:#x}", total_binary_size);
            logger.verbose("   Saved: {:#x}", exe_data.size() - total_binary_size);
            exe_data.resize(total_binary_size);
        }

        {
            const fs::path out_path{
                exe_path.parent_path() / options.output_name.value()
            };
            logger.verbose("Writing out new executable to '{}'...", out_path.string());
            auto write_popper = logger.push_segment();

            if (fs::exists(out_path))
            {
                fmt::print("Path '{}' already exists, press any key to override and continue...", out_path.string());
                std::cin.get();
                if (fs::exists(out_path))
                {
                    fs::remove(out_path);
                }
            }

            WriteWholeFile(out_path, exe_data);
        }

        // TODO: Find absolute addresses in binary and patch those
        // TODO: Check which image dos header members need to change (?page size?)
        // TODO: Patch function reading assets (https://gist.github.com/iojon/0bb1e95bad8b601025bfac14e07d8b25)
        // TODO: nop function that calculates the size of the data bundle
        // TODO: nop 0x22c77de2 -- VirtualLock(&g_DataBundle,(ulonglong)g_DataBundleSize);
        // TODO: handle some code in GameMain around 0x22c782ef

        (void)image_dos_header;
    }
    catch (const RipException& e)
    {
        fmt::print(fmt::fg(fmt::color::red), "Error: {}\n", e.what());
        return e.why();
    }
    catch (const structopt::exception& e)
    {
        fmt::print(fmt::fg(fmt::color::red), "Error: {}\n", e.what());
        fmt::print("{}\n", e.help());
        return FAILED_PARSING_COMMAND_LINE;
    }
    catch (const std::exception& e)
    {
        fmt::print(fmt::fg(fmt::color::red), "Error: {}\n", e.what());
        return FAILED_UNCAUGHT_EXCEPTION;
    }

    return SUCCESS;
}
