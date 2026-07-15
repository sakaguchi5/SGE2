#include "FrozenExecutablePackage.h"

namespace sge::package
{
const SectionView* FrozenExecutablePackage::FindSection(SectionKind kind) const noexcept
{
    for (const auto& section : sections_)
        if (section.descriptor.sectionKind == kind) return &section;
    return nullptr;
}
}
