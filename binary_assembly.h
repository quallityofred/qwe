#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace binary_assembly {

struct AssemblyManifest {
    std::uintmax_t input_size{0};
    std::size_t padding_size{0};
    std::uintmax_t output_size{0};
    std::filesystem::path output_path;
};

[[nodiscard]] AssemblyManifest assemble_with_random_padding(
    const std::filesystem::path& input_path,
    const std::filesystem::path& output_path);

}  // namespace binary_assembly
