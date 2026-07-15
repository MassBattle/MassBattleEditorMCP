// Copyright (c) 2025-2026 Winyunq. All rights reserved.
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "MassBattleUnitEditorMCPApi.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogMassBattleUnitEditorMCPApi, Log, All);

/**
 * MCP version of the MassBattle editor workflow.
 *
 * The original MassBattleEditor module exposes Blueprint-callable editor tools
 * for converting purchased assets into MassBattle-ready data. This API composes
 * those tools into reviewable MCP editor operations.
 */
UCLASS()
class MASSBATTLEEDITORMCP_API UMassBattleUnitEditorMCPApi : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** List MCP editor profiles and authoring recipes. */
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|UnitEditor")
	static FString MCP_EditorListProfiles(const FString& OptionsJson);

	/** Read one MCP editor profile or recipe JSON file. ProfileType: style or recipe. */
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|UnitEditor")
	static FString MCP_EditorGetProfile(const FString& ProfileType, const FString& ProfileId);

	/** Plan a staged unit authoring workflow across prepare, animation update, VAT create/refresh, and organization steps. */
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|UnitEditor")
	static FString MCP_EditorPlanUnitAuthoringWorkflow(const FString& SpecJson);

	/** Apply a reviewed unit authoring workflow. SpecJson defaults to dry_run=true. */
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|UnitEditor")
	static FString MCP_EditorApplyUnitAuthoringWorkflow(const FString& SpecJson, bool bSaveAssets);

	/** Plan discovery, official-style naming, and optional source-folder organization for a purchased skeletal asset pack. */
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|UnitEditor")
	static FString MCP_EditorPlanPreparePurchasedAsset(const FString& SpecJson);

	/** Apply a reviewed purchased-asset preparation plan. SpecJson defaults to dry_run=true. */
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|UnitEditor")
	static FString MCP_EditorApplyPreparePurchasedAsset(const FString& SpecJson, bool bSaveAssets);

	/** Discover compatible animation sequences across explicit and style-configured search roots. */
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|UnitEditor")
	static FString MCP_EditorDiscoverCompatibleAnimations(const FString& SkeletalMeshPath, const FString& OptionsJson);

	/** Plan adding or replacing a unit animation set using MassBattleEditor asset-discovery/conversion functions. */
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|UnitEditor")
	static FString MCP_EditorPlanAddAnimationsToUnit(const FString& UnitPath, const FString& SpecJson);

	/** Validate whether an animation-set edit can produce an applicable unit merge plan. */
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|UnitEditor")
	static FString MCP_EditorValidateAddAnimationsToUnit(const FString& UnitPath, const FString& SpecJson);

	/** Convenience wrapper: plan animation-set edit and apply it when applicable. */
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|UnitEditor")
	static FString MCP_EditorApplyAddAnimationsToUnit(const FString& UnitPath, const FString& SpecJson, bool bSaveAssets);

	/** Plan the MassBattleEditor VAT skeletal-mesh unit authoring workflow without writing generated assets. */
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|UnitEditor")
	static FString MCP_EditorPlanCreateVatUnit(const FString& SpecJson);

	/** Validate the VAT skeletal-mesh unit authoring workflow before executing it. */
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|UnitEditor")
	static FString MCP_EditorValidateCreateVatUnit(const FString& SpecJson);

	/** Execute the VAT skeletal-mesh unit authoring workflow. SpecJson supports dry_run and overwrite_existing safety options. */
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|UnitEditor")
	static FString MCP_EditorApplyCreateVatUnit(const FString& SpecJson, bool bSaveAssets);

	/** Build a VAT unit create spec from current editor selection or selected_assets, then plan the create workflow. */
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|UnitEditor")
	static FString MCP_EditorPlanCreateVatUnitFromSelection(const FString& OptionsJson);

	/** Build a VAT unit create spec from current editor selection or selected_assets, then apply the create workflow. */
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|UnitEditor")
	static FString MCP_EditorApplyCreateVatUnitFromSelection(const FString& OptionsJson, bool bSaveAssets);

	/** Inspect the renderable skeletal/static component assembly produced by an Actor class and optional component overrides. */
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|UnitEditor")
	static FString MCP_EditorInspectActorAssembly(const FString& ActorPath, const FString& OptionsJson);

	/** Audit assembled skin weights, baked VAT position-frame motion, lookup UVs, and the selected animation profile. */
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|UnitEditor")
	static FString MCP_EditorInspectVatAnimation(const FString& SpecJson);

	/** Plan Actor component assembly followed by the strict VAT unit authoring workflow without writing assets. */
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|UnitEditor")
	static FString MCP_EditorPlanCreateVatUnitFromActor(const FString& SpecJson);

	/** Assemble an Actor into an animation-compatible SkeletalMesh, then execute the strict VAT unit authoring workflow. */
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|UnitEditor")
	static FString MCP_EditorApplyCreateVatUnitFromActor(const FString& SpecJson, bool bSaveAssets);

	/** Plan moving a unit and its MassBattle editor-generated linked assets into the selected style layout. */
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|UnitEditor")
	static FString MCP_EditorPlanOrganizeUnitAssets(const FString& UnitPath, const FString& OptionsJson);

	/** Apply a reviewed unit asset organization plan. OptionsJson defaults to dry_run=true. */
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|UnitEditor")
	static FString MCP_EditorApplyOrganizeUnitAssets(const FString& UnitPath, const FString& OptionsJson, bool bSaveAssets);

	/** Return categorized metadata for this MCP editor layer. */
	UFUNCTION(BlueprintCallable, Category = "MassBattleEditorMCP|UnitEditor")
	static FString MCP_EditorGetStatus();
};
