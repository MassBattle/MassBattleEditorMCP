# MassBattleEditorMCP 技术方案与设计文档

本项目旨在为 **MassBattle** 插件构建一个基于 **MCP (Model Context Protocol)** 的编辑器后端，允许 AI 智能体直接在虚幻引擎 5 编辑器中操纵、配置并监控大规模战争实体（Battle Agents）的仿真。

---

## 1. MCP 后端架构方案

MCP 后端将参照 `UmgMcp` (FabUmgMcp) 的成熟架构进行设计，通过以下几个层级实现与 AI 的双向通信：

1. **协议传输层 (Transport Layer)**：
   * 在编辑器子系统（Editor Subsystem）中建立异步 TCP/WebSocket 本地服务器。
   * 监听并解析标准的 MCP JSON-RPC 2.0 请求（如 `tools/list`、`tools/call`）。
2. **命令路由与分发 (Command Dispatcher)**：
   * 将接收到的 JSON 命令分发至对应的功能处理器（如 Spawner 处理器、Simulation 处理器等）。
   * 统一进行主线程（GameThread）调度，保证 UObject 读写和关卡 Actor 操作的线程安全。
3. **接口层 (Tool Bindings)**：
   * 将 MassBattle 的 C++ API 封装为标准 JSON Schema 格式，供 LLM（大语言模型）识别并调用。

---

## 2. MassBattle MCP 核心工具（Tools）列表设计

为充分支持大规模实体战争的配置和仿真，初步规划以下 MCP 工具列表：

### 2.1 实体生成器配置 (Spawners & Agents)

#### `massbattle_get_spawners`
* **功能**：获取当前关卡中所有的 Mass 实体生成器（Spawner）及其关键配置。
* **返回**：Spawner Actor 名称、生成实体类型、关联数据资产（Config Asset）等。

#### `massbattle_edit_spawner`
* **功能**：动态修改生成器的参数。
* **参数**：
  * `spawner_name` (string): 目标生成器名称
  * `agent_count` (number, optional): 生成的实体数量
  * `spawn_radius` (number, optional): 生成半径范围
  * `alignment` (string, optional): 阵营（例如：Red / Blue / Neutral）

---

### 2.2 阵型与战术控制 (Formations & Tactics)

#### `massbattle_set_formation`
* **功能**：配置或更改实体群的生成/移动阵型。
* **参数**：
  * `spawner_name` (string): 目标生成器
  * `formation_type` (string): 阵型样式（如 `Line`、`Column`、`Wedge`、`Circle`）
  * `spacing` (number): 实体间隔距离

#### `massbattle_set_move_target`
* **功能**：为指定阵营或生成器的实体群设定行军目标点。
* **参数**：
  * `spawner_name` (string)
  * `target_location` (vector3: `{x, y, z}`)

---

### 2.3 仿真控制与监控 (Simulation & Monitoring)

#### `massbattle_control_simulation`
* **功能**：启动、暂停、单步运行或重置 Mass 战斗仿真。
* **参数**：
  * `action` (string): `start` / `pause` / `step` / `reset`

#### `massbattle_get_simulation_stats`
* **功能**：实时查询当前战斗仿真的统计数据与性能指标。
* **返回**：总实体数、当前帧率（FPS）、GPU 渲染开销、各状态下（如 Idle、Combat、Flee）的实体比例。

---

### 2.4 粒子渲染控制 (Niagara Visualization)

#### `massbattle_modify_niagara_params`
* **功能**：调整用于表示和渲染 Mass 实体的 Niagara 粒子系统参数，以优化视觉效果或性能。
* **参数**：
  * `emitter_name` (string): 粒子发射器或系统名
  * `particle_limit` (number, optional): 最大渲染粒子上限
  * `lod_distance` (number, optional): LOD 裁剪距离阀值

---

## 3. 已实现：单位与风格 MCP API

当前 `MassBattleEditorMCP` 新增了独立的单位/风格 API 类，不依赖 `UmgMcp`：

* `UMassBattleUnitMCPApi`：单位查询、Schema、导出、编辑计划、克隆/生成计划、软删除、素材检索。
* `UMassBattleStyleMCPApi`：按 `StyleType`、路径类别、推断风格族汇总单位，并生成风格化移动计划。

### 3.1 单位 JSON 约定

`MCP_UnitList` 和 `MCP_UnitGet` 默认返回 simple JSON。单位数据放在 `Data` 字段下，字段名保持 `UMassBattleAgentConfigDataAsset` 源码命名，例如：

```json
{
  "AssetName": "AgentConfig_Tank_RTS",
  "ObjectPath": "/MassBattle/Demo/Agent/Tank/AgentConfig_Tank_RTS.AgentConfig_Tank_RTS",
  "Data": {
    "Move": {
      "XY": {
        "MoveSpeed": 2000
      }
    },
    "Attack": {
      "Range": 7500,
      "CoolDown": 2
    }
  }
}
```

默认忽略策略：

* 自动省略与 `UMassBattleAgentConfigDataAsset` 默认对象相同的字段。
* 默认隐藏 runtime/system/deprecated/editor visibility 字段，例如 `Moving`、`Attacking`、`DataVersion`、`bShow...`。
* simple 模式只展开常用平衡和生成相关对象；复杂对象需要显式指定。

详细/复杂对象请求：

* `{"detail":"detailed"}` 或 `{"mode":"full"}`：返回非默认、非忽略的完整作者配置。
* `{"object":"AnimShared"}` 或 `{"objects":["AnimShared","Visualize"]}`：只返回指定对象，`detail` 标记为 `objects`。
* `{"include_defaults":true}`：允许返回默认值，用于 Schema 或完整审计场景。

`ExtraData` 特殊处理为 `FMassBattleTemplate` 源码字段：`Tags`、`Fragments`、`MutableSharedFragments`、`ConstSharedFragments`、`BattleFlags`。

### 3.2 编辑与生成约定

资产写入采用计划优先：

* `MCP_UnitPlanUpdate(UnitPath, PatchJson)`：只生成 diff 和计划文件。
* `MCP_UnitPlanMergeUpdate(UnitPath, UnitDataJson)`：按源码字段名并集写入局部单位 JSON，只生成 diff 和计划文件。
* `MCP_UnitMergeUpdate(UnitPath, UnitDataJson, bSaveAssets)`：并集写的便捷入口，可选择保存资产。
* `MCP_UnitPlanCreate(CreateSpecJson)`：基于模板单位生成克隆计划和 `diff_from_template`。
* `MCP_UnitPreviewDiff(PlanId)`：顶层返回 `diff` 或 `diff_from_template`，也返回完整 `plan`。
* `MCP_UnitApplyPlan(PlanId, bSaveAssets)`：唯一会真正修改/创建资产的单位计划入口。
* `MCP_UnitClone(...)`：便捷入口，正式批量流程优先使用 plan/create/apply。
* `MCP_UnitDeleteSoft(UnitPath, OptionsJson)`：软删除为移动资产；`{"dry_run":true}` 只返回目标路径和引用者，不移动。
* `MCP_UnitPlanDelete(UnitPath, OptionsJson)`：生成删除计划；默认 `mode:"soft"`，`mode:"hard"` 会永久删除且默认阻止有引用资产。
* `MCP_UnitDelete(UnitPath, OptionsJson)`：删除便捷入口；默认 `dry_run:true`。

Patch 路径保持源码属性路径，例如 `Move.XY.MoveSpeed`、`Attack.Range`。支持 `set`、`multiply`、`add/increase`、`subtract/decrease`。

并集写输入可以直接使用局部 `Data` JSON：

```json
{
  "Data": {
    "Move": {
      "XY": {
        "MoveSpeed": 1234
      }
    },
    "Attack": {
      "Range": 4321,
      "CoolDown": 1.75
    },
    "Damage": {
      "Damage": 9
    }
  }
}
```

该输入只会展开并更新 `Move.XY.MoveSpeed`、`Attack.Range`、`Attack.CoolDown`、`Damage.Damage`；未出现的字段保持不变。默认会写入 `expected_before`，因此计划生成后如果目标值被其他流程改过，应用时会报冲突。

### 3.3 官方 Demo 验证

默认扫描根为 `/MassBattle/Demo` 和 `/MassBattle/Test`。在当前项目中，UE AssetRegistry 实际识别到 22 个 `UMassBattleAgentConfigDataAsset`：

* `MCP_UnitList`：`count=22`。
* `MCP_UnitExport`：导出 22 行 JSON/CSV。
* `MCP_UnitPlanUpdate`：可为 `Move.XY.MoveSpeed` 生成可预览 diff。
* `MCP_UnitPlanCreate`：可基于 Demo Tank 单位生成克隆计划，未应用时不会创建资产。
* `MCP_UnitDeleteSoft`：`dry_run=true` 验证不移动资产。
* `MCP_UnitPlanMergeUpdate`：可对日本步兵局部生成并集写 diff。
* `MCP_UnitDelete`：已通过临时克隆资产验证 hard delete；真实单位默认仍建议使用 `dry_run` 或 soft delete。

### 3.4 MCP 版本 MassBattle 单位编辑器

`UMassBattleUnitEditorMCPApi` 是 `MassBattleFrame/Source/MassBattleEditor` 的 MCP 编排层，目标是复用官方编辑器模块里已经存在的资产转换逻辑，提供一个非 UI 的单位编辑器版本。它不依赖 `UmgMcp`；任意 MCP transport 只要能调用这些 JSON UFUNCTION 即可。

配置目录分两类：

* `Resources/UnitManagementStyles`：单位管理风格 profile。控制默认扫描根、项目扫描根、simple JSON 返回哪些顶层源码字段、默认导出哪些平衡字段，以及生成资产的组织路径/命名前缀。
* `Resources/UnitAuthoringRecipes`：单位创作/编辑 recipe。描述 MassBattleEditor 函数链路，例如 VAT 骨骼单位生成、给已有单位添加动画组。

当前 editor API：

* `MCP_EditorListProfiles(OptionsJson)`：列出 style profile 与 authoring recipe。
* `MCP_EditorGetProfile(ProfileType, ProfileId)`：读取一个 profile/recipe 的 JSON。
* `MCP_EditorPlanUnitAuthoringWorkflow(SpecJson)`：总计划入口。根据 `workflow_id` 与 `include_prepare/include_add_animations/include_create_vat/include_organize` 把购买素材准备、已有单位动画更新、VAT 单位创建/刷新和关联资产整理串成一个可审查计划。该入口会把 recipe 文件夹里的阶段工具和 style 文件夹里的默认路径/命名配置联动起来。
* `MCP_EditorApplyUnitAuthoringWorkflow(SpecJson, bSaveAssets)`：总应用入口，默认 `dry_run:true`。非 dry-run 时按 prepare → create/refresh VAT → add animations → organize 的顺序执行已审查工作流；计划不可应用时默认拒绝执行，除非显式 `allow_partial:true`。
* `MCP_EditorPlanPreparePurchasedAsset(SpecJson)`：针对新购买的 `USkeletalMesh` 素材包，按 style 解析源资产目录，调用 `MCP_FindAndFillOriginalTextures` 和 `MCP_FindAndFillAnimSequences`，再按 MassBattleEditor 官方命名规则生成 SkeletalMesh、贴图、动画的 rename/move 计划。默认目标目录来自 style 的 `authoring_defaults.source_folder_name`。
* `MCP_EditorApplyPreparePurchasedAsset(SpecJson, bSaveAssets)`：应用已审查的购买素材准备计划。`SpecJson` 默认 `dry_run:true`，正式执行时只处理 `status=would_rename` 的项，不覆盖目标已有资产。
* `MCP_EditorDiscoverCompatibleAnimations(SkeletalMeshPath, OptionsJson)`：按显式 `animation_search_path`、`animation_search_roots` 和 style 的 `authoring_defaults.animation_search_roots` 检索兼容 `AnimSequence`，返回候选根、选中根、命中数量和 `found_anims`。
* `MCP_EditorPlanAddAnimationsToUnit(UnitPath, SpecJson)`：先调用 `MCP_EditorDiscoverCompatibleAnimations`，再调用 `MCP_CreateAnimsDataFromSequences`，生成 `Data.AnimShared.AnimData` 的并集写计划。
* `MCP_EditorValidateAddAnimationsToUnit(UnitPath, SpecJson)`：验证动画组编辑是否能生成可应用计划。会返回 `valid`、`issues` 和完整 `plan_result`。
* `MCP_EditorApplyAddAnimationsToUnit(UnitPath, SpecJson, bSaveAssets)`：在计划可应用时执行动画组并集写。
* `MCP_EditorPlanCreateVatUnit(SpecJson)`：按 `vat_skeletal_unit` recipe 做非破坏式规划，返回目标路径、素材发现结果、LOD/动画数据转换结果、`unit_patch`，并在提供 `target_unit` 时生成已有单位 merge plan，在提供 `template_unit` 时生成克隆计划。目标路径会按 style profile 的 `organization.families` 解析，例如 `infantry` 默认落到 `Army/Soldier`。
* `MCP_EditorValidateCreateVatUnit(SpecJson)`：验证 VAT 单位工作流的必填项、资产存在性、路径冲突、Renderer 依赖和单位写入计划。返回 `valid`、`issues`、`execution_preview` 和完整 `plan`。
* `MCP_EditorApplyCreateVatUnit(SpecJson, bSaveAssets)`：执行 `vat_skeletal_unit` 工作流。默认 `dry_run:false` 时会按计划调用 `MCP_ConvertSkeletalMeshToStaticMeshWithLODs`、`MCP_CreateMaterialInstanceForStaticMeshWithLODs`、`MCP_DuplicateClassAsset`、`MCP_SetClassDefaultProperties` 和单位 `MCP_UnitApplyPlan`；传 `{"dry_run":true}` 时只返回执行计划，不修改资产。
* `MCP_EditorPlanOrganizeUnitAssets(UnitPath, OptionsJson)`：读取单位 JSON 中的显式对象引用，并通过 AssetRegistry 收集单位、Renderer Blueprint、StaticMesh、材质实例等可管理依赖；默认还会扫描同目录同 `asset_slug` 的生成物，例如 VAT DataAsset、AnimDataTex 和 VAT 贴图，然后按 style profile 规划移动到目标生成目录。
* `MCP_EditorApplyOrganizeUnitAssets(UnitPath, OptionsJson, bSaveAssets)`：应用已审查的关联资产整理计划。`OptionsJson` 默认 `dry_run:true`，不会覆盖目标已有资产，且默认阻止移动插件内容。

执行入口的安全选项：

* `overwrite_existing` 默认 `false`。已有 StaticMesh / Renderer class 默认跳过，不覆盖。
* `refresh_materials` 默认跟随 `overwrite_existing`。如果已有 StaticMesh 且不刷新材质，则不会重建材质实例。
* 新 Renderer class 不存在时需要 `source_renderer_class` / `renderer_template_class`，否则执行会失败并阻止写入无效 `Visualize.RendererClass`。
* 没有 `target_unit` 时需要 `template_unit`，执行会先克隆单位，再对克隆单位做并集写。
* `bSaveAssets=true` 时会保存生成/更新的 StaticMesh、Renderer Blueprint 以及 StaticMesh 当前引用的材质实例；单位 DataAsset 的保存仍由 `MCP_UnitApplyPlan` 控制。
* `dry_run:true` 会调用 `MCP_EditorValidateCreateVatUnit` 并返回 `execution_preview`，因此可以作为正式执行前的预检入口。
* `MCP_EditorApplyOrganizeUnitAssets` 默认 `dry_run:true`。正式移动只处理计划中 `status=would_move` 的项；`blocked_conflict`、`blocked_plugin_content` 等状态会让计划不可应用。
* `MCP_EditorApplyPreparePurchasedAsset` 默认 `dry_run:true`。它负责源资产命名和归档，不声称完成 VAT bake；`MCP_EditorPlanAddAnimationsToUnit` 仍要求动画已经存在于对应 `AnimToTextureDataAsset` 才能生成有效 `AnimShared`。
* `MCP_EditorApplyUnitAuthoringWorkflow` 默认 `dry_run:true`，适合自然语言请求先汇总完整计划。真实执行前应先检查 `steps[].result`、`issues` 和 `applicable`。

动画写入的默认保护：

* 如果所有候选根都没有找到兼容动画，`MCP_EditorPlanAddAnimationsToUnit` 默认返回 `ready_to_plan:false`，不会生成会把 `AnimShared` 写成 `-1/0` 的计划。
* `MCP_CreateAnimsDataFromSequences` 会返回 `found_animation_count`、`resolved_animation_count` 和 `unresolved_animation_count`。如果兼容动画不在指定 `AnimToTextureDataAsset` 中，`MCP_EditorPlanAddAnimationsToUnit` 默认返回 `ready_to_plan:false`；只有显式传 `allow_unresolved_animation_data:true` 才会放宽。
* `MCP_EditorPlanCreateVatUnit` 在没有动画时会跳过 `AnimShared.AnimData` 生成，只保留 `Visualize`、`LODShared.RenderLOD` 等可安全规划的数据。
* 如果确实需要生成空动画数据，必须显式传 `{"allow_empty_anims": true}`。

VAT 单位创建计划使用的源码字段保持与 MassBattle 一致：

```json
{
  "Data": {
    "Visualize": {
      "RendererClass": "/Game/Unit/Actor/Army/Soldier/Gen_Foo/Renderer_Foo.Renderer_Foo_C"
    },
    "LODShared": {
      "RenderLOD": {
        "Data0": {
          "ScreenSize": 1,
          "LODIndex": 0,
          "AnimBlendLevel": 2
        }
      }
    },
    "AnimShared": {
      "AnimData": {
        "IdleAnimData": {
          "Anim0": {
            "X": 0,
            "Y": 1,
            "Z": 0,
            "W": 30
          }
        }
      }
    }
  }
}
```

写入仍遵循计划优先：editor API 负责调用官方编辑器函数、组织目标路径并生成 patch；真正修改 DataAsset 仍通过 `MCP_UnitApplyPlan` 或 `MCP_UnitMergeUpdate` 完成。
