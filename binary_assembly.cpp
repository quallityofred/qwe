#include "binary_assembly.h"
#include <fstream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>
#include <Windows.h> // Необходим для структур IMAGE_DOS_HEADER / IMAGE_NT_HEADERS64

namespace binary_assembly {
    namespace {

        constexpr std::uintmax_t kMaximumInputSize = 256ULL * 1024ULL * 1024ULL;
        constexpr std::size_t kMinimumPaddingSize = 64;
        constexpr std::size_t kMaximumPaddingSize = 256;

        [[nodiscard]] std::runtime_error file_error(
            const std::string& operation,
            const std::filesystem::path& path) {
            return std::runtime_error(operation + ": " + path.string());
        }

        [[nodiscard]] std::vector<std::uint8_t> read_binary_file(
            const std::filesystem::path& input_path) {
            std::ifstream input(input_path, std::ios::binary | std::ios::ate);
            if (!input.is_open()) {
                throw file_error("Unable to open input file", input_path);
            }

            const std::streampos end_position = input.tellg();
            if (end_position < std::streampos{ 0 }) {
                throw file_error("Unable to determine input file size", input_path);
            }

            const auto input_size = static_cast<std::uintmax_t>(end_position);
            if (input_size > kMaximumInputSize) {
                throw file_error("Input file exceeds the 256 MiB safety limit", input_path);
            }

            std::vector<std::uint8_t> buffer(static_cast<std::size_t>(input_size));
            input.seekg(0, std::ios::beg);
            if (!input) {
                throw file_error("Unable to seek to the start of the input file", input_path);
            }

            if (!buffer.empty()) {
                const auto bytes_to_read = static_cast<std::streamsize>(buffer.size());
                input.read(reinterpret_cast<char*>(buffer.data()), bytes_to_read);
            }

            return buffer;
        }

        [[nodiscard]] std::vector<std::uint8_t> generate_padding() {
            std::random_device entropy;
            std::seed_seq seed{
                entropy(), entropy(), entropy(), entropy(),
                entropy(), entropy(), entropy(), entropy() };
            std::mt19937 generator(seed);

            std::uniform_int_distribution<std::size_t> size_distribution(kMinimumPaddingSize, kMaximumPaddingSize);
            std::uniform_int_distribution<unsigned int> byte_distribution(0, 255);

            std::vector<std::uint8_t> padding(size_distribution(generator));
            for (std::uint8_t& byte : padding) {
                byte = static_cast<std::uint8_t>(byte_distribution(generator));
            }

            return padding;
        }

        void ensure_output_directory(const std::filesystem::path& output_path) {
            const std::filesystem::path output_directory = output_path.parent_path();
            std::error_code error;
            std::filesystem::create_directories(output_directory, error);
        }

        void write_binary_file(
            const std::filesystem::path& output_path,
            const std::vector<std::uint8_t>& buffer) {
            std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
            if (!output.is_open()) {
                throw file_error("Unable to open output file", output_path);
            }
            if (!buffer.empty()) {
                output.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            }
            output.close();
        }

        // Новая внутренняя функция для безопасного удаления цифровой подписи из структуры PE
        void strip_digital_signature(std::vector<std::uint8_t>& buffer) {
            if (buffer.size() < sizeof(IMAGE_DOS_HEADER)) return;

            auto* dos_header = reinterpret_cast<IMAGE_DOS_HEADER*>(buffer.data());
            if (dos_header->e_magic != IMAGE_DOS_SIGNATURE) return;

            if (buffer.size() < static_cast<std::size_t>(dos_header->e_lfanew) + sizeof(IMAGE_NT_HEADERS64)) return;

            auto* nt_headers = reinterpret_cast<IMAGE_NT_HEADERS64*>(buffer.data() + dos_header->e_lfanew);
            if (nt_headers->Signature != IMAGE_NT_SIGNATURE) return;

            // Получаем доступ к таблице директорий безопасности (Security Directory)
            auto& security_dir = nt_headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY];

            if (security_dir.VirtualAddress != 0 && security_dir.Size != 0) {
                // Удаляем саму подпись (обрезаем оверлей, где она хранилась)
                if (buffer.size() >= (security_dir.VirtualAddress + security_dir.Size)) {
                    buffer.resize(security_dir.VirtualAddress);
                }
                // Полностью обнуляем указатели на таблицу сертификатов в заголовке
                security_dir.VirtualAddress = 0;
                security_dir.Size = 0;
            }
        }

    }  // namespace

    AssemblyManifest assemble_with_random_padding(
        const std::filesystem::path& input_path,
        const std::filesystem::path& output_path) {
        if (!input_path.is_absolute() || !output_path.is_absolute()) {
            throw std::invalid_argument("Input and output paths must be absolute");
        }

        std::vector<std::uint8_t> assembled_buffer = read_binary_file(input_path);

        // Сначала полностью вырезаем старую цифровую подпись, которая ломает формат
        strip_digital_signature(assembled_buffer);

        const std::uintmax_t input_size = assembled_buffer.size();
        std::vector<std::uint8_t> padding = generate_padding();

        // Теперь безопасно пристыковываем новый рандомный паддинг в конец чистого PE-файла
        assembled_buffer.insert(assembled_buffer.end(), padding.cbegin(), padding.cend());

        ensure_output_directory(output_path);
        write_binary_file(output_path, assembled_buffer);

        return {
            input_size,
            padding.size(),
            assembled_buffer.size(),
            output_path };
    }

}  // namespace binary_assembly
