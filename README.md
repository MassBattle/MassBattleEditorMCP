# MassBattleEditorMCP Documents

本目录记录 MCP 之上的自然语言工作流。MCP 只提供低层“手”：查询、读取、计划、并集写、复制、设置默认值、验证。用户自然语言需求先匹配到下面的 order，再由 skill/文档组合这些 MCP 工具执行。

## Order 1: 编辑单位表现

用户用自然语言说明“某个单位看起来/表现上要改什么”，例如：

- 把日本步兵死亡时加一个爆炸火焰。
- 把中国步兵皮肤换成另一套材质。
- 把城市单位显示成某个国家旗帜。
- 调整单位 renderer、Niagara、死亡/受击/攻击特效、动画、LOD 或视觉缩放。

执行方式：

1. 用 Unit MCP 查询目标单位，默认读取 simple JSON；复杂表现字段只读取指定对象，例如 `Visualize`、`AnimShared`、`Attack`、`Hit`、`Death`。
2. 用 Effect MCP 或 Editor MCP 查询可用素材：renderer Blueprint、Niagara、Cascade、材质、skeletal mesh、static mesh、动画。
3. 只写用户要求的表现字段，使用 `MCP_UnitPlanMergeUpdate` 生成计划，再用 `MCP_UnitApplyPlan` 应用。
4. 对 `SpawnFx`、`SpawnProjectile`、`PlaySound` 等数组，默认允许并集追加；重复验证时应更新同一槽位，避免无限追加。
5. 写后必须读回目标对象，确认路径、SubType、StyleType、renderer class、材质或数组项已生效。

验收标准：

- MCP plan/apply/readback 均成功。
- 输出明确列出改动的单位路径和字段。
- 对批处理 FX，单位 `FFxConfig` 不应误填 unbatched Niagara/Cascade，除非用户明确要求非批处理特效。

## Order 2: 调整数值平衡

用户用自然语言说明“某类单位数值怎么平衡”，例如：

- 日本步兵太硬，血量降低 10%。
- 中国步兵射程提高一点但伤害不变。
- 军官要更稀有、更强，但攻速慢一些。
- 批量导出单位 JSON 用来做平衡表。

执行方式：

1. 用 Unit MCP 查询目标单位或单位集合，先返回 simple JSON，必要时导出详细对象。
2. 区分表现字段和数值字段。数值平衡通常修改 `Health`、`Damage`、`Attack`、`Trace`、`Move`、`Defence`、`Collider`、成本/生产相关字段。
3. 优先做小补丁：自然语言需求转成部分 JSON； omitted 字段保持原样。
4. 批量改动前先导出或保存 plan/diff，检查同一类单位是否采用一致公式。
5. 用 `MCP_UnitPlanMergeUpdate` 生成可审查 diff，再应用；写后读回 simple JSON 并汇总关键数值。

验收标准：

- 给出修改前后关键数值。
- 批量改动必须说明选择范围和排除范围。
- 不把视觉表现修改混入纯数值平衡 order，除非用户明确要求。

## Order 3: 市场特效转批处理特效

用户给出一个特效、特效包路径或视觉目标，要求转成 MassBattle 批处理特效，例如：

- 把这个市场爆炸特效做成批处理死亡爆炸。
- 参考某个 Cascade 火焰，做一个 MassBattle 可批处理的 Burst FX。
- 给某个持续光环做 Attached batch FX。

执行方式：

1. 用 `MCP_EffectAssetQuery` 在用户给的包路径内宽类别检索，不假设入口一定是 Niagara。
2. 用 `MCP_EffectAssetReadSummary` 或 `MCP_EffectAssetExportText` 读取候选资产，判断它是 Burst、Attached，还是应走材质/VAT 表现。
3. 复制或复用批处理 Niagara、NDC、`AMassBattleFxRenderer` Blueprint。
4. 用 `MCP_BatchFxSetRendererDefaults` 设置 renderer CDO：Niagara、NDC、SubType、batch size、pooling cooldown。
5. 确认测试关卡里存在对应 renderer actor；否则单位写了 `FFxConfig` 也不会播放。
6. 交给 Unit MCP 把 `FFxConfig` 写入 `Hit.SpawnFx`、`Death.SpawnFx`、`Attack.SpawnFx`、`Appear.SpawnFx` 等目标字段。
7. 读回 renderer defaults 和单位目标对象，确认 SubType/StyleType 对齐。

验收标准：

- 有一个批处理 renderer Blueprint 和一个明确 SubType。
- 有一个单位字段实际引用同一 SubType。
- 验证 JSON 证明 Effect MCP 与 Unit MCP 已联动。

参考文档：

- `BatchFxMarketplaceConversion.md`
