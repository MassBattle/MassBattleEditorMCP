# MassBattleEditorMCP

[English](README.md)

我一直认为 RTS 是最适合思考 AI 社会的游戏类型：人类不应该被困在每一个低层动作里，而应该负责战略、约束、取舍和目标；AI 和工具链负责把这些目标拆成可执行的战术动作。未来真正稀缺的人，不是只会反复执行细节的人，而是能提出目标、判断方向、组织系统并承担后果的人。

这也是我喜欢 RTS 的原因。一个真正有野心的 RTS 不应该因为技术不够，就把原本成千上万的单位砍成几个可控对象，最后把“大规模战争”做成一个玩具。规模本身不是装饰，它会反过来决定玩法、战术空间、表现方式和工具链。

[Mass Battle Frame](https://github.com/MassBattle/MassBattleFrame) 非常符合我的胃口，因为它以极致性能为目标。只有底层性能足够强，我们才有资格把预算花在那些“浪费性能但创造体验”的东西上：更密集的单位、更复杂的表现、更大的战场、更丰富的战术反馈。如果你的 RTS 目标是上千甚至上万单位，那么 [Fab 上的 Mass Battle Frame](https://www.fab.com/listings/191850b4-44d3-4455-aa76-874bc0196a10?lang=zh-cn) 不是锦上添花，而是非常必要的基础设施。

一个插件优秀到一定程度，就会自然吸引业余爱好者围绕它继续开发。MassBattleEditorMCP 就是这样的证明：它不是 Mass Battle Frame 的运行时功能，而是给它补上一只可被 AI 使用的编辑器工具手。它把单位、特效、材质、Renderer、Niagara 和 DataAsset 的制作流程做成可调用、可回读、可批量执行的编辑器接口，让 AI 可以承担战术层素材转译，人类则继续负责设计判断。

如果你的游戏单位规模已经上千，这套生态就很有价值。前面的故事也许无聊，但它是真的：足够好的核心技术会吸引社区继续补齐周边工具。除了这个 MCP 插件，你还能看到更多围绕 Mass Battle / RTS 工作流的社区插件：

- [fogofwar](https://github.com/winyunq/FogOfWar)：战争迷雾插件，包含小地图插件和场景迷雾插件。
- [RTS InputSystem](https://github.com/winyunq/RTSInputSystem)：RTS 控制系统，包含 RTS 相机、可以在小地图上显示的镜头（可以与战争迷雾小地图联动），以及单位选择面板。
- [landmark](https://github.com/winyunq/LandmarkSystem)：地编场景系统，包含在特定坐标显示地编文本、在特定地区生成建筑，以及在特定地点生成指定数量的 Mass Battle Frame 单位。

## 插件定位：服务于 Mass Battle 的大规模战斗插件

MassBattleEditorMCP 的作用，是在 Mass Battle 工作流里把“战术层素材和配置转译”做成可调用、可回读、可批量执行的机制。它服务于单位创建、配置表转译、特效批处理转译，以及动画、材质、Mesh、FX 等表现链路转译。

开发者不必再把大量时间花在逐个单位、逐个特效的手工适配上，而是能更快确认规则是否符合既定策略目标。

## 现在可用的核心能力

插件以“先读、再创造或并集写入、必要时显式删除、改后可回读”为默认节奏，强调可重复和可追溯。
你可以用它完成单位表现与数值的差异化改动，也可以快速把非批处理素材适配到可扩展的批处理系统。

## 使用入口

在线文档：`https://github.com/MassBattle/MassBattleEditorMCP`
本地文档入口：`Document/index.html`（`Document` worktree / `Document` 分支）

## AI Skills

仓库内置可给其他 AI 直接使用的 Codex skills，位置在 `skills/`：

- `skills/massbattle-effect-mcp`：把任意源特效转成消费 MassBattleFrame 批处理输入的、忠于原效果的 Niagara。
- `skills/massbattle-instant-damage-fx`：制作由 Agent 在命中时刻直接结算的攻击，以及发射/命中的一次性 Burst FX。
- `skills/massbattle-projectile-authoring`：制作由 Mass Projectile Entity 负责飞行、碰撞、伤害和生命周期的攻击，以及 Attached 飞行 FX 与 Burst 生命周期 FX。
- `skills/massbattle-unit-authoring`：创建、编辑单位，并把已经验证的直接攻击或抛体配置接入单位数组。

这些 Skill 对 Niagara 批处理只有一个定义：

`BatchE([C0, C1, ..., Cn]) == [E(C0), E(C1), ..., E(Cn)]`

原特效的视觉图仍然是原特效；转译只把“逐单位生成并传 Transform”改成消费 MassBattleFrame 的 Burst NDC 或 Attached 数组输入。GPU Sim、对象池、复制模板、修改 Renderer CDO 或只填写 `SubType` 都不等于完成批处理转译。即时伤害与抛体伤害只是玩法归属不同，两者使用同一套 Niagara 转译定义。

安装到本机 Codex：

```powershell
Copy-Item -Recurse -Force .\skills\massbattle-unit-authoring $env:USERPROFILE\.codex\skills\
Copy-Item -Recurse -Force .\skills\massbattle-effect-mcp $env:USERPROFILE\.codex\skills\
Copy-Item -Recurse -Force .\skills\massbattle-instant-damage-fx $env:USERPROFILE\.codex\skills\
Copy-Item -Recurse -Force .\skills\massbattle-projectile-authoring $env:USERPROFILE\.codex\skills\
```

如果设置了 `CODEX_HOME`，则复制到 `$env:CODEX_HOME\skills\`。
MCP 是编辑器工具接口，skill 只描述如何组合这些工具，不把 workflow 写成一个大按钮。

### Codex MCP Server 安装

MassBattleEditorMCP 的 Codex 入口由两层组成：

1. UE 编辑器插件内的本地 TCP bridge：默认监听 `127.0.0.1:55258`。
2. `Resources/Python/MassBattleMcpServer.py`：STDIO MCP server，把 Codex tool call 转发给 UE bridge。

安装到 Codex：

```powershell
.\Scripts\Install-CodexMassBattleMCP.ps1
```

快速检查安装和 UE bridge：

```powershell
.\Scripts\QuickStart-CodexMassBattleMCP.ps1
```

安装后需要重启 Codex 或新开会话；UE 编辑器也需要加载本插件，bridge 才会开始监听。
安装成功后应能看到 `massbattle-editor-mcp`，并可调用 `unit_get`、`unit_create`、`projectile_get`、`projectile_write`、`projectile_validate`、`editor_apply_create_vat_unit_from_selection`、`effect_asset_read_summary`、`niagara_set_module_pin`、`batch_fx_read_renderer_defaults`、`batch_fx_set_renderer_defaults` 等原语工具。

单位制作的默认 AI 入口应对应官方 `/MassBattle/Core/MassBattleTools` 的 DoAll 按钮：一次 apply 调用完成 mesh、material、VAT texture、renderer、unit config 的创建或刷新。非 selection 入口要求完整 canonical 输入，并会在写任何资产前先 validate；缺字段、旧别名、静态兜底或未覆盖旧生成 mesh 都直接失败。plan/validate 工具保留给诊断和 dry-run，不是正常制作流程的必经步骤。

注意：`FFxConfig.AgentBehaviorState` 使用的是 `EAgentBehaviorState`，可写值包括 `None`、`Appearing`、`Sleeping`、`Patrolling`、`Attacking`、`Hit`、`Dying`。受击 FX 应写 `Hit`，不要把运行时 flag 名 `BeingHit` 写进这个字段。

## MCP 功能清单

| 分类 | MCP 工具 | 状态 | 用途 |
| --- | --- | :---: | --- |
| 连接与诊断 | `massbattle_ping` | 可用 | 确认 Codex MCP server 能连接 UE 编辑器 bridge。 |
| 连接与诊断 | `unit_get_api_status` | 可用 | 读取 Unit MCP 能力表。 |
| 连接与诊断 | `effect_asset_get_api_status` | 可用 | 读取 Effect Asset / Batch FX MCP 能力表。 |
| 连接与诊断 | `niagara_get_api_status` | 可用 | 读取 Niagara MCP 能力表。 |
| 连接与诊断 | `projectile_get_api_status` | 可用 | 读取 Projectile DataAsset 的 CRUD、Schema 与验证能力。 |
| Unit MCP | `unit_list` | 可用 | 列出 `MassBattleAgentConfigDataAsset` 单位配置资产。 |
| Unit MCP | `unit_get` | 可用 | 读取一个单位配置，支持 simple/full 视图和默认过滤。 |
| Unit MCP | `unit_get_schema` | 可用 | 读取单位可编辑字段、类型、角色和 tooltip。 |
| Unit MCP | `unit_export` | 可用 | 导出紧凑单位数值表，支持给平衡分析或批量复核使用。 |
| Unit MCP | `unit_find_assets` | 可用 | 按单位制作场景查找 SkeletalMesh、Renderer、Niagara 等候选资产。 |
| Unit MCP | `unit_create` | 可用 | 创建新单位；未指定模板时使用默认单位模板，可带初始单位数据。 |
| Unit MCP | `unit_write` | 可用 | 对已有单位按源码字段名并集写入局部 JSON，省略字段保持不变。 |
| Unit MCP | `unit_delete` | 可用 | 显式删除或软删除单位；默认 dry-run。 |
| Projectile MCP | `projectile_list` | 可用 | 在指定根目录列出 `MassBattleProjectileConfigDataAsset`。 |
| Projectile MCP | `projectile_query` | 可用 | 按路径或名称查询抛体配置。 |
| Projectile MCP | `projectile_get` | 可用 | 以 active-only 或完整源码对齐视图读取一个抛体。 |
| Projectile MCP | `projectile_get_schema` | 可用 | 读取抛体字段、枚举、编辑条件和写入规则。 |
| Projectile MCP | `projectile_create` | 可用 | 从类默认值或已有模板创建抛体；创建前先在瞬态副本上验证。 |
| Projectile MCP | `projectile_write` | 可用 | 对抛体并集写入局部补丁；数组追加/替换必须显式声明，写前验证。 |
| Projectile MCP | `projectile_validate` | 可用 | 验证运动、伤害归属、触发条件、生命周期 FX、关联单位与重复爆炸。 |
| Projectile MCP | `projectile_delete` | 可用 | 规划或执行受保护的软/硬删除；默认 dry-run。 |
| Style MCP | `style_summarize_units` | 可用 | 按风格、单位族、路径类别汇总单位资产。 |
| Style MCP | `style_plan_organize_units` | 可用 | 生成按默认风格整理单位目录的计划，不直接移动资产。 |
| Unit Editor MCP | `editor_get_status` | 可用 | 读取单位编辑工作流能力。 |
| Unit Editor MCP | `editor_list_profiles` | 可用 | 列出风格 profile 和 authoring recipe。 |
| Unit Editor MCP | `editor_get_profile` | 可用 | 读取指定 profile 或 recipe。 |
| Unit Editor MCP | `editor_plan_create_vat_unit` | 诊断 | 预览 DoAll 等价 VAT 单位计划；严格执行仍要求完整 canonical 输入。 |
| Unit Editor MCP | `editor_apply_create_vat_unit` | 可用 | 非 selection 的主生成入口，等价 DoAll；先验证完整 canonical 输入，再执行网格转换、VAT 材质创建、VAT 烘焙、Renderer 复制、单位配置创建或合并。 |
| Unit Editor MCP | `editor_plan_create_vat_unit_from_selection` | 诊断 | 从当前编辑器选择或 `selected_assets` 推导 DoAll spec，并返回可审查计划。 |
| Unit Editor MCP | `editor_apply_create_vat_unit_from_selection` | 可用 | AI 使用的“获取当前，生成”一键主入口。 |
| Unit Editor MCP | `editor_plan_organize_unit_assets` | 可用 | 计划把一个单位和关联生成资产移动到风格化目录。 |
| Unit Editor MCP | `editor_apply_organize_unit_assets` | 可用 | 应用已审核的单位资产整理计划；默认可 dry-run。 |
| Effect Asset MCP | `effect_asset_query` | 可用 | 按 `query/root/classes/limit` 查找 Niagara、Cascade、Blueprint、Material、Texture、Sound 等视觉相关资产。 |
| Effect Asset MCP | `effect_asset_read_summary` | 可用 | 读取未知类型特效资产摘要；返回依赖、引用和缺失项目依赖信息。 |
| Effect Asset MCP | `effect_asset_export_text` | 可用 | 导出确定性文本，供 AI 精读和复核。 |
| Effect Asset MCP | `effect_asset_soft_delete` | 可用 | 规划把未引用资产软移动到 `_Trash`；运行态移动默认阻止，需显式 force。 |
| Effect Asset MCP | `effect_duplicate_asset` | 可用 | 加法复制资产，不删除或覆盖源资产。 |
| Niagara MCP | `niagara_query` | 可用 | 按路径或名称查找 Niagara System。 |
| Niagara MCP | `niagara_read_summary` | 可用 | 读取 Niagara system、emitter、renderer、user parameter、module 摘要。 |
| Niagara MCP | `niagara_read_module` | 可用 | 精读指定 Niagara module 节点和 pin。 |
| Niagara MCP | `niagara_export_text` | 可用 | 导出 Niagara 确定性文本。 |
| Niagara MCP | `niagara_merge_write` | 可用 | 并集写 Niagara 属性，不负责删除。 |
| Niagara MCP | `niagara_set_module_pin` | 可用 | 写一个 Niagara FunctionCall 模块输入 pin 的默认值；默认拒绝已连接 pin。 |
| Niagara MCP | `niagara_set_emitter_enabled` | 可用 | 显式启用或禁用一个 Niagara emitter handle。 |
| Niagara MCP | `niagara_delete` | 可用 | 显式删除 renderer、user parameter、禁用 emitter 等。 |
| Batch FX MCP | `batch_fx_read_renderer_defaults` | 可用 | 读取 `AMassBattleFxRenderer` 蓝图默认值；这些默认值会被之后拖进关卡的新 Actor 实例继承。 |
| Batch FX MCP | `batch_fx_set_renderer_defaults` | 可用 | 设置 `AMassBattleFxRenderer` 蓝图默认值，包括 `NiagaraSystemAsset`、`NDC_BurstFx`、`SubType`、batch size 和 pooling cooldown。 |

忠于源特效的批处理闭环是：精读源资产，复制原 Niagara，以 `niagara_compare_systems(mode=exact)` 证明复制体与源一致，只增加 MassBattleFrame 输入适配，再以 `mode=translation` 验证保真和可运行性。模板近似只能作为单独的可选优化版本，不能算作原特效转译完成。转译后要验证 Renderer 蓝图默认值，并使用当前可用的关卡编辑工具把 Renderer Actor 放进测试关卡，再通过 Unit MCP 或 Projectile MCP 接入真实触发链；工具能完成的关卡工作不能再交还给用户手动处理。

直接攻击与抛体攻击不能同时拥有同一份伤害。常规抛体攻击应关闭 Agent 的命中时刻伤害，由 Projectile DataAsset 负责飞行、碰撞、伤害和生命周期。`projectile_validate` 还会拒绝完全相同且已启用的 `OnHit.SpawnFx` 与 `OnRemoval.SpawnFx`，因为两者可能在同一个模拟 Tick 执行并生成两次完整爆炸。

VAT 单位创建会先使用发现到的原始贴图；如果基于文件名的贴图发现漏掉源材质贴图，Editor MCP 可以在创建 VAT 材质实例前从源 SkeletalMesh 材质继承常见贴图参数和 used textures。`defaulted_original_textures_from_source_material` warning 是复核项，需要检查生成材质依赖是否指向预期贴图。

严格的非 selection VAT create 需要这些 canonical 字段：`skeletal_mesh`、`unit_name`、`target_package_path`、`parent_material`、`source_renderer_class`、`niagara_system`、`vat_sample_rate`、`animations`。刷新已有单位使用 `target_unit`；新建单位还必须提供 `template_unit`、`target_unit_package_path`、`subtype`。刷新已生成资产时应传 `overwrite_existing=true` 和 `refresh_materials=true`，否则旧 StaticMesh 会阻止执行，避免静默复用白材质/无动画资产。

VAT 单位的 MassBattleTools DoAll 对应关系：

1. `FillTex`、`FillLod`、`FillAnim` 解析贴图、LOD 设置和动画数组。
2. `CreateStaticMeshAndMat` 转换 SkeletalMesh，并创建 VAT 材质实例。
3. `CreateVATTextures` 创建或更新 AnimToTexture DataAsset，烘焙 VAT 贴图，写入 AnimDataTex，并更新材质实例。
4. `CreateDataAsset` 把 `Visualize`、`LODShared`、`AnimShared` 和运行时采样率默认值写入单位配置。
5. `CreateRenderer` 复制或更新 renderer 蓝图默认值。

apply 调用会先执行 validate，失败时不写资产。plan/validate 调用只是把这些步骤暴露出来供复核；普通 AI 命令应一次性提供完整 spec。

## 默认风格与可选模板工作流

默认风格 profile 位于 `Resources/UnitManagementStyles/default.massbattle_unit_style.json`。
它不是运行时功能，而是给 AI 和 MCP 工具使用的制作上下文：扫描根目录、单位组织规则、单位 authoring 默认值，以及批处理 FX 模板。
`unit_create` 未传 `template_unit` 时，会读取 `authoring_defaults.default_unit_template` 作为默认单位模板；项目应在使用前配置这个路径。

模板流程只用于全新特效或明确要求的优化复刻。若任务是忠于 Marketplace/客户源资产的转译，必须复制原 Niagara 并保留其 emitter、renderer、材质、曲线、事件和时序，模板不能替代源资产。

可选模板流程是：

1. 用 `editor_get_profile` 读取 `style/default`。
2. 从 `batch_fx_templates` 选择最接近目标的模板，例如 `mesh_burst_hit`、`sprite_explosion_burst`、`projectile_muzzle_burst`。
3. 用 `effect_duplicate_asset` 复制模板 Niagara 和 renderer Blueprint。
4. 用 `niagara_set_emitter_enabled` / `niagara_delete` 关闭不需要的 emitter。
5. 用 `niagara_merge_write` 写 renderer、emitter data、bounds 等 UObject 属性。
6. 用 `niagara_set_module_pin` 微调模块输入 pin，例如 Lifetime、Scale、Spawn Count 或随机范围。
7. 用 `batch_fx_set_renderer_defaults` 写 renderer CDO 的 Niagara、NDC、SubType 和 batch size。
8. 用 `unit_write` 把单位 `FFxConfig` 指向对应 `SubType`。

这个模式的产物应明确标记为“优化复刻/近似版本”，不能在保真转译验收数量中计数。
