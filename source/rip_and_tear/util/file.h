#pragma once

#include <cstddef>
#include <filesystem>
#include <vector>

std::vector<std::byte> ReadWholeFile(const std::filesystem::path& file_path);
