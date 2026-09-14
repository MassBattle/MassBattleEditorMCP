// Copyright (c) 2026 Winyunq. All rights reserved.
#include "MassBattleEditorMCP.h"

#include "Editor.h"
#include "EditorUtilitySubsystem.h"
#include "EditorUtilityWidgetBlueprint.h"
#include "Misc/MessageDialog.h"
#include "ToolMenus.h"
#include "MassBattleUnitSource.h"
#include "ISettingsModule.h"

#define LOCTEXT_NAMESPACE "FMassBattleEditorMCPModule"

DEFINE_LOG_CATEGORY(LogMassBattleEditorMCP);

static FDelegateHandle ActorToUnitMenuStartupHandle;

void FMassBattleEditorMCPModule::StartupModule()
{
	if (IsRunningCommandlet())
	{
		return;
	}
	AMassBattleUnitSource::RegisterDetails(true);
	ActorToUnitMenuStartupHandle = UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateLambda([this]()
	{
		FToolMenuOwnerScoped Owner(this);
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
		FToolMenuSection& Section = Menu->FindOrAddSection("MassBattleEditorMCP",
			LOCTEXT("MCPSection", "MassBattle Editor MCP"));
		Section.AddMenuEntry("MassBattleMCPActorToUnit",
			LOCTEXT("ActorToUnit", "Actor to Mass Unit"),
			LOCTEXT("ActorToUnitTooltip", "Open the Actor-to-unit editor UI migrated by Winyunq."),
			FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([]()
			{
				UEditorUtilityWidgetBlueprint* Widget = LoadObject<UEditorUtilityWidgetBlueprint>(nullptr,
					TEXT("/MassBattleEditorMCP/ActorToMassBattleUnitEditor/MassBattleTools.MassBattleTools"));
				UEditorUtilitySubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorUtilitySubsystem>() : nullptr;
				if (!Widget || !Subsystem || !Subsystem->SpawnAndRegisterTab(Widget))
				{
					FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("OpenFailed",
						"Could not open the MassBattle Actor-to-unit editor. See the Output Log for asset or dependency errors."));
				}
			})));
		Section.AddMenuEntry("MassBattleMCPExportRules", LOCTEXT("ExportRules", "Unit Export Rules"),
			LOCTEXT("ExportRulesTooltip", "Shared source Actor export defaults."), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([]()
			{
				FModuleManager::LoadModuleChecked<ISettingsModule>("Settings").ShowViewer(
					"Project", "Plugins", "MassBattleUnitSourceSettings");
			})));
	}));
}

void FMassBattleEditorMCPModule::ShutdownModule()
{
	AMassBattleUnitSource::RegisterDetails(false);
	UToolMenus::UnRegisterStartupCallback(ActorToUnitMenuStartupHandle);
	UToolMenus::UnregisterOwner(this);
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FMassBattleEditorMCPModule, MassBattleEditorMCP)
