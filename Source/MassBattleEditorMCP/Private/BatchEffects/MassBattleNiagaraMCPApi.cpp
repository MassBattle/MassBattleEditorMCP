// Copyright (c) 2025-2026 Winyunq. All rights reserved.
#include "BatchEffects/MassBattleNiagaraMCPApi.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_Niagara.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/SecureHash.h"
#include "Misc/StringOutputDevice.h"
#include "Modules/ModuleManager.h"
#include "NiagaraDataInterface.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraExternalSystemEditorUtilities.h"
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraNodeAssignment.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraParameterStore.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "Materials/MaterialInterface.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"
#include "ViewModels/NiagaraEmitterHandleViewModel.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/Stack/NiagaraStackEntry.h"
#include "ViewModels/Stack/NiagaraStackFunctionInput.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "ViewModels/Stack/NiagaraStackModuleItem.h"
#include "ViewModels/Stack/NiagaraStackScriptItemGroup.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"

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

static FString ToCondensedJsonString(const TSharedPtr<FJsonObject>& Obj)
{
	FString OutputString;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutputString);
	FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
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
	Obj->SetStringField(TEXT("subcategory_object"), GetObjectPathString(Pin->PinType.PinSubCategoryObject.Get()));
	Obj->SetStringField(TEXT("pin_id"), Pin->PinId.ToString(EGuidFormats::DigitsWithHyphens));
	Obj->SetStringField(TEXT("persistent_guid"), Pin->PersistentGuid.ToString(EGuidFormats::DigitsWithHyphens));
	Obj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
	Obj->SetStringField(TEXT("default_object"), GetObjectPathString(Pin->DefaultObject));
	Obj->SetBoolField(TEXT("hidden"), Pin->bHidden);
	Obj->SetBoolField(TEXT("orphaned"), Pin->bOrphanedPin);
	Obj->SetBoolField(TEXT("not_connectable"), Pin->bNotConnectable);
	Obj->SetNumberField(TEXT("linked_to_count"), Pin->LinkedTo.Num());

	TArray<TSharedPtr<FJsonValue>> Links;
	for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
	{
		if (!LinkedPin)
		{
			continue;
		}
		TSharedPtr<FJsonObject> Link = MakeShared<FJsonObject>();
		const UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
		Link->SetStringField(TEXT("node_guid"), LinkedNode ? LinkedNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) : TEXT(""));
		Link->SetStringField(TEXT("node_name"), LinkedNode ? LinkedNode->GetName() : TEXT(""));
		Link->SetStringField(TEXT("pin_name"), LinkedPin->PinName.ToString());
		Link->SetStringField(TEXT("pin_id"), LinkedPin->PinId.ToString(EGuidFormats::DigitsWithHyphens));
		Link->SetStringField(TEXT("persistent_guid"), LinkedPin->PersistentGuid.ToString(EGuidFormats::DigitsWithHyphens));
		Link->SetStringField(TEXT("direction"), PinDirectionToString(LinkedPin->Direction));
		Links.Add(MakeShared<FJsonValueObject>(Link));
	}
	Obj->SetArrayField(TEXT("links"), Links);
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

struct FNiagaraGraphContext
{
	UNiagaraGraph* Graph = nullptr;
	UNiagaraNodeOutput* OutputNode = nullptr;
	FString Scope;
	FString EmitterName;
	ENiagaraScriptUsage Usage = ENiagaraScriptUsage::Function;
	FGuid UsageId;
};

struct FCreatedNodeRecord
{
	UNiagaraNode* Node = nullptr;
	FNiagaraGraphContext Context;
};

static bool ParseGuidField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, FGuid& OutGuid)
{
	FString Value;
	if (!Object.IsValid() || !Object->TryGetStringField(FieldName, Value) || Value.IsEmpty())
	{
		return false;
	}
	return FGuid::Parse(Value, OutGuid);
}

static bool ParseScriptUsage(FString Value, ENiagaraScriptUsage& OutUsage)
{
	Value.TrimStartAndEndInline();
	Value.ReplaceInline(TEXT("-"), TEXT("_"));
	const FString Compact = Value.Replace(TEXT("_"), TEXT("")).ToLower();
	if (Compact == TEXT("systemspawn") || Compact == TEXT("systemspawnscript")) { OutUsage = ENiagaraScriptUsage::SystemSpawnScript; return true; }
	if (Compact == TEXT("systemupdate") || Compact == TEXT("systemupdatescript")) { OutUsage = ENiagaraScriptUsage::SystemUpdateScript; return true; }
	if (Compact == TEXT("emitterspawn") || Compact == TEXT("emitterspawnscript")) { OutUsage = ENiagaraScriptUsage::EmitterSpawnScript; return true; }
	if (Compact == TEXT("emitterupdate") || Compact == TEXT("emitterupdatescript")) { OutUsage = ENiagaraScriptUsage::EmitterUpdateScript; return true; }
	if (Compact == TEXT("particlespawn") || Compact == TEXT("particlespawnscript")) { OutUsage = ENiagaraScriptUsage::ParticleSpawnScript; return true; }
	if (Compact == TEXT("particleupdate") || Compact == TEXT("particleupdatescript")) { OutUsage = ENiagaraScriptUsage::ParticleUpdateScript; return true; }
	if (Compact == TEXT("particleevent") || Compact == TEXT("particleeventscript")) { OutUsage = ENiagaraScriptUsage::ParticleEventScript; return true; }
	if (Compact == TEXT("particlesimulationstage") || Compact == TEXT("particlesimulationstagescript")) { OutUsage = ENiagaraScriptUsage::ParticleSimulationStageScript; return true; }
	if (Compact == TEXT("particlegpucompute") || Compact == TEXT("particlegpucomputescript")) { OutUsage = ENiagaraScriptUsage::ParticleGPUComputeScript; return true; }

	const UEnum* UsageEnum = StaticEnum<ENiagaraScriptUsage>();
	if (UsageEnum)
	{
		for (int32 Index = 0; Index < UsageEnum->NumEnums(); ++Index)
		{
			if (UsageEnum->GetNameStringByIndex(Index).Equals(Value, ESearchCase::IgnoreCase))
			{
				OutUsage = static_cast<ENiagaraScriptUsage>(UsageEnum->GetValueByIndex(Index));
				return true;
			}
		}
	}
	return false;
}

static void AddGraphContexts(
	UNiagaraGraph* Graph,
	const FString& Scope,
	const FString& EmitterName,
	TArray<FNiagaraGraphContext>& OutContexts)
{
	if (!Graph)
	{
		return;
	}

	for (UEdGraphNode* GraphNode : Graph->Nodes)
	{
		UNiagaraNodeOutput* OutputNode = Cast<UNiagaraNodeOutput>(GraphNode);
		if (!OutputNode)
		{
			continue;
		}

		const ENiagaraScriptUsage Usage = OutputNode->GetUsage();
		const bool bSystemUsage = Usage == ENiagaraScriptUsage::SystemSpawnScript || Usage == ENiagaraScriptUsage::SystemUpdateScript;
		if ((Scope == TEXT("system")) != bSystemUsage)
		{
			continue;
		}

		FNiagaraGraphContext Context;
		Context.Graph = Graph;
		Context.OutputNode = OutputNode;
		Context.Scope = Scope;
		Context.EmitterName = EmitterName;
		Context.Usage = Usage;
		Context.UsageId = OutputNode->GetUsageId();
		OutContexts.Add(Context);
	}
}

static void CollectGraphContexts(UNiagaraSystem* System, TArray<FNiagaraGraphContext>& OutContexts)
{
	OutContexts.Reset();
	if (!System)
	{
		return;
	}

	TSet<UNiagaraGraph*> SystemGraphs;
	SystemGraphs.Add(GetGraphFromScript(System->GetSystemSpawnScript()));
	SystemGraphs.Add(GetGraphFromScript(System->GetSystemUpdateScript()));
	SystemGraphs.Remove(nullptr);
	for (UNiagaraGraph* Graph : SystemGraphs)
	{
		AddGraphContexts(Graph, TEXT("system"), TEXT(""), OutContexts);
	}

	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
		UNiagaraScriptSource* Source = EmitterData ? Cast<UNiagaraScriptSource>(EmitterData->GraphSource) : nullptr;
		AddGraphContexts(Source ? Source->NodeGraph : nullptr, TEXT("emitter"), Handle.GetName().ToString(), OutContexts);
	}

	OutContexts.Sort([](const FNiagaraGraphContext& A, const FNiagaraGraphContext& B)
	{
		const FString AKey = A.Scope + TEXT("|") + A.EmitterName + TEXT("|") + ScriptUsageToString(A.Usage) + TEXT("|") + A.UsageId.ToString();
		const FString BKey = B.Scope + TEXT("|") + B.EmitterName + TEXT("|") + ScriptUsageToString(B.Usage) + TEXT("|") + B.UsageId.ToString();
		return AKey < BKey;
	});
}

static TSharedPtr<FJsonObject> GetGraphSelector(const TSharedPtr<FJsonObject>& Object)
{
	const TSharedPtr<FJsonObject>* GraphSelector = nullptr;
	if (Object.IsValid() && Object->TryGetObjectField(TEXT("graph"), GraphSelector) && GraphSelector && GraphSelector->IsValid())
	{
		return *GraphSelector;
	}
	return Object;
}

static bool SelectGraphContexts(
	UNiagaraSystem* System,
	const TSharedPtr<FJsonObject>& Selector,
	TArray<FNiagaraGraphContext>& OutContexts,
	FString& OutError,
	bool bRequireExactlyOne)
{
	TArray<FNiagaraGraphContext> Contexts;
	CollectGraphContexts(System, Contexts);

	FString ScopeFilter;
	FString EmitterFilter;
	FString UsageFilter;
	FString OutputGuidFilter;
	FGuid UsageIdFilter;
	bool bHasUsageId = false;
	if (Selector.IsValid())
	{
		Selector->TryGetStringField(TEXT("scope"), ScopeFilter);
		Selector->TryGetStringField(TEXT("emitter"), EmitterFilter);
		Selector->TryGetStringField(TEXT("emitter_name"), EmitterFilter);
		Selector->TryGetStringField(TEXT("script_usage"), UsageFilter);
		Selector->TryGetStringField(TEXT("usage"), UsageFilter);
		Selector->TryGetStringField(TEXT("output_node_guid"), OutputGuidFilter);
		bHasUsageId = ParseGuidField(Selector, TEXT("usage_id"), UsageIdFilter);
	}

	ENiagaraScriptUsage RequiredUsage = ENiagaraScriptUsage::Function;
	if (!UsageFilter.IsEmpty() && !ParseScriptUsage(UsageFilter, RequiredUsage))
	{
		OutError = FString::Printf(TEXT("Unknown Niagara script usage: %s"), *UsageFilter);
		return false;
	}

	OutContexts.Reset();
	for (const FNiagaraGraphContext& Context : Contexts)
	{
		if (!ScopeFilter.IsEmpty() && !Context.Scope.Equals(ScopeFilter, ESearchCase::IgnoreCase))
		{
			continue;
		}
		if (!EmitterFilter.IsEmpty() && !Context.EmitterName.Equals(EmitterFilter, ESearchCase::IgnoreCase))
		{
			continue;
		}
		if (!UsageFilter.IsEmpty() && Context.Usage != RequiredUsage)
		{
			continue;
		}
		if (bHasUsageId && Context.UsageId != UsageIdFilter)
		{
			continue;
		}
		if (!OutputGuidFilter.IsEmpty() && (!Context.OutputNode || !Context.OutputNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens).Equals(OutputGuidFilter, ESearchCase::IgnoreCase)))
		{
			continue;
		}
		OutContexts.Add(Context);
	}

	if (OutContexts.IsEmpty())
	{
		OutError = TEXT("No Niagara graph matched the selector");
		return false;
	}
	if (bRequireExactlyOne && OutContexts.Num() != 1)
	{
		TArray<FString> Matches;
		for (const FNiagaraGraphContext& Context : OutContexts)
		{
			Matches.Add(Context.Scope + TEXT(":") + Context.EmitterName + TEXT(":") + ScriptUsageToString(Context.Usage) + TEXT(":") + Context.UsageId.ToString(EGuidFormats::DigitsWithHyphens));
		}
		OutError = FString::Printf(TEXT("Graph selector is ambiguous (%d matches): %s"), OutContexts.Num(), *FString::Join(Matches, TEXT(", ")));
		return false;
	}
	return true;
}

static void BuildContextTraversal(const FNiagaraGraphContext& Context, TArray<UNiagaraNode*>& OutNodes)
{
	OutNodes.Reset();
	if (Context.Graph)
	{
		Context.Graph->BuildTraversal(OutNodes, Context.Usage, Context.UsageId, false);
	}
}

static FString JoinContextErrors(const TArray<FText>& Errors);

// UE 5.8's public external-edit stack reference currently resolves script groups with an empty
// usage id. Event-handler and simulation-stage stacks require their real usage id, so mirror the
// small public-stack traversal used by NiagaraStackQuery. Use the same traversal for regular
// stacks too, which keeps repeated module instances keyed by node GUID instead of function name.
// Actual input mutations still go through Niagara's exported stack-input editor methods.
static bool RequiresUsageIdStackLookup(ENiagaraScriptUsage Usage)
{
	return Usage == ENiagaraScriptUsage::ParticleEventScript
		|| Usage == ENiagaraScriptUsage::ParticleSimulationStageScript;
}

static UNiagaraStackScriptItemGroup* FindUsageAwareScriptGroup(
	FNiagaraExternalEditContext& ExternalContext,
	const FNiagaraGraphContext& GraphContext,
	FString& OutError)
{
	TSharedPtr<FNiagaraSystemViewModel> SystemViewModel = ExternalContext.GetDiagnosticsSystemViewModel();
	if (!SystemViewModel.IsValid())
	{
		OutError = ExternalContext.HasErrors()
			? JoinContextErrors(ExternalContext.Errors)
			: TEXT("Failed to create a Niagara system view model");
		return nullptr;
	}

	UNiagaraStackViewModel* StackViewModel = nullptr;
	if (GraphContext.Scope.Equals(TEXT("system"), ESearchCase::IgnoreCase))
	{
		StackViewModel = SystemViewModel->GetSystemStackViewModel();
	}
	else
	{
		const TSharedRef<FNiagaraEmitterHandleViewModel>* EmitterViewModel =
			SystemViewModel->GetEmitterHandleViewModels().FindByPredicate(
				[&GraphContext](const TSharedRef<FNiagaraEmitterHandleViewModel>& Candidate)
				{
					return Candidate->GetName().ToString().Equals(GraphContext.EmitterName, ESearchCase::CaseSensitive);
				});
		if (!EmitterViewModel)
		{
			OutError = FString::Printf(TEXT("Niagara emitter stack not found: %s"), *GraphContext.EmitterName);
			return nullptr;
		}
		StackViewModel = (*EmitterViewModel)->GetEmitterStackViewModel();
	}

	UNiagaraStackEntry* RootEntry = StackViewModel ? StackViewModel->GetRootEntry() : nullptr;
	if (!RootEntry)
	{
		OutError = GraphContext.Scope.Equals(TEXT("system"), ESearchCase::IgnoreCase)
			? TEXT("Niagara system stack root not found")
			: FString::Printf(TEXT("Niagara emitter stack root not found: %s"), *GraphContext.EmitterName);
		return nullptr;
	}

	TArray<UNiagaraStackScriptItemGroup*> ScriptGroups;
	RootEntry->GetUnfilteredChildrenOfType(ScriptGroups, false);
	for (UNiagaraStackScriptItemGroup* ScriptGroup : ScriptGroups)
	{
		if (ScriptGroup
			&& ScriptGroup->GetScriptUsage() == GraphContext.Usage
			&& ScriptGroup->GetScriptUsageId() == GraphContext.UsageId)
		{
			return ScriptGroup;
		}
	}

	OutError = FString::Printf(
		TEXT("Niagara script stack not found for emitter '%s', usage '%s', usage id '%s'"),
		*GraphContext.EmitterName,
		*ScriptUsageToString(GraphContext.Usage),
		*GraphContext.UsageId.ToString(EGuidFormats::DigitsWithHyphens));
	return nullptr;
}

static UNiagaraStackModuleItem* FindUsageAwareModule(
	UNiagaraStackScriptItemGroup* ScriptGroup,
	UNiagaraNodeFunctionCall* ModuleNode,
	FString& OutError)
{
	if (!ScriptGroup || !ModuleNode)
	{
		OutError = TEXT("Invalid Niagara stack module lookup");
		return nullptr;
	}

	TArray<UNiagaraStackModuleItem*> ModuleItems;
	ScriptGroup->GetUnfilteredChildrenOfType(ModuleItems, false);
	for (UNiagaraStackModuleItem* ModuleItem : ModuleItems)
	{
		if (ModuleItem && &ModuleItem->GetModuleNode() == ModuleNode)
		{
			return ModuleItem;
		}
	}

	OutError = FString::Printf(
		TEXT("Niagara stack module not found by node GUID: %s"),
		*ModuleNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	return nullptr;
}

// Flatten categories and static-switch/conditional children, but stop at dynamic-input chain
// boundaries. This is the same shape exposed by Niagara's GetModuleTopology endpoint.
static bool ForEachFlattenedStackInput(
	UNiagaraStackEntry* Entry,
	TFunctionRef<bool(UNiagaraStackFunctionInput*)> Visitor)
{
	if (!Entry)
	{
		return true;
	}

	TArray<UNiagaraStackEntry*> Children;
	Entry->GetUnfilteredChildren(Children);
	for (UNiagaraStackEntry* Child : Children)
	{
		if (UNiagaraStackFunctionInput* Input = Cast<UNiagaraStackFunctionInput>(Child))
		{
			if (!Visitor(Input))
			{
				return false;
			}
			if (Input->GetValueMode() != UNiagaraStackFunctionInput::EValueMode::Dynamic
				&& !ForEachFlattenedStackInput(Input, Visitor))
			{
				return false;
			}
		}
		else if (!ForEachFlattenedStackInput(Child, Visitor))
		{
			return false;
		}
	}
	return true;
}

static void PopulateStackInputTopology(
	UNiagaraStackFunctionInput* Input,
	FNiagaraExt_StackInputTopology& OutTopology)
{
	OutTopology.Name = Input->GetInputParameterHandle().GetName();
	OutTopology.Type = Input->GetInputType();
	const bool bHidden = Input->GetIsHidden();
	const bool bVisibleCondition = Input->GetHasVisibleCondition() ? Input->GetVisibleConditionEnabled() : true;
	const bool bEditCondition = Input->GetHasEditCondition() ? Input->GetEditConditionEnabled() : true;
	OutTopology.bIsVisible = !bHidden && bVisibleCondition;
	OutTopology.bIsEditable = OutTopology.bIsVisible && bEditCondition;
	OutTopology.bIsDynamic = Input->GetValueMode() == UNiagaraStackFunctionInput::EValueMode::Dynamic;
	OutTopology.bIsStaticSwitch = Input->IsStaticParameter();
}

struct FStackInputReadback
{
	FNiagaraExt_StackInputTopology Topology;
	TWeakObjectPtr<UNiagaraStackFunctionInput> Input;
};

struct FStackModuleReadback
{
	bool bEnabled = true;
	TArray<FStackInputReadback> Inputs;
};

static void PopulateUsageAwareModuleReadback(
	UNiagaraStackModuleItem* ModuleItem,
	FStackModuleReadback& OutReadback)
{
	OutReadback.bEnabled = ModuleItem->GetIsEnabled();

	TSet<FName> EmittedInlineEditConditions;
	ForEachFlattenedStackInput(ModuleItem, [&](UNiagaraStackFunctionInput* Input)
	{
		if (Input->GetShowEditConditionInline())
		{
			const TOptional<FNiagaraVariable> EditConditionVariable = Input->GetEditConditionVariable();
			if (EditConditionVariable.IsSet() && !EmittedInlineEditConditions.Contains(EditConditionVariable->GetName()))
			{
				EmittedInlineEditConditions.Add(EditConditionVariable->GetName());
				FStackInputReadback& InlineReadback = OutReadback.Inputs.AddDefaulted_GetRef();
				FNiagaraExt_StackInputTopology& InlineTopology = InlineReadback.Topology;
				InlineTopology.Name = EditConditionVariable->GetName();
				InlineTopology.Type = EditConditionVariable->GetType();
				const bool bHostHidden = Input->GetIsHidden();
				const bool bHostVisible = Input->GetHasVisibleCondition() ? Input->GetVisibleConditionEnabled() : true;
				InlineTopology.bIsVisible = !bHostHidden && bHostVisible;
				InlineTopology.bIsEditable = InlineTopology.bIsVisible;
				InlineTopology.bIsDynamic = false;
				InlineTopology.bIsStaticSwitch = EditConditionVariable->GetType().IsStatic();
			}
		}

		FStackInputReadback& InputReadback = OutReadback.Inputs.AddDefaulted_GetRef();
		PopulateStackInputTopology(Input, InputReadback.Topology);
		InputReadback.Input = Input;
		return true;
	});
}

static bool BuildUsageAwareStackTopologies(
	FNiagaraExternalEditContext& ExternalContext,
	const FNiagaraGraphContext& GraphContext,
	TMap<FGuid, FStackModuleReadback>& OutTopologies,
	FString& OutError)
{
	UNiagaraStackScriptItemGroup* ScriptGroup = FindUsageAwareScriptGroup(ExternalContext, GraphContext, OutError);
	if (!ScriptGroup)
	{
		return false;
	}

	TArray<UNiagaraStackModuleItem*> ModuleItems;
	ScriptGroup->GetUnfilteredChildrenOfType(ModuleItems, false);
	for (UNiagaraStackModuleItem* ModuleItem : ModuleItems)
	{
		if (!ModuleItem)
		{
			continue;
		}
		FStackModuleReadback ModuleReadback;
		PopulateUsageAwareModuleReadback(ModuleItem, ModuleReadback);
		OutTopologies.Add(ModuleItem->GetModuleNode().NodeGuid, MoveTemp(ModuleReadback));
	}
	return true;
}

static TSharedPtr<FJsonObject> StackInputTopologyToJson(const FNiagaraExt_StackInputTopology& Input)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("name"), Input.Name.ToString());
	Obj->SetStringField(TEXT("type"), Input.Type.GetName());
	Obj->SetBoolField(TEXT("visible"), Input.bIsVisible);
	Obj->SetBoolField(TEXT("editable"), Input.bIsEditable);
	Obj->SetBoolField(TEXT("dynamic"), Input.bIsDynamic);
	Obj->SetBoolField(TEXT("static_switch"), Input.bIsStaticSwitch);
	return Obj;
}

static FString StackInputValueModeToString(UNiagaraStackFunctionInput::EValueMode Mode)
{
	switch (Mode)
	{
	case UNiagaraStackFunctionInput::EValueMode::Local: return TEXT("local");
	case UNiagaraStackFunctionInput::EValueMode::Linked: return TEXT("linked_parameter");
	case UNiagaraStackFunctionInput::EValueMode::LinkedShared: return TEXT("linked_shared_parameter");
	case UNiagaraStackFunctionInput::EValueMode::Dynamic: return TEXT("dynamic_input");
	case UNiagaraStackFunctionInput::EValueMode::Data: return TEXT("data_interface");
	case UNiagaraStackFunctionInput::EValueMode::ObjectAsset: return TEXT("object_asset");
	case UNiagaraStackFunctionInput::EValueMode::Expression: return TEXT("hlsl");
	case UNiagaraStackFunctionInput::EValueMode::DefaultFunction: return TEXT("default_function");
	case UNiagaraStackFunctionInput::EValueMode::InvalidOverride: return TEXT("invalid_override");
	case UNiagaraStackFunctionInput::EValueMode::UnsupportedDefault: return TEXT("unsupported_default");
	case UNiagaraStackFunctionInput::EValueMode::None: return TEXT("none");
	default: return TEXT("unknown");
	}
}

static void AddStackInputValueReadback(UNiagaraStackFunctionInput* Input, const TSharedPtr<FJsonObject>& Obj, int32 Depth = 0)
{
	if (!Obj.IsValid())
	{
		return;
	}
	if (!Input)
	{
		Obj->SetStringField(TEXT("value_mode"), TEXT("inline_edit_condition"));
		return;
	}

	const UNiagaraStackFunctionInput::EValueMode Mode = Input->GetValueMode();
	Obj->SetStringField(TEXT("value_mode"), StackInputValueModeToString(Mode));
	Obj->SetStringField(TEXT("display_value"), Input->GetCollapsedStateText().ToString());

	switch (Mode)
	{
	case UNiagaraStackFunctionInput::EValueMode::Local:
	{
		const TSharedPtr<const FStructOnScope> LocalValue = Input->GetLocalValueStruct();
		const UScriptStruct* ScriptStruct = LocalValue.IsValid() ? Cast<const UScriptStruct>(LocalValue->GetStruct()) : nullptr;
		if (LocalValue.IsValid() && LocalValue->IsValid() && ScriptStruct)
		{
			Obj->SetStringField(TEXT("value_struct"), ScriptStruct->GetPathName());
			FString ValueText;
			ScriptStruct->ExportText(
				ValueText,
				LocalValue->GetStructMemory(),
				LocalValue->GetStructMemory(),
				nullptr,
				PPF_SerializedAsImportText,
				nullptr);
			Obj->SetStringField(TEXT("value_text"), ValueText);
			if (UEnum* Enum = Input->GetInputType().GetEnum())
			{
				const int32 EnumValue = *reinterpret_cast<const int32*>(LocalValue->GetStructMemory());
				Obj->SetStringField(TEXT("enum_name"), Enum->GetNameStringByValue(EnumValue));
				Obj->SetStringField(TEXT("enum_display_name"), Enum->GetDisplayNameTextByValue(EnumValue).ToString());
			}
		}
		break;
	}
	case UNiagaraStackFunctionInput::EValueMode::Linked:
	case UNiagaraStackFunctionInput::EValueMode::LinkedShared:
	{
		const FNiagaraVariableBase& LinkedValue = Input->GetLinkedParameterValue();
		Obj->SetStringField(TEXT("linked_parameter"), LinkedValue.GetName().ToString());
		Obj->SetStringField(TEXT("linked_parameter_type"), LinkedValue.GetType().GetName());
		break;
	}
	case UNiagaraStackFunctionInput::EValueMode::Dynamic:
	{
		if (UNiagaraNodeFunctionCall* DynamicNode = Input->GetDynamicInputNode())
		{
			Obj->SetStringField(TEXT("dynamic_input_name"), DynamicNode->GetFunctionName());
			Obj->SetStringField(TEXT("dynamic_input_script"), GetObjectPathString(DynamicNode->FunctionScript));
			Obj->SetStringField(TEXT("dynamic_input_node_guid"), DynamicNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		}
		if (Depth < 16)
		{
			TArray<TSharedPtr<FJsonValue>> DynamicInputs;
			ForEachFlattenedStackInput(Input, [&](UNiagaraStackFunctionInput* ChildInput)
			{
				FNiagaraExt_StackInputTopology ChildTopology;
				PopulateStackInputTopology(ChildInput, ChildTopology);
				TSharedPtr<FJsonObject> ChildJson = StackInputTopologyToJson(ChildTopology);
				AddStackInputValueReadback(ChildInput, ChildJson, Depth + 1);
				DynamicInputs.Add(MakeShared<FJsonValueObject>(ChildJson));
				return true;
			});
			Obj->SetArrayField(TEXT("dynamic_inputs"), DynamicInputs);
		}
		break;
	}
	case UNiagaraStackFunctionInput::EValueMode::Data:
	{
		if (UNiagaraDataInterface* DataInterface = Input->GetDataValueObject())
		{
			Obj->SetStringField(TEXT("data_interface_class"), DataInterface->GetClass()->GetPathName());
			Obj->SetStringField(TEXT("data_interface_path"), DataInterface->GetPathName());
			Obj->SetArrayField(TEXT("data_interface_properties"), PropertiesToJson(DataInterface->GetClass(), DataInterface, 256));
		}
		break;
	}
	case UNiagaraStackFunctionInput::EValueMode::ObjectAsset:
		Obj->SetStringField(TEXT("object_asset"), GetObjectPathString(Input->GetObjectAssetValue()));
		break;
	case UNiagaraStackFunctionInput::EValueMode::Expression:
		Obj->SetStringField(TEXT("hlsl"), Input->GetCustomExpressionText().ToString());
		break;
	case UNiagaraStackFunctionInput::EValueMode::DefaultFunction:
	{
		if (UNiagaraNodeFunctionCall* DefaultNode = Input->GetDefaultFunctionNode())
		{
			Obj->SetStringField(TEXT("default_function_name"), DefaultNode->GetFunctionName());
			Obj->SetStringField(TEXT("default_function_script"), GetObjectPathString(DefaultNode->FunctionScript));
		}
		break;
	}
	default:
		break;
	}
}

static TSharedPtr<FJsonObject> GraphNodeToJson(
	UNiagaraNode* Node,
	const FNiagaraGraphContext& GraphContext,
	int32 TraversalIndex,
	bool bIncludePins,
	const FStackModuleReadback* StackTopology)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	if (!Node)
	{
		return Obj;
	}

	Obj->SetNumberField(TEXT("traversal_index"), TraversalIndex);
	Obj->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	Obj->SetStringField(TEXT("node_name"), Node->GetName());
	Obj->SetStringField(TEXT("node_title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
	Obj->SetStringField(TEXT("node_class"), Node->GetClass()->GetPathName());
	Obj->SetNumberField(TEXT("node_pos_x"), Node->NodePosX);
	Obj->SetNumberField(TEXT("node_pos_y"), Node->NodePosY);
	Obj->SetStringField(TEXT("referenced_asset"), GetObjectPathString(Node->GetReferencedAsset()));
	TSharedPtr<FJsonObject> Reference = MakeShared<FJsonObject>();
	Reference->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	TSharedPtr<FJsonObject> GraphReference = MakeShared<FJsonObject>();
	GraphReference->SetStringField(TEXT("scope"), GraphContext.Scope);
	GraphReference->SetStringField(TEXT("emitter"), GraphContext.EmitterName);
	GraphReference->SetStringField(TEXT("script_usage"), ScriptUsageToString(GraphContext.Usage));
	GraphReference->SetStringField(TEXT("usage_id"), GraphContext.UsageId.ToString(EGuidFormats::DigitsWithHyphens));
	GraphReference->SetStringField(TEXT("output_node_guid"), GraphContext.OutputNode ? GraphContext.OutputNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) : TEXT(""));
	Reference->SetObjectField(TEXT("graph"), GraphReference);
	Obj->SetObjectField(TEXT("reference"), Reference);

	if (UNiagaraNodeFunctionCall* FunctionNode = Cast<UNiagaraNodeFunctionCall>(Node))
	{
		Obj->SetStringField(TEXT("kind"), TEXT("function_call"));
		Obj->SetStringField(TEXT("function_name"), FunctionNode->GetFunctionName());
		Obj->SetStringField(TEXT("function_script"), GetObjectPathString(FunctionNode->FunctionScript));
		Obj->SetStringField(TEXT("function_script_asset_object_path"), FunctionNode->FunctionScriptAssetObjectPath.ToString());
		Obj->SetStringField(TEXT("signature_name"), FunctionNode->Signature.Name.ToString());
		Obj->SetBoolField(TEXT("is_stack_module"), StackTopology != nullptr);
		if (StackTopology)
		{
			Obj->SetBoolField(TEXT("stack_enabled"), StackTopology->bEnabled);
			TArray<TSharedPtr<FJsonValue>> StackInputs;
			for (const FStackInputReadback& InputReadback : StackTopology->Inputs)
			{
				TSharedPtr<FJsonObject> InputJson = StackInputTopologyToJson(InputReadback.Topology);
				AddStackInputValueReadback(InputReadback.Input.Get(), InputJson);
				StackInputs.Add(MakeShared<FJsonValueObject>(InputJson));
			}
			Obj->SetArrayField(TEXT("stack_inputs"), StackInputs);
		}
	}
	else if (UNiagaraNodeOutput* OutputNode = Cast<UNiagaraNodeOutput>(Node))
	{
		Obj->SetStringField(TEXT("kind"), TEXT("output"));
		Obj->SetStringField(TEXT("script_usage"), ScriptUsageToString(OutputNode->GetUsage()));
		Obj->SetStringField(TEXT("usage_id"), OutputNode->GetUsageId().ToString(EGuidFormats::DigitsWithHyphens));
	}
	else
	{
		Obj->SetStringField(TEXT("kind"), TEXT("node"));
	}

	if (bIncludePins)
	{
		Obj->SetArrayField(TEXT("pins"), PinsToJson(Node->Pins));
	}
	return Obj;
}

static TSharedPtr<FJsonObject> GraphContextToJson(
	UNiagaraSystem* System,
	const FNiagaraGraphContext& Context,
	FNiagaraExternalEditContext* StackExternalContext,
	bool bIncludeNodes,
	bool bIncludePins,
	bool bIncludeStackInputs,
	int32 MaxNodes)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("scope"), Context.Scope);
	Obj->SetStringField(TEXT("emitter"), Context.EmitterName);
	Obj->SetStringField(TEXT("script_usage"), ScriptUsageToString(Context.Usage));
	Obj->SetStringField(TEXT("usage_id"), Context.UsageId.ToString(EGuidFormats::DigitsWithHyphens));
	Obj->SetStringField(TEXT("graph"), GetObjectPathString(Context.Graph));
	Obj->SetStringField(TEXT("output_node_guid"), Context.OutputNode ? Context.OutputNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) : TEXT(""));

	TArray<UNiagaraNode*> Nodes;
	BuildContextTraversal(Context, Nodes);
	Obj->SetNumberField(TEXT("node_count"), Nodes.Num());
	if (bIncludeNodes)
	{
		TMap<FGuid, FStackModuleReadback> StackModules;
		if (bIncludeStackInputs && System && StackExternalContext)
		{
			FString TopologyError;
			if (!BuildUsageAwareStackTopologies(*StackExternalContext, Context, StackModules, TopologyError))
			{
				Obj->SetStringField(TEXT("stack_topology_error"), TopologyError);
			}
		}

		TArray<TSharedPtr<FJsonValue>> NodeValues;
		const int32 Count = MaxNodes > 0 ? FMath::Min(MaxNodes, Nodes.Num()) : Nodes.Num();
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const UNiagaraNodeFunctionCall* FunctionNode = Cast<UNiagaraNodeFunctionCall>(Nodes[Index]);
			const FStackModuleReadback* StackTopology = nullptr;
			if (FunctionNode)
			{
				StackTopology = StackModules.Find(FunctionNode->NodeGuid);
			}
			NodeValues.Add(MakeShared<FJsonValueObject>(GraphNodeToJson(Nodes[Index], Context, Index, bIncludePins, StackTopology)));
		}
		Obj->SetNumberField(TEXT("returned_node_count"), NodeValues.Num());
		Obj->SetBoolField(TEXT("truncated"), Count < Nodes.Num());
		Obj->SetArrayField(TEXT("nodes"), NodeValues);
	}
	return Obj;
}

static UNiagaraNode* FindNodeByGuid(UNiagaraSystem* System, const FGuid& NodeGuid, FString& OutError)
{
	TArray<FNiagaraGraphContext> Contexts;
	CollectGraphContexts(System, Contexts);
	TSet<UNiagaraGraph*> VisitedGraphs;
	UNiagaraNode* Match = nullptr;
	for (const FNiagaraGraphContext& Context : Contexts)
	{
		if (!Context.Graph || VisitedGraphs.Contains(Context.Graph))
		{
			continue;
		}
		VisitedGraphs.Add(Context.Graph);
		for (UEdGraphNode* GraphNode : Context.Graph->Nodes)
		{
			UNiagaraNode* NiagaraNode = Cast<UNiagaraNode>(GraphNode);
			if (!NiagaraNode || NiagaraNode->NodeGuid != NodeGuid)
			{
				continue;
			}
			if (Match && Match != NiagaraNode)
			{
				OutError = FString::Printf(TEXT("Node GUID is not unique in this system: %s"), *NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
				return nullptr;
			}
			Match = NiagaraNode;
		}
	}
	if (!Match)
	{
		OutError = FString::Printf(TEXT("Niagara node not found: %s"), *NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	}
	return Match;
}

static bool HasGraphSelector(const TSharedPtr<FJsonObject>& Reference)
{
	if (!Reference.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Graph = nullptr;
	if (Reference->TryGetObjectField(TEXT("graph"), Graph) && Graph && Graph->IsValid())
	{
		return true;
	}
	return Reference->HasField(TEXT("scope"))
		|| Reference->HasField(TEXT("emitter"))
		|| Reference->HasField(TEXT("emitter_name"))
		|| Reference->HasField(TEXT("script_usage"))
		|| Reference->HasField(TEXT("usage"))
		|| Reference->HasField(TEXT("usage_id"))
		|| Reference->HasField(TEXT("output_node_guid"));
}

static UNiagaraNode* FindNodeByGuidInSelectedGraphs(
	UNiagaraSystem* System,
	const FGuid& NodeGuid,
	const TSharedPtr<FJsonObject>& Reference,
	FString& OutError)
{
	TArray<FNiagaraGraphContext> Contexts;
	if (!SelectGraphContexts(System, GetGraphSelector(Reference), Contexts, OutError, false))
	{
		return nullptr;
	}

	TSet<UNiagaraNode*> Matches;
	TSet<UNiagaraGraph*> VisitedGraphs;
	for (const FNiagaraGraphContext& Context : Contexts)
	{
		if (!Context.Graph || VisitedGraphs.Contains(Context.Graph))
		{
			continue;
		}
		VisitedGraphs.Add(Context.Graph);
		for (UEdGraphNode* GraphNode : Context.Graph->Nodes)
		{
			UNiagaraNode* Node = Cast<UNiagaraNode>(GraphNode);
			if (Node && Node->NodeGuid == NodeGuid)
			{
				Matches.Add(Node);
			}
		}
	}
	if (Matches.Num() != 1)
	{
		OutError = FString::Printf(TEXT("Niagara node GUID resolved to %d nodes in the selected graph context: %s"),
			Matches.Num(),
			*NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		return nullptr;
	}
	return Matches.Array()[0];
}

static UNiagaraNode* ResolveNodeReference(
	UNiagaraSystem* System,
	const TSharedPtr<FJsonObject>& Reference,
	const TMap<FString, FCreatedNodeRecord>& CreatedNodes,
	FString& OutError)
{
	if (!Reference.IsValid())
	{
		OutError = TEXT("Node reference must be an object");
		return nullptr;
	}

	const TSharedPtr<FJsonObject>* NestedNode = nullptr;
	if (Reference->TryGetObjectField(TEXT("node"), NestedNode) && NestedNode && NestedNode->IsValid())
	{
		return ResolveNodeReference(System, *NestedNode, CreatedNodes, OutError);
	}

	FString OperationId;
	Reference->TryGetStringField(TEXT("operation"), OperationId);
	Reference->TryGetStringField(TEXT("operation_id"), OperationId);
	if (!OperationId.IsEmpty())
	{
		if (const FCreatedNodeRecord* Created = CreatedNodes.Find(OperationId))
		{
			return Created->Node;
		}
		OutError = FString::Printf(TEXT("Created-node operation not found (operations can only reference earlier inserts): %s"), *OperationId);
		return nullptr;
	}

	FGuid NodeGuid;
	if (!ParseGuidField(Reference, TEXT("node_guid"), NodeGuid))
	{
		OutError = TEXT("Node reference requires node_guid or operation");
		return nullptr;
	}
	return HasGraphSelector(Reference)
		? FindNodeByGuidInSelectedGraphs(System, NodeGuid, Reference, OutError)
		: FindNodeByGuid(System, NodeGuid, OutError);
}

static UEdGraphPin* ResolvePinReference(
	UNiagaraSystem* System,
	const TSharedPtr<FJsonObject>& Reference,
	const TMap<FString, FCreatedNodeRecord>& CreatedNodes,
	FString& OutError)
{
	UNiagaraNode* Node = ResolveNodeReference(System, Reference, CreatedNodes, OutError);
	if (!Node)
	{
		return nullptr;
	}

	FString PinName;
	FString Direction;
	FGuid PinId;
	FGuid PersistentGuid;
	Reference->TryGetStringField(TEXT("pin"), PinName);
	Reference->TryGetStringField(TEXT("pin_name"), PinName);
	Reference->TryGetStringField(TEXT("direction"), Direction);
	const bool bHasPinId = ParseGuidField(Reference, TEXT("pin_id"), PinId);
	const bool bHasPersistentGuid = ParseGuidField(Reference, TEXT("persistent_guid"), PersistentGuid);
	if (PinName.IsEmpty() && !bHasPinId && !bHasPersistentGuid)
	{
		OutError = TEXT("Pin reference requires pin/pin_name, pin_id, or persistent_guid");
		return nullptr;
	}

	TArray<UEdGraphPin*> Matches;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin)
		{
			continue;
		}
		if (bHasPinId && Pin->PinId != PinId)
		{
			continue;
		}
		if (bHasPersistentGuid && Pin->PersistentGuid != PersistentGuid)
		{
			continue;
		}
		if (!PinName.IsEmpty() && !Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase) && !Pin->GetDisplayName().ToString().Equals(PinName, ESearchCase::IgnoreCase))
		{
			continue;
		}
		if (!Direction.IsEmpty() && !PinDirectionToString(Pin->Direction).Equals(Direction, ESearchCase::IgnoreCase))
		{
			continue;
		}
		Matches.Add(Pin);
	}

	if (Matches.Num() != 1)
	{
		OutError = FString::Printf(TEXT("Pin reference resolved to %d pins on node %s; add direction or pin_id to disambiguate"), Matches.Num(), *Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		return nullptr;
	}
	return Matches[0];
}

static bool FindContextForNode(UNiagaraSystem* System, UNiagaraNode* Node, FNiagaraGraphContext& OutContext, FString& OutError)
{
	TArray<FNiagaraGraphContext> Contexts;
	CollectGraphContexts(System, Contexts);
	TArray<FNiagaraGraphContext> Matches;
	for (const FNiagaraGraphContext& Context : Contexts)
	{
		if (Context.Graph != Node->GetGraph())
		{
			continue;
		}
		TArray<UNiagaraNode*> Traversal;
		BuildContextTraversal(Context, Traversal);
		if (Traversal.Contains(Node))
		{
			Matches.Add(Context);
		}
	}
	if (Matches.Num() != 1)
	{
		OutError = FString::Printf(TEXT("Node belongs to %d script traversals; set_stack_input requires an unambiguous stack node"), Matches.Num());
		return false;
	}
	OutContext = Matches[0];
	return true;
}

static bool ResolveContextForNodeReference(
	UNiagaraSystem* System,
	UNiagaraNode* Node,
	const TSharedPtr<FJsonObject>& NodeReference,
	FNiagaraGraphContext& OutContext,
	FString& OutError)
{
	if (!HasGraphSelector(NodeReference))
	{
		return FindContextForNode(System, Node, OutContext, OutError);
	}

	TArray<FNiagaraGraphContext> Contexts;
	if (!SelectGraphContexts(System, GetGraphSelector(NodeReference), Contexts, OutError, true))
	{
		return false;
	}
	const FNiagaraGraphContext& SelectedContext = Contexts[0];
	TArray<UNiagaraNode*> Traversal;
	BuildContextTraversal(SelectedContext, Traversal);
	if (SelectedContext.Graph != Node->GetGraph() || !Traversal.Contains(Node))
	{
		OutError = FString::Printf(
			TEXT("Node %s does not belong to the selected Niagara script traversal"),
			*Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		return false;
	}
	OutContext = SelectedContext;
	return true;
}

static FString JoinContextErrors(const TArray<FText>& Errors)
{
	TArray<FString> Lines;
	for (const FText& Error : Errors)
	{
		Lines.Add(Error.ToString());
	}
	return FString::Join(Lines, TEXT("\n"));
}

static bool ApplyJsonProperties(UObject* Object, const TSharedPtr<FJsonObject>& Properties, FString& OutError)
{
	if (!Object)
	{
		OutError = TEXT("Invalid property target object");
		return false;
	}
	if (!Properties.IsValid())
	{
		return true;
	}

	Object->Modify();
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
	{
		FProperty* Property = Object->GetClass()->FindPropertyByName(*Pair.Key);
		if (!Property)
		{
			OutError = FString::Printf(TEXT("Property not found on %s: %s"), *Object->GetClass()->GetName(), *Pair.Key);
			return false;
		}

		if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			FString ObjectPath;
			if (Pair.Value.IsValid() && Pair.Value->Type == EJson::String)
			{
				ObjectPath = Pair.Value->AsString();
			}
			else if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject> ValueObject = Pair.Value->AsObject();
				if (ValueObject.IsValid())
				{
					ValueObject->TryGetStringField(TEXT("object_path"), ObjectPath);
					ValueObject->TryGetStringField(TEXT("path"), ObjectPath);
				}
			}
			else if (!Pair.Value.IsValid() || Pair.Value->Type == EJson::Null)
			{
				ObjectPath.Reset();
			}
			else
			{
				OutError = FString::Printf(TEXT("Object property %s requires an object path string or null"), *Pair.Key);
				return false;
			}

			UObject* Value = nullptr;
			if (!ObjectPath.IsEmpty())
			{
				Value = StaticLoadObject(ObjectProperty->PropertyClass, nullptr, *NormalizeObjectPath(ObjectPath));
				if (!Value)
				{
					OutError = FString::Printf(TEXT("Failed to load %s for property %s: %s"), *ObjectProperty->PropertyClass->GetName(), *Pair.Key, *ObjectPath);
					return false;
				}
			}
			ObjectProperty->SetObjectPropertyValue_InContainer(Object, Value);
			continue;
		}

		FString ValueText;
		if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> ValueObject = Pair.Value->AsObject();
			if (!ValueObject.IsValid() || !ValueObject->TryGetStringField(TEXT("value_text"), ValueText))
			{
				OutError = FString::Printf(TEXT("Complex property %s requires {\"value_text\": \"...\"}"), *Pair.Key);
				return false;
			}
		}
		else
		{
			ValueText = JsonValueToImportText(Pair.Value);
		}
		if (ValueText.IsEmpty() && (!Pair.Value.IsValid() || Pair.Value->Type != EJson::String))
		{
			OutError = FString::Printf(TEXT("Property %s requires a scalar or value_text"), *Pair.Key);
			return false;
		}

		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
		if (!Property->ImportText_Direct(*ValueText, ValuePtr, Object, PPF_None))
		{
			OutError = FString::Printf(TEXT("Failed to import '%s' into %s.%s"), *ValueText, *Object->GetClass()->GetName(), *Pair.Key);
			return false;
		}
	}
	Object->PostEditChange();
	return true;
}

static FString StableFingerprint(const TArray<FString>& Values)
{
	TArray<FString> Sorted = Values;
	Sorted.Sort();
	const FString Canonical = FString::Join(Sorted, TEXT("\n"));
	FTCHARToUTF8 Utf8(*Canonical);
	FMD5 Md5;
	Md5.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	uint8 Digest[16];
	Md5.Final(Digest);
	return BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
}

struct FNiagaraPreservationState
{
	TArray<FString> SystemSettings;
	TArray<FString> Emitters;
	TArray<FString> EmitterSettings;
	TArray<FString> Renderers;
	TArray<FString> RendererProperties;
	TArray<FString> UserParameters;
	TArray<FString> RapidIterationParameters;
	TArray<FString> SimTargets;
	TArray<FString> EventHandlers;
	TArray<FString> Modules;
	TArray<FString> GraphNodes;
	TArray<FString> GraphPins;
	TArray<FString> Edges;
};

static FString ParameterValueSignature(const FNiagaraParameterStore& Store, const FNiagaraVariableWithOffset& StoredVariable)
{
	const FNiagaraVariable Variable(StoredVariable.GetType(), StoredVariable.GetName());
	if (Variable.GetType().IsDataInterface())
	{
		UNiagaraDataInterface* DataInterface = Store.GetDataInterface(Variable);
		return DataInterface
			? DataInterface->GetClass()->GetPathName() + TEXT(":") + ToJsonString(PropertiesToJson(DataInterface->GetClass(), DataInterface, 0))
			: TEXT("null_data_interface");
	}
	if (Variable.GetType().IsUObject())
	{
		return GetObjectPathString(Store.GetUObject(Variable).Get());
	}

	const int32 Size = Variable.GetSizeInBytes();
	if (Size <= 0)
	{
		return TEXT("empty");
	}
	TArray<uint8> Bytes;
	Bytes.SetNumZeroed(Size);
	if (!Store.CopyParameterData(Variable, Bytes.GetData()))
	{
		return TEXT("unreadable");
	}
	return BytesToHex(Bytes.GetData(), Bytes.Num());
}

static FString ExportNamedProperty(UStruct* StructType, const void* Container, const TCHAR* PropertyName)
{
	if (!StructType || !Container)
	{
		return TEXT("<invalid>");
	}
	FProperty* Property = StructType->FindPropertyByName(PropertyName);
	return Property ? ExportPropertyText(Property, Container) : TEXT("<missing>");
}

static FString RendererPropertyValueForPreservation(FProperty* Property, const void* Renderer)
{
	if (!Property || !Renderer)
	{
		return TEXT("<invalid>");
	}

	const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
	if (StructProperty && StructProperty->Struct == FNiagaraVariableAttributeBinding::StaticStruct())
	{
		const FNiagaraVariableAttributeBinding* Binding = StructProperty->ContainerPtrToValuePtr<FNiagaraVariableAttributeBinding>(Renderer);
		if (!Binding)
		{
			return TEXT("<invalid_binding>");
		}
		// CacheValues intentionally rewrites ParamMapVariable/DataSetName/display/cache flags
		// during compilation. RootName, type, and source mode are the authored binding.
		return FString::Printf(TEXT("root=%s|type=%s|source_mode=%d"),
			*ExportNamedProperty(FNiagaraVariableAttributeBinding::StaticStruct(), Binding, TEXT("RootName")),
			*Binding->GetType().GetName(),
			static_cast<int32>(Binding->GetBindingSourceMode()));
	}

	if (StructProperty && StructProperty->Struct && StructProperty->Struct->GetFName() == TEXT("NiagaraMaterialAttributeBinding"))
	{
		const void* Binding = StructProperty->ContainerPtrToValuePtr<void>(Renderer);
		return FString::Printf(TEXT("material_parameter=%s|variable=%s|child_variable=%s"),
			*ExportNamedProperty(StructProperty->Struct, Binding, TEXT("MaterialParameterName")),
			*ExportNamedProperty(StructProperty->Struct, Binding, TEXT("NiagaraVariable")),
			*ExportNamedProperty(StructProperty->Struct, Binding, TEXT("NiagaraChildVariable")));
	}

	return ExportPropertyText(Property, Renderer);
}

static void CaptureRapidIterationParameters(const FString& OwnerKey, const UNiagaraScript* Script, TArray<FString>& OutParameters)
{
	if (!Script)
	{
		return;
	}
	const FString ScriptKey = FString::Printf(TEXT("%s|%s|%s|%s"),
		*OwnerKey,
		*ScriptUsageToString(Script->GetUsage()),
		*Script->GetUsageId().ToString(EGuidFormats::DigitsWithHyphens),
		*Script->GetPathName());
	const FNiagaraParameterStore& Store = Script->RapidIterationParameters;
	for (const FNiagaraVariableWithOffset& Variable : Store.ReadParameterVariables())
	{
		OutParameters.Add(ScriptKey + TEXT("|") + Variable.GetName().ToString() + TEXT("|")
			+ Variable.GetType().GetName() + TEXT("|") + ParameterValueSignature(Store, Variable));
	}
}

static void CapturePreservationState(UNiagaraSystem* System, FNiagaraPreservationState& OutState)
{
	OutState = FNiagaraPreservationState();
	if (!System)
	{
		return;
	}

	OutState.SystemSettings.Add(FString::Printf(TEXT("fixed=%s|bounds=%s|determinism=%d|seed=%d|warmup_time=%.9g|warmup_ticks=%d|warmup_delta=%.9g"),
		*ExportNamedProperty(UNiagaraSystem::StaticClass(), System, TEXT("bFixedBounds")),
		*System->GetFixedBounds().ToString(),
		System->NeedsDeterminism() ? 1 : 0,
		System->GetRandomSeed(),
		System->GetWarmupTime(),
		System->GetWarmupTickCount(),
		System->GetWarmupTickDelta()));
	CaptureRapidIterationParameters(TEXT("system"), System->GetSystemSpawnScript(), OutState.RapidIterationParameters);
	CaptureRapidIterationParameters(TEXT("system"), System->GetSystemUpdateScript(), OutState.RapidIterationParameters);

	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		const FVersionedNiagaraEmitter Instance = Handle.GetInstance();
		OutState.Emitters.Add(FString::Printf(TEXT("%s|%s|%d|%s"),
			*Handle.GetId().ToString(EGuidFormats::DigitsWithHyphens),
			*Handle.GetName().ToString(),
			Handle.GetIsEnabled() ? 1 : 0,
			*GetObjectPathString(Instance.Emitter)));

		FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
		if (!EmitterData)
		{
			continue;
		}
		const FString EmitterKey = Handle.GetId().ToString(EGuidFormats::DigitsWithHyphens);
		OutState.EmitterSettings.Add(FString::Printf(
			TEXT("%s|local=%d|determinism=%d|seed=%d|interpolated=%d|importance=%d|bounds_mode=%d|bounds=%s|persistent_ids=%d|max_gpu_spawn=%d|allocation_mode=%d|preallocation=%d|dependencies=%s|platforms=%s|scalability=%s"),
			*EmitterKey,
			EmitterData->bLocalSpace ? 1 : 0,
			EmitterData->bDeterminism ? 1 : 0,
			EmitterData->RandomSeed,
			static_cast<int32>(EmitterData->InterpolatedSpawnMode),
			static_cast<int32>(EmitterData->Importance),
			static_cast<int32>(EmitterData->CalculateBoundsMode),
			*EmitterData->FixedBounds.ToString(),
			EmitterData->bRequiresPersistentIDs ? 1 : 0,
			EmitterData->MaxGPUParticlesSpawnPerFrame,
			static_cast<int32>(EmitterData->AllocationMode),
			EmitterData->PreAllocationCount,
			*ExportNamedProperty(FVersionedNiagaraEmitterData::StaticStruct(), EmitterData, TEXT("EmitterDependencies")),
			*ExportNamedProperty(FVersionedNiagaraEmitterData::StaticStruct(), EmitterData, TEXT("Platforms")),
			*ExportNamedProperty(FVersionedNiagaraEmitterData::StaticStruct(), EmitterData, TEXT("ScalabilityOverrides"))));
		OutState.SimTargets.Add(Handle.GetId().ToString(EGuidFormats::DigitsWithHyphens) + TEXT("|") + EnumToStringSafe(StaticEnum<ENiagaraSimTarget>(), static_cast<int64>(EmitterData->SimTarget)));
		TArray<UNiagaraScript*> EmitterScripts;
		EmitterData->GetScripts(EmitterScripts, false, false);
		for (const UNiagaraScript* Script : EmitterScripts)
		{
			CaptureRapidIterationParameters(EmitterKey, Script, OutState.RapidIterationParameters);
		}

		const TArray<UNiagaraRendererProperties*>& RendererList = EmitterData->GetRenderers();
		for (int32 RendererIndex = 0; RendererIndex < RendererList.Num(); ++RendererIndex)
		{
			UNiagaraRendererProperties* Renderer = RendererList[RendererIndex];
			if (!Renderer)
			{
				OutState.Renderers.Add(FString::Printf(TEXT("%s|%d|null"), *Handle.GetId().ToString(), RendererIndex));
				continue;
			}
			TArray<UMaterialInterface*> Materials;
			Renderer->GetUsedMaterials(nullptr, Materials);
			TArray<FString> MaterialPaths;
			for (UMaterialInterface* Material : Materials)
			{
				MaterialPaths.Add(GetObjectPathString(Material));
			}
			const FString RendererKey = FString::Printf(TEXT("%s|%d|%s"),
				*Handle.GetId().ToString(EGuidFormats::DigitsWithHyphens),
				RendererIndex,
				*Renderer->GetClass()->GetPathName());
			OutState.Renderers.Add(FString::Printf(TEXT("%s|%d|%s"),
				*RendererKey,
				Renderer->GetIsEnabled() ? 1 : 0,
				*FString::Join(MaterialPaths, TEXT(","))));

			const EPropertyFlags NonSemanticFlags = CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient | CPF_SkipSerialization;
			for (TFieldIterator<FProperty> It(Renderer->GetClass()); It; ++It)
			{
				FProperty* Property = *It;
				if (!Property || Property->HasAnyPropertyFlags(NonSemanticFlags))
				{
					continue;
				}
				TArray<FString> PropertyValue;
				PropertyValue.Add(RendererPropertyValueForPreservation(Property, Renderer));
				OutState.RendererProperties.Add(RendererKey + TEXT("|") + Property->GetName()
					+ TEXT("|") + Property->GetCPPType() + TEXT("|") + StableFingerprint(PropertyValue));
			}
		}

		for (const FNiagaraEventScriptProperties& EventHandler : EmitterData->GetEventHandlers())
		{
			OutState.EventHandlers.Add(FString::Printf(TEXT("%s|%s|%s|%s|%d|%u|%u|%d|%u|%d"),
				*Handle.GetId().ToString(EGuidFormats::DigitsWithHyphens),
				*EventHandler.SourceEmitterID.ToString(EGuidFormats::DigitsWithHyphens),
				*EventHandler.SourceEventName.ToString(),
				*FString::Printf(TEXT("%d"), static_cast<int32>(EventHandler.ExecutionMode)),
				EventHandler.bRandomSpawnNumber ? 1 : 0,
				EventHandler.SpawnNumber,
				EventHandler.MinSpawnNumber,
				EventHandler.UpdateAttributeInitialValues ? 1 : 0,
				EventHandler.MaxEventsPerFrame,
				EventHandler.Script ? static_cast<int32>(EventHandler.Script->GetUsage()) : -1));
		}
	}

	const FNiagaraParameterStore& Store = System->GetExposedParameters();
	for (const FNiagaraVariableWithOffset& Variable : Store.ReadParameterVariables())
	{
		OutState.UserParameters.Add(Variable.GetName().ToString() + TEXT("|") + Variable.GetType().GetName() + TEXT("|") + ParameterValueSignature(Store, Variable));
	}

	TArray<FNiagaraModuleRecord> ModuleRecords;
	CollectModules(System, ModuleRecords);
	TSet<FString> UniqueModules;
	for (const FNiagaraModuleRecord& Record : ModuleRecords)
	{
		if (!Record.Node)
		{
			continue;
		}
		UniqueModules.Add(Record.Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens)
			+ TEXT("|") + GetObjectPathString(Record.Node->FunctionScript)
			+ TEXT("|") + Record.Node->FunctionScriptAssetObjectPath.ToString()
			+ TEXT("|enabled=") + FString::FromInt(static_cast<int32>(Record.Node->GetDesiredEnabledState()))
			+ TEXT("|user_set=") + FString::FromInt(Record.Node->HasUserSetTheEnabledState() ? 1 : 0));
	}
	OutState.Modules = UniqueModules.Array();

	TArray<FNiagaraGraphContext> Contexts;
	CollectGraphContexts(System, Contexts);
	TSet<UNiagaraGraph*> VisitedGraphs;
	TSet<FString> UniqueNodes;
	TSet<FString> UniquePins;
	TSet<FString> UniqueEdges;
	for (const FNiagaraGraphContext& Context : Contexts)
	{
		if (!Context.Graph || VisitedGraphs.Contains(Context.Graph))
		{
			continue;
		}
		VisitedGraphs.Add(Context.Graph);
		for (UEdGraphNode* Node : Context.Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}
			const FString NodeGuid = Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
			FString NodeSignature = NodeGuid + TEXT("|") + Node->GetClass()->GetPathName()
				+ TEXT("|") + GetObjectPathString(Cast<UNiagaraNode>(Node) ? CastChecked<UNiagaraNode>(Node)->GetReferencedAsset() : nullptr);
			if (const UNiagaraNodeFunctionCall* FunctionNode = Cast<UNiagaraNodeFunctionCall>(Node))
			{
				NodeSignature += TEXT("|") + FunctionNode->GetFunctionName()
					+ TEXT("|") + GetObjectPathString(FunctionNode->FunctionScript)
					+ TEXT("|") + FunctionNode->FunctionScriptAssetObjectPath.ToString()
					+ TEXT("|enabled=") + FString::FromInt(static_cast<int32>(FunctionNode->GetDesiredEnabledState()))
					+ TEXT("|user_set=") + FString::FromInt(FunctionNode->HasUserSetTheEnabledState() ? 1 : 0);
			}
			UniqueNodes.Add(NodeSignature);
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin)
				{
					continue;
				}
				// Niagara reconstructs anonymous unlinked literal pins while duplicating some module
				// graphs (and can toggle their hidden flag). They are implementation cache, not an authored input. Named or linked
				// pins remain part of the preservation fingerprint.
				if (!(Pin->PinName.IsNone() && Pin->LinkedTo.IsEmpty()))
				{
					UniquePins.Add(NodeGuid
						+ TEXT("|") + (Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out"))
						+ TEXT("|") + Pin->PinName.ToString()
						+ TEXT("|") + Pin->PersistentGuid.ToString(EGuidFormats::DigitsWithHyphens)
						+ TEXT("|default=") + Pin->DefaultValue
						+ TEXT("|object=") + GetObjectPathString(Pin->DefaultObject)
						+ TEXT("|hidden=") + FString::FromInt(Pin->bHidden ? 1 : 0)
						+ TEXT("|orphaned=") + FString::FromInt(Pin->bOrphanedPin ? 1 : 0));
				}
				if (Pin->Direction != EGPD_Output)
				{
					continue;
				}
				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					UEdGraphNode* LinkedNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
					if (!LinkedNode)
					{
						continue;
					}
					UniqueEdges.Add(Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) + TEXT(".") + Pin->PinName.ToString()
						+ TEXT("->") + LinkedNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) + TEXT(".") + LinkedPin->PinName.ToString());
				}
			}
		}
	}
	OutState.GraphNodes = UniqueNodes.Array();
	OutState.GraphPins = UniquePins.Array();
	OutState.Edges = UniqueEdges.Array();

	OutState.SystemSettings.Sort();
	OutState.Emitters.Sort();
	OutState.EmitterSettings.Sort();
	OutState.Renderers.Sort();
	OutState.RendererProperties.Sort();
	OutState.UserParameters.Sort();
	OutState.RapidIterationParameters.Sort();
	OutState.SimTargets.Sort();
	OutState.EventHandlers.Sort();
	OutState.Modules.Sort();
	OutState.GraphNodes.Sort();
	OutState.GraphPins.Sort();
	OutState.Edges.Sort();
}

static void NormalizePreservationStateForSystem(UNiagaraSystem* System, FNiagaraPreservationState& State)
{
	if (!System)
	{
		return;
	}

	const FString ObjectPath = System->GetPathName();
	const FString PackagePath = System->GetOutermost() ? System->GetOutermost()->GetName() : FString();
	auto NormalizeArray = [&ObjectPath, &PackagePath](TArray<FString>& Values)
	{
		for (FString& Value : Values)
		{
			if (!ObjectPath.IsEmpty())
			{
				Value.ReplaceInline(*ObjectPath, TEXT("<SYSTEM>"), ESearchCase::CaseSensitive);
			}
			if (!PackagePath.IsEmpty())
			{
				Value.ReplaceInline(*PackagePath, TEXT("<PACKAGE>"), ESearchCase::CaseSensitive);
			}
		}
		Values.Sort();
	};

	NormalizeArray(State.SystemSettings);
	NormalizeArray(State.Emitters);
	NormalizeArray(State.EmitterSettings);
	NormalizeArray(State.Renderers);
	NormalizeArray(State.RendererProperties);
	NormalizeArray(State.UserParameters);
	NormalizeArray(State.RapidIterationParameters);
	NormalizeArray(State.SimTargets);
	NormalizeArray(State.EventHandlers);
	NormalizeArray(State.Modules);
	NormalizeArray(State.GraphNodes);
	NormalizeArray(State.GraphPins);
	NormalizeArray(State.Edges);
}

static bool ArePreservationStatesExact(const FNiagaraPreservationState& A, const FNiagaraPreservationState& B)
{
	return A.SystemSettings == B.SystemSettings
		&& A.Emitters == B.Emitters
		&& A.EmitterSettings == B.EmitterSettings
		&& A.Renderers == B.Renderers
		&& A.RendererProperties == B.RendererProperties
		&& A.UserParameters == B.UserParameters
		&& A.RapidIterationParameters == B.RapidIterationParameters
		&& A.SimTargets == B.SimTargets
		&& A.EventHandlers == B.EventHandlers
		&& A.Modules == B.Modules
		&& A.GraphNodes == B.GraphNodes
		&& A.GraphPins == B.GraphPins
		&& A.Edges == B.Edges;
}

static bool IsSubsetWithMissing(const TArray<FString>& Before, const TArray<FString>& After, TArray<FString>& OutMissing)
{
	TSet<FString> AfterSet;
	for (const FString& Value : After)
	{
		AfterSet.Add(Value);
	}
	for (const FString& Value : Before)
	{
		if (!AfterSet.Contains(Value))
		{
			OutMissing.Add(Value);
		}
	}
	return OutMissing.IsEmpty();
}

static TArray<TSharedPtr<FJsonValue>> StringsToJson(const TArray<FString>& Values, int32 Limit = 100)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	const int32 Count = Limit > 0 ? FMath::Min(Limit, Values.Num()) : Values.Num();
	for (int32 Index = 0; Index < Count; ++Index)
	{
		Result.Add(MakeShared<FJsonValueString>(Values[Index]));
	}
	return Result;
}

static void ReadStringSetField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TSet<FString>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Values) || !Values)
	{
		return;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		FString Text;
		if (Value.IsValid() && Value->TryGetString(Text) && !Text.IsEmpty())
		{
			OutValues.Add(Text);
		}
	}
}

static bool ValidateRemovedEdgePolicy(
	const FNiagaraPreservationState& Before,
	const FNiagaraPreservationState& After,
	const TSharedPtr<FJsonObject>& Options,
	TArray<FString>& OutRemovedEdges,
	TArray<FString>& OutUnexpectedRemovedEdges)
{
	TSet<FString> AfterEdges;
	for (const FString& Edge : After.Edges)
	{
		AfterEdges.Add(Edge);
	}
	for (const FString& Edge : Before.Edges)
	{
		if (!AfterEdges.Contains(Edge))
		{
			OutRemovedEdges.Add(Edge);
		}
	}

	bool bAllowAnyRemovedEdges = false;
	if (Options.IsValid())
	{
		Options->TryGetBoolField(TEXT("allow_removed_edges"), bAllowAnyRemovedEdges);
	}
	if (bAllowAnyRemovedEdges)
	{
		return true;
	}

	TSet<FString> AllowedRemovedEdges;
	ReadStringSetField(Options, TEXT("allowed_removed_edges"), AllowedRemovedEdges);
	for (const FString& Edge : OutRemovedEdges)
	{
		if (!AllowedRemovedEdges.Contains(Edge))
		{
			OutUnexpectedRemovedEdges.Add(Edge);
		}
	}
	return OutUnexpectedRemovedEdges.IsEmpty();
}

static TSharedPtr<FJsonObject> BuildPreservationValidation(
	const FNiagaraPreservationState& Before,
	const FNiagaraPreservationState& After,
	bool& bOutAllPreserved)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	const bool bSystemSettings = Before.SystemSettings == After.SystemSettings;
	const bool bEmitters = Before.Emitters == After.Emitters;
	const bool bEmitterSettings = Before.EmitterSettings == After.EmitterSettings;
	const bool bRendererIdentities = Before.Renderers == After.Renderers;
	const bool bRendererProperties = Before.RendererProperties == After.RendererProperties;
	const bool bRenderers = bRendererIdentities && bRendererProperties;
	const bool bSimTargets = Before.SimTargets == After.SimTargets;
	const bool bEventHandlers = Before.EventHandlers == After.EventHandlers;
	TArray<FString> MissingParameters;
	TArray<FString> MissingRapidIterationParameters;
	TArray<FString> MissingModules;
	TArray<FString> MissingGraphNodes;
	TArray<FString> MissingGraphPins;
	TArray<FString> AddedGraphNodes;
	TArray<FString> AddedGraphPins;
	TArray<FString> MissingRendererProperties;
	const bool bParameters = IsSubsetWithMissing(Before.UserParameters, After.UserParameters, MissingParameters);
	const bool bRapidIterationParameters = IsSubsetWithMissing(Before.RapidIterationParameters, After.RapidIterationParameters, MissingRapidIterationParameters);
	const bool bModules = IsSubsetWithMissing(Before.Modules, After.Modules, MissingModules);
	const bool bGraphNodes = IsSubsetWithMissing(Before.GraphNodes, After.GraphNodes, MissingGraphNodes);
	const bool bGraphPins = IsSubsetWithMissing(Before.GraphPins, After.GraphPins, MissingGraphPins);
	IsSubsetWithMissing(After.GraphNodes, Before.GraphNodes, AddedGraphNodes);
	IsSubsetWithMissing(After.GraphPins, Before.GraphPins, AddedGraphPins);
	IsSubsetWithMissing(Before.RendererProperties, After.RendererProperties, MissingRendererProperties);
	bOutAllPreserved = bSystemSettings && bEmitters && bEmitterSettings && bRenderers && bSimTargets
		&& bEventHandlers && bParameters && bRapidIterationParameters && bModules && bGraphNodes && bGraphPins;

	Root->SetBoolField(TEXT("all_preserved"), bOutAllPreserved);
	Root->SetBoolField(TEXT("system_timing_and_determinism_preserved"), bSystemSettings);
	Root->SetBoolField(TEXT("emitters_preserved"), bEmitters);
	Root->SetBoolField(TEXT("emitter_settings_preserved"), bEmitterSettings);
	Root->SetBoolField(TEXT("renderers_and_materials_preserved"), bRenderers);
	Root->SetBoolField(TEXT("renderer_identities_and_materials_preserved"), bRendererIdentities);
	Root->SetBoolField(TEXT("renderer_serialized_properties_preserved"), bRendererProperties);
	Root->SetBoolField(TEXT("sim_targets_preserved"), bSimTargets);
	Root->SetBoolField(TEXT("event_handlers_preserved"), bEventHandlers);
	Root->SetBoolField(TEXT("existing_user_parameters_preserved"), bParameters);
	Root->SetBoolField(TEXT("existing_rapid_iteration_values_preserved"), bRapidIterationParameters);
	Root->SetBoolField(TEXT("existing_module_nodes_preserved"), bModules);
	Root->SetBoolField(TEXT("existing_graph_nodes_preserved"), bGraphNodes);
	Root->SetBoolField(TEXT("existing_graph_pin_defaults_preserved"), bGraphPins);
	Root->SetArrayField(TEXT("missing_user_parameters"), StringsToJson(MissingParameters));
	Root->SetArrayField(TEXT("missing_rapid_iteration_values"), StringsToJson(MissingRapidIterationParameters));
	Root->SetArrayField(TEXT("missing_module_nodes"), StringsToJson(MissingModules));
	Root->SetArrayField(TEXT("missing_graph_nodes"), StringsToJson(MissingGraphNodes, 200));
	Root->SetArrayField(TEXT("changed_or_missing_graph_pin_defaults"), StringsToJson(MissingGraphPins, 200));
	Root->SetArrayField(TEXT("added_graph_nodes"), StringsToJson(AddedGraphNodes, 200));
	Root->SetArrayField(TEXT("added_graph_pin_defaults"), StringsToJson(AddedGraphPins, 200));
	Root->SetArrayField(TEXT("changed_or_missing_renderer_properties"), StringsToJson(MissingRendererProperties, 200));

	TSharedPtr<FJsonObject> BeforeFingerprints = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> AfterFingerprints = MakeShared<FJsonObject>();
	auto AddFingerprints = [](const FNiagaraPreservationState& State, const TSharedPtr<FJsonObject>& Target)
	{
		Target->SetStringField(TEXT("system_settings"), StableFingerprint(State.SystemSettings));
		Target->SetStringField(TEXT("emitters"), StableFingerprint(State.Emitters));
		Target->SetStringField(TEXT("emitter_settings"), StableFingerprint(State.EmitterSettings));
		Target->SetStringField(TEXT("renderers"), StableFingerprint(State.Renderers));
		Target->SetStringField(TEXT("renderer_properties"), StableFingerprint(State.RendererProperties));
		Target->SetStringField(TEXT("user_parameters"), StableFingerprint(State.UserParameters));
		Target->SetStringField(TEXT("rapid_iteration_parameters"), StableFingerprint(State.RapidIterationParameters));
		Target->SetStringField(TEXT("sim_targets"), StableFingerprint(State.SimTargets));
		Target->SetStringField(TEXT("event_handlers"), StableFingerprint(State.EventHandlers));
		Target->SetStringField(TEXT("modules"), StableFingerprint(State.Modules));
		Target->SetStringField(TEXT("graph_nodes"), StableFingerprint(State.GraphNodes));
		Target->SetStringField(TEXT("graph_pin_defaults"), StableFingerprint(State.GraphPins));
		Target->SetStringField(TEXT("edges"), StableFingerprint(State.Edges));
	};
	AddFingerprints(Before, BeforeFingerprints);
	AddFingerprints(After, AfterFingerprints);
	Root->SetObjectField(TEXT("before_fingerprints"), BeforeFingerprints);
	Root->SetObjectField(TEXT("after_fingerprints"), AfterFingerprints);

	TArray<FString> AddedEdges;
	TArray<FString> RemovedEdges;
	TSet<FString> BeforeEdgeSet;
	TSet<FString> AfterEdgeSet;
	for (const FString& Edge : Before.Edges) BeforeEdgeSet.Add(Edge);
	for (const FString& Edge : After.Edges) AfterEdgeSet.Add(Edge);
	for (const FString& Edge : After.Edges) if (!BeforeEdgeSet.Contains(Edge)) AddedEdges.Add(Edge);
	for (const FString& Edge : Before.Edges) if (!AfterEdgeSet.Contains(Edge)) RemovedEdges.Add(Edge);
	Root->SetNumberField(TEXT("added_edge_count"), AddedEdges.Num());
	Root->SetNumberField(TEXT("removed_edge_count"), RemovedEdges.Num());
	Root->SetArrayField(TEXT("added_edges"), StringsToJson(AddedEdges, 200));
	Root->SetArrayField(TEXT("removed_edges"), StringsToJson(RemovedEdges, 200));
	Root->SetNumberField(TEXT("before_module_count"), Before.Modules.Num());
	Root->SetNumberField(TEXT("after_module_count"), After.Modules.Num());
	return Root;
}

static TSharedPtr<FJsonObject> CompileStateToJson(UNiagaraSystem* System, bool& bOutHasErrors)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	bOutHasErrors = true;
	if (!System)
	{
		Root->SetStringField(TEXT("error"), TEXT("Invalid Niagara system"));
		return Root;
	}

	FNiagaraExternalEditContext Context(System);
	FNiagaraExt_SystemCompileState State;
	UNiagaraExternalEditUtilities::GetSystemCompileState(System, State, Context);
	Root->SetStringField(TEXT("aggregate_status"), EnumToStringSafe(StaticEnum<ENiagaraExt_ScriptCompileStatus>(), static_cast<int64>(State.AggregateStatus)));
	Root->SetBoolField(TEXT("is_compiling"), State.bIsCompiling);
	Root->SetBoolField(TEXT("is_stale"), State.bIsStale);
	Root->SetBoolField(TEXT("has_errors"), State.bHasErrors);
	Root->SetBoolField(TEXT("has_warnings"), State.bHasWarnings);
	Root->SetBoolField(TEXT("ready_to_run"), System->IsReadyToRun());
	bOutHasErrors = State.bHasErrors || Context.HasErrors();

	TArray<TSharedPtr<FJsonValue>> Scripts;
	for (const FNiagaraExt_ScriptCompileInfo& ScriptInfo : State.Scripts)
	{
		TSharedPtr<FJsonObject> Script = MakeShared<FJsonObject>();
		Script->SetStringField(TEXT("emitter"), ScriptInfo.EmitterName.ToString());
		Script->SetStringField(TEXT("script"), ScriptInfo.ScriptName.ToString());
		Script->SetStringField(TEXT("status"), EnumToStringSafe(StaticEnum<ENiagaraExt_ScriptCompileStatus>(), static_cast<int64>(ScriptInfo.LastCompileStatus)));
		Script->SetStringField(TEXT("error_summary"), ScriptInfo.ErrorSummary);
		TArray<TSharedPtr<FJsonValue>> Events;
		for (const FNiagaraExt_CompileEvent& CompileEvent : ScriptInfo.CompileEvents)
		{
			TSharedPtr<FJsonObject> Event = MakeShared<FJsonObject>();
			Event->SetStringField(TEXT("severity"), EnumToStringSafe(StaticEnum<ENiagaraExt_CompileEventSeverity>(), static_cast<int64>(CompileEvent.Severity)));
			Event->SetStringField(TEXT("message"), CompileEvent.Message);
			Event->SetStringField(TEXT("short_description"), CompileEvent.ShortDescription);
			Event->SetStringField(TEXT("node_guid"), CompileEvent.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
			Event->SetStringField(TEXT("pin_guid"), CompileEvent.PinGuid.ToString(EGuidFormats::DigitsWithHyphens));
			Event->SetBoolField(TEXT("from_script_dependency"), CompileEvent.bFromScriptDependency);
			Events.Add(MakeShared<FJsonValueObject>(Event));
		}
		Script->SetArrayField(TEXT("events"), Events);
		Scripts.Add(MakeShared<FJsonValueObject>(Script));
	}
	Root->SetArrayField(TEXT("scripts"), Scripts);
	if (Context.HasErrors())
	{
		Root->SetStringField(TEXT("diagnostic_error"), JoinContextErrors(Context.Errors));
	}
	return Root;
}

static TSharedPtr<FJsonObject> MakeOperationResult(int32 Index, const TSharedPtr<FJsonObject>& Operation, const FString& Status, const FString& Message)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("index"), Index);
	FString Id;
	FString Op;
	if (Operation.IsValid())
	{
		Operation->TryGetStringField(TEXT("id"), Id);
		Operation->TryGetStringField(TEXT("op"), Op);
		Operation->TryGetStringField(TEXT("type"), Op);
	}
	Result->SetStringField(TEXT("id"), Id);
	Result->SetStringField(TEXT("op"), Op);
	Result->SetStringField(TEXT("status"), Status);
	Result->SetStringField(TEXT("message"), Message);
	return Result;
}

static bool ResolveSimpleNiagaraType(const FString& TypeName, FNiagaraTypeDefinition& OutType)
{
	FString Normalized = TypeName;
	Normalized.TrimStartAndEndInline();
	Normalized.ToLowerInline();
	Normalized.ReplaceInline(TEXT(" "), TEXT(""));
	Normalized.ReplaceInline(TEXT("_"), TEXT(""));
	if (Normalized.StartsWith(TEXT("niagara")))
	{
		Normalized.RightChopInline(7);
	}

	if (Normalized == TEXT("bool") || Normalized == TEXT("boolean"))
	{
		OutType = FNiagaraTypeDefinition::GetBoolDef();
		return true;
	}
	if (Normalized == TEXT("int") || Normalized == TEXT("int32") || Normalized == TEXT("integer"))
	{
		OutType = FNiagaraTypeDefinition::GetIntDef();
		return true;
	}
	if (Normalized == TEXT("float") || Normalized == TEXT("float32") || Normalized == TEXT("scalar"))
	{
		OutType = FNiagaraTypeDefinition::GetFloatDef();
		return true;
	}
	return false;
}

static bool ApplyAddUserParameter(
	UNiagaraSystem* System,
	const TSharedPtr<FJsonObject>& Operation,
	const TSharedPtr<FJsonObject>& Result,
	FString& OutError)
{
	FString Name;
	FString TypeName;
	Operation->TryGetStringField(TEXT("name"), Name);
	Operation->TryGetStringField(TEXT("parameter"), Name);
	Operation->TryGetStringField(TEXT("value_type"), TypeName);
	Operation->TryGetStringField(TEXT("parameter_type"), TypeName);
	Operation->TryGetStringField(TEXT("niagara_type"), TypeName);
	Name.TrimStartAndEndInline();
	if (!Name.StartsWith(TEXT("User.")))
	{
		Name = TEXT("User.") + Name;
	}
	if (Name == TEXT("User.") || TypeName.IsEmpty())
	{
		OutError = TEXT("add_user_parameter requires name and value_type/parameter_type");
		return false;
	}

	FNiagaraTypeDefinition TypeDefinition;
	if (!ResolveSimpleNiagaraType(TypeName, TypeDefinition))
	{
		OutError = FString::Printf(TEXT("Unsupported scalar Niagara user-parameter type: %s"), *TypeName);
		return false;
	}

	FNiagaraParameterStore& Store = System->GetExposedParameters();
	const FNiagaraVariable Variable(TypeDefinition, *Name);
	bool bExisting = false;
	for (const FNiagaraVariableWithOffset& Existing : Store.ReadParameterVariables())
	{
		if (!Existing.GetName().ToString().Equals(Name, ESearchCase::IgnoreCase))
		{
			continue;
		}
		if (Existing.GetType() != TypeDefinition)
		{
			OutError = FString::Printf(TEXT("User parameter already exists with a different type: %s (%s)"), *Name, *Existing.GetType().GetName());
			return false;
		}
		bExisting = true;
		break;
	}

	System->Modify();
	if (!bExisting && !Store.AddParameter(Variable, true, true))
	{
		OutError = FString::Printf(TEXT("Failed to add Niagara user parameter: %s"), *Name);
		return false;
	}

	const TSharedPtr<FJsonValue> DefaultValue = Operation->TryGetField(TEXT("default"));
	if (DefaultValue.IsValid())
	{
		bool bSet = false;
		if (TypeDefinition == FNiagaraTypeDefinition::GetBoolDef() && DefaultValue->Type == EJson::Boolean)
		{
			bSet = Store.SetParameterValue(FNiagaraBool(DefaultValue->AsBool()), Variable);
		}
		else if (TypeDefinition == FNiagaraTypeDefinition::GetIntDef() && DefaultValue->Type == EJson::Number)
		{
			bSet = Store.SetParameterValue(static_cast<int32>(DefaultValue->AsNumber()), Variable);
		}
		else if (TypeDefinition == FNiagaraTypeDefinition::GetFloatDef() && DefaultValue->Type == EJson::Number)
		{
			bSet = Store.SetParameterValue(static_cast<float>(DefaultValue->AsNumber()), Variable);
		}
		else
		{
			OutError = FString::Printf(TEXT("Default value does not match Niagara type %s for %s"), *TypeDefinition.GetName(), *Name);
			return false;
		}
		if (!bSet)
		{
			OutError = FString::Printf(TEXT("Failed to set default value for Niagara user parameter: %s"), *Name);
			return false;
		}
	}

	System->MarkPackageDirty();
	Result->SetStringField(TEXT("parameter"), Name);
	Result->SetStringField(TEXT("parameter_type"), TypeDefinition.GetName());
	Result->SetBoolField(TEXT("reused_existing"), bExisting);
	return true;
}

static bool ApplyAddUserDataInterface(
	UNiagaraSystem* System,
	const TSharedPtr<FJsonObject>& Operation,
	const TSharedPtr<FJsonObject>& Result,
	FString& OutError)
{
	FString Name;
	FString ClassPath;
	Operation->TryGetStringField(TEXT("name"), Name);
	Operation->TryGetStringField(TEXT("parameter"), Name);
	Operation->TryGetStringField(TEXT("class"), ClassPath);
	Operation->TryGetStringField(TEXT("data_interface_class"), ClassPath);
	Name.TrimStartAndEndInline();
	if (!Name.StartsWith(TEXT("User.")))
	{
		Name = TEXT("User.") + Name;
	}
	if (Name == TEXT("User.") || ClassPath.IsEmpty())
	{
		OutError = TEXT("add_user_data_interface requires name and data_interface_class");
		return false;
	}

	UClass* DataInterfaceClass = LoadObject<UClass>(nullptr, *ClassPath);
	if (!DataInterfaceClass)
	{
		DataInterfaceClass = StaticLoadClass(UNiagaraDataInterface::StaticClass(), nullptr, *ClassPath);
	}
	if (!DataInterfaceClass || !DataInterfaceClass->IsChildOf(UNiagaraDataInterface::StaticClass()))
	{
		OutError = FString::Printf(TEXT("Data interface class not found or invalid: %s"), *ClassPath);
		return false;
	}

	FNiagaraParameterStore& Store = System->GetExposedParameters();
	const FNiagaraTypeDefinition TypeDefinition(DataInterfaceClass);
	const FNiagaraVariable Variable(TypeDefinition, *Name);
	bool bExisting = false;
	for (const FNiagaraVariableWithOffset& Existing : Store.ReadParameterVariables())
	{
		if (!Existing.GetName().ToString().Equals(Name, ESearchCase::IgnoreCase))
		{
			continue;
		}
		if (Existing.GetType() != TypeDefinition)
		{
			OutError = FString::Printf(TEXT("User parameter already exists with a different type: %s (%s)"), *Name, *Existing.GetType().GetName());
			return false;
		}
		bExisting = true;
		break;
	}

	System->Modify();
	if (!bExisting)
	{
		Store.AddParameter(Variable, true, true);
	}
	UNiagaraDataInterface* DataInterface = Store.GetDataInterface(Variable);
	if (!DataInterface)
	{
		OutError = FString::Printf(TEXT("Failed to create data interface default for user parameter: %s"), *Name);
		return false;
	}

	const TSharedPtr<FJsonObject>* Properties = nullptr;
	if (Operation->TryGetObjectField(TEXT("properties"), Properties) && Properties && Properties->IsValid())
	{
		if (!ApplyJsonProperties(DataInterface, *Properties, OutError))
		{
			return false;
		}
	}
	System->MarkPackageDirty();
	Result->SetStringField(TEXT("parameter"), Name);
	Result->SetStringField(TEXT("data_interface_class"), DataInterfaceClass->GetPathName());
	Result->SetStringField(TEXT("data_interface"), DataInterface->GetPathName());
	Result->SetBoolField(TEXT("reused_existing"), bExisting);
	return true;
}

static bool ApplyInsertModule(
	UNiagaraSystem* System,
	const TSharedPtr<FJsonObject>& Operation,
	TMap<FString, FCreatedNodeRecord>& CreatedNodes,
	const TSharedPtr<FJsonObject>& Result,
	FString& OutError)
{
	TArray<FNiagaraGraphContext> Contexts;
	if (!SelectGraphContexts(System, GetGraphSelector(Operation), Contexts, OutError, true))
	{
		return false;
	}
	const FNiagaraGraphContext& Context = Contexts[0];
	if (!Context.OutputNode || !Context.Graph)
	{
		OutError = TEXT("Selected Niagara graph has no editable output node");
		return false;
	}

	FString ModulePath;
	Operation->TryGetStringField(TEXT("module_asset"), ModulePath);
	Operation->TryGetStringField(TEXT("module"), ModulePath);
	Operation->TryGetStringField(TEXT("script"), ModulePath);
	UNiagaraScript* ModuleScript = ModulePath.IsEmpty() ? nullptr : LoadObject<UNiagaraScript>(nullptr, *NormalizeObjectPath(ModulePath));
	if (!ModuleScript)
	{
		OutError = FString::Printf(TEXT("Niagara module script not found: %s"), *ModulePath);
		return false;
	}
	if (ModuleScript->GetUsage() != ENiagaraScriptUsage::Module)
	{
		OutError = FString::Printf(TEXT("Niagara script is not a module: %s"), *ModuleScript->GetPathName());
		return false;
	}

	int32 TargetIndex = INDEX_NONE;
	Operation->TryGetNumberField(TEXT("index"), TargetIndex);
	Operation->TryGetNumberField(TEXT("stack_index"), TargetIndex);
	if (TargetIndex < INDEX_NONE)
	{
		OutError = TEXT("insert_module index must be -1 (append) or a non-negative stack index");
		return false;
	}
	FString SuggestedName;
	Operation->TryGetStringField(TEXT("suggested_name"), SuggestedName);
	FGuid VersionGuid = ModuleScript->GetExposedVersion().VersionGuid;
	ParseGuidField(Operation, TEXT("version_guid"), VersionGuid);

	System->Modify();
	Context.Graph->Modify();
	UNiagaraNodeFunctionCall* NewNode = FNiagaraStackGraphUtilities::AddScriptModuleToStack(
		ModuleScript,
		*Context.OutputNode,
		TargetIndex,
		SuggestedName,
		VersionGuid);
	if (!NewNode)
	{
		OutError = FString::Printf(TEXT("Failed to insert module into %s:%s:%s"), *Context.Scope, *Context.EmitterName, *ScriptUsageToString(Context.Usage));
		return false;
	}

	Context.Graph->NotifyGraphChanged();
	System->MarkPackageDirty();
	FString Id;
	Operation->TryGetStringField(TEXT("id"), Id);
	if (!Id.IsEmpty())
	{
		if (CreatedNodes.Contains(Id))
		{
			OutError = FString::Printf(TEXT("Duplicate graph-edit operation id: %s"), *Id);
			return false;
		}
		FCreatedNodeRecord Created;
		Created.Node = NewNode;
		Created.Context = Context;
		CreatedNodes.Add(Id, Created);
	}

	Result->SetStringField(TEXT("node_guid"), NewNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	Result->SetStringField(TEXT("function_name"), NewNode->GetFunctionName());
	Result->SetStringField(TEXT("module_asset"), ModuleScript->GetPathName());
	Result->SetNumberField(TEXT("requested_stack_index"), TargetIndex);
	Result->SetStringField(TEXT("emitter"), Context.EmitterName);
	Result->SetStringField(TEXT("script_usage"), ScriptUsageToString(Context.Usage));
	Result->SetStringField(TEXT("usage_id"), Context.UsageId.ToString(EGuidFormats::DigitsWithHyphens));
	return true;
}

static bool GetRequiredObjectField(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Name,
	TSharedPtr<FJsonObject>& OutObject,
	FString& OutError)
{
	const TSharedPtr<FJsonObject>* Value = nullptr;
	if (!Object.IsValid() || !Object->TryGetObjectField(Name, Value) || !Value || !Value->IsValid())
	{
		OutError = FString::Printf(TEXT("Operation requires object field: %s"), Name);
		return false;
	}
	OutObject = *Value;
	return true;
}

static bool ApplyConnectPins(
	UNiagaraSystem* System,
	const TSharedPtr<FJsonObject>& Operation,
	const TMap<FString, FCreatedNodeRecord>& CreatedNodes,
	const TSharedPtr<FJsonObject>& Result,
	FString& OutError)
{
	TSharedPtr<FJsonObject> FromRef;
	TSharedPtr<FJsonObject> ToRef;
	if (!GetRequiredObjectField(Operation, TEXT("from"), FromRef, OutError) || !GetRequiredObjectField(Operation, TEXT("to"), ToRef, OutError))
	{
		return false;
	}
	UEdGraphPin* FromPin = ResolvePinReference(System, FromRef, CreatedNodes, OutError);
	UEdGraphPin* ToPin = ResolvePinReference(System, ToRef, CreatedNodes, OutError);
	if (!FromPin || !ToPin)
	{
		return false;
	}
	if (FromPin->Direction != EGPD_Output || ToPin->Direction != EGPD_Input)
	{
		OutError = TEXT("connect_pins requires from=output and to=input");
		return false;
	}
	if (FromPin->GetOwningNode()->GetGraph() != ToPin->GetOwningNode()->GetGraph())
	{
		OutError = TEXT("connect_pins cannot link pins from different Niagara graphs");
		return false;
	}
	if (FromPin->LinkedTo.Contains(ToPin))
	{
		Result->SetBoolField(TEXT("changed"), false);
		Result->SetStringField(TEXT("connection"), TEXT("already_connected"));
		return true;
	}

	bool bReplace = false;
	bool bBreakSource = false;
	Operation->TryGetBoolField(TEXT("replace"), bReplace);
	Operation->TryGetBoolField(TEXT("break_source"), bBreakSource);
	if (!bReplace && ToPin->LinkedTo.Num() > 0)
	{
		OutError = TEXT("Target pin already has a link; pass replace=true for an explicit rewire");
		return false;
	}

	const UEdGraphSchema_Niagara* Schema = GetDefault<UEdGraphSchema_Niagara>();
	const FPinConnectionResponse InitialResponse = Schema->CanCreateConnection(FromPin, ToPin);
	if (InitialResponse.Response == CONNECT_RESPONSE_DISALLOW)
	{
		OutError = FString::Printf(TEXT("Niagara schema rejected connection: %s"), *InitialResponse.Message.ToString());
		return false;
	}

	FromPin->GetOwningNode()->Modify();
	ToPin->GetOwningNode()->Modify();
	if (bReplace)
	{
		Schema->BreakPinLinks(*ToPin, true);
		if (bBreakSource)
		{
			Schema->BreakPinLinks(*FromPin, true);
		}
	}
	const FPinConnectionResponse FinalResponse = Schema->CanCreateConnection(FromPin, ToPin);
	if (FinalResponse.Response == CONNECT_RESPONSE_DISALLOW || !Schema->TryCreateConnection(FromPin, ToPin))
	{
		OutError = FString::Printf(TEXT("Failed to create Niagara pin connection: %s"), *FinalResponse.Message.ToString());
		return false;
	}

	System->MarkPackageDirty();
	Result->SetBoolField(TEXT("changed"), true);
	Result->SetStringField(TEXT("from_node_guid"), FromPin->GetOwningNode()->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	Result->SetStringField(TEXT("from_pin"), FromPin->PinName.ToString());
	Result->SetStringField(TEXT("to_node_guid"), ToPin->GetOwningNode()->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	Result->SetStringField(TEXT("to_pin"), ToPin->PinName.ToString());
	return true;
}

static bool ApplyDisconnectPins(
	UNiagaraSystem* System,
	const TSharedPtr<FJsonObject>& Operation,
	const TMap<FString, FCreatedNodeRecord>& CreatedNodes,
	const TSharedPtr<FJsonObject>& Result,
	FString& OutError)
{
	TSharedPtr<FJsonObject> FromRef;
	TSharedPtr<FJsonObject> ToRef;
	if (!GetRequiredObjectField(Operation, TEXT("from"), FromRef, OutError) || !GetRequiredObjectField(Operation, TEXT("to"), ToRef, OutError))
	{
		return false;
	}
	UEdGraphPin* FromPin = ResolvePinReference(System, FromRef, CreatedNodes, OutError);
	UEdGraphPin* ToPin = ResolvePinReference(System, ToRef, CreatedNodes, OutError);
	if (!FromPin || !ToPin)
	{
		return false;
	}
	if (!FromPin->LinkedTo.Contains(ToPin))
	{
		Result->SetBoolField(TEXT("changed"), false);
		Result->SetStringField(TEXT("connection"), TEXT("already_disconnected"));
		return true;
	}
	GetDefault<UEdGraphSchema_Niagara>()->BreakSinglePinLink(FromPin, ToPin);
	System->MarkPackageDirty();
	Result->SetBoolField(TEXT("changed"), true);
	return true;
}

static bool ApplyDisconnectPin(
	UNiagaraSystem* System,
	const TSharedPtr<FJsonObject>& Operation,
	const TMap<FString, FCreatedNodeRecord>& CreatedNodes,
	const TSharedPtr<FJsonObject>& Result,
	FString& OutError)
{
	TSharedPtr<FJsonObject> PinRef;
	if (!GetRequiredObjectField(Operation, TEXT("pin_ref"), PinRef, OutError))
	{
		return false;
	}
	UEdGraphPin* Pin = ResolvePinReference(System, PinRef, CreatedNodes, OutError);
	if (!Pin)
	{
		return false;
	}
	const int32 RemovedLinks = Pin->LinkedTo.Num();
	if (RemovedLinks > 0)
	{
		GetDefault<UEdGraphSchema_Niagara>()->BreakPinLinks(*Pin, true);
		System->MarkPackageDirty();
	}
	Result->SetBoolField(TEXT("changed"), RemovedLinks > 0);
	Result->SetNumberField(TEXT("removed_link_count"), RemovedLinks);
	return true;
}

static bool ApplyRewirePin(
	UNiagaraSystem* System,
	const TSharedPtr<FJsonObject>& Operation,
	const TMap<FString, FCreatedNodeRecord>& CreatedNodes,
	const TSharedPtr<FJsonObject>& Result,
	FString& OutError)
{
	TSharedPtr<FJsonObject> TargetRef;
	TSharedPtr<FJsonObject> ThroughInputRef;
	TSharedPtr<FJsonObject> ThroughOutputRef;
	if (!GetRequiredObjectField(Operation, TEXT("target"), TargetRef, OutError)
		|| !GetRequiredObjectField(Operation, TEXT("through_input"), ThroughInputRef, OutError)
		|| !GetRequiredObjectField(Operation, TEXT("through_output"), ThroughOutputRef, OutError))
	{
		return false;
	}
	UEdGraphPin* TargetPin = ResolvePinReference(System, TargetRef, CreatedNodes, OutError);
	UEdGraphPin* ThroughInput = ResolvePinReference(System, ThroughInputRef, CreatedNodes, OutError);
	UEdGraphPin* ThroughOutput = ResolvePinReference(System, ThroughOutputRef, CreatedNodes, OutError);
	if (!TargetPin || !ThroughInput || !ThroughOutput)
	{
		return false;
	}
	if (TargetPin->Direction != EGPD_Input || ThroughInput->Direction != EGPD_Input || ThroughOutput->Direction != EGPD_Output)
	{
		OutError = TEXT("rewire_pin requires target=input, through_input=input, through_output=output");
		return false;
	}
	if (TargetPin->LinkedTo.Num() != 1)
	{
		OutError = FString::Printf(TEXT("rewire_pin target must have exactly one existing link; found %d"), TargetPin->LinkedTo.Num());
		return false;
	}
	if (ThroughInput->LinkedTo.Num() > 0)
	{
		OutError = TEXT("rewire_pin through_input is already linked");
		return false;
	}
	UEdGraphPin* OriginalSource = TargetPin->LinkedTo[0];
	if (!OriginalSource || OriginalSource->Direction != EGPD_Output)
	{
		OutError = TEXT("rewire_pin could not resolve the original output source");
		return false;
	}
	UEdGraph* Graph = TargetPin->GetOwningNode()->GetGraph();
	if (OriginalSource->GetOwningNode()->GetGraph() != Graph || ThroughInput->GetOwningNode()->GetGraph() != Graph || ThroughOutput->GetOwningNode()->GetGraph() != Graph)
	{
		OutError = TEXT("rewire_pin requires all pins in the same Niagara graph");
		return false;
	}

	const UEdGraphSchema_Niagara* Schema = GetDefault<UEdGraphSchema_Niagara>();
	const FPinConnectionResponse FirstResponse = Schema->CanCreateConnection(OriginalSource, ThroughInput);
	const FPinConnectionResponse SecondResponse = Schema->CanCreateConnection(ThroughOutput, TargetPin);
	if (FirstResponse.Response == CONNECT_RESPONSE_DISALLOW || SecondResponse.Response == CONNECT_RESPONSE_DISALLOW)
	{
		OutError = FString::Printf(TEXT("Niagara schema rejected rewire: source->through=%s; through->target=%s"), *FirstResponse.Message.ToString(), *SecondResponse.Message.ToString());
		return false;
	}

	Schema->BreakSinglePinLink(OriginalSource, TargetPin);
	if (!Schema->TryCreateConnection(OriginalSource, ThroughInput))
	{
		Schema->TryCreateConnection(OriginalSource, TargetPin);
		OutError = TEXT("Failed to connect original source to through_input; original link restored");
		return false;
	}
	if (!Schema->TryCreateConnection(ThroughOutput, TargetPin))
	{
		Schema->BreakSinglePinLink(OriginalSource, ThroughInput);
		Schema->TryCreateConnection(OriginalSource, TargetPin);
		OutError = TEXT("Failed to connect through_output to target; original link restored");
		return false;
	}

	System->MarkPackageDirty();
	Result->SetBoolField(TEXT("changed"), true);
	Result->SetStringField(TEXT("original_source_node_guid"), OriginalSource->GetOwningNode()->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	Result->SetStringField(TEXT("original_source_pin"), OriginalSource->PinName.ToString());
	return true;
}

static FString NormalizeStackInputLookup(FString Value)
{
	Value.TrimStartAndEndInline();
	Value.ToLowerInline();
	Value.ReplaceInline(TEXT(" "), TEXT(""));
	Value.ReplaceInline(TEXT("_"), TEXT(""));
	Value.ReplaceInline(TEXT("-"), TEXT(""));
	Value.ReplaceInline(TEXT("."), TEXT(""));
	return Value;
}

static UNiagaraStackFunctionInput* ResolveUsageAwareStackInput(
	FNiagaraExternalEditContext& ExternalContext,
	const FNiagaraGraphContext& GraphContext,
	UNiagaraNodeFunctionCall* ModuleNode,
	const TArray<FName>& RequestedNameStack,
	TArray<FName>& OutCanonicalNameStack,
	FString& OutError)
{
	UNiagaraStackScriptItemGroup* ScriptGroup = FindUsageAwareScriptGroup(ExternalContext, GraphContext, OutError);
	UNiagaraStackModuleItem* ModuleItem = ScriptGroup
		? FindUsageAwareModule(ScriptGroup, ModuleNode, OutError)
		: nullptr;
	if (!ModuleItem)
	{
		return nullptr;
	}

	UNiagaraStackEntry* SearchRoot = ModuleItem;
	UNiagaraStackFunctionInput* ResolvedInput = nullptr;
	OutCanonicalNameStack.Reset();
	for (int32 Depth = 0; Depth < RequestedNameStack.Num(); ++Depth)
	{
		const FName RequestedName = RequestedNameStack[Depth];
		const FString NormalizedRequested = NormalizeStackInputLookup(RequestedName.ToString());
		TArray<UNiagaraStackFunctionInput*> ExactMatches;
		TArray<UNiagaraStackFunctionInput*> NormalizedMatches;
		TArray<FString> AvailableInputs;
		ForEachFlattenedStackInput(SearchRoot, [&](UNiagaraStackFunctionInput* Candidate)
		{
			const FName CandidateName = Candidate->GetInputParameterHandle().GetName();
			AvailableInputs.AddUnique(CandidateName.ToString());
			if (CandidateName == RequestedName)
			{
				ExactMatches.Add(Candidate);
			}
			if (NormalizeStackInputLookup(CandidateName.ToString()) == NormalizedRequested)
			{
				NormalizedMatches.Add(Candidate);
			}
			return true;
		});

		const TArray<UNiagaraStackFunctionInput*>& Matches = ExactMatches.Num() > 0 ? ExactMatches : NormalizedMatches;
		if (Matches.Num() != 1)
		{
			AvailableInputs.Sort();
			OutError = FString::Printf(
				TEXT("Stack input '%s' at path depth %d resolved to %d inputs on module '%s'. Available inputs: %s"),
				*RequestedName.ToString(),
				Depth,
				Matches.Num(),
				*ModuleNode->GetFunctionName(),
				*FString::Join(AvailableInputs, TEXT(", ")));
			return nullptr;
		}

		ResolvedInput = Matches[0];
		OutCanonicalNameStack.Add(ResolvedInput->GetInputParameterHandle().GetName());
		SearchRoot = ResolvedInput;
	}
	return ResolvedInput;
}

static bool ApplyUsageAwareStackInputValue(
	UNiagaraSystem* System,
	const FNiagaraGraphContext& GraphContext,
	UNiagaraNodeFunctionCall* ModuleNode,
	const TArray<FName>& RequestedNameStack,
	const TSharedPtr<FJsonObject>& Value,
	const FString& Mode,
	const TSharedPtr<FJsonObject>& Result,
	FString& OutError,
	FNiagaraExternalEditContext* ReusedExternalContext = nullptr)
{
	TUniquePtr<FNiagaraExternalEditContext> OwnedExternalContext;
	if (!ReusedExternalContext)
	{
		OwnedExternalContext = MakeUnique<FNiagaraExternalEditContext>(System);
		ReusedExternalContext = OwnedExternalContext.Get();
	}
	FNiagaraExternalEditContext& ExternalContext = *ReusedExternalContext;
	TArray<FName> CanonicalNameStack;
	UNiagaraStackFunctionInput* Input = ResolveUsageAwareStackInput(
		ExternalContext,
		GraphContext,
		ModuleNode,
		RequestedNameStack,
		CanonicalNameStack,
		OutError);
	if (!Input)
	{
		return false;
	}

	const bool bHidden = Input->GetIsHidden();
	const bool bVisibleCondition = Input->GetHasVisibleCondition() ? Input->GetVisibleConditionEnabled() : true;
	const bool bEditCondition = Input->GetHasEditCondition() ? Input->GetEditConditionEnabled() : true;
	if (bHidden)
	{
		OutError = FString::Printf(
			TEXT("Refusing to set input '%s': it is hidden by static-switch or conditional logic"),
			*Input->GetInputParameterHandle().GetName().ToString());
		return false;
	}
	if (!bVisibleCondition)
	{
		OutError = FString::Printf(
			TEXT("Refusing to set input '%s': its VisibleCondition is false"),
			*Input->GetInputParameterHandle().GetName().ToString());
		return false;
	}
	if (!bEditCondition)
	{
		OutError = FString::Printf(
			TEXT("Refusing to set input '%s': its EditCondition is false"),
			*Input->GetInputParameterHandle().GetName().ToString());
		return false;
	}

	const FNiagaraTypeDefinition InputType = Input->GetInputType();
	if (Mode == TEXT("local") || Mode == TEXT("literal") || Mode == TEXT("constant"))
	{
		UScriptStruct* ValueStruct = InputType.GetScriptStruct();
		if (!ValueStruct)
		{
			OutError = FString::Printf(
				TEXT("Input '%s' does not expose a local Niagara value struct (actual type: %s)"),
				*Input->GetInputParameterHandle().GetName().ToString(),
				*InputType.GetName());
			return false;
		}

		TSharedRef<FStructOnScope> LocalValue = MakeShared<FStructOnScope>(ValueStruct);
		FString ValueText;
		FString EnumName;
		Value->TryGetStringField(TEXT("value_text"), ValueText);
		Value->TryGetStringField(TEXT("enum_name"), EnumName);

		if (!EnumName.IsEmpty())
		{
			UEnum* Enum = InputType.GetEnum();
			if (!Enum)
			{
				OutError = FString::Printf(
					TEXT("Input '%s' is not an enum, so enum_name is invalid"),
					*Input->GetInputParameterHandle().GetName().ToString());
				return false;
			}
			const int64 EnumValue = Enum->GetValueByNameString(EnumName);
			if (EnumValue == INDEX_NONE)
			{
				OutError = FString::Printf(
					TEXT("Enum value '%s' was not found in %s"),
					*EnumName,
					*Enum->GetPathName());
				return false;
			}
			const int32 NiagaraEnumValue = static_cast<int32>(EnumValue);
			if (ValueStruct->GetStructureSize() < sizeof(NiagaraEnumValue))
			{
				OutError = FString::Printf(TEXT("Enum input '%s' has an invalid backing value size"), *InputType.GetName());
				return false;
			}
			FMemory::Memcpy(LocalValue->GetStructMemory(), &NiagaraEnumValue, sizeof(NiagaraEnumValue));
			ValueText = FString::Printf(TEXT("(Value=%d)"), NiagaraEnumValue);
		}
		else
		{
			if (ValueText.IsEmpty())
			{
				TSharedPtr<FJsonValue> LiteralField = Value->TryGetField(TEXT("literal"));
				if (!LiteralField.IsValid())
				{
					LiteralField = Value->TryGetField(TEXT("value"));
				}
				if (LiteralField.IsValid())
				{
					FString LiteralText;
					if (LiteralField->Type == EJson::Boolean && InputType == FNiagaraTypeDefinition::GetBoolDef())
					{
						LiteralText = LiteralField->AsBool() ? TEXT("-1") : TEXT("0");
					}
					else
					{
						LiteralText = JsonValueToImportText(LiteralField);
					}
					if (!LiteralText.IsEmpty())
					{
						ValueText = FString::Printf(TEXT("(Value=%s)"), *LiteralText);
					}
				}
			}

			if (ValueText.IsEmpty())
			{
				OutError = TEXT("local value requires value_text, enum_name, or scalar literal/value");
				return false;
			}

			FStringOutputDevice ImportErrors;
			const TCHAR* ImportEnd = ValueStruct->ImportText(
				*ValueText,
				LocalValue->GetStructMemory(),
				nullptr,
				PPF_Copy,
				&ImportErrors,
				ValueStruct->GetName());
			if (!ImportEnd)
			{
				const FString ImportDetails = ImportErrors.IsEmpty()
					? FString()
					: FString::Printf(TEXT(": %s"), *ImportErrors);
				OutError = FString::Printf(
					TEXT("Failed to import local value '%s' into %s%s"),
					*ValueText,
					*ValueStruct->GetPathName(),
					*ImportDetails);
				return false;
			}
		}

		// Mirror Niagara's clipboard/external-edit path: construct the exact backing
		// struct, then let the stack view model author the override.
		Input->SetLocalValue(LocalValue);
		Result->SetStringField(TEXT("local_value_text"), ValueText);
	}
	else if (Mode == TEXT("linked_parameter") || Mode == TEXT("linked") || Mode == TEXT("link"))
	{
		FString ParameterName;
		Value->TryGetStringField(TEXT("name"), ParameterName);
		Value->TryGetStringField(TEXT("parameter"), ParameterName);
		if (ParameterName.IsEmpty())
		{
			OutError = TEXT("linked_parameter value requires name/parameter");
			return false;
		}
		Input->SetLinkedParameterValue(FNiagaraVariableBase(InputType, *ParameterName));
		Result->SetStringField(TEXT("linked_parameter"), ParameterName);
	}
	else if (Mode == TEXT("hlsl") || Mode == TEXT("expression"))
	{
		if (!Input->SupportsCustomExpressions())
		{
			OutError = FString::Printf(
				TEXT("Input '%s' does not support custom HLSL expressions"),
				*Input->GetInputParameterHandle().GetName().ToString());
			return false;
		}
		FString Expression;
		Value->TryGetStringField(TEXT("expression"), Expression);
		Value->TryGetStringField(TEXT("hlsl"), Expression);
		Input->SetCustomExpression(Expression);
	}
	else if (Mode == TEXT("dynamic_input"))
	{
		FString AssetPath;
		Value->TryGetStringField(TEXT("asset"), AssetPath);
		Value->TryGetStringField(TEXT("script"), AssetPath);
		UNiagaraScript* DynamicInputScript = AssetPath.IsEmpty()
			? nullptr
			: LoadObject<UNiagaraScript>(nullptr, *NormalizeObjectPath(AssetPath));
		if (!DynamicInputScript || DynamicInputScript->GetUsage() != ENiagaraScriptUsage::DynamicInput)
		{
			OutError = FString::Printf(TEXT("Dynamic-input script not found or invalid: %s"), *AssetPath);
			return false;
		}
		Input->SetDynamicInput(DynamicInputScript);
	}
	else if (Mode == TEXT("data_interface"))
	{
		UClass* DataInterfaceClass = InputType.GetClass();
		if (!DataInterfaceClass || !DataInterfaceClass->IsChildOf<UNiagaraDataInterface>())
		{
			OutError = FString::Printf(
				TEXT("Input '%s' is not a data-interface type (actual type: %s)"),
				*Input->GetInputParameterHandle().GetName().ToString(),
				*InputType.GetName());
			return false;
		}

		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		FString PropertyValues;
		Value->TryGetStringField(TEXT("property_values"), PropertyValues);
		if (!PropertyValues.IsEmpty())
		{
			Properties = ParseObject(PropertyValues);
			if (!Properties.IsValid())
			{
				OutError = TEXT("data_interface property_values must contain a JSON object");
				return false;
			}
		}
		else
		{
			const TSharedPtr<FJsonObject>* PropertiesPtr = nullptr;
			if (Value->TryGetObjectField(TEXT("properties"), PropertiesPtr) && PropertiesPtr && PropertiesPtr->IsValid())
			{
				Properties = *PropertiesPtr;
			}
		}

		bool bCallbackInvoked = false;
		bool bPropertiesApplied = true;
		FString PropertyError;
		Input->SetDataInterfaceValueExternal(DataInterfaceClass, [&](UNiagaraDataInterface* DataInterface)
		{
			bCallbackInvoked = true;
			if (!DataInterface)
			{
				bPropertiesApplied = false;
				PropertyError = TEXT("Niagara failed to create the stack-input data interface");
				return;
			}
			bPropertiesApplied = ApplyJsonProperties(DataInterface, Properties, PropertyError);
		});
		if (!bCallbackInvoked || !bPropertiesApplied)
		{
			OutError = PropertyError.IsEmpty()
				? TEXT("Niagara failed to set the stack-input data interface")
				: PropertyError;
			return false;
		}
	}
	else
	{
		OutError = TEXT("set_stack_input value.mode must be local, linked_parameter, hlsl, dynamic_input, or data_interface");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> CanonicalPathJson;
	for (const FName& Name : CanonicalNameStack)
	{
		CanonicalPathJson.Add(MakeShared<FJsonValueString>(Name.ToString()));
	}
	System->MarkPackageDirty();
	Result->SetBoolField(TEXT("changed"), true);
	Result->SetStringField(TEXT("node_guid"), ModuleNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	Result->SetStringField(TEXT("module"), ModuleNode->GetFunctionName());
	Result->SetStringField(TEXT("input_type"), InputType.GetName());
	Result->SetStringField(TEXT("value_mode"), Mode);
	Result->SetArrayField(TEXT("input_name_stack"), CanonicalPathJson);
	Result->SetBoolField(TEXT("stable_stack_lookup"), true);
	Result->SetBoolField(TEXT("usage_id_aware_stack_lookup"), RequiresUsageIdStackLookup(GraphContext.Usage));
	return true;
}

static bool ApplySetStackInput(
	UNiagaraSystem* System,
	const TSharedPtr<FJsonObject>& Operation,
	const TMap<FString, FCreatedNodeRecord>& CreatedNodes,
	const TSharedPtr<FJsonObject>& Result,
	FString& OutError)
{
	TSharedPtr<FJsonObject> NodeReference;
	const TSharedPtr<FJsonObject>* NodeReferencePtr = nullptr;
	if (Operation->TryGetObjectField(TEXT("node"), NodeReferencePtr) && NodeReferencePtr && NodeReferencePtr->IsValid())
	{
		NodeReference = *NodeReferencePtr;
	}
	else
	{
		NodeReference = Operation;
	}
	UNiagaraNodeFunctionCall* Node = Cast<UNiagaraNodeFunctionCall>(ResolveNodeReference(System, NodeReference, CreatedNodes, OutError));
	if (!Node)
	{
		if (OutError.IsEmpty()) OutError = TEXT("set_stack_input node must be a Niagara function-call module");
		return false;
	}

	FNiagaraGraphContext GraphContext;
	FString CreatedOperationId;
	NodeReference->TryGetStringField(TEXT("operation"), CreatedOperationId);
	NodeReference->TryGetStringField(TEXT("operation_id"), CreatedOperationId);
	if (!CreatedOperationId.IsEmpty())
	{
		const FCreatedNodeRecord* Created = CreatedNodes.Find(CreatedOperationId);
		if (!Created)
		{
			OutError = FString::Printf(TEXT("Created-node operation not found: %s"), *CreatedOperationId);
			return false;
		}
		GraphContext = Created->Context;
	}
	else if (!ResolveContextForNodeReference(System, Node, NodeReference, GraphContext, OutError))
	{
		return false;
	}

	TArray<FName> RequestedNameStack;
	const TArray<TSharedPtr<FJsonValue>>* InputNameStack = nullptr;
	if (Operation->TryGetArrayField(TEXT("input_name_stack"), InputNameStack) && InputNameStack)
	{
		for (const TSharedPtr<FJsonValue>& NameValue : *InputNameStack)
		{
			if (NameValue.IsValid() && NameValue->Type == EJson::String)
			{
				RequestedNameStack.Add(*NameValue->AsString());
			}
		}
	}
	else
	{
		FString InputName;
		Operation->TryGetStringField(TEXT("input"), InputName);
		Operation->TryGetStringField(TEXT("input_name"), InputName);
		if (!InputName.IsEmpty())
		{
			RequestedNameStack.Add(*InputName);
		}
	}
	if (RequestedNameStack.IsEmpty())
	{
		OutError = TEXT("set_stack_input requires input/input_name or input_name_stack");
		return false;
	}

	const TSharedPtr<FJsonObject>* ValuePtr = nullptr;
	if (!Operation->TryGetObjectField(TEXT("value"), ValuePtr) || !ValuePtr || !ValuePtr->IsValid())
	{
		OutError = TEXT("set_stack_input requires a value object");
		return false;
	}
	const TSharedPtr<FJsonObject> Value = *ValuePtr;
	FString Mode;
	Value->TryGetStringField(TEXT("mode"), Mode);
	Mode.ToLowerInline();
	return ApplyUsageAwareStackInputValue(
		System,
		GraphContext,
		Node,
		RequestedNameStack,
		Value,
		Mode,
		Result,
		OutError);
}

static bool ApplySetStackInputs(
	UNiagaraSystem* System,
	const TSharedPtr<FJsonObject>& Operation,
	const TMap<FString, FCreatedNodeRecord>& CreatedNodes,
	const TSharedPtr<FJsonObject>& Result,
	FString& OutError)
{
	const TSharedPtr<FJsonObject>* NodeReferencePtr = nullptr;
	TSharedPtr<FJsonObject> NodeReference;
	if (Operation->TryGetObjectField(TEXT("node"), NodeReferencePtr) && NodeReferencePtr && NodeReferencePtr->IsValid())
	{
		NodeReference = *NodeReferencePtr;
	}
	else
	{
		NodeReference = Operation;
	}

	UNiagaraNodeFunctionCall* Node = Cast<UNiagaraNodeFunctionCall>(ResolveNodeReference(System, NodeReference, CreatedNodes, OutError));
	if (!Node)
	{
		if (OutError.IsEmpty()) OutError = TEXT("set_stack_inputs node must be a Niagara function-call module");
		return false;
	}

	FNiagaraGraphContext GraphContext;
	FString CreatedOperationId;
	NodeReference->TryGetStringField(TEXT("operation"), CreatedOperationId);
	NodeReference->TryGetStringField(TEXT("operation_id"), CreatedOperationId);
	if (!CreatedOperationId.IsEmpty())
	{
		const FCreatedNodeRecord* Created = CreatedNodes.Find(CreatedOperationId);
		if (!Created)
		{
			OutError = FString::Printf(TEXT("Created-node operation not found: %s"), *CreatedOperationId);
			return false;
		}
		GraphContext = Created->Context;
	}
	else if (!ResolveContextForNodeReference(System, Node, NodeReference, GraphContext, OutError))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
	if (!Operation->TryGetArrayField(TEXT("inputs"), Inputs) || !Inputs)
	{
		OutError = TEXT("set_stack_inputs requires an inputs array");
		return false;
	}

	FNiagaraExternalEditContext ExternalContext(System);
	TArray<TSharedPtr<FJsonValue>> InputResults;
	for (int32 Index = 0; Index < Inputs->Num(); ++Index)
	{
		const TSharedPtr<FJsonObject> InputEdit = (*Inputs)[Index].IsValid() ? (*Inputs)[Index]->AsObject() : nullptr;
		if (!InputEdit.IsValid())
		{
			OutError = FString::Printf(TEXT("set_stack_inputs.inputs[%d] must be an object"), Index);
			return false;
		}

		TArray<FName> RequestedNameStack;
		const TArray<TSharedPtr<FJsonValue>>* InputNameStack = nullptr;
		if (InputEdit->TryGetArrayField(TEXT("input_name_stack"), InputNameStack) && InputNameStack)
		{
			for (const TSharedPtr<FJsonValue>& NameValue : *InputNameStack)
			{
				if (NameValue.IsValid() && NameValue->Type == EJson::String)
				{
					RequestedNameStack.Add(*NameValue->AsString());
				}
			}
		}
		else
		{
			FString InputName;
			InputEdit->TryGetStringField(TEXT("input"), InputName);
			InputEdit->TryGetStringField(TEXT("input_name"), InputName);
			if (!InputName.IsEmpty())
			{
				RequestedNameStack.Add(*InputName);
			}
		}
		if (RequestedNameStack.IsEmpty())
		{
			OutError = FString::Printf(TEXT("set_stack_inputs.inputs[%d] requires input/input_name or input_name_stack"), Index);
			return false;
		}

		const TSharedPtr<FJsonObject>* ValuePtr = nullptr;
		if (!InputEdit->TryGetObjectField(TEXT("value"), ValuePtr) || !ValuePtr || !ValuePtr->IsValid())
		{
			OutError = FString::Printf(TEXT("set_stack_inputs.inputs[%d] requires a value object"), Index);
			return false;
		}
		const TSharedPtr<FJsonObject> Value = *ValuePtr;
		FString Mode;
		Value->TryGetStringField(TEXT("mode"), Mode);
		Mode.ToLowerInline();

		TSharedPtr<FJsonObject> InputResult = MakeShared<FJsonObject>();
		InputResult->SetNumberField(TEXT("index"), Index);
		FString InputError;
		if (!ApplyUsageAwareStackInputValue(
			System,
			GraphContext,
			Node,
			RequestedNameStack,
			Value,
			Mode,
			InputResult,
			InputError,
			&ExternalContext))
		{
			InputResult->SetStringField(TEXT("status"), TEXT("error"));
			InputResult->SetStringField(TEXT("message"), InputError);
			InputResults.Add(MakeShared<FJsonValueObject>(InputResult));
			Result->SetArrayField(TEXT("inputs"), InputResults);
			OutError = FString::Printf(TEXT("set_stack_inputs.inputs[%d] failed: %s"), Index, *InputError);
			return false;
		}
		InputResult->SetStringField(TEXT("status"), TEXT("applied"));
		InputResults.Add(MakeShared<FJsonValueObject>(InputResult));
	}

	Result->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	Result->SetStringField(TEXT("module"), Node->GetFunctionName());
	Result->SetNumberField(TEXT("input_count"), Inputs->Num());
	Result->SetArrayField(TEXT("inputs"), InputResults);
	Result->SetBoolField(TEXT("changed"), Inputs->Num() > 0);
	return true;
}

static bool ApplySetModuleEnabled(
	UNiagaraSystem* System,
	const TSharedPtr<FJsonObject>& Operation,
	const TMap<FString, FCreatedNodeRecord>& CreatedNodes,
	const TSharedPtr<FJsonObject>& Result,
	FString& OutError)
{
	const TSharedPtr<FJsonObject>* NodeReferencePtr = nullptr;
	TSharedPtr<FJsonObject> NodeReference;
	if (Operation->TryGetObjectField(TEXT("node"), NodeReferencePtr) && NodeReferencePtr && NodeReferencePtr->IsValid())
	{
		NodeReference = *NodeReferencePtr;
	}
	else
	{
		NodeReference = Operation;
	}

	UNiagaraNodeFunctionCall* Node = Cast<UNiagaraNodeFunctionCall>(ResolveNodeReference(System, NodeReference, CreatedNodes, OutError));
	if (!Node)
	{
		if (OutError.IsEmpty()) OutError = TEXT("set_module_enabled node must be a Niagara function-call module");
		return false;
	}

	FNiagaraGraphContext GraphContext;
	FString CreatedOperationId;
	NodeReference->TryGetStringField(TEXT("operation"), CreatedOperationId);
	NodeReference->TryGetStringField(TEXT("operation_id"), CreatedOperationId);
	if (!CreatedOperationId.IsEmpty())
	{
		const FCreatedNodeRecord* Created = CreatedNodes.Find(CreatedOperationId);
		if (!Created)
		{
			OutError = FString::Printf(TEXT("Created-node operation not found: %s"), *CreatedOperationId);
			return false;
		}
		GraphContext = Created->Context;
	}
	else if (!ResolveContextForNodeReference(System, Node, NodeReference, GraphContext, OutError))
	{
		return false;
	}

	bool bEnabled = true;
	bool bHasEnabled = Operation->TryGetBoolField(TEXT("enabled"), bEnabled);
	if (!bHasEnabled)
	{
		bHasEnabled = Operation->TryGetBoolField(TEXT("value"), bEnabled);
	}
	if (!bHasEnabled)
	{
		FString Op;
		Operation->TryGetStringField(TEXT("op"), Op);
		Op.ToLowerInline();
		if (Op == TEXT("enable_module"))
		{
			bEnabled = true;
			bHasEnabled = true;
		}
		else if (Op == TEXT("disable_module"))
		{
			bEnabled = false;
			bHasEnabled = true;
		}
	}
	if (!bHasEnabled)
	{
		OutError = TEXT("set_module_enabled requires enabled/value");
		return false;
	}

	FNiagaraExternalEditContext ExternalContext(System);
	UNiagaraStackScriptItemGroup* ScriptGroup = FindUsageAwareScriptGroup(ExternalContext, GraphContext, OutError);
	UNiagaraStackModuleItem* ModuleItem = ScriptGroup ? FindUsageAwareModule(ScriptGroup, Node, OutError) : nullptr;
	if (!ModuleItem)
	{
		return false;
	}

	const bool bWasEnabled = ModuleItem->GetIsEnabled();
	ModuleItem->SetIsEnabled(bEnabled);
	System->MarkPackageDirty();
	Result->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	Result->SetStringField(TEXT("module"), Node->GetFunctionName());
	Result->SetBoolField(TEXT("previous_enabled"), bWasEnabled);
	Result->SetBoolField(TEXT("enabled"), bEnabled);
	Result->SetBoolField(TEXT("changed"), bWasEnabled != bEnabled);
	return true;
}
}

FString UMassBattleNiagaraMCPApi::MCP_NiagaraGetApiStatus()
{
	using namespace MassBattleNiagaraMCP;

	TSharedPtr<FJsonObject> Root = MakeSuccessObject();
	Root->SetStringField(TEXT("api_name"), TEXT("MassBattleNiagaraMCP"));
	Root->SetStringField(TEXT("version"), TEXT("0.2.0"));
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
	Tools.Add(Tool(TEXT("MCP_NiagaraReadGraph"), TEXT("niagara.graph.read"), TEXT("Read script graphs with stable node/pin ids and explicit links."), TEXT("SystemPath, SelectorJson")));
	Tools.Add(Tool(TEXT("MCP_NiagaraCompareSystems"), TEXT("niagara.validate"), TEXT("Compare a source system with a duplicate or translation using source-neutral semantic fingerprints."), TEXT("SourceSystemPath, TargetSystemPath, OptionsJson")));
	Tools.Add(Tool(TEXT("MCP_NiagaraReadAll"), TEXT("niagara.read"), TEXT("Read full reflected Niagara data plus all module nodes."), TEXT("SystemPath, OptionsJson")));
	Tools.Add(Tool(TEXT("MCP_NiagaraExportText"), TEXT("niagara.text"), TEXT("Write a deterministic text dump for LLM reading."), TEXT("SystemPath, OptionsJson")));
	Tools.Add(Tool(TEXT("MCP_NiagaraMergeWrite"), TEXT("niagara.write"), TEXT("Union-merge property writes on system/emitter_data/renderer targets; never deletes."), TEXT("SystemPath, PatchJson, bSaveAssets")));
	Tools.Add(Tool(TEXT("MCP_NiagaraSetModulePin"), TEXT("niagara.write"), TEXT("Set one unlinked function-call module pin default value."), TEXT("SystemPath, SelectorJson, PinName, ValueText, bSaveAssets")));
	Tools.Add(Tool(TEXT("MCP_NiagaraApplyGraphEdit"), TEXT("niagara.graph.write"), TEXT("Apply ordered user-DI/module/link edits, compile once, and validate lossless preservation before saving."), TEXT("SystemPath, EditJson, bSaveAssets")));
	Tools.Add(Tool(TEXT("MCP_NiagaraSetEmitterEnabled"), TEXT("niagara.write"), TEXT("Set one emitter handle enabled state explicitly."), TEXT("SystemPath, EmitterName, bEnabled, bSaveAssets")));
	Tools.Add(Tool(TEXT("MCP_NiagaraDelete"), TEXT("niagara.delete"), TEXT("Explicit destructive operations such as renderer removal or user-parameter removal."), TEXT("SystemPath, DeleteJson, bSaveAssets")));
	Tools.Add(Tool(TEXT("MCP_NiagaraAddSpriteRenderer"), TEXT("niagara.write"), TEXT("Add one configured sprite renderer to an existing emitter."), TEXT("SystemPath, EmitterName, RendererJson, bSaveAssets")));
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

FString UMassBattleNiagaraMCPApi::MCP_NiagaraReadGraph(const FString& SystemPath, const FString& SelectorJson)
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

	bool bIncludeNodes = true;
	bool bIncludePins = true;
	bool bIncludeStackInputs = true;
	bool bIncludeCompileState = true;
	int32 MaxNodes = 5000;
	Selector->TryGetBoolField(TEXT("include_nodes"), bIncludeNodes);
	Selector->TryGetBoolField(TEXT("include_pins"), bIncludePins);
	Selector->TryGetBoolField(TEXT("include_stack_inputs"), bIncludeStackInputs);
	Selector->TryGetBoolField(TEXT("include_compile_state"), bIncludeCompileState);
	Selector->TryGetNumberField(TEXT("max_nodes"), MaxNodes);
	MaxNodes = FMath::Clamp(MaxNodes, 1, 50000);

	TArray<FNiagaraGraphContext> Contexts;
	FString SelectorError;
	if (!SelectGraphContexts(System, Selector, Contexts, SelectorError, false))
	{
		return MakeErrorJson(SelectorError);
	}

	TUniquePtr<FNiagaraExternalEditContext> StackExternalContext;
	if (bIncludeNodes && bIncludeStackInputs)
	{
		StackExternalContext = MakeUnique<FNiagaraExternalEditContext>(System);
	}

	TArray<TSharedPtr<FJsonValue>> Graphs;
	for (const FNiagaraGraphContext& Context : Contexts)
	{
		Graphs.Add(MakeShared<FJsonValueObject>(GraphContextToJson(System, Context, StackExternalContext.Get(), bIncludeNodes, bIncludePins, bIncludeStackInputs, MaxNodes)));
	}

	TSharedPtr<FJsonObject> Root = MakeSuccessObject();
	Root->SetStringField(TEXT("system"), System->GetPathName());
	Root->SetNumberField(TEXT("graph_count"), Graphs.Num());
	Root->SetArrayField(TEXT("graphs"), Graphs);
	if (bIncludeCompileState)
	{
		bool bHasCompileErrors = false;
		Root->SetObjectField(TEXT("compile_state"), CompileStateToJson(System, bHasCompileErrors));
	}
	return ToCondensedJsonString(Root);
}

FString UMassBattleNiagaraMCPApi::MCP_NiagaraCompareSystems(
	const FString& SourceSystemPath,
	const FString& TargetSystemPath,
	const FString& OptionsJson)
{
	using namespace MassBattleNiagaraMCP;

	TSharedPtr<FJsonObject> Options = ParseObject(OptionsJson);
	if (!Options.IsValid())
	{
		return MakeErrorJson(TEXT("OptionsJson must be a JSON object"));
	}

	FString LoadError;
	UNiagaraSystem* SourceSystem = LoadSystem(SourceSystemPath, LoadError);
	if (!SourceSystem)
	{
		return MakeErrorJson(FString::Printf(TEXT("Source: %s"), *LoadError));
	}
	UNiagaraSystem* TargetSystem = LoadSystem(TargetSystemPath, LoadError);
	if (!TargetSystem)
	{
		return MakeErrorJson(FString::Printf(TEXT("Target: %s"), *LoadError));
	}
	if (SourceSystem == TargetSystem)
	{
		return MakeErrorJson(TEXT("SourceSystemPath and TargetSystemPath must resolve to different assets"));
	}

	FString Mode = TEXT("translation");
	Options->TryGetStringField(TEXT("mode"), Mode);
	Mode.ToLowerInline();
	if (Mode != TEXT("exact") && Mode != TEXT("translation"))
	{
		return MakeErrorJson(TEXT("options.mode must be exact or translation"));
	}

	FNiagaraPreservationState SourceState;
	FNiagaraPreservationState TargetState;
	CapturePreservationState(SourceSystem, SourceState);
	CapturePreservationState(TargetSystem, TargetState);
	NormalizePreservationStateForSystem(SourceSystem, SourceState);
	NormalizePreservationStateForSystem(TargetSystem, TargetState);

	bool bAllSourcePreserved = false;
	TSharedPtr<FJsonObject> Preservation = BuildPreservationValidation(SourceState, TargetState, bAllSourcePreserved);
	TArray<FString> RemovedEdges;
	TArray<FString> UnexpectedRemovedEdges;
	const bool bRemovedEdgePolicyPassed = ValidateRemovedEdgePolicy(
		SourceState,
		TargetState,
		Options,
		RemovedEdges,
		UnexpectedRemovedEdges);
	Preservation->SetBoolField(TEXT("removed_edge_policy_passed"), bRemovedEdgePolicyPassed);
	Preservation->SetArrayField(TEXT("unexpected_removed_edges"), StringsToJson(UnexpectedRemovedEdges, 200));

	const bool bExactMatch = ArePreservationStatesExact(SourceState, TargetState);
	const bool bSourcePreserved = bAllSourcePreserved && bRemovedEdgePolicyPassed;

	bool bSourceHasCompileErrors = false;
	bool bTargetHasCompileErrors = false;
	TSharedPtr<FJsonObject> SourceCompileState = CompileStateToJson(SourceSystem, bSourceHasCompileErrors);
	TSharedPtr<FJsonObject> TargetCompileState = CompileStateToJson(TargetSystem, bTargetHasCompileErrors);
	bool bSourceHasWarnings = false;
	bool bTargetHasWarnings = false;
	SourceCompileState->TryGetBoolField(TEXT("has_warnings"), bSourceHasWarnings);
	TargetCompileState->TryGetBoolField(TEXT("has_warnings"), bTargetHasWarnings);

	// An unsaved duplicate can be structurally exact before Niagara has queued its first
	// compile.  Exact mode is the source-neutral clone gate; translation mode is the
	// post-edit delivery gate and therefore requires runtime readiness by default.
	bool bRequireReadyToRun = Mode != TEXT("exact");
	bool bRequireNoCompileErrors = true;
	bool bRequireNoWarnings = false;
	Options->TryGetBoolField(TEXT("require_ready_to_run"), bRequireReadyToRun);
	Options->TryGetBoolField(TEXT("require_no_compile_errors"), bRequireNoCompileErrors);
	Options->TryGetBoolField(TEXT("require_no_warnings"), bRequireNoWarnings);

	const bool bStructuralPass = Mode == TEXT("exact") ? bExactMatch : bSourcePreserved;
	const bool bCompilePass = (!bRequireNoCompileErrors || (!bSourceHasCompileErrors && !bTargetHasCompileErrors))
		&& (!bRequireNoWarnings || (!bSourceHasWarnings && !bTargetHasWarnings))
		&& (!bRequireReadyToRun || (SourceSystem->IsReadyToRun() && TargetSystem->IsReadyToRun()));
	const bool bComparisonPassed = bStructuralPass && bCompilePass;

	TSharedPtr<FJsonObject> Counts = MakeShared<FJsonObject>();
	Counts->SetNumberField(TEXT("source_emitters"), SourceState.Emitters.Num());
	Counts->SetNumberField(TEXT("target_emitters"), TargetState.Emitters.Num());
	Counts->SetNumberField(TEXT("source_renderers"), SourceState.Renderers.Num());
	Counts->SetNumberField(TEXT("target_renderers"), TargetState.Renderers.Num());
	Counts->SetNumberField(TEXT("source_modules"), SourceState.Modules.Num());
	Counts->SetNumberField(TEXT("target_modules"), TargetState.Modules.Num());
	Counts->SetNumberField(TEXT("source_graph_nodes"), SourceState.GraphNodes.Num());
	Counts->SetNumberField(TEXT("target_graph_nodes"), TargetState.GraphNodes.Num());
	Counts->SetNumberField(TEXT("source_graph_pins"), SourceState.GraphPins.Num());
	Counts->SetNumberField(TEXT("target_graph_pins"), TargetState.GraphPins.Num());
	Counts->SetNumberField(TEXT("source_edges"), SourceState.Edges.Num());
	Counts->SetNumberField(TEXT("target_edges"), TargetState.Edges.Num());

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("success"), bComparisonPassed);
	Root->SetStringField(TEXT("mode"), Mode);
	Root->SetStringField(TEXT("source"), SourceSystem->GetPathName());
	Root->SetStringField(TEXT("target"), TargetSystem->GetPathName());
	Root->SetBoolField(TEXT("exact_match"), bExactMatch);
	Root->SetBoolField(TEXT("source_preserved"), bSourcePreserved);
	Root->SetBoolField(TEXT("compile_policy_passed"), bCompilePass);
	Root->SetBoolField(TEXT("comparison_passed"), bComparisonPassed);
	Root->SetObjectField(TEXT("counts"), Counts);
	Root->SetObjectField(TEXT("preservation"), Preservation);
	Root->SetObjectField(TEXT("source_compile_state"), SourceCompileState);
	Root->SetObjectField(TEXT("target_compile_state"), TargetCompileState);
	if (!bComparisonPassed)
	{
		Root->SetStringField(TEXT("error"), Mode == TEXT("exact")
			? TEXT("Target is not an exact source-neutral clone or does not satisfy compile policy")
			: TEXT("Target does not preserve the source under the declared translation policy"));
	}
	return ToJsonString(Root);
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

FString UMassBattleNiagaraMCPApi::MCP_NiagaraApplyGraphEdit(const FString& SystemPath, const FString& EditJson, bool bSaveAssets)
{
	using namespace MassBattleNiagaraMCP;

	TSharedPtr<FJsonObject> Edit = ParseObject(EditJson);
	if (!Edit.IsValid())
	{
		return MakeErrorJson(TEXT("EditJson must be a JSON object"));
	}

	FString LoadError;
	UNiagaraSystem* System = LoadSystem(SystemPath, LoadError);
	if (!System)
	{
		return MakeErrorJson(LoadError);
	}

	const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
	if (!Edit->TryGetArrayField(TEXT("operations"), Operations))
	{
		Edit->TryGetArrayField(TEXT("edits"), Operations);
	}
	if (!Operations)
	{
		return MakeErrorJson(TEXT("EditJson requires an operations array (it may be empty for compile/validation only)"));
	}

	// Validate the stable operation ids before any mutation so later references are deterministic.
	TSet<FString> OperationIds;
	for (int32 Index = 0; Index < Operations->Num(); ++Index)
	{
		const TSharedPtr<FJsonObject> Operation = (*Operations)[Index].IsValid() ? (*Operations)[Index]->AsObject() : nullptr;
		if (!Operation.IsValid())
		{
			return MakeErrorJson(FString::Printf(TEXT("operations[%d] must be an object"), Index));
		}
		FString Op;
		FString Id;
		Operation->TryGetStringField(TEXT("op"), Op);
		Operation->TryGetStringField(TEXT("type"), Op);
		Operation->TryGetStringField(TEXT("id"), Id);
		if (Op.IsEmpty())
		{
			return MakeErrorJson(FString::Printf(TEXT("operations[%d] requires op/type"), Index));
		}
		if (!Id.IsEmpty())
		{
			if (OperationIds.Contains(Id))
			{
				return MakeErrorJson(FString::Printf(TEXT("Duplicate graph-edit operation id: %s"), *Id));
			}
			OperationIds.Add(Id);
		}
	}

	FNiagaraPreservationState BeforeState;
	CapturePreservationState(System, BeforeState);
	TMap<FString, FCreatedNodeRecord> CreatedNodes;
	TArray<TSharedPtr<FJsonValue>> OperationResults;
	bool bOperationFailed = false;
	bool bAnyApplied = false;
	int32 FailedIndex = INDEX_NONE;
	FString FailureMessage;

	System->Modify();
	for (int32 Index = 0; Index < Operations->Num(); ++Index)
	{
		const TSharedPtr<FJsonObject> Operation = (*Operations)[Index]->AsObject();
		FString Op;
		Operation->TryGetStringField(TEXT("op"), Op);
		Operation->TryGetStringField(TEXT("type"), Op);
		Op.ToLowerInline();
		TSharedPtr<FJsonObject> Result = MakeOperationResult(Index, Operation, TEXT("pending"), TEXT(""));
		FString OperationError;
		bool bApplied = false;

		if (Op == TEXT("add_user_parameter") || Op == TEXT("add_user_value"))
		{
			bApplied = ApplyAddUserParameter(System, Operation, Result, OperationError);
		}
		else if (Op == TEXT("add_user_data_interface") || Op == TEXT("add_user_di"))
		{
			bApplied = ApplyAddUserDataInterface(System, Operation, Result, OperationError);
		}
		else if (Op == TEXT("insert_module") || Op == TEXT("add_module"))
		{
			bApplied = ApplyInsertModule(System, Operation, CreatedNodes, Result, OperationError);
		}
		else if (Op == TEXT("connect_pins") || Op == TEXT("connect"))
		{
			bApplied = ApplyConnectPins(System, Operation, CreatedNodes, Result, OperationError);
		}
		else if (Op == TEXT("disconnect_pins") || Op == TEXT("disconnect"))
		{
			bApplied = ApplyDisconnectPins(System, Operation, CreatedNodes, Result, OperationError);
		}
		else if (Op == TEXT("disconnect_pin"))
		{
			bApplied = ApplyDisconnectPin(System, Operation, CreatedNodes, Result, OperationError);
		}
		else if (Op == TEXT("rewire_pin") || Op == TEXT("insert_between"))
		{
			bApplied = ApplyRewirePin(System, Operation, CreatedNodes, Result, OperationError);
		}
		else if (Op == TEXT("set_stack_inputs") || Op == TEXT("link_stack_inputs"))
		{
			bApplied = ApplySetStackInputs(System, Operation, CreatedNodes, Result, OperationError);
		}
		else if (Op == TEXT("set_stack_input") || Op == TEXT("link_stack_input"))
		{
			bApplied = ApplySetStackInput(System, Operation, CreatedNodes, Result, OperationError);
		}
		else if (Op == TEXT("set_module_enabled") || Op == TEXT("enable_module") || Op == TEXT("disable_module"))
		{
			bApplied = ApplySetModuleEnabled(System, Operation, CreatedNodes, Result, OperationError);
		}
		else
		{
			OperationError = FString::Printf(TEXT("Unknown graph-edit operation: %s"), *Op);
		}

		if (!bApplied)
		{
			bOperationFailed = true;
			FailedIndex = Index;
			FailureMessage = OperationError.IsEmpty() ? TEXT("Graph-edit operation failed") : OperationError;
			Result->SetStringField(TEXT("status"), TEXT("error"));
			Result->SetStringField(TEXT("message"), FailureMessage);
			OperationResults.Add(MakeShared<FJsonValueObject>(Result));
			break;
		}

		bAnyApplied = true;
		Result->SetStringField(TEXT("status"), TEXT("applied"));
		Result->SetStringField(TEXT("message"), TEXT("operation applied"));
		OperationResults.Add(MakeShared<FJsonValueObject>(Result));
	}

	// One explicit compile barrier makes the result deterministic even when individual editor
	// operations queued their own refreshes. Include GPU shader compilation for mixed CPU/GPU FX.
	System->PostEditChange();
	System->MarkPackageDirty();
	System->RequestCompile(true);
	System->WaitForCompilationComplete(true, false);

	bool bHasCompileErrors = false;
	TSharedPtr<FJsonObject> CompileState = CompileStateToJson(System, bHasCompileErrors);
	bool bHasWarnings = false;
	CompileState->TryGetBoolField(TEXT("has_warnings"), bHasWarnings);
	const bool bReadyToRun = System->IsReadyToRun();

	FNiagaraPreservationState AfterState;
	CapturePreservationState(System, AfterState);
	bool bAllPreserved = false;
	TSharedPtr<FJsonObject> Preservation = BuildPreservationValidation(BeforeState, AfterState, bAllPreserved);

	bool bRequireNoWarnings = false;
	bool bRequireReadyToRun = true;
	const TSharedPtr<FJsonObject>* ValidationOptions = nullptr;
	TSharedPtr<FJsonObject> ValidationOptionsObject;
	if (Edit->TryGetObjectField(TEXT("validation"), ValidationOptions) && ValidationOptions && ValidationOptions->IsValid())
	{
		ValidationOptionsObject = *ValidationOptions;
		(*ValidationOptions)->TryGetBoolField(TEXT("require_no_warnings"), bRequireNoWarnings);
		(*ValidationOptions)->TryGetBoolField(TEXT("require_ready_to_run"), bRequireReadyToRun);
	}
	TArray<FString> RemovedEdges;
	TArray<FString> UnexpectedRemovedEdges;
	const bool bRemovedEdgePolicyPassed = ValidateRemovedEdgePolicy(
		BeforeState,
		AfterState,
		ValidationOptionsObject,
		RemovedEdges,
		UnexpectedRemovedEdges);
	Preservation->SetBoolField(TEXT("removed_edge_policy_passed"), bRemovedEdgePolicyPassed);
	Preservation->SetArrayField(TEXT("unexpected_removed_edges"), StringsToJson(UnexpectedRemovedEdges, 200));

	bool bValidationPassed = !bOperationFailed
		&& !bHasCompileErrors
		&& bAllPreserved
		&& bRemovedEdgePolicyPassed
		&& (!bRequireNoWarnings || !bHasWarnings)
		&& (!bRequireReadyToRun || bReadyToRun);

	bool bSaved = false;
	FString SaveError;
	if (bSaveAssets && bValidationPassed)
	{
		bSaved = SaveAsset(System, SaveError);
		if (!bSaved)
		{
			bValidationPassed = false;
			FailureMessage = SaveError;
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("success"), bValidationPassed);
	Root->SetStringField(TEXT("system"), System->GetPathName());
	Root->SetBoolField(TEXT("applied"), bAnyApplied);
	Root->SetBoolField(TEXT("partial_mutation"), bOperationFailed && bAnyApplied);
	Root->SetNumberField(TEXT("failed_operation_index"), FailedIndex);
	Root->SetBoolField(TEXT("compiled"), true);
	Root->SetBoolField(TEXT("saved"), bSaved);
	Root->SetBoolField(TEXT("save_requested"), bSaveAssets);
	Root->SetBoolField(TEXT("validation_passed"), bValidationPassed);
	Root->SetArrayField(TEXT("operations"), OperationResults);
	Root->SetObjectField(TEXT("compile_state"), CompileState);
	Root->SetObjectField(TEXT("preservation"), Preservation);
	if (!bValidationPassed)
	{
		if (FailureMessage.IsEmpty())
		{
			if (bHasCompileErrors) FailureMessage = TEXT("Niagara compile completed with errors");
			else if (!bAllPreserved) FailureMessage = TEXT("Lossless preservation validation failed");
			else if (!bRemovedEdgePolicyPassed) FailureMessage = TEXT("Graph edit removed source edges that were not explicitly allowed");
			else if (bRequireNoWarnings && bHasWarnings) FailureMessage = TEXT("Niagara compile completed with warnings while require_no_warnings=true");
			else if (bRequireReadyToRun && !bReadyToRun) FailureMessage = TEXT("Niagara system is not ready to run after compilation");
			else FailureMessage = TEXT("Graph edit validation failed");
		}
		Root->SetStringField(TEXT("error"), FailureMessage);
	}
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

FString UMassBattleNiagaraMCPApi::MCP_NiagaraAddSpriteRenderer(
	const FString& SystemPath,
	const FString& EmitterName,
	const FString& RendererJson,
	const bool bSaveAssets)
{
	using namespace MassBattleNiagaraMCP;

	TSharedPtr<FJsonObject> Config = ParseObject(RendererJson);
	if (!Config.IsValid())
	{
		return MakeErrorJson(TEXT("RendererJson must be a JSON object"));
	}

	FString LoadError;
	UNiagaraSystem* System = LoadSystem(SystemPath, LoadError);
	if (!System)
	{
		return MakeErrorJson(LoadError);
	}

	FNiagaraEmitterHandle* Handle = nullptr;
	if (!FindEmitterHandle(System, EmitterName, Handle) || !Handle || !Handle->GetInstance().Emitter)
	{
		return MakeErrorJson(FString::Printf(TEXT("Emitter not found: %s"), *EmitterName));
	}

	FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData();
	if (!EmitterData)
	{
		return MakeErrorJson(FString::Printf(TEXT("Emitter has no editable data: %s"), *EmitterName));
	}

	auto NormalizeUserName = [](FString Name)
	{
		Name.TrimStartAndEndInline();
		if (!Name.IsEmpty() && !Name.StartsWith(TEXT("User.")))
		{
			Name = TEXT("User.") + Name;
		}
		return Name;
	};

	FString MaterialPath;
	Config->TryGetStringField(TEXT("material"), MaterialPath);
	UMaterialInterface* Material = MaterialPath.IsEmpty()
		? nullptr
		: LoadObject<UMaterialInterface>(nullptr, *NormalizeObjectPath(MaterialPath));
	if (!MaterialPath.IsEmpty() && !Material)
	{
		return MakeErrorJson(FString::Printf(TEXT("Material not found: %s"), *MaterialPath));
	}

	FString MaterialUserParameter;
	Config->TryGetStringField(TEXT("material_user_parameter"), MaterialUserParameter);
	MaterialUserParameter = NormalizeUserName(MaterialUserParameter);

	FString EnabledUserParameter;
	Config->TryGetStringField(TEXT("enabled_user_parameter"), EnabledUserParameter);
	EnabledUserParameter = NormalizeUserName(EnabledUserParameter);

	const FNiagaraTypeDefinition MaterialType(UMaterialInterface::StaticClass());
	if (!MaterialUserParameter.IsEmpty())
	{
		System->GetExposedParameters().AddParameter(FNiagaraVariable(MaterialType, *MaterialUserParameter));
	}
	if (!EnabledUserParameter.IsEmpty())
	{
		System->GetExposedParameters().AddParameter(
			FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(), *EnabledUserParameter));
	}

	UNiagaraEmitter* Emitter = Handle->GetInstance().Emitter;
	UNiagaraSpriteRendererProperties* Renderer = NewObject<UNiagaraSpriteRendererProperties>(Emitter);
	if (!Renderer)
	{
		return MakeErrorJson(TEXT("Failed to allocate Niagara sprite renderer"));
	}

	Renderer->Material = Material;
	Renderer->SourceMode = ENiagaraRendererSourceDataMode::Particles;
	Renderer->Alignment = ENiagaraSpriteAlignment::Unaligned;
	Renderer->FacingMode = ENiagaraSpriteFacingMode::FaceCameraPlane;
	Renderer->SortMode = ENiagaraSortMode::None;
	Renderer->bCastShadows = false;
	Renderer->bEnableCameraDistanceCulling = false;
	Renderer->PixelCoverageMode = ENiagaraRendererPixelCoverageMode::Disabled;

	int32 SortOrderHint = 0;
	Config->TryGetNumberField(TEXT("sort_order"), SortOrderHint);
	Renderer->SortOrderHint = SortOrderHint;

	int32 RendererVisibility = 0;
	Config->TryGetNumberField(TEXT("renderer_visibility"), RendererVisibility);
	Renderer->RendererVisibility = static_cast<uint32>(FMath::Max(0, RendererVisibility));

	const FVersionedNiagaraEmitterBase VersionedEmitter = Handle->GetInstance().ToBase();
	auto SetBinding = [&Config, &VersionedEmitter, Renderer](
		const TCHAR* Field,
		const TCHAR* DefaultName,
		FNiagaraVariableAttributeBinding& Binding)
	{
		FString Name = DefaultName;
		Config->TryGetStringField(Field, Name);
		if (!Name.IsEmpty())
		{
			Binding.SetValue(*Name, VersionedEmitter, Renderer->SourceMode);
		}
	};

	SetBinding(TEXT("position_binding"), TEXT("Particles.Position"), Renderer->PositionBinding);
	SetBinding(TEXT("sprite_size_binding"), TEXT("Particles.SpriteSize"), Renderer->SpriteSizeBinding);
	SetBinding(TEXT("dynamic_material_binding"), TEXT("Particles.DynamicMaterialParameter"), Renderer->DynamicMaterialBinding);
	SetBinding(TEXT("visibility_binding"), TEXT("Particles.VisibilityTag"), Renderer->RendererVisibilityTagBinding);

	if (!MaterialUserParameter.IsEmpty())
	{
		Renderer->MaterialUserParamBinding = FNiagaraUserParameterBinding(MaterialType);
		Renderer->MaterialUserParamBinding.Parameter = FNiagaraVariable(MaterialType, *MaterialUserParameter);
	}
	if (!EnabledUserParameter.IsEmpty())
	{
		Renderer->RendererEnabledBinding.SetValue(*EnabledUserParameter, VersionedEmitter, Renderer->SourceMode);
	}

	const int32 RendererIndex = EmitterData->GetRenderers().Num();
	Emitter->AddRenderer(Renderer, EmitterData->Version.VersionGuid);
	System->bFixedBounds = true;
	System->SetFixedBounds(FBox(FVector(-100000000.0), FVector(100000000.0)));
	System->MarkPackageDirty();
	System->RequestCompile(true);
	System->WaitForCompilationComplete(true, false);

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
	Root->SetStringField(TEXT("emitter"), Handle->GetName().ToString());
	Root->SetNumberField(TEXT("renderer_index"), RendererIndex);
	Root->SetStringField(TEXT("renderer"), Renderer->GetPathName());
	Root->SetStringField(TEXT("material_user_parameter"), MaterialUserParameter);
	Root->SetStringField(TEXT("enabled_user_parameter"), EnabledUserParameter);
	Root->SetBoolField(TEXT("saved"), bSaved);
	return ToJsonString(Root);
}
