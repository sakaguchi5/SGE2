#include "FileIO.h"

#include <fstream>

namespace sge::base
{
Result<std::vector<std::byte>, std::string> ReadAllBytes(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) return Result<std::vector<std::byte>, std::string>::Failure("failed to open file for reading");
    const auto end = stream.tellg();
    if (end < 0) return Result<std::vector<std::byte>, std::string>::Failure("failed to determine file size");
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    stream.seekg(0, std::ios::beg);
    if (!bytes.empty() && !stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        return Result<std::vector<std::byte>, std::string>::Failure("failed to read file");
    return Result<std::vector<std::byte>, std::string>::Success(std::move(bytes));
}

Result<void, std::string> WriteAllBytes(const std::filesystem::path& path, std::span<const std::byte> bytes)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) return Result<void, std::string>::Failure("failed to open file for writing");
    if (!bytes.empty()) stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream) return Result<void, std::string>::Failure("failed to write file");
    return Result<void, std::string>::Success();
}
}
