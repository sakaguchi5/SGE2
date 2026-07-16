# SGE2 docs v0.2

このフォルダは、Semantic GPU Engine 2 Level 2完成後の文書体系である。

Level 2完成物は変更しない。本フォルダは、Level 2のas-built事実を現在の規範へ昇格し、v0.1および開発Stage資料を歴史資料として保存する。

## Directory

```text
docs/
├─ current/
│  ├─ SGE2_Design_Constitution_v0.2.md
│  ├─ SGE2_Level_Model_v0.2.md
│  ├─ SGE2_Level2_Capability_Constitution.md
│  ├─ SGE2_Level2_Qualification.md
│  ├─ Package_Container_ABI_v1.md
│  ├─ D3D12_Package_Schema_v17.md
│  ├─ Package_Runtime_Contract_v17.md
│  ├─ Materialization_Delegation_Contract.md
│  └─ Level3_Development_Plan.md
├─ history/
│  ├─ Semantic GPU Engine 第二世代 設計憲法 v0.1.pdf
│  ├─ Frozen Executable Package ABI 設計仕様 v0.1.pdf
│  ├─ Slice資料/
│  ├─ G1-G7資料/
│  └─ Stage A-M資料/
└─ README.md
```

## Authority order

1. `current/SGE2_Design_Constitution_v0.2.md`
2. Level Capability Constitution
3. Container ABI / Target Schema / Runtime Contract
4. Qualification document
5. Level development plans
6. `history/`のStage/Slice/実装ノート

`history/`は削除しないが、現在の設計判断の規範として用いない。

## Level 2 freeze

```text
Identity: SGE2-Level2-D3D12-v1-FinalFreeze-Corpus1
Baseline commit: fc6b883b20d5428a4bf4f82b072fab15e8cb844a
Target schema: 17
Minimum Runtime: 17
Accepted corpus: 54 Packages
Semantic corpus digest: 4b07e0725dc87f30f495794ea2ef5e245dd0d6415397761c441a9a1ca4f83c49
Authoritative command: run_level2_final.bat
```

## Historical primary-file SHA-256

```text
Semantic GPU Engine 第二世代 設計憲法 v0.1.pdf
4f72907f6e1743e3d731b878a3de95e5ed06a1b1e3937becfe8f5601475cc9f4

Frozen Executable Package ABI 設計仕様 v0.1.pdf
f8f0894971064321e619f93c4cc6076873b8eac673fd594f68fb26eed03110cf
```

## Reading order

初めて読む場合:

1. Design Constitution v0.2
2. Level Model v0.2
3. Level 2 Capability Constitution
4. Materialization and Delegation Contract
5. Level 3 Development Plan

ABI/実装を確認する場合:

1. Package Container ABI v1
2. D3D12 Package Schema v17
3. Package Runtime Contract v17
4. Level 2 Qualification

## Important interpretation

SGE2は物理GPU実行の時刻や独立Queue間interleavingを決定論化しない。

SGE2が決定論的に生成するのは、正しい実行に必要な静的Planと動的境界契約を保存したFrozen Executable Packageである。
