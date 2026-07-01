# Demo 批处理特效学习与日本步兵升级流程

本文记录一次实际操作流程：先学习 MassBattleFrame Demo 中已有的批处理特效，再把测试用日本步兵升级为可验证的批处理特效链路。

## 结论

MassBattle 的批处理特效不是在单位资产里直接放一个 Niagara 或 Cascade。

正确链路是：

```text
单位配置 FFxConfig / SpawnProjectile
  -> HostSubsystem 生成 FxHost 或 ProjectileHost
  -> MassBattleFxRenderProcessor 聚合大量实例
  -> 关卡中的 AMassBattleFxRenderer 按 SubType 接收批量数据
  -> 一个或少量 NiagaraComponent 使用数组/NDC 批量渲染
```

所以测试时必须同时满足两件事：

1. 单位配置里的 `SubType` 或 projectile 配置指向正确的批处理子类型。
2. 测试关卡里放置了对应的 `BP_FxRenderer_*` Actor，并且它在 BeginPlay 时注册同一个 `SubType`。

如果只有单位配置，没有 renderer actor，运行时不会看到特效。

## 学习到的 Demo 资产

### 批处理爆炸

```text
/MassBattle/Demo/SharedFx/Explosion/Batched/BP_FxRenderer_Explosion
/MassBattle/Demo/SharedFx/Explosion/Batched/NS_FxRenderer_Explosion
/MassBattle/Demo/SharedFx/Explosion/Batched/NDC_Explosion
```

关键默认值：

```text
SubType: 31
RenderBatchSize: 1000
PoolingCooldown: 3
```

适用场景：受击爆炸、死亡爆炸、短生命周期火焰冲击。它是 `Burst` 形态，数据通过 NDC 写入。

### 批处理子弹

```text
/MassBattle/Demo/Projectile/Batched/Bullet/Renderer/BP_FxRenderer_Bullet
/MassBattle/Demo/Projectile/Batched/Bullet/Renderer/NS_FxRenderer_Bullet
/MassBattle/Demo/Projectile/Batched/Bullet/ProjectileConfig_Bullet_WarSim
```

关键默认值：

```text
SubType: 0
RenderBatchSize: 2048
PoolingCooldown: 3
NDC: /MassBattle/Core/FxRenderer/NS_Modules/NDC_BurstFx
```

`ProjectileConfig_Bullet_WarSim` 自己会在 `OnBirth`、`OnHit`、`OnRemoval` 中生成批处理 FX。单位攻击时不需要直接生成子弹 Niagara，只需要在 `Attack.SpawnProjectile` 中引用这个 projectile config。

### 选择圈

```text
/MassBattle/Demo/SharedFx/RingDecal/BP_FxRenderer_SelectRing
/MassBattle/Demo/SharedFx/RingDecal/NS_FxRenderer_SelectRing
```

关键默认值：

```text
SubType: 0
RenderBatchSize: 2048
PoolingCooldown: 3
```

选择相关视觉通常有两条路径：

1. AgentRenderer 内部通过动态参数/选择状态显示。
2. Unit `Select.SpawnOnSelected.SpawnFx` 生成批处理 FX。

实际项目要先读单位配置和 renderer 材质，不要假设所有选择框都来自 Niagara。

## Runtime 代码证据

`AMassBattleFxRenderer::BeginPlay` 会把自身注册到 MassBattleSubsystem：

```text
MassBattleSubsystem->FxRenderers.Add(SubType.Index, this)
```

`MassBattleFxRenderProcessor` 会按 `Reg.SubTypeIndex` 查找 renderer：

```text
MB.FxRenderers.Find(Reg.SubTypeIndex)
```

找到 renderer 后，processor 会把 burst/attached 数据写入数组或 NDC：

```text
LocationArray_Burst
OrientationArray_Burst
ScaleArray_Burst
StyleArray_Burst

LocationArray_Attached
OrientationArray_Attached
ScaleArray_Attached
IsHiddenArray_Attached
```

这说明性能优化点在于 renderer 聚合数据并复用 NiagaraComponent，而不是每个单位或每次攻击单独 spawn 一个 Niagara。

## 日本步兵升级

目标资产：

```text
/Game/Unit/Actor/Army/Soldier/Gen_SK_JapanSoldier/AgentConfig_SK_JapanSoldier.AgentConfig_SK_JapanSoldier
```

### 受击批处理 FX

把 `Hit.SpawnFx` 指向 Demo batched explosion：

```json
{
  "Hit": {
    "SpawnFx": [
      {
        "bEnable": true,
        "SubType": "SubType31",
        "StyleType": "Style0",
        "SoftNiagaraAsset": "",
        "SoftCascadeAsset": "",
        "Transform": {
          "Translation": {"X": 0, "Y": 0, "Z": 60},
          "Scale3D": {"X": 0.8, "Y": 0.8, "Z": 0.8}
        },
        "bAttached": false,
        "Quantity": 1,
        "Delay": 0,
        "LifeSpan": 1.2,
        "AgentBehaviorState": "Hit"
      }
    ]
  }
}
```

注意：

- `SoftNiagaraAsset` 和 `SoftCascadeAsset` 留空，避免走非批处理路径。
- `SubType31` 必须和 `BP_FxRenderer_Explosion` 的 SubType 一致。
- `AgentBehaviorState` 使用 `Hit`，不要写运行时 flag 名 `BeingHit`。

### 攻击批处理 projectile

把 `Attack.SpawnProjectile` 指向 Demo batched bullet：

```json
{
  "Attack": {
    "TimeOfHitAction": "None",
    "SpawnProjectile": [
      {
        "bEnable": true,
        "ProjectileConfigDataAsset": "/MassBattle/Demo/Projectile/Batched/Bullet/ProjectileConfig_Bullet_WarSim.ProjectileConfig_Bullet_WarSim",
        "Transform": {
          "Translation": {"X": 50, "Y": 0, "Z": 60},
          "Scale3D": {"X": 1, "Y": 1, "Z": 1}
        },
        "bAttached": false,
        "SpawnOrigin": "AtSelf",
        "Quantity": 1,
        "Delay": 0.35,
        "BindToAnimIndex": -1,
        "AgentBehaviorState": "None"
      }
    ]
  }
}
```

实战中可以放 3 条 projectile，延迟分别为 `0.35`、`0.60`、`0.85`，这样攻击动作更容易在测试中看见连续效果。

`TimeOfHitAction` 设为 `None` 的原因是：伤害和命中特效改由 projectile hit 链路处理，避免单位攻击时刻直接结算和 projectile 结算混在一起。

### 索敌调整

为了让测试更容易触发攻击：

```json
{
  "Trace": {
    "SectorTrace": {
      "Common": {
        "TraceRadius": 5000,
        "TraceHeight": 5000,
        "SortMode": "Randomize"
      }
    },
    "RandomDelayOnInit": {"Y": 1},
    "bSkipTraceWhileTargetValid": true,
    "bCheckLOSWhileTargetValid": true
  },
  "Attack": {
    "Range": 5000,
    "CoolDown": 1
  }
}
```

## 测试关卡要求

用户需要手动把下面两个 Blueprint Actor 拖进测试关卡：

```text
/MassBattle/Demo/Projectile/Batched/Bullet/Renderer/BP_FxRenderer_Bullet
/MassBattle/Demo/SharedFx/Explosion/Batched/BP_FxRenderer_Explosion
```

MCP 不负责把 actor 放进当前 editor world。放置属于关卡 authoring，用户手动处理更清晰，也能避免 MCP 修改错误关卡。

拖入后不要在关卡实例上覆盖 `SubType`、`NiagaraSystemAsset`、`NDC_BurstFx` 等默认值。只要实例没有覆盖，actor 会继承 Blueprint 默认值。

## 验证是否真的批处理

运行时观察点：

1. Outliner 中应该是少量 `BP_FxRenderer_*` actor，而不是每次攻击/受击都新增大量 Niagara Actor。
2. `stat niagara` 中 NiagaraComponent 数量不应随单位数量线性暴涨。
3. 单位配置中 `SoftNiagaraAsset`、`SoftCascadeAsset` 为空，说明单位没有直接走非批处理资产。
4. Renderer actor 的 `SubType` 与单位 `FFxConfig.SubType` 或 projectile 内部 FX SubType 一致。
5. 如果无效果，优先检查关卡里有没有对应 renderer actor，而不是先怀疑单位资产。

## MCP 操作顺序

推荐顺序：

1. `effect_asset_read_summary` 读取 Demo renderer Blueprint 依赖。
2. `batch_fx_read_renderer_defaults` 读取 renderer 默认值，确认 `SubType`、Niagara、NDC。
3. `unit_get` 读取目标单位当前 `Hit`、`Attack`、`Trace`。
4. `unit_plan_merge_update` 生成并集写入计划。
5. `unit_preview_diff` 审核 diff。
6. `unit_apply_plan` 应用并保存资产。
7. `unit_get` 全量读回目标字段。
8. 用户在关卡里放置 renderer actor 后运行测试。

这个流程把 MCP 当成低层工具，而不是让 MCP 变成一个带大量开关的一键 skill。
