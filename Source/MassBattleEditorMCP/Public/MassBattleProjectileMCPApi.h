// Copyright (c) 2025-2026 Winyunq. All rights reserved.
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "MassBattleProjectileMCPApi.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogMassBattleProjectileMCPApi, Log, All);

/**
 * Source-aligned CRUD, schema, and validation primitives for
 * UMassBattleProjectileConfigDataAsset.
 *
 * This API edits projectile gameplay assets only. Linked one-shot and attached
 * visuals remain MassBattle Batch FX/Niagara work and are authored through the
 * effect APIs.
 */
UCLASS()
class MASSBATTLEEDITORMCP_API UMassBattleProjectileMCPApi : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|Projectile")
	static FString MCP_ProjectileGetApiStatus();

	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|Projectile")
	static FString MCP_ProjectileList(const FString& OptionsJson);

	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|Projectile")
	static FString MCP_ProjectileQuery(const FString& QueryJson);

	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|Projectile")
	static FString MCP_ProjectileGet(const FString& ProjectilePath, const FString& OptionsJson);

	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|Projectile")
	static FString MCP_ProjectileGetSchema(const FString& OptionsJson);

	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|Projectile")
	static FString MCP_ProjectileCreate(const FString& CreateSpecJson, bool bSaveAssets = true);

	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|Projectile")
	static FString MCP_ProjectileWrite(const FString& ProjectilePath, const FString& PatchJson, bool bSaveAssets = true);

	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|Projectile")
	static FString MCP_ProjectileValidate(const FString& ProjectilePath, const FString& OptionsJson);

	/** Delete is a dry run unless OptionsJson contains dry_run=false. */
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|Projectile")
	static FString MCP_ProjectileDelete(const FString& ProjectilePath, const FString& OptionsJson);
};
