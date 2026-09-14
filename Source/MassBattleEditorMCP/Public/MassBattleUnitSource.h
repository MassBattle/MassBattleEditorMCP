// Copyright (c) 2026 Winyunq. All rights reserved.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/DeveloperSettings.h"
#include "MassBattleUnitSource.generated.h"

class UMassBattleAgentConfigDataAsset;

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
	friend class UMassBattleUnitMCPApi;
	UPROPERTY(Instanced)
	TObjectPtr<UMassBattleAgentConfigDataAsset> LastUpdatedSource;
	UPROPERTY()
	bool bHasUpdated = false;
};
