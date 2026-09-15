#include "MassBattleUnitSource.h"
#include "MassBattleUnitMCPApi.h"
#include "DataAssets/MassBattleAgentConfigDataAsset.h"
#include "Misc/AutomationTest.h"
#include "Misc/CoreDelegates.h"
#include "Misc/ScopeExit.h"
#include "Editor.h"
#include "Internationalization/Text.h"
#include "CoreGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMassBattleSourceDestroyTest, "MassBattle.MCP.SourceDestroy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMassBattleSourceDestroyTest::RunTest(const FString& Parameters)
{
	const FString Path = TEXT("/Game/__MCPDestroyTest/BP_Source");
	const FString OutputPath = TEXT("/Game/__MCPDestroyTest/DA_Unit.DA_Unit");
	if (LoadObject<UObject>(nullptr, *Path))
	{
		AddError(TEXT("Test fixture already exists; refusing to replace it."));
		return false;
	}
	UMassBattleUnitMCPApi::MCP_UnitCreate(TEXT("{\"asset_type\":\"source_actor\",\"unit_path\":\"/Game/__MCPDestroyTest/BP_Source\",\"export_path\":\"/Game/__MCPDestroyTest/DA_Unit\"}"), true);
	AMassBattleUnitSource* Source = AMassBattleUnitSource::Resolve(LoadObject<UObject>(nullptr, *Path));
	if (!TestNotNull(TEXT("Source created"), Source)) return false;
	UMassBattleUnitMCPApi::UpdateSource(Source, true);
	if (!TestNotNull(TEXT("Unit exported"), Source->ExportedUnit.Get())) return false;
	auto PreviousDialog = FCoreDelegates::ModalMessageDialog;
	ON_SCOPE_EXIT { FCoreDelegates::ModalMessageDialog = PreviousDialog; };
	EAppReturnType::Type Answer = EAppReturnType::No;
	int32 Prompts = 0;
	FCoreDelegates::ModalMessageDialog.BindLambda([&](EAppMsgCategory, EAppMsgType::Type, const FText& Message, const FText&)
	{
		if (Message.BuildSourceString().StartsWith(TEXT("Destroying this unit"))) { ++Prompts; return Answer; }
		return EAppReturnType::No;
	});
	TGuardValue<bool> InteractiveDialogTest(GIsAutomationTesting, false);
	Source->DestroySource();
	TestEqual(TEXT("Direct Destroy asks once"), Prompts, 1);
	TestNotNull(TEXT("No preserves output"), Source->ExportedUnit.Get());
	Answer = EAppReturnType::Yes;
	Source->DestroySource();
	TestNull(TEXT("Yes destroys only output"), Source->ExportedUnit.Get());
	TestNotNull(TEXT("Source survives"), LoadObject<UObject>(nullptr, *Path));
	TestNull(TEXT("Output asset deleted"), LoadObject<UObject>(nullptr, *OutputPath));
	UMassBattleUnitMCPApi::UpdateSource(Source, true);
	TestNotNull(TEXT("Can regenerate"), Source->ExportedUnit.Get());
	const TArray<UObject*> Targets = { Source->GetSourceAsset() };
	TSet<UObject*> Extra;
	Answer = EAppReturnType::No;
	FEditorDelegates::OnAddExtraObjectsToDelete.Broadcast(Targets, Extra);
	TestTrue(TEXT("No excludes associated unit"), Extra.IsEmpty());
	Answer = EAppReturnType::Yes;
	FEditorDelegates::OnAddExtraObjectsToDelete.Broadcast(Targets, Extra);
	TestTrue(TEXT("Yes joins unit to UE deletion batch"), Extra.Contains(Source->ExportedUnit));
	TestNotNull(TEXT("Adding to deletion batch does not delete early"), LoadObject<UObject>(nullptr, *OutputPath));
	Extra.Reset();
	UMassBattleUnitMCPApi::MCP_UnitDelete(Path, TEXT("{\"mode\":\"hard\",\"dry_run\":false,\"delete_exported_unit\":true}"));
	TestNull(TEXT("Confirmed batch removes source"), LoadObject<UObject>(nullptr, *Path));
	TestNull(TEXT("Confirmed batch removes output"), LoadObject<UObject>(nullptr, *OutputPath));
	return !HasAnyErrors();
}
#endif
