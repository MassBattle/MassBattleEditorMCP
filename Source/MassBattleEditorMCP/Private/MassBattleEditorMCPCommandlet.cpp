// Copyright (c) 2025-2026 Winyunq. All rights reserved.
#include "MassBattleEditorMCPCommandlet.h"

#include "Dom/JsonObject.h"
#include "MassBattleProjectileMCPApi.h"
#include "MassBattleUnitEditorMCPApi.h"
#include "MassBattleUnitMCPApi.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
TSharedPtr<FJsonObject> CommandletParseJsonObject(const FString& Json)
{
	TSharedPtr<FJsonObject> Object;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
	{
		return nullptr;
	}
	return Object;
}

FString CommandletToJsonString(const TSharedPtr<FJsonObject>& Object)
{
	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	if (Object.IsValid())
	{
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}
	return Output;
}

FString CommandletMakeErrorJson(const FString& Error)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("success"), false);
	Root->SetStringField(TEXT("error"), Error);
	return CommandletToJsonString(Root);
}

FString CommandletJsonFieldAsString(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
{
	if (!Object.IsValid())
	{
		return FString();
	}

	FString Value;
	if (Object->TryGetStringField(FieldName, Value))
	{
		return Value;
	}

	const TSharedPtr<FJsonObject>* Child = nullptr;
	if (Object->TryGetObjectField(FieldName, Child) && Child && Child->IsValid())
	{
		return CommandletToJsonString(*Child);
	}

	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (Object->TryGetArrayField(FieldName, Array) && Array)
	{
		FString Output;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
		FJsonSerializer::Serialize(*Array, Writer);
		return Output;
	}
	return FString();
}

FString CommandletJsonFieldByNamesAsString(const TSharedPtr<FJsonObject>& Object, const TArray<FString>& FieldNames)
{
	for (const FString& FieldName : FieldNames)
	{
		const FString Value = CommandletJsonFieldAsString(Object, FieldName);
		if (!Value.IsEmpty())
		{
			return Value;
		}
	}
	return FString();
}

bool CommandletBoolFieldByNames(const TSharedPtr<FJsonObject>& Object, const TArray<FString>& FieldNames, bool bDefaultValue)
{
	bool bValue = bDefaultValue;
	if (!Object.IsValid())
	{
		return bValue;
	}

	for (const FString& FieldName : FieldNames)
	{
		if (Object->TryGetBoolField(FieldName, bValue))
		{
			return bValue;
		}
	}
	return bValue;
}

TSharedPtr<FJsonObject> CommandletInvocationParams(const TSharedPtr<FJsonObject>& Invocation)
{
	const TSharedPtr<FJsonObject>* Params = nullptr;
	if (Invocation.IsValid() && Invocation->TryGetObjectField(TEXT("params"), Params) && Params && Params->IsValid())
	{
		return *Params;
	}
	return Invocation;
}

FString CommandletDispatchInvocation(const TSharedPtr<FJsonObject>& Invocation)
{
	if (!Invocation.IsValid())
	{
		return CommandletMakeErrorJson(TEXT("Input file is not valid JSON."));
	}

	FString Command;
	Invocation->TryGetStringField(TEXT("command"), Command);
	Invocation->TryGetStringField(TEXT("tool"), Command);
	if (Command.IsEmpty())
	{
		return CommandletMakeErrorJson(TEXT("Input JSON must contain command or tool."));
	}

	const TSharedPtr<FJsonObject> Params = CommandletInvocationParams(Invocation);
	const bool bSaveAssets = CommandletBoolFieldByNames(Params, { TEXT("save_assets"), TEXT("bSaveAssets") }, true);

	if (Command == TEXT("MCP_EditorGetStatus") || Command == TEXT("editor_get_status"))
	{
		return UMassBattleUnitEditorMCPApi::MCP_EditorGetStatus();
	}
	if (Command == TEXT("MCP_UnitGetApiStatus") || Command == TEXT("unit_get_api_status"))
	{
		return UMassBattleUnitMCPApi::MCP_UnitGetApiStatus();
	}
	if (Command == TEXT("MCP_EditorPlanCreateVatUnit") || Command == TEXT("editor_plan_create_vat_unit"))
	{
		return UMassBattleUnitEditorMCPApi::MCP_EditorPlanCreateVatUnit(CommandletJsonFieldByNamesAsString(Params, { TEXT("SpecJson"), TEXT("spec") }));
	}
	if (Command == TEXT("MCP_EditorValidateCreateVatUnit") || Command == TEXT("editor_validate_create_vat_unit"))
	{
		return UMassBattleUnitEditorMCPApi::MCP_EditorValidateCreateVatUnit(CommandletJsonFieldByNamesAsString(Params, { TEXT("SpecJson"), TEXT("spec") }));
	}
	if (Command == TEXT("MCP_EditorApplyCreateVatUnit") || Command == TEXT("editor_apply_create_vat_unit"))
	{
		return UMassBattleUnitEditorMCPApi::MCP_EditorApplyCreateVatUnit(CommandletJsonFieldByNamesAsString(Params, { TEXT("SpecJson"), TEXT("spec") }), bSaveAssets);
	}
	if (Command == TEXT("MCP_EditorPlanCreateVatUnitFromSelection") || Command == TEXT("editor_plan_create_vat_unit_from_selection"))
	{
		return UMassBattleUnitEditorMCPApi::MCP_EditorPlanCreateVatUnitFromSelection(CommandletJsonFieldByNamesAsString(Params, { TEXT("OptionsJson"), TEXT("options") }));
	}
	if (Command == TEXT("MCP_EditorApplyCreateVatUnitFromSelection") || Command == TEXT("editor_apply_create_vat_unit_from_selection"))
	{
		return UMassBattleUnitEditorMCPApi::MCP_EditorApplyCreateVatUnitFromSelection(CommandletJsonFieldByNamesAsString(Params, { TEXT("OptionsJson"), TEXT("options") }), bSaveAssets);
	}
	if (Command == TEXT("MCP_UnitCreate") || Command == TEXT("unit_create"))
	{
		return UMassBattleUnitMCPApi::MCP_UnitCreate(CommandletJsonFieldByNamesAsString(Params, { TEXT("CreateSpecJson"), TEXT("create_spec") }), bSaveAssets);
	}
	if (Command == TEXT("MCP_UnitGet") || Command == TEXT("unit_get"))
	{
		FString UnitPath;
		Params->TryGetStringField(TEXT("UnitPath"), UnitPath);
		Params->TryGetStringField(TEXT("unit_path"), UnitPath);
		return UMassBattleUnitMCPApi::MCP_UnitGet(UnitPath, CommandletJsonFieldByNamesAsString(Params, { TEXT("OptionsJson"), TEXT("options") }));
	}
	if (Command == TEXT("MCP_ProjectileGetApiStatus") || Command == TEXT("projectile_get_api_status"))
	{
		return UMassBattleProjectileMCPApi::MCP_ProjectileGetApiStatus();
	}
	if (Command == TEXT("MCP_ProjectileList") || Command == TEXT("projectile_list"))
	{
		return UMassBattleProjectileMCPApi::MCP_ProjectileList(CommandletJsonFieldByNamesAsString(Params, { TEXT("OptionsJson"), TEXT("options") }));
	}
	if (Command == TEXT("MCP_ProjectileQuery") || Command == TEXT("projectile_query"))
	{
		return UMassBattleProjectileMCPApi::MCP_ProjectileQuery(CommandletJsonFieldByNamesAsString(Params, { TEXT("QueryJson"), TEXT("query") }));
	}
	if (Command == TEXT("MCP_ProjectileGet") || Command == TEXT("projectile_get"))
	{
		FString ProjectilePath;
		Params->TryGetStringField(TEXT("ProjectilePath"), ProjectilePath);
		Params->TryGetStringField(TEXT("projectile_path"), ProjectilePath);
		return UMassBattleProjectileMCPApi::MCP_ProjectileGet(ProjectilePath, CommandletJsonFieldByNamesAsString(Params, { TEXT("OptionsJson"), TEXT("options") }));
	}
	if (Command == TEXT("MCP_ProjectileGetSchema") || Command == TEXT("projectile_get_schema"))
	{
		return UMassBattleProjectileMCPApi::MCP_ProjectileGetSchema(CommandletJsonFieldByNamesAsString(Params, { TEXT("OptionsJson"), TEXT("options") }));
	}
	if (Command == TEXT("MCP_ProjectileCreate") || Command == TEXT("projectile_create"))
	{
		return UMassBattleProjectileMCPApi::MCP_ProjectileCreate(CommandletJsonFieldByNamesAsString(Params, { TEXT("CreateSpecJson"), TEXT("create_spec") }), bSaveAssets);
	}
	if (Command == TEXT("MCP_ProjectileWrite") || Command == TEXT("projectile_write"))
	{
		FString ProjectilePath;
		Params->TryGetStringField(TEXT("ProjectilePath"), ProjectilePath);
		Params->TryGetStringField(TEXT("projectile_path"), ProjectilePath);
		return UMassBattleProjectileMCPApi::MCP_ProjectileWrite(ProjectilePath, CommandletJsonFieldByNamesAsString(Params, { TEXT("PatchJson"), TEXT("patch") }), bSaveAssets);
	}
	if (Command == TEXT("MCP_ProjectileValidate") || Command == TEXT("projectile_validate"))
	{
		FString ProjectilePath;
		Params->TryGetStringField(TEXT("ProjectilePath"), ProjectilePath);
		Params->TryGetStringField(TEXT("projectile_path"), ProjectilePath);
		return UMassBattleProjectileMCPApi::MCP_ProjectileValidate(ProjectilePath, CommandletJsonFieldByNamesAsString(Params, { TEXT("OptionsJson"), TEXT("options") }));
	}
	if (Command == TEXT("MCP_ProjectileDelete") || Command == TEXT("projectile_delete"))
	{
		FString ProjectilePath;
		Params->TryGetStringField(TEXT("ProjectilePath"), ProjectilePath);
		Params->TryGetStringField(TEXT("projectile_path"), ProjectilePath);
		return UMassBattleProjectileMCPApi::MCP_ProjectileDelete(ProjectilePath, CommandletJsonFieldByNamesAsString(Params, { TEXT("OptionsJson"), TEXT("options") }));
	}

	return CommandletMakeErrorJson(FString::Printf(TEXT("Unsupported MassBattleEditorMCP commandlet command: %s"), *Command));
}
}

UMassBattleEditorMCPCommandlet::UMassBattleEditorMCPCommandlet()
{
	IsClient = false;
	IsEditor = true;
	LogToConsole = true;
}

int32 UMassBattleEditorMCPCommandlet::Main(const FString& Params)
{
	FString InputFile;
	FString OutputFile;
	FParse::Value(*Params, TEXT("InputFile="), InputFile);
	FParse::Value(*Params, TEXT("OutputFile="), OutputFile);

	if (InputFile.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("MassBattleEditorMCPCommandlet requires -InputFile=<json>."));
		return 1;
	}

	FString InputJson;
	if (!FFileHelper::LoadFileToString(InputJson, *InputFile))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to read input file: %s"), *InputFile);
		return 1;
	}

	const FString ResultJson = CommandletDispatchInvocation(CommandletParseJsonObject(InputJson));
	if (!OutputFile.IsEmpty())
	{
		if (!FFileHelper::SaveStringToFile(ResultJson, *OutputFile))
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to write output file: %s"), *OutputFile);
			return 1;
		}
	}
	else
	{
		UE_LOG(LogTemp, Display, TEXT("%s"), *ResultJson);
	}

	TSharedPtr<FJsonObject> Result = CommandletParseJsonObject(ResultJson);
	return Result.IsValid() && CommandletBoolFieldByNames(Result, { TEXT("success") }, false) ? 0 : 2;
}
