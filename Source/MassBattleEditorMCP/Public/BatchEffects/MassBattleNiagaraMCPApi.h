// Copyright (c) 2025-2026 Winyunq. All rights reserved.
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "MassBattleNiagaraMCPApi.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogMassBattleNiagaraMCPApi, Log, All);

/**
 * Low-level Niagara MCP tools.
 *
 * These tools intentionally behave like asset/graph command-line primitives:
 * query, read, export text, union-merge write, and explicit delete.
 */
UCLASS()
class MASSBATTLEEDITORMCP_API UMassBattleNiagaraMCPApi : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|Niagara")
	static FString MCP_NiagaraGetApiStatus();

	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|Niagara")
	static FString MCP_NiagaraQuery(const FString& QueryJson);

	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|Niagara")
	static FString MCP_NiagaraReadSummary(const FString& SystemPath, const FString& OptionsJson);

	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|Niagara")
	static FString MCP_NiagaraReadModule(const FString& SystemPath, const FString& SelectorJson);

	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|Niagara")
	static FString MCP_NiagaraReadAll(const FString& SystemPath, const FString& OptionsJson);

	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|Niagara")
	static FString MCP_NiagaraExportText(const FString& SystemPath, const FString& OptionsJson);

	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|Niagara")
	static FString MCP_NiagaraMergeWrite(const FString& SystemPath, const FString& PatchJson, bool bSaveAssets);

	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|Niagara")
	static FString MCP_NiagaraSetModulePin(const FString& SystemPath, const FString& SelectorJson, const FString& PinName, const FString& ValueText, bool bSaveAssets);

	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|Niagara")
	static FString MCP_NiagaraSetEmitterEnabled(const FString& SystemPath, const FString& EmitterName, bool bEnabled, bool bSaveAssets);

	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|Niagara")
	static FString MCP_NiagaraDelete(const FString& SystemPath, const FString& DeleteJson, bool bSaveAssets);
};
