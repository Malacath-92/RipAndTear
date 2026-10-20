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
    std::optional<bool> extract_assets{ false };
    std::optional<int32_t> exit_reason;
    std::optional<bool> verbose{ false };
};
VISITABLE_STRUCT(CommandLineOptions,
                 exe_dir,
                 output_name,
                 insert_debug_interrupts,
                 debug_do_not_relocate,
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

void strip_authenticode(Logger& logger,
                        IMAGE_OPTIONAL_HEADER& image_optional_header)
{
    // The Authenticode signature is a blob appended after all sections.  It is
    // pointed to by DataDirectory[SECURITY] using a *file offset* (not an RVA,
    // uniquely among all data directories).  After we mutate and resize the
    // binary the original signature no longer covers the right bytes, so Windows
    // would reject it.  The simplest correct action is to zero the entry; the
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
