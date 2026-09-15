# MCP 中枢与源码 Actor：本轮实施方案

状态：方案草案，2026-09-14。仅文档，尚未实现或提交。前一版 ActorToMassbattleUnitEditor 方案由本目录细化；本文是本次讨论的入口。

## 1. 要实现的结果

用户最新收窄范围：本轮只解决 MassBattleEditorMCP 的配置、协议与导出，不修改其他业务插件之间的依赖，不处理 Fab 打包发布。

用户从 MCP 的源码 Actor 编辑模型、炮塔枢轴、炮口、特效与队伍色，或通过同一组 MCP 命令配置这些数据；点击导出时，有炮塔插件则生成带炮塔能力的单位，没有则生成保留炮塔外形、但无独立炮塔运动的普通单位。MCP 基础模块不得因缺少可选业务插件而无法加载。

基础依赖必须明示：UE 是运行环境，导出 MassBattle 游戏单位必须有 MassBattle。这里消除的是额外插件之间的强制依赖，不声称能够在没有 MassBattle 的情况下运行 MassBattle 单位。第三方算法后端缺失时，对应适配器不可用，不等于能替代该算法。

“插件能加载”“配置能保存”“功能能执行”分别验证。缺失服务可使相应功能不可用，但不能使其他无关插件、源码 Actor 或基础导出整体无法加载。

## 2. 已有能力及实际缺口

| 能力 | 已有入口/实现 | 本次应补的内容 |
|---|---|---|
| Actor 组装及 VAT | MCP_EditorApplyCreateVatUnitFromActor → MCP_EditorApplyCreateVatUnit | 让视图和协议复用同一入口，收敛当前重复组装代码 |
| 炮塔 | UMBSTSingleTurretAuthoringComponent；ConvertActorToSingleTurretVAT | 中立配置、原生标记和炮塔提供者，直接调用现有库 |
| 炮口姿态 | UMBSTSingleTurretAsset::CalculateMuzzleWorldTransform；FMBSTFireRequest::MuzzleWorld | 协议挂点身份和事件时刻语义；复用现有姿态计算 |
| 批量特效 | 已有 Effect/Niagara MCP 与 MassBattle Burst/Attached 通路 | 将挂点引用映射到已存在的特效/投射物绑定字段 |
| 自动队伍色 | Content/Python/TeamColor 已验证的主色分析、遮罩、材质绑定 | 移入 MCP 原生编辑器实现，保留算法参数和回归样例 |
| 产能 | RegisterCapacity、SetCapacityLaneCount；自动注册当前按类型取默认值 | 插件自己的持久生产配置及注册读取入口；不能只增加界面字段 |
| 科研/生产公共执行 | 现有共享任务、提交、同步、状态通知 | 消除编译依赖时继续复用它，不复制队列和执行器 |

两框架最新核查点：普通版 48c0106、Net 0db13d5；当前 MCP b3a38c2。普通版 Actor 相关11文件与历史5.6/5.7/5.8成功构建包相同；MCP 的 VAT 工作流直接调用 AnimToTextureEditor，不要求 Net Frame 提供额外烘焙包装。当前MCP无条件依赖MassCore并调用5.8的SkeletalMesh::Clear()，尚不能宣称六种组合兼容。

## 3. 职责和目录

MassBattleEditorMCP 负责协议、发现功能、配置编辑、源码 Actor、预览、导出报告、基础特效绑定及队伍色制作。功能插件负责自己的参数含义、校验、配置落盘及现有运行时行为。中枢不复制炮塔、生产、研发、迷雾算法，也不成为游戏运行时服务。

    MassBattleEditorMCP/
      Source/ActorToMassbattleUnitEditor/
        Public/                源码 Actor、标记、编辑器公共入口
        Private/Source/        持久配置及视口显示
        Private/Protocol/      版本化数据与通用编辑命令
        Private/Export/        复用现有 authoring 流程
        Private/Bindings/      挂点、特效、材质绑定
        Private/TeamColor/     自动遮罩及材质生成
        Private/Compatibility/ UE及框架接口适配
      Source/MassBattleEditorMCP/  MCP传输入口，转发至同一服务
      Content/ActorToMassBattleUnitEditor/
        Templates/
        Examples/
        UnitSources/           用户 BP 源配置；升级不覆盖
      Docs/AuthoringProtocol/  本目录：协议、维护与验收

新增模块 Type=Editor。不在两个 MassBattleFrame 仓库各复制实现，两边安装同一版本 MCP 源码并各自编译。插件无需连接AI也能使用可视化配置。

## 4. MCP 自身的可选接入边界

本轮仅保证 MCP 不强制依赖炮塔等可选功能插件。采用声明式配置协议，核心配置类型不引用功能插件 UCLASS/USTRUCT。中枢通过 UE 已有 IModularFeatures 接收可用适配器的注册/注销，按确定的 FeatureId 调用，禁止 Tick 扫描插件或单位。

首批适配器由MCP侧维护，调用既有插件公开C++接口，不要求修改炮塔插件的运行时算法或让其反向依赖MCP。直接包含炮塔头文件的代码与基础MCP模块隔离，只在目标插件可用的适配模块中编译加载；基础模块、源Actor、通用标记不能包含这些头文件。具体发行打包组织留到后续Fab工作，本轮不提前制定所有业务插件的发布结构。

新增配置入口不能只是执行Python脚本。源码Actor和MCP共享原生编辑器服务，适配器调用现有库；Python用户脚本不再是配置前提。

INI仅承载MCP项目级默认值、功能开关和提供者选择；单位/皮肤配置存在源码Actor，原生插件配置仍由其已有接口管理，不另建镜像配置系统。

其他业务插件之间已有的Build.cs依赖、公共任务归属、输入/科研/生产关系保持本轮范围之外。协议预留它们将来接入的方式，不承诺本轮完成所有插件独立发布。

## 5. 配置覆盖的对象

源码 Actor 持有 SourceId、已有通用单位引用、SkinId、模型及动画、稳定节点标识、功能配置和导出策略。通用单位继续承载玩法，皮肤重建不新增单位身份或重新写平衡参数。

节点使用原生 SceneComponent/ArrowComponent（或MCP自己的编辑器标记组件），固定 nodeId + parentId + localTransform。外部名称只用于显示；重命名组件不改变挂点身份。导出前检查唯一性、引用有效性、坐标空间和父子关系。

未知/缺插件扩展保存 FeatureId、schemaVersion、普通数据payload及纯数据资产路径；不序列化可选组件、枚举或FInstancedStruct。缺提供者时不主动加载这些路径，保留未知字段。编辑控件由schema或可用的提供者提供，用户不需要写JSON。

未来其他插件可将配置目标登记为其原生INI、设置对象或配置资产，而不是把所有全局数据塞入每个源码Actor。本轮只验证基础单位导出、炮塔适配、挂点/特效和队伍色四项。产能/科研等后续按同一协议接入，本轮不重构它们。

## 6. 导出缺省协议

详细字段见 protocol.md。每个功能声明能力、输入、输出、版本、缺失策略和会写入的字段。中枢先完成整体校验及降级解析，再调用已有 authoring 能力。不新增运行时调度器/队列。

有炮塔提供者：调用 ConvertActorToSingleTurretVAT，并将炮口标记交给既有布局生成过程。

无炮塔提供者：保留炮塔网格，按源码默认姿态参与基础模型导出；忽略独立旋转/俯仰/后坐，炮口绑定降为明确的车体局部固定挂点，攻击走既有基础单位武器配置。仅有炮塔专属武器策略而无基础武器定义时，不启用射击并明确报告，不能猜测攻击配置。输出不携带炮塔插件结构体、材质、处理器引用。

再次导出到同一目标时，必须撤掉本流程上次生成、此次已禁用的扩展引用，不能仅跳过步骤而留下旧炮塔Fragment。优先重建本导出器拥有的视觉产物，并从基础通用单位模板恢复合法绑定；保留用户玩法字段。若旧目标因为插件缺失已不能加载，直接从干净基础模板生成普通变体并报告旧目标未迁移，不宣称原地替换成功。

产物分清两类：普通产物运行时仅需基础框架；带炮塔产物运行时仍需炮塔插件。卸载炮塔插件后要重新基础导出，不能要求既有带炮塔的游戏产物自动获得同样能力。

## 7. 炮口、旋转炮塔和特效

协议保存相对哪个节点的挂点，而不是导出时的世界坐标。例如 body → turretPivot → barrelPivot → muzzle，特效和武器都引用 muzzle 的稳定标识。

MBST 已有 CalculateMuzzleWorldTransform，按炮口物体空间变换、后坐、俯仰、炮塔旋转、网格根世界变换计算实际姿态；开火请求已携带 MuzzleWorld 和 ShotSequence。直接复用这个公共计算。由现有开火状态处理确认射击，在同一逻辑时刻取姿态，投射物与Burst枪口特效消费同一份变换。现有实现取本发触发前的后坐状态；协议保留该语义，不再从当前视口或另一个时刻重算。

MCP 的职责是把源码绑定导出到既有武器、投射物和特效配置，MCP 不在游戏中驱动开火。不得再写通用运行时事件总线、按帧扫描单位、逐单位Niagara组件或第二个开火队列。

Burst：一次开火位置/方向快照，后续不跟随炮口。必须经 AMassBattleFxRenderer 按 SubType 聚合、NDC_BurstFx，以及真正消费该通道的 Niagara 模块链。

Attached：持续跟随时由现有Attached批处理通路维护逻辑槽位，必须消费正确的炮口姿态。现有通路若只支持实体根挂点，不得假称已支持炮管；先验证是否有可复用的姿态输入，缺口作为对应插件公共接口的单独扩展项。不能用一单位一Niagara或MCP Tick代替。

枪口方向指物理炮管/标记方向；投射物瞄准方向可能由已有弹道/预测逻辑决定。协议区分 muzzle_transform 与 launch_direction，不为了“统一”擅改瞄准算法。

非均匀缩放、斜坡、炮管初始仰角均以既有网格根变换和局部空间处理；测试不得只覆盖世界原点、零旋转。

## 8. 队伍色成为MCP基础能力

将已经验证的Python算法移入MCP原生编辑器命令 GenerateTeamMask，并暴露 SourceActor 的材质配置页：BaseColor来源、自动主色或显式主色、颜色容差、暗部保护、TeamMask与调色板引用。

按纹理内容/参数/算法版本确定生成结果，多个皮肤材质复用同一遮罩；结果写入派生纹理及当前材质，原BaseColor不改写。用户操作无需Python环境。Python旧实现仅保留为迁移回归参考，不成为第二条生产配置入口。

运行时沿用现有 TeamIndex 材质输入。已验证Net路径为DynamicParams0.W；普通版相同语义的输入必须单独核实，框架差异集中适配，不把Net的位布局硬套到普通版。更不能与炮塔使用的Style位数据共用写入位置。

遮罩采样必须与实际BaseColor的UV、平铺/滚动变换一致，使用线性数据。材质插入只改颜色合成，保留VAT/炮塔WPO、法线、透明、LOD和选中状态。自动主色只做颜色相似度判断，不承诺理解木材/装甲语义；结果提供预览与覆盖率。

## 9. 实施顺序和验收

P0：冻结MCP协议，明确基础导出、炮塔、挂点特效和队伍色的公共入口与缺失行为；补齐两框架×三引擎差异的证据。

P1：源码Actor、中立节点、通用配置与注册协议；复用已有MCP Plan/Apply，GUI与MCP同一实现；无可选插件时普通导出。

P2：炮塔提供者、默认姿态降级和炮口绑定；加入普通→带炮塔→普通的导出往返测试。

P3：MCP原生队伍遮罩与枪口Burst绑定，检验同一炮口事件/姿态；Attached单独验证实际后端能力。

P4：整理原生MCP命令、示例源码Actor和维护文档；生产、科研等插件接入与依赖重构留到后续。

P5：UE5.6/5.7/5.8 × 普通版/Net 编译和运行验证；可选插件缺失/安装/再次移除，MCP关闭后的游戏打包；确认源码Actor和普通输出没有可选插件硬引用。

MassBattleFrame/Source 现有只读约束继续有效。若协议落实需要更改框架公共helper，记录具体缺口并单独取得授权，不能借本方案直接修改。

详细验收：稳定ID重命名；未知版本配置保留；出错不报告全部成功；重复导出不重复追加特效；红绿队伍和转炮塔同时工作；旋转/俯仰/后坐/坡面上的炮口闪光正确；Burst/Attached符合真实批处理路径；无MCP运行时依赖。
