# MassBattleEditorMCP

[English](README.md)

我一直认为 RTS 是最适合思考 AI 社会的游戏类型：人类不应该被困在每一个低层动作里，而应该负责战略、约束、取舍和目标；AI 和工具链负责把这些目标拆成可执行的战术动作。未来真正稀缺的人，不是只会反复执行细节的人，而是能提出目标、判断方向、组织系统并承担后果的人。

这也是我喜欢 RTS 的原因。一个真正有野心的 RTS 不应该因为技术不够，就把原本成千上万的单位砍成几个可控对象，最后把“大规模战争”做成一个玩具。规模本身不是装饰，它会反过来决定玩法、战术空间、表现方式和工具链。

[Mass Battle Frame](https://github.com/MassBattle/MassBattleFrame) 非常符合我的胃口，因为它以极致性能为目标。只有底层性能足够强，我们才有资格把预算花在那些“浪费性能但创造体验”的东西上：更密集的单位、更复杂的表现、更大的战场、更丰富的战术反馈。如果你的 RTS 目标是上千甚至上万单位，那么 [Fab 上的 Mass Battle Frame](https://www.fab.com/listings/191850b4-44d3-4455-aa76-874bc0196a10?lang=zh-cn) 不是锦上添花，而是非常必要的基础设施。

一个插件优秀到一定程度，就会自然吸引业余爱好者围绕它继续开发。MassBattleEditorMCP 就是这样的证明：它不是 Mass Battle Frame 的运行时功能，而是给它补上一只可被 AI 使用的编辑器工具手。它把单位、特效、材质、Renderer、Niagara 和 DataAsset 的制作流程做成可调用、可回读、可批量执行的编辑器接口，让 AI 可以承担战术层素材转译，人类则继续负责设计判断。

如果你的游戏单位规模已经上千，这套生态就很有价值。前面的故事也许无聊，但它是真的：足够好的核心技术会吸引社区继续补齐周边工具。除了这个 MCP 插件，你还能看到更多围绕 Mass Battle / RTS 工作流的社区插件：

- [FogOfWar](https://github.com/MassBattle/FogOfWar)：Mass Battle 战争迷雾插件。
- [MassBattleMinimap](https://github.com/winyunq/MassBattleMinimap)：RTS 面板 / 小地图方向的插件。
- [LandmarkSystem](https://github.com/winyunq/LandmarkSystem)：单位初始编辑器与地标系统方向的插件。

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

`skills/massbattle-unit-authoring`：指导 AI 使用 Unit MCP 创建、读取、并集写入、删除和验证 MassBattle 单位配置。
`skills/massbattle-effect-mcp`：指导 AI 使用 Niagara / Effect MCP 查询、读取、导出、复制和配置批处理特效，并与 Unit MCP 联动。

安装到本机 Codex：

```powershell
Copy-Item -Recurse -Force .\skills\massbattle-unit-authoring $env:USERPROFILE\.codex\skills\
Copy-Item -Recurse -Force .\skills\massbattle-effect-mcp $env:USERPROFILE\.codex\skills\
```

如果设置了 `CODEX_HOME`，则复制到 `$env:CODEX_HOME\skills\`。
MCP 是编辑器工具接口，skill 只描述如何组合这些工具，不把 workflow 写成一个大按钮。

### Codex MCP Server 安装

MassBattleEditorMCP 的 Codex 入口由两层组成：

1. UE 编辑器插件内的本地 TCP bridge：默认监听 `127.0.0.1:55558`。
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
安装成功后应能看到 `massbattle-editor-mcp`，并可调用 `unit_get`、`unit_create`、`unit_write`、`unit_delete`、`effect_asset_read_summary`、`niagara_set_module_pin`、`batch_fx_read_renderer_defaults`、`batch_fx_set_renderer_defaults` 等原语工具。

注意：`FFxConfig.AgentBehaviorState` 使用的是 `EAgentBehaviorState`，可写值包括 `None`、`Appearing`、`Sleeping`、`Patrolling`、`Attacking`、`Hit`、`Dying`。受击 FX 应写 `Hit`，不要把运行时 flag 名 `BeingHit` 写进这个字段。

## MCP 功能清单

| 分类 | MCP 工具 | 状态 | 用途 |
| --- | --- | :---: | --- |
| 连接与诊断 | `massbattle_ping` | 可用 | 确认 Codex MCP server 能连接 UE 编辑器 bridge。 |
| 连接与诊断 | `unit_get_api_status` | 可用 | 读取 Unit MCP 能力表。 |
| 连接与诊断 | `effect_asset_get_api_status` | 可用 | 读取 Effect Asset / Batch FX MCP 能力表。 |
| 连接与诊断 | `niagara_get_api_status` | 可用 | 读取 Niagara MCP 能力表。 |
| Unit MCP | `unit_list` | 可用 | 列出 `MassBattleAgentConfigDataAsset` 单位配置资产。 |
| Unit MCP | `unit_get` | 可用 | 读取一个单位配置，支持 simple/full 视图和默认过滤。 |
| Unit MCP | `unit_get_schema` | 可用 | 读取单位可编辑字段、类型、角色和 tooltip。 |
| Unit MCP | `unit_export` | 可用 | 导出紧凑单位数值表，支持给平衡分析或批量复核使用。 |
| Unit MCP | `unit_find_assets` | 可用 | 按单位制作场景查找 SkeletalMesh、Renderer、Niagara 等候选资产。 |
| Unit MCP | `unit_create` | 可用 | 创建新单位；未指定模板时使用默认单位模板，可带初始单位数据。 |
| Unit MCP | `unit_write` | 可用 | 对已有单位按源码字段名并集写入局部 JSON，省略字段保持不变。 |
| Unit MCP | `unit_delete` | 可用 | 显式删除或软删除单位；默认 dry-run。 |
| Style MCP | `style_summarize_units` | 可用 | 按风格、单位族、路径类别汇总单位资产。 |
| Style MCP | `style_plan_organize_units` | 可用 | 生成按默认风格整理单位目录的计划，不直接移动资产。 |
| Unit Editor MCP | `editor_get_status` | 可用 | 读取单位编辑工作流能力。 |
| Unit Editor MCP | `editor_list_profiles` | 可用 | 列出风格 profile 和 authoring recipe。 |
| Unit Editor MCP | `editor_get_profile` | 可用 | 读取指定 profile 或 recipe。 |
| Unit Editor MCP | `editor_plan_organize_unit_assets` | 可用 | 计划把一个单位和关联生成资产移动到风格化目录。 |
| Unit Editor MCP | `editor_apply_organize_unit_assets` | 可用 | 应用已审核的单位资产整理计划；默认可 dry-run。 |
| Effect Asset MCP | `effect_asset_query` | 可用 | 按 `query/root/classes/limit` 查找 Niagara、Cascade、Blueprint、Material、Texture、Sound 等视觉相关资产。 |
| Effect Asset MCP | `effect_asset_read_summary` | 可用 | 读取未知类型特效资产摘要；Cascade 会返回 emitter、LOD、module 和依赖。 |
| Effect Asset MCP | `effect_asset_export_text` | 可用 | 导出确定性文本，供 AI 精读和复核。 |
| Effect Asset MCP | `effect_asset_soft_delete` | 可用 | 读取引用后把未引用资产软移动到 `_Trash`；默认 dry-run。 |
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

批处理 FX 的闭环是：读取/复制参考特效资产，准备 batched Niagara/NDC/Renderer 蓝图，MCP 写入并验证 renderer 蓝图默认值，由用户把 renderer actor 放进测试关卡，再用 Unit MCP 把 `FFxConfig` 写入 `Hit.SpawnFx`、`Death.SpawnFx`、`Attack.SpawnFx` 等数组。MCP 不负责自动修改当前关卡布局；只要用户不在关卡里覆盖实例参数，拖进去的 actor 应继承资产默认值。

## 默认风格与模板化工作流

默认风格 profile 位于 `Resources/UnitManagementStyles/default.massbattle_unit_style.json`。
它不是运行时功能，而是给 AI 和 MCP 工具使用的制作上下文：扫描根目录、单位组织规则、单位 authoring 默认值，以及批处理 FX 模板。
`unit_create` 未传 `template_unit` 时，会读取 `authoring_defaults.default_unit_template` 作为默认单位模板；项目应在使用前配置这个路径。

批处理 FX 不应该从空白资产开始。推荐流程是：

1. 用 `editor_get_profile` 读取 `style/default`。
2. 从 `batch_fx_templates` 选择最接近目标的模板，例如 `mesh_burst_hit`、`sprite_explosion_burst`、`projectile_muzzle_burst`。
3. 用 `effect_duplicate_asset` 复制模板 Niagara 和 renderer Blueprint。
4. 用 `niagara_set_emitter_enabled` / `niagara_delete` 关闭不需要的 emitter。
5. 用 `niagara_merge_write` 写 renderer、emitter data、bounds 等 UObject 属性。
6. 用 `niagara_set_module_pin` 微调模块输入 pin，例如 Lifetime、Scale、Spawn Count 或随机范围。
7. 用 `batch_fx_set_renderer_defaults` 写 renderer CDO 的 Niagara、NDC、SubType 和 batch size。
8. 用 `unit_write` 把单位 `FFxConfig` 指向对应 `SubType`。

这个模式和代码 diff 类似：模板表达大部分结构，MCP 只做可复查的小改动。
