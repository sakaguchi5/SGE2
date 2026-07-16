# SGE2 Level 2 Capability Constitution

## 1. Freeze identity

```text
Identity: SGE2-Level2-D3D12-v1-FinalFreeze-Corpus1
Baseline commit: fc6b883b20d5428a4bf4f82b072fab15e8cb844a
Target schema: 17
Minimum Runtime: 17
Accepted Package corpus: 54
Semantic corpus digest: 4b07e0725dc87f30f495794ea2ef5e245dd0d6415397761c441a9a1ca4f83c49
Authoritative command: run_level2_final.bat
```

Level 2完成物は凍結する。本書は完成ZIP内の`LEVEL2_CAPABILITY_CONSTITUTION_FINAL.md`をv0.2文書体系へ移した現行規範であり、Level 2 binaryまたはcodeを変更する根拠にはしない。

## 2. Level 2の定義

Level 2は、機能数ではなくcardinalityとgraph generalityで定義する。

Compilerは、宣言された有限Semantic vocabulary内の入力について、必ず次の一方を行う。

1. 完全なSemantic Graphをacceptし、Target execution decisionをFrozen Packageへ固定する。
2. Package物質化前に、名前付きsemantic-analysisまたはtarget-feasibility diagnosticでrejectする。

RuntimeとExecutorは、Source object、debug name、歴史的Slice形状、D3D12 convenience assumptionから欠けた判断を回復してはならない。

## 3. Accepted semantic vocabulary

### Resource

- Buffer
- fixed-size Texture2D、mip levelは1、formatはBgra8Unorm
- presentation SurfaceImageは0または1、Bgra8Unorm
- Raster Workごとにsurface-relative Depth32Float attachmentは0または1

### Lifetime / origin

- Persistent Package-owned
- FrameLocal Package-owned
- Temporal Package-owned、Current/Previous generations
- External Buffer、明示incoming/outgoing stateとcompletion contract
- Preparation Resource、明示alias contract専用
- Runtime-managed Surface image

### Work

- Raster
- Compute
- Buffer Copy
- RasterのPresentSource structural operandによるpresentation

Standalone Present WorkはLevel 2でrejectする。

### Graph shape

- vocabularyで表現可能な任意有限DAG
- Resource/Work cardinalityは固定しない
- chain、fan-in、fan-out、diamond、layered、dense-transitive、disconnected、queue zig-zag
- RAW、WAR、WAW ordering
- Source vectorは実行順でなくてよい
- Source IDはsparseでよい
- 一つのSource Programを複数Workが利用可能
- 非意味的vector orderとtransitively redundant dependency edgeはPackage bytesを変えない

### Queue profile

- Direct queueは厳密に1
- Compute queueは0または1
- Copy queueは0または1
- Compute/CopyはDirectへfallback可能
- 同じQueue classへ再入可能
- cross-queue waitは明示SignalPointIdを参照

### Raster profile

- Raster Workごとにvertex buffer 1
- Surface color attachment 1
- Depth32Float attachment 0または1
- triangle-list topology
- Surface frameごとに最終Present 1

### Program / binding

ProgramInterfaceは安定したProgramParameterIdを所有する。Raster/Compute Workは各required parameterをWorkOperandで厳密に1回bindingする。

Supported parameter kind:

- ConstantBuffer
- SampledTexture
- ReadOnlyBuffer
- Compute用UnorderedBuffer

Shader register identityはProgramParameterが所有し、ResourceUse registration orderに依存しない。Shader Reflectionは宣言interfaceと完全一致しなければならない。

## 4. Runtime-qualified interactions

- Surface Packageとheadless Package
- multiple Dynamic slots
- multiple External Buffer slots
- FrameLocal physical-instance reuse
- Temporal Previous/Current across frames and queues
- dedicated Compute/Copy queueとDirect fallback
- exact GPU ReadbackとCPU reference一致
- controlled/actual device removal後のPackage-owned reconstruction
- Recovery後のTemporal resetとExternal rebind
- Source/Compilerなしのfresh-process rematerialization

## 5. Explicit rejection

Level 2 D3D12 v1は次を明示的にrejectする。

- cyclic graph、dangling identity
- presentation Surfaceが2以上
- Texture2D mip countが1以外
- unsupported fixed/surface-relative Texture format
- native Compute/Copy queueが2以上
- Direct queue countが1以外
- standalone Present Work
- index buffer
- MRT
- offscreen color attachment
- MSAA
- Texture array、cube map、mip chain
- External Texture
- missing/duplicate/mismatched ProgramParameter binding
- 一つのResourceUseを複数Workが所有
- invalid Resource/ViewRole/Effect combination
- non-Temporal ResourceへのPrevious relation
- out-of-range Copy
- overlapping/incompatible alias contract
- unsupported root-signature costまたはTarget Profile
- malformed、contradictory、unknown-version、internally inconsistent Package

すべてD3D12 command recording前に失敗しなければならない。Device removalはPackage validatorではない。

## 6. Deferred beyond Level 2

- 複数valid planの生成と比較
- Cost model、latency/memory objective、profiling、plan selection
- same-class multiple native queues
- MRT、index、MSAA、mip chain、offscreen等の新vocabulary
- dynamic recompilationまたはprofile-guided Package replacement

Level 2は一つの完全で決定的な正しいPlanを生成する。最良Planの選択はLevel 3以降の問題である。
