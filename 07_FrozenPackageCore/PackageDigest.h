#pragma once

#include "PackageFormat.h"

#include <span>
#include <vector>

namespace sge::package
{
struct SectionView;
[[nodiscard]] base::Digest256 ComputeExecutionDigest(std::span<const PackageSectionInput> sections);
[[nodiscard]] base::Digest256 ComputeExecutionDigestFromViews(std::span<const SectionView> sections);
[[nodiscard]] base::Digest256 ComputeFileDigest(std::span<const std::byte> fileBytes);
}
