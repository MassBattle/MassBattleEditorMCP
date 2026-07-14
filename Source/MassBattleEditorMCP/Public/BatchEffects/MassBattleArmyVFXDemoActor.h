#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MassAPIStructs.h"
#include "MassBattleArmyVFXDemoActor.generated.h"

class UCameraComponent;
class USceneComponent;
class UTextRenderComponent;

/**
 * Batch-only PIE acceptance harness for all 27 ArmyVFX Niagara entry systems.
 *
 * The actor never spawns a source Niagara component and never uses an unbatched
 * Niagara/Cascade asset.  Every visible effect is requested through
 * UMassBattleFuncLib::SpawnBatchedFx and resolved by a placed
 * AMassBattleFxRenderer actor registered for the effect SubType.
 */
UCLASS(Blueprintable)
class MASSBATTLEEDITORMCP_API AMassBattleArmyVFXDemoActor : public AActor
{
	GENERATED_BODY()

public:
	AMassBattleArmyVFXDemoActor();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;

	/** Warm all renderer systems below the stage before the first visible pass. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ArmyVFX Batch Demo")
	bool bWarmUpRenderers = true;

	/** Automatically replay the complete 27-effect batch-only gallery. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ArmyVFX Batch Demo")
	bool bAutoPlayAll = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ArmyVFX Batch Demo", meta = (ClampMin = "0.5"))
	float InitialDelay = 3.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ArmyVFX Batch Demo", meta = (ClampMin = "8.0"))
	float ReplayInterval = 12.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ArmyVFX Batch Demo", meta = (ClampMin = "0", ClampMax = "26"))
	int32 CurrentEffectIndex = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ArmyVFX Batch Demo", meta = (ClampMin = "600.0"))
	float ColumnSpacing = 1250.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ArmyVFX Batch Demo", meta = (ClampMin = "500.0"))
	float RowSpacing = 1050.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ArmyVFX Batch Demo", meta = (ClampMin = "0.01"))
	float PreviewScaleMultiplier = 1.0f;

	/** Replay all 27 source-faithful MassBattle batch translations. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "ArmyVFX Batch Demo")
	void TriggerAllBatchEffects();

	/** Replay only the selected batch translation at its gallery cell. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "ArmyVFX Batch Demo")
	void TriggerCurrentEffect();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "ArmyVFX Batch Demo")
	void NextEffect();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "ArmyVFX Batch Demo")
	void PreviousEffect();

	/** Log renderer registration, target asset, render-batch, and particle evidence. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "ArmyVFX Batch Demo")
	void LogAcceptanceDiagnostics();

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "ArmyVFX Batch Demo")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "ArmyVFX Batch Demo")
	TObjectPtr<UCameraComponent> OverviewCamera;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "ArmyVFX Batch Demo")
	TObjectPtr<UTextRenderComponent> TitleText;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "ArmyVFX Batch Demo")
	TObjectPtr<UTextRenderComponent> StatusText;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "ArmyVFX Batch Demo")
	TArray<TObjectPtr<UTextRenderComponent>> EffectLabels;

private:
	struct FMovingBatchFx
	{
		FEntityHandle Host;
		FVector Center = FVector::ZeroVector;
		float Radius = 300.0f;
		float LinearSpeed = 0.0f;
		float PhaseOffset = 0.0f;
	};

	void WarmUpAllRenderers();
	void DestroyWarmupEffects();
	void TriggerAllInternal(bool bWarmup);
	void TriggerSpec(int32 SpecIndex, const FVector& WorldLocation, bool bWarmup);
	FEntityHandle SpawnRecipeStep(
		int32 SpecIndex,
		bool bAttached,
		int32 StyleIndex,
		float Delay,
		float LifeSpan,
		const FVector& WorldLocation,
		bool bWarmup);
	void CleanupVisibleEffects();
	void CleanupHosts(TArray<FEntityHandle>& Hosts);
	void UpdateMovingEffects(float DeltaSeconds);
	void UpdateStatusText();
	FVector GetGridLocation(int32 SpecIndex) const;
	void LogAcceptanceDiagnosticsAt(float SecondsAfterTrigger);

	TArray<FEntityHandle> ActiveVisibleHosts;
	TArray<FEntityHandle> ActiveWarmupHosts;
	TArray<FMovingBatchFx> MovingEffects;
	float MovementTime = 0.0f;

	FTimerHandle WarmupTimer;
	FTimerHandle WarmupCleanupTimer;
	FTimerHandle InitialTriggerTimer;
	FTimerHandle ReplayTimer;
};
