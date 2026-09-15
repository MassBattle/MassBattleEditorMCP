// Copyright (c) 2026 Winyunq. All rights reserved.
#include "MassBattleUnitSource.h"
#include "MassBattleUnitMCPApi.h"
#include "DataAssets/MassBattleAgentConfigDataAsset.h"
#include "Engine/Blueprint.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/ArrowComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/Texture2D.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "IDetailCustomization.h"
#include "PropertyEditorModule.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"
#include "Editor.h"
#include "Misc/MessageDialog.h"
#include "Misc/App.h"

class FMassBattleUnitSourceDetails final : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance() { return MakeShared<FMassBattleUnitSourceDetails>(); }
	virtual void CustomizeDetails(IDetailLayoutBuilder& Builder) override
	{
		TArray<TWeakObjectPtr<UObject>> Objects;
		Builder.GetObjectsBeingCustomized(Objects);
		if (Objects.Num() != 1) return;
		TWeakObjectPtr<AMassBattleUnitSource> Source = Cast<AMassBattleUnitSource>(Objects[0].Get());
		if (!Source.IsValid()) return;
		Builder.EditCategory("Team Color").AddCustomRow(NSLOCTEXT("MassBattleUnitSource", "TeamPreview", "Team Color Preview"))
		.WholeRowContent()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(2)
			[
				SNew(SButton).Text(Source->FindFunction(GET_FUNCTION_NAME_CHECKED(AMassBattleUnitSource, ApplyTeamColor))->GetDisplayNameText())
				.OnClicked_Lambda([Source]() { if (Source.IsValid()) Source->ApplyTeamColor(); return FReply::Handled(); })
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(2)
			[
				SNew(SButton).Text(Source->FindFunction(GET_FUNCTION_NAME_CHECKED(AMassBattleUnitSource, ShowOriginalColor))->GetDisplayNameText())
				.OnClicked_Lambda([Source]() { if (Source.IsValid()) Source->ShowOriginalColor(); return FReply::Handled(); })
			]
		];
		Builder.EditCategory("Export").AddCustomRow(NSLOCTEXT("MassBattleUnitSource", "Actions", "Update / Destroy"))
		.WholeRowContent()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(2)
			[
				SNew(SButton).Text(Source->FindFunction(GET_FUNCTION_NAME_CHECKED(AMassBattleUnitSource, Update))->GetDisplayNameText())
				.OnClicked_Lambda([Source]() { if (Source.IsValid()) Source->Update(); return FReply::Handled(); })
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(2)
			[
				SNew(SButton).Text(Source->FindFunction(GET_FUNCTION_NAME_CHECKED(AMassBattleUnitSource, DestroySource))->GetDisplayNameText())
				.OnClicked_Lambda([Source]() { if (Source.IsValid()) Source->DestroySource(); return FReply::Handled(); })
			]
		];
	}
};

void AMassBattleUnitSource::RegisterDetails(bool bRegister)
{
	FPropertyEditorModule* Module = bRegister ? &FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor")
		: FModuleManager::GetModulePtr<FPropertyEditorModule>("PropertyEditor");
	if (!Module) return;
	if (bRegister) Module->RegisterCustomClassLayout(StaticClass()->GetFName(),
		FOnGetDetailCustomizationInstance::CreateStatic(&FMassBattleUnitSourceDetails::MakeInstance));
	else Module->UnregisterCustomClassLayout(StaticClass()->GetFName());
}

AMassBattleUnitSource::AMassBattleUnitSource()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Body = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
	Body->SetupAttachment(RootComponent);
	TurretPivot = CreateDefaultSubobject<UArrowComponent>(TEXT("TurretPivot"));
	TurretPivot->SetupAttachment(RootComponent);
	TurretPivot->ComponentTags.Add(TEXT("MBST_TurretYaw"));
	TurretPivot->ArrowColor = FColor::Blue;
	Turret = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Turret"));
	Turret->SetupAttachment(TurretPivot);
	BarrelPivot = CreateDefaultSubobject<UArrowComponent>(TEXT("BarrelPivot"));
	BarrelPivot->SetupAttachment(TurretPivot);
	BarrelPivot->ComponentTags.Add(TEXT("MBST_BarrelPitch"));
	BarrelPivot->ArrowColor = FColor::Green;
	Barrel = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Barrel"));
	Barrel->SetupAttachment(BarrelPivot);
	Muzzle = CreateDefaultSubobject<UArrowComponent>(TEXT("Muzzle"));
	Muzzle->SetupAttachment(BarrelPivot);
	Muzzle->ComponentTags.Add(TEXT("MBST_Muzzle"));
	Muzzle->ArrowColor = FColor::Red;
	UnitData = CreateDefaultSubobject<UMassBattleAgentConfigDataAsset>(TEXT("UnitData"));
	LastUpdatedSource = CreateDefaultSubobject<UMassBattleAgentConfigDataAsset>(TEXT("LastUpdatedSource"));
}

void AMassBattleUnitSource::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	ApplyPreviewMaterial();
}

void AMassBattleUnitSource::ApplyPreviewMaterial()
{
	if (!TeamMaterial) return;
	UMaterialInstanceDynamic* Preview = UMaterialInstanceDynamic::Create(TeamMaterial, this);
	Preview->SetFlags(RF_Transient);
	Preview->SetScalarParameterValue(TEXT("PreviewTeamIndex"), PreviewTeamIndex);
	Preview->SetScalarParameterValue(TEXT("TeamTintStrength"), bPreviewTeamColor ? TeamTintStrength : 0.0f);
	if (TeamMask) Preview->SetTextureParameterValue(TEXT("TeamMask"), TeamMask);
	Body->SetMaterial(0, Preview);
	Turret->SetMaterial(0, Preview);
	Barrel->SetMaterial(0, Preview);
}

void AMassBattleUnitSource::RefreshSourcePreview()
{
	if (IsTemplate())
	{
		if (UBlueprint* Blueprint = Cast<UBlueprint>(GetSourceAsset()))
		{
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
		}
	}
	else ApplyPreviewMaterial();
}

void AMassBattleUnitSource::ApplyTeamColor()
{
	Modify();
	bPreviewTeamColor = true;
	RefreshSourcePreview();
}

void AMassBattleUnitSource::ShowOriginalColor()
{
	Modify();
	bPreviewTeamColor = false;
	RefreshSourcePreview();
}

AMassBattleUnitSource* AMassBattleUnitSource::Resolve(UObject* Object)
{
	if (const UBlueprint* Blueprint = Cast<UBlueprint>(Object))
	{
		Object = Blueprint->GeneratedClass;
	}
	if (UClass* Class = Cast<UClass>(Object))
	{
		Object = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject() : nullptr;
	}
	return Cast<AMassBattleUnitSource>(Object);
}

UObject* AMassBattleUnitSource::GetSourceAsset() const
{
	return GetClass()->ClassGeneratedBy;
}

void AMassBattleUnitSource::Update()
{
	LastReport = UMassBattleUnitMCPApi::UpdateSource(this, true);
}

void AMassBattleUnitSource::DestroySource()
{
	AMassBattleUnitSource* Defaults = Resolve(GetSourceAsset());
	if (!Defaults || !Defaults->ExportedUnit || !ConfirmDestroyUnit()) return;
	LastReport = UMassBattleUnitMCPApi::DestroySourceUnit(Defaults);
	UE_LOG(LogMassBattleUnitMCPApi, Display, TEXT("%s"), *LastReport);
}

void AMassBattleUnitSource::PostInitProperties()
{
	Super::PostInitProperties();
	if (HasAnyFlags(RF_ClassDefaultObject))
		FEditorDelegates::OnAddExtraObjectsToDelete.AddUObject(this, &AMassBattleUnitSource::AddAssociatedUnitToDeletion);
}

void AMassBattleUnitSource::BeginDestroy()
{
	FEditorDelegates::OnAddExtraObjectsToDelete.RemoveAll(this);
	Super::BeginDestroy();
}

bool AMassBattleUnitSource::ConfirmDestroyUnit() const
{
	if (IsRunningCommandlet() || FApp::IsUnattended()) return false;
	return FMessageDialog::Open(EAppMsgType::YesNo, EAppReturnType::No,
		NSLOCTEXT("MassBattleUnitSource", "DestroyUnitConfirmation",
			"Destroying this unit and generating it again later may lose some data. Are you sure you want to destroy the unit? (Deleting this source asset can also show this prompt.)")) == EAppReturnType::Yes;
}

void AMassBattleUnitSource::AddAssociatedUnitToDeletion(const TArray<UObject*>& Objects, TSet<UObject*>& ExtraObjects)
{
	UObject* Asset = GetSourceAsset();
	if (Resolve(Asset) != this || !ExportedUnit || !Objects.Contains(Asset)
		|| Objects.Contains(ExportedUnit) || ExtraObjects.Contains(ExportedUnit)) return;
	if (ConfirmDestroyUnit()) ExtraObjects.Add(ExportedUnit);
}
