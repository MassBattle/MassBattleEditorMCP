# 源码 Actor

维护：Winyunq。`AMassBattleUnitSource` 是编辑器中的单位源格式。创建它的 Actor 蓝图资产，在 Class Defaults 中编辑 `UnitData` 和 `ExportPath`，Details 提供 Update、Destroy。放入关卡的实例可预览；Update 会把该实例的单位配置写回对应源码蓝图。源码 Actor 不进入游戏运行时。

可复制插件内的空白模板 `/MassBattleEditorMCP/ActorToMassBattleUnitEditor/BP_UnitSource` 到项目内容目录开始配置。该模板尚未生成导出单位；在内容浏览器开启“显示插件内容”即可找到。

`UnitData` 直接使用现有 `UMassBattleAgentConfigDataAsset`，没有第二套单位字段。源码保存上次成功 Update 的配置快照和导出单位引用。初次 Update 创建单位；后续只导出相对源码快照改变的字段。数组按整个数组比较和替换，普通结构体按现有 MCP 属性路径处理。

例如源码 HP=100，导出后手动把产物 HP 改为200：源码未改 HP，Update 保留200；源码改成120再 Update，则产物变为120。其他字段变更不会顺带重写 HP。读取、差异预览、应用和审计复用现有单位 MCP 实现。

`ExportPath` 是完整 UE 包名，例如 `/Game/Units/DA_Tank`。修改后 Update 使用 UE AssetTools 移动原单位资产，保留正常重定向与引用修复；不会复制出第二份单位。目标已被其他资产占用则失败。空路径使用项目设置中的默认目录和源码资产名。模型、材质等被引用的共享资产不随单位配置移动或删除。

Destroy 按钮只销毁导出的 Unit，保留源码 Actor。执行前询问：“销毁此单位，之后再生成，可能使得某些数据丢失。确定要销毁单位吗？（删除本资产也可以触发此询问）”。拒绝则不做修改；确认后通过原 MCP 引用检查和删除入口处理，成功后清空导出引用。再次 Update 从保留的源码生成单位，因此产物中未写回源码的手工修改会丢失。

在内容浏览器删除源码 Actor 资产时，复用 UE 的 `OnAddExtraObjectsToDelete` 事件询问是否一起销毁 Unit。确认则把 Unit 加入同一删除批次；拒绝则仅删除源码 Actor。此阶段不提前删除任何资产，之后取消 UE 删除窗口会一起取消。删除关卡中的 Actor 实例不会触发资产清理。

MCP 删除源码路径默认仅删除源码，保留原 dry_run、soft/hard 行为；明确传入 `delete_exported_unit:true` 才规划并执行关联 Unit 的删除/移动。无交互命令行不会默认同意弹窗。外部引用仍由原删除流程检查。

自动化测试 `MassBattle.MCP.SourceDestroy` 已在 UE 5.8 通过：拒绝直接销毁保留 Unit、确认后仅删除 Unit、源码可重新生成，以及资产删除事件按回答决定是否加入 Unit。测试只使用 `/Game/__MCPDestroyTest`，结束时清理该测试资产。

## 原 MCP 命令

命令名称及 `UnitPath` 参数不变，可以传单位 DataAsset 路径或源码 Actor 蓝图路径。`MCP_UnitList` 使用 UE 资产注册表查询两种资产；`MCP_UnitGet`、差异规划、应用、表格导出和删除都通过同一目标解析。

创建一个只保存源码、尚未导出单位的资产：

```json
{
  "asset_type": "source_actor",
  "asset_name": "BP_TankSource",
  "package_path": "/Game/UnitSources",
  "export_path": "/Game/Units/DA_Tank",
  "unit_data": { "Health": { "Current": 100, "Maximum": 300 } }
}
```

将上面的 JSON 传给 `MCP_UnitCreate`，也可提供 `template_unit`。省略 `asset_type` 时保留原单位创建流程。

`MCP_UnitMergeUpdate` 指向源码时，保存的写操作会执行 Update；`bSaveAssets=false` 只修改源码内存，便于继续编辑。修改输出路径使用同一 JSON：

```json
{
  "export_path": "/Game/Units/Tanks/DA_Tank",
  "Data": { "Health": { "Maximum": 350 } }
}
```

## 共用导出规则与语言

Tools → Unit Export Rules 打开 Project Settings → Plugins → MassBattle Unit Export。`ExportRoot` 保存到 Editor 配置，源码和 MCP 的 Update 共用这一默认值；源码显式路径优先。

按钮与入口使用 UE 本地化，提供英文、简体中文。翻译源位于 `Content/Localization/MassBattleEditorMCP`，修改 archive 后，在项目目录使用 UE `-run=GatherText -config=Plugins/MassBattleEditorMCP/Config/Localization/Compile.ini` 重新生成资源。

源码集中在 `MassBattleUnitSource.h/.cpp`，MCP 接入集中在既有 `MassBattleUnitMCPApi.cpp`。验证脚本 `Scripts/test_source_actor.py` 连续在两个独立 UE 进程运行：第一次验证创建、保留产物手改字段、差异更新与移动；第二次验证快照重载、引用阻止删除和删除两份资产。测试使用隔离的 `/Game/__MCPSourceActorTest`。

2026-09-14：UE 5.8 编译通过，已验证上述差异、移动、重载及成对删除行为；空白模板已保存并打开，英文与简体中文按钮文本加载通过。UE 5.6、5.7 尚未实机编译验证。

## 炮塔标记和队伍色 Demo

打开 `/MassBattleEditorMCP/Demo/TeamColorTank/BP_TankSourceDemo`，切到 Viewport，再点 Class Defaults。在 Team Color 分类点击“应用队伍颜色”；默认编号 2 显示红色，编号 1 为绿色。“显示原始颜色”恢复未染色预览。Demo 初始保存为原色。按钮在源码 Actor 蓝图和关卡实例上都能使用，预览不需要运行游戏。

原测试机枪坦克的源网格、导入材质和纹理通过 UE AssetTools 移到 Demo 的 `SourceMeshes` 目录，旧位置保留 UE 重定向。已有游戏单位并未移走。Demo 的单位配置从该单位通过原 MCP 模板入口保存到源码中，尚未创建新的游戏单位。

源码拥有 Body、Turret、Barrel 网格组件及 TurretPivot、BarrelPivot、Muzzle 箭头组件。标记复用炮塔插件已有的 `MBST_TurretYaw`、`MBST_BarrelPitch`、`MBST_Muzzle` 标签协议；Muzzle 挂在 BarrelPivot 下，BarrelPivot 挂在 TurretPivot 下。无炮塔插件时这些都是普通 UE 组件，仍可保存、移动和预览。安装炮塔插件后，其 `AddTransientAuthoringComponent` 与 `ConvertActorToSingleTurretVAT` 既有入口可直接识别这些节点；这次没有替换其转换器，也没有把炮塔烘焙自动并入 Update。

本轮队伍颜色按钮使用源码指定的 TeamMaterial、TeamMask、PreviewTeamIndex 和 TeamTintStrength，仅更新 Actor 的预览材质。槽位 0 是本 Demo 的涂装槽；其他槽保留其原材质。预览实例是瞬态材质，不覆盖共享皮肤，不把预览队伍写进游戏单位。游戏仍使用原有 TeamIndex 驱动的材质链。

Demo 的遮罩复用前次自动分析结果：主色 RGB=(132,151,118)，平均遮罩权重约 0.511，容差 0.065。不是重新手绘选区。主色分析、贴图导出与材质接线程序集中维护在 `Content/Python/massbattle_team_color`；原项目 Python 入口只是兼容转发。离线分析依赖 NumPy、Pillow，Actor 的应用/还原预览不依赖 Python。材质接线复用 `author_team_masks.configure_base`；Demo 只增加默认值 -1 的 `PreviewTeamIndex`，非负时覆盖预览队伍，默认仍按 MassBattle 的 DP0.W 解码队伍。

验证：MCP 模块 UE 5.8 编译通过；既有遮罩算法测试通过；三个标记经炮塔插件入口解析通过；旋转炮塔后炮口世界位置改变；蓝图默认对象与实例的应用/还原均通过；实际渲染了原色、队伍 1、队伍 2。新预览按钮中文资源加载通过。未进行卸载炮塔插件后的完整项目启动测试。

### Shared minimap team buffer (UE 5.8)

Unit materials read `Scene.MassBattleTeamColors.Colors[TeamIndex]`. The scene
extension binds the exact SRV owned by `FMassBattleMinimapRenderData::TeamColorsBuffer`;
there is no separate palette texture or color copy. `CommitTeamColors` remains
the existing upload entry. The extension only attaches that render-data reference
to the corresponding scene. `TeamMask` and brightness preservation are unchanged.
If that scene has no palette, tint weight is zero and the original surface remains.

The adapter is isolated in FogOfWar's `MassBattleTeamColorSceneExtension.cpp` and
uses UE 5.8 SceneUniformBuffer/ISceneExtensionRenderer. It includes Renderer private
headers; it does not change engine or MassBattleFrame source. This material path
requires FogOfWar and UE 5.8; earlier engine versions have not been validated.

`migrate_team_buffer_materials.py` reconnects the known authored materials and
source demo. Validate the result by switching team colors in the actual game after
restarting the editor to load the rebuilt plugin.
