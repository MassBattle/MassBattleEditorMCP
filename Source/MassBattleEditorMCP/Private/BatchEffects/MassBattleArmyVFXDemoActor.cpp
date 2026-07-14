#include "BatchEffects/MassBattleArmyVFXDemoActor.h"

#include "Camera/CameraComponent.h"
#include "Components/SceneComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/Engine.h"
#include "Fragments/FxHostConfig.h"
#include "FuncLibs/MassBattleFuncLib.h"
#include "FuncLibs/MassBattleTagHelpers.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "MassAPISubsystem.h"
#include "MassBattleEditorMCP.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitterInstance.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemInstance.h"
#include "Renderers/MassBattleFxRenderer.h"
#include "Subsystems/MassBattleSubsystem.h"
#include "TimerManager.h"

namespace
{
struct FArmyVFXRecipeStep
{
	bool bAttached;
	int32 Style;
	float Delay;
	float LifeSpan;
};

struct FArmyVFXDemoSpec
{
	const TCHAR* Category;
	const TCHAR* Name;
	const TCHAR* TargetSystem;
	const TCHAR* RendererClass;
	int32 SubType;
	float Scale;
	int32 StepCount;
	FArmyVFXRecipeStep Steps[2];
	float MovementSpeed;
};

#define ARMY_BATCH_SPEC(CategoryName, SourceName, TargetStem, RendererName, SubTypeValue, ScaleValue, AttachedValue, LifeValue, SpeedValue) \
	{ TEXT(CategoryName), TEXT(SourceName), \
		TEXT("/Game/ArmyVFX/WinyunqRelease/Effects/NS_MB_" TargetStem "_Batch.NS_MB_" TargetStem "_Batch"), \
		TEXT("/Game/ArmyVFX/WinyunqRelease/Renderers/" RendererName "." RendererName "_C"), \
		SubTypeValue, ScaleValue, 1, {{AttachedValue, 0, 0.0f, LifeValue}, {false, 0, 0.0f, 0.0f}}, SpeedValue }

#define ARMY_HYBRID_SPEC(CategoryName, SourceName, TargetStem, RendererName, SubTypeValue, ScaleValue, AttachedLifeValue) \
	{ TEXT(CategoryName), TEXT(SourceName), \
		TEXT("/Game/ArmyVFX/WinyunqRelease/Effects/NS_MB_" TargetStem "_Batch.NS_MB_" TargetStem "_Batch"), \
		TEXT("/Game/ArmyVFX/WinyunqRelease/Renderers/" RendererName "." RendererName "_C"), \
		SubTypeValue, ScaleValue, 2, {{false, 0, 0.0f, FMath::Max(AttachedLifeValue, 1.0f)}, {true, 0, 0.0f, AttachedLifeValue}}, 0.0f }

static const FArmyVFXDemoSpec GArmyVFXDemoSpecs[] =
{
	// The second burst preserves the source smoke_after Spawn Time of 0.3 seconds.
	{ TEXT("Destroyed"), TEXT("NS_Expl_Tank_1"),
		TEXT("/Game/ArmyVFX/WinyunqRelease/Effects/NS_MB_Expl_Tank_1_Batch.NS_MB_Expl_Tank_1_Batch"),
		TEXT("/Game/ArmyVFX/WinyunqRelease/Renderers/BP_FxRenderer_ExplTank_Batch.BP_FxRenderer_ExplTank_Batch_C"),
		97, 0.30f, 2, {{false, 0, 0.0f, 5.0f}, {false, 1, 0.3f, 5.0f}}, 0.0f },
	ARMY_BATCH_SPEC("Destroyed", "NS_Expl_Tank_Tower_1", "Expl_Tank_Tower_1", "BP_FxRenderer_ExplTankTower_Batch", 98, 0.35f, true, 10.0f, 0.0f),
	ARMY_BATCH_SPEC("Destroyed", "NS_Fire_Tank_1", "Fire_Tank_1", "BP_FxRenderer_FireTank_Batch", 89, 0.45f, true, 8.0f, 0.0f),
	ARMY_BATCH_SPEC("Destroyed", "NS_Tank_FireShells_1", "Tank_FireShells_1", "BP_FxRenderer_TankFireShells_Batch", 90, 0.60f, true, 3.0f, 0.0f),
	ARMY_BATCH_SPEC("Environment", "NS_AA_Gun_1", "AA_Gun_1", "BP_FxRenderer_AAGun_Batch", 91, 0.65f, true, 8.0f, 0.0f),
	ARMY_BATCH_SPEC("Environment", "NS_AA_SplashGround_1", "AA_SplashGround_1", "BP_FxRenderer_AASplash_Batch", 70, 0.50f, true, 1.0f, 0.0f),
	ARMY_BATCH_SPEC("Environment", "NS_Arty_SplashGround_1", "Arty_SplashGround_1", "BP_FxRenderer_ArtySplash_Batch", 80, 0.50f, false, 5.0f, 0.0f),
	ARMY_BATCH_SPEC("Environment", "NS_Heli_SplashGround_1", "Heli_SplashGround_1", "BP_FxRenderer_HeliSplash_Batch", 88, 0.55f, true, 8.0f, 0.0f),
	ARMY_BATCH_SPEC("Environment", "NS_Tank_SplashGround_1", "Tank_SplashGround_1", "BP_FxRenderer_TankSplash_Batch", 81, 0.50f, false, 5.0f, 0.0f),
	ARMY_BATCH_SPEC("Jet", "NS_Jet_countermeasures", "Jet_countermeasures", "BP_FxRenderer_JetCountermeasures_Batch", 82, 0.80f, false, 5.0f, 0.0f),
	ARMY_BATCH_SPEC("Jet", "NS_Jet_Fire_Continuous", "Jet_Fire_Continuous", "BP_FxRenderer_JetFireContinuous_Batch", 92, 0.80f, true, 8.0f, 6000.0f),
	ARMY_BATCH_SPEC("Jet", "NS_Jet_Trails", "Jet_Trails", "BP_FxRenderer_JetTrails74_Batch", 74, 0.80f, true, 8.0f, 6000.0f),
	ARMY_HYBRID_SPEC("MuzzleFlash", "NS_MuzzleFlash_APC_1", "MuzzleFlash_APC_1", "BP_FxRenderer_MuzzleFlashAPC_Batch", 95, 1.00f, 0.1f),
	ARMY_HYBRID_SPEC("MuzzleFlash", "NS_MuzzleFlash_Arty_1", "MuzzleFlash_Arty_1", "BP_FxRenderer_MuzzleFlashArty_Batch", 71, 1.00f, 1.0f),
	ARMY_HYBRID_SPEC("MuzzleFlash", "NS_MuzzleFlash_SPG_1", "MuzzleFlash_SPG_1", "BP_FxRenderer_MuzzleFlashSPG_Batch", 72, 1.00f, 1.0f),
	ARMY_HYBRID_SPEC("MuzzleFlash", "NS_MuzzleFlash_Tank_Maingun_1", "MuzzleFlash_Tank_Maingun_1", "BP_FxRenderer_MuzzleFlashTankMainGun_Batch", 73, 1.00f, 1.0f),
	ARMY_HYBRID_SPEC("MuzzleFlash", "NS_MuzzleFlash_Tank_Mashingun_1", "MuzzleFlash_Tank_Mashingun_1", "BP_FxRenderer_MuzzleFlashTankMG_Batch", 96, 1.00f, 0.1f),
	ARMY_BATCH_SPEC("Projectiles", "NS_Rocket_Engine_1", "Rocket_Engine_1", "BP_FxRenderer_RocketEngine1_Batch", 93, 0.80f, true, 8.0f, 2200.0f),
	ARMY_BATCH_SPEC("Projectiles", "NS_Rocket_Engine_2", "Rocket_Engine_2", "BP_FxRenderer_RocketEngine2_Batch", 94, 0.80f, true, 8.0f, 2200.0f),
	ARMY_BATCH_SPEC("Projectiles", "NS_Rocket_Smoke_1", "Rocket_Smoke_1", "BP_FxRenderer_RocketSmoke1_75_V2_Batch", 75, 0.80f, true, 8.0f, 2200.0f),
	ARMY_BATCH_SPEC("Projectiles", "NS_Rocket_Smoke_2", "Rocket_Smoke_2", "BP_FxRenderer_RocketSmoke2_76_V2_Batch", 76, 0.80f, true, 8.0f, 2200.0f),
	ARMY_BATCH_SPEC("Projectiles", "NS_Rocket_Start", "Rocket_Start", "BP_FxRenderer_RocketStart_Batch", 99, 0.75f, true, 0.2f, 0.0f),
	ARMY_BATCH_SPEC("Projectiles", "NS_StartFlash_1", "StartFlash_1", "BP_FxRenderer_StartFlash_Batch", 83, 0.75f, false, 5.0f, 0.0f),
	ARMY_BATCH_SPEC("Shells", "NS_Shells_APC_1", "Shells_APC_1", "BP_FxRenderer_ShellsAPC_Batch", 84, 1.00f, false, 5.0f, 0.0f),
	ARMY_BATCH_SPEC("Shells", "NS_Shells_Arty_1", "Shells_Arty_1", "BP_FxRenderer_ShellsArty_Batch", 85, 1.00f, false, 5.0f, 0.0f),
	ARMY_BATCH_SPEC("Shells", "NS_Shells_Tank_Mashingun_1", "Shells_Tank_Mashingun_1", "BP_FxRenderer_ShellsTankMG_Batch", 86, 1.00f, false, 5.0f, 0.0f),
	ARMY_BATCH_SPEC("SmokeScreen", "NS_SmokeScreen_APC", "SmokeScreen_APC", "BP_FxRenderer_SmokeScreenAPC_Batch", 87, 0.55f, false, 5.0f, 0.0f),
};

#undef ARMY_HYBRID_SPEC
#undef ARMY_BATCH_SPEC

constexpr int32 ArmyVFXDemoCount = UE_ARRAY_COUNT(GArmyVFXDemoSpecs);
constexpr int32 GridColumns = 6;

FVector GridLocalLocation(int32 SpecIndex, float ColumnSpacing, float RowSpacing)
{
	const int32 Row = SpecIndex / GridColumns;
	const int32 Column = SpecIndex % GridColumns;
	const float HalfColumns = 0.5f * static_cast<float>(GridColumns - 1);
	return FVector(
		static_cast<float>(Row) * RowSpacing,
		(static_cast<float>(Column) - HalfColumns) * ColumnSpacing,
		180.0f);
}
}

AMassBattleArmyVFXDemoActor::AMassBattleArmyVFXDemoActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	OverviewCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("OverviewCamera"));
	OverviewCamera->SetupAttachment(SceneRoot);
	OverviewCamera->SetRelativeLocation(FVector(-7000.0, 0.0, 6500.0));
	OverviewCamera->SetRelativeRotation(FRotator(-35.3, 0.0, 0.0));
	OverviewCamera->FieldOfView = 65.0f;

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
	ConfigureText(TitleText, FVector(2200.0, 0.0, 1250.0), FColor::White, 72.0f);
	TitleText->SetText(FText::FromString(TEXT("ArmyVFX | 27/27 MassBattleFrame Batch FX | Batch-only acceptance")));

	StatusText = CreateDefaultSubobject<UTextRenderComponent>(TEXT("StatusText"));
	ConfigureText(StatusText, FVector(2200.0, 0.0, 1110.0), FColor(100, 210, 255), 45.0f);

	EffectLabels.Reserve(ArmyVFXDemoCount);
	for (int32 Index = 0; Index < ArmyVFXDemoCount; ++Index)
	{
		UTextRenderComponent* Label = CreateDefaultSubobject<UTextRenderComponent>(
			*FString::Printf(TEXT("EffectLabel_%02d"), Index));
		ConfigureText(
			Label,
			GridLocalLocation(Index, ColumnSpacing, RowSpacing) + FVector(0.0, 0.0, 360.0),
			FColor(180, 225, 255),
			31.0f);
		Label->SetText(FText::FromString(FString::Printf(
			TEXT("%02d  %s\nSubType %d"),
			Index + 1,
			GArmyVFXDemoSpecs[Index].Name,
			GArmyVFXDemoSpecs[Index].SubType)));
		EffectLabels.Add(Label);
	}

	UpdateStatusText();
}

void AMassBattleArmyVFXDemoActor::BeginPlay()
{
	Super::BeginPlay();
	CurrentEffectIndex = FMath::Clamp(CurrentEffectIndex, 0, ArmyVFXDemoCount - 1);

	for (int32 Index = 0; Index < EffectLabels.Num(); ++Index)
	{
		EffectLabels[Index]->SetRelativeLocation(
			GridLocalLocation(Index, ColumnSpacing, RowSpacing) + FVector(0.0, 0.0, 360.0));
	}

	if (APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0))
	{
		PlayerController->SetViewTargetWithBlend(this, 0.0f);
		EnableInput(PlayerController);
		if (InputComponent)
		{
			InputComponent->BindKey(EKeys::SpaceBar, IE_Pressed, this, &AMassBattleArmyVFXDemoActor::TriggerAllBatchEffects);
			InputComponent->BindKey(EKeys::R, IE_Pressed, this, &AMassBattleArmyVFXDemoActor::TriggerCurrentEffect);
			InputComponent->BindKey(EKeys::Right, IE_Pressed, this, &AMassBattleArmyVFXDemoActor::NextEffect);
			InputComponent->BindKey(EKeys::D, IE_Pressed, this, &AMassBattleArmyVFXDemoActor::NextEffect);
			InputComponent->BindKey(EKeys::Left, IE_Pressed, this, &AMassBattleArmyVFXDemoActor::PreviousEffect);
			InputComponent->BindKey(EKeys::A, IE_Pressed, this, &AMassBattleArmyVFXDemoActor::PreviousEffect);
			InputComponent->BindKey(EKeys::V, IE_Pressed, this, &AMassBattleArmyVFXDemoActor::LogAcceptanceDiagnostics);
		}
	}

	if (bWarmUpRenderers)
	{
		GetWorldTimerManager().SetTimer(
			WarmupTimer,
			this,
			&AMassBattleArmyVFXDemoActor::WarmUpAllRenderers,
			0.35f,
			false);
	}

	if (bAutoPlayAll)
	{
		GetWorldTimerManager().SetTimer(
			InitialTriggerTimer,
			this,
			&AMassBattleArmyVFXDemoActor::TriggerAllBatchEffects,
			FMath::Max(InitialDelay, 0.5f),
			false);
		GetWorldTimerManager().SetTimer(
			ReplayTimer,
			this,
			&AMassBattleArmyVFXDemoActor::TriggerAllBatchEffects,
			FMath::Max(ReplayInterval, 8.0f),
			true,
			FMath::Max(InitialDelay, 0.5f) + FMath::Max(ReplayInterval, 8.0f));
	}

	UpdateStatusText();
	UE_LOG(LogMassBattleEditorMCP, Display,
		TEXT("[ArmyVFXFaithfulBatch] Demo started: entries=%d, source_components=0, unbatched_components=0."),
		ArmyVFXDemoCount);
}

void AMassBattleArmyVFXDemoActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorldTimerManager().ClearAllTimersForObject(this);
	CleanupVisibleEffects();
	CleanupHosts(ActiveWarmupHosts);
	Super::EndPlay(EndPlayReason);
}

void AMassBattleArmyVFXDemoActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UpdateMovingEffects(DeltaSeconds);
}

void AMassBattleArmyVFXDemoActor::WarmUpAllRenderers()
{
	CleanupHosts(ActiveWarmupHosts);
	TriggerAllInternal(true);
	GetWorldTimerManager().SetTimer(
		WarmupCleanupTimer,
		this,
		&AMassBattleArmyVFXDemoActor::DestroyWarmupEffects,
		1.5f,
		false);
	UE_LOG(LogMassBattleEditorMCP, Display,
		TEXT("[ArmyVFXFaithfulBatch] Warm-up requested for all 27 placed renderers below the stage."));
}

void AMassBattleArmyVFXDemoActor::DestroyWarmupEffects()
{
	CleanupHosts(ActiveWarmupHosts);
}

void AMassBattleArmyVFXDemoActor::TriggerAllBatchEffects()
{
	CleanupVisibleEffects();
	MovementTime = 0.0f;
	TriggerAllInternal(false);
	UpdateStatusText();

	for (const float Delay : {0.25f, 1.0f, 3.0f})
	{
		FTimerHandle DiagnosticTimer;
		GetWorldTimerManager().SetTimer(
			DiagnosticTimer,
			FTimerDelegate::CreateUObject(this, &AMassBattleArmyVFXDemoActor::LogAcceptanceDiagnosticsAt, Delay),
			Delay,
			false);
	}

	UE_LOG(LogMassBattleEditorMCP, Display,
		TEXT("[ArmyVFXFaithfulBatch] Visible gallery triggered: entries=27, recipe_steps=33, API=SpawnBatchedFx."));
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(
			-1,
			4.0f,
			FColor::Cyan,
			TEXT("ArmyVFX 27/27: MassBattle SpawnBatchedFx gallery triggered"));
	}
}

void AMassBattleArmyVFXDemoActor::TriggerCurrentEffect()
{
	CleanupVisibleEffects();
	MovementTime = 0.0f;
	CurrentEffectIndex = FMath::Clamp(CurrentEffectIndex, 0, ArmyVFXDemoCount - 1);
	TriggerSpec(CurrentEffectIndex, GetGridLocation(CurrentEffectIndex), false);
	UpdateStatusText();

	for (const float Delay : {0.25f, 1.0f})
	{
		FTimerHandle DiagnosticTimer;
		GetWorldTimerManager().SetTimer(
			DiagnosticTimer,
			FTimerDelegate::CreateUObject(this, &AMassBattleArmyVFXDemoActor::LogAcceptanceDiagnosticsAt, Delay),
			Delay,
			false);
	}
}

void AMassBattleArmyVFXDemoActor::NextEffect()
{
	CurrentEffectIndex = (CurrentEffectIndex + 1) % ArmyVFXDemoCount;
	TriggerCurrentEffect();
}

void AMassBattleArmyVFXDemoActor::PreviousEffect()
{
	CurrentEffectIndex = (CurrentEffectIndex + ArmyVFXDemoCount - 1) % ArmyVFXDemoCount;
	TriggerCurrentEffect();
}

void AMassBattleArmyVFXDemoActor::TriggerAllInternal(bool bWarmup)
{
	for (int32 Index = 0; Index < ArmyVFXDemoCount; ++Index)
	{
		const FVector Location = bWarmup
			? GetGridLocation(Index) + FVector(0.0, 0.0, -50000.0)
			: GetGridLocation(Index);
		TriggerSpec(Index, Location, bWarmup);
	}
}

void AMassBattleArmyVFXDemoActor::TriggerSpec(int32 SpecIndex, const FVector& WorldLocation, bool bWarmup)
{
	if (!ensure(GArmyVFXDemoSpecs[SpecIndex].StepCount > 0))
	{
		return;
	}

	const FArmyVFXDemoSpec& Spec = GArmyVFXDemoSpecs[SpecIndex];
	for (int32 StepIndex = 0; StepIndex < Spec.StepCount; ++StepIndex)
	{
		const FArmyVFXRecipeStep& Step = Spec.Steps[StepIndex];
		const FEntityHandle Host = SpawnRecipeStep(
			SpecIndex,
			Step.bAttached,
			Step.Style,
			Step.Delay,
			Step.LifeSpan,
			WorldLocation,
			bWarmup);

		if (!bWarmup && Step.bAttached && Spec.MovementSpeed > 0.0f && Host.IsSet())
		{
			FMovingBatchFx& Moving = MovingEffects.AddDefaulted_GetRef();
			Moving.Host = Host;
			Moving.Center = WorldLocation;
			Moving.Radius = Spec.MovementSpeed >= 5000.0f ? 350.0f : 300.0f;
			Moving.LinearSpeed = Spec.MovementSpeed;
			Moving.PhaseOffset = static_cast<float>(SpecIndex) * 0.73f;
		}
	}
}

FEntityHandle AMassBattleArmyVFXDemoActor::SpawnRecipeStep(
	int32 SpecIndex,
	bool bAttached,
	int32 StyleIndex,
	float Delay,
	float LifeSpan,
	const FVector& WorldLocation,
	bool bWarmup)
{
	const FArmyVFXDemoSpec& Spec = GArmyVFXDemoSpecs[SpecIndex];
	UMassBattleSubsystem* MassBattleSubsystem = UMassBattleSubsystem::GetPtr(this);
	if (!MassBattleSubsystem || !MassBattleSubsystem->FxRenderers.Contains(Spec.SubType))
	{
		UE_LOG(LogMassBattleEditorMCP, Error,
			TEXT("[ArmyVFXFaithfulBatch] Missing placed renderer: index=%d name=%s subtype=%d class=%s."),
			SpecIndex + 1, Spec.Name, Spec.SubType, Spec.RendererClass);
		return FEntityHandle();
	}

	FFxConfig Config;
	Config.bEnable = true;
	Config.SubType = UMassBattleTagHelpers::SubTypeIndexToEnum(Spec.SubType);
	Config.StyleType = static_cast<EEStyleType>(FMath::Clamp(StyleIndex, 0, 31));
	Config.SoftNiagaraAsset.Reset();
	Config.SoftCascadeAsset.Reset();
	Config.NiagaraAsset = nullptr;
	Config.CascadeAsset = nullptr;
	Config.Transform = FTransform3f::Identity;
	Config.Transform.SetScale3D(FVector3f(Spec.Scale * PreviewScaleMultiplier));
	Config.bAttached = bAttached;
	Config.Quantity = 1;
	Config.Delay = bWarmup ? 0.0f : Delay;
	Config.LifeSpan = bWarmup ? 1.0f : LifeSpan;
	Config.bDespawnWhenNoParent = false;

	const FTransform SpawnTransform(FRotator::ZeroRotator, WorldLocation, FVector::OneVector);
	const FEntityHandle Host = UMassBattleFuncLib::SpawnBatchedFx(this, Config, SpawnTransform);
	if (!Host.IsSet())
	{
		UE_LOG(LogMassBattleEditorMCP, Error,
			TEXT("[ArmyVFXFaithfulBatch] SpawnBatchedFx returned an invalid handle: index=%d name=%s subtype=%d channel=%s style=%d."),
			SpecIndex + 1, Spec.Name, Spec.SubType, bAttached ? TEXT("Attached") : TEXT("Burst"), StyleIndex);
		return Host;
	}

	(bWarmup ? ActiveWarmupHosts : ActiveVisibleHosts).Add(Host);
	return Host;
}

void AMassBattleArmyVFXDemoActor::CleanupVisibleEffects()
{
	CleanupHosts(ActiveVisibleHosts);
	MovingEffects.Reset();
}

void AMassBattleArmyVFXDemoActor::CleanupHosts(TArray<FEntityHandle>& Hosts)
{
	for (const FEntityHandle& Host : Hosts)
	{
		if (Host.IsSet())
		{
			UMassBattleFuncLib::DestroyBatchedFx(this, Host);
		}
	}
	Hosts.Reset();
}

void AMassBattleArmyVFXDemoActor::UpdateMovingEffects(float DeltaSeconds)
{
	if (MovingEffects.IsEmpty())
	{
		return;
	}

	MovementTime += DeltaSeconds;
	UMassAPISubsystem* MassAPI = UMassAPISubsystem::GetPtr(this);
	if (!MassAPI)
	{
		return;
	}

	for (const FMovingBatchFx& Moving : MovingEffects)
	{
		if (!MassAPI->IsValid(Moving.Host) || !MassAPI->HasFragment<FFxConfig_Final>(Moving.Host))
		{
			continue;
		}

		const FFxConfig_Final& HostConfig = MassAPI->GetFragment<FFxConfig_Final>(Moving.Host);
		if (!HostConfig.bSpawned || !MassAPI->IsValid(HostConfig.PairedFxEntity))
		{
			continue;
		}

		const float AngularSpeed = Moving.LinearSpeed / FMath::Max(Moving.Radius, 1.0f);
		const float Angle = Moving.PhaseOffset + MovementTime * AngularSpeed;
		const FVector Location = Moving.Center + FVector(
			Moving.Radius * FMath::Cos(Angle),
			Moving.Radius * FMath::Sin(Angle),
			0.0f);
		const FVector Tangent(-FMath::Sin(Angle), FMath::Cos(Angle), 0.0f);
		UMassBattleFuncLib::SetAgentLocation(this, HostConfig.PairedFxEntity, Location);
		UMassBattleFuncLib::SetAgentRotation(this, HostConfig.PairedFxEntity, Tangent.Rotation());
	}
}

FVector AMassBattleArmyVFXDemoActor::GetGridLocation(int32 SpecIndex) const
{
	return GetActorTransform().TransformPosition(
		GridLocalLocation(SpecIndex, ColumnSpacing, RowSpacing));
}

void AMassBattleArmyVFXDemoActor::UpdateStatusText()
{
	if (!StatusText)
	{
		return;
	}

	const int32 SafeIndex = FMath::Clamp(CurrentEffectIndex, 0, ArmyVFXDemoCount - 1);
	StatusText->SetText(FText::FromString(FString::Printf(
		TEXT("Space: replay all | A/D: select | R: replay selected | V: validate | selected %02d/%02d %s"),
		SafeIndex + 1,
		ArmyVFXDemoCount,
		GArmyVFXDemoSpecs[SafeIndex].Name)));
}

void AMassBattleArmyVFXDemoActor::LogAcceptanceDiagnostics()
{
	LogAcceptanceDiagnosticsAt(-1.0f);
}

void AMassBattleArmyVFXDemoActor::LogAcceptanceDiagnosticsAt(float SecondsAfterTrigger)
{
	UMassBattleSubsystem* MassBattleSubsystem = UMassBattleSubsystem::GetPtr(this);
	if (!MassBattleSubsystem)
	{
		UE_LOG(LogMassBattleEditorMCP, Error,
			TEXT("[ArmyVFXFaithfulBatch][Summary] t=%.2f MassBattleSubsystem unavailable."),
			SecondsAfterTrigger);
		return;
	}

	int32 RegisteredCount = 0;
	int32 MatchingAssetCount = 0;
	int32 EffectsWithBatches = 0;
	int32 EffectsWithComponents = 0;
	int32 TotalComponents = 0;
	int32 TotalActiveComponents = 0;
	int32 TotalParticles = 0;
	int32 MissingRendererCount = 0;
	int32 MismatchedAssetCount = 0;

	for (int32 Index = 0; Index < ArmyVFXDemoCount; ++Index)
	{
		const FArmyVFXDemoSpec& Spec = GArmyVFXDemoSpecs[Index];
		AMassBattleFxRenderer* Renderer = MassBattleSubsystem->FxRenderers.FindRef(Spec.SubType);
		if (!IsValid(Renderer))
		{
			++MissingRendererCount;
			UE_LOG(LogMassBattleEditorMCP, Error,
				TEXT("[ArmyVFXFaithfulBatch][Effect] index=%d name=%s subtype=%d renderer=missing expected_class=%s."),
				Index + 1, Spec.Name, Spec.SubType, Spec.RendererClass);
			continue;
		}
		++RegisteredCount;

		const FString ActualAsset = IsValid(Renderer->NiagaraSystemAsset)
			? Renderer->NiagaraSystemAsset->GetPathName()
			: FString();
		const bool bAssetMatches = ActualAsset == Spec.TargetSystem;
		MatchingAssetCount += bAssetMatches ? 1 : 0;
		MismatchedAssetCount += bAssetMatches ? 0 : 1;

		int32 ValidComponents = 0;
		int32 ActiveComponents = 0;
		int32 ActiveParticles = 0;
		int32 AttachedSlots = 0;
		int32 VisibleAttachedSlots = 0;
		int32 PendingBurstEvents = 0;
		for (const TPair<int32, FFxRenderBatchData>& BatchPair : Renderer->SpawnedRenderBatches)
		{
			const FFxRenderBatchData& Batch = BatchPair.Value;
			AttachedSlots += Batch.LocationArray_Attached.Num();
			PendingBurstEvents += Batch.LocationArray_Burst.Num();
			for (int32 SlotIndex = 0; SlotIndex < Batch.IsHiddenArray_Attached.Num(); ++SlotIndex)
			{
				VisibleAttachedSlots += Batch.IsHiddenArray_Attached[SlotIndex] ? 0 : 1;
			}

			UNiagaraComponent* NiagaraComponent = Batch.SpawnedNiagaraSystem;
			if (!IsValid(NiagaraComponent))
			{
				continue;
			}
			++ValidComponents;
			ActiveComponents += NiagaraComponent->IsActive() ? 1 : 0;

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

		EffectsWithBatches += Renderer->SpawnedRenderBatches.IsEmpty() ? 0 : 1;
		EffectsWithComponents += ValidComponents > 0 ? 1 : 0;
		TotalComponents += ValidComponents;
		TotalActiveComponents += ActiveComponents;
		TotalParticles += ActiveParticles;

		UE_LOG(LogMassBattleEditorMCP, Display,
			TEXT("[ArmyVFXFaithfulBatch][Effect] t=%.2f index=%d name=%s subtype=%d asset_match=%s batches=%d components=%d active_components=%d cpu_particles=%d attached_slots=%d visible_attached_slots=%d pending_burst=%d target=%s actual=%s."),
			SecondsAfterTrigger,
			Index + 1,
			Spec.Name,
			Spec.SubType,
			bAssetMatches ? TEXT("true") : TEXT("false"),
			Renderer->SpawnedRenderBatches.Num(),
			ValidComponents,
			ActiveComponents,
			ActiveParticles,
			AttachedSlots,
			VisibleAttachedSlots,
			PendingBurstEvents,
			Spec.TargetSystem,
			*ActualAsset);
	}

	const bool bPass =
		RegisteredCount == ArmyVFXDemoCount
		&& MatchingAssetCount == ArmyVFXDemoCount
		&& EffectsWithBatches == ArmyVFXDemoCount
		&& EffectsWithComponents == ArmyVFXDemoCount;
	const FString Summary = FString::Printf(
		TEXT("[ArmyVFXFaithfulBatch][Summary] t=%.2f pass=%s entries=27 registered=%d asset_matches=%d effects_with_batches=%d effects_with_components=%d components=%d active_components=%d cpu_particles=%d missing_renderers=%d mismatched_assets=%d source_components=0 unbatched_components=0."),
		SecondsAfterTrigger,
		bPass ? TEXT("true") : TEXT("false"),
		RegisteredCount,
		MatchingAssetCount,
		EffectsWithBatches,
		EffectsWithComponents,
		TotalComponents,
		TotalActiveComponents,
		TotalParticles,
		MissingRendererCount,
		MismatchedAssetCount);
	if (bPass)
	{
		UE_LOG(LogMassBattleEditorMCP, Display, TEXT("%s"), *Summary);
	}
	else
	{
		UE_LOG(LogMassBattleEditorMCP, Error, TEXT("%s"), *Summary);
	}
}
