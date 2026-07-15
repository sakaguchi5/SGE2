#include "BinaryIO.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace sge::base
{
namespace
{
template<class T>
void WriteLittle(std::vector<std::byte>& out, T value)
{
    for (std::size_t i = 0; i < sizeof(T); ++i)
        out.push_back(static_cast<std::byte>((static_cast<std::uint64_t>(value) >> (i * 8)) & 0xffu));
}

template<class T>
void PatchLittle(std::vector<std::byte>& out, std::size_t offset, T value)
{
    if (offset + sizeof(T) > out.size()) throw std::out_of_range("BinaryWriter patch out of range");
    for (std::size_t i = 0; i < sizeof(T); ++i)
        out[offset + i] = static_cast<std::byte>((static_cast<std::uint64_t>(value) >> (i * 8)) & 0xffu);
}

template<class T>
Result<T, std::string> ReadLittle(std::span<const std::byte> bytes, std::size_t& position)
{
    if (position + sizeof(T) > bytes.size()) return Result<T, std::string>::Failure("binary read out of range");
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i)
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[position + i])) << (i * 8);
    position += sizeof(T);
    return Result<T, std::string>::Success(static_cast<T>(value));
}
}

void BinaryWriter::WriteU8(std::uint8_t value) { bytes_.push_back(static_cast<std::byte>(value)); }
void BinaryWriter::WriteU16(std::uint16_t value) { WriteLittle(bytes_, value); }
void BinaryWriter::WriteU32(std::uint32_t value) { WriteLittle(bytes_, value); }
void BinaryWriter::WriteI32(std::int32_t value) { WriteLittle(bytes_, static_cast<std::uint32_t>(value)); }
void BinaryWriter::WriteU64(std::uint64_t value) { WriteLittle(bytes_, value); }
void BinaryWriter::WriteBytes(std::span<const std::byte> bytes) { bytes_.insert(bytes_.end(), bytes.begin(), bytes.end()); }
void BinaryWriter::WriteZeroes(std::size_t count) { bytes_.insert(bytes_.end(), count, std::byte{0}); }
void BinaryWriter::Align(std::uint32_t alignment)
{
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) throw std::invalid_argument("alignment must be a power of two");
    const auto remainder = bytes_.size() % alignment;
    if (remainder != 0) WriteZeroes(alignment - remainder);
}
void BinaryWriter::PatchU16(std::size_t offset, std::uint16_t value) { PatchLittle(bytes_, offset, value); }
void BinaryWriter::PatchU32(std::size_t offset, std::uint32_t value) { PatchLittle(bytes_, offset, value); }
void BinaryWriter::PatchU64(std::size_t offset, std::uint64_t value) { PatchLittle(bytes_, offset, value); }
void BinaryWriter::PatchBytes(std::size_t offset, std::span<const std::byte> bytes)
{
    if (offset + bytes.size() > bytes_.size()) throw std::out_of_range("BinaryWriter patch out of range");
    std::copy(bytes.begin(), bytes.end(), bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
}

Result<std::uint8_t, std::string> BinaryReader::ReadU8() { return ReadLittle<std::uint8_t>(bytes_, position_); }
Result<std::uint16_t, std::string> BinaryReader::ReadU16() { return ReadLittle<std::uint16_t>(bytes_, position_); }
Result<std::uint32_t, std::string> BinaryReader::ReadU32() { return ReadLittle<std::uint32_t>(bytes_, position_); }
Result<std::int32_t, std::string> BinaryReader::ReadI32()
{
    auto value = ReadLittle<std::uint32_t>(bytes_, position_);
    if (!value) return Result<std::int32_t, std::string>::Failure(value.Error());
    return Result<std::int32_t, std::string>::Success(static_cast<std::int32_t>(value.Value()));
}
Result<std::uint64_t, std::string> BinaryReader::ReadU64() { return ReadLittle<std::uint64_t>(bytes_, position_); }
Result<std::span<const std::byte>, std::string> BinaryReader::ReadBytes(std::size_t count)
{
    if (position_ + count > bytes_.size()) return Result<std::span<const std::byte>, std::string>::Failure("binary read out of range");
    auto result = bytes_.subspan(position_, count);
    position_ += count;
    return Result<std::span<const std::byte>, std::string>::Success(result);
}
Result<void, std::string> BinaryReader::Skip(std::size_t count)
{
    if (position_ + count > bytes_.size()) return Result<void, std::string>::Failure("binary skip out of range");
    position_ += count;
    return Result<void, std::string>::Success();
}
}
