#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

std::vector<std::byte> ReadWholeFile(const std::filesystem::path& file_path);
bool WriteWholeFile(const std::filesystem::path& file_path, const std::span<std::byte>& data);
