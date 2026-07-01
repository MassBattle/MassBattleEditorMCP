// Copyright (c) 2025-2026 Winyunq. All rights reserved.
#include "BatchEffects/MassBattleNiagaraMCPApi.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraParameterStore.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY(LogMassBattleNiagaraMCPApi);

namespace MassBattleNiagaraMCP
{
struct FNiagaraModuleRecord
{
	UNiagaraNodeFunctionCall* Node = nullptr;
	FString Scope;
	FString EmitterName;
	FString ScriptUsage;
	int32 ModuleIndex = INDEX_NONE;
};

struct FResolvedTarget
{
	UObject* Object = nullptr;
	void* StructPtr = nullptr;
	UStruct* StructType = nullptr;
	FString Label;
};

static FString ToJsonString(const TSharedPtr<FJsonObject>& Obj)
{
	FString OutputString;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
	return OutputString;
}

static FString ToJsonString(const TArray<TSharedPtr<FJsonValue>>& Array)
{
	FString OutputString;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Array, Writer);
	return OutputString;
}

static TSharedPtr<FJsonObject> ParseObject(const FString& JsonString)
{
	if (JsonString.TrimStartAndEnd().IsEmpty())
	{
		return MakeShared<FJsonObject>();
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return nullptr;
	}
	return Root;
}

static TSharedPtr<FJsonObject> MakeSuccessObject()
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("success"), true);
	return Root;
}

static FString MakeErrorJson(const FString& ErrorMessage)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("success"), false);
	Root->SetStringField(TEXT("error"), ErrorMessage);
	return ToJsonString(Root);
}

static FString NormalizeObjectPath(FString Path)
{
	Path.TrimStartAndEndInline();
	if (Path.IsEmpty())
	{
		return Path;
	}

	FString Prefix;
	FString Quoted;
	if (Path.Split(TEXT("'"), &Prefix, &Quoted))
	{
		FString Suffix;
		Quoted.Split(TEXT("'"), &Path, &Suffix);
		Path.TrimStartAndEndInline();
	}

	if (!Path.Contains(TEXT(".")) && Path.StartsWith(TEXT("/")))
	{
		const FString AssetName = FPackageName::GetLongPackageAssetName(Path);
		if (!AssetName.IsEmpty())
		{
			Path = FString::Printf(TEXT("%s.%s"), *Path, *AssetName);
		}
	}
	return Path;
}

static UNiagaraSystem* LoadSystem(const FString& SystemPath, FString& OutError)
{
	const FString ObjectPath = NormalizeObjectPath(SystemPath);
	if (ObjectPath.IsEmpty())
	{
		OutError = TEXT("SystemPath is required");
		return nullptr;
	}

	UNiagaraSystem* System = Cast<UNiagaraSystem>(FSoftObjectPath(ObjectPath).TryLoad());
	if (!System)
	{
		System = LoadObject<UNiagaraSystem>(nullptr, *ObjectPath);
	}
	if (!System)
	{
		OutError = FString::Printf(TEXT("Failed to load Niagara system: %s"), *ObjectPath);
	}
	return System;
}

static bool SaveAsset(UObject* Asset, FString& OutError)
{
	if (!Asset)
	{
		OutError = TEXT("Invalid asset");
		return false;
	}

	UPackage* Package = Asset->GetOutermost();
	if (!Package)
	{
		OutError = TEXT("Asset has no package");
		return false;
	}

	Package->MarkPackageDirty();
	const FString PackageFileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (!UPackage::SavePackage(Package, Asset, *PackageFileName, SaveArgs))
	{
		OutError = FString::Printf(TEXT("Failed to save package: %s"), *PackageFileName);
		return false;
	}
	return true;
}

static FString GetSavedExportDir()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MassBattleEditorMCP"), TEXT("NiagaraText"));
}

static FString EnumToStringSafe(const UEnum* Enum, int64 Value)
{
	return Enum ? Enum->GetNameStringByValue(Value) : FString::FromInt(static_cast<int32>(Value));
}

static FString ScriptUsageToString(ENiagaraScriptUsage Usage)
{
	return EnumToStringSafe(StaticEnum<ENiagaraScriptUsage>(), static_cast<int64>(Usage));
}

static FString PinDirectionToString(EEdGraphPinDirection Direction)
{
	switch (Direction)
	{
	case EGPD_Input: return TEXT("input");
	case EGPD_Output: return TEXT("output");
	default: return TEXT("unknown");
	}
}

static FString GetObjectPathString(const UObject* Object)
{
	return Object ? Object->GetPathName() : FString();
}

static TSharedPtr<FJsonObject> PinToJson(const UEdGraphPin* Pin)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	if (!Pin)
	{
		return Obj;
	}

	Obj->SetStringField(TEXT("name"), Pin->PinName.ToString());
	Obj->SetStringField(TEXT("direction"), PinDirectionToString(Pin->Direction));
	Obj->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
	Obj->SetStringField(TEXT("subcategory"), Pin->PinType.PinSubCategory.ToString());
	Obj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
	Obj->SetStringField(TEXT("default_object"), GetObjectPathString(Pin->DefaultObject));
	Obj->SetNumberField(TEXT("linked_to_count"), Pin->LinkedTo.Num());
	return Obj;
}

static TArray<TSharedPtr<FJsonValue>> PinsToJson(const TArray<UEdGraphPin*>& Pins)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const UEdGraphPin* Pin : Pins)
	{
		Values.Add(MakeShared<FJsonValueObject>(PinToJson(Pin)));
	}
	return Values;
}

static TSharedPtr<FJsonObject> ModuleNodeToJson(const FNiagaraModuleRecord& Record, bool bIncludePins)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	UNiagaraNodeFunctionCall* Node = Record.Node;
	if (!Node)
	{
		return Obj;
	}

	Obj->SetNumberField(TEXT("module_index"), Record.ModuleIndex);
	Obj->SetStringField(TEXT("scope"), Record.Scope);
	Obj->SetStringField(TEXT("emitter"), Record.EmitterName);
	Obj->SetStringField(TEXT("script_usage"), Record.ScriptUsage);
	Obj->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	Obj->SetStringField(TEXT("node_name"), Node->GetName());
	Obj->SetStringField(TEXT("node_title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
	Obj->SetStringField(TEXT("function_name"), Node->GetFunctionName());
	Obj->SetStringField(TEXT("function_script"), GetObjectPathString(Node->FunctionScript));
	Obj->SetStringField(TEXT("function_script_asset_object_path"), Node->FunctionScriptAssetObjectPath.ToString());
	Obj->SetStringField(TEXT("signature_name"), Node->Signature.Name.ToString());
	if (bIncludePins)
	{
		Obj->SetArrayField(TEXT("pins"), PinsToJson(Node->Pins));
	}
	return Obj;
}

static void CollectModulesFromGraph(UNiagaraGraph* Graph, const FString& Scope, const FString& EmitterName, const FString& ScriptUsage, TArray<FNiagaraModuleRecord>& OutModules)
{
	if (!Graph)
	{
		return;
	}

	TArray<UNiagaraNodeFunctionCall*> FunctionNodes;
	Graph->GetNodesOfClass(FunctionNodes);
	for (UNiagaraNodeFunctionCall* Node : FunctionNodes)
	{
		FNiagaraModuleRecord Record;
		Record.Node = Node;
		Record.Scope = Scope;
		Record.EmitterName = EmitterName;
		Record.ScriptUsage = ScriptUsage;
		Record.ModuleIndex = OutModules.Num();
		OutModules.Add(Record);
	}
}

static UNiagaraGraph* GetGraphFromScript(UNiagaraScript* Script)
{
	if (!Script)
	{
		return nullptr;
	}
	UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Script->GetLatestSource());
	return Source ? Source->NodeGraph : nullptr;
}

static void CollectModules(UNiagaraSystem* System, TArray<FNiagaraModuleRecord>& OutModules)
{
	if (!System)
	{
		return;
	}

	CollectModulesFromGraph(GetGraphFromScript(System->GetSystemSpawnScript()), TEXT("system"), TEXT(""), TEXT("SystemSpawnScript"), OutModules);
	CollectModulesFromGraph(GetGraphFromScript(System->GetSystemUpdateScript()), TEXT("system"), TEXT(""), TEXT("SystemUpdateScript"), OutModules);

	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
		if (!EmitterData)
		{
			continue;
		}

		UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
		if (!Source)
		{
			continue;
		}

		const FString EmitterName = Handle.GetName().ToString();
		CollectModulesFromGraph(Source->NodeGraph, TEXT("emitter"), EmitterName, TEXT("EmitterGraph"), OutModules);
	}
}

static TArray<TSharedPtr<FJsonValue>> ModulesToJson(UNiagaraSystem* System, bool bIncludePins)
{
	TArray<FNiagaraModuleRecord> Records;
	CollectModules(System, Records);

	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FNiagaraModuleRecord& Record : Records)
	{
		Values.Add(MakeShared<FJsonValueObject>(ModuleNodeToJson(Record, bIncludePins)));
	}
	return Values;
}

static TSharedPtr<FJsonObject> ParameterToJson(const FNiagaraVariableBase& Variable)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("name"), Variable.GetName().ToString());
	Obj->SetStringField(TEXT("type"), Variable.GetType().GetName());
	return Obj;
}

static TArray<TSharedPtr<FJsonValue>> UserParametersToJson(UNiagaraSystem* System)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	if (!System)
	{
		return Values;
	}

	for (const FNiagaraVariableWithOffset& Variable : System->GetExposedParameters().ReadParameterVariables())
	{
		Values.Add(MakeShared<FJsonValueObject>(ParameterToJson(Variable)));
	}
	return Values;
}

static TSharedPtr<FJsonObject> RendererToJson(UNiagaraRendererProperties* Renderer, int32 Index)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	if (!Renderer)
	{
		return Obj;
	}

	Obj->SetNumberField(TEXT("index"), Index);
	Obj->SetStringField(TEXT("name"), Renderer->GetName());
	Obj->SetStringField(TEXT("class"), Renderer->GetClass()->GetName());
	Obj->SetStringField(TEXT("path"), Renderer->GetPathName());
	Obj->SetBoolField(TEXT("enabled"), Renderer->GetIsEnabled());
	return Obj;
}

static TSharedPtr<FJsonObject> EmitterToJson(const FNiagaraEmitterHandle& Handle, bool bIncludeRenderers)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("name"), Handle.GetName().ToString());
	Obj->SetStringField(TEXT("id"), Handle.GetId().ToString(EGuidFormats::DigitsWithHyphens));
	Obj->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());

	const FVersionedNiagaraEmitter Instance = Handle.GetInstance();
	Obj->SetStringField(TEXT("emitter_asset"), GetObjectPathString(Instance.Emitter));

	FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
	if (EmitterData)
	{
		Obj->SetStringField(TEXT("sim_target"), EnumToStringSafe(StaticEnum<ENiagaraSimTarget>(), static_cast<int64>(EmitterData->SimTarget)));
		Obj->SetStringField(TEXT("calculate_bounds_mode"), EnumToStringSafe(StaticEnum<ENiagaraEmitterCalculateBoundMode>(), static_cast<int64>(EmitterData->CalculateBoundsMode)));
		Obj->SetStringField(TEXT("version_guid"), EmitterData->Version.VersionGuid.ToString(EGuidFormats::DigitsWithHyphens));
		Obj->SetNumberField(TEXT("renderer_count"), EmitterData->GetRenderers().Num());

		if (bIncludeRenderers)
		{
			TArray<TSharedPtr<FJsonValue>> Renderers;
			const TArray<UNiagaraRendererProperties*>& RendererList = EmitterData->GetRenderers();
			for (int32 Index = 0; Index < RendererList.Num(); ++Index)
			{
				Renderers.Add(MakeShared<FJsonValueObject>(RendererToJson(RendererList[Index], Index)));
			}
			Obj->SetArrayField(TEXT("renderers"), Renderers);
		}
	}
	return Obj;
}

static TArray<TSharedPtr<FJsonValue>> EmittersToJson(UNiagaraSystem* System, bool bIncludeRenderers)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	if (!System)
	{
		return Values;
	}

	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		Values.Add(MakeShared<FJsonValueObject>(EmitterToJson(Handle, bIncludeRenderers)));
	}
	return Values;
}

static FString ExportPropertyText(FProperty* Property, const void* Container)
{
	if (!Property || !Container)
	{
		return FString();
	}

	FString Text;
	const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Container);
	Property->ExportTextItem_Direct(Text, ValuePtr, nullptr, nullptr, PPF_None);
	return Text;
}

static TArray<TSharedPtr<FJsonValue>> PropertiesToJson(UStruct* StructType, const void* Container, int32 MaxProperties)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	if (!StructType || !Container)
	{
		return Values;
	}

	int32 Count = 0;
	for (TFieldIterator<FProperty> It(StructType); It; ++It)
	{
		if (MaxProperties > 0 && Count >= MaxProperties)
		{
			break;
		}
		FProperty* Property = *It;
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Property->GetName());
		Obj->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
		Obj->SetStringField(TEXT("value_text"), ExportPropertyText(Property, Container));
		Values.Add(MakeShared<FJsonValueObject>(Obj));
		++Count;
	}
	return Values;
}

static TSharedPtr<FJsonObject> BuildSummary(UNiagaraSystem* System, bool bIncludeModules)
{
	TSharedPtr<FJsonObject> Root = MakeSuccessObject();
	Root->SetStringField(TEXT("object_path"), System ? System->GetPathName() : TEXT(""));
	if (!System)
	{
		return Root;
	}

	Root->SetBoolField(TEXT("ready_to_run"), System->IsReadyToRun());
	Root->SetBoolField(TEXT("needs_warmup"), System->NeedsWarmup());
	Root->SetNumberField(TEXT("warmup_time"), System->GetWarmupTime());
	Root->SetNumberField(TEXT("warmup_tick_count"), System->GetWarmupTickCount());
	Root->SetNumberField(TEXT("warmup_tick_delta"), System->GetWarmupTickDelta());
	Root->SetStringField(TEXT("fixed_bounds"), System->GetFixedBounds().ToString());
	Root->SetArrayField(TEXT("user_parameters"), UserParametersToJson(System));
	Root->SetArrayField(TEXT("emitters"), EmittersToJson(System, true));
	if (bIncludeModules)
	{
		Root->SetArrayField(TEXT("modules"), ModulesToJson(System, false));
	}
	return Root;
}

static FString JsonValueToImportText(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid())
	{
		return FString();
	}

	switch (Value->Type)
	{
	case EJson::String:
		return Value->AsString();
	case EJson::Number:
		return FString::SanitizeFloat(Value->AsNumber());
	case EJson::Boolean:
		return Value->AsBool() ? TEXT("True") : TEXT("False");
	default:
		return TEXT("");
	}
}

static bool FindEmitterHandle(UNiagaraSystem* System, const FString& NameOrId, FNiagaraEmitterHandle*& OutHandle)
{
	OutHandle = nullptr;
	if (!System || NameOrId.IsEmpty())
	{
		return false;
	}

	for (FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		if (Handle.GetName().ToString().Equals(NameOrId, ESearchCase::IgnoreCase) ||
			Handle.GetId().ToString(EGuidFormats::DigitsWithHyphens).Equals(NameOrId, ESearchCase::IgnoreCase))
		{
			OutHandle = &Handle;
			return true;
		}
	}
	return false;
}

static bool ResolveTarget(UNiagaraSystem* System, const TSharedPtr<FJsonObject>& Patch, FResolvedTarget& OutTarget, FString& OutError)
{
	if (!System)
	{
		OutError = TEXT("Invalid Niagara system");
		return false;
	}

	FString Target = TEXT("system");
	if (Patch.IsValid())
	{
		Patch->TryGetStringField(TEXT("target"), Target);
		Patch->TryGetStringField(TEXT("object"), Target);
	}

	if (Target.Equals(TEXT("system"), ESearchCase::IgnoreCase))
	{
		OutTarget.Object = System;
		OutTarget.StructType = System->GetClass();
		OutTarget.StructPtr = System;
		OutTarget.Label = TEXT("system");
		return true;
	}

	if (Target.Equals(TEXT("emitter_data"), ESearchCase::IgnoreCase) || Target.Equals(TEXT("emitter"), ESearchCase::IgnoreCase))
	{
		FString EmitterName;
		Patch->TryGetStringField(TEXT("emitter"), EmitterName);
		Patch->TryGetStringField(TEXT("emitter_name"), EmitterName);
		Patch->TryGetStringField(TEXT("emitter_id"), EmitterName);

		FNiagaraEmitterHandle* Handle = nullptr;
		if (!FindEmitterHandle(System, EmitterName, Handle) || !Handle)
		{
			OutError = FString::Printf(TEXT("Emitter not found: %s"), *EmitterName);
			return false;
		}

		FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData();
		if (!EmitterData)
		{
			OutError = FString::Printf(TEXT("Emitter has no editable data: %s"), *EmitterName);
			return false;
		}

		OutTarget.StructType = FVersionedNiagaraEmitterData::StaticStruct();
		OutTarget.StructPtr = EmitterData;
		OutTarget.Label = FString::Printf(TEXT("emitter_data:%s"), *Handle->GetName().ToString());
		return true;
	}

	if (Target.Equals(TEXT("renderer"), ESearchCase::IgnoreCase))
	{
		FString EmitterName;
		Patch->TryGetStringField(TEXT("emitter"), EmitterName);
		Patch->TryGetStringField(TEXT("emitter_name"), EmitterName);
		int32 RendererIndex = INDEX_NONE;
		Patch->TryGetNumberField(TEXT("renderer_index"), RendererIndex);
		Patch->TryGetNumberField(TEXT("index"), RendererIndex);

		FNiagaraEmitterHandle* Handle = nullptr;
		if (!FindEmitterHandle(System, EmitterName, Handle) || !Handle)
		{
			OutError = FString::Printf(TEXT("Emitter not found for renderer target: %s"), *EmitterName);
			return false;
		}

		FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData();
		UNiagaraRendererProperties* Renderer = EmitterData ? EmitterData->GetRenderer(RendererIndex) : nullptr;
		if (!Renderer)
		{
			OutError = FString::Printf(TEXT("Renderer not found: emitter=%s index=%d"), *EmitterName, RendererIndex);
			return false;
		}

		OutTarget.Object = Renderer;
		OutTarget.StructType = Renderer->GetClass();
		OutTarget.StructPtr = Renderer;
		OutTarget.Label = FString::Printf(TEXT("renderer:%s:%d"), *Handle->GetName().ToString(), RendererIndex);
		return true;
	}

	OutError = FString::Printf(TEXT("Unknown target: %s"), *Target);
	return false;
}

static bool ApplyPropertyPatch(const FResolvedTarget& Target, const FString& PropertyName, const FString& ValueText, FString& OutBefore, FString& OutAfter, FString& OutError)
{
	if (!Target.StructType || !Target.StructPtr)
	{
		OutError = TEXT("Invalid target");
		return false;
	}

	FProperty* Property = Target.StructType->FindPropertyByName(*PropertyName);
	if (!Property)
	{
		OutError = FString::Printf(TEXT("Property not found on %s: %s"), *Target.Label, *PropertyName);
		return false;
	}

	OutBefore = ExportPropertyText(Property, Target.StructPtr);
	void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Target.StructPtr);
	const TCHAR* ImportEnd = Property->ImportText_Direct(*ValueText, ValuePtr, Target.Object, PPF_None);
	if (!ImportEnd)
	{
		OutError = FString::Printf(TEXT("Failed to import '%s' into %s.%s"), *ValueText, *Target.Label, *PropertyName);
		return false;
	}
	OutAfter = ExportPropertyText(Property, Target.StructPtr);
	return true;
}

static TSharedPtr<FJsonObject> PatchResult(const TSharedPtr<FJsonObject>& Patch, const FString& Status, const FString& Message)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("status"), Status);
	Obj->SetStringField(TEXT("message"), Message);
	if (Patch.IsValid())
	{
		Obj->SetObjectField(TEXT("patch"), Patch);
	}
	return Obj;
}

static bool ReadBoolField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Name, bool& OutValue)
{
	return Obj.IsValid() && Obj->TryGetBoolField(Name, OutValue);
}

static bool ReadStringField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Name, FString& OutValue)
{
	return Obj.IsValid() && Obj->TryGetStringField(Name, OutValue);
}

static bool ModuleMatchesSelector(const FNiagaraModuleRecord& Record, const TSharedPtr<FJsonObject>& Selector)
{
	if (!Record.Node || !Selector.IsValid())
	{
		return false;
	}

	FString EmitterFilter;
	FString ModuleFilter;
	FString NodeGuidFilter;
	int32 ModuleIndex = INDEX_NONE;
	ReadStringField(Selector, TEXT("emitter"), EmitterFilter);
	ReadStringField(Selector, TEXT("module"), ModuleFilter);
	ReadStringField(Selector, TEXT("module_title"), ModuleFilter);
	ReadStringField(Selector, TEXT("function_name"), ModuleFilter);
	ReadStringField(Selector, TEXT("node_guid"), NodeGuidFilter);
	Selector->TryGetNumberField(TEXT("module_index"), ModuleIndex);
	Selector->TryGetNumberField(TEXT("index"), ModuleIndex);

	if (!EmitterFilter.IsEmpty() && !Record.EmitterName.Equals(EmitterFilter, ESearchCase::IgnoreCase))
	{
		return false;
	}
	if (ModuleIndex != INDEX_NONE && Record.ModuleIndex != ModuleIndex)
	{
		return false;
	}
	if (!NodeGuidFilter.IsEmpty() && !Record.Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens).Equals(NodeGuidFilter, ESearchCase::IgnoreCase))
	{
		return false;
	}
	if (!ModuleFilter.IsEmpty())
	{
		const FString Haystack = Record.Node->GetFunctionName()
			+ TEXT(" ")
			+ Record.Node->GetNodeTitle(ENodeTitleType::ListView).ToString()
			+ TEXT(" ")
			+ Record.Node->FunctionScriptAssetObjectPath.ToString();
		if (!Haystack.Contains(ModuleFilter, ESearchCase::IgnoreCase))
		{
			return false;
		}
	}
	return true;
}

static bool FindModuleRecord(UNiagaraSystem* System, const TSharedPtr<FJsonObject>& Selector, FNiagaraModuleRecord& OutRecord, FString& OutError)
{
	if (!System)
	{
		OutError = TEXT("Invalid Niagara system");
		return false;
	}
	if (!Selector.IsValid())
	{
		OutError = TEXT("SelectorJson must be a JSON object");
		return false;
	}

	TArray<FNiagaraModuleRecord> Records;
	CollectModules(System, Records);
	for (const FNiagaraModuleRecord& Record : Records)
	{
		if (ModuleMatchesSelector(Record, Selector))
		{
			OutRecord = Record;
			return true;
		}
	}

	OutError = TEXT("No matching Niagara module node found");
	return false;
}

static UEdGraphPin* FindModulePin(UNiagaraNodeFunctionCall* Node, const FString& PinName)
{
	if (!Node || PinName.IsEmpty())
	{
		return nullptr;
	}

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin)
		{
			continue;
		}
		const bool bNameMatches = Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase);
		const bool bFriendlyNameMatches = Pin->GetDisplayName().ToString().Equals(PinName, ESearchCase::IgnoreCase);
		if (bNameMatches || bFriendlyNameMatches)
		{
			return Pin;
		}
	}
	return nullptr;
}
}

FString UMassBattleNiagaraMCPApi::MCP_NiagaraGetApiStatus()
{
	using namespace MassBattleNiagaraMCP;

	TSharedPtr<FJsonObject> Root = MakeSuccessObject();
	Root->SetStringField(TEXT("api_name"), TEXT("MassBattleNiagaraMCP"));
	Root->SetStringField(TEXT("version"), TEXT("0.1.0"));
	Root->SetStringField(TEXT("model"), TEXT("primitive_tools_not_workflow_buttons"));

	TArray<TSharedPtr<FJsonValue>> Tools;
	auto Tool = [](const FString& Name, const FString& Category, const FString& Description, const FString& Params) -> TSharedPtr<FJsonValue>
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Name);
		Obj->SetStringField(TEXT("category"), Category);
		Obj->SetStringField(TEXT("description"), Description);
		Obj->SetStringField(TEXT("parameters"), Params);
		return MakeShared<FJsonValueObject>(Obj);
	};

	Tools.Add(Tool(TEXT("MCP_NiagaraQuery"), TEXT("niagara.query"), TEXT("Query Niagara systems by path/name text."), TEXT("QueryJson")));
	Tools.Add(Tool(TEXT("MCP_NiagaraReadSummary"), TEXT("niagara.read"), TEXT("Read system, emitter, renderer, user parameter, and module summary."), TEXT("SystemPath, OptionsJson")));
	Tools.Add(Tool(TEXT("MCP_NiagaraReadModule"), TEXT("niagara.read"), TEXT("Read one function-call module node with pins."), TEXT("SystemPath, SelectorJson")));
	Tools.Add(Tool(TEXT("MCP_NiagaraReadAll"), TEXT("niagara.read"), TEXT("Read full reflected Niagara data plus all module nodes."), TEXT("SystemPath, OptionsJson")));
	Tools.Add(Tool(TEXT("MCP_NiagaraExportText"), TEXT("niagara.text"), TEXT("Write a deterministic text dump for LLM reading."), TEXT("SystemPath, OptionsJson")));
	Tools.Add(Tool(TEXT("MCP_NiagaraMergeWrite"), TEXT("niagara.write"), TEXT("Union-merge property writes on system/emitter_data/renderer targets; never deletes."), TEXT("SystemPath, PatchJson, bSaveAssets")));
	Tools.Add(Tool(TEXT("MCP_NiagaraSetModulePin"), TEXT("niagara.write"), TEXT("Set one unlinked function-call module pin default value."), TEXT("SystemPath, SelectorJson, PinName, ValueText, bSaveAssets")));
	Tools.Add(Tool(TEXT("MCP_NiagaraSetEmitterEnabled"), TEXT("niagara.write"), TEXT("Set one emitter handle enabled state explicitly."), TEXT("SystemPath, EmitterName, bEnabled, bSaveAssets")));
	Tools.Add(Tool(TEXT("MCP_NiagaraDelete"), TEXT("niagara.delete"), TEXT("Explicit destructive operations such as renderer removal or user-parameter removal."), TEXT("SystemPath, DeleteJson, bSaveAssets")));
	Root->SetArrayField(TEXT("tools"), Tools);
	return ToJsonString(Root);
}

FString UMassBattleNiagaraMCPApi::MCP_NiagaraQuery(const FString& QueryJson)
{
	using namespace MassBattleNiagaraMCP;

	TSharedPtr<FJsonObject> Query = ParseObject(QueryJson);
	if (!Query.IsValid())
	{
		return MakeErrorJson(TEXT("QueryJson must be a JSON object"));
	}

	FString Text;
	FString RootPath = TEXT("/Game");
	int32 Limit = 100;
	Query->TryGetStringField(TEXT("query"), Text);
	Query->TryGetStringField(TEXT("path"), RootPath);
	Query->TryGetStringField(TEXT("root"), RootPath);
	Query->TryGetNumberField(TEXT("limit"), Limit);
	Limit = FMath::Clamp(Limit, 1, 1000);

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	FARFilter Filter;
	Filter.ClassPaths.Add(UNiagaraSystem::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(FName(*RootPath));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Assets;
	AssetRegistryModule.Get().GetAssets(Filter, Assets);
	Assets.Sort([](const FAssetData& A, const FAssetData& B)
	{
		return A.GetObjectPathString() < B.GetObjectPathString();
	});

	TArray<TSharedPtr<FJsonValue>> Results;
	int32 Matched = 0;
	for (const FAssetData& Asset : Assets)
	{
		const FString ObjectPath = Asset.GetObjectPathString();
		if (!Text.IsEmpty() && !ObjectPath.Contains(Text, ESearchCase::IgnoreCase) && !Asset.AssetName.ToString().Contains(Text, ESearchCase::IgnoreCase))
		{
			continue;
		}
		++Matched;
		if (Results.Num() >= Limit)
		{
			continue;
		}

		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		Obj->SetStringField(TEXT("object_path"), ObjectPath);
		Obj->SetStringField(TEXT("package_path"), Asset.PackagePath.ToString());
		Results.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedPtr<FJsonObject> Root = MakeSuccessObject();
	Root->SetNumberField(TEXT("matched"), Matched);
	Root->SetNumberField(TEXT("returned"), Results.Num());
	Root->SetArrayField(TEXT("systems"), Results);
	return ToJsonString(Root);
}

FString UMassBattleNiagaraMCPApi::MCP_NiagaraReadSummary(const FString& SystemPath, const FString& OptionsJson)
{
	using namespace MassBattleNiagaraMCP;

	TSharedPtr<FJsonObject> Options = ParseObject(OptionsJson);
	if (!Options.IsValid())
	{
		return MakeErrorJson(TEXT("OptionsJson must be a JSON object"));
	}

	FString LoadError;
	UNiagaraSystem* System = LoadSystem(SystemPath, LoadError);
	if (!System)
	{
		return MakeErrorJson(LoadError);
	}

	bool bIncludeModules = true;
	Options->TryGetBoolField(TEXT("include_modules"), bIncludeModules);
	return ToJsonString(BuildSummary(System, bIncludeModules));
}

FString UMassBattleNiagaraMCPApi::MCP_NiagaraReadModule(const FString& SystemPath, const FString& SelectorJson)
{
	using namespace MassBattleNiagaraMCP;

	TSharedPtr<FJsonObject> Selector = ParseObject(SelectorJson);
	if (!Selector.IsValid())
	{
		return MakeErrorJson(TEXT("SelectorJson must be a JSON object"));
	}

	FString LoadError;
	UNiagaraSystem* System = LoadSystem(SystemPath, LoadError);
	if (!System)
	{
		return MakeErrorJson(LoadError);
	}

	FString EmitterFilter;
	FString ModuleFilter;
	FString NodeGuidFilter;
	int32 ModuleIndex = INDEX_NONE;
	ReadStringField(Selector, TEXT("emitter"), EmitterFilter);
	ReadStringField(Selector, TEXT("module"), ModuleFilter);
	ReadStringField(Selector, TEXT("function_name"), ModuleFilter);
	ReadStringField(Selector, TEXT("node_guid"), NodeGuidFilter);
	Selector->TryGetNumberField(TEXT("module_index"), ModuleIndex);
	Selector->TryGetNumberField(TEXT("index"), ModuleIndex);

	TArray<FNiagaraModuleRecord> Records;
	CollectModules(System, Records);

	for (const FNiagaraModuleRecord& Record : Records)
	{
		if (!Record.Node)
		{
			continue;
		}
		if (!EmitterFilter.IsEmpty() && !Record.EmitterName.Equals(EmitterFilter, ESearchCase::IgnoreCase))
		{
			continue;
		}
		if (ModuleIndex != INDEX_NONE && Record.ModuleIndex != ModuleIndex)
		{
			continue;
		}
		if (!NodeGuidFilter.IsEmpty() && !Record.Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens).Equals(NodeGuidFilter, ESearchCase::IgnoreCase))
		{
			continue;
		}
		if (!ModuleFilter.IsEmpty())
		{
			const FString Haystack = Record.Node->GetFunctionName() + TEXT(" ") + Record.Node->GetNodeTitle(ENodeTitleType::ListView).ToString() + TEXT(" ") + Record.Node->FunctionScriptAssetObjectPath.ToString();
			if (!Haystack.Contains(ModuleFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		TSharedPtr<FJsonObject> Root = MakeSuccessObject();
		Root->SetObjectField(TEXT("module"), ModuleNodeToJson(Record, true));
		return ToJsonString(Root);
	}

	return MakeErrorJson(TEXT("No matching Niagara module node found"));
}

FString UMassBattleNiagaraMCPApi::MCP_NiagaraReadAll(const FString& SystemPath, const FString& OptionsJson)
{
	using namespace MassBattleNiagaraMCP;

	TSharedPtr<FJsonObject> Options = ParseObject(OptionsJson);
	if (!Options.IsValid())
	{
		return MakeErrorJson(TEXT("OptionsJson must be a JSON object"));
	}

	FString LoadError;
	UNiagaraSystem* System = LoadSystem(SystemPath, LoadError);
	if (!System)
	{
		return MakeErrorJson(LoadError);
	}

	int32 MaxProperties = 256;
	Options->TryGetNumberField(TEXT("max_properties"), MaxProperties);

	TSharedPtr<FJsonObject> Root = BuildSummary(System, true);
	Root->SetArrayField(TEXT("system_properties"), PropertiesToJson(System->GetClass(), System, MaxProperties));
	Root->SetArrayField(TEXT("modules_detailed"), ModulesToJson(System, true));

	TArray<TSharedPtr<FJsonValue>> EmitterDataProperties;
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
		if (!EmitterData)
		{
			continue;
		}
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("emitter"), Handle.GetName().ToString());
		Obj->SetArrayField(TEXT("properties"), PropertiesToJson(FVersionedNiagaraEmitterData::StaticStruct(), EmitterData, MaxProperties));
		EmitterDataProperties.Add(MakeShared<FJsonValueObject>(Obj));
	}
	Root->SetArrayField(TEXT("emitter_data_properties"), EmitterDataProperties);
	return ToJsonString(Root);
}

FString UMassBattleNiagaraMCPApi::MCP_NiagaraExportText(const FString& SystemPath, const FString& OptionsJson)
{
	using namespace MassBattleNiagaraMCP;

	FString LoadError;
	UNiagaraSystem* System = LoadSystem(SystemPath, LoadError);
	if (!System)
	{
		return MakeErrorJson(LoadError);
	}

	TSharedPtr<FJsonObject> Dump = BuildSummary(System, true);
	Dump->SetArrayField(TEXT("modules_detailed"), ModulesToJson(System, true));

	FString Text;
	Text += FString::Printf(TEXT("NiagaraSystem: %s\n"), *System->GetPathName());
	Text += FString::Printf(TEXT("ReadyToRun: %s\n"), System->IsReadyToRun() ? TEXT("true") : TEXT("false"));
	Text += FString::Printf(TEXT("Warmup: time=%f ticks=%d delta=%f\n"), System->GetWarmupTime(), System->GetWarmupTickCount(), System->GetWarmupTickDelta());
	Text += TEXT("\nJSON:\n");
	Text += ToJsonString(Dump);
	Text += LINE_TERMINATOR;

	bool bWriteFile = true;
	TSharedPtr<FJsonObject> Options = ParseObject(OptionsJson);
	if (Options.IsValid())
	{
		Options->TryGetBoolField(TEXT("write_file"), bWriteFile);
	}

	TSharedPtr<FJsonObject> Root = MakeSuccessObject();
	Root->SetStringField(TEXT("text"), Text);
	if (bWriteFile)
	{
		IFileManager::Get().MakeDirectory(*GetSavedExportDir(), true);
		const FString AssetName = FPackageName::GetLongPackageAssetName(System->GetOutermost()->GetName());
		const FString OutputPath = FPaths::Combine(GetSavedExportDir(), AssetName + TEXT("_niagara.txt"));
		FFileHelper::SaveStringToFile(Text, *OutputPath);
		Root->SetStringField(TEXT("text_path"), OutputPath);
	}
	return ToJsonString(Root);
}

FString UMassBattleNiagaraMCPApi::MCP_NiagaraMergeWrite(const FString& SystemPath, const FString& PatchJson, bool bSaveAssets)
{
	using namespace MassBattleNiagaraMCP;

	TSharedPtr<FJsonObject> PatchRoot = ParseObject(PatchJson);
	if (!PatchRoot.IsValid())
	{
		return MakeErrorJson(TEXT("PatchJson must be a JSON object"));
	}

	FString LoadError;
	UNiagaraSystem* System = LoadSystem(SystemPath, LoadError);
	if (!System)
	{
		return MakeErrorJson(LoadError);
	}

	TArray<TSharedPtr<FJsonValue>> Results;

	const TArray<TSharedPtr<FJsonValue>>* Patches = nullptr;
	if (!PatchRoot->TryGetArrayField(TEXT("patches"), Patches))
	{
		Patches = nullptr;
	}

	TArray<TSharedPtr<FJsonValue>> SinglePatchArray;
	if (!Patches)
	{
		SinglePatchArray.Add(MakeShared<FJsonValueObject>(PatchRoot));
		Patches = &SinglePatchArray;
	}

	for (const TSharedPtr<FJsonValue>& PatchValue : *Patches)
	{
		const TSharedPtr<FJsonObject> Patch = PatchValue.IsValid() ? PatchValue->AsObject() : nullptr;
		if (!Patch.IsValid())
		{
			Results.Add(MakeShared<FJsonValueObject>(PatchResult(nullptr, TEXT("error"), TEXT("Patch entry is not an object"))));
			continue;
		}

		FResolvedTarget Target;
		FString TargetError;
		if (!ResolveTarget(System, Patch, Target, TargetError))
		{
			Results.Add(MakeShared<FJsonValueObject>(PatchResult(Patch, TEXT("error"), TargetError)));
			continue;
		}

		FString PropertyName;
		Patch->TryGetStringField(TEXT("property"), PropertyName);
		Patch->TryGetStringField(TEXT("path"), PropertyName);
		if (PropertyName.IsEmpty())
		{
			Results.Add(MakeShared<FJsonValueObject>(PatchResult(Patch, TEXT("error"), TEXT("Patch requires property or path"))));
			continue;
		}

		FString ValueText;
		if (!Patch->TryGetStringField(TEXT("value_text"), ValueText))
		{
			const TSharedPtr<FJsonValue> ValueField = Patch->TryGetField(TEXT("value"));
			if (ValueField.IsValid())
			{
				ValueText = JsonValueToImportText(ValueField);
			}
		}
		if (ValueText.IsEmpty())
		{
			Results.Add(MakeShared<FJsonValueObject>(PatchResult(Patch, TEXT("error"), TEXT("Patch requires value_text, or scalar value"))));
			continue;
		}

		FString Before;
		FString After;
		FString Error;
		if (!ApplyPropertyPatch(Target, PropertyName, ValueText, Before, After, Error))
		{
			Results.Add(MakeShared<FJsonValueObject>(PatchResult(Patch, TEXT("error"), Error)));
			continue;
		}

		TSharedPtr<FJsonObject> Result = PatchResult(Patch, TEXT("applied"), TEXT("property updated"));
		Result->SetStringField(TEXT("target_resolved"), Target.Label);
		Result->SetStringField(TEXT("before"), Before);
		Result->SetStringField(TEXT("after"), After);
		Results.Add(MakeShared<FJsonValueObject>(Result));
	}

	System->MarkPackageDirty();
	FString SaveError;
	bool bSaved = false;
	if (bSaveAssets)
	{
		bSaved = SaveAsset(System, SaveError);
		if (!bSaved)
		{
			return MakeErrorJson(SaveError);
		}
	}

	TSharedPtr<FJsonObject> Root = MakeSuccessObject();
	Root->SetStringField(TEXT("system"), System->GetPathName());
	Root->SetBoolField(TEXT("saved"), bSaved);
	Root->SetArrayField(TEXT("results"), Results);
	return ToJsonString(Root);
}

FString UMassBattleNiagaraMCPApi::MCP_NiagaraSetModulePin(const FString& SystemPath, const FString& SelectorJson, const FString& PinName, const FString& ValueText, bool bSaveAssets)
{
	using namespace MassBattleNiagaraMCP;

	TSharedPtr<FJsonObject> Selector = ParseObject(SelectorJson);
	if (!Selector.IsValid())
	{
		return MakeErrorJson(TEXT("SelectorJson must be a JSON object"));
	}

	FString LoadError;
	UNiagaraSystem* System = LoadSystem(SystemPath, LoadError);
	if (!System)
	{
		return MakeErrorJson(LoadError);
	}

	FNiagaraModuleRecord Record;
	FString SelectorError;
	if (!FindModuleRecord(System, Selector, Record, SelectorError))
	{
		return MakeErrorJson(SelectorError);
	}

	UEdGraphPin* Pin = FindModulePin(Record.Node, PinName);
	if (!Pin)
	{
		return MakeErrorJson(FString::Printf(TEXT("Pin not found on module %s: %s"), *Record.Node->GetName(), *PinName));
	}

	bool bAllowLinked = false;
	bool bAllowOutput = false;
	Selector->TryGetBoolField(TEXT("allow_linked"), bAllowLinked);
	Selector->TryGetBoolField(TEXT("allow_output"), bAllowOutput);
	if (Pin->Direction != EGPD_Input && !bAllowOutput)
	{
		return MakeErrorJson(FString::Printf(TEXT("Refusing to write non-input pin: %s"), *PinName));
	}
	if (Pin->LinkedTo.Num() > 0 && !bAllowLinked)
	{
		return MakeErrorJson(FString::Printf(TEXT("Pin is linked and its default value may be ignored: %s. Pass allow_linked=true in SelectorJson to force."), *PinName));
	}

	const FString Before = Pin->DefaultValue;
	System->Modify();
	if (UNiagaraGraph* Graph = Cast<UNiagaraGraph>(Record.Node->GetGraph()))
	{
		Graph->Modify();
	}
	Record.Node->Modify();
	Pin->DefaultValue = ValueText;
	Pin->DefaultObject = nullptr;
	Pin->DefaultTextValue = FText::GetEmpty();
	if (UEdGraph* Graph = Record.Node->GetGraph())
	{
		Graph->NotifyGraphChanged();
	}
	System->MarkPackageDirty();

	FString SaveError;
	bool bSaved = false;
	if (bSaveAssets)
	{
		bSaved = SaveAsset(System, SaveError);
		if (!bSaved)
		{
			return MakeErrorJson(SaveError);
		}
	}

	TSharedPtr<FJsonObject> Root = MakeSuccessObject();
	Root->SetStringField(TEXT("system"), System->GetPathName());
	Root->SetNumberField(TEXT("module_index"), Record.ModuleIndex);
	Root->SetStringField(TEXT("emitter"), Record.EmitterName);
	Root->SetStringField(TEXT("module"), Record.Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
	Root->SetStringField(TEXT("pin"), Pin->PinName.ToString());
	Root->SetNumberField(TEXT("linked_to_count"), Pin->LinkedTo.Num());
	Root->SetStringField(TEXT("before"), Before);
	Root->SetStringField(TEXT("after"), Pin->DefaultValue);
	Root->SetBoolField(TEXT("saved"), bSaved);
	return ToJsonString(Root);
}

FString UMassBattleNiagaraMCPApi::MCP_NiagaraSetEmitterEnabled(const FString& SystemPath, const FString& EmitterName, bool bEnabled, bool bSaveAssets)
{
	using namespace MassBattleNiagaraMCP;

	FString LoadError;
	UNiagaraSystem* System = LoadSystem(SystemPath, LoadError);
	if (!System)
	{
		return MakeErrorJson(LoadError);
	}

	FNiagaraEmitterHandle* Handle = nullptr;
	if (!FindEmitterHandle(System, EmitterName, Handle) || !Handle)
	{
		return MakeErrorJson(FString::Printf(TEXT("Emitter not found: %s"), *EmitterName));
	}

	const bool bBefore = Handle->GetIsEnabled();
	const bool bChanged = Handle->SetIsEnabled(bEnabled, *System, true);
	const bool bAfter = Handle->GetIsEnabled();
	if (bChanged)
	{
		System->MarkPackageDirty();
	}

	FString SaveError;
	bool bSaved = false;
	if (bSaveAssets && bChanged)
	{
		bSaved = SaveAsset(System, SaveError);
		if (!bSaved)
		{
			return MakeErrorJson(SaveError);
		}
	}

	TSharedPtr<FJsonObject> Root = MakeSuccessObject();
	Root->SetStringField(TEXT("system"), System->GetPathName());
	Root->SetStringField(TEXT("emitter"), Handle->GetName().ToString());
	Root->SetBoolField(TEXT("before"), bBefore);
	Root->SetBoolField(TEXT("after"), bAfter);
	Root->SetBoolField(TEXT("changed"), bChanged);
	Root->SetBoolField(TEXT("saved"), bSaved);
	return ToJsonString(Root);
}

FString UMassBattleNiagaraMCPApi::MCP_NiagaraDelete(const FString& SystemPath, const FString& DeleteJson, bool bSaveAssets)
{
	using namespace MassBattleNiagaraMCP;

	TSharedPtr<FJsonObject> DeleteRoot = ParseObject(DeleteJson);
	if (!DeleteRoot.IsValid())
	{
		return MakeErrorJson(TEXT("DeleteJson must be a JSON object"));
	}

	FString LoadError;
	UNiagaraSystem* System = LoadSystem(SystemPath, LoadError);
	if (!System)
	{
		return MakeErrorJson(LoadError);
	}

	FString Type;
	ReadStringField(DeleteRoot, TEXT("type"), Type);
	ReadStringField(DeleteRoot, TEXT("target"), Type);
	TSharedPtr<FJsonObject> Root = MakeSuccessObject();
	Root->SetStringField(TEXT("system"), System->GetPathName());

	bool bChanged = false;
	if (Type.Equals(TEXT("disable_emitter"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("emitter"), ESearchCase::IgnoreCase))
	{
		FString EmitterName;
		ReadStringField(DeleteRoot, TEXT("emitter"), EmitterName);
		ReadStringField(DeleteRoot, TEXT("emitter_name"), EmitterName);

		FNiagaraEmitterHandle* Handle = nullptr;
		if (!FindEmitterHandle(System, EmitterName, Handle) || !Handle)
		{
			return MakeErrorJson(FString::Printf(TEXT("Emitter not found: %s"), *EmitterName));
		}
		bChanged = Handle->SetIsEnabled(false, *System, true);
		Root->SetStringField(TEXT("operation"), TEXT("disable_emitter"));
		Root->SetStringField(TEXT("emitter"), Handle->GetName().ToString());
	}
	else if (Type.Equals(TEXT("renderer"), ESearchCase::IgnoreCase))
	{
		FString EmitterName;
		ReadStringField(DeleteRoot, TEXT("emitter"), EmitterName);
		int32 RendererIndex = INDEX_NONE;
		DeleteRoot->TryGetNumberField(TEXT("renderer_index"), RendererIndex);
		DeleteRoot->TryGetNumberField(TEXT("index"), RendererIndex);

		FNiagaraEmitterHandle* Handle = nullptr;
		if (!FindEmitterHandle(System, EmitterName, Handle) || !Handle)
		{
			return MakeErrorJson(FString::Printf(TEXT("Emitter not found: %s"), *EmitterName));
		}

		FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData();
		UNiagaraRendererProperties* Renderer = EmitterData ? EmitterData->GetRenderer(RendererIndex) : nullptr;
		if (!Renderer || !Handle->GetInstance().Emitter)
		{
			return MakeErrorJson(FString::Printf(TEXT("Renderer not found: emitter=%s index=%d"), *EmitterName, RendererIndex));
		}

		Handle->GetInstance().Emitter->RemoveRenderer(Renderer, EmitterData->Version.VersionGuid);
		bChanged = true;
		Root->SetStringField(TEXT("operation"), TEXT("remove_renderer"));
		Root->SetStringField(TEXT("emitter"), Handle->GetName().ToString());
		Root->SetNumberField(TEXT("renderer_index"), RendererIndex);
	}
	else if (Type.Equals(TEXT("user_parameter"), ESearchCase::IgnoreCase))
	{
		FString Name;
		ReadStringField(DeleteRoot, TEXT("name"), Name);
		ReadStringField(DeleteRoot, TEXT("parameter"), Name);
		if (Name.IsEmpty())
		{
			return MakeErrorJson(TEXT("user_parameter delete requires name"));
		}

		bool bRemoved = false;
		for (const FNiagaraVariableWithOffset& Variable : System->GetExposedParameters().ReadParameterVariables())
		{
			if (Variable.GetName().ToString().Equals(Name, ESearchCase::IgnoreCase))
			{
				bRemoved = System->GetExposedParameters().RemoveParameter(Variable);
				break;
			}
		}
		bChanged = bRemoved;
		Root->SetStringField(TEXT("operation"), TEXT("remove_user_parameter"));
		Root->SetStringField(TEXT("parameter"), Name);
		Root->SetBoolField(TEXT("removed"), bRemoved);
	}
	else
	{
		return MakeErrorJson(TEXT("Delete type must be renderer, user_parameter, or disable_emitter"));
	}

	if (bChanged)
	{
		System->MarkPackageDirty();
	}

	FString SaveError;
	bool bSaved = false;
	if (bSaveAssets && bChanged)
	{
		bSaved = SaveAsset(System, SaveError);
		if (!bSaved)
		{
			return MakeErrorJson(SaveError);
		}
	}

	Root->SetBoolField(TEXT("changed"), bChanged);
	Root->SetBoolField(TEXT("saved"), bSaved);
	return ToJsonString(Root);
}
