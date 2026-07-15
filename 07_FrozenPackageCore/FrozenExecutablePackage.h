#pragma once

#include "PackageFormat.h"

#include <memory>
#include <span>
#include <vector>

namespace sge::package
{
struct SectionView final
{
    SectionDescriptor descriptor;
    std::span<const std::byte> bytes;
};

class FrozenExecutablePackage final
{
public:
    [[nodiscard]] const PackageHeader& Header() const noexcept { return header_; }
    [[nodiscard]] std::span<const SectionView> Sections() const noexcept { return sections_; }
    [[nodiscard]] const base::Digest256& ExecutionDigest() const noexcept { return header_.executionDigest; }
    [[nodiscard]] std::uint32_t Target() const noexcept { return header_.targetKind; }
    [[nodiscard]] std::span<const std::byte> FileBytes() const noexcept { return *storage_; }
    [[nodiscard]] const SectionView* FindSection(SectionKind kind) const noexcept;

private:
    friend class PackageReader;
    std::shared_ptr<const std::vector<std::byte>> storage_;
    PackageHeader header_;
    std::vector<SectionView> sections_;
};
}
