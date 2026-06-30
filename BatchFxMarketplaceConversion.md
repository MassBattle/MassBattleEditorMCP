# 市场特效转 MassBattle 批处理特效流程

本文档描述的是 skill 如何组合 MCP 工具。MCP 本身只提供低层能力，像命令行一样查询、读取、复制、写默认值，不负责做“一键转换”。

## 目标

把未知来源的市场特效转成 MassBattle 可批处理运行的视觉效果。输入可能是 Niagara、Cascade 粒子、材质、Blueprint、Mesh、贴图或这些资产的组合。输出通常是：

- 一个批处理 Niagara System。
- 一个 Niagara Data Channel 资产，或复用已有 NDC。
- 一个 `AMassBattleFxRenderer` Blueprint，负责在运行时注册 `SubType` 并承载 Niagara。
- 一个写入单位配置的 `FFxConfig`，由 Unit MCP 完成。

## MCP 工具边界

Effect MCP 提供这些低层工具：

- `MCP_EffectAssetQuery(QueryJson)`: 在未知市场包里查询视觉相关资产。
- `MCP_EffectAssetReadSummary(AssetPath, OptionsJson)`: 读取资产摘要。Cascade 会输出 emitter、LOD、module 结构。
- `MCP_EffectAssetExportText(AssetPath, OptionsJson)`: 把资产摘要和依赖写成确定性文本，便于精读和留档。
- `MCP_EffectDuplicateAsset(SourceAssetPath, NewAssetName, PackagePath, bSaveAssets)`: 复制模板或参考资产。
- `MCP_BatchFxSetRendererDefaults(TargetClassPath, NiagaraSystemPath, NdcBurstFxPath, SubType, RenderBatchSize, PoolingCooldown, bSaveAssets)`: 设置批处理特效渲染器 Blueprint CDO。

Effect MCP 不直接修改单位。单位上的 `Hit.SpawnFx`、`Death.SpawnFx` 等配置应由 Unit MCP 按它的 schema 写入。

## 判断批处理形态

先判断视觉语义，再选择 MassBattle 批处理原语：

- `Burst`: 爆炸、死亡火焰、受击火花、枪口火光、落点冲击。特点是事件触发、短生命周期、位置数组写入 NDC。
- `Attached`: 选择圈、持续燃烧、buff 光环、持续拖尾。特点是跟随单位，需要 acquire/release 或隐藏数组。
- `Material/VAT`: 闪白、溶解、受伤变色、攻击动画。特点是主要由材质参数、Custom Primitive Data、VAT 参数驱动，不一定需要 Niagara。

爆炸火焰用于单位死亡或受击时，优先做成 `Burst`。

## 示例：爆炸/火焰死亡特效

当前工程里已经能找到非批处理和批处理两套爆炸参考：

- 非批处理参考：`Plugins/MassBattleFrame/Content/Demo/SharedFx/Explosion/Standard/NS_Explosion.uasset`
- 非批处理参考：`Plugins/MassBattleFrame/Content/Demo/SharedFx/Explosion/Standard/CP_Explosion.uasset`
- 批处理参考：`Plugins/MassBattleFrame/Content/Demo/SharedFx/Explosion/Batched/NS_FxRenderer_Explosion.uasset`
- 批处理参考：`Plugins/MassBattleFrame/Content/Demo/SharedFx/Explosion/Batched/NDC_Explosion.uasset`
- 批处理参考：`Plugins/MassBattleFrame/Content/Demo/SharedFx/Explosion/Batched/BP_FxRenderer_Explosion.uasset`

如果明天只想验证“单位死亡触发批处理爆炸火焰”，最快路线是复用已有 batched explosion 资产，然后用 Unit MCP 把单位死亡特效指向它的 `SubType`。

### 1. 查询市场包或参考目录

先用宽类别查，不要假设它一定是 Niagara：

```json
MCP_EffectAssetQuery({
  "root": "/MassBattle/Demo/SharedFx/Explosion",
  "query": "Explosion",
  "classes": ["niagara", "cascade", "material", "texture", "blueprint"],
  "limit": 50
})
```

如果是从虚幻商城导入的包，把 `root` 换成该包的挂载路径，例如 `/Game/MarketplacePack`。以工具返回的 `path` 为准，不要手写猜测 object path。

### 2. 读取原特效

对入口资产做粗读：

```json
MCP_EffectAssetReadSummary(
  "/MassBattle/Demo/SharedFx/Explosion/Standard/CP_Explosion.CP_Explosion",
  "{\"include_dependencies\":true,\"include_module_properties\":false}"
)
```

如果粗读看不出行为，再精读模块属性：

```json
MCP_EffectAssetExportText(
  "/MassBattle/Demo/SharedFx/Explosion/Standard/CP_Explosion.CP_Explosion",
  "{\"include_dependencies\":true,\"include_module_properties\":true,\"include_reflected\":true}"
)
```

导出的文本会写到 `Saved/MassBattleEditorMCP/EffectText/`。对爆炸类 Cascade，重点看 emitter 数量、spawn 数量、生命周期、初速度、颜色、SubUV 材质和依赖贴图。

### 3. 生成或复制批处理资产

快速验证可以复用现有批处理爆炸。正式从市场包转换时，复制模板到项目目录：

```json
MCP_EffectDuplicateAsset(
  "/MassBattle/Core/FxRenderer/NS_FxRendererTemplate.NS_FxRendererTemplate",
  "NS_FxRenderer_MarketExplosion",
  "/Game/MassBattleGenerated/BatchFx/MarketExplosion",
  true
)
```

```json
MCP_EffectDuplicateAsset(
  "/MassBattle/Core/FxRenderer/BP_FxRendererTemplate.BP_FxRendererTemplate",
  "BP_FxRenderer_MarketExplosion",
  "/Game/MassBattleGenerated/BatchFx/MarketExplosion",
  true
)
```

Niagara 本体需要按参考语义重做：读取 Burst NDC 的位置、朝向、缩放、样式数组，然后在 Niagara 内产生火焰、烟、火星等 emitter。这里使用 Niagara MCP 做模块级读取/写入；如果现有 Niagara 写入粒度不够，再补窄工具，不要做一个“转换所有市场特效”的大工具。

### 4. 配置批处理渲染器

给复制出的 `AMassBattleFxRenderer` Blueprint 设置 Niagara、NDC 和 `SubType`。示例选择 `31` 作为待测子类型，实际项目里应避开已占用值。

```json
MCP_BatchFxSetRendererDefaults(
  "/Game/MassBattleGenerated/BatchFx/MarketExplosion/BP_FxRenderer_MarketExplosion.BP_FxRenderer_MarketExplosion",
  "/Game/MassBattleGenerated/BatchFx/MarketExplosion/NS_FxRenderer_MarketExplosion.NS_FxRenderer_MarketExplosion",
  "/MassBattle/Core/FxRenderer/NS_Modules/NDC_BurstFx.NDC_BurstFx",
  31,
  2048,
  3.0,
  true
)
```

也可以用已有爆炸参考资产：

```json
MCP_BatchFxSetRendererDefaults(
  "/MassBattle/Demo/SharedFx/Explosion/Batched/BP_FxRenderer_Explosion.BP_FxRenderer_Explosion",
  "/MassBattle/Demo/SharedFx/Explosion/Batched/NS_FxRenderer_Explosion.NS_FxRenderer_Explosion",
  "/MassBattle/Demo/SharedFx/Explosion/Batched/NDC_Explosion.NDC_Explosion",
  31,
  2048,
  3.0,
  true
)
```

`AMassBattleFxRenderer::BeginPlay` 会把自身注册到 `MassBattleSubsystem->FxRenderers`，所以测试关卡里必须放一个该 Blueprint 的 actor 实例。只有写了单位 `FFxConfig` 但关卡里没有 renderer actor 时，不会播放批处理特效。

### 5. 用 Unit MCP 应用到日本步兵

Effect MCP 不直接改单位。用 Unit MCP 先读日本步兵配置和 schema，再把死亡或受击特效追加到对应 `FFxConfig`。

概念配置如下，最终字段名以 Unit MCP schema 为准：

```json
{
  "Death": {
    "SpawnFx": [
      {
        "bEnable": true,
        "SoftNiagaraAsset": "",
        "SoftCascadeAsset": "",
        "SubType": 31,
        "StyleType": 0,
        "bAttached": false,
        "Quantity": 1,
        "Delay": 0.0,
        "LifeSpan": 1.2,
        "SpawnProbability": 1.0,
        "Scale": [1.0, 1.0, 1.0]
      }
    ]
  }
}
```

受击测试同理，把目标改成 `Hit.SpawnFx`，生命周期可以缩短到 `0.35` 到 `0.6` 秒，缩放可以降低到 `0.4` 到 `0.7`。

## 明天测试检查表

1. 打开测试关卡，确认存在 `BP_FxRenderer_Explosion` 或新复制的 renderer actor。
2. 确认 renderer 的 `SubType.Index` 与单位 `FFxConfig.SubType` 一致。
3. 确认 `NDC_BurstFx` 或 `NDC_Explosion` 已设置。
4. 确认单位配置里的 `SoftNiagaraAsset` 和 `SoftCascadeAsset` 为空，避免走非批处理路径。
5. 触发死亡或受击事件，观察 Niagara 是否读取到 Burst 数据。

## 什么时候继续补 MCP

只在低层访问缺失时补工具：

- 市场特效入口是 Blueprint，并且逻辑在节点里设置材质参数：补 Blueprint graph 读取工具。
- 视觉主要是材质溶解、闪白、燃烧边缘：补 Material graph 读取工具。
- 需要精确写 Niagara 模块栈，而现有 Niagara MCP 不够：补模块级写入工具。
- 需要替换或移除错误资产：用专门删除工具。普通写入工具保持并集/追加语义，不顺手删除旧内容。

这个流程的核心是：读取原实现表达的视觉意图，然后用 MassBattle 的批处理输入重新实现，而不是机械修改原资产参数。
