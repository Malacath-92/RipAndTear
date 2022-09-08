#pragma once

#include <filesystem>
#include <cstddef>
#include <vector>

std::vector<std::byte> ReadWholeFile(const std::filesystem::path& file_path);
