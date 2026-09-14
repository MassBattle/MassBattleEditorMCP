# 源码 Actor 配置与导出协议 v1（拟定）

状态：设计契约，尚无对应新增命令实现。范围限 MCP：基础导出、炮塔适配、挂点/特效、队伍色。其他插件的解耦及Fab发布不在本轮。

## 1. 一个配置来源、两个操作入口

Actor Blueprint 持久保存强类型基础字段和版本化扩展payload。视口/Details与MCP命令读写同一个资产。sourceId与nodeId持久化，不能由名称或数组下标推断。

配置协议是可序列化数据，不是可执行脚本。MCP新增命令直接调用原生编辑器服务。既有 Plan/Apply 负责生成工作，新增入口只做配置解析和接入，不自建执行队列。

### 拟新增的统一命令

| 命令 | 输入/输出 | 职责 |
|---|---|---|
| MCP_AuthoringListCapabilities | 配置目标 → 能力及状态 | 显示可用/未安装/不兼容的功能 |
| MCP_AuthoringGetSource | SourceActor路径 → 配置与revision | GUI/MCP读取同一资产 |
| MCP_AuthoringUpdateSource | SourceActor路径、expectedRevision、明确字段patch | 校验并保存；删除使用显式操作，不依赖空值猜测 |
| MCP_AuthoringValidateSource | SourceActor路径、导出策略 → 字段问题与降级结果 | 不创建游戏资产 |
| MCP_AuthoringExportSource | SourceActor路径、expectedRevision、导出策略 | 复用已有authoring入口并返回产物/降级/失败报告 |
| MCP_AuthoringGenerateTeamMask | SourceActor材质绑定ID → 遮罩及分析报告 | 与材质页同一个原生实现 |

除这些通用命令外，不为每一辆坦克、每一种皮肤添加工具。增加功能优先注册新的schema和适配器。旧MCP方法保留兼容，通过同一实现执行。

## 2. 能力描述

每个功能描述至少含 capabilityId、schemaVersion、providerId、providerVersion、available、unavailableReason、inputSchema、outputKinds、downloadUrl、documentationUrl、missingPolicy、writeOwnership。

核心目录包含未安装功能的说明、schema和已核实下载入口，不能只有适配器安装后才能看到。插件ID与能力ID分开：一个插件可提供多项能力。可用性由已注册的提供者和版本判断，不能只检查磁盘文件夹。

同一独占能力只能选一个提供者。基础网格/VAT导出与炮塔导出属于同一个视觉生成位置，选定后只走一条已有生成路径，防止先造普通单位又追加另一套炮塔单位。材质和特效绑定消费该路径的产物引用。

适配器接口职责为 Describe、Validate、ResolveFallback、ApplyExistingAuthoring。接口名称为拟定。Apply仅调用既有编辑器能力并报告写入范围，不能获得默认重写全部单位资产的权限。

MCP在普通导出前完成缺失策略解析。在写入过程中插件不可用、源revision变化或出错，应明确报告partial_mutation及已经生成/保存的产物；不能宣称多包UE资产保存天然原子。复用既有错误报告和重导出路径，不新增自动重试调度器。

## 3. 源码结构

示例见 source-example.json。它是协议示例，不是当前引擎已可加载的资产。

- protocolVersion：协议主版本。
- sourceId、revision：源身份与修改版本。
- unit：已有通用单位引用；skin：皮肤身份和基础表示。
- nodes：id、parentId、原生组件引用、role、localTransform。
- features：稳定功能ID到versioned配置的映射。
- bindings：武器、特效及材质通过ID引用节点/配置。
- export：目标、默认缺失策略、是否允许基础降级。

单位厘米，角度度，四元数顺序X/Y/Z/W。localTransform总是相对parentId，根相对源码Actor；导出使用UE自身FTransform组合规则。轴向显式配置，不能假设任何源模型的炮管都朝+X。基本缩放和网格根由已有转换库处理。

同一个节点只能有一个父节点，检查悬空引用和循环。变更名称保留ID；复制源码Actor分配新的sourceId，同时保持内部绑定完整。已确定组件引用用于编辑事件更新，不做全局节点/单位查找链。

缺插件时仅保留普通数据/字符串资产路径，不加载可选对象。基类、组件、扩展payload不能引用第三方UHT类型；未知schema/字段原样保留，但不执行不理解的配置。

v1炮塔能力仅承诺现有单炮塔/可选单炮管及主炮口。多个独立炮塔、多个按不同规则轮射的炮口不因协议数组存在就视为已支持，应校验拒绝或报告对应能力缺失。

## 4. 有/无炮塔的同一配置

有提供者时：将中立节点引用映射到临时 UMBSTSingleTurretAuthoringComponent，填写枢轴、轴向、角度、速度及后坐，调用 ConvertActorToSingleTurretVAT。临时插件对象不保存到源码蓝图。

无提供者时：missingPolicy=UseBaseRepresentation。模型保持默认姿态整体导出，炮口局部变换折算到基础模型根，配置保留，报告DisabledCapabilities包括炮塔独立运动。普通攻击只有在已有基础weapon定义可用时启用，否则导出普通外观并报告武器未启用。

“无炮塔”指无炮塔功能，不删除炮塔外形。若用户要拆掉炮塔模型，应修改皮肤或另设明确外观变体，不与缺插件策略混淆。

重导出清理只针对本导出器拥有的旧扩展数据；不能仅跳过适配器而留下对缺失插件的硬引用。不能加载的旧输出从基础模板另建可用产物并报告迁移状态。

## 5. 开火点与特效绑定语义

AttachmentRef由source nodeId导出为明确的target layout/socket及local offset。配置端保存相对关系，不保存运行时世界坐标。

绑定至少包含：event（如WeaponFire）、attachmentId、effect资产引用、mode（Burst/Attached）、orientationPolicy、timingPolicy及基础降级策略。

- Burst 默认 sample_at_fire：使用既有开火状态机确认本发时计算的MuzzleWorld。本发使用触发后坐之前的采样，沿用现有MBST实现。投射物和枪口火光引用同一结果。
- 发射方向是单独语义。枪口闪光通常跟随物理炮管方向；投射物方向沿用已有瞄准/弹道逻辑。二者可共享位置而不强制修改原算法。
- 有视觉延迟的Burst默认锁定开火快照；需要延迟时刻姿态必须声明相应能力，不偷偷重新采样。
- Attached 默认 follow_attachment：只能映射到已验证支持该挂点的现有MassBattle Attached批处理通路；当前挂点不支持时明确拒绝此绑定或执行用户指定的禁用策略。
- 事件和姿态在运行时由既有插件处理；MCP不在游戏内运行或转发逐帧数据。用既有Shooter/ShotSequence和来源引用标识事件，不再新增业务消息队列或全局事件总线。

Burst必须通过AMassBattleFxRenderer按SubType聚合、写NDC_BurstFx，目标Niagara实际消费对应模块链。Attached必须使用已有并行数组/逻辑槽位语义。发布配置前通过现有Effect MCP核验renderer CDO与启用模块，普通Niagara不能只改名称冒充批量特效。

## 6. 队伍色配置

材质绑定保存BaseColor来源、maskMode（AutoDominant/ExplicitMask/Disabled）、tolerance、亮暗候选范围、显式遮罩或自动产物路径、palette引用、strength、UV关联与algorithmVersion。

原生GenerateTeamMask复用已验证OKLab主色算法的行为，输出线性灰度遮罩、dominantColor和coverage。没有有效候选时沿用现有全黑遮罩策略并说明原因。输入纹理、算法版本与参数构成可复现生成依据。

材质绑定与遮罩生成由MCP维护，不要求炮塔插件。炮塔适配器生成的材质同样接受TeamColor绑定；只插入颜色合成，不能覆盖其顶点位移、骨骼/VAT通路。UV及平铺/滚动必须与BaseColor一致。

运行时TeamIndex及其打包位置由目标框架材质适配确认。普通版与Net逐一核对；队伍颜色不得写入炮塔Style通道。队伍调色板是项目资产/配置引用，不硬编码某张地图的绝对路径。

## 7. 不变量

- 不因打开面板或运行MCP客户端才使游戏能力正常工作。
- GUI和MCP写同一SourceActor；插件原生资产由既有接口更新。
- 无可选插件时SourceActor仍可加载，普通产物无该插件硬依赖。
- 带插件能力的产物明示其运行时依赖；插件卸载后重新基础导出。
- 一次导出只选择一个视觉生成后端，不重复创建玩法单位。
- 不宣称协议字段存在就代表后端已实现；能力不足必须返回具体状态。
