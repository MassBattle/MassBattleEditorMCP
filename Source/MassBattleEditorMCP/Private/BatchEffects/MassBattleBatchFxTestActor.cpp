#include "BatchEffects/MassBattleBatchFxTestActor.h"

#include "Camera/CameraComponent.h"
#include "Components/SceneComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "FuncLibs/MassBattleFuncLib.h"
#include "FuncLibs/MassBattleTagHelpers.h"
#include "Fragments/FxHostConfig.h"
#include "Fragments/Chase.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "MassAPISubsystem.h"
#include "MassBattleEditorMCP.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitterInstance.h"
#include "NiagaraSystemInstance.h"
#include "Fragments/Attack.h"
#include "Fragments/Trace.h"
#include "Renderers/MassBattleFxRenderer.h"
#include "Subsystems/MassBattleSubsystem.h"
#include "Tasks/MassBattleBPTaskAgentsChaseAttack.h"
#include "TimerManager.h"

namespace
{
	const FName KatyushaAcceptanceTag(TEXT("KatyushaBatchAcceptance"));
	const FName KatyushaAcceptanceIssuedTag(TEXT("KatyushaBatchAcceptanceIssued"));
	const FName InstantAttackAcceptanceTag(TEXT("InstantAttackBatchAcceptance"));
	const FName InstantAttackAcceptanceIssuedTag(TEXT("InstantAttackBatchAcceptanceIssued"));
	constexpr int32 KatyushaAttackerTeam = 0;
	constexpr int32 KatyushaTargetTeam = 1;
	constexpr int32 KatyushaExpectedAttackers = 32;
	constexpr int32 KatyushaRocketsPerAttacker = 16;
	constexpr float KatyushaFormationSearchRadius = 800.0f;
	const FVector KatyushaAttackerCenter(-700.0, 0.0, 20.0);
	const FVector KatyushaTargetCenter(700.0, 0.0, 20.0);
	constexpr int32 InstantAttackAttackerTeam = 0;
	constexpr int32 InstantAttackTargetTeam = 1;
	constexpr int32 InstantAttackExpectedAttackers = 64;
	constexpr float InstantAttackFormationSearchRadius = 1000.0f;
	constexpr float AcceptanceDiagnosticsSearchRadius = 10000.0f;
	const FVector InstantAttackAttackerCenter(-700.0, 0.0, 20.0);
	const FVector InstantAttackTargetCenter(700.0, 0.0, 20.0);

	struct FKatyushaRendererPeak
	{
		int32 Batches = 0;
		int32 ValidComponents = 0;
		int32 ActiveComponents = 0;
		int32 ActiveParticles = 0;
		int32 AttachedSlots = 0;
		int32 VisibleAttachedSlots = 0;
		int32 PendingBurstEvents = 0;
		FString LastSignature;
	};

	TMap<FString, FKatyushaRendererPeak> KatyushaRendererPeaks;

	TArray<FTraceResult> FindAcceptanceAgents(
		const UObject* WorldContext,
		const FVector& Center,
		const int32 Team,
		const float SearchRadius)
	{
		bool bHit = false;
		TArray<FTraceResult> TraceResults;
		UMassBattleFuncLib::SphereTraceForAgents(
			WorldContext,
			bHit,
			TraceResults,
			-1,
			Center,
			SearchRadius);

		TArray<FTraceResult> TeamResults;
		TSet<FEntityHandle> UniqueEntities;
		if (!bHit)
		{
			return TeamResults;
		}

		for (const FTraceResult& Result : TraceResults)
		{
			if (!Result.Entity.IsSet()
				|| UniqueEntities.Contains(Result.Entity)
				|| !UMassBattleTagHelpers::HasEntityTeamTagByIndex(WorldContext, Team, Result.Entity))
			{
				continue;
			}
			UniqueEntities.Add(Result.Entity);
			TeamResults.Add(Result);
		}
		return TeamResults;
	}

	void LogAcceptanceAttackState(
		AMassBattleBatchFxTestActor* TestActor,
		const TCHAR* Prefix,
		const FVector& Center,
		const int32 Team,
		const float SearchRadius)
	{
		UMassAPISubsystem* MassAPI = UMassAPISubsystem::GetPtr(TestActor);
		if (!MassAPI)
		{
			return;
		}

		int32 Locked = 0;
		int32 Chasing = 0;
		int32 Attacking = 0;
		int32 Aim = 0;
		int32 PreCast = 0;
		int32 PostCast = 0;
		int32 Cooling = 0;
		int32 ValidTargets = 0;
		const TArray<FTraceResult> Agents =
			FindAcceptanceAgents(TestActor, Center, Team, SearchRadius);
		for (const FTraceResult& Result : Agents)
		{
			const FEntityHandle& Entity = Result.Entity;
			Locked += MassAPI->HasFlag(Entity, "BPTask_ChaseAttack") ? 1 : 0;
			Chasing += MassAPI->HasFlag(Entity, "Chasing") ? 1 : 0;
			Attacking += MassAPI->HasFlag(Entity, "Attacking") ? 1 : 0;

			if (const FTracing* Tracing = MassAPI->GetFragmentPtr<FTracing>(Entity))
			{
				ValidTargets += MassAPI->IsValid(Tracing->TraceResult) ? 1 : 0;
			}
			if (const FAttacking* AttackState = MassAPI->GetFragmentPtr<FAttacking>(Entity))
			{
				switch (AttackState->State)
				{
					case EAttackState::Aim_FirstExec:
					case EAttackState::Aim:
						++Aim;
						break;
					case EAttackState::PreCast_FirstExec:
					case EAttackState::PreCast:
						++PreCast;
						break;
					case EAttackState::PostCast:
						++PostCast;
						break;
					case EAttackState::Cooling:
						++Cooling;
						break;
					default:
						break;
				}
			}
		}

		UE_LOG(
			LogMassBattleEditorMCP,
			Display,
			TEXT("[%s] AgentState{Found=%d Locked=%d Targets=%d Chasing=%d Attacking=%d Aim=%d PreCast=%d PostCast=%d Cooling=%d}"),
			Prefix,
			Agents.Num(),
			Locked,
			ValidTargets,
			Chasing,
			Attacking,
			Aim,
			PreCast,
			PostCast,
			Cooling);
	}

	void LogKatyushaAcceptanceRendererState(AMassBattleBatchFxTestActor* TestActor, const bool bForceSummary)
	{
		if (!IsValid(TestActor))
		{
			return;
		}

		UMassBattleSubsystem* MassBattleSubsystem = UMassBattleSubsystem::GetPtr(TestActor);
		UWorld* World = TestActor->GetWorld();
		if (!MassBattleSubsystem || !World)
		{
			return;
		}
		if (bForceSummary)
		{
			LogAcceptanceAttackState(
				TestActor,
				TEXT("KatyushaBatchAcceptance"),
				FVector::ZeroVector,
				KatyushaAttackerTeam,
				AcceptanceDiagnosticsSearchRadius);
		}

		for (const TPair<const TCHAR*, int32> Entry : {
			TPair<const TCHAR*, int32>(TEXT("LaunchShockwaveBurst"), 77),
			TPair<const TCHAR*, int32>(TEXT("YellowProjectileAttached"), 78),
			TPair<const TCHAR*, int32>(TEXT("ImpactExplosionBurst"), 79)})
		{
			AMassBattleFxRenderer* Renderer = MassBattleSubsystem->FxRenderers.FindRef(Entry.Value);
			if (!IsValid(Renderer))
			{
				if (bForceSummary)
				{
					UE_LOG(LogMassBattleEditorMCP, Error,
						TEXT("[KatyushaBatchAcceptance] %s renderer missing for SubType=%d."),
						Entry.Key,
						Entry.Value);
				}
				continue;
			}

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
				for (const bool bHidden : Batch.IsHiddenArray_Attached)
				{
					VisibleAttachedSlots += bHidden ? 0 : 1;
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

			const FString PeakKey = FString::Printf(TEXT("%p:%d"), World, Entry.Value);
			FKatyushaRendererPeak& Peak = KatyushaRendererPeaks.FindOrAdd(PeakKey);
			Peak.Batches = FMath::Max(Peak.Batches, Renderer->SpawnedRenderBatches.Num());
			Peak.ValidComponents = FMath::Max(Peak.ValidComponents, ValidComponents);
			Peak.ActiveComponents = FMath::Max(Peak.ActiveComponents, ActiveComponents);
			Peak.ActiveParticles = FMath::Max(Peak.ActiveParticles, ActiveParticles);
			Peak.AttachedSlots = FMath::Max(Peak.AttachedSlots, AttachedSlots);
			Peak.VisibleAttachedSlots = FMath::Max(Peak.VisibleAttachedSlots, VisibleAttachedSlots);
			Peak.PendingBurstEvents = FMath::Max(Peak.PendingBurstEvents, PendingBurstEvents);

			const FString Signature = FString::Printf(
				TEXT("%d/%d/%d/%d/%d/%d/%d"),
				Renderer->SpawnedRenderBatches.Num(),
				ValidComponents,
				ActiveComponents,
				ActiveParticles,
				AttachedSlots,
				VisibleAttachedSlots,
				PendingBurstEvents);
			const bool bChanged = Signature != Peak.LastSignature;
			Peak.LastSignature = Signature;

			if (bForceSummary || (bChanged && (ValidComponents > 0 || ActiveParticles > 0 || VisibleAttachedSlots > 0 || PendingBurstEvents > 0)))
			{
				UE_LOG(LogMassBattleEditorMCP, Display,
					TEXT("[KatyushaBatchAcceptance] %s SubType=%d Current{Batches=%d Components=%d Active=%d Particles=%d AttachedSlots=%d VisibleAttached=%d PendingBurst=%d} Peak{Batches=%d Components=%d Active=%d Particles=%d AttachedSlots=%d VisibleAttached=%d PendingBurst=%d}"),
					Entry.Key,
					Entry.Value,
					Renderer->SpawnedRenderBatches.Num(),
					ValidComponents,
					ActiveComponents,
					ActiveParticles,
					AttachedSlots,
					VisibleAttachedSlots,
					PendingBurstEvents,
					Peak.Batches,
					Peak.ValidComponents,
					Peak.ActiveComponents,
					Peak.ActiveParticles,
					Peak.AttachedSlots,
					Peak.VisibleAttachedSlots,
					Peak.PendingBurstEvents);
			}
		}
	}

	bool IssueKatyushaAcceptanceAttack(AMassBattleBatchFxTestActor* TestActor)
	{
		if (!IsValid(TestActor) || TestActor->ActorHasTag(KatyushaAcceptanceIssuedTag))
		{
			return true;
		}

		const TArray<FTraceResult> AttackerResults = FindAcceptanceAgents(
			TestActor,
			KatyushaAttackerCenter,
			KatyushaAttackerTeam,
			KatyushaFormationSearchRadius);
		const TArray<FTraceResult> TargetResults = FindAcceptanceAgents(
			TestActor,
			KatyushaTargetCenter,
			KatyushaTargetTeam,
			KatyushaFormationSearchRadius);

		if (AttackerResults.Num() != KatyushaExpectedAttackers || TargetResults.IsEmpty())
		{
			UE_LOG(LogMassBattleEditorMCP, Display,
				TEXT("[KatyushaBatchAcceptance] Waiting for Mass formations: attackers=%d/%d targets=%d."),
				AttackerResults.Num(),
				KatyushaExpectedAttackers,
				TargetResults.Num());
			return false;
		}

		TArray<FEntityHandle> Attackers;
		Attackers.Reserve(AttackerResults.Num());
		for (const FTraceResult& Result : AttackerResults)
		{
			Attackers.Add(Result.Entity);
		}

		const FTraceResult* ClosestTarget = nullptr;
		float ClosestTargetDistanceSq = TNumericLimits<float>::Max();
		for (const FTraceResult& Result : TargetResults)
		{
			const float DistanceSq = FVector::DistSquared(Result.EntityLocation, KatyushaTargetCenter);
			if (DistanceSq < ClosestTargetDistanceSq)
			{
				ClosestTargetDistanceSq = DistanceSq;
				ClosestTarget = &Result;
			}
		}
		if (!ClosestTarget)
		{
			return false;
		}

		TestActor->AcceptanceAttackTask = UMassBattleBPTaskAgentsChaseAttack::AgentsChaseAttack(
			TestActor,
			Attackers,
			ClosestTarget->Entity,
			false,
			0.0f,
			0.0f,
			FAgentTaskVisualizationConfig());
		if (!TestActor->AcceptanceAttackTask)
		{
			UE_LOG(LogMassBattleEditorMCP, Error, TEXT("[KatyushaBatchAcceptance] Failed to create native ChaseAttack task."));
			return false;
		}

		TestActor->AcceptanceAttackTask->Activate();
		TestActor->Tags.AddUnique(KatyushaAcceptanceIssuedTag);
		LogAcceptanceAttackState(
			TestActor,
			TEXT("KatyushaBatchAcceptance/AfterActivate"),
			FVector::ZeroVector,
			KatyushaAttackerTeam,
			AcceptanceDiagnosticsSearchRadius);
		UE_LOG(LogMassBattleEditorMCP, Display,
			TEXT("[KatyushaBatchAcceptance] Native ChaseAttack issued: attackers=%d rockets-per-attacker=%d expected-projectile-events-per-volley=%d target-team=%d. No FX was spawned by the harness."),
			Attackers.Num(),
			KatyushaRocketsPerAttacker,
			Attackers.Num() * KatyushaRocketsPerAttacker,
			KatyushaTargetTeam);
		return true;
	}

	void LogInstantAttackAcceptanceRendererState(AMassBattleBatchFxTestActor* TestActor, const bool bForceSummary)
	{
		if (!IsValid(TestActor))
		{
			return;
		}

		UMassBattleSubsystem* MassBattleSubsystem = UMassBattleSubsystem::GetPtr(TestActor);
		UWorld* World = TestActor->GetWorld();
		if (!MassBattleSubsystem || !World)
		{
			return;
		}
		if (bForceSummary)
		{
			LogAcceptanceAttackState(
				TestActor,
				TEXT("InstantAttackBatchAcceptance"),
				FVector::ZeroVector,
				InstantAttackAttackerTeam,
				AcceptanceDiagnosticsSearchRadius);
		}

		for (const TPair<const TCHAR*, int32> Entry : {
			TPair<const TCHAR*, int32>(TEXT("InfantryFlashBurst"), 42),
			TPair<const TCHAR*, int32>(TEXT("InfantryTracerBurst"), 43),
			TPair<const TCHAR*, int32>(TEXT("TankMuzzleRingBurst"), 44)})
		{
			AMassBattleFxRenderer* Renderer = MassBattleSubsystem->FxRenderers.FindRef(Entry.Value);
			if (!IsValid(Renderer))
			{
				if (bForceSummary)
				{
					UE_LOG(LogMassBattleEditorMCP, Error,
						TEXT("[InstantAttackBatchAcceptance] %s renderer missing for SubType=%d."),
						Entry.Key,
						Entry.Value);
				}
				continue;
			}

			int32 ValidComponents = 0;
			int32 ActiveComponents = 0;
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

			const FString PeakKey = FString::Printf(TEXT("%p:Instant:%d"), World, Entry.Value);
			FKatyushaRendererPeak& Peak = KatyushaRendererPeaks.FindOrAdd(PeakKey);
			Peak.Batches = FMath::Max(Peak.Batches, Renderer->SpawnedRenderBatches.Num());
			Peak.ValidComponents = FMath::Max(Peak.ValidComponents, ValidComponents);
			Peak.ActiveComponents = FMath::Max(Peak.ActiveComponents, ActiveComponents);
			Peak.ActiveParticles = FMath::Max(Peak.ActiveParticles, ActiveParticles);
			Peak.PendingBurstEvents = FMath::Max(Peak.PendingBurstEvents, PendingBurstEvents);

			const FString Signature = FString::Printf(
				TEXT("%d/%d/%d/%d/%d"),
				Renderer->SpawnedRenderBatches.Num(),
				ValidComponents,
				ActiveComponents,
				ActiveParticles,
				PendingBurstEvents);
			const bool bChanged = Signature != Peak.LastSignature;
			Peak.LastSignature = Signature;

			if (bForceSummary || (bChanged && (ValidComponents > 0 || ActiveParticles > 0 || PendingBurstEvents > 0)))
			{
				UE_LOG(LogMassBattleEditorMCP, Display,
					TEXT("[InstantAttackBatchAcceptance] %s SubType=%d Current{Batches=%d Components=%d Active=%d Particles=%d PendingBurst=%d} Peak{Batches=%d Components=%d Active=%d Particles=%d PendingBurst=%d}"),
					Entry.Key,
					Entry.Value,
					Renderer->SpawnedRenderBatches.Num(),
					ValidComponents,
					ActiveComponents,
					ActiveParticles,
					PendingBurstEvents,
					Peak.Batches,
					Peak.ValidComponents,
					Peak.ActiveComponents,
					Peak.ActiveParticles,
					Peak.PendingBurstEvents);
			}
		}
	}

	bool IssueInstantAttackAcceptanceAttack(AMassBattleBatchFxTestActor* TestActor)
	{
		if (!IsValid(TestActor) || TestActor->ActorHasTag(InstantAttackAcceptanceIssuedTag))
		{
			return true;
		}

		const TArray<FTraceResult> AttackerResults = FindAcceptanceAgents(
			TestActor,
			InstantAttackAttackerCenter,
			InstantAttackAttackerTeam,
			InstantAttackFormationSearchRadius);
		const TArray<FTraceResult> TargetResults = FindAcceptanceAgents(
			TestActor,
			InstantAttackTargetCenter,
			InstantAttackTargetTeam,
			InstantAttackFormationSearchRadius);

		if (AttackerResults.Num() != InstantAttackExpectedAttackers || TargetResults.IsEmpty())
		{
			UE_LOG(LogMassBattleEditorMCP, Display,
				TEXT("[InstantAttackBatchAcceptance] Waiting for Mass formations: attackers=%d/%d targets=%d."),
				AttackerResults.Num(),
				InstantAttackExpectedAttackers,
				TargetResults.Num());
			return false;
		}

		TArray<FEntityHandle> Attackers;
		Attackers.Reserve(AttackerResults.Num());
		for (const FTraceResult& Result : AttackerResults)
		{
			Attackers.Add(Result.Entity);
		}

		const FTraceResult* ClosestTarget = nullptr;
		float ClosestTargetDistanceSq = TNumericLimits<float>::Max();
		for (const FTraceResult& Result : TargetResults)
		{
			const float DistanceSq = FVector::DistSquared(Result.EntityLocation, InstantAttackTargetCenter);
			if (DistanceSq < ClosestTargetDistanceSq)
			{
				ClosestTargetDistanceSq = DistanceSq;
				ClosestTarget = &Result;
			}
		}
		if (!ClosestTarget)
		{
			return false;
		}

		TestActor->AcceptanceAttackTask = UMassBattleBPTaskAgentsChaseAttack::AgentsChaseAttack(
			TestActor,
			Attackers,
			ClosestTarget->Entity,
			false,
			0.0f,
			0.0f,
			FAgentTaskVisualizationConfig());
		if (!TestActor->AcceptanceAttackTask)
		{
			UE_LOG(LogMassBattleEditorMCP, Error, TEXT("[InstantAttackBatchAcceptance] Failed to create native ChaseAttack task."));
			return false;
		}

		TestActor->AcceptanceAttackTask->Activate();
		TestActor->Tags.AddUnique(InstantAttackAcceptanceIssuedTag);
		LogAcceptanceAttackState(
			TestActor,
			TEXT("InstantAttackBatchAcceptance/AfterActivate"),
			FVector::ZeroVector,
			InstantAttackAttackerTeam,
			AcceptanceDiagnosticsSearchRadius);
		UE_LOG(LogMassBattleEditorMCP, Display,
			TEXT("[InstantAttackBatchAcceptance] Native ChaseAttack issued: attackers=%d target-team=%d. The harness spawned no FX; all SubType 42/43/44 events must come from unit attack configuration."),
			Attackers.Num(),
			InstantAttackTargetTeam);
		return true;
	}
}

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

	if (ActorHasTag(InstantAttackAcceptanceTag))
	{
		// This mode only issues the ordinary MassBattle attack task and observes
		// renderer state. It never calls SpawnGrid/SpawnBatchedFx.
		bAutoPlay = false;
		TitleText->SetVisibility(false);
		MuzzleText->SetVisibility(false);
		ExplosionText->SetVisibility(false);
		OverviewCamera->SetRelativeLocation(FVector(-1800.0, 0.0, 700.0));
		OverviewCamera->SetRelativeRotation(FRotator(-18.0, 0.0, 0.0));
		OverviewCamera->FieldOfView = 60.0f;

		if (APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0))
		{
			PlayerController->SetViewTargetWithBlend(this, 0.0f);
		}

		for (const float RetryDelay : {0.5f, 1.5f, 3.0f, 5.0f})
		{
			FTimerHandle RetryHandle;
			GetWorldTimerManager().SetTimer(
				RetryHandle,
				FTimerDelegate::CreateWeakLambda(this, [this]()
				{
					IssueInstantAttackAcceptanceAttack(this);
				}),
				RetryDelay,
				false);
		}

		FTimerHandle ObserverHandle;
		GetWorldTimerManager().SetTimer(
			ObserverHandle,
			FTimerDelegate::CreateWeakLambda(this, [this]()
			{
				LogInstantAttackAcceptanceRendererState(this, false);
			}),
			0.05f,
			true,
			0.05f);

		for (const float SummaryDelay : {4.0f, 8.0f, 12.0f})
		{
			FTimerHandle SummaryHandle;
			GetWorldTimerManager().SetTimer(
				SummaryHandle,
				FTimerDelegate::CreateWeakLambda(this, [this]()
				{
					LogInstantAttackAcceptanceRendererState(this, true);
				}),
				SummaryDelay,
				false);
		}

		UE_LOG(LogMassBattleEditorMCP, Display,
			TEXT("[InstantAttackBatchAcceptance] Harness started. It will issue native ChaseAttack for 64 mixed attackers and observe SubTypes 42/43/44; direct FX spawning is disabled."));
		return;
	}

	if (ActorHasTag(KatyushaAcceptanceTag))
	{
		// Acceptance mode drives the ordinary MassBattle attack task only. It never
		// calls SpawnGrid/SpawnBatchedFx, so every observed FX must originate from
		// the Katyusha unit/projectile configuration under test.
		bAutoPlay = false;
		TitleText->SetVisibility(false);
		MuzzleText->SetVisibility(false);
		ExplosionText->SetVisibility(false);
		OverviewCamera->SetRelativeLocation(FVector(-3200.0, 0.0, 1350.0));
		OverviewCamera->SetRelativeRotation(FRotator(-23.0, 0.0, 0.0));
		OverviewCamera->FieldOfView = 72.0f;

		if (APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0))
		{
			PlayerController->SetViewTargetWithBlend(this, 0.0f);
		}

		for (const float RetryDelay : {0.5f, 1.5f, 3.0f})
		{
			FTimerHandle RetryHandle;
			GetWorldTimerManager().SetTimer(
				RetryHandle,
				FTimerDelegate::CreateWeakLambda(this, [this]()
				{
					IssueKatyushaAcceptanceAttack(this);
				}),
				RetryDelay,
				false);
		}

		FTimerHandle ObserverHandle;
		GetWorldTimerManager().SetTimer(
			ObserverHandle,
			FTimerDelegate::CreateWeakLambda(this, [this]()
			{
				LogKatyushaAcceptanceRendererState(this, false);
			}),
			0.1f,
			true,
			0.1f);

		for (const float SummaryDelay : {4.0f, 8.0f, 12.0f})
		{
			FTimerHandle SummaryHandle;
			GetWorldTimerManager().SetTimer(
				SummaryHandle,
				FTimerDelegate::CreateWeakLambda(this, [this]()
				{
					LogKatyushaAcceptanceRendererState(this, true);
				}),
				SummaryDelay,
				false);
		}

		UE_LOG(LogMassBattleEditorMCP, Display,
			TEXT("[KatyushaBatchAcceptance] Harness started. It will issue native ChaseAttack and observe auto-registered SubTypes 77/78/79; direct FX spawning is disabled."));
		return;
	}

	MuzzleText->SetText(FText::FromString(FString::Printf(TEXT("MUZZLE | SubType %d | Scale %.3f"), MuzzleSubType, MuzzleScale)));
	ExplosionText->SetText(FText::FromString(FString::Printf(TEXT("EXPLOSION | SubType %d | Scale %.3f"), ExplosionSubType, ExplosionScale)));

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
			FMath::Max(LoopInterval, 0.05f),
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
	SpawnGrid(MuzzleSubType, MuzzleCenter, MuzzleScale, 1.5f, TEXT("MuzzleWarmup"));
	SpawnGrid(ExplosionSubType, ExplosionCenter, ExplosionScale, 5.0f, TEXT("ExplosionWarmup"));
	UE_LOG(LogMassBattleEditorMCP, Display, TEXT("[ArmyVFXBatchTest] Renderer warm-up requested; visible auto-play follows after %.2fs."), InitialDelay);
}

void AMassBattleBatchFxTestActor::TriggerMuzzle()
{
	SpawnGrid(MuzzleSubType, MuzzleCenter, MuzzleScale, 1.5f, TEXT("Muzzle"));
}

void AMassBattleBatchFxTestActor::TriggerExplosion()
{
	SpawnGrid(ExplosionSubType, ExplosionCenter, ExplosionScale, 5.0f, TEXT("Explosion"));
}

void AMassBattleBatchFxTestActor::TriggerAll()
{
	TriggerMuzzle();
	TriggerExplosion();

	if (bLogDiagnostics)
	{
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
