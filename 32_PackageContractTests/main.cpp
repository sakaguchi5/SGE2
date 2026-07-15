#include "../00_Base/BinaryIO.h"
#include "../07_FrozenPackageCore/PackageReader.h"
#include "../07_FrozenPackageCore/PackageWriter.h"

#include <algorithm>
#include <iostream>

namespace
{
sge::package::PackageBuildInput MakeInput()
{
    using namespace sge::package;
    PackageBuildInput input;
    sge::base::BinaryWriter manifest;
    for (int i = 0; i < 16; ++i) manifest.WriteU32(i == 13 ? InvalidIndex : (i == 14 ? InvalidIndex : 0));
    PackageSectionInput m;
    m.kind = SectionKind::Manifest;
    m.flags = static_cast<std::uint32_t>(SectionFlags::Required | SectionFlags::ExecutionAffecting);
    m.alignment = 8;
    m.elementCount = 1;
    m.elementStride = 64;
    m.bytes = std::move(manifest).Take();
    input.sections.push_back(std::move(m));

    PackageSectionInput profile;
    profile.kind = SectionKind::D3D12TargetProfile;
    profile.flags = static_cast<std::uint32_t>(SectionFlags::Required | SectionFlags::ExecutionAffecting);
    profile.alignment = 8;
    profile.elementCount = 1;
    profile.elementStride = 80;
    profile.bytes.assign(80, std::byte{0});
    input.sections.push_back(std::move(profile));
    return input;
}
}

int main()
{
    auto first = sge::package::PackageWriter::Write(MakeInput());
    auto second = sge::package::PackageWriter::Write(MakeInput());
    if (!first || !second || first.Value() != second.Value()) { std::cerr << "writer determinism failed\n"; return 1; }
    auto read = sge::package::PackageReader::Read(first.Value());
    if (!read) { std::cerr << read.Error().message << '\n'; return 2; }

    sge::package::PackageBuildInput roundTrip;
    roundTrip.targetKind = read.Value().Header().targetKind;
    roundTrip.targetSchemaVersion = read.Value().Header().targetSchemaVersion;
    roundTrip.minimumRuntimeVersion = read.Value().Header().minimumRuntimeVersion;
    for (const auto& section : read.Value().Sections())
    {
        sge::package::PackageSectionInput copy;
        copy.kind = section.descriptor.sectionKind;
        copy.schemaVersion = section.descriptor.schemaVersion;
        copy.flags = section.descriptor.flags;
        copy.alignment = section.descriptor.alignment;
        copy.elementCount = section.descriptor.elementCount;
        copy.elementStride = section.descriptor.elementStride;
        copy.bytes.assign(section.bytes.begin(), section.bytes.end());
        roundTrip.sections.push_back(std::move(copy));
    }
    auto rewritten = sge::package::PackageWriter::Write(std::move(roundTrip));
    if (!rewritten || rewritten.Value() != first.Value()) { std::cerr << "round trip bytes differ\n"; return 3; }

    auto badMagic = first.Value();
    badMagic[0] ^= std::byte{1};
    if (sge::package::PackageReader::Read(std::move(badMagic))) { std::cerr << "bad magic accepted\n"; return 4; }
    auto corrupt = first.Value();
    corrupt.back() ^= std::byte{1};
    if (sge::package::PackageReader::Read(std::move(corrupt))) { std::cerr << "corruption accepted\n"; return 5; }

    auto unknownInput = MakeInput();
    sge::package::PackageSectionInput unknown;
    unknown.kind = static_cast<sge::package::SectionKind>(0x00006000u);
    unknown.flags = static_cast<std::uint32_t>(sge::package::SectionFlags::Required | sge::package::SectionFlags::ExecutionAffecting);
    unknown.alignment = 8;
    unknown.bytes = {std::byte{1}};
    unknownInput.sections.push_back(std::move(unknown));
    auto unknownBytes = sge::package::PackageWriter::Write(std::move(unknownInput));
    if (!unknownBytes || sge::package::PackageReader::Read(std::move(unknownBytes).Value())) { std::cerr << "unknown required section accepted\n"; return 6; }
    std::cout << "Package contract tests passed.\n";
    return 0;
}
