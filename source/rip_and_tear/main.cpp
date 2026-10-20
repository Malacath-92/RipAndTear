#include "asset_extractor.hpp"
#include "unwind_info.h"
#include "util/algorithms.h"
#include "util/error.h"
#include "util/file.h"
#include "util/format.h"
#include "util/on_scope_exit.h"
#include "util/span_util.h"

#include <Windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <imagehlp.h>
#include <iostream>
#include <numeric>
#include <span>

#include <structopt/app.hpp>

#include <magic_enum.hpp>

struct CommandLineOptions
{
    std::optional<std::string> exe_dir;
    std::optional<std::string> output_name{ "Mick2.exe" };
    std::optional<bool> insert_debug_interrupts{ false };
    std::optional<bool> debug_do_not_relocate{ false };
    std::optional<bool> custom_code_patches{ false };
    std::optional<bool> extract_assets{ false };
    std::optional<int32_t> exit_reason;
    std::optional<bool> verbose{ false };
};
VISITABLE_STRUCT(CommandLineOptions,
                 exe_dir,
                 output_name,
                 insert_debug_interrupts,
                 debug_do_not_relocate,
                 custom_code_patches,
                 extract_assets,
                 exit_reason,
                 verbose);

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
    void verbose(fmt::format_string<Args...> fmt, Args&&... args)
    {
        if (is_verbose)
        {
            fmt::print("{:\t>{}}", "", segment_depth);
            fmt::print(fmt, std::forward<Args>(args)...);
            fmt::print("\n");
        }
    }
    template<class... Args>
    void warning(fmt::format_string<Args...> fmt, Args&&... args)
    {
        fmt::print(fmt::fg(fmt::color::yellow), "{:\t>{}}", "Warning: ", segment_depth);
        fmt::print(fmt::fg(fmt::color::yellow), fmt, std::forward<Args>(args)...);
        fmt::print("\n");
    }
    template<class... Args>
    void error(fmt::format_string<Args...> fmt, Args&&... args)
    {
        fmt::print(fmt::fg(fmt::color::red), "{:\t>{}}", "Error: ", segment_depth);
        fmt::print(fmt::fg(fmt::color::red), fmt, std::forward<Args>(args)...);
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

[[nodiscard]] const IMAGE_SECTION_HEADER* find_section_for_rva(
    std::span<const IMAGE_SECTION_HEADER> image_section_headers,
    size_t virtual_address)
{
    for (const IMAGE_SECTION_HEADER& header : image_section_headers)
    {
        if (header.VirtualAddress <= virtual_address &&
            virtual_address < header.VirtualAddress + header.Misc.VirtualSize)
        {
            return &header;
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
    size_t relocation_offset;
    std::vector<std::byte> bundle_data;
};
expected<BundleExtractionResult> extract_bundle(Logger logger,
                                                IMAGE_OPTIONAL_HEADER& image_optional_header,
                                                IMAGE_FILE_HEADER& image_file_header,
                                                std::span<IMAGE_SECTION_HEADER>& image_section_headers,
                                                std::byte* executable,
                                                bool debug_do_not_relocate)
{
    using namespace std::string_view_literals;

    logger.verbose("Begin patching headers and relocating sections...");
    auto image_headers_popper = logger.push_segment();

    size_t relocation_offset{};
    std::vector<std::byte> bundle_data{};
    bool text_section_found{};
    bool text_virtual_size_fixed{};
    DWORD text_section_virtual_address{};

    // Remove the embedded bundle from the file while preserving every RVA.
    //
    // The bundle lives at the start of .text raw data, followed by the actual
    // code. We copy the bundle out, then move the raw bytes left by
    // relocation_offset. To keep the code at its original RVA, .text's
    // VirtualAddress is moved right by exactly the same amount. All following
    // sections keep their VirtualAddress and only their PointerToRawData moves.
    // That leaves every RVA (and every absolute VA) untouched, so no .pdata,
    // FuncInfo, import, relocation, TLS, or load-config metadata needs rebasing.
    for (IMAGE_SECTION_HEADER& header : image_section_headers)
    {
        if (!text_section_found && (const char*)header.Name == ".text"sv)
        {
            text_section_found = true;
            logger.verbose("Found .text section...");
            auto text_header_popper = logger.push_segment();
            logger.verbose("Extracting data bundle...");

            // Compute bundle size
            const size_t bundle_data_offset{ header.PointerToRawData };
            std::byte* const bundle_data_start{ executable + bundle_data_offset };
            const size_t bundle_size{ get_data_bunde_size(bundle_data_start) };

            if (debug_do_not_relocate)
            {
                relocation_offset = 0;
            }
            else
            {
                // Round the removal offset DOWN to a multiple of both section and
                // file alignment. It must not be rounded up: the bundle is
                // immediately followed by live code, and removing even one byte
                // more than necessary would chop the first function's prologue.
                // SectionAlignment is normally a multiple of FileAlignment, but
                // using lcm keeps every section's PointerToRawData aligned even if
                // that assumption does not hold.
                const size_t section_alignment{ static_cast<size_t>(image_optional_header.SectionAlignment) };
                const size_t file_alignment{ static_cast<size_t>(image_optional_header.FileAlignment) };
                const size_t alignment{ std::lcm(section_alignment, file_alignment) };
                relocation_offset = (bundle_size / alignment) * alignment;
            }

            logger.verbose("Found data bundle of size {:#x}...", bundle_size);
            logger.verbose("Aligning to {:#x} bytes, resulting in {:#x}...", image_optional_header.FileAlignment, relocation_offset);

            if (relocation_offset > bundle_size)
            {
                return Error{
                    "Removal offset overlaps the first code function...",
                    FAILED_UNEXPECTED_TEXT_SECTION,
                };
            }
            if (relocation_offset > header.SizeOfRawData)
            {
                return Error{
                    "Removal offset is larger than the .text raw data...",
                    FAILED_UNEXPECTED_TEXT_SECTION,
                };
            }

            // Move bundle into its own data
            bundle_data.resize(bundle_size);
            std::memmove(bundle_data.data(), bundle_data_start, bundle_size);
            std::memset(bundle_data_start, 0, bundle_size);

            logger.verbose("Extracted data bundle...");
            logger.verbose("Relocating {} section...", (const char*)header.Name);

            // Move the raw data left. The source may be inside the now-zeroed
            // bundle region, which is fine: those bytes become leading zero
            // padding in .text. It must never be inside live code, which is
            // guaranteed by relocation_offset <= bundle_size.
            const size_t remaining_text_size{ header.SizeOfRawData - relocation_offset };
            header.SizeOfRawData = static_cast<DWORD>(remaining_text_size);
            std::memmove(bundle_data_start, bundle_data_start + relocation_offset, remaining_text_size);
            std::memset(bundle_data_start + remaining_text_size, 0, relocation_offset);

            // Keep the code's virtual addresses unchanged. The code moved left
            // by relocation_offset in the file, so the section base moves right
            // by relocation_offset in the virtual address space.
            text_section_virtual_address = header.VirtualAddress;
            header.VirtualAddress = static_cast<DWORD>(header.VirtualAddress + relocation_offset);

            logger.verbose("Fixing image optional header...");

            // SizeOfImage is unchanged: the highest section still ends at the
            // same VA. The removed bundle leaves a gap before .text; that gap
            // is covered by an uninitialized .gap section below so the Windows
            // loader accepts the image. AddressOfEntryPoint is an RVA and is
            // also unchanged.
            if (image_optional_header.SizeOfCode < relocation_offset)
            {
                return Error{
                    "Removal offset is larger than SizeOfCode...",
                    FAILED_UNEXPECTED_TEXT_SECTION,
                };
            }
            image_optional_header.SizeOfCode = static_cast<DWORD>(image_optional_header.SizeOfCode - relocation_offset);
            image_optional_header.BaseOfCode = header.VirtualAddress;
        }
        else if (text_section_found)
        {
            logger.verbose("Found {} section...", (const char*)header.Name);
            auto section_header_popper = logger.push_segment();
            logger.verbose("Relocating section raw data...");

            // Move only the raw bytes. The VirtualAddress stays put, so all
            // RVAs that point into this section remain valid.
            if (header.PointerToRawData < relocation_offset)
            {
                return Error{
                    "Section raw data starts before the moved data...",
                    FAILED_UNEXPECTED_TEXT_SECTION,
                };
            }

            const size_t section_offset{ header.PointerToRawData };
            std::byte* const section_start{ executable + section_offset };
            std::byte* const section_end{ executable + section_offset + header.SizeOfRawData };
            header.PointerToRawData = static_cast<DWORD>(section_offset - relocation_offset);
            std::memmove(section_start - relocation_offset, section_start, header.SizeOfRawData);
            std::byte* const new_section_end{ section_start - relocation_offset + header.SizeOfRawData };
            std::byte* const zero_memory_begin{ std::max(section_start, new_section_end) };
            const size_t zero_memory_size{ static_cast<size_t>(section_end - zero_memory_begin) };
            std::memset(zero_memory_begin, 0, zero_memory_size);

            // VirtualSize must span the full virtual extent of .text, including
            // any inter-section padding. Its VirtualAddress has already been
            // moved forward, so the distance to the next section is the correct
            // new size. We only do this once (first section after .text).
            IMAGE_SECTION_HEADER& text_header{ image_section_headers.data()[0] };
            if (!text_virtual_size_fixed)
            {
                text_header.Misc.VirtualSize = header.VirtualAddress - text_header.VirtualAddress;
                logger.verbose("Fixed .text VirtualSize to {:#x}...", text_header.Misc.VirtualSize);
                text_virtual_size_fixed = true;
            }
        }
        else
        {
            return Error{
                ".text section is not the first section...",
                FAILED_UNEXPECTED_TEXT_SECTION,
            };
        }
    }

    if (!text_section_found)
    {
        return Error{
            "Could not find .text section...",
            FAILED_NO_TEXT_SECTION_FOUND,
        };
    }

    if (relocation_offset != 0)
    {
        // The change leaves the removed bundle's RVA range uncovered. The
        // Windows loader rejects an image with an unbacked gap between the
        // headers and the first section, so add a zero-filled .gap section
        // covering exactly that range. It has no file data, so the executable
        // stays small; the loader maps it as demand-zero virtual memory.
        const size_t section_table_offset{
            static_cast<size_t>(reinterpret_cast<const std::byte*>(image_section_headers.data()) - executable)
        };
        const size_t section_count{ image_section_headers.size() };
        const size_t new_section_table_end{
            section_table_offset + (section_count + 1) * sizeof(IMAGE_SECTION_HEADER)
        };
        if (new_section_table_end > image_optional_header.SizeOfHeaders)
        {
            return Error{
                "No room in the PE headers for the .gap section...",
                FAILED_UNEXPECTED_TEXT_SECTION,
            };
        }

        std::memmove(image_section_headers.data() + 1,
                     image_section_headers.data(),
                     section_count * sizeof(IMAGE_SECTION_HEADER));

        IMAGE_SECTION_HEADER& gap_header{ image_section_headers.data()[0] };
        std::memset(&gap_header, 0, sizeof(IMAGE_SECTION_HEADER));
        std::memcpy(gap_header.Name, ".gap", 4);
        gap_header.Misc.VirtualSize = static_cast<DWORD>(relocation_offset);
        gap_header.VirtualAddress = text_section_virtual_address;
        gap_header.Characteristics = IMAGE_SCN_CNT_UNINITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;

        image_section_headers = std::span<IMAGE_SECTION_HEADER>{
            image_section_headers.data(),
            section_count + 1,
        };
        ++image_file_header.NumberOfSections;
        image_optional_header.SizeOfUninitializedData += static_cast<DWORD>(relocation_offset);

        logger.verbose("Added .gap section at RVA {:#x}, size {:#x}...",
                       gap_header.VirtualAddress,
                       gap_header.Misc.VirtualSize);
    }

    return BundleExtractionResult{
        relocation_offset,
        std::move(bundle_data),
    };
}

[[nodiscard]] expected<std::monostate> patch_data_directory(Logger& logger,
                                                            IMAGE_OPTIONAL_HEADER& image_optional_header,
                                                            std::span<IMAGE_SECTION_HEADER> image_section_headers,
                                                            size_t relocation_offset,
                                                            std::byte* executable)
{
    logger.verbose("Patching file-offset data directories...");
    auto data_directory_popper{ logger.push_segment() };

    // The change preserves every RVA and every absolute VA. The only data
    // directory whose payload stores a raw file offset is the debug directory:
    // AddressOfRawData is an RVA (unchanged), while PointerToRawData is a file
    // offset into a section whose raw data was moved left by relocation_offset.
    const IMAGE_DATA_DIRECTORY& debug_data_directory{
        image_optional_header.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG]
    };
    if (debug_data_directory.VirtualAddress == 0 || debug_data_directory.Size == 0)
    {
        return std::monostate{};
    }

    const IMAGE_SECTION_HEADER* debug_section{
        find_section_for_rva(image_section_headers, debug_data_directory.VirtualAddress)
    };
    if (debug_section == nullptr)
    {
        return Error{
            "Debug data directory is not contained within a section...",
            FAILED_MYSTERY_DATA_DIRECTORY,
        };
    }

    const ptrdiff_t virtual_raw_offset{
        static_cast<ptrdiff_t>(debug_section->PointerToRawData) - debug_section->VirtualAddress
    };
    std::byte* const entry_start{
        executable + debug_data_directory.VirtualAddress + virtual_raw_offset
    };

    const size_t num_debug_entries{ debug_data_directory.Size / sizeof(IMAGE_DEBUG_DIRECTORY) };
    std::span<IMAGE_DEBUG_DIRECTORY> debug_directories{
        get_ptr<IMAGE_DEBUG_DIRECTORY>(entry_start),
        num_debug_entries,
    };

    for (IMAGE_DEBUG_DIRECTORY& debug_directory : debug_directories)
    {
        if (debug_directory.PointerToRawData != 0)
        {
            if (debug_directory.PointerToRawData < relocation_offset)
            {
                return Error{
                    "Debug raw data pointer is before the moved data...",
                    FAILED_MYSTERY_DATA_DIRECTORY,
                };
            }

            // AddressOfRawData is an RVA and deliberately stays unchanged.
            debug_directory.PointerToRawData = static_cast<DWORD>(
                debug_directory.PointerToRawData - relocation_offset);
        }

        if (debug_directory.Type == IMAGE_DEBUG_TYPE_CODEVIEW &&
            debug_directory.PointerToRawData != 0)
        {
#pragma warning(push)
#pragma warning(disable : 4200)
            struct IMAGE_DEBUG_CODEVIEW_DIRECTORY
            {
                char Signature[0x4];
                BYTE Guid[0x10];
                DWORD Age;
                char PdbName[];
            };
#pragma warning(pop)
            IMAGE_DEBUG_CODEVIEW_DIRECTORY& codeview_directory{
                get_ref<IMAGE_DEBUG_CODEVIEW_DIRECTORY>(executable + debug_directory.PointerToRawData)
            };
            logger.verbose("codeview signature: {}", std::string_view{ codeview_directory.Signature, 4 });
            logger.verbose("codeview pdb path: {}", codeview_directory.PdbName);
        }
        else
        {
            logger.verbose("Skipping debug entry of type {}...", debug_directory.Type);
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
        // The sequence we want to NOP is immediately after the anchor:
        // MOV EDX, dword ptr [g_DataBundleSize]   8B 15 ** ** ** **   (6 bytes)
        // LEA RCX, [g_DataBundle]                 48 8D 0D ** ** ** ** (7 bytes)
        // CALL qword ptr [->VirtualLock]           FF 15 ** ** ** **   (6 bytes)
        // Total: 19 bytes
        //
        // Search for the MOV EDX (8B 15) that immediately follows the anchor.
        const std::span<std::byte> after_anchor{ target_addr, static_cast<size_t>(executable.data() + executable.size() - target_addr) };
        target_addr = find_pattern(after_anchor, "\x8B\x15"sv);
        if (target_addr == nullptr)
        {
            return Error{
                "Could not patch main function (MOV EDX pattern not found)...",
                FAILED_PATCHING_MAIN_FUNCTION,
            };
        }

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
            const ptrdiff_t raw_to_rva_offset{
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
                (mov_data_bundle_size_addr + raw_to_rva_offset + 7 + data_bundle_size_rel_offset) - executable.data()
            };
            const ptrdiff_t data_bundle_ptr_rva{
                (mov_data_bundle_ptr_addr + raw_to_rva_offset + 7 + data_bundle_ptr_rel_offset) - executable.data()
            };

            // TODO: Do we need to set DAT_162dd97b8, DAT_162dd97a0?

            constexpr std::string_view new_code{
                "\x48\xc7\x05\xff\xff\xff\xff\x00\x00\x00\x00" // MOV qword ptr [g_DataBundleSize], 0x0
                "\x48\x8d\x05\xff\xff\xff\xff"                 // LEA RAX, [g_DataBundle]
                "\x48\x89\x05\xff\xff\xff\xff"                 // MOV qword ptr [g_DataBundlePtr], RAX
                "\x48\x83\xc4\x30"                             // ADD RSP, 0x30
                "\x5d"                                         // POP RBP
                "\xc3"                                         // RET
                ""sv
            };
            constexpr size_t empty_bundle_offset{ new_code.size() + 16 };
            if (static_cast<size_t>(function_end - function_begin) < empty_bundle_offset + 8)
            {
                return Error{
                    "Not enough space for empty bundle sentinel...",
                    FAILED_PATCHING_BUNDLE_INIT_FUNCTION,
                };
            }

            std::memset(function_begin, '\x90', function_end - function_begin);
            std::memcpy(function_begin, new_code.data(), new_code.size());
            std::memset(function_begin + empty_bundle_offset, 0, 8);

            // Get addresses of new instructions
            std::byte* new_mov_data_bundle_size_addr{ function_begin };
            std::byte* new_lea_data_bundle_addr{ new_mov_data_bundle_size_addr + 11 };
            std::byte* new_mov_data_bundle_ptr_addr{ new_lea_data_bundle_addr + 7 };

            // Translate RVAs back into instruction-relative offsets
            const uint32_t mew_data_bundle_size_rel_offset{
                static_cast<uint32_t>(executable.data() + data_bundle_size_rva - (new_mov_data_bundle_size_addr + raw_to_rva_offset + 11))
            };
            const uint32_t mew_data_bundle_rel_offset{
                static_cast<uint32_t>(function_begin + raw_to_rva_offset + empty_bundle_offset -
                                      (new_lea_data_bundle_addr + raw_to_rva_offset + 7))
            };
            const uint32_t mew_data_bundle_ptr_rel_offset{
                static_cast<uint32_t>(executable.data() + data_bundle_ptr_rva - (new_mov_data_bundle_ptr_addr + raw_to_rva_offset + 7))
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

[[nodiscard]] expected<std::monostate> patch_bundle_checksum(Logger& logger,
                                                             std::span<std::byte> executable)
{
    using namespace std::string_view_literals;

    logger.verbose("Patching v1.28 data bundle checksum check...");
    auto patch_bundle_checksum_popper = logger.push_segment();

    // v1.28 starts a worker thread which validates a ChaCha20 checksum of the
    // encrypted data bundle. RipAndTear removes that bundle and makes the
    // bundle init function create an empty one, so the validator always sees a
    // mismatch and calls _Exit(0) before the first frame is drawn.
    //
    //     movabs rax, 0x6f018167ea9cd34a
    //     cmp    rcx, rax
    //     jne    <failure>
    //
    // NOP the conditional branch so execution always takes the success path.
    constexpr std::string_view anchor{
        "\x48\xb8\x4a\xd3\x9c\xea\x67\x81\x01\x6f\x48\x39\xc1\x75\x29"sv
    };
    std::byte* const checksum_check{ find_pattern(executable, anchor) };
    if (checksum_check == nullptr)
    {
        return Error{
            "Could not find data bundle checksum check...",
            FAILED_PATCHING_BUNDLE_CHECKSUM,
        };
    }

    const size_t remaining_size{
        static_cast<size_t>(executable.data() + executable.size() - checksum_check)
    };
    if (remaining_size < anchor.size() ||
        find_pattern(std::span<std::byte>{ checksum_check + 1, remaining_size - 1 },
                     anchor) != nullptr)
    {
        return Error{
            "Data bundle checksum check signature is ambiguous or malformed...",
            FAILED_PATCHING_BUNDLE_CHECKSUM,
        };
    }

    std::memset(checksum_check + 13, 0x90, 2);
    logger.verbose("Neutralized v1.28 data bundle checksum validation...");
    return std::monostate{};
}

[[nodiscard]] expected<std::monostate> patch_asset_reader(Logger& logger,
                                                          IMAGE_OPTIONAL_HEADER& image_optional_header,
                                                          std::span<IMAGE_SECTION_HEADER> image_section_headers,
                                                          std::span<std::byte> executable)
{
    using namespace std::string_view_literals;

    logger.verbose("Patching asset reader function...");
    auto patch_asset_reader_popper = logger.push_segment();

    // -------------------------------------------------------------------------
    // Step 1: find openEncryptedFile
    //
    // Strategy derived from Overlunky's search for "read_encrypted_file":
    //   1. Find `mov r8d, 0x4650`  (41 B8 50 46 00 00) after the bundle.
    //      This immediate appears at the call site of openEncryptedFile.
    //   2. Find the first `call` (E8) at or after that instruction.
    //   3. Find the *next* `call` (E8) after that — this is the call to
    //      openEncryptedFile.
    //   4. Decode the near-call relative offset to get the function address:
    //      target = call_site + 5 + *(int32_t*)(call_site + 1)
    // -------------------------------------------------------------------------
    constexpr std::string_view anchor{ "\x41\xB8\x50\x46\x00\x00"sv };

    std::byte* anchor_match{ find_pattern(executable, anchor) };
    if (anchor_match == nullptr)
    {
        return Error{
            "Could not find openEncryptedFile anchor (mov r8d, 0x4650 not found)...",
            FAILED_ASSET_READER_FUNCTION_NOT_FOUND,
        };
    }

    // Find first call after the anchor
    const std::span<std::byte> after_anchor{ anchor_match, static_cast<size_t>(executable.data() + executable.size() - anchor_match) };
    std::byte* first_call{ find_pattern(after_anchor, "\xE8"sv) };
    if (first_call == nullptr)
    {
        return Error{
            "Could not find openEncryptedFile anchor (first call not found)...",
            FAILED_ASSET_READER_FUNCTION_NOT_FOUND,
        };
    }

    // Find second call (skip past the first one)
    const std::span<std::byte> after_first_call{ first_call + 1, static_cast<size_t>(executable.data() + executable.size() - first_call - 1) };
    std::byte* second_call{ find_pattern(after_first_call, "\xE8"sv) };
    if (second_call == nullptr)
    {
        return Error{
            "Could not find openEncryptedFile anchor (second call not found)...",
            FAILED_ASSET_READER_FUNCTION_NOT_FOUND,
        };
    }

    // Decode the near-call: target_file_offset = call_site + 5 + rel32
    // The call instruction is in the file buffer; we need to convert its
    // file offset to an RVA first so the arithmetic works in address space.
    const IMAGE_SECTION_HEADER* text_section_header = algo::find(
        image_section_headers,
        &IMAGE_SECTION_HEADER::Name,
        std::basic_string_view<BYTE>{ (unsigned char*)".text" });

    const ptrdiff_t text_virtual_raw_offset{
        static_cast<ptrdiff_t>(text_section_header->PointerToRawData) - text_section_header->VirtualAddress
    };

    const size_t call_site_file_offset{ static_cast<size_t>(second_call - executable.data()) };
    const size_t call_site_rva{ call_site_file_offset - static_cast<size_t>(text_virtual_raw_offset) };
    const int32_t rel32{ *reinterpret_cast<int32_t*>(second_call + 1) };
    const size_t function_rva{ static_cast<size_t>(static_cast<int64_t>(call_site_rva) + 5 + rel32) };
    const size_t function_file_offset{ function_rva + static_cast<size_t>(text_virtual_raw_offset) };
    std::byte* function_begin{ executable.data() + function_file_offset };

    logger.verbose("Found openEncryptedFile at file offset {:#x}, RVA {:#x}, VA {:#x}...",
                   function_file_offset,
                   function_rva,
                   image_optional_header.ImageBase + function_rva);

    // -------------------------------------------------------------------------
    // Step 3: locate IAT slots for the five CRT functions
    //
    // Order matches the data section in the blob:
    //   ftell[0], fseek[1], fread[2], fopen[3], fclose[4]
    // -------------------------------------------------------------------------
    constexpr std::array<std::string_view, 6> required_functions{ "ftell", "fseek", "fread", "fopen", "fclose", "malloc" };
    std::array<uint64_t, 6> iat_vas{};
    std::array<bool, 6> iat_found{};

    const IMAGE_DATA_DIRECTORY& import_dir{ image_optional_header.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT] };
    const ptrdiff_t import_virtual_raw_offset = [&]() -> ptrdiff_t
    {
        for (const IMAGE_SECTION_HEADER& sec : image_section_headers)
        {
            if (sec.VirtualAddress <= import_dir.VirtualAddress &&
                sec.VirtualAddress + sec.Misc.VirtualSize > import_dir.VirtualAddress)
            {
                return static_cast<ptrdiff_t>(sec.PointerToRawData) - sec.VirtualAddress;
            }
        }
        return 0;
    }();

    std::span<IMAGE_IMPORT_DESCRIPTOR> import_desc_table{
        get_nullterminated_table<IMAGE_IMPORT_DESCRIPTOR>(
            executable.data() + import_dir.VirtualAddress + import_virtual_raw_offset)
    };

    for (const IMAGE_IMPORT_DESCRIPTOR& desc : import_desc_table)
    {
        const ptrdiff_t hint_table_raw{ desc.OriginalFirstThunk + import_virtual_raw_offset };
        std::span<IMAGE_THUNK_DATA> hint_table{
            get_nullterminated_table<IMAGE_THUNK_DATA>(executable.data() + hint_table_raw)
        };

        for (size_t i = 0; i < hint_table.size(); ++i)
        {
            using THUNK_T = decltype(decltype(IMAGE_THUNK_DATA::u1)::AddressOfData);
            constexpr THUNK_T ordinal_bit{ THUNK_T(1) << (sizeof(THUNK_T) * 8 - 1) };
            if (hint_table[i].u1.AddressOfData & ordinal_bit)
                continue;

            const IMAGE_IMPORT_BY_NAME* name_entry{
                reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                    executable.data() + hint_table[i].u1.AddressOfData + import_virtual_raw_offset)
            };
            const std::string_view fn_name{ name_entry->Name };

            for (size_t j = 0; j < required_functions.size(); ++j)
            {
                if (fn_name == required_functions[j])
                {
                    // VA of IAT slot = ImageBase + FirstThunk_RVA + index * 8
                    iat_vas[j] = image_optional_header.ImageBase + desc.FirstThunk + i * sizeof(uint64_t);
                    iat_found[j] = true;
                    logger.verbose("Found IAT slot for '{}': VA {:#x}...", fn_name, iat_vas[j]);
                }
            }
        }
    }

    for (size_t i = 0; i < required_functions.size(); ++i)
    {
        if (!iat_found[i])
        {
            return Error{
                fmt::format("Could not find IAT slot for '{}'...", required_functions[i]),
                FAILED_ASSET_READER_IAT_FUNCTION_NOT_FOUND,
            };
        }
    }

    // -------------------------------------------------------------------------
    // Step 4: write the replacement function blob
    //
    // Blob layout (320 bytes total):
    //   [0..0x103]  function body ending with ret
    //   [0x104..0x106] "rb\0" mode string
    //   [0x107..0x10F] padding to 16-byte align the slot table
    //   [0x110..0x13F] 6 x uint64 IAT offset slots: ftell, fseek, fread, fopen, fclose, malloc
    //
    // Each slot stores: IAT_slot_RVA - function_RVA, a base-independent displacement.
    // -------------------------------------------------------------------------
    // clang-format off
    constexpr std::string_view blob{
        "\x53\x55\x41\x54\x48\x83\xec\x20\x48\x8d\x15\xf5\x00\x00\x00\x48" // 000 push rbx/rbp/r12; sub rsp,0x20; lea rdx,[rip+mode]
        "\x8d\x05\xea\xff\xff\xff\x48\x03\x05\x0b\x01\x00\x00\xff\x10\x48" // 010 lea rax,[rip-func]; add rax,[fopen]; call [rax]; mov rbp,rax
        "\x89\xc5\x48\x85\xed\x74\xd2\x48\x89\xe9\x31\xd2\x41\xb8\x02\x00" // 020 test rbp,rbp; jz fail; mov rcx,rbp; xor edx,edx; mov r8d,2
        "\x00\x00\x48\x8d\x05\xc7\xff\xff\xff\x48\x03\x05\xd8\x00\x00\x00" // 030 lea rax,[rip-func]; add rax,[fseek]; call [rax]
        "\xff\x10\x48\x89\xe9\x48\x8d\x05\xb4\xff\xff\xff\x48\x03\x05\xbd" // 040 mov rcx,rbp; lea rax,[rip-func]; add rax,[ftell]
        "\x00\x00\x00\xff\x10\x49\x89\xc4\x48\x89\xe9\x31\xd2\x45\x31\xc0" // 050 call [rax]; mov r12,rax; mov rcx,rbp; xor edx,edx; xor r8d,r8d
        "\x48\x8d\x05\x99\xff\xff\xff\x48\x03\x05\xaa\x00\x00\x00\xff\x10" // 060 lea rax,[rip-func]; add rax,[fseek]; call [rax]
        "\x49\x8d\x4c\x24\x20\x48\x8d\x05\x84\xff\xff\xff\x48\x03\x05\xb5" // 070 lea rcx,[r12+0x20]; lea rax,[rip-func]; add rax,[malloc]
        "\x00\x00\x00\xff\x10\x48\x89\xc3\x48\x85\xdb\x74\x59\x48\x8d\x4b" // 080 call [rax]; mov rbx,rax; test rbx,rbx; jz close_fail; lea rcx,[rbx+0x20]
        "\x20\xba\x01\x00\x00\x00\x4d\x89\xe0\x49\x89\xe9\x48\x8d\x05\x5d" // 090 mov edx,1; mov r8,r12; mov r9,rbp; lea rax,[rip-func]
        "\xff\xff\xff\x48\x03\x05\x76\x00\x00\x00\xff\x10\x48\x89\xe9\x48" // 0a0 add rax,[fread]; call [rax]; mov rcx,rbp; lea rax,[rip-func]
        "\x8d\x05\x4a\xff\xff\xff\x48\x03\x05\x73\x00\x00\x00\xff\x10\x48" // 0b0 add rax,[fclose]; call [rax]; mov rax,rbx
        "\x89\xd8\x48\x8d\x48\x20\x48\x89\x08\xc7\x40\x08\x00\x00\x00\x00" // 0c0 lea rcx,[rax+0x20]; mov [rax],rcx; mov dword [rax+8],0
        "\x44\x89\x60\x0c\x49\x8d\x4c\x24\x20\x48\x89\x48\x10\x48\x83\xc4" // 0d0 mov [rax+0xc],r12d; lea rcx,[r12+0x20]; mov [rax+0x10],rcx
        "\x20\x41\x5c\x5d\x5b\xc3\x48\x89\xe9\x48\x8d\x05\x10\xff\xff\xff" // 0e0 add rsp,0x20; pop r12/rbp/rbx; ret; close_fail: mov rcx,rbp; lea rax,[rip-func]
        "\x48\x03\x05\x39\x00\x00\x00\xff\x10\x31\xc0\x48\x83\xc4\x20\x41" // 0f0 add rax,[fclose]; call [rax]; fail: xor eax,eax; add rsp,0x20
        "\x5c\x5d\x5b\xc3\x72\x62\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" // 100 pop r12/rbp/rbx; ret; "rb"; padding
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" // 110 ftell slot, fseek slot
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" // 120 fread slot, fopen slot
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" // 130 fclose slot, malloc slot
        ""sv
    };
    // clang-format on
    static_assert(blob.size() == 320);

    // Copy blob into a mutable buffer and fill in the IAT slots.
    std::vector<std::byte> patched_blob(blob.size());
    std::memcpy(patched_blob.data(), blob.data(), blob.size());

    // The code adds each slot value to the runtime VA of this function, so the
    // preferred image base must cancel out of the displacement.
    // Order: ftell[0], fseek[1], fread[2], fopen[3], fclose[4], malloc[5]
    constexpr size_t slot_base_offset{ 320 - required_functions.size() * sizeof(uint64_t) };
    for (size_t i = 0; i < required_functions.size(); ++i)
    {
        const uint64_t slot_value{ iat_vas[i] - image_optional_header.ImageBase - function_rva };
        logger.verbose("IAT slot [{}] '{}': {:#x}...", i, required_functions[i], slot_value);
        std::memcpy(patched_blob.data() + slot_base_offset + i * sizeof(uint64_t),
                    &slot_value,
                    sizeof(uint64_t));
    }

    std::memcpy(function_begin, patched_blob.data(), patched_blob.size());

    // -------------------------------------------------------------------------
    // Step 5: replace the original unwind information.
    //
    // The original function and the replacement have completely different
    // prologues. The preserves the .pdata entry, but that entry still
    // describes the old function. Rewrite both its range and its unwind data.
    // -------------------------------------------------------------------------
    constexpr size_t replacement_code_size{ 0x104 };
    static_assert(blob.size() > replacement_code_size);

    const IMAGE_DATA_DIRECTORY& exception_dir{
        image_optional_header.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION]
    };
    if (exception_dir.VirtualAddress == 0 || exception_dir.Size == 0)
    {
        return Error{
            "Exception data directory is missing...",
            FAILED_ASSET_READER_UNWIND_INFO,
        };
    }

    const IMAGE_SECTION_HEADER* pdata_section{
        find_section_for_rva(image_section_headers, exception_dir.VirtualAddress)
    };
    if (pdata_section == nullptr)
    {
        return Error{
            "Exception data directory is not contained within a section...",
            FAILED_ASSET_READER_UNWIND_INFO,
        };
    }

    const ptrdiff_t pdata_virtual_raw_offset{
        static_cast<ptrdiff_t>(pdata_section->PointerToRawData) - pdata_section->VirtualAddress
    };
    std::byte* const pdata_start{
        executable.data() + exception_dir.VirtualAddress + pdata_virtual_raw_offset
    };
    std::span<IMAGE_RUNTIME_FUNCTION_ENTRY> runtime_functions{
        get_nullterminated_table<IMAGE_RUNTIME_FUNCTION_ENTRY>(pdata_start)
    };

    IMAGE_RUNTIME_FUNCTION_ENTRY* loader_entry{ nullptr };
    for (IMAGE_RUNTIME_FUNCTION_ENTRY& runtime_function : runtime_functions)
    {
        if (runtime_function.BeginAddress == function_rva)
        {
            loader_entry = &runtime_function;
            break;
        }
    }
    if (loader_entry == nullptr)
    {
        return Error{
            fmt::format("Could not find .pdata entry for openEncryptedFile at RVA {:#x}...", function_rva),
            FAILED_ASSET_READER_UNWIND_INFO,
        };
    }

    // Unwind info is written in place, so make sure no other runtime function
    // shares this record.
    size_t unwind_info_references{};
    for (const IMAGE_RUNTIME_FUNCTION_ENTRY& runtime_function : runtime_functions)
    {
        if (runtime_function.UnwindInfoAddress == loader_entry->UnwindInfoAddress)
        {
            ++unwind_info_references;
        }
    }
    if (unwind_info_references != 1)
    {
        return Error{
            fmt::format("openEncryptedFile shares unwind info with {} other functions...",
                        unwind_info_references - 1),
            FAILED_ASSET_READER_UNWIND_INFO,
        };
    }

    const IMAGE_SECTION_HEADER* unwind_section{
        find_section_for_rva(image_section_headers, loader_entry->UnwindInfoAddress)
    };
    if (unwind_section == nullptr)
    {
        return Error{
            "Unwind info is not contained within a section...",
            FAILED_ASSET_READER_UNWIND_INFO,
        };
    }

    const ptrdiff_t unwind_virtual_raw_offset{
        static_cast<ptrdiff_t>(unwind_section->PointerToRawData) - unwind_section->VirtualAddress
    };
    std::byte* const unwind_info_data{
        executable.data() + loader_entry->UnwindInfoAddress + unwind_virtual_raw_offset
    };

    const UNWIND_INFO& old_unwind_info{ get_ref<UNWIND_INFO>(unwind_info_data) };
    const size_t old_unwind_size{
        4 + 2 * static_cast<size_t>((old_unwind_info.CountOfCodes + 1) & ~1) +
        ((old_unwind_info.Flags & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER)) ? 8 : 0) +
        ((old_unwind_info.Flags & UNW_FLAG_CHAININFO) ? 4 : 0)
    };

    // Version 1, no language-specific handler, prolog length 8, 4 codes:
    //   sub rsp, 0x20; push r12; push rbp; push rbx
    constexpr std::array<std::byte, 16> replacement_unwind{
        std::byte{ 0x01 },
        std::byte{ 0x08 },
        std::byte{ 0x04 },
        std::byte{ 0x00 },
        std::byte{ 0x08 },
        std::byte{ 0x32 },
        std::byte{ 0x04 },
        std::byte{ 0xC0 },
        std::byte{ 0x02 },
        std::byte{ 0x50 },
        std::byte{ 0x01 },
        std::byte{ 0x30 },
        std::byte{ 0x00 },
        std::byte{ 0x00 },
        std::byte{ 0x00 },
        std::byte{ 0x00 },
    };

    if (old_unwind_size < replacement_unwind.size())
    {
        return Error{
            fmt::format("Existing unwind record is too small ({} bytes)...", old_unwind_size),
            FAILED_ASSET_READER_UNWIND_INFO,
        };
    }

    loader_entry->EndAddress = static_cast<DWORD>(function_rva + replacement_code_size);
    std::memcpy(unwind_info_data, replacement_unwind.data(), replacement_unwind.size());

    logger.verbose("Replaced openEncryptedFile unwind info ({} bytes, EndAddress {:#x})...",
                   replacement_unwind.size(),
                   loader_entry->EndAddress);

    logger.verbose("Patched openEncryptedFile successfully...");
    return std::monostate{};
}

void strip_authenticode(Logger& logger,
                        IMAGE_OPTIONAL_HEADER& image_optional_header)
{
    // The Authenticode signature is a blob appended after all sections. It is
    // pointed to by DataDirectory[SECURITY] using a *file offset* (not an RVA,
    // uniquely among all data directories). After we mutate and resize the
    // binary the original signature no longer covers the right bytes, so Windows
    // would reject it. The simplest correct action is to zero the entry; the
    // blob itself will be dropped when resize_executable truncates the file to
    // the end of the last section.
    IMAGE_DATA_DIRECTORY& security_dir{ image_optional_header.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY] };
    if (security_dir.VirtualAddress != 0 || security_dir.Size != 0)
    {
        logger.verbose("Stripping Authenticode signature...");
        security_dir.VirtualAddress = 0;
        security_dir.Size = 0;
    }
}

void recalculate_checksum(Logger& logger,
                          std::vector<std::byte>& executable)
{
    // CheckSumMappedFile computes the PE checksum over an in-memory image.
    // Windows enforces it for any binary that has CFG or code integrity enabled,
    // and for any binary where the original CheckSum field was non-zero.
    // We always recompute so the output is unconditionally valid.
    logger.verbose("Recalculating PE checksum...");

    DWORD header_sum{};
    DWORD computed_sum{};
    PIMAGE_NT_HEADERS nt_headers{ CheckSumMappedFile(
        executable.data(),
        static_cast<DWORD>(executable.size()),
        &header_sum,
        &computed_sum) };

    if (nt_headers == nullptr)
    {
        logger.warning("CheckSumMappedFile failed ({}), leaving checksum as-is...", GetLastError());
        return;
    }

    logger.verbose("Old checksum: {:#010x}, new checksum: {:#010x}", header_sum, computed_sum);
    nt_headers->OptionalHeader.CheckSum = computed_sum;
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
                                    headers.image_file_header,
                                    headers.image_section_headers,
                                    exe_data.data(),
                                    options.debug_do_not_relocate.value_or(false)))
        };

        // Strip the Authenticode signature before patch_data_directory runs so it
        // never sees DataDirectory[SECURITY] with a stale file offset.
        strip_authenticode(logger, headers.image_optional_header);

        (void)as_value(patch_data_directory(logger,
                                            headers.image_optional_header,
                                            headers.image_section_headers,
                                            bundle_extraction_result.relocation_offset,
                                            exe_data.data()));

        if (options.custom_code_patches)
        {
            (void)as_value(patch_main(logger,
                                      exe_data,
                                      options.insert_debug_interrupts.value_or(false)));

            (void)as_value(patch_bundle_init(logger,
                                             headers.image_section_headers,
                                             exe_data,
                                             options.insert_debug_interrupts.value_or(false)));

            // The bundle is always removed by extract_bundle, so the v1.28
            // checksum validator has to be neutralized for the result to run.
            (void)as_value(patch_bundle_checksum(logger, exe_data));

            (void)as_value(patch_asset_reader(logger,
                                              headers.image_optional_header,
                                              headers.image_section_headers,
                                              exe_data));
        }

        (void)as_value(debug_verify(logger,
                                    headers.image_section_headers,
                                    exe_data,
                                    bundle_extraction_result.relocation_offset));

        resize_executable(logger, headers.image_section_headers, exe_data);

        // Checksum must be computed last, over the final byte sequence.
        recalculate_checksum(logger, exe_data);

        const fs::path out_path{
            exe_path.parent_path() / options.output_name.value()
        };
        write_binary(logger, exe_data, out_path);

        if (options.extract_assets.value_or(false))
        {
            const AssetExtractionReport extraction_report{
                ExtractAssetBundle(
                    bundle_extraction_result.bundle_data,
                    out_path.parent_path(),
                    [](const size_t completed, const size_t total)
                    {
                        if (completed % 16 == 0 || completed == total)
                        {
                            fmt::print("Extracted {}/{} assets...\n", completed, total);
                        }
                    })
            };
            fmt::print("Extraction complete: {} of {} recognized assets, {} output bytes.\n",
                       extraction_report.extracted_asset_count,
                       extraction_report.matched_asset_count,
                       extraction_report.extracted_file_bytes);
            fmt::print("Bundle audit: {} bytes total; {} bytes in {} records; {} bytes of inter-record padding; {} terminator bytes; {} trailing padding bytes; {} unclassified bytes.\n",
                       extraction_report.bundle_bytes,
                       extraction_report.record_bytes,
                       extraction_report.record_count,
                       extraction_report.inter_record_padding_bytes,
                       extraction_report.terminator_bytes,
                       extraction_report.trailing_padding_bytes,
                       extraction_report.unclassified_bytes);
            fmt::print("Unrecognized path hashes: {} records, {} record bytes.\n",
                       extraction_report.unrecognized_record_count,
                       extraction_report.unrecognized_record_bytes);
            for (const UnrecognizedAssetInfo& asset : extraction_report.unrecognized_assets)
            {
                fmt::print("  Offset {:#x}: hash={}, payload={} bytes, encrypted={}\n",
                           asset.bundle_offset,
                           asset.filepath_hash,
                           asset.payload_size,
                           asset.encrypted);
            }
            fmt::print("Output directory: '{}'\n", out_path.parent_path().string());
        }
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
