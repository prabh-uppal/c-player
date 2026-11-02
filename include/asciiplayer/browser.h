#pragma once

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

std::string browse(fs::path& dir);
