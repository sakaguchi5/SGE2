#include "PackageDigest.h"
#include "../00_Base/BinaryIO.h"
#include "FrozenExecutablePackage.h"

#include <algorithm>

namespace sge::package
{
namespace
{
template<class Section, class GetKind, class GetFlags, class GetBytes>
base::Digest256 Compute(std::span<const Section> sections, GetKind getKind, GetFlags getFlags, GetBytes getBytes)
{
    base::BinaryWriter writer;
    for (const auto& section : sections)
    {
        if (!HasFlag(getFlags(section), SectionFlags::ExecutionAffecting)) continue;
        const auto bytes = getBytes(section);
        writer.WriteU32(static_cast<std::uint32_t>(getKind(section)));
        writer.WriteU64(bytes.size());
        writer.WriteBytes(bytes);
    }
    return base::Sha256(writer.Bytes());
}
}

base::Digest256 ComputeExecutionDigest(std::span<const PackageSectionInput> sections)
{
    return Compute<PackageSectionInput>(sections,
        [](const auto& s) { return s.kind; },
        [](const auto& s) { return s.flags; },
        [](const auto& s) { return std::span<const std::byte>(s.bytes); });
}

base::Digest256 ComputeExecutionDigestFromViews(std::span<const SectionView> sections)
{
    return Compute<SectionView>(sections,
        [](const auto& s) { return s.descriptor.sectionKind; },
        [](const auto& s) { return s.descriptor.flags; },
        [](const auto& s) { return s.bytes; });
}

base::Digest256 ComputeFileDigest(std::span<const std::byte> fileBytes)
{
    std::vector<std::byte> copy(fileBytes.begin(), fileBytes.end());
    constexpr std::size_t FileDigestOffset = 0x80;
    if (copy.size() >= FileDigestOffset + 32)
        std::fill(copy.begin() + FileDigestOffset, copy.begin() + FileDigestOffset + 32, std::byte{0});
    return base::Sha256(copy);
}
}
