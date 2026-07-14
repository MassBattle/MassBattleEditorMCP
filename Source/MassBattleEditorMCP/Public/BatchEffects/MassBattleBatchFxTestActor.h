#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MassBattleBatchFxTestActor.generated.h"

class UCameraComponent;
class USceneComponent;
class UTextRenderComponent;

/** Editor-only PIE harness for exercising MassBattle NDC burst renderers. */
UCLASS(Blueprintable)
class MASSBATTLEEDITORMCP_API AMassBattleBatchFxTestActor : public AActor
{
	GENERATED_BODY()

public:
	AMassBattleBatchFxTestActor();

	virtual void BeginPlay() override;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MassBattle Batch FX Test")
	bool bAutoPlay = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MassBattle Batch FX Test", meta = (ClampMin = "0.1"))
	float InitialDelay = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MassBattle Batch FX Test", meta = (ClampMin = "1.0"))
	float LoopInterval = 8.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MassBattle Batch FX Test", meta = (ClampMin = "1", ClampMax = "4"))
	int32 GridSide = 2;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MassBattle Batch FX Test", meta = (ClampMin = "50.0"))
	float GridSpacing = 350.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MassBattle Batch FX Test")
	int32 MuzzleSubType = 40;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MassBattle Batch FX Test")
	int32 ExplosionSubType = 41;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MassBattle Batch FX Test")
	FVector MuzzleCenter = FVector(-250.0, -700.0, 150.0);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MassBattle Batch FX Test")
	FVector ExplosionCenter = FVector(0.0, 700.0, 150.0);

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "MassBattle Batch FX Test")
	void TriggerMuzzle();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "MassBattle Batch FX Test")
	void TriggerExplosion();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "MassBattle Batch FX Test")
	void TriggerAll();

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "MassBattle Batch FX Test")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "MassBattle Batch FX Test")
	TObjectPtr<UCameraComponent> OverviewCamera;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "MassBattle Batch FX Test")
	TObjectPtr<UTextRenderComponent> TitleText;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "MassBattle Batch FX Test")
	TObjectPtr<UTextRenderComponent> MuzzleText;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "MassBattle Batch FX Test")
	TObjectPtr<UTextRenderComponent> ExplosionText;

private:
	void WarmUpRenderers();
	void SpawnGrid(int32 SubTypeIndex, const FVector& LocalCenter, float EffectScale, float LifeSpan, const TCHAR* Label);
	void LogRendererDiagnostics(float SecondsAfterTrigger);

	FTimerHandle AutoPlayTimer;
	FTimerHandle WarmUpTimer;
};
