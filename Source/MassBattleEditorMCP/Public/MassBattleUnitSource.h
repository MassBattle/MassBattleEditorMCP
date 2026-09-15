// Copyright (c) 2026 Winyunq. All rights reserved.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/DeveloperSettings.h"
#include "MassBattleUnitSource.generated.h"

class UMassBattleAgentConfigDataAsset;
class UStaticMeshComponent;
class UArrowComponent;
class UMaterialInterface;
class UTexture2D;

/** Shared editor export defaults; per-source ExportPath takes precedence. */
UCLASS(Config=Editor, DefaultConfig, meta=(DisplayName="MassBattle Unit Export"))
class MASSBATTLEEDITORMCP_API UMassBattleUnitSourceSettings : public UDeveloperSettings
{
	GENERATED_BODY()
public:
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
	UPROPERTY(Config, EditAnywhere, Category="Export", meta=(LongPackageName))
	FString ExportRoot = TEXT("/Game/Units");
};

/** Blueprint asset containing source configuration; never a runtime Mass unit. */
UCLASS(Blueprintable)
class MASSBATTLEEDITORMCP_API AMassBattleUnitSource : public AActor
{
	GENERATED_BODY()
public:
	AMassBattleUnitSource();
	virtual bool IsEditorOnly() const override { return true; }
	virtual void PostInitProperties() override;
	virtual void BeginDestroy() override;
	virtual void OnConstruction(const FTransform& Transform) override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Source Geometry")
	TObjectPtr<UStaticMeshComponent> Body;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Source Geometry")
	TObjectPtr<UStaticMeshComponent> Turret;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Source Geometry")
	TObjectPtr<UStaticMeshComponent> Barrel;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Source Markers")
	TObjectPtr<UArrowComponent> TurretPivot;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Source Markers")
	TObjectPtr<UArrowComponent> BarrelPivot;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Source Markers")
	TObjectPtr<UArrowComponent> Muzzle;

	/** Material reads the shared minimap team buffer; PreviewTeamIndex selects the preview team. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Team Color")
	TObjectPtr<UMaterialInterface> TeamMaterial;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Team Color")
	TObjectPtr<UTexture2D> TeamMask;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Team Color", meta=(ClampMin="0", ClampMax="1023"))
	int32 PreviewTeamIndex = 1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Team Color", meta=(ClampMin="0", ClampMax="1"))
	float TeamTintStrength = 1.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Team Color")
	bool bPreviewTeamColor = false;
	UFUNCTION(BlueprintCallable, Category="Team Color", meta=(DisplayName="Apply Team Color"))
	void ApplyTeamColor();
	UFUNCTION(BlueprintCallable, Category="Team Color", meta=(DisplayName="Show Original Color"))
	void ShowOriginalColor();

	UPROPERTY(EditAnywhere, Instanced, BlueprintReadOnly, Category="Unit", meta=(ShowOnlyInnerProperties, NoClear))
	TObjectPtr<UMassBattleAgentConfigDataAsset> UnitData;

	/** Full output package name, e.g. /Game/Units/DA_Tank. Empty uses export defaults. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Export", meta=(LongPackageName))
	FString ExportPath;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Export")
	TObjectPtr<UMassBattleAgentConfigDataAsset> ExportedUnit;

	UPROPERTY(VisibleAnywhere, Category="Export", meta=(MultiLine))
	FString LastReport;

	UFUNCTION(BlueprintCallable, Category="Export", meta=(DisplayName="Update"))
	void Update();

	UFUNCTION(BlueprintCallable, Category="Export", meta=(DisplayName="Destroy"))
	void DestroySource();

	static AMassBattleUnitSource* Resolve(UObject* Object);
	static void RegisterDetails(bool bRegister);
	UObject* GetSourceAsset() const;

private:
	bool ConfirmDestroyUnit() const;
	void AddAssociatedUnitToDeletion(const TArray<UObject*>& Objects, TSet<UObject*>& ExtraObjects);
	void ApplyPreviewMaterial();
	void RefreshSourcePreview();
	friend class UMassBattleUnitMCPApi;
	UPROPERTY(Instanced)
	TObjectPtr<UMassBattleAgentConfigDataAsset> LastUpdatedSource;
	UPROPERTY()
	bool bHasUpdated = false;
};
