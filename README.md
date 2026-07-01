# MassBattleEditorMCP 中文文档中心

欢迎使用 MassBattleEditorMCP 文档中心。  
所有文档集中在当前仓库，支持中文说明，并提供网页式查看体验。

## 你现在在看什么

这里是该插件的官方中文说明入口。建议优先使用 `index.html` 打开，能获得更好的排版、目录、搜索和代码高亮体验。

## 文档入口（网页版）

1. 推荐方式：打开 `index.html`（本仓库同级目录直接双击可启动，若页面不显示请用本地服务器启动）。
2. 本地预览命令（任一）：  
   - `python -m http.server 8080`  
   - `npx serve .`
3. 浏览器打开 `http://localhost:8080/index.html`。

## 文档目录

### Order 1：编辑单位表现

文件：`README.md`（本页）  
场景：单位外观、效果、模型、动画、材质、SpawnFx 等表现参数调整。

### Order 2：调整数值平衡

文件：`MassBattleEditor.md`  
场景：血量、伤害、射程、移动、抗性等平衡参数调节。

### Order 3：市场特效转批处理特效

文件：`BatchFxMarketplaceConversion.md`  
场景：市场/普通 FX 转为批处理 FX，并通过单元配置联动。

### Order 4：Demo 批处理特效实战

文件：`BatchFxDemoLearning_JapanSoldier.md`
场景：学习 MassBattleFrame Demo 的批处理爆炸、子弹、选择圈链路，并把测试日本步兵接入批处理 FX。

### API 与设计说明

文件：`MassBattleEditorMCP_Design.md`  
场景：接口能力、插件能力边界与技术实现细节。

## 快速部署说明（给团队用）

1. 该仓库使用 `Document` 分支承载文档站源文件；本地 worktree 通常位于 `Document/`。
2. 如果你已开启 Pages，可设置 `Source` 为 `Document` 分支根目录。
3. 发布后访问地址直接作为团队统一文档入口，例如：  
   - `https://<你的组织>.github.io/MassBattleEditorMCP/`

## 阅读建议

- 先看本页：先确认当前任务属于哪个 order。  
- 再跳转到对应文档：按模块分开操作，避免把表现与数值混在同一单次任务里。  
- 修改后建议保留 `plan` 与 `readback` 证据：这也是复核和回溯的基础。

## 统一入口

打开 [index.html](./index.html) 查看完整网页式文档。
