// Copyright (c) 2025-2026 Winyunq. All rights reserved.
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "MassBattleEffectAssetMCPApi.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogMassBattleEffectAssetMCPApi, Log, All);

/**
 * Low-level effect asset MCP primitives.
 *
 * These tools intentionally do not perform one-click conversion. They provide
 * asset discovery, text inspection, duplication, and MassBattle FX renderer
 * default writes so a skill can assemble a conversion workflow.
 */
UCLASS()
class MASSBATTLEEDITORMCP_API UMassBattleEffectAssetMCPApi : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|BatchEffects")
	static FString MCP_EffectAssetGetApiStatus();

	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|BatchEffects")
	static FString MCP_EffectAssetQuery(const FString& QueryJson);

	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|BatchEffects")
	static FString MCP_EffectAssetReadSummary(const FString& AssetPath, const FString& OptionsJson);

	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|BatchEffects")
	static FString MCP_EffectAssetExportText(const FString& AssetPath, const FString& OptionsJson);

	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|BatchEffects")
	static FString MCP_EffectAssetSoftDelete(const FString& AssetPath, const FString& OptionsJson);

	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|BatchEffects")
	static FString MCP_EffectDuplicateAsset(const FString& SourceAssetPath, const FString& NewAssetName, const FString& PackagePath, bool bSaveAssets);

	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|BatchEffects")
	static FString MCP_BatchFxReadRendererDefaults(const FString& TargetClassPath);

	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|BatchEffects")
	static FString MCP_BatchFxSetRendererDefaults(const FString& TargetClassPath, const FString& NiagaraSystemPath, const FString& NdcBurstFxPath, int32 SubType, int32 RenderBatchSize, float PoolingCooldown, bool bSaveAssets);
};
