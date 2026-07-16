# SGE Package Container ABI v1

## 1. Scope

本書はTarget非依存の`.sgep` container formatを定める。D3D12 record、operation payload、Runtime lifecycleは別仕様で定める。

As-built source:

```text
07_FrozenPackageCore/PackageFormat.h
07_FrozenPackageCore/PackageWriter.cpp
07_FrozenPackageCore/PackageReader.cpp
```

Container formatは1.0、Headerは176 bytes、Section Descriptorは80 bytes、little endian、SHA-256を使用する。

## 2. Binaryとmemory representation

Binary ABIとLoaderが生成するC++ memory objectを同一視してはならない。

Binaryへ次を直接書き出してはならない。

- pointer
- size_t
- bool
- std::string / vector / variant / optional
- compiler paddingを含むC++ structのmemcpy
- implementation-defined enum width

固定幅整数をfield単位でencode/decodeする。

## 3. Header

Magic:

```text
SGE2PKG\0
```

Header fields:

| Offset | Size | Field |
|---:|---:|---|
| 0x00 | 8 | magic |
| 0x08 | 2 | formatMajor = 1 |
| 0x0A | 2 | formatMinor = 0 |
| 0x0C | 2 | headerBytes = 176 |
| 0x0E | 2 | sectionDescriptorBytes = 80 |
| 0x10 | 4 | targetKind |
| 0x14 | 4 | targetSchemaVersion |
| 0x18 | 4 | minimumRuntimeVersion |
| 0x1C | 4 | flags |
| 0x20 | 4 | endianTag = 0x01020304 |
| 0x24 | 4 | digestAlgorithm = 1（SHA-256） |
| 0x28 | 8 | fileBytes |
| 0x30 | 8 | sectionTableOffset |
| 0x38 | 4 | sectionCount |
| 0x3C | 4 | reserved0 = 0 |
| 0x40 | 32 | targetProfileDigest |
| 0x60 | 32 | executionDigest |
| 0x80 | 32 | fileDigest |
| 0xA0 | 16 | reserved1 = 0 |

`targetKind = 1`はD3D12である。

## 4. Section Descriptor

各descriptorは80 bytes固定。

| Offset | Size | Field |
|---:|---:|---|
| 0x00 | 4 | sectionKind |
| 0x04 | 2 | schemaVersion |
| 0x06 | 2 | descriptorBytes = 80 |
| 0x08 | 4 | flags |
| 0x0C | 4 | alignment |
| 0x10 | 8 | fileOffset |
| 0x18 | 8 | storedBytes |
| 0x20 | 8 | logicalBytes |
| 0x28 | 4 | elementCount |
| 0x2C | 4 | elementStride |
| 0x30 | 32 | sectionDigest |

Rules:

- Section tableはSectionKind昇順
- 同一kindの重複は禁止
- alignmentは非0 power-of-two
- `offset + size` overflow禁止
- Section overlap禁止
- padding byteは0
- v1ではcompressionなし、`storedBytes == logicalBytes`

## 5. Section flags

```text
Required           = 1 << 0
ExecutionAffecting = 1 << 1
DebugOnly          = 1 << 2
OpaqueToCore       = 1 << 3
```

Unknown section:

- Requiredならreject
- OptionalかつCoreが安全に無視できる場合のみ許可

Unknown OperationはTarget Schema規則により常にrejectする。

## 6. Section kind registry

### Core / common

```text
0x00000001 Manifest
0x00000002 InvocationSchema
0x00000003 OperationStreamTable
0x00000004 OperationTable
0x00000005 OperationPayload
0x00000006 InitialData
0x00000007 ShaderData
0x00000008 NativeObjectData
```

### D3D12 target

```text
0x00001000 D3D12TargetProfile
0x00001001 D3D12ResourceTable
0x00001002 D3D12AllocationTable
0x00001003 D3D12ViewTable
0x00001004 D3D12ShaderTable
0x00001005 D3D12ProgramTable
0x00001006 D3D12BindingLayoutTable
0x00001007 D3D12ExecutableTable
0x00001008 D3D12RasterCommandTable
0x00001009 D3D12DescriptorPlan
0x0000100A D3D12DynamicSlotTable
0x0000100B D3D12ExternalSlotTable
0x0000100C D3D12SurfaceSlotTable
0x0000100D D3D12VertexElementTable
0x0000100E D3D12AttachmentOperationTable
0x0000100F D3D12RootParameterTable
0x00001010 D3D12ComputeExecutableTable
0x00001011 D3D12ComputeCommandTable
```

### Optional metadata

```text
0x00007F00 StringTable
0x00007F01 DebugMap
0x00007F02 Provenance
```

## 7. Digest semantics

### targetProfileDigest

Target Profile sectionのcanonical bytesを覆う。Target compatibility/cache identityに使う。

### executionDigest

実行結果へ影響する全ExecutionAffecting sectionをcanonical順で覆う。Debug name、timestamp、Source mapping等は含めない。

### fileDigest

File全体を覆う。計算時のみHeader内fileDigest fieldを0として扱う。

## 8. Canonical serialization

- SectionKind昇順
- Table recordはdense ID順
- IDは0から連続
- Map/Setはkey順へ変換
- Blobはowner identity順
- Texture dataはmip/array/plane順
- padding/reservedは0
- Timestamp、random UUID、pointer、process固有値を入れない
- FloatはIEEE 754 bit pattern
- NaNは実行用fieldでreject
- -0.0は+0.0へcanonicalize

## 9. Reader validation order

1. minimum file size
2. magic
3. endian
4. container version
5. header/descriptor size
6. file size
7. section table bounds
8. section order/duplicates
9. alignment
10. overflow/bounds/overlap
11. required sections
12. section digest
13. file digest
14. execution digest
15. record strideとID sequence
16. Target Schemaへ引き渡し

静的検証完了前にTarget API objectを作成してはならない。

## 10. Versioning

- Container format version
- Target schema version
- Section schema version
- Operation version

Field意味変更、digest規則変更、既存readerが安全に解釈できない変更はMajor変更。安全に無視可能なoptional metadata追加等はMinor候補とする。
