// Copyright (c) 2026 Winyunq. All rights reserved.
#include "MassBattleUnitSource.h"
#include "MassBattleUnitMCPApi.h"
#include "DataAssets/MassBattleAgentConfigDataAsset.h"
#include "Engine/Blueprint.h"
#include "Components/SceneComponent.h"
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "IDetailCustomization.h"
#include "PropertyEditorModule.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"

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
	UnitData = CreateDefaultSubobject<UMassBattleAgentConfigDataAsset>(TEXT("UnitData"));
	LastUpdatedSource = CreateDefaultSubobject<UMassBattleAgentConfigDataAsset>(TEXT("LastUpdatedSource"));
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
	UObject* Asset = GetSourceAsset();
	if (Asset)
	{
		const FString Result = UMassBattleUnitMCPApi::MCP_UnitDelete(Asset->GetPathName(),
			TEXT("{\"mode\":\"hard\",\"dry_run\":false}"));
		UE_LOG(LogMassBattleUnitMCPApi, Display, TEXT("%s"), *Result);
	}
}
