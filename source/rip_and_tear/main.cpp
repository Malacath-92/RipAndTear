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
    std::optional<bool> insert_debug_interrupts{ false };
    std::optional<bool> debug_do_not_relocated{ false };
    std::optional<int32_t> exit_reason;
    std::optional<bool> verbose{ false };
};
VISITABLE_STRUCT(CommandLineOptions, exe_dir, output_name, insert_debug_interrupts, debug_do_not_relocated, exit_reason, verbose);

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
template<class T, class U>
[[nodiscard]] std::span<T> get_nullterminated_table(U* const data)
{
    const size_t table_size = [&]()
    {
        static constexpr T zero_object{};
        static constexpr size_t no_idx{ 0xffffffffffffffff };
        T* table_memory{ get_ptr<T>(data) };
        for (size_t i = 0; i < no_idx; i++)
        {
            if (memcmp(table_memory + i, &zero_object, sizeof(T)) == 0)
            {
                return i;
            }
        }

        return no_idx;
    }();
    std::span<T> table{
        get_ptr<T>(data),
        table_size,
    };
    return table;
}
template<class T, class U>
[[nodiscard]] std::span<T> get_nullterminated_table_and_advance(U*& data)
{
    std::span<T> table{ get_nullterminated_table<T>(data) };
    data += sizeof(T) * (table.size() + 1);
    return table;
}

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

std::byte* find_pattern(std::span<std::byte> executable, std::string_view pattern)
{
    for (size_t i = 0; i < executable.size() - pattern.size(); i++)
    {
        if (std::memcmp(executable.data() + i, pattern.data(), pattern.size()) == 0)
        {
            return executable.data() + i;
        }
    }
    return nullptr;
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

    return std::move(exe_path);
}

expected<std::vector<std::byte>> load_exe(Logger& logger, const std::filesystem::path& exe_path)
{
    logger.verbose("Trying to load executable '{}'...", exe_path.string());

    std::vector<std::byte> exe_data{ ReadWholeFile(exe_path) };
    if (exe_data.empty())
    {
        return Error{
            fmt::format("File '{}' could not be loaded...", exe_path.string()),
            FAILED_LOADING_GAME_EXE,
        };
    }

    return std::move(exe_data);
}

struct PortableExecutableHeaders
{
    IMAGE_DOS_HEADER& image_dos_header;
    IMAGE_FILE_HEADER& image_file_header;
    IMAGE_OPTIONAL_HEADER& image_optional_header;
    std::span<IMAGE_SECTION_HEADER> image_section_headers;
};
expected<PortableExecutableHeaders> get_headers(Logger& logger, std::byte* executable)
{
    using namespace std::string_view_literals;
    using DOS_STUB = std::array<std::byte, 56>;
    using IMAGE_NT_HEADERS_SIGNATURE = decltype(IMAGE_NT_HEADERS::Signature);

    logger.verbose("Extracting image headers...");

    void* data_current{ executable };

    IMAGE_DOS_HEADER& image_dos_header = get_ref_and_advance<IMAGE_DOS_HEADER>(data_current);
    (void)get_ref_and_advance<DOS_STUB>(data_current);
    (void)get_ref_and_advance<IMAGE_NT_HEADERS_SIGNATURE>(data_current);
    IMAGE_FILE_HEADER& image_file_header = get_ref_and_advance<IMAGE_FILE_HEADER>(data_current);
    if (image_file_header.SizeOfOptionalHeader == sizeof(IMAGE_OPTIONAL_HEADER32))
    {
        return Error{
            "32bit executable is not supported, where the hell did you even get this from???",
            FAILED_32BIT_EXE_NOT_SUPPORTED,
        };
    }
    IMAGE_OPTIONAL_HEADER& image_optional_header = get_ref_and_advance<IMAGE_OPTIONAL_HEADER>(data_current);
    const WORD num_sections{ image_file_header.NumberOfSections };
    std::span<IMAGE_SECTION_HEADER> image_section_headers{
        get_ptr_and_advance<IMAGE_SECTION_HEADER>(data_current, num_sections), num_sections
    };

    return PortableExecutableHeaders{
        image_dos_header,
        image_file_header,
        image_optional_header,
        image_section_headers,
    };
}

struct BundleExtractionResult
{
    size_t text_section_start;
    size_t relocation_offset;
    std::vector<std::byte> bundle_data;
};
expected<BundleExtractionResult> extract_bundle(Logger logger,
                                                IMAGE_OPTIONAL_HEADER& image_optional_header,
                                                std::span<IMAGE_SECTION_HEADER> image_section_headers,
                                                std::byte* executable,
                                                bool debug_do_not_relocated)
{
    using namespace std::string_view_literals;

    logger.verbose("Begin patching headers and relocating sections...");
    auto image_headers_popper = logger.push_segment();

    size_t text_section_start{};
    size_t relocation_offset{};
    std::vector<std::byte> bundle_data{};

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
            std::byte* const bundle_data_start{ executable + bundle_data_offset };
            const size_t bundle_size{ get_data_bunde_size(bundle_data_start) };

            if (debug_do_not_relocated)
            {
                relocation_offset = 0;
            }
            else
            {
                // Align to the value specified in the header
                relocation_offset = bundle_size & ~(static_cast<size_t>(image_optional_header.FileAlignment) - 1);
            }

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
            std::byte* const section_start{ executable + section_offset };
            std::byte* const section_end{ executable + section_offset + header.SizeOfRawData };
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
            return Error{
                ".text section is not the first section...",
                FAILED_UNEXPECTED_TEXT_SECTION,
            };
        }
    }

    if (text_section_start == 0)
    {
        return Error{
            "Could not find .text section...",
            FAILED_NO_TEXT_SECTION_FOUND,
        };
    }

    return BundleExtractionResult{
        text_section_start,
        relocation_offset,
        std::move(bundle_data),
    };
}

void patch_absolute_addresses(Logger logger,
                              std::span<IMAGE_SECTION_HEADER> image_section_headers,
                              size_t relocation_offset,
                              std::byte* executable)
{
    logger.verbose("Patching absolute addresses in sections...");
    auto patch_sections_popper = logger.push_segment();
    for (IMAGE_SECTION_HEADER& header : image_section_headers)
    {
        logger.verbose("Patching {}...", (const char*)header.Name);
        auto patch_section_popper = logger.push_segment();

        const size_t section_offset{ header.PointerToRawData };
        const size_t section_size{ header.SizeOfRawData };
        std::byte* section_data = executable + section_offset;

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

[[nodiscard]] expected<std::monostate> patch_data_directory(Logger& logger,
                                                            IMAGE_OPTIONAL_HEADER& image_optional_header,
                                                            std::span<IMAGE_SECTION_HEADER> image_section_headers,
                                                            size_t text_section_start,
                                                            size_t relocation_offset,
                                                            std::byte* executable)
{
    logger.verbose("Patch data directory...");
    auto data_directory_popper{ logger.push_segment() };

    for (const auto& [idx, data_directory] : algo::enumerate(image_optional_header.DataDirectory))
    {
        if (data_directory.VirtualAddress > text_section_start)
        {
            data_directory.VirtualAddress = static_cast<DWORD>(data_directory.VirtualAddress - relocation_offset);

            const IMAGE_SECTION_HEADER* containing_section = algo::find_if(
                image_section_headers,
                [&](const IMAGE_SECTION_HEADER& image_section_headers)
                {
                    // clang-format off
                    return image_section_headers.VirtualAddress <= data_directory.VirtualAddress
                        && image_section_headers.VirtualAddress + image_section_headers.Misc.VirtualSize > data_directory.VirtualAddress;
                    // clang-format on
                });

            if (containing_section == nullptr)
            {
                return Error{
                    "Data directory is not contained within a section...",
                    FAILED_MYSTERY_DATA_DIRECTORY,
                };
            }

            const ptrdiff_t virtual_raw_offset{ static_cast<ptrdiff_t>(containing_section->PointerToRawData) - containing_section->VirtualAddress };
            const ptrdiff_t address_of_raw_data{ data_directory.VirtualAddress + virtual_raw_offset };
            std::byte* const entry_start{ executable + address_of_raw_data };

            enum class DataDirectoryEntry
            {
                EXPORT = IMAGE_DIRECTORY_ENTRY_EXPORT,
                IMPORT = IMAGE_DIRECTORY_ENTRY_IMPORT,
                RESOURCE = IMAGE_DIRECTORY_ENTRY_RESOURCE,
                EXCEPTION = IMAGE_DIRECTORY_ENTRY_EXCEPTION,
                SECURITY = IMAGE_DIRECTORY_ENTRY_SECURITY,
                BASERELOC = IMAGE_DIRECTORY_ENTRY_BASERELOC,
                DEBUG = IMAGE_DIRECTORY_ENTRY_DEBUG,
                ARCHITECTURE = IMAGE_DIRECTORY_ENTRY_ARCHITECTURE,
                GLOBALPTR = IMAGE_DIRECTORY_ENTRY_GLOBALPTR,
                TLS = IMAGE_DIRECTORY_ENTRY_TLS,
                LOAD_CONFIG = IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG,
                BOUND_IMPORT = IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT,
                IAT = IMAGE_DIRECTORY_ENTRY_IAT,
                DELAY_IMPORT = IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT,
                COM_DESCRIPTOR = IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR,
            };
            DataDirectoryEntry data_directory_entry{ static_cast<DataDirectoryEntry>(idx) };

            switch (data_directory_entry)
            {
            case DataDirectoryEntry::EXPORT:
            {
                logger.verbose("Patching export table...");
                auto export_table_popper{ logger.push_segment() };

                IMAGE_EXPORT_DIRECTORY& export_table{ get_ref<IMAGE_EXPORT_DIRECTORY>(entry_start) };
                export_table.Name = static_cast<DWORD>(export_table.Name - relocation_offset);
                auto offset_pointer_table = [&](size_t rva, size_t size)
                {
                    std::span<DWORD> table{
                        get_ptr<DWORD>(executable + rva + virtual_raw_offset),
                        size,
                    };
                    for (DWORD& table_entry : table)
                    {
                        if (table_entry > 0)
                        {
                            table_entry = static_cast<DWORD>(table_entry - relocation_offset);
                        }
                    }
                    return table;
                };

                if (logger.is_verbose)
                {
                    offset_pointer_table(export_table.AddressOfFunctions, export_table.NumberOfFunctions);
                    const std::span<DWORD> names{ offset_pointer_table(export_table.AddressOfNames, export_table.NumberOfNames) };
                    for (const DWORD name_rva : names)
                    {
                        const char* name{ reinterpret_cast<const char*>(executable + name_rva + virtual_raw_offset) };
                        logger.verbose("Relocating exported function '{}'...", name);
                    }
                }

                break;
            }
            case DataDirectoryEntry::IMPORT:
            {
                logger.verbose("Patching import table...");
                auto import_table_popper{ logger.push_segment() };

                std::span<IMAGE_IMPORT_DESCRIPTOR> import_desc_table{
                    get_nullterminated_table<IMAGE_IMPORT_DESCRIPTOR>(entry_start)
                };

                for (IMAGE_IMPORT_DESCRIPTOR& import_desc : import_desc_table)
                {
                    import_desc.OriginalFirstThunk = static_cast<DWORD>(import_desc.OriginalFirstThunk - relocation_offset);
                    import_desc.Name = static_cast<DWORD>(import_desc.Name - relocation_offset);
                    import_desc.FirstThunk = static_cast<DWORD>(import_desc.FirstThunk - relocation_offset);

                    const char* module_name{ reinterpret_cast<const char*>(executable + import_desc.Name + virtual_raw_offset) };
                    logger.verbose("Relocating imported module '{}'...", module_name);
                    auto import_module_popper{ logger.push_segment() };

                    const ptrdiff_t original_thunk_table_address{ import_desc.OriginalFirstThunk + virtual_raw_offset };
                    std::span<IMAGE_THUNK_DATA> original_thunk_table{
                        get_nullterminated_table<IMAGE_THUNK_DATA>(executable + original_thunk_table_address)
                    };
                    for (IMAGE_THUNK_DATA& original_thunk : original_thunk_table)
                    {
                        using THUNK_T = decltype(decltype(IMAGE_THUNK_DATA::u1)::AddressOfData);
                        constexpr THUNK_T ordinal_bit{ THUNK_T(1) << (sizeof(THUNK_T) * 8 - 1) };
                        const bool is_ordinal{ (original_thunk.u1.AddressOfData & ordinal_bit) != 0 };
                        if (is_ordinal)
                        {
                            const size_t ordinal{ original_thunk.u1.Ordinal & ~ordinal_bit };
                            logger.verbose("No need to relocate imported function '{}::Ordinal.{}'...", module_name, ordinal);
                        }
                        else
                        {
                            original_thunk.u1.AddressOfData = static_cast<DWORD>(original_thunk.u1.AddressOfData - relocation_offset);
                            IMAGE_IMPORT_BY_NAME* function_import_desc{
                                reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(executable + original_thunk.u1.AddressOfData + virtual_raw_offset)
                            };
                            logger.verbose("Relocating imported function '{}::{}'...", module_name, function_import_desc->Name);
                        }
                    }

                    const ptrdiff_t thunk_table_address{ import_desc.FirstThunk + virtual_raw_offset };
                    std::span<size_t> thunk_table{
                        get_nullterminated_table<size_t>(executable + thunk_table_address)
                    };
                    for (size_t& thunk : thunk_table)
                    {
                        thunk = thunk - relocation_offset;
                    }

                    if (import_desc.ForwarderChain != 0)
                    {
                        return Error{
                            "Import forwarder chain is not supported...",
                            FAILED_UNSUPPORTED_IMPORT_FORWARDER_CHAIN,
                        };
                    }
                }

                break;
            }
            case DataDirectoryEntry::RESOURCE:
            {
                logger.verbose("Patching resource directory...");
                auto res_section_popper = logger.push_segment();

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
                                std::byte* const name_address{ entry_start + resource_entry.NameOffset };
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
                            self(entry_start + resource_entry.OffsetToDirectory, self);
                        }
                        else
                        {
                            std::byte* const data_address{ entry_start + resource_entry.OffsetToData };
                            IMAGE_RESOURCE_DATA_ENTRY& data_entry{ get_ref<IMAGE_RESOURCE_DATA_ENTRY>(data_address) };
                            data_entry.OffsetToData = static_cast<DWORD>(data_entry.OffsetToData - relocation_offset);
                        }
                    }
                };
                logger.verbose("Root");
                auto root_popper = logger.push_segment();
                patch_directory_recursive(entry_start, patch_directory_recursive);

                break;
            }
            case DataDirectoryEntry::EXCEPTION:
            {
                logger.verbose("Patching runtime function entries...");

                std::span<IMAGE_RUNTIME_FUNCTION_ENTRY> image_runtime_function_array{
                    get_nullterminated_table<IMAGE_RUNTIME_FUNCTION_ENTRY>(entry_start),
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
                    std::byte* const unwind_info_data = executable + unwind_info_offset;
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
                break;
            }
            case DataDirectoryEntry::BASERELOC:
            {
                logger.verbose("Patching relocation table...");

                std::byte* reloc_table_data{ entry_start };
                while (true)
                {
                    DWORD& page_offset{ get_ref_and_advance<DWORD>(reloc_table_data) };
                    if (page_offset > 0)
                    {
                        page_offset = static_cast<DWORD>(page_offset - relocation_offset);
                        DWORD& reloc_block_size{ get_ref_and_advance<DWORD>(reloc_table_data) };
                        reloc_table_data += reloc_block_size - sizeof(page_offset) - sizeof(reloc_block_size);
                    }
                    else
                    {
                        break;
                    }
                }

                break;
            }
            case DataDirectoryEntry::SECURITY:
                [[fallthrough]];
            case DataDirectoryEntry::DEBUG:
                [[fallthrough]];
            case DataDirectoryEntry::ARCHITECTURE:
                [[fallthrough]];
            case DataDirectoryEntry::GLOBALPTR:
                [[fallthrough]];
            case DataDirectoryEntry::TLS:
                [[fallthrough]];
            case DataDirectoryEntry::LOAD_CONFIG:
                [[fallthrough]];
            case DataDirectoryEntry::BOUND_IMPORT:
                [[fallthrough]];
            case DataDirectoryEntry::IAT:
                [[fallthrough]];
            case DataDirectoryEntry::DELAY_IMPORT:
                [[fallthrough]];
            case DataDirectoryEntry::COM_DESCRIPTOR:
                [[fallthrough]];
            default:
                return Error{
                    fmt::format("Data Directory Entry '{}' is not supported...", magic_enum::enum_name(data_directory_entry)),
                    FAILED_UNSUPPORTED_DATA_DIRECTORY_ENTRY,
                };
            }
        }
    }

    return std::monostate{};
}

[[nodiscard]] expected<std::monostate> patch_main(Logger& logger,
                                                  std::span<std::byte> executable,
                                                  bool debug_int)
{
    using namespace std::string_view_literals;

    logger.verbose("Patching main function...");
    auto patch_main_popper = logger.push_segment();

    constexpr std::string_view pattern_before_virtual_lock{ "\xba\x00\x00\x00\x40\x41\xb8\x00\x00\x00\x80"sv };
    if (std::byte* target_addr = find_pattern(executable, pattern_before_virtual_lock))
    {
        // Find next call
        target_addr = find_pattern(executable, "\xff"sv);
        // Skip the call
        target_addr += 6;

        // Expected following instructions:
        // MOV EDX, dword ptr [g_DataBundleSize]
        // LEA RCX, [g_DataBundle]
        // CALL qword ptr[->KERNEL32.DLL::VirtualLock]
        //          which mean
        // VirtualLock(&g_DataBundle, g_DataBundleSize)
        //          equal to 19 bytes in 1.25.2
        // 8b 15 cd 19 16 00
        // 48 8d 0d 1e 92 38 dd
        // ff 15 c0 45 0d 00
        //          or more generally
        // 8b 15 ** ** ** **
        // 48 8d 0d ** ** ** **
        // ff 15 ** ** ** **

        // We just nop all of this

        logger.verbose("Found VirtualLock call in main, replacing with nop...");

        constexpr std::string_view new_code{
            "\x90"                             // 1 byte NOP
            "\x90"                             // 1 byte NOP
            "\x90"                             // 1 byte NOP
            "\x0F\x1F\x84\x00\x00\x00\x00\x00" // 8 byte NOP
            "\x0F\x1F\x84\x00\x00\x00\x00\x00" // 8 byte NOP
            ""sv
        };
        static_assert(new_code.size() == 19);
        std::memcpy(target_addr, new_code.data(), new_code.size());

        if (debug_int)
        {
            logger.verbose("Writing 'int3' instruction at the beginning of patched code in main...");
            *target_addr = std::byte{ 0xcc };
        }

        // Note: there is more code accessing the data bundle in main, but it appears to correctly handle a zero-size bundle
    }
    else
    {
        return Error{
            "Could not patch main function...",
            FAILED_PATCHING_MAIN_FUNCTION,
        };
    }

    return std::monostate{};
}

[[nodiscard]] expected<std::monostate> patch_bundle_init(Logger& logger,
                                                         std::span<IMAGE_SECTION_HEADER> image_section_headers,
                                                         std::span<std::byte> executable,
                                                         bool debug_int)
{
    using namespace std::string_view_literals;

    logger.verbose("Patching data bundle init function...");
    auto patch_bundle_init_popper = logger.push_segment();

    constexpr std::string_view pattern_in_function{ "\x48\xb8\x08\x00\x00\x00\x00\x00\x10\x00"sv };
    if (std::byte* const function_begin = find_pattern(executable, pattern_in_function))
    {
        // TODO: Should use exception handling data to find the range of this function ???

        // Find the last call in the function
        if (std::byte* function_end = find_pattern(std::span{ function_begin, 0x10000 }, "\xe9"sv))
        {
            // Skip the call
            function_end += 5;

            const IMAGE_SECTION_HEADER* text_section_header = algo::find(
                image_section_headers,
                &IMAGE_SECTION_HEADER::Name,
                std::basic_string_view<BYTE>{ (unsigned char*)".text" });
            const ptrdiff_t virtual_raw_offset{
                static_cast<ptrdiff_t>(text_section_header->VirtualAddress) - text_section_header->PointerToRawData
            };

            // Find MOV dword ptr [g_DataBundleSize], RAX
            std::byte* mov_data_bundle_size_addr{ find_pattern(std::span{ function_begin, 0x1000 }, "\x48\x89\x05"sv) };
            // Intermediary
            std::byte* const intermediary_addr{ find_pattern(std::span{ mov_data_bundle_size_addr + 1, 0x1000 }, "\x48\x89\x05"sv) };
            // Find MOV qword ptr [g_DataBundlePtr], RAX
            std::byte* const mov_data_bundle_ptr_addr{ find_pattern(std::span{ intermediary_addr + 1, 0x1000 }, "\x48\x89\x05"sv) };

            // Get instruction-relative offsets
            const uint32_t data_bundle_size_rel_offset{ *reinterpret_cast<uint32_t*>(mov_data_bundle_size_addr + 3) };
            const uint32_t data_bundle_ptr_rel_offset{ *reinterpret_cast<uint32_t*>(mov_data_bundle_ptr_addr + 3) };

            // Convert to RVAs
            const ptrdiff_t data_bundle_size_rva{
                (mov_data_bundle_size_addr + virtual_raw_offset + 7 + data_bundle_size_rel_offset) - executable.data()
            };
            const ptrdiff_t data_bundle_ptr_rva{
                (mov_data_bundle_ptr_addr + virtual_raw_offset + 7 + data_bundle_ptr_rel_offset) - executable.data()
            };

            // Data bundle always at beginning of .text section
            const ptrdiff_t data_bundle_rva{ text_section_header->VirtualAddress };

            // TODO: Do we need to set DAT_162dd97b8, DAT_162dd97a0?

            constexpr std::string_view new_code{
                "\x48\xc7\x05\xff\xff\xff\xff\x00\x00\x00\x00" // MOV qword ptr [g_DataBundleSize], 0x0
                "\x48\x8d\x05\xff\xff\xff\xff"                 // LEA RAX, [g_DataBundle]
                "\x48\x89\x05\xff\xff\xff\xff"                 // MOV qword ptr [g_DataBundlePtr], RAX
                "\xc3"                                         // RET
                ""sv
            };
            std::memset(function_begin, '\x90', function_end - function_begin);
            std::memcpy(function_begin, new_code.data(), new_code.size());

            // Get addresses of new instructions
            std::byte* new_mov_data_bundle_size_addr{ function_begin };
            std::byte* new_lea_data_bundle_addr{ new_mov_data_bundle_size_addr + 11 };
            std::byte* new_mov_data_bundle_ptr_addr{ new_lea_data_bundle_addr + 7 };

            // Translate RVAs back into instruction-relative offsets
            const uint32_t mew_data_bundle_size_rel_offset{
                static_cast<uint32_t>(executable.data() + data_bundle_size_rva - (new_mov_data_bundle_size_addr + virtual_raw_offset + 11))
            };
            const uint32_t mew_data_bundle_rel_offset{
                static_cast<uint32_t>(executable.data() + data_bundle_rva - (new_lea_data_bundle_addr + virtual_raw_offset + 7))
            };
            const uint32_t mew_data_bundle_ptr_rel_offset{
                static_cast<uint32_t>(executable.data() + data_bundle_ptr_rva - (new_mov_data_bundle_ptr_addr + virtual_raw_offset + 7))
            };

            // Write the offsets to memory
            *reinterpret_cast<uint32_t*>(new_mov_data_bundle_size_addr + 3) = mew_data_bundle_size_rel_offset;
            *reinterpret_cast<uint32_t*>(new_lea_data_bundle_addr + 3) = mew_data_bundle_rel_offset;
            *reinterpret_cast<uint32_t*>(new_mov_data_bundle_ptr_addr + 3) = mew_data_bundle_ptr_rel_offset;

            if (debug_int)
            {
                logger.verbose("Writing 'int3' instruction at the beginning of patched code in main...");
                function_begin[new_code.size() - 1] = std::byte{ 0xcc };
                function_begin[new_code.size()] = std::byte{ 0xc3 };
            }

            return std::monostate{};
        }
    }

    return Error{
        "Could not patch bundle init function...",
        FAILED_PATCHING_BUNDLE_INIT_FUNCTION,
    };
}

[[nodiscard]] expected<std::monostate> debug_verify([[maybe_unused]] Logger& logger,
                                                    [[maybe_unused]] std::span<IMAGE_SECTION_HEADER> image_section_headers,
                                                    [[maybe_unused]] std::span<std::byte> executable,
                                                    [[maybe_unused]] size_t relocation_offset)
{
#ifndef NDEBUG
    logger.verbose("Verifying that all data was moved...");
    auto debug_popper = logger.push_segment();

    const size_t last_section_offset{ image_section_headers.back().PointerToRawData };
    const size_t last_section_size{ image_section_headers.back().SizeOfRawData };
    std::byte* const last_section_start{ executable.data() + last_section_offset };
    std::byte* const last_section_end{ last_section_start + last_section_size };
    std::byte* const exe_end{ executable.data() + executable.size() };
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

    if (leftover_size > 0 && !memzcmp(last_section_end, leftover_size))
    {
        logger.warning("Not all data past the relocated executable is nulled out...");
    }
#endif

    return std::monostate{};
}

void resize_executable(Logger& logger,
                       std::span<IMAGE_SECTION_HEADER> image_section_headers,
                       std::vector<std::byte>& executable)
{
    logger.verbose("Resizing binary...");
    auto resize_popper = logger.push_segment();

    const size_t last_section_offset{ image_section_headers.back().PointerToRawData };
    const size_t last_section_size{ image_section_headers.back().SizeOfRawData };
    const size_t total_binary_size{ last_section_offset + last_section_size };

    logger.verbose("Old size: {:#x}", executable.size());
    logger.verbose("New size: {:#x}", total_binary_size);
    logger.verbose("   Saved: {:#x}", executable.size() - total_binary_size);

    executable.resize(total_binary_size);
}

void write_binary(Logger& logger,
                  std::span<std::byte> executable,
                  const std::filesystem::path& out_path)
{
    namespace fs = std::filesystem;

    logger.verbose("Writing out new executable to '{}'...", out_path.string());
    auto write_popper = logger.push_segment();

    if (fs::exists(out_path))
    {
        logger.warning("Path '{}' already exists, press any key to override and continue...", out_path.string());
        std::cin.get();
        if (fs::exists(out_path))
        {
            fs::remove(out_path);
        }
    }

    WriteWholeFile(out_path, executable);
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
                magic_enum::enum_name(static_cast<ExitReason>(options.exit_reason.value())));
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

        const fs::path exe_path{ as_value(get_exe_path(options)) };
        std::vector<std::byte> exe_data{ as_value(load_exe(logger, exe_path)) };

        logger.verbose("Begin patching image...\n");

        PortableExecutableHeaders headers{ as_value(get_headers(logger, exe_data.data())) };

        BundleExtractionResult bundle_extraction_result{
            as_value(extract_bundle(logger,
                                    headers.image_optional_header,
                                    headers.image_section_headers,
                                    exe_data.data(),
                                    options.debug_do_not_relocated.value_or(false)))
        };

        patch_absolute_addresses(logger,
                                 headers.image_section_headers,
                                 bundle_extraction_result.relocation_offset,
                                 exe_data.data());

        (void)as_value(patch_data_directory(logger,
                                            headers.image_optional_header,
                                            headers.image_section_headers,
                                            bundle_extraction_result.text_section_start,
                                            bundle_extraction_result.relocation_offset,
                                            exe_data.data()));

        (void)as_value(patch_main(logger,
                                  exe_data,
                                  options.insert_debug_interrupts.value_or(false)));

        (void)as_value(patch_bundle_init(logger,
                                         headers.image_section_headers,
                                         exe_data,
                                         options.insert_debug_interrupts.value_or(false)));

        (void)as_value(debug_verify(logger,
                                    headers.image_section_headers,
                                    exe_data,
                                    bundle_extraction_result.relocation_offset));

        resize_executable(logger, headers.image_section_headers, exe_data);

        const fs::path out_path{
            exe_path.parent_path() / options.output_name.value()
        };
        write_binary(logger, exe_data, out_path);

        // TODO: Find absolute addresses in binary and patch those
        // TODO: Check which image dos header members need to change (?page size?)
        // TODO: Patch function reading assets (https://gist.github.com/iojon/0bb1e95bad8b601025bfac14e07d8b25)
    }
    catch (const RipException& e)
    {
        Logger{}.error("{}", e.what());
        return e.why();
    }
    catch (const structopt::exception& e)
    {
        Logger{}.error("{}", e.what());
        fmt::print("{}\n", e.help());
        return FAILED_PARSING_COMMAND_LINE;
    }
    catch (const std::exception& e)
    {
        Logger{}.error("{}", e.what());
        return FAILED_UNCAUGHT_EXCEPTION;
    }

    return SUCCESS;
}
