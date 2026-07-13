#include "BatchEffects/MassBattleBatchFxTestActor.h"

#include "Camera/CameraComponent.h"
#include "Components/SceneComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "FuncLibs/MassBattleFuncLib.h"
#include "FuncLibs/MassBattleTagHelpers.h"
#include "Fragments/FxHostConfig.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "MassBattleEditorMCP.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitterInstance.h"
#include "NiagaraSystemInstance.h"
#include "Renderers/MassBattleFxRenderer.h"
#include "Subsystems/MassBattleSubsystem.h"
#include "TimerManager.h"

AMassBattleBatchFxTestActor::AMassBattleBatchFxTestActor()
{
	PrimaryActorTick.bCanEverTick = false;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	OverviewCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("OverviewCamera"));
	OverviewCamera->SetupAttachment(SceneRoot);
	OverviewCamera->SetRelativeLocation(FVector(-3200.0, 0.0, 1200.0));
	OverviewCamera->SetRelativeRotation(FRotator(-18.0, 0.0, 0.0));
	OverviewCamera->FieldOfView = 60.0f;

	auto ConfigureText = [this](UTextRenderComponent* Text, const FVector& Location, const FColor& Color, float WorldSize)
	{
		Text->SetupAttachment(SceneRoot);
		Text->SetRelativeLocation(Location);
		Text->SetRelativeRotation(FRotator(0.0, 180.0, 0.0));
		Text->SetHorizontalAlignment(EHorizTextAligment::EHTA_Center);
		Text->SetTextRenderColor(Color);
		Text->SetWorldSize(WorldSize);
	};

	TitleText = CreateDefaultSubobject<UTextRenderComponent>(TEXT("TitleText"));
	ConfigureText(TitleText, FVector(0.0, 0.0, 650.0), FColor::White, 70.0f);
	TitleText->SetText(FText::FromString(TEXT("MassBattle NDC Batch FX Test | 1: Muzzle | 2: Explosion | Space: Both")));

	MuzzleText = CreateDefaultSubobject<UTextRenderComponent>(TEXT("MuzzleText"));
	ConfigureText(MuzzleText, FVector(0.0, -700.0, 420.0), FColor(255, 170, 40), 85.0f);
	MuzzleText->SetText(FText::FromString(TEXT("MUZZLE | SubType 40")));

	ExplosionText = CreateDefaultSubobject<UTextRenderComponent>(TEXT("ExplosionText"));
	ConfigureText(ExplosionText, FVector(0.0, 700.0, 420.0), FColor(255, 70, 30), 85.0f);
	ExplosionText->SetText(FText::FromString(TEXT("EXPLOSION | SubType 41")));
}

void AMassBattleBatchFxTestActor::BeginPlay()
{
	Super::BeginPlay();

	if (APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0))
	{
		PlayerController->SetViewTargetWithBlend(this, 0.0f);
		EnableInput(PlayerController);
		if (InputComponent)
		{
			InputComponent->BindKey(EKeys::One, IE_Pressed, this, &AMassBattleBatchFxTestActor::TriggerMuzzle);
			InputComponent->BindKey(EKeys::Two, IE_Pressed, this, &AMassBattleBatchFxTestActor::TriggerExplosion);
			InputComponent->BindKey(EKeys::SpaceBar, IE_Pressed, this, &AMassBattleBatchFxTestActor::TriggerAll);
		}
	}

	if (bAutoPlay)
	{
		// The renderer creates its Niagara component lazily on the first NDC event.
		// Give that one-shot event a dedicated warm-up cycle so the visible cycle is
		// not lost while the Niagara system compiles asynchronously.
		GetWorldTimerManager().SetTimer(
			WarmUpTimer,
			this,
			&AMassBattleBatchFxTestActor::WarmUpRenderers,
			0.25f,
			false);

		GetWorldTimerManager().SetTimer(
			AutoPlayTimer,
			this,
			&AMassBattleBatchFxTestActor::TriggerAll,
			FMath::Max(LoopInterval, 1.0f),
			true,
			FMath::Max(InitialDelay, 0.1f));
	}

	UE_LOG(LogMassBattleEditorMCP, Display, TEXT("[ArmyVFXBatchTest] Started. AutoPlay=%s, MuzzleSubType=%d, ExplosionSubType=%d"), bAutoPlay ? TEXT("true") : TEXT("false"), MuzzleSubType, ExplosionSubType);
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 8.0f, FColor::Green, TEXT("ArmyVFX batch test ready: 1=Muzzle, 2=Explosion, Space=Both"));
	}
}

void AMassBattleBatchFxTestActor::WarmUpRenderers()
{
	SpawnGrid(MuzzleSubType, MuzzleCenter, 1.0f, 1.5f, TEXT("MuzzleWarmup"));
	SpawnGrid(ExplosionSubType, ExplosionCenter, 0.30f, 5.0f, TEXT("ExplosionWarmup"));
	UE_LOG(LogMassBattleEditorMCP, Display, TEXT("[ArmyVFXBatchTest] Renderer warm-up requested; visible auto-play follows after %.2fs."), InitialDelay);
}

void AMassBattleBatchFxTestActor::TriggerMuzzle()
{
	SpawnGrid(MuzzleSubType, MuzzleCenter, 1.0f, 1.5f, TEXT("Muzzle"));
}

void AMassBattleBatchFxTestActor::TriggerExplosion()
{
	SpawnGrid(ExplosionSubType, ExplosionCenter, 0.30f, 5.0f, TEXT("Explosion"));
}

void AMassBattleBatchFxTestActor::TriggerAll()
{
	TriggerMuzzle();
	TriggerExplosion();

	for (const float Delay : {0.1f, 0.4f, 1.0f})
	{
		FTimerHandle DiagnosticHandle;
		GetWorldTimerManager().SetTimer(
			DiagnosticHandle,
			FTimerDelegate::CreateUObject(this, &AMassBattleBatchFxTestActor::LogRendererDiagnostics, Delay),
			Delay,
			false);
	}
}

void AMassBattleBatchFxTestActor::LogRendererDiagnostics(float SecondsAfterTrigger)
{
	UMassBattleSubsystem* MassBattleSubsystem = UMassBattleSubsystem::GetPtr(this);
	if (!MassBattleSubsystem)
	{
		UE_LOG(LogMassBattleEditorMCP, Error, TEXT("[ArmyVFXBatchTest] Diagnostics t+%.1fs failed: MassBattleSubsystem is unavailable."), SecondsAfterTrigger);
		return;
	}

	for (const TPair<const TCHAR*, int32> Entry : {
		TPair<const TCHAR*, int32>(TEXT("Muzzle"), MuzzleSubType),
		TPair<const TCHAR*, int32>(TEXT("Explosion"), ExplosionSubType)})
	{
		AMassBattleFxRenderer* Renderer = MassBattleSubsystem->FxRenderers.FindRef(Entry.Value);
		if (!IsValid(Renderer))
		{
			UE_LOG(LogMassBattleEditorMCP, Error, TEXT("[ArmyVFXBatchTest] Diagnostics t+%.1fs %s: renderer missing for SubType=%d."), SecondsAfterTrigger, Entry.Key, Entry.Value);
			continue;
		}

		int32 ValidComponents = 0;
		int32 ActiveComponents = 0;
		int32 CompleteComponents = 0;
		int32 ActiveParticles = 0;
		int32 PendingBurstEvents = 0;
		for (const TPair<int32, FFxRenderBatchData>& BatchPair : Renderer->SpawnedRenderBatches)
		{
			const FFxRenderBatchData& Batch = BatchPair.Value;
			PendingBurstEvents += Batch.LocationArray_Burst.Num();
			UNiagaraComponent* NiagaraComponent = Batch.SpawnedNiagaraSystem;
			if (!IsValid(NiagaraComponent))
			{
				continue;
			}

			++ValidComponents;
			ActiveComponents += NiagaraComponent->IsActive() ? 1 : 0;
			CompleteComponents += NiagaraComponent->IsComplete() ? 1 : 0;

			PRAGMA_DISABLE_DEPRECATION_WARNINGS
			if (const FNiagaraSystemInstance* SystemInstance = NiagaraComponent->GetSystemInstance())
			{
				for (const FNiagaraEmitterInstanceRef& EmitterInstance : SystemInstance->GetEmitters())
				{
					ActiveParticles += EmitterInstance->GetNumParticles();
				}
			}
			PRAGMA_ENABLE_DEPRECATION_WARNINGS
		}

		UE_LOG(
			LogMassBattleEditorMCP,
			Display,
			TEXT("[ArmyVFXBatchTest] Diagnostics t+%.1fs %s: SubType=%d, Batches=%d, ValidComponents=%d, ActiveComponents=%d, CompleteComponents=%d, ActiveParticles=%d, PendingBurstEvents=%d"),
			SecondsAfterTrigger,
			Entry.Key,
			Entry.Value,
			Renderer->SpawnedRenderBatches.Num(),
			ValidComponents,
			ActiveComponents,
			CompleteComponents,
			ActiveParticles,
			PendingBurstEvents);
	}
}

void AMassBattleBatchFxTestActor::SpawnGrid(int32 SubTypeIndex, const FVector& LocalCenter, float EffectScale, float LifeSpan, const TCHAR* Label)
{
	UMassBattleSubsystem* MassBattleSubsystem = UMassBattleSubsystem::GetPtr(this);
	if (!MassBattleSubsystem)
	{
		UE_LOG(LogMassBattleEditorMCP, Error, TEXT("[ArmyVFXBatchTest] %s trigger failed: MassBattleSubsystem is unavailable."), Label);
		return;
	}

	if (!MassBattleSubsystem->FxRenderers.Contains(SubTypeIndex))
	{
		UE_LOG(LogMassBattleEditorMCP, Error, TEXT("[ArmyVFXBatchTest] %s trigger failed: no FxRenderer is registered for SubType %d."), Label, SubTypeIndex);
		return;
	}

	FFxConfig Config;
	Config.bEnable = true;
	// EESubType begins with None, so its raw enum ordinal is not the renderer
	// SubType index. Always use the MassBattle conversion helper.
	Config.SubType = UMassBattleTagHelpers::SubTypeIndexToEnum(SubTypeIndex);
	Config.StyleType = EEStyleType::Style0;
	Config.Transform = FTransform3f::Identity;
	Config.Transform.SetScale3D(FVector3f(EffectScale));
	Config.bAttached = false;
	Config.Quantity = 1;
	Config.Delay = 0.0f;
	Config.LifeSpan = LifeSpan;
	Config.bDespawnWhenNoParent = true;

	const int32 Side = FMath::Clamp(GridSide, 1, 4);
	const float HalfSpan = 0.5f * static_cast<float>(Side - 1) * GridSpacing;
	int32 SpawnedCount = 0;
	int32 SetHandleCount = 0;
	TSet<FEntityHandle> UniqueHandles;
	for (int32 Row = 0; Row < Side; ++Row)
	{
		for (int32 Column = 0; Column < Side; ++Column)
		{
			const FVector GridOffset(
				static_cast<float>(Row) * GridSpacing - HalfSpan,
				static_cast<float>(Column) * GridSpacing - HalfSpan,
				0.0);
			const FVector WorldLocation = GetActorTransform().TransformPosition(LocalCenter + GridOffset);
			const FTransform SpawnTransform(GetActorQuat(), WorldLocation, FVector::OneVector);
			const FEntityHandle HostHandle = UMassBattleFuncLib::SpawnBatchedFx(this, Config, SpawnTransform);
			if (HostHandle.IsSet())
			{
				++SetHandleCount;
				UniqueHandles.Add(HostHandle);
			}
			++SpawnedCount;
		}
	}

	UE_LOG(
		LogMassBattleEditorMCP,
		Display,
		TEXT("[ArmyVFXBatchTest] Triggered %s: SubType=%d, Events=%d, SetHostHandles=%d, UniqueHostHandles=%d"),
		Label,
		SubTypeIndex,
		SpawnedCount,
		SetHandleCount,
		UniqueHandles.Num());
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 2.0f, FColor::Cyan, FString::Printf(TEXT("%s SubType %d: %d NDC events"), Label, SubTypeIndex, SpawnedCount));
	}
}
