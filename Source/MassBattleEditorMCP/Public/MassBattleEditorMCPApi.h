// Copyright (c) 2025-2026 Winyunq. All rights reserved.
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "MassBattleEditorMCPApi.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogMassBattleEditorMCPApi, Log, All);

/**
 * MassBattleEditor 函数的 MCP 包装层。
 * 所有函数接受字符串参数（资产路径），返回 JSON 格式结果字符串。
 * MCP 协议层可直接调用这些函数并透传参数/结果。
 */
UCLASS()
class MASSBATTLEEDITORMCP_API UMassBattleEditorMCPApi : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:

	// ==================== 资产操作 ====================

	/**
	 * @brief       复制蓝图类资产
	 *
	 * @param       参数名称: SourceClassPath               数据类型:        const FString&
	 *              说明: 源蓝图类的资产路径 (e.g. "/Game/BP/BP_Source.BP_Source_C")
	 * @param       参数名称: NewClassName                   数据类型:        const FString&
	 *              说明: 新蓝图类的名称
	 * @param       参数名称: PackagePath                    数据类型:        const FString&
	 *              说明: 目标包路径 (e.g. "/Game/Blueprints/Duplicated")
	 *
	 * @return      JSON 字符串: {"success": bool, "class_path": "...", "error": "..."}
	 **/
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP")
	static FString MCP_DuplicateClassAsset(const FString& SourceClassPath, const FString& NewClassName, const FString& PackagePath);

	/**
	 * @brief       设置 MassBattleAgentRenderer 蓝图类的默认属性
	 *
	 * @param       参数名称: TargetClassPath                数据类型:        const FString&
	 *              说明: 目标蓝图类的资产路径
	 * @param       参数名称: AgentMeshPath                  数据类型:        const FString&
	 *              说明: StaticMesh 资产路径 (可为空)
	 * @param       参数名称: NiagaraSystemPath              数据类型:        const FString&
	 *              说明: NiagaraSystem 资产路径 (可为空)
	 * @param       参数名称: SubType                        数据类型:        int32
	 * @param       参数名称: bSaveAssets                    数据类型:        bool
	 *              说明: 是否保存被修改的 Blueprint 资产
	 *
	 * @return      JSON 字符串: {"success": bool, "agent_mesh": "...", "saved": bool, "error": "..."}
	 **/
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP")
	static FString MCP_SetClassDefaultProperties(const FString& TargetClassPath, const FString& AgentMeshPath, const FString& NiagaraSystemPath, int32 SubType, bool bSaveAssets = false);

	// ==================== 网格转换 ====================

	/**
	 * @brief       将 SkeletalMesh 转换为带 LOD 的 StaticMesh
	 *
	 * @param       参数名称: SkeletalMeshPath               数据类型:        const FString&
	 *              说明: SkeletalMesh 资产路径
	 * @param       参数名称: OutputPackagePath               数据类型:        const FString&
	 *              说明: 输出包路径
	 * @param       参数名称: OutputAssetName                 数据类型:        const FString&
	 *              说明: 输出资产名称
	 * @param       参数名称: LightmapIndex                   数据类型:        int32
	 * @param       参数名称: bGenerateLightmapUVs            数据类型:        bool
	 *
	 * @return      JSON 字符串: {"success": bool, "static_mesh_path": "...", "error": "..."}
	 **/
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP")
	static FString MCP_ConvertSkeletalMeshToStaticMeshWithLODs(const FString& SkeletalMeshPath, const FString& OutputPackagePath, const FString& OutputAssetName, int32 LightmapIndex = 0, bool bGenerateLightmapUVs = true);

	// ==================== 材质操作 ====================

	/**
	 * @brief       为 StaticMesh 的每个 LOD 创建材质实例
	 *
	 * @param       参数名称: StaticMeshPath                  数据类型:        const FString&
	 * @param       参数名称: PackagePath                     数据类型:        const FString&
	 * @param       参数名称: AssetName                       数据类型:        const FString&
	 * @param       参数名称: ParentMaterialPath              数据类型:        const FString&
	 * @param       参数名称: OriginalTexturesJson            数据类型:        const FString&
	 *              说明: FOriginalTextures 数组的 JSON 序列化
	 *
	 * @return      JSON 字符串: {"success": bool, "error": "..."}
	 **/
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP")
	static FString MCP_CreateMaterialInstanceForStaticMeshWithLODs(const FString& StaticMeshPath, const FString& PackagePath, const FString& AssetName, const FString& ParentMaterialPath, const FString& OriginalTexturesJson);

	// ==================== 纹理查找 ====================

	/**
	 * @brief       根据 SkeletalMesh 的材质槽自动查找并填充原始纹理
	 *
	 * @param       参数名称: SkeletalMeshPath                数据类型:        const FString&
	 * @param       参数名称: SearchPath                      数据类型:        const FString&
	 *              说明: 纹理搜索路径 (Content 路径)
	 * @param       参数名称: AssetName                       数据类型:        const FString&
	 *              说明: 资产名称过滤
	 *
	 * @return      JSON 字符串: {"success": bool, "textures": [...], "error": "..."}
	 **/
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP")
	static FString MCP_FindAndFillOriginalTextures(const FString& SkeletalMeshPath, const FString& SearchPath, const FString& AssetName);

	// ==================== 动画查找 ====================

	/**
	 * @brief       根据 SkeletalMesh 的骨架自动查找并分类动画序列
	 *
	 * @param       参数名称: SkeletalMeshPath                数据类型:        const FString&
	 * @param       参数名称: SearchPath                      数据类型:        const FString&
	 *              说明: 动画搜索路径
	 * @param       参数名称: FileName                        数据类型:        const FString&
	 *              说明: 文件名过滤
	 *
	 * @return      JSON 字符串: {"success": bool, "anims": {"Idle": [...], "Move": [...], ...}, "error": "..."}
	 **/
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP")
	static FString MCP_FindAndFillAnimSequences(const FString& SkeletalMeshPath, const FString& SearchPath, const FString& FileName);

	// ==================== LOD 设置 ====================

	/**
	 * @brief       根据 SkeletalMesh 的 LOD 信息自动填充 LOD 设置
	 *
	 * @param       参数名称: SkeletalMeshPath                数据类型:        const FString&
	 *
	 * @return      JSON 字符串: {"success": bool, "lod_settings": [...], "error": "..."}
	 **/
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP")
	static FString MCP_FindAndFillLODSettings(const FString& SkeletalMeshPath);

	// ==================== 动画数据纹理 ====================

	/**
	 * @brief       创建存储动画帧数据的 Texture2D (40x1 RGBA16F)
	 *
	 * @param       参数名称: AnimsInfoJson                   数据类型:        const FString&
	 *              说明: FAnimToTextureAnimInfo 数组的 JSON: [{"StartFrame": N, "EndFrame": N}, ...]
	 * @param       参数名称: PackagePath                     数据类型:        const FString&
	 * @param       参数名称: AssetName                       数据类型:        const FString&
	 *
	 * @return      JSON 字符串: {"success": bool, "texture_path": "...", "error": "..."}
	 **/
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP")
	static FString MCP_CreateAnimDataTexture(const FString& AnimsInfoJson, const FString& PackagePath, const FString& AssetName);

	// ==================== 动画数据转换 ====================

	/**
	 * @brief       使用 VAT DataAsset 将动画序列转换为 MassBattle 的 FAnimsData
	 *
	 * @param       参数名称: DataAssetPath                   数据类型:        const FString&
	 *              说明: UAnimToTextureDataAsset 资产路径
	 * @param       参数名称: FoundAnimsJson                  数据类型:        const FString&
	 *              说明: FFoundAnimSequences 的 JSON 序列化 (各分类的动画资产路径数组)
	 *
	 * @return      JSON 字符串: {"success": bool, "anims_data": {...}, "error": "..."}
	 **/
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP")
	static FString MCP_CreateAnimsDataFromSequences(const FString& DataAssetPath, const FString& FoundAnimsJson);

	/**
	 * @brief       将 LOD 编辑器设置转换为运行时 LODs 数据
	 *
	 * @param       参数名称: LODSettingsJson                 数据类型:        const FString&
	 *              说明: FLODDataEd 数组的 JSON
	 *
	 * @return      JSON 字符串: {"success": bool, "lods_data": {...}, "error": "..."}
	 **/
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP")
	static FString MCP_ConvertLODSettingsToLODsData(const FString& LODSettingsJson);

	// ==================== 验证 ====================

	/**
	 * @brief       验证动画序列 (截断超过5个的分类，检查总帧数)
	 *
	 * @param       参数名称: FoundAnimsJson                  数据类型:        const FString&
	 *              说明: FFoundAnimSequences 的 JSON (各分类的动画资产路径数组)
	 *
	 * @return      JSON 字符串: {"success": bool, "validated_anims": {...}, "warnings": [...], "error": "..."}
	 **/
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP")
	static FString MCP_ValidateAnimSequences(const FString& FoundAnimsJson);

	// ==================== 重命名操作 ====================

	/**
	 * @brief       重命名 SkeletalMesh 资产
	 *
	 * @param       参数名称: SkeletalMeshPath                数据类型:        const FString&
	 * @param       参数名称: OriginalTexturesJson            数据类型:        const FString&
	 * @param       参数名称: AssetName                       数据类型:        const FString&
	 *
	 * @return      JSON 字符串: {"success": bool, "error": "..."}
	 **/
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP")
	static FString MCP_RenameSkeletalMesh(const FString& SkeletalMeshPath, const FString& OriginalTexturesJson, const FString& AssetName);

	/**
	 * @brief       重命名原始纹理资产
	 *
	 * @param       参数名称: OriginalTexturesJson            数据类型:        const FString&
	 * @param       参数名称: AssetName                       数据类型:        const FString&
	 *
	 * @return      JSON 字符串: {"success": bool, "error": "..."}
	 **/
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP")
	static FString MCP_RenameOriginalTextures(const FString& OriginalTexturesJson, const FString& AssetName);

	/**
	 * @brief       重命名动画序列资产
	 *
	 * @param       参数名称: FoundAnimsJson                  数据类型:        const FString&
	 * @param       参数名称: AssetName                       数据类型:        const FString&
	 *
	 * @return      JSON 字符串: {"success": bool, "error": "..."}
	 **/
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP")
	static FString MCP_RenameAnimSequences(const FString& FoundAnimsJson, const FString& AssetName);

	// ==================== 工具函数 ====================

	/**
	 * @brief       清理字符串中不允许用于路径的字符
	 *
	 * @param       参数名称: InString                        数据类型:        const FString&
	 *
	 * @return      清理后的字符串
	 **/
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP")
	static FString MCP_SanitizeForPath(const FString& InString);

	/**
	 * @brief       获取所有 MCP API 的状态摘要 (工具列表、版本等)
	 *
	 * @return      JSON 字符串: {"api_name": "...", "version": "...", "tools": [...]}
	 **/
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP")
	static FString MCP_GetApiStatus();

private:

	/// 内部辅助: 将 JSON 数组字符串解析为 FOriginalTextures 数组
	static bool ParseOriginalTexturesFromJson(const FString& JsonString, TArray<struct FOriginalTextures>& OutTextures);

	/// 内部辅助: 将 JSON 字符串解析为 FFoundAnimSequences
	static bool ParseFoundAnimsFromJson(const FString& JsonString, struct FFoundAnimSequences& OutAnims);

	/// 内部辅助: 将 FOriginalTextures 数组序列化为 JSON 字符串
	static FString SerializeOriginalTexturesToJson(const TArray<struct FOriginalTextures>& Textures);

	/// 内部辅助: 将 FFoundAnimSequences 序列化为 JSON 字符串
	static FString SerializeFoundAnimsToJson(const struct FFoundAnimSequences& Anims);

	/// 内部辅助: 构造错误 JSON
	static FString MakeErrorJson(const FString& ErrorMessage);

	/// 内部辅助: 构造成功 JSON
	static FString MakeSuccessJson();
};
