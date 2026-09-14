# Actor to Mass Unit 编辑器

UI 迁入及维护：Winyunq。

在 UE 编辑器选择 **Tools → MassBattle Editor MCP → Actor to Mass Unit**（MassBattle Editor MCP 是菜单分组），打开插件内的 `ActorToMassBattleUnitEditor/MassBattleTools`。也可以在内容浏览器启用 Show Plugin Content，找到该 Editor Utility Widget 并选择 Run Editor Utility Widget。

本次搬入 PR 工作区提交 `c16558d` 的 `Content/Core/MassBattleTools.uasset`。保留原界面和 Generate 流程：Source 支持 Skeletal Mesh、Actor Blueprint、Actor Instance；填写 Asset Name、Generate Path 等参数后使用原 Generate 按钮。

界面继续依赖 MassBattleEditor 已有的 `FMassBattleVatSource` 属性定制和 Actor/VAT 编辑器函数。需要包含该 PR 接口的框架版本；本次未迁移或复制转换算法，也未承诺缺少这些接口的框架版本可用。当前 MCP 版本面向 UE 5.8。

维护入口是插件模块中的菜单注册和上述控件资产。控件修改须通过 UE 编辑器保存，保持现有转换函数调用。现有 MCP 单位接口、Target、源码资产保存方式、全局管理和图鉴均未调整。

验证：本地 UE 5.8 的 WinyunqEditor 构建通过；迁入控件通过蓝图编译、保存及重新加载，并成功生成编辑器标签页。本次没有重新烘焙或生成游戏单位，未验证 UE 5.6/5.7。
