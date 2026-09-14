# 源码 Actor

维护：Winyunq。`AMassBattleUnitSource` 是编辑器中的单位源格式。创建它的 Actor 蓝图资产，在 Class Defaults 中编辑 `UnitData` 和 `ExportPath`，Details 提供 Update、Destroy。放入关卡的实例可预览；Update 会把该实例的单位配置写回对应源码蓝图。源码 Actor 不进入游戏运行时。

可复制插件内的空白模板 `/MassBattleEditorMCP/ActorToMassBattleUnitEditor/BP_UnitSource` 到项目内容目录开始配置。该模板尚未生成导出单位；在内容浏览器开启“显示插件内容”即可找到。

`UnitData` 直接使用现有 `UMassBattleAgentConfigDataAsset`，没有第二套单位字段。源码保存上次成功 Update 的配置快照和导出单位引用。初次 Update 创建单位；后续只导出相对源码快照改变的字段。数组按整个数组比较和替换，普通结构体按现有 MCP 属性路径处理。

例如源码 HP=100，导出后手动把产物 HP 改为200：源码未改 HP，Update 保留200；源码改成120再 Update，则产物变为120。其他字段变更不会顺带重写 HP。读取、差异预览、应用和审计复用现有单位 MCP 实现。

`ExportPath` 是完整 UE 包名，例如 `/Game/Units/DA_Tank`。修改后 Update 使用 UE AssetTools 移动原单位资产，保留正常重定向与引用修复；不会复制出第二份单位。目标已被其他资产占用则失败。空路径使用项目设置中的默认目录和源码资产名。模型、材质等被引用的共享资产不随单位配置移动或删除。

Destroy 同时删除源码蓝图和它导出的单位。默认通过现有引用检查；两者内部引用不算外部引用，其他资产的引用会阻止硬删除。编辑器按钮执行硬删除；MCP 的删除入口仍保留原有 dry_run、soft/hard 行为。删除已放入关卡的源码蓝图可能被关卡引用阻止。

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
