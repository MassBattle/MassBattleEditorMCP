# MassBattleEditorMCP

在我读大学的时候接触到了 RTS 游戏，这段经历让我更擅长从全局看问题，而不是只盯着单一细节。  
后来的 AI 时代又让我下了一个很实用的判断：RTS 的思路更适合理解人机关系。  
人类负责战略规划，AI 负责战术执行，二者分工清晰、目标可控、效率更高。

如果把真实世界看成一个复杂的简化模拟，那么 Mass Battle 的游戏世界就是很好的近似。  
当单位规模到达上万后，这个结论更明显：越高层次的战略思考越重要，越底层的战术细节越适合系统化交给工具链处理。

## 插件定位：服务于 Mass Battle 的大规模战斗插件

Mass Battle 插件目标是以“上万单位”为规模进行 RTS 模拟，既要拉满性能，也要保持通用性。  
这意味着它的实现方式不能和低规模、低复杂度的传统玩法逻辑直接混用，很多流程都需要“转译”。

常见的转译场景包括：  
单位创建器转译  
配置表转译  
特效批处理转译  
表现链路转译（动画、材质、Mesh、FX）

这些转译在传统流程里往往非常耗时、容易出错，也容易因改动小而引入回归。  
所以我们需要一个工具层，把重复且繁琐、规则又明显的工作交给 AI，让开发者把精力放回策略设计、玩法平衡、玩法验证这些更高层问题。

MassBattleEditorMCP 的作用，就是在 Mass Battle 的工作流里，把“战术层素材和配置转译”做成可调用、可复查、可批量执行的机制。  
开发者不必再把大量时间花在逐个单位、逐个特效的手工适配上，而是能更快确认规则是否符合既定策略目标。

## 现在可用的核心能力

插件以“先查后改、先计划后应用、改后可回读”为默认节奏，强调可重复和可追溯。  
你可以用它完成单位表现与数值的差异化改动，也可以快速把非批处理素材适配到可扩展的批处理系统。

## 使用入口

在线文档（中文）：`https://winyunq.github.io/MassBattleEditorMCP/`  
本地文档入口：`Document/index.html`（在 `Document` 分支）

## AI Skills

仓库内置可给其他 AI 直接使用的 Codex skills，位置在 `skills/`：

`skills/massbattle-unit-authoring`：指导 AI 使用 Unit MCP 创建、读取、计划、合并、删除和验证 MassBattle 单位配置。  
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
安装成功后应能看到 `massbattle-editor-mcp`，并可调用 `unit_get`、`unit_plan_merge_update`、`unit_apply_plan`、`effect_asset_read_summary`、`batch_fx_set_renderer_defaults` 等原语工具。

注意：`FFxConfig.AgentBehaviorState` 使用的是 `EAgentBehaviorState`，可写值包括 `None`、`Appearing`、`Sleeping`、`Patrolling`、`Attacking`、`Hit`、`Dying`。受击 FX 应写 `Hit`，不要把运行时 flag 名 `BeingHit` 写进这个字段。

## MCP 功能清单

### 连接与诊断

- `massbattle_ping`：确认 Codex MCP server 能连接 UE 编辑器 bridge。
- `unit_get_api_status`：读取 Unit MCP 能力表。
- `effect_asset_get_api_status`：读取 Effect/Batch FX MCP 能力表。
- `niagara_get_api_status`：读取 Niagara MCP 能力表。

### Unit MCP

- `unit_list`：列出 `MassBattleAgentConfigDataAsset` 单位配置资产。
- `unit_get`：读取一个单位配置，支持 simple/full 视图和默认过滤。
- `unit_get_schema`：读取单位可编辑字段、类型、角色和 tooltip。
- `unit_find_assets`：按单位制作场景查找 SkeletalMesh、Renderer、Niagara 等候选资产。
- `unit_plan_merge_update`：对单位配置生成并集写入计划，只产生 diff，不直接写资产。
- `unit_preview_diff`：读取已保存计划的 diff。
- `unit_apply_plan`：应用已审核计划并保存资产。

### Unit Editor MCP

- `editor_get_status`：读取单位编辑工作流能力。
- `editor_list_profiles`：列出风格 profile 和 authoring recipe。
- `editor_get_profile`：读取指定 profile 或 recipe。

### Effect Asset MCP

- `effect_asset_query`：按 `query/root/classes/limit` 查找 Niagara、Cascade、Blueprint、Material、Texture、Sound 等视觉相关资产。
- `effect_asset_read_summary`：读取未知类型特效资产摘要；Cascade 会返回 emitter、LOD、module 和依赖。
- `effect_asset_export_text`：导出确定性文本，供 AI 精读和复核。
- `effect_duplicate_asset`：加法复制资产，不删除或覆盖源资产。

### Niagara MCP

- `niagara_query`：按路径或名称查找 Niagara System。
- `niagara_read_summary`：读取 Niagara system、emitter、renderer、user parameter、module 摘要。
- `niagara_read_module`：精读指定 Niagara module 节点和 pin。
- `niagara_export_text`：导出 Niagara 确定性文本。
- `niagara_merge_write`：并集写 Niagara 属性，不负责删除。
- `niagara_delete`：显式删除 renderer、user parameter、禁用 emitter 等。

### Batch FX MCP

- `batch_fx_set_renderer_defaults`：设置 `AMassBattleFxRenderer` 蓝图默认值，包括 `NiagaraSystemAsset`、`NDC_BurstFx`、`SubType`、batch size 和 pooling cooldown。

批处理 FX 的闭环是：读取/复制参考特效资产，准备 batched Niagara/NDC/Renderer 蓝图，由用户把 renderer actor 放进测试关卡，再用 Unit MCP 把 `FFxConfig` 写入 `Hit.SpawnFx`、`Death.SpawnFx`、`Attack.SpawnFx` 等数组。MCP 不负责自动修改当前关卡布局。
