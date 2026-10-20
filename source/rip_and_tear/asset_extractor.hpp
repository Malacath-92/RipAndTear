#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

struct UnrecognizedAssetInfo
{
    std::size_t bundle_offset{};
    std::size_t record_size{};
    std::size_t payload_size{};
    bool encrypted{};
    std::string filepath_hash;
};

struct AssetExtractionReport
{
    std::size_t bundle_bytes{};
    std::size_t record_count{};
    std::size_t matched_asset_count{};
    std::size_t extracted_asset_count{};
    std::size_t unrecognized_record_count{};
    std::size_t unrecognized_record_bytes{};
    std::vector<UnrecognizedAssetInfo> unrecognized_assets;
    std::size_t extracted_file_bytes{};
    std::size_t record_bytes{};
    std::size_t inter_record_padding_bytes{};
    std::size_t trailing_padding_bytes{};
    std::size_t terminator_bytes{};
    std::size_t unclassified_bytes{};
};

using AssetExtractionProgress = std::function<void(std::size_t, std::size_t)>;

[[nodiscard]] AssetExtractionReport ExtractAssetBundle(
    std::span<const std::byte> bundle_data,
    const std::filesystem::path& output_directory,
    const AssetExtractionProgress& on_progress = {});