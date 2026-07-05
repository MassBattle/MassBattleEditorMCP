// Copyright (c) 2025-2026 Winyunq. All rights reserved.
#include "MassBattleUnitMCPApi.h"

#include "AnimToTextureDataAsset.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "DataAssets/MassBattleAgentConfigDataAsset.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "MassBattleEnums.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "NiagaraSystem.h"
#include "ObjectTools.h"
#include "Renderers/MassBattleAgentRenderer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY(LogMassBattleUnitMCPApi);

namespace MassBattleUnitMCP
{
static constexpr int32 MaxDefaultUnitList = 200;
static constexpr int32 MaxPropertyDepth = 5;

enum class EFieldRole : uint8
{
	AuthoringConfig,
	SharedTypeConfig,
	SpawnInitialState,
	RuntimeState,
	SystemOwned,
	Deprecated,
	VisibilityControl,
	ExtensionConfig
};

struct FResolvedPropertyPath
{
	FProperty* Property = nullptr;
	void* ValuePtr = nullptr;
	FString Error;
};

struct FPatchPreview
{
	FString Path;
	FString Operation;
	FString Before;
	FString After;
	FString Error;
};

static FString RoleToString(const EFieldRole Role)
{
	switch (Role)
	{
	case EFieldRole::AuthoringConfig: return TEXT("AuthoringConfig");
	case EFieldRole::SharedTypeConfig: return TEXT("SharedTypeConfig");
	case EFieldRole::SpawnInitialState: return TEXT("SpawnInitialState");
	case EFieldRole::RuntimeState: return TEXT("RuntimeState");
	case EFieldRole::SystemOwned: return TEXT("SystemOwned");
	case EFieldRole::Deprecated: return TEXT("Deprecated");
	case EFieldRole::VisibilityControl: return TEXT("VisibilityControl");
	case EFieldRole::ExtensionConfig: return TEXT("ExtensionConfig");
	default: return TEXT("Unknown");
	}
}

static EFieldRole ClassifyTopLevelField(const FString& Name)
{
	static const TSet<FString> RuntimeFields = {
		TEXT("Agent"), TEXT("Locating"), TEXT("Rotating"), TEXT("EventRT"), TEXT("Visualizing"),
		TEXT("Animating"), TEXT("Tracing"), TEXT("TracingShared"), TEXT("Moving"), TEXT("Navigating"),
		TEXT("Gathering"), TEXT("Appearing"), TEXT("Patrolling"), TEXT("Reinforcing"),
		TEXT("Returning"), TEXT("Attacking"), TEXT("Debuffing"), TEXT("BeingSelect"),
		TEXT("BeingHit"), TEXT("Dying")
	};
	static const TSet<FString> SharedFields = {
		TEXT("PhysicsShared"), TEXT("CurveShared"), TEXT("TrafficShared"), TEXT("LODShared"), TEXT("AnimShared")
	};
	static const TSet<FString> DeprecatedFields = {
		TEXT("Scale"), TEXT("Curves"), TEXT("Animation"), TEXT("Render"), TEXT("RenderShared")
	};

	if (Name.StartsWith(TEXT("bShow")) || Name == TEXT("bHideRuntimeFragments"))
	{
		return EFieldRole::VisibilityControl;
	}
	if (Name.StartsWith(TEXT("bMigrated")) || Name == TEXT("DataVersion"))
	{
		return EFieldRole::SystemOwned;
	}
	if (RuntimeFields.Contains(Name))
	{
		return EFieldRole::RuntimeState;
	}
	if (SharedFields.Contains(Name))
	{
		return EFieldRole::SharedTypeConfig;
	}
	if (DeprecatedFields.Contains(Name))
	{
		return EFieldRole::Deprecated;
	}
	if (Name == TEXT("ExtraData"))
	{
		return EFieldRole::ExtensionConfig;
	}
	if (Name == TEXT("SubType") || Name == TEXT("StyleType") || Name == TEXT("Scaling"))
	{
		return EFieldRole::SpawnInitialState;
	}
	return EFieldRole::AuthoringConfig;
}

static bool ShouldSkipTopLevelField(const FString& Name, const TSharedPtr<FJsonObject>& Options)
{
	bool bOmitRuntime = true;
	bool bOmitSystem = true;
	bool bOmitDeprecated = true;
	bool bOmitVisibility = true;

	if (Options.IsValid())
	{
		Options->TryGetBoolField(TEXT("omit_runtime"), bOmitRuntime);
		Options->TryGetBoolField(TEXT("omit_system"), bOmitSystem);
		Options->TryGetBoolField(TEXT("omit_deprecated"), bOmitDeprecated);
		Options->TryGetBoolField(TEXT("omit_visibility"), bOmitVisibility);
	}

	const EFieldRole Role = ClassifyTopLevelField(Name);
	return (Role == EFieldRole::RuntimeState && bOmitRuntime)
		|| (Role == EFieldRole::SystemOwned && bOmitSystem)
		|| (Role == EFieldRole::Deprecated && bOmitDeprecated)
		|| (Role == EFieldRole::VisibilityControl && bOmitVisibility);
}

static bool ShouldSkipNestedField(const FString& Name)
{
	return Name == TEXT("DeterministicHash")
		|| Name.StartsWith(TEXT("bMigrated"))
		|| Name.StartsWith(TEXT("Deprecated"))
		|| Name.EndsWith(TEXT("_DEPRECATED"));
}

static FString ToJsonString(const TSharedPtr<FJsonObject>& Root)
{
	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	if (Root.IsValid())
	{
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	}
	return Output;
}

static FString ToJsonString(const TArray<TSharedPtr<FJsonValue>>& Array)
{
	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Array, Writer);
	return Output;
}

static TSharedPtr<FJsonObject> MakeSuccess()
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("success"), true);
	return Root;
}

static FString MakeErrorJson(const FString& Error)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("success"), false);
	Root->SetStringField(TEXT("error"), Error);
	return ToJsonString(Root);
}

static TSharedPtr<FJsonObject> ParseObject(const FString& Json)
{
	if (Json.TrimStartAndEnd().IsEmpty())
	{
		return MakeShared<FJsonObject>();
	}

	TSharedPtr<FJsonObject> Object;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
	{
		return nullptr;
	}
	return Object;
}

static TArray<TSharedPtr<FJsonValue>> ParseArrayField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
{
	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (Object.IsValid() && Object->TryGetArrayField(FieldName, Array))
	{
		return *Array;
	}
	return {};
}

static bool TryGetIntField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, int32& OutValue)
{
	double Number = 0.0;
	if (Object.IsValid() && Object->TryGetNumberField(FieldName, Number))
	{
		OutValue = static_cast<int32>(Number);
		return true;
	}
	return false;
}

static FString GetSavedRoot()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MassBattleEditorMCP"));
}

static FString GetPlanDir()
{
	return FPaths::Combine(GetSavedRoot(), TEXT("Plans"));
}

static FString GetAuditDir()
{
	return FPaths::Combine(GetSavedRoot(), TEXT("Audit"));
}

static FString GetExportDir()
{
	return FPaths::Combine(GetSavedRoot(), TEXT("Exports"));
}

static FString GetPluginRootDir()
{
	return FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("MassBattleEditorMCP"));
}

static FString GetUnitManagementStyleDir()
{
	return FPaths::Combine(GetPluginRootDir(), TEXT("Resources"), TEXT("UnitManagementStyles"));
}

static TArray<FString> ReadStringArrayField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
{
	TArray<FString> Result;
	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (Object.IsValid() && Object->TryGetArrayField(FieldName, Array))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			const FString Text = Value->AsString();
			if (!Text.IsEmpty())
			{
				Result.Add(Text);
			}
		}
	}
	return Result;
}

static TSharedPtr<FJsonObject> LoadJsonFileObject(const FString& FilePath)
{
	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
	{
		return nullptr;
	}
	return ParseObject(JsonText);
}

static FString FindConfigFileById(const FString& Directory, const FString& ConfigId)
{
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("*.json"), true, false);
	Files.Sort();

	const FString WantedId = ConfigId.IsEmpty() ? TEXT("default") : ConfigId;
	for (const FString& File : Files)
	{
		const FString BaseName = FPaths::GetBaseFilename(File);
		if (BaseName.Equals(WantedId, ESearchCase::IgnoreCase) || BaseName.StartsWith(WantedId + TEXT("."), ESearchCase::IgnoreCase))
		{
			return File;
		}

		TSharedPtr<FJsonObject> Config = LoadJsonFileObject(File);
		FString Id;
		if (Config.IsValid() && Config->TryGetStringField(TEXT("id"), Id) && Id.Equals(WantedId, ESearchCase::IgnoreCase))
		{
			return File;
		}
	}
	return FString();
}

static TSharedPtr<FJsonObject> LoadUnitStyleConfig(const TSharedPtr<FJsonObject>& Options)
{
	FString StyleId = TEXT("default");
	if (Options.IsValid())
	{
		Options->TryGetStringField(TEXT("style_profile"), StyleId);
		Options->TryGetStringField(TEXT("style_config"), StyleId);
		Options->TryGetStringField(TEXT("unit_style"), StyleId);
	}

	const FString FilePath = FindConfigFileById(GetUnitManagementStyleDir(), StyleId);
	return FilePath.IsEmpty() ? nullptr : LoadJsonFileObject(FilePath);
}

static FString PlanPathFromId(const FString& PlanId)
{
	return FPaths::Combine(GetPlanDir(), PlanId + TEXT(".json"));
}

static FString GetObjectPathString(const UObject* Object)
{
	return Object ? Object->GetPathName() : FString();
}

static FString NormalizeObjectPath(const FString& InPath)
{
	FString Path = InPath.TrimStartAndEnd();
	if (Path.IsEmpty() || Path.Contains(TEXT(".")))
	{
		return Path;
	}

	FString PackagePath;
	FString AssetName;
	if (Path.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
	{
		return Path + TEXT(".") + AssetName;
	}
	return Path;
}

static UMassBattleAgentConfigDataAsset* LoadUnit(const FString& UnitPath, FString& OutError)
{
	UObject* Object = FSoftObjectPath(NormalizeObjectPath(UnitPath)).TryLoad();
	UMassBattleAgentConfigDataAsset* Unit = Cast<UMassBattleAgentConfigDataAsset>(Object);
	if (!Unit)
	{
		OutError = FString::Printf(TEXT("Failed to load UMassBattleAgentConfigDataAsset: %s"), *UnitPath);
	}
	return Unit;
}

static TArray<FName> DefaultScanRoots(const TSharedPtr<FJsonObject>& Options)
{
	TArray<FName> Roots;
	const TSharedPtr<FJsonObject> StyleConfig = LoadUnitStyleConfig(Options);
	for (const FString& Root : ReadStringArrayField(StyleConfig, TEXT("default_scan_roots")))
	{
		Roots.Add(FName(*Root));
	}
	if (Roots.IsEmpty())
	{
		Roots = { FName(TEXT("/MassBattle/Demo")), FName(TEXT("/MassBattle/Test")) };
	}
	return Roots;
}

static TArray<FName> ParseRoots(const TSharedPtr<FJsonObject>& Options)
{
	TArray<FName> Roots;
	const TArray<TSharedPtr<FJsonValue>>* RootArray = nullptr;
	if (Options.IsValid() && Options->TryGetArrayField(TEXT("roots"), RootArray))
	{
		for (const TSharedPtr<FJsonValue>& Value : *RootArray)
		{
			FString Root = Value->AsString();
			if (!Root.IsEmpty())
			{
				Roots.Add(FName(*Root));
			}
		}
	}

	if (Roots.IsEmpty())
	{
		Roots = DefaultScanRoots(Options);
		bool bIncludeProject = false;
		if (Options.IsValid() && Options->TryGetBoolField(TEXT("include_project"), bIncludeProject) && bIncludeProject)
		{
			const TSharedPtr<FJsonObject> StyleConfig = LoadUnitStyleConfig(Options);
			TArray<FString> ProjectRoots = ReadStringArrayField(StyleConfig, TEXT("project_scan_roots"));
			if (ProjectRoots.IsEmpty())
			{
				ProjectRoots.Add(TEXT("/Game"));
			}
			for (const FString& ProjectRoot : ProjectRoots)
			{
				Roots.Add(FName(*ProjectRoot));
			}
		}
	}
	return Roots;
}

static IAssetRegistry& GetAssetRegistry()
{
	return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
}

static TArray<FAssetData> ScanUnitAssets(const TSharedPtr<FJsonObject>& Options)
{
	TArray<FName> Roots = ParseRoots(Options);
	TArray<FString> RootStrings;
	for (const FName& Root : Roots)
	{
		RootStrings.Add(Root.ToString());
	}

	IAssetRegistry& Registry = GetAssetRegistry();
	if (!RootStrings.IsEmpty())
	{
		Registry.ScanPathsSynchronous(RootStrings, true);
	}

	FARFilter Filter;
	Filter.ClassPaths.Add(UMassBattleAgentConfigDataAsset::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	Filter.bRecursivePaths = true;
	Filter.PackagePaths = Roots;

	TArray<FAssetData> Assets;
	Registry.GetAssets(Filter, Assets);
	Assets.Sort([](const FAssetData& A, const FAssetData& B)
	{
		return A.GetObjectPathString() < B.GetObjectPathString();
	});
	return Assets;
}

static FProperty* FindPropertyBySegment(UStruct* Struct, const FString& Segment)
{
	if (!Struct)
	{
		return nullptr;
	}

	if (FProperty* Exact = Struct->FindPropertyByName(FName(*Segment)))
	{
		return Exact;
	}

	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		if (It->GetName().Equals(Segment, ESearchCase::IgnoreCase))
		{
			return *It;
		}
	}
	return nullptr;
}

static bool IsNumericSegment(const FString& Segment, int32& OutIndex)
{
	if (Segment.IsNumeric())
	{
		OutIndex = FCString::Atoi(*Segment);
		return true;
	}
	return false;
}

static bool ResolvePropertyPath(void* ContainerPtr, UStruct* ContainerStruct, const FString& Path, FResolvedPropertyPath& Out)
{
	TArray<FString> Segments;
	Path.ParseIntoArray(Segments, TEXT("."), true);
	if (Segments.IsEmpty())
	{
		Out.Error = TEXT("Empty property path");
		return false;
	}

	void* CurrentPtr = ContainerPtr;
	UStruct* CurrentStruct = ContainerStruct;
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FString& Segment = Segments[Index];
		FProperty* Property = FindPropertyBySegment(CurrentStruct, Segment);
		if (!Property)
		{
			Out.Error = FString::Printf(TEXT("Property segment '%s' not found under %s"), *Segment, CurrentStruct ? *CurrentStruct->GetName() : TEXT("<none>"));
			return false;
		}

		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(CurrentPtr);
		const bool bLast = Index == Segments.Num() - 1;
		if (bLast)
		{
			Out.Property = Property;
			Out.ValuePtr = ValuePtr;
			return true;
		}

		if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			CurrentStruct = StructProperty->Struct;
			CurrentPtr = ValuePtr;
			continue;
		}

		if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			if (Index + 1 >= Segments.Num())
			{
				Out.Error = FString::Printf(TEXT("Array property '%s' requires an index"), *Segment);
				return false;
			}

			int32 ArrayIndex = INDEX_NONE;
			if (!IsNumericSegment(Segments[Index + 1], ArrayIndex))
			{
				Out.Error = FString::Printf(TEXT("Array property '%s' requires numeric index, got '%s'"), *Segment, *Segments[Index + 1]);
				return false;
			}

			FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
			if (!Helper.IsValidIndex(ArrayIndex))
			{
				Out.Error = FString::Printf(TEXT("Array index %d is out of range for '%s'"), ArrayIndex, *Segment);
				return false;
			}

			void* ElementPtr = Helper.GetRawPtr(ArrayIndex);
			Index++;
			const bool bArrayElementLast = Index == Segments.Num() - 1;
			if (bArrayElementLast)
			{
				Out.Property = ArrayProperty->Inner;
				Out.ValuePtr = ElementPtr;
				return true;
			}

			if (FStructProperty* InnerStruct = CastField<FStructProperty>(ArrayProperty->Inner))
			{
				CurrentStruct = InnerStruct->Struct;
				CurrentPtr = ElementPtr;
				continue;
			}

			Out.Error = FString::Printf(TEXT("Array property '%s' element is not a struct"), *Segment);
			return false;
		}

		Out.Error = FString::Printf(TEXT("Property '%s' is not a struct/array and cannot contain '%s'"), *Property->GetName(), *Segments[Index + 1]);
		return false;
	}

	Out.Error = FString::Printf(TEXT("Unable to resolve property path: %s"), *Path);
	return false;
}

static FString ExportProperty(FProperty* Property, const void* ValuePtr)
{
	if (!Property || !ValuePtr)
	{
		return FString();
	}

	FString Text;
	Property->ExportTextItem_Direct(Text, ValuePtr, nullptr, nullptr, PPF_None);
	return Text;
}

static bool TryExportPath(UObject* Object, const FString& Path, FString& OutText)
{
	FResolvedPropertyPath Resolved;
	if (!ResolvePropertyPath(Object, Object->GetClass(), Path, Resolved))
	{
		return false;
	}
	OutText = ExportProperty(Resolved.Property, Resolved.ValuePtr);
	return true;
}

static FString JsonValueToImportText(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid())
	{
		return TEXT("");
	}

	switch (Value->Type)
	{
	case EJson::String:
		return Value->AsString();
	case EJson::Number:
		return FString::SanitizeFloat(Value->AsNumber());
	case EJson::Boolean:
		return Value->AsBool() ? TEXT("True") : TEXT("False");
	case EJson::Object:
	{
		FString Output;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
		FJsonSerializer::Serialize(Value->AsObject().ToSharedRef(), Writer);
		return Output;
	}
	case EJson::Array:
		return ToJsonString(Value->AsArray());
	case EJson::Null:
	default:
		return TEXT("");
	}
}

static bool ComputePatchAfterValue(FProperty* Property, const FString& BeforeText, const TSharedPtr<FJsonObject>& Patch, FString& OutAfter, FString& OutError)
{
	FString Operation = TEXT("set");
	Patch->TryGetStringField(TEXT("op"), Operation);
	Patch->TryGetStringField(TEXT("operation"), Operation);

	if (Operation.Equals(TEXT("set"), ESearchCase::IgnoreCase))
	{
		const TSharedPtr<FJsonValue> Value = Patch->TryGetField(TEXT("value"));
		if (!Value.IsValid())
		{
			OutError = TEXT("set patch requires 'value'");
			return false;
		}
		OutAfter = JsonValueToImportText(Value);
		return true;
	}

	if (Operation.Equals(TEXT("append"), ESearchCase::IgnoreCase))
	{
		const TSharedPtr<FJsonValue> Value = Patch->TryGetField(TEXT("value"));
		if (!Value.IsValid())
		{
			OutError = TEXT("append patch requires 'value'");
			return false;
		}
		OutAfter = BeforeText + TEXT(" + ") + JsonValueToImportText(Value);
		return true;
	}

	const TSharedPtr<FJsonValue> Value = Patch->TryGetField(TEXT("value"));
	if (!Value.IsValid() || Value->Type != EJson::Number)
	{
		OutError = FString::Printf(TEXT("%s patch requires numeric 'value'"), *Operation);
		return false;
	}

	if (!Property->IsA<FNumericProperty>())
	{
		OutError = FString::Printf(TEXT("%s patch requires numeric target property"), *Operation);
		return false;
	}

	const double Before = FCString::Atod(*BeforeText);
	const double Arg = Value->AsNumber();
	double After = Before;
	if (Operation.Equals(TEXT("multiply"), ESearchCase::IgnoreCase))
	{
		After = Before * Arg;
	}
	else if (Operation.Equals(TEXT("add"), ESearchCase::IgnoreCase) || Operation.Equals(TEXT("increase"), ESearchCase::IgnoreCase))
	{
		After = Before + Arg;
	}
	else if (Operation.Equals(TEXT("subtract"), ESearchCase::IgnoreCase) || Operation.Equals(TEXT("decrease"), ESearchCase::IgnoreCase))
	{
		After = Before - Arg;
	}
	else
	{
		OutError = FString::Printf(TEXT("Unsupported patch operation: %s"), *Operation);
		return false;
	}

	OutAfter = FString::SanitizeFloat(After);
	return true;
}

static bool ApplyImportText(FProperty* Property, void* ValuePtr, const FString& ImportText, UObject* Owner, FString& OutError)
{
	if (!Property || !ValuePtr)
	{
		OutError = TEXT("Invalid property target");
		return false;
	}

	const TCHAR* Result = Property->ImportText_Direct(*ImportText, ValuePtr, Owner, PPF_None);
	if (!Result)
	{
		OutError = FString::Printf(TEXT("Failed to import '%s' into %s"), *ImportText, *Property->GetName());
		return false;
	}
	return true;
}

static bool ApplyJsonValueToProperty(FProperty* Property, void* ValuePtr, const TSharedPtr<FJsonValue>& Value, UObject* Owner, FString& OutError)
{
	if (!Property || !ValuePtr || !Value.IsValid())
	{
		OutError = TEXT("Invalid JSON property assignment target");
		return false;
	}

	if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
	{
		if (Value->Type != EJson::Object)
		{
			return ApplyImportText(Property, ValuePtr, JsonValueToImportText(Value), Owner, OutError);
		}

		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
		{
			FProperty* Child = FindPropertyBySegment(StructProperty->Struct, Pair.Key);
			if (!Child)
			{
				OutError = FString::Printf(TEXT("Struct property '%s' has no field '%s'"), *StructProperty->GetName(), *Pair.Key);
				return false;
			}

			void* ChildPtr = Child->ContainerPtrToValuePtr<void>(ValuePtr);
			if (!ApplyJsonValueToProperty(Child, ChildPtr, Pair.Value, Owner, OutError))
			{
				return false;
			}
		}
		return true;
	}

	if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
	{
		if (Value->Type != EJson::Array)
		{
			OutError = FString::Printf(TEXT("Array property '%s' requires a JSON array"), *ArrayProperty->GetName());
			return false;
		}

		FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
		Helper.EmptyValues();
		const TArray<TSharedPtr<FJsonValue>>& Array = Value->AsArray();
		for (const TSharedPtr<FJsonValue>& ElementValue : Array)
		{
			const int32 NewIndex = Helper.AddValue();
			if (!ApplyJsonValueToProperty(ArrayProperty->Inner, Helper.GetRawPtr(NewIndex), ElementValue, Owner, OutError))
			{
				return false;
			}
		}
		return true;
	}

	return ApplyImportText(Property, ValuePtr, JsonValueToImportText(Value), Owner, OutError);
}

static FPatchPreview PreviewPatch(UObject* Object, const TSharedPtr<FJsonObject>& Patch)
{
	FPatchPreview Preview;
	if (!Object || !Patch.IsValid())
	{
		Preview.Error = TEXT("Invalid object or patch");
		return Preview;
	}

	if (!Patch->TryGetStringField(TEXT("path"), Preview.Path) && !Patch->TryGetStringField(TEXT("field"), Preview.Path))
	{
		Preview.Error = TEXT("Patch requires 'path'");
		return Preview;
	}
	Patch->TryGetStringField(TEXT("op"), Preview.Operation);
	Patch->TryGetStringField(TEXT("operation"), Preview.Operation);
	if (Preview.Operation.IsEmpty())
	{
		Preview.Operation = TEXT("set");
	}

	FResolvedPropertyPath Resolved;
	if (!ResolvePropertyPath(Object, Object->GetClass(), Preview.Path, Resolved))
	{
		Preview.Error = Resolved.Error;
		return Preview;
	}

	Preview.Before = ExportProperty(Resolved.Property, Resolved.ValuePtr);
	if (Preview.Operation.Equals(TEXT("append"), ESearchCase::IgnoreCase) && !Resolved.Property->IsA<FArrayProperty>())
	{
		Preview.Error = FString::Printf(TEXT("append patch requires array target property, got %s"), *Resolved.Property->GetName());
		return Preview;
	}

	FString Error;
	if (!ComputePatchAfterValue(Resolved.Property, Preview.Before, Patch, Preview.After, Error))
	{
		Preview.Error = Error;
	}
	return Preview;
}

static bool ApplyPatch(UObject* Object, const TSharedPtr<FJsonObject>& Patch, bool bForce, FPatchPreview& OutPreview)
{
	OutPreview = PreviewPatch(Object, Patch);
	if (!OutPreview.Error.IsEmpty())
	{
		return false;
	}

	FString Expected;
	if (!bForce && Patch->TryGetStringField(TEXT("expected_before"), Expected) && Expected != OutPreview.Before)
	{
		OutPreview.Error = FString::Printf(TEXT("Conflict at %s. Expected '%s', current '%s'"), *OutPreview.Path, *Expected, *OutPreview.Before);
		return false;
	}

	FResolvedPropertyPath Resolved;
	if (!ResolvePropertyPath(Object, Object->GetClass(), OutPreview.Path, Resolved))
	{
		OutPreview.Error = Resolved.Error;
		return false;
	}

	FString Error;
	if (OutPreview.Operation.Equals(TEXT("append"), ESearchCase::IgnoreCase))
	{
		FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Resolved.Property);
		if (!ArrayProperty)
		{
			OutPreview.Error = FString::Printf(TEXT("append patch requires array target property, got %s"), *Resolved.Property->GetName());
			return false;
		}

		const TSharedPtr<FJsonValue> Value = Patch->TryGetField(TEXT("value"));
		if (!Value.IsValid())
		{
			OutPreview.Error = TEXT("append patch requires 'value'");
			return false;
		}

		FScriptArrayHelper Helper(ArrayProperty, Resolved.ValuePtr);
		const int32 NewIndex = Helper.AddValue();
		if (!ApplyJsonValueToProperty(ArrayProperty->Inner, Helper.GetRawPtr(NewIndex), Value, Object, Error))
		{
			Helper.RemoveValues(NewIndex, 1);
			OutPreview.Error = Error;
			return false;
		}

		OutPreview.After = ExportProperty(ArrayProperty, Resolved.ValuePtr);
		return true;
	}

	if (!ApplyImportText(Resolved.Property, Resolved.ValuePtr, OutPreview.After, Object, Error))
	{
		OutPreview.Error = Error;
		return false;
	}
	return true;
}

static TSharedPtr<FJsonObject> PatchPreviewToJson(const FPatchPreview& Preview)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("path"), Preview.Path);
	Object->SetStringField(TEXT("operation"), Preview.Operation);
	Object->SetStringField(TEXT("before"), Preview.Before);
	Object->SetStringField(TEXT("after"), Preview.After);
	if (!Preview.Error.IsEmpty())
	{
		Object->SetStringField(TEXT("error"), Preview.Error);
	}
	return Object;
}

static TSharedPtr<FJsonValue> PropertyToJsonValue(FProperty* Property, const void* ValuePtr, int32 Depth)
{
	if (!Property || !ValuePtr)
	{
		return MakeShared<FJsonValueNull>();
	}

	if (FNumericProperty* Numeric = CastField<FNumericProperty>(Property))
	{
		if (Numeric->IsInteger())
		{
			return MakeShared<FJsonValueNumber>(static_cast<double>(Numeric->GetSignedIntPropertyValue(ValuePtr)));
		}
		return MakeShared<FJsonValueNumber>(Numeric->GetFloatingPointPropertyValue(ValuePtr));
	}
	if (FBoolProperty* Bool = CastField<FBoolProperty>(Property))
	{
		return MakeShared<FJsonValueBoolean>(Bool->GetPropertyValue(ValuePtr));
	}
	if (FStrProperty* StringProperty = CastField<FStrProperty>(Property))
	{
		return MakeShared<FJsonValueString>(StringProperty->GetPropertyValue(ValuePtr));
	}
	if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
	{
		return MakeShared<FJsonValueString>(NameProperty->GetPropertyValue(ValuePtr).ToString());
	}
	if (FTextProperty* TextProperty = CastField<FTextProperty>(Property))
	{
		return MakeShared<FJsonValueString>(TextProperty->GetPropertyValue(ValuePtr).ToString());
	}
	if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
	{
		const int64 Raw = EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
		return MakeShared<FJsonValueString>(EnumProperty->GetEnum()->GetNameStringByValue(Raw));
	}
	if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
	{
		if (ByteProperty->Enum)
		{
			return MakeShared<FJsonValueString>(ByteProperty->Enum->GetNameStringByValue(ByteProperty->GetPropertyValue(ValuePtr)));
		}
		return MakeShared<FJsonValueNumber>(ByteProperty->GetPropertyValue(ValuePtr));
	}
	if (FSoftObjectProperty* SoftObject = CastField<FSoftObjectProperty>(Property))
	{
		return MakeShared<FJsonValueString>(SoftObject->GetPropertyValue(ValuePtr).ToString());
	}
	if (FSoftClassProperty* SoftClass = CastField<FSoftClassProperty>(Property))
	{
		return MakeShared<FJsonValueString>(SoftClass->GetPropertyValue(ValuePtr).ToString());
	}
	if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
	{
		return MakeShared<FJsonValueString>(GetObjectPathString(ObjectProperty->GetObjectPropertyValue(ValuePtr)));
	}
	if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
	{
		if (Depth <= 0)
		{
			return MakeShared<FJsonValueString>(ExportProperty(Property, ValuePtr));
		}

		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		for (TFieldIterator<FProperty> It(StructProperty->Struct); It; ++It)
		{
			FProperty* Child = *It;
			if (ShouldSkipNestedField(Child->GetName()))
			{
				continue;
			}
			Object->SetField(Child->GetName(), PropertyToJsonValue(Child, Child->ContainerPtrToValuePtr<void>(ValuePtr), Depth - 1));
		}
		return MakeShared<FJsonValueObject>(Object);
	}
	if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
	{
		FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
		TArray<TSharedPtr<FJsonValue>> Array;
		const int32 Limit = FMath::Min(Helper.Num(), 64);
		for (int32 Index = 0; Index < Limit; ++Index)
		{
			Array.Add(PropertyToJsonValue(ArrayProperty->Inner, Helper.GetRawPtr(Index), Depth - 1));
		}
		if (Helper.Num() > Limit)
		{
			Array.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("<truncated %d more>"), Helper.Num() - Limit)));
		}
		return MakeShared<FJsonValueArray>(Array);
	}

	return MakeShared<FJsonValueString>(ExportProperty(Property, ValuePtr));
}

static TArray<FString> GetFallbackSimpleTopLevelFields()
{
	return {
		TEXT("SubType"),
		TEXT("StyleType"),
		TEXT("Scaling"),
		TEXT("Collider"),
		TEXT("PhysicsShared"),
		TEXT("Health"),
		TEXT("Move"),
		TEXT("Attack"),
		TEXT("Damage"),
		TEXT("Defence"),
		TEXT("Trace"),
		TEXT("Visualize"),
		TEXT("LODShared"),
		TEXT("AnimShared")
	};
}

static TSet<FString> GetSimpleTopLevelFields(const TSharedPtr<FJsonObject>& Options)
{
	TArray<FString> Fields = ReadStringArrayField(LoadUnitStyleConfig(Options), TEXT("simple_top_level_fields"));
	if (Fields.IsEmpty())
	{
		Fields = GetFallbackSimpleTopLevelFields();
	}

	TSet<FString> Result;
	for (const FString& Field : Fields)
	{
		if (!Field.IsEmpty())
		{
			Result.Add(Field);
		}
	}
	return Result;
}

static bool JsonValueIsEmptyObjectOrArray(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid())
	{
		return true;
	}
	if (Value->Type == EJson::Object)
	{
		TSharedPtr<FJsonObject> Object = Value->AsObject();
		return !Object.IsValid() || Object->Values.IsEmpty();
	}
	if (Value->Type == EJson::Array)
	{
		return Value->AsArray().IsEmpty();
	}
	return false;
}

static TSharedPtr<FJsonObject> InstancedStructToJson(const FInstancedStruct& InstancedStruct, int32 Depth)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	const UScriptStruct* ScriptStruct = InstancedStruct.GetScriptStruct();
	Object->SetStringField(TEXT("Struct"), ScriptStruct ? ScriptStruct->GetPathName() : FString());
	if (!InstancedStruct.IsValid() || !ScriptStruct || !InstancedStruct.GetMemory())
	{
		return Object;
	}

	if (Depth <= 0)
	{
		FString ValueText;
		FInstancedStruct DefaultValue;
		InstancedStruct.ExportTextItem(ValueText, DefaultValue, nullptr, PPF_None, nullptr);
		Object->SetStringField(TEXT("Value"), ValueText);
		return Object;
	}

	TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> It(ScriptStruct); It; ++It)
	{
		FProperty* Child = *It;
		if (ShouldSkipNestedField(Child->GetName()))
		{
			continue;
		}
		Value->SetField(Child->GetName(), PropertyToJsonValue(Child, Child->ContainerPtrToValuePtr<void>(InstancedStruct.GetMemory()), Depth - 1));
	}
	Object->SetObjectField(TEXT("Value"), Value);
	return Object;
}

static TArray<TSharedPtr<FJsonValue>> InstancedStructArrayToJson(const TArray<FInstancedStruct>& Values, int32 Depth)
{
	TArray<TSharedPtr<FJsonValue>> Array;
	for (const FInstancedStruct& Value : Values)
	{
		Array.Add(MakeShared<FJsonValueObject>(InstancedStructToJson(Value, Depth)));
	}
	return Array;
}

static TSharedPtr<FJsonObject> ExtraDataToJson(const FMassBattleTemplate& ExtraData, int32 Depth, bool bOmitDefaults)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	if (!bOmitDefaults || !ExtraData.Tags.IsEmpty())
	{
		Object->SetArrayField(TEXT("Tags"), InstancedStructArrayToJson(ExtraData.Tags, Depth));
	}
	if (!bOmitDefaults || !ExtraData.Fragments.IsEmpty())
	{
		Object->SetArrayField(TEXT("Fragments"), InstancedStructArrayToJson(ExtraData.Fragments, Depth));
	}
	if (!bOmitDefaults || !ExtraData.MutableSharedFragments.IsEmpty())
	{
		Object->SetArrayField(TEXT("MutableSharedFragments"), InstancedStructArrayToJson(ExtraData.MutableSharedFragments, Depth));
	}
	if (!bOmitDefaults || !ExtraData.ConstSharedFragments.IsEmpty())
	{
		Object->SetArrayField(TEXT("ConstSharedFragments"), InstancedStructArrayToJson(ExtraData.ConstSharedFragments, Depth));
	}
	if (!bOmitDefaults || !ExtraData.BattleFlags.IsEmpty())
	{
		TArray<TSharedPtr<FJsonValue>> Flags;
		const UEnum* Enum = StaticEnum<EBattleFlags>();
		for (const EBattleFlags Flag : ExtraData.BattleFlags)
		{
			Flags.Add(MakeShared<FJsonValueString>(Enum ? Enum->GetNameStringByValue(static_cast<int64>(Flag)) : FString::FromInt(static_cast<int32>(Flag))));
		}
		Object->SetArrayField(TEXT("BattleFlags"), Flags);
	}
	return Object;
}

static TSharedPtr<FJsonValue> PropertyToEffectiveJsonValue(FProperty* Property, const void* ValuePtr, const void* DefaultValuePtr, int32 Depth, bool bOmitDefaults)
{
	if (!Property || !ValuePtr)
	{
		return nullptr;
	}

	if (bOmitDefaults && DefaultValuePtr && Property->Identical(ValuePtr, DefaultValuePtr, PPF_None))
	{
		return nullptr;
	}

	if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
	{
		if (Depth <= 0)
		{
			return PropertyToJsonValue(Property, ValuePtr, Depth);
		}

		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		for (TFieldIterator<FProperty> It(StructProperty->Struct); It; ++It)
		{
			FProperty* Child = *It;
			if (ShouldSkipNestedField(Child->GetName()))
			{
				continue;
			}
			const void* ChildValuePtr = Child->ContainerPtrToValuePtr<void>(ValuePtr);
			const void* ChildDefaultPtr = DefaultValuePtr ? Child->ContainerPtrToValuePtr<void>(DefaultValuePtr) : nullptr;
			TSharedPtr<FJsonValue> ChildValue = PropertyToEffectiveJsonValue(Child, ChildValuePtr, ChildDefaultPtr, Depth - 1, bOmitDefaults);
			if (!JsonValueIsEmptyObjectOrArray(ChildValue))
			{
				Object->SetField(Child->GetName(), ChildValue);
			}
		}
		if (Object->Values.IsEmpty())
		{
			return nullptr;
		}
		return MakeShared<FJsonValueObject>(Object);
	}

	if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
	{
		FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
		if (bOmitDefaults && Helper.Num() == 0)
		{
			return nullptr;
		}
		return PropertyToJsonValue(Property, ValuePtr, Depth);
	}

	return PropertyToJsonValue(Property, ValuePtr, Depth);
}

static TArray<FString> ParseStringArrayOption(const TSharedPtr<FJsonObject>& Options, const FString& FieldName)
{
	TArray<FString> Result;
	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (Options.IsValid() && Options->TryGetArrayField(FieldName, Array))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			const FString Text = Value->AsString();
			if (!Text.IsEmpty())
			{
				Result.Add(Text);
			}
		}
	}
	return Result;
}

static bool IsDetailedMode(const TSharedPtr<FJsonObject>& Options)
{
	FString Detail = TEXT("simple");
	if (Options.IsValid())
	{
		Options->TryGetStringField(TEXT("detail"), Detail);
		Options->TryGetStringField(TEXT("mode"), Detail);
		bool bCompact = true;
		if (Options->TryGetBoolField(TEXT("compact"), bCompact) && !bCompact)
		{
			Detail = TEXT("detailed");
		}
	}
	return Detail.Equals(TEXT("detailed"), ESearchCase::IgnoreCase)
		|| Detail.Equals(TEXT("detail"), ESearchCase::IgnoreCase)
		|| Detail.Equals(TEXT("full"), ESearchCase::IgnoreCase);
}

static TArray<FString> GetRequestedObjects(const TSharedPtr<FJsonObject>& Options)
{
	TArray<FString> Objects = ParseStringArrayOption(Options, TEXT("objects"));
	FString SingleObject;
	if (Options.IsValid() && Options->TryGetStringField(TEXT("object"), SingleObject) && !SingleObject.IsEmpty())
	{
		Objects.Add(SingleObject);
	}
	return Objects;
}

static FString GetDetailLabel(const TSharedPtr<FJsonObject>& Options)
{
	if (!GetRequestedObjects(Options).IsEmpty())
	{
		return TEXT("objects");
	}
	return IsDetailedMode(Options) ? TEXT("detailed") : TEXT("simple");
}

static bool ShouldIncludeObjectInUnitJson(const FString& FieldName, const TSharedPtr<FJsonObject>& Options)
{
	const TArray<FString> RequestedObjects = GetRequestedObjects(Options);
	if (!RequestedObjects.IsEmpty())
	{
		for (const FString& ObjectName : RequestedObjects)
		{
			if (FieldName.Equals(ObjectName, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	if (IsDetailedMode(Options))
	{
		return true;
	}

	return GetSimpleTopLevelFields(Options).Contains(FieldName);
}

static TSharedPtr<FJsonObject> BuildSourceAlignedUnitJson(UMassBattleAgentConfigDataAsset* Unit, const TSharedPtr<FJsonObject>& Options)
{
	TSharedPtr<FJsonObject> Fields = MakeShared<FJsonObject>();
	if (!Unit)
	{
		return Fields;
	}

	const UMassBattleAgentConfigDataAsset* DefaultUnit = GetDefault<UMassBattleAgentConfigDataAsset>();
	bool bIncludeDefaults = false;
	if (Options.IsValid())
	{
		Options->TryGetBoolField(TEXT("include_defaults"), bIncludeDefaults);
	}
	const bool bOmitDefaults = !bIncludeDefaults;
	const bool bDetailed = IsDetailedMode(Options);
	const bool bSpecificObjects = !GetRequestedObjects(Options).IsEmpty();
	const int32 Depth = bDetailed || bSpecificObjects ? MaxPropertyDepth : 3;

	for (TFieldIterator<FProperty> It(Unit->GetClass()); It; ++It)
	{
		FProperty* Property = *It;
		const FString FieldName = Property->GetName();
		if (ShouldSkipTopLevelField(FieldName, Options) || !ShouldIncludeObjectInUnitJson(FieldName, Options))
		{
			continue;
		}

		if (FieldName == TEXT("ExtraData"))
		{
			TSharedPtr<FJsonObject> ExtraDataJson = ExtraDataToJson(Unit->ExtraData, Depth, bOmitDefaults);
			if (!ExtraDataJson->Values.IsEmpty())
			{
				Fields->SetObjectField(TEXT("ExtraData"), ExtraDataJson);
			}
			continue;
		}

		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Unit);
		const void* DefaultValuePtr = DefaultUnit ? Property->ContainerPtrToValuePtr<void>(DefaultUnit) : nullptr;
		TSharedPtr<FJsonValue> Value = PropertyToEffectiveJsonValue(Property, ValuePtr, DefaultValuePtr, Depth, bOmitDefaults);
		if (!JsonValueIsEmptyObjectOrArray(Value))
		{
			Fields->SetField(FieldName, Value);
		}
	}
	return Fields;
}

static TSharedPtr<FJsonObject> BuildIgnorePolicyJson()
{
	TSharedPtr<FJsonObject> Policy = MakeShared<FJsonObject>();
	Policy->SetBoolField(TEXT("omit_runtime"), true);
	Policy->SetBoolField(TEXT("omit_system"), true);
	Policy->SetBoolField(TEXT("omit_deprecated"), true);
	Policy->SetBoolField(TEXT("omit_visibility"), true);
	Policy->SetStringField(TEXT("reason"), TEXT("Default output hides runtime cache, system migration state, deprecated migration fields, and editor visibility toggles to reduce context."));

	TArray<TSharedPtr<FJsonValue>> Runtime;
	const TArray<FString> RuntimeExamples = {
		TEXT("Moving"), TEXT("Attacking"), TEXT("Animating"), TEXT("Locating"),
		TEXT("Rotating"), TEXT("BeingHit"), TEXT("Dying"), TEXT("EventRT")
	};
	for (const FString& Name : RuntimeExamples)
	{
		Runtime.Add(MakeShared<FJsonValueString>(Name));
	}
	Policy->SetArrayField(TEXT("runtime_examples"), Runtime);
	return Policy;
}

static FString InferPathCategory(const FString& ObjectPath)
{
	if (ObjectPath.Contains(TEXT("/Demo/Agent/"))) return TEXT("Demo.Agent");
	if (ObjectPath.Contains(TEXT("/Demo/Building/"))) return TEXT("Demo.Building");
	if (ObjectPath.Contains(TEXT("/Demo/Player/"))) return TEXT("Demo.Player");
	if (ObjectPath.Contains(TEXT("/Test/CompoundUnitAsset/Weapon/"))) return TEXT("Test.CompoundWeapon");
	if (ObjectPath.Contains(TEXT("/Test/CompoundUnitAsset/"))) return TEXT("Test.CompoundUnit");
	if (ObjectPath.Contains(TEXT("/Test/"))) return TEXT("Test");
	if (ObjectPath.Contains(TEXT("/Army/"))) return TEXT("Project.Army");
	if (ObjectPath.Contains(TEXT("/Aircraft/"))) return TEXT("Project.Aircraft");
	if (ObjectPath.Contains(TEXT("/Building/"))) return TEXT("Project.Building");
	return TEXT("Uncategorized");
}

static FString InferStyleFamily(const FString& ObjectPath)
{
	const FString Lower = ObjectPath.ToLower();
	if (Lower.Contains(TEXT("tank"))) return TEXT("armored");
	if (Lower.Contains(TEXT("soldier")) || Lower.Contains(TEXT("human"))) return TEXT("infantry");
	if (Lower.Contains(TEXT("zombie"))) return TEXT("creature");
	if (Lower.Contains(TEXT("helicopter")) || Lower.Contains(TEXT("fighterjet")) || Lower.Contains(TEXT("aircraft"))) return TEXT("air");
	if (Lower.Contains(TEXT("camp")) || Lower.Contains(TEXT("spawner")) || Lower.Contains(TEXT("building"))) return TEXT("building");
	if (Lower.Contains(TEXT("cannon")) || Lower.Contains(TEXT("machinegun")) || Lower.Contains(TEXT("rocketlauncher"))) return TEXT("weapon");
	return TEXT("default");
}

static void AddExportedPath(TSharedPtr<FJsonObject> Object, UObject* Source, const FString& JsonName, const FString& PropertyPath)
{
	FString Text;
	if (TryExportPath(Source, PropertyPath, Text))
	{
		Object->SetStringField(JsonName, Text);
	}
}

static TSharedPtr<FJsonObject> BuildUnitSummary(const FAssetData& AssetData, bool bLoadForFields, const TSharedPtr<FJsonObject>& Options)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	const FString ObjectPath = AssetData.GetObjectPathString();
	Object->SetStringField(TEXT("AssetName"), AssetData.AssetName.ToString());
	Object->SetStringField(TEXT("ObjectPath"), ObjectPath);
	Object->SetStringField(TEXT("PackagePath"), AssetData.PackagePath.ToString());
	Object->SetStringField(TEXT("Class"), AssetData.AssetClassPath.ToString());

	if (bLoadForFields)
	{
		if (UMassBattleAgentConfigDataAsset* Unit = Cast<UMassBattleAgentConfigDataAsset>(AssetData.GetAsset()))
		{
			Object->SetObjectField(TEXT("Data"), BuildSourceAlignedUnitJson(Unit, Options));
		}
	}
	return Object;
}

static void AddRootsToJson(TSharedPtr<FJsonObject> Root, const TSharedPtr<FJsonObject>& Options)
{
	TArray<TSharedPtr<FJsonValue>> RootValues;
	for (const FName& RootPath : ParseRoots(Options))
	{
		RootValues.Add(MakeShared<FJsonValueString>(RootPath.ToString()));
	}
	Root->SetArrayField(TEXT("roots"), RootValues);
}

static TArray<TSharedPtr<FJsonObject>> GetPatchObjects(const TSharedPtr<FJsonObject>& PatchRoot)
{
	TArray<TSharedPtr<FJsonObject>> Patches;
	for (const TSharedPtr<FJsonValue>& Value : ParseArrayField(PatchRoot, TEXT("patches")))
	{
		TSharedPtr<FJsonObject> Patch = Value->AsObject();
		if (Patch.IsValid())
		{
			Patches.Add(Patch);
		}
	}
	return Patches;
}

static bool SavePlan(const TSharedPtr<FJsonObject>& Plan, FString& OutPath, FString& OutError)
{
	IFileManager::Get().MakeDirectory(*GetPlanDir(), true);
	FString PlanId;
	Plan->TryGetStringField(TEXT("plan_id"), PlanId);
	if (PlanId.IsEmpty())
	{
		OutError = TEXT("Plan is missing plan_id");
		return false;
	}
	OutPath = PlanPathFromId(PlanId);
	if (!FFileHelper::SaveStringToFile(ToJsonString(Plan), *OutPath))
	{
		OutError = FString::Printf(TEXT("Failed to save plan: %s"), *OutPath);
		return false;
	}
	return true;
}

static TSharedPtr<FJsonObject> LoadPlan(const FString& PlanId, FString& OutError)
{
	FString PlanJson;
	const FString Path = PlanPathFromId(PlanId);
	if (!FFileHelper::LoadFileToString(PlanJson, *Path))
	{
		OutError = FString::Printf(TEXT("Failed to load plan: %s"), *Path);
		return nullptr;
	}
	TSharedPtr<FJsonObject> Plan = ParseObject(PlanJson);
	if (!Plan.IsValid())
	{
		OutError = FString::Printf(TEXT("Failed to parse plan: %s"), *Path);
	}
	return Plan;
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

static void AppendAuditRecord(const TSharedPtr<FJsonObject>& Record)
{
	IFileManager::Get().MakeDirectory(*GetAuditDir(), true);
	const FString AuditPath = FPaths::Combine(GetAuditDir(), TEXT("unit_changes.jsonl"));
	FString Line = ToJsonString(Record) + LINE_TERMINATOR;
	FFileHelper::SaveStringToFile(Line, *AuditPath, FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), FILEWRITE_Append);
}

static UObject* DuplicateAsset(const FString& SourcePath, const FString& NewAssetName, const FString& PackagePath, FString& OutError)
{
	FString LoadError;
	UMassBattleAgentConfigDataAsset* Source = LoadUnit(SourcePath, LoadError);
	if (!Source)
	{
		OutError = LoadError;
		return nullptr;
	}
	if (!PackagePath.StartsWith(TEXT("/")))
	{
		OutError = TEXT("PackagePath must be a long package path such as /Game/MassBattle/Units");
		return nullptr;
	}
	if (NewAssetName.IsEmpty())
	{
		OutError = TEXT("NewAssetName is required");
		return nullptr;
	}

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UObject* NewAsset = AssetTools.DuplicateAsset(NewAssetName, PackagePath, Source);
	if (!NewAsset)
	{
		OutError = FString::Printf(TEXT("Failed to duplicate %s to %s/%s"), *SourcePath, *PackagePath, *NewAssetName);
	}
	return NewAsset;
}

static FString ResolveDefaultUnitTemplatePath(FString& OutError)
{
	const FString StylePath = FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("MassBattleEditorMCP"),
		TEXT("Resources"),
		TEXT("UnitManagementStyles"),
		TEXT("default.massbattle_unit_style.json"));

	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *StylePath))
	{
		OutError = FString::Printf(TEXT("Failed to read default unit style profile: %s"), *StylePath);
		return FString();
	}

	TSharedPtr<FJsonObject> Style = ParseObject(JsonText);
	if (!Style.IsValid())
	{
		OutError = FString::Printf(TEXT("Default unit style profile is not valid JSON: %s"), *StylePath);
		return FString();
	}

	const TSharedPtr<FJsonObject>* AuthoringDefaults = nullptr;
	if (!Style->TryGetObjectField(TEXT("authoring_defaults"), AuthoringDefaults) || !AuthoringDefaults || !AuthoringDefaults->IsValid())
	{
		OutError = TEXT("Default unit style profile does not define authoring_defaults.default_unit_template");
		return FString();
	}

	FString TemplatePath;
	(*AuthoringDefaults)->TryGetStringField(TEXT("default_unit_template"), TemplatePath);
	if (TemplatePath.IsEmpty())
	{
		OutError = TEXT("Default unit template is not configured. Set authoring_defaults.default_unit_template or pass template_unit.");
	}
	return TemplatePath;
}

static bool ResolveCreateTarget(const TSharedPtr<FJsonObject>& Spec, FString& OutAssetName, FString& OutPackagePath, FString& OutUnitPath, FString& OutError)
{
	Spec->TryGetStringField(TEXT("asset_name"), OutAssetName);
	Spec->TryGetStringField(TEXT("package_path"), OutPackagePath);
	Spec->TryGetStringField(TEXT("unit_path"), OutUnitPath);
	Spec->TryGetStringField(TEXT("target_unit"), OutUnitPath);
	Spec->TryGetStringField(TEXT("new_unit_path"), OutUnitPath);

	if ((!OutAssetName.IsEmpty() && !OutPackagePath.IsEmpty()) || OutUnitPath.IsEmpty())
	{
		if (OutAssetName.IsEmpty() || OutPackagePath.IsEmpty())
		{
			OutError = TEXT("Create spec requires either unit_path/new_unit_path or both asset_name and package_path");
			return false;
		}
		OutUnitPath = OutPackagePath / OutAssetName + TEXT(".") + OutAssetName;
		return true;
	}

	FString ObjectPath = OutUnitPath;
	FString PackageName;
	FString ObjectName;
	if (!ObjectPath.Split(TEXT("."), &PackageName, &ObjectName, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
	{
		ObjectName = FPackageName::GetLongPackageAssetName(ObjectPath);
		PackageName = ObjectPath;
	}

	if (ObjectName.IsEmpty())
	{
		OutError = FString::Printf(TEXT("Could not resolve asset name from unit_path: %s"), *OutUnitPath);
		return false;
	}

	FString ParentPath;
	if (!PackageName.Split(TEXT("/"), &ParentPath, &OutAssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
	{
		OutError = FString::Printf(TEXT("Could not resolve package path from unit_path: %s"), *OutUnitPath);
		return false;
	}

	OutPackagePath = ParentPath;
	OutUnitPath = OutPackagePath / OutAssetName + TEXT(".") + OutAssetName;
	return true;
}

static TSharedPtr<FJsonObject> BuildPlanBase(const FString& Type)
{
	TSharedPtr<FJsonObject> Plan = MakeShared<FJsonObject>();
	Plan->SetStringField(TEXT("plan_id"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens));
	Plan->SetStringField(TEXT("type"), Type);
	Plan->SetStringField(TEXT("created_utc"), FDateTime::UtcNow().ToIso8601());
	Plan->SetStringField(TEXT("risk_model"), TEXT("Diff-only until MCP_UnitApplyPlan is called. Runtime entities are not migrated by this MVP."));
	return Plan;
}

static TArray<TSharedPtr<FJsonValue>> BuildPatchDiffArray(UObject* Target, const TArray<TSharedPtr<FJsonObject>>& Patches, bool& bHasError)
{
	TArray<TSharedPtr<FJsonValue>> DiffArray;
	bHasError = false;
	for (const TSharedPtr<FJsonObject>& Patch : Patches)
	{
		const FPatchPreview Preview = PreviewPatch(Target, Patch);
		if (!Preview.Error.IsEmpty())
		{
			bHasError = true;
		}
		DiffArray.Add(MakeShared<FJsonValueObject>(PatchPreviewToJson(Preview)));
	}
	return DiffArray;
}

static TArray<TSharedPtr<FJsonValue>> CopyPatchesToJsonValues(const TArray<TSharedPtr<FJsonObject>>& Patches)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const TSharedPtr<FJsonObject>& Patch : Patches)
	{
		Values.Add(MakeShared<FJsonValueObject>(Patch));
	}
	return Values;
}

static void AddMergeError(TArray<TSharedPtr<FJsonValue>>& Errors, const FString& Path, const FString& Message)
{
	TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
	Error->SetStringField(TEXT("path"), Path);
	Error->SetStringField(TEXT("message"), Message);
	Errors.Add(MakeShared<FJsonValueObject>(Error));
}

static bool IsMergeMetadataField(const FString& FieldName)
{
	static const TSet<FString> MetadataFields = {
		TEXT("success"), TEXT("AssetName"), TEXT("ObjectPath"), TEXT("PackagePath"),
		TEXT("Class"), TEXT("detail"), TEXT("default_detail"), TEXT("default_ignore_policy"),
		TEXT("roots"), TEXT("count"), TEXT("total_scanned"), TEXT("units"),
		TEXT("options"), TEXT("merge_options")
	};
	return MetadataFields.Contains(FieldName);
}

static TSharedPtr<FJsonObject> ExtractMergeOptions(const TSharedPtr<FJsonObject>& Root)
{
	if (!Root.IsValid())
	{
		return MakeShared<FJsonObject>();
	}

	const TSharedPtr<FJsonObject>* Options = nullptr;
	if (Root->TryGetObjectField(TEXT("options"), Options) && Options && Options->IsValid())
	{
		return *Options;
	}
	if (Root->TryGetObjectField(TEXT("merge_options"), Options) && Options && Options->IsValid())
	{
		return *Options;
	}
	return MakeShared<FJsonObject>();
}

static TSharedPtr<FJsonObject> ExtractMergeDataObject(const TSharedPtr<FJsonObject>& Root, FString& OutError)
{
	if (!Root.IsValid())
	{
		OutError = TEXT("Merge JSON is not a valid object");
		return nullptr;
	}

	const TSharedPtr<FJsonObject>* Data = nullptr;
	if (Root->TryGetObjectField(TEXT("Data"), Data))
	{
		if (Data && Data->IsValid())
		{
			return *Data;
		}
		OutError = TEXT("'Data' must be an object when provided");
		return nullptr;
	}

	TSharedPtr<FJsonObject> DataObject = MakeShared<FJsonObject>();
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Root->Values)
	{
		if (!IsMergeMetadataField(Pair.Key))
		{
			DataObject->SetField(Pair.Key, Pair.Value);
		}
	}
	return DataObject;
}

static bool IsWritableTopLevelField(const FString& FieldName, const TSharedPtr<FJsonObject>& Options, FString& OutError)
{
	const EFieldRole Role = ClassifyTopLevelField(FieldName);
	if (Role == EFieldRole::RuntimeState || Role == EFieldRole::SystemOwned || Role == EFieldRole::Deprecated || Role == EFieldRole::VisibilityControl)
	{
		bool bAllowIgnoredFields = false;
		if (Options.IsValid())
		{
			Options->TryGetBoolField(TEXT("allow_ignored_fields"), bAllowIgnoredFields);
		}
		if (!bAllowIgnoredFields)
		{
			OutError = FString::Printf(TEXT("Field '%s' is %s and is blocked by default merge policy"), *FieldName, *RoleToString(Role));
			return false;
		}
	}
	return true;
}

static TSharedPtr<FJsonValue> CloneJsonValue(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid())
	{
		return MakeShared<FJsonValueNull>();
	}
	return Value;
}

static void AddSetPatchForMerge(UObject* Target, const FString& Path, const TSharedPtr<FJsonValue>& Value, const TSharedPtr<FJsonObject>& Options, TArray<TSharedPtr<FJsonObject>>& Patches, TArray<TSharedPtr<FJsonValue>>& Errors)
{
	FResolvedPropertyPath Resolved;
	if (!ResolvePropertyPath(Target, Target ? Target->GetClass() : nullptr, Path, Resolved))
	{
		AddMergeError(Errors, Path, Resolved.Error);
		return;
	}

	bool bIncludeExpectedBefore = true;
	if (Options.IsValid())
	{
		Options->TryGetBoolField(TEXT("expected_before"), bIncludeExpectedBefore);
	}

	TSharedPtr<FJsonObject> Patch = MakeShared<FJsonObject>();
	Patch->SetStringField(TEXT("path"), Path);
	Patch->SetStringField(TEXT("op"), TEXT("set"));
	Patch->SetField(TEXT("value"), CloneJsonValue(Value));
	if (bIncludeExpectedBefore)
	{
		Patch->SetStringField(TEXT("expected_before"), ExportProperty(Resolved.Property, Resolved.ValuePtr));
	}
	Patches.Add(Patch);
}

static void AddAppendPatchForMerge(UObject* Target, const FString& Path, const TSharedPtr<FJsonValue>& Value, const TSharedPtr<FJsonObject>& Options, TArray<TSharedPtr<FJsonObject>>& Patches, TArray<TSharedPtr<FJsonValue>>& Errors)
{
	FResolvedPropertyPath Resolved;
	if (!ResolvePropertyPath(Target, Target ? Target->GetClass() : nullptr, Path, Resolved))
	{
		AddMergeError(Errors, Path, Resolved.Error);
		return;
	}

	FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Resolved.Property);
	if (!ArrayProperty)
	{
		AddMergeError(Errors, Path, TEXT("append can only target an array property"));
		return;
	}

	TSharedPtr<FJsonObject> Patch = MakeShared<FJsonObject>();
	Patch->SetStringField(TEXT("path"), Path);
	Patch->SetStringField(TEXT("op"), TEXT("append"));
	Patch->SetField(TEXT("value"), CloneJsonValue(Value));
	Patches.Add(Patch);
}

static void FlattenMergeValue(UObject* Target, const UObject* DefaultObject, const FString& Path, const TSharedPtr<FJsonValue>& Value, const TSharedPtr<FJsonObject>& Options, TArray<TSharedPtr<FJsonObject>>& Patches, TArray<TSharedPtr<FJsonValue>>& Errors)
{
	if (!Target || Path.IsEmpty())
	{
		AddMergeError(Errors, Path, TEXT("Invalid merge target or empty path"));
		return;
	}

	FResolvedPropertyPath Resolved;
	if (!ResolvePropertyPath(Target, Target->GetClass(), Path, Resolved))
	{
		AddMergeError(Errors, Path, Resolved.Error);
		return;
	}

	if (!Value.IsValid() || Value->Type == EJson::Null)
	{
		bool bNullResets = false;
		if (Options.IsValid() && (Options->TryGetBoolField(TEXT("null_resets"), bNullResets) || Options->TryGetBoolField(TEXT("null_deletes"), bNullResets)) && bNullResets)
		{
			FResolvedPropertyPath DefaultResolved;
			if (DefaultObject && ResolvePropertyPath(const_cast<UObject*>(DefaultObject), DefaultObject->GetClass(), Path, DefaultResolved))
			{
				AddSetPatchForMerge(Target, Path, PropertyToJsonValue(DefaultResolved.Property, DefaultResolved.ValuePtr, MaxPropertyDepth), Options, Patches, Errors);
			}
			else
			{
				AddMergeError(Errors, Path, TEXT("Cannot reset null value because the default property path was not resolved"));
			}
		}
		else
		{
			AddMergeError(Errors, Path, TEXT("Null merge values are ignored by default; pass null_resets=true to reset to class defaults"));
		}
		return;
	}

	if (Value->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		if (FStructProperty* StructProperty = CastField<FStructProperty>(Resolved.Property))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
			{
				FlattenMergeValue(Target, DefaultObject, Path + TEXT(".") + Pair.Key, Pair.Value, Options, Patches, Errors);
			}
			return;
		}

		if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Resolved.Property))
		{
			AddMergeError(Errors, Path, FString::Printf(TEXT("Array property '%s' requires a JSON array for indexed union merge"), *ArrayProperty->GetName()));
			return;
		}

		AddMergeError(Errors, Path, TEXT("Object value can only be merged into a struct property"));
		return;
	}

	if (Value->Type == EJson::Array)
	{
		FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Resolved.Property);
		if (!ArrayProperty)
		{
			AddMergeError(Errors, Path, TEXT("Array value can only be merged into an array property"));
			return;
		}

		FScriptArrayHelper Helper(ArrayProperty, Resolved.ValuePtr);
		const TArray<TSharedPtr<FJsonValue>>& Array = Value->AsArray();
		bool bAppendArrays = true;
		if (Options.IsValid())
		{
			Options->TryGetBoolField(TEXT("append_arrays"), bAppendArrays);
			Options->TryGetBoolField(TEXT("allow_array_append"), bAppendArrays);
		}
		for (int32 Index = 0; Index < Array.Num(); ++Index)
		{
			const FString IndexPath = FString::Printf(TEXT("%s.%d"), *Path, Index);
			const TSharedPtr<FJsonValue>& ElementValue = Array[Index];
			if (!Helper.IsValidIndex(Index))
			{
				if (bAppendArrays && Index >= Helper.Num())
				{
					AddAppendPatchForMerge(Target, Path, ElementValue, Options, Patches, Errors);
				}
				else
				{
					AddMergeError(Errors, IndexPath, TEXT("Union merge array index is out of range and append_arrays=false"));
				}
				continue;
			}

			if (ElementValue.IsValid() && ElementValue->Type == EJson::Object && CastField<FStructProperty>(ArrayProperty->Inner))
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : ElementValue->AsObject()->Values)
				{
					FlattenMergeValue(Target, DefaultObject, IndexPath + TEXT(".") + Pair.Key, Pair.Value, Options, Patches, Errors);
				}
			}
			else
			{
				AddSetPatchForMerge(Target, IndexPath, ElementValue, Options, Patches, Errors);
			}
		}
		return;
	}

	AddSetPatchForMerge(Target, Path, Value, Options, Patches, Errors);
}

static TArray<TSharedPtr<FJsonObject>> BuildUnionMergePatches(UMassBattleAgentConfigDataAsset* Unit, const TSharedPtr<FJsonObject>& MergeRoot, const TSharedPtr<FJsonObject>& Options, TArray<TSharedPtr<FJsonValue>>& Errors)
{
	TArray<TSharedPtr<FJsonObject>> Patches;
	FString Error;
	TSharedPtr<FJsonObject> DataObject = ExtractMergeDataObject(MergeRoot, Error);
	if (!DataObject.IsValid())
	{
		AddMergeError(Errors, TEXT("Data"), Error);
		return Patches;
	}

	const UMassBattleAgentConfigDataAsset* DefaultUnit = GetDefault<UMassBattleAgentConfigDataAsset>();
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : DataObject->Values)
	{
		FString RoleError;
		if (!IsWritableTopLevelField(Pair.Key, Options, RoleError))
		{
			AddMergeError(Errors, Pair.Key, RoleError);
			continue;
		}
		FlattenMergeValue(Unit, DefaultUnit, Pair.Key, Pair.Value, Options, Patches, Errors);
	}
	return Patches;
}

static TArray<TSharedPtr<FJsonValue>> BuildReferencerJson(UMassBattleAgentConfigDataAsset* Unit)
{
	TArray<TSharedPtr<FJsonValue>> RefJson;
	if (!Unit)
	{
		return RefJson;
	}

	IAssetRegistry& Registry = GetAssetRegistry();
	TArray<FName> Referencers;
	Registry.GetReferencers(Unit->GetOutermost()->GetFName(), Referencers);
	for (const FName& Referencer : Referencers)
	{
		RefJson.Add(MakeShared<FJsonValueString>(Referencer.ToString()));
	}
	return RefJson;
}

static FString NormalizeDeleteMode(const TSharedPtr<FJsonObject>& Options)
{
	FString Mode = TEXT("soft");
	if (Options.IsValid())
	{
		Options->TryGetStringField(TEXT("mode"), Mode);
		Options->TryGetStringField(TEXT("delete_mode"), Mode);
	}
	Mode = Mode.ToLower();
	if (Mode == TEXT("permanent"))
	{
		return TEXT("hard");
	}
	return Mode == TEXT("hard") ? TEXT("hard") : TEXT("soft");
}

static bool ApplyDeletePlan(const TSharedPtr<FJsonObject>& Plan, const FString& PlanId, TSharedPtr<FJsonObject>& OutRoot, FString& OutError)
{
	FString TargetPath;
	FString DeleteMode = TEXT("soft");
	bool bForce = false;
	Plan->TryGetStringField(TEXT("target_path"), TargetPath);
	Plan->TryGetStringField(TEXT("delete_mode"), DeleteMode);
	Plan->TryGetBoolField(TEXT("force"), bForce);

	UMassBattleAgentConfigDataAsset* Unit = LoadUnit(TargetPath, OutError);
	if (!Unit)
	{
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Referencers = BuildReferencerJson(Unit);
	TSharedPtr<FJsonObject> Audit = MakeShared<FJsonObject>();
	Audit->SetStringField(TEXT("timestamp_utc"), FDateTime::UtcNow().ToIso8601());
	Audit->SetStringField(TEXT("plan_id"), PlanId);
	Audit->SetStringField(TEXT("type"), TEXT("delete_unit"));
	Audit->SetStringField(TEXT("asset_path"), Unit->GetPathName());
	Audit->SetStringField(TEXT("delete_mode"), DeleteMode);
	Audit->SetArrayField(TEXT("referencers"), Referencers);

	OutRoot = MakeSuccess();
	OutRoot->SetStringField(TEXT("asset_path"), Unit->GetPathName());
	OutRoot->SetStringField(TEXT("delete_mode"), DeleteMode);
	OutRoot->SetArrayField(TEXT("referencers"), Referencers);

	if (DeleteMode == TEXT("hard"))
	{
		int32 DeletedCount = 0;
		if (bForce)
		{
			TArray<UObject*> ObjectsToDelete = { Unit };
			DeletedCount = ObjectTools::ForceDeleteObjects(ObjectsToDelete, false);
		}
		else
		{
			TArray<FAssetData> AssetsToDelete = { FAssetData(Unit) };
			DeletedCount = ObjectTools::DeleteAssets(AssetsToDelete, false);
		}

		const bool bDeleted = DeletedCount > 0;
		OutRoot->SetBoolField(TEXT("deleted"), bDeleted);
		OutRoot->SetBoolField(TEXT("moved"), false);
		OutRoot->SetNumberField(TEXT("deleted_count"), DeletedCount);
		Audit->SetBoolField(TEXT("deleted"), bDeleted);
		AppendAuditRecord(Audit);
		if (!bDeleted)
		{
			OutError = FString::Printf(TEXT("ObjectTools deleted %d assets for %s"), DeletedCount, *TargetPath);
			return false;
		}
		return true;
	}

	FString TrashPackagePath;
	FString TrashAssetName;
	Plan->TryGetStringField(TEXT("trash_package_path"), TrashPackagePath);
	Plan->TryGetStringField(TEXT("trash_asset_name"), TrashAssetName);
	if (TrashPackagePath.IsEmpty() || TrashAssetName.IsEmpty())
	{
		OutError = TEXT("Soft delete plan is missing trash_package_path or trash_asset_name");
		return false;
	}

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	TArray<FAssetRenameData> RenameData;
	RenameData.Emplace(Unit, TrashPackagePath, TrashAssetName);
	const bool bMoved = AssetTools.RenameAssets(RenameData);
	if (!bMoved)
	{
		OutError = FString::Printf(TEXT("Failed to move %s to %s/%s"), *TargetPath, *TrashPackagePath, *TrashAssetName);
		return false;
	}

	OutRoot->SetBoolField(TEXT("deleted"), false);
	OutRoot->SetBoolField(TEXT("moved"), true);
	OutRoot->SetStringField(TEXT("trash_path"), TrashPackagePath / TrashAssetName + TEXT(".") + TrashAssetName);
	Audit->SetBoolField(TEXT("moved"), true);
	Audit->SetStringField(TEXT("trash_path"), TrashPackagePath / TrashAssetName + TEXT(".") + TrashAssetName);
	AppendAuditRecord(Audit);
	return true;
}

static FString EscapeCsv(const FString& In)
{
	FString Out = In;
	Out.ReplaceInline(TEXT("\""), TEXT("\"\""));
	return FString::Printf(TEXT("\"%s\""), *Out);
}

static TArray<FString> GetFallbackExportFields()
{
	return {
		TEXT("StyleType.Index"),
		TEXT("SubType.Index"),
		TEXT("Health.Maximum"),
		TEXT("Health.Current"),
		TEXT("Move.MovementModel"),
		TEXT("Move.XY.MoveSpeed"),
		TEXT("Move.XY.MoveAcceleration"),
		TEXT("Move.XY.AcceptanceRadius"),
		TEXT("Attack.bEnable"),
		TEXT("Attack.Range"),
		TEXT("Attack.CoolDown"),
		TEXT("Attack.DurationPerRound"),
		TEXT("Damage.Damage"),
		TEXT("Damage.DmgRadius"),
		TEXT("Defence.NormalDmgImmune"),
		TEXT("Collider.Radius"),
		TEXT("Collider.Height"),
		TEXT("Collider.Mass")
	};
}

static TArray<FString> GetDefaultExportFields(const TSharedPtr<FJsonObject>& Options)
{
	TArray<FString> Fields = ReadStringArrayField(LoadUnitStyleConfig(Options), TEXT("default_export_fields"));
	return Fields.IsEmpty() ? GetFallbackExportFields() : Fields;
}

static TArray<FString> GetExportFields(const TSharedPtr<FJsonObject>& Options)
{
	TArray<FString> Fields;
	const TArray<TSharedPtr<FJsonValue>>* FieldArray = nullptr;
	if (Options.IsValid() && Options->TryGetArrayField(TEXT("fields"), FieldArray))
	{
		for (const TSharedPtr<FJsonValue>& Value : *FieldArray)
		{
			const FString Field = Value->AsString();
			if (!Field.IsEmpty())
			{
				Fields.Add(Field);
			}
		}
	}
	return Fields.IsEmpty() ? GetDefaultExportFields(Options) : Fields;
}

static TSharedPtr<FJsonObject> BuildFieldSchemaObject(const FString& Path, FProperty* Property, const EFieldRole Role)
{
	TSharedPtr<FJsonObject> Field = MakeShared<FJsonObject>();
	Field->SetStringField(TEXT("path"), Path);
	Field->SetStringField(TEXT("role"), RoleToString(Role));
	Field->SetStringField(TEXT("cpp_type"), Property ? Property->GetCPPType() : FString());
	Field->SetBoolField(TEXT("editable"), Role != EFieldRole::RuntimeState && Role != EFieldRole::SystemOwned && Role != EFieldRole::Deprecated && Role != EFieldRole::VisibilityControl);
	if (Property)
	{
		const FString Tooltip = Property->GetMetaData(TEXT("ToolTip"));
		if (!Tooltip.IsEmpty())
		{
			Field->SetStringField(TEXT("tooltip"), Tooltip);
		}
	}
	return Field;
}

static void AddSchemaFieldsRecursive(TArray<TSharedPtr<FJsonValue>>& OutFields, UStruct* Struct, const FString& Prefix, EFieldRole Role, int32 Depth)
{
	if (!Struct || Depth < 0)
	{
		return;
	}

	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		FProperty* Property = *It;
		const FString Name = Property->GetName();
		if (ShouldSkipNestedField(Name))
		{
			continue;
		}

		const FString Path = Prefix.IsEmpty() ? Name : Prefix + TEXT(".") + Name;
		OutFields.Add(MakeShared<FJsonValueObject>(BuildFieldSchemaObject(Path, Property, Role)));

		if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			AddSchemaFieldsRecursive(OutFields, StructProperty->Struct, Path, Role, Depth - 1);
		}
	}
}

static void AddObjectClassFilter(FARFilter& Filter, UClass* Class)
{
	if (Class)
	{
		Filter.ClassPaths.Add(Class->GetClassPathName());
	}
}

} // namespace MassBattleUnitMCP

using namespace MassBattleUnitMCP;

FString UMassBattleUnitMCPApi::MCP_UnitList(const FString& OptionsJson)
{
	TSharedPtr<FJsonObject> Options = ParseObject(OptionsJson);
	if (!Options.IsValid())
	{
		return MakeErrorJson(TEXT("OptionsJson is not valid JSON"));
	}

	FString Query;
	Options->TryGetStringField(TEXT("query"), Query);
	Query = Query.ToLower();

	int32 Limit = MaxDefaultUnitList;
	TryGetIntField(Options, TEXT("limit"), Limit);
	Limit = FMath::Clamp(Limit, 1, 2000);

	bool bLoadFields = true;
	Options->TryGetBoolField(TEXT("load_fields"), bLoadFields);

	TArray<TSharedPtr<FJsonValue>> Units;
	const TArray<FAssetData> Assets = ScanUnitAssets(Options);
	for (const FAssetData& Asset : Assets)
	{
		const FString ObjectPath = Asset.GetObjectPathString();
		if (!Query.IsEmpty() && !ObjectPath.ToLower().Contains(Query))
		{
			continue;
		}
		if (Units.Num() >= Limit)
		{
			break;
		}
		Units.Add(MakeShared<FJsonValueObject>(BuildUnitSummary(Asset, bLoadFields, Options)));
	}

	TSharedPtr<FJsonObject> Root = MakeSuccess();
	Root->SetNumberField(TEXT("count"), Units.Num());
	Root->SetNumberField(TEXT("total_scanned"), Assets.Num());
	AddRootsToJson(Root, Options);
	Root->SetObjectField(TEXT("default_ignore_policy"), BuildIgnorePolicyJson());
	Root->SetStringField(TEXT("default_detail"), TEXT("simple"));
	Root->SetArrayField(TEXT("units"), Units);
	return ToJsonString(Root);
}

FString UMassBattleUnitMCPApi::MCP_UnitGet(const FString& UnitPath, const FString& OptionsJson)
{
	TSharedPtr<FJsonObject> Options = ParseObject(OptionsJson);
	if (!Options.IsValid())
	{
		return MakeErrorJson(TEXT("OptionsJson is not valid JSON"));
	}

	FString Error;
	UMassBattleAgentConfigDataAsset* Unit = LoadUnit(UnitPath, Error);
	if (!Unit)
	{
		return MakeErrorJson(Error);
	}

	TSharedPtr<FJsonObject> Root = MakeSuccess();
	Root->SetStringField(TEXT("AssetName"), Unit->GetName());
	Root->SetStringField(TEXT("ObjectPath"), Unit->GetPathName());
	Root->SetStringField(TEXT("Class"), Unit->GetClass()->GetPathName());
	Root->SetStringField(TEXT("detail"), GetDetailLabel(Options));
	Root->SetObjectField(TEXT("default_ignore_policy"), BuildIgnorePolicyJson());
	Root->SetObjectField(TEXT("Data"), BuildSourceAlignedUnitJson(Unit, Options));
	return ToJsonString(Root);
}

FString UMassBattleUnitMCPApi::MCP_UnitGetSchema(const FString& OptionsJson)
{
	TSharedPtr<FJsonObject> Options = ParseObject(OptionsJson);
	if (!Options.IsValid())
	{
		return MakeErrorJson(TEXT("OptionsJson is not valid JSON"));
	}

	bool bRecursive = true;
	Options->TryGetBoolField(TEXT("recursive"), bRecursive);
	int32 Depth = bRecursive ? 3 : 0;
	TryGetIntField(Options, TEXT("depth"), Depth);
	Depth = FMath::Clamp(Depth, 0, 8);

	TArray<TSharedPtr<FJsonValue>> Fields;
	for (TFieldIterator<FProperty> It(UMassBattleAgentConfigDataAsset::StaticClass()); It; ++It)
	{
		FProperty* Property = *It;
		const FString Name = Property->GetName();
		if (ShouldSkipTopLevelField(Name, Options))
		{
			continue;
		}

		const EFieldRole Role = ClassifyTopLevelField(Name);
		Fields.Add(MakeShared<FJsonValueObject>(BuildFieldSchemaObject(Name, Property, Role)));
		if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			AddSchemaFieldsRecursive(Fields, StructProperty->Struct, Name, Role, Depth - 1);
		}
	}

	TSharedPtr<FJsonObject> Root = MakeSuccess();
	Root->SetStringField(TEXT("asset_class"), UMassBattleAgentConfigDataAsset::StaticClass()->GetPathName());
	Root->SetObjectField(TEXT("default_ignore_policy"), BuildIgnorePolicyJson());
	Root->SetArrayField(TEXT("fields"), Fields);
	return ToJsonString(Root);
}

FString UMassBattleUnitMCPApi::MCP_UnitExport(const FString& OptionsJson)
{
	TSharedPtr<FJsonObject> Options = ParseObject(OptionsJson);
	if (!Options.IsValid())
	{
		return MakeErrorJson(TEXT("OptionsJson is not valid JSON"));
	}

	FString Format = TEXT("csv");
	Options->TryGetStringField(TEXT("format"), Format);
	Format = Format.ToLower();

	TArray<FString> Fields = GetExportFields(Options);
	TArray<FAssetData> Assets = ScanUnitAssets(Options);

	TArray<TSharedPtr<FJsonValue>> Rows;
	TArray<FString> CsvLines;
	TArray<FString> Header = { TEXT("AssetName"), TEXT("ObjectPath"), TEXT("PackagePath") };
	Header.Append(Fields);
	TArray<FString> EscapedHeader;
	for (const FString& Column : Header)
	{
		EscapedHeader.Add(EscapeCsv(Column));
	}
	CsvLines.Add(FString::Join(EscapedHeader, TEXT(",")));

	for (const FAssetData& Asset : Assets)
	{
		UObject* Loaded = Asset.GetAsset();
		TSharedPtr<FJsonObject> Row = BuildUnitSummary(Asset, false, Options);
		TArray<FString> CsvCells = {
			EscapeCsv(Asset.AssetName.ToString()),
			EscapeCsv(Asset.GetObjectPathString()),
			EscapeCsv(Asset.PackagePath.ToString())
		};

		for (const FString& Field : Fields)
		{
			FString Value;
			if (Loaded && TryExportPath(Loaded, Field, Value))
			{
				Row->SetStringField(Field, Value);
			}
			CsvCells.Add(EscapeCsv(Value));
		}
		Rows.Add(MakeShared<FJsonValueObject>(Row));
		CsvLines.Add(FString::Join(CsvCells, TEXT(",")));
	}

	IFileManager::Get().MakeDirectory(*GetExportDir(), true);
	FString OutputPath;
	if (!Options->TryGetStringField(TEXT("output_path"), OutputPath) || OutputPath.IsEmpty())
	{
		OutputPath = FPaths::Combine(GetExportDir(), FString::Printf(TEXT("massbattle_units_%s.%s"), *FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S")), Format == TEXT("json") ? TEXT("json") : TEXT("csv")));
	}

	bool bSaved = false;
	if (Format == TEXT("json"))
	{
		TSharedPtr<FJsonObject> ExportRoot = MakeShared<FJsonObject>();
		ExportRoot->SetArrayField(TEXT("fields"), [&Fields]()
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FString& Field : Fields)
			{
				Values.Add(MakeShared<FJsonValueString>(Field));
			}
			return Values;
		}());
		ExportRoot->SetArrayField(TEXT("rows"), Rows);
		bSaved = FFileHelper::SaveStringToFile(ToJsonString(ExportRoot), *OutputPath);
	}
	else
	{
		bSaved = FFileHelper::SaveStringToFile(FString::Join(CsvLines, LINE_TERMINATOR), *OutputPath);
	}

	if (!bSaved)
	{
		return MakeErrorJson(FString::Printf(TEXT("Failed to save export: %s"), *OutputPath));
	}

	TSharedPtr<FJsonObject> Root = MakeSuccess();
	Root->SetStringField(TEXT("format"), Format);
	Root->SetStringField(TEXT("output_path"), OutputPath);
	Root->SetNumberField(TEXT("unit_count"), Rows.Num());
	Root->SetArrayField(TEXT("fields"), [&Fields]()
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& Field : Fields)
		{
			Values.Add(MakeShared<FJsonValueString>(Field));
		}
		return Values;
	}());
	return ToJsonString(Root);
}

FString UMassBattleUnitMCPApi::MCP_UnitPlanUpdate(const FString& UnitPath, const FString& PatchJson)
{
	TSharedPtr<FJsonObject> PatchRoot = ParseObject(PatchJson);
	if (!PatchRoot.IsValid())
	{
		return MakeErrorJson(TEXT("PatchJson is not valid JSON"));
	}

	FString Error;
	UMassBattleAgentConfigDataAsset* Unit = LoadUnit(UnitPath, Error);
	if (!Unit)
	{
		return MakeErrorJson(Error);
	}

	TArray<TSharedPtr<FJsonObject>> Patches = GetPatchObjects(PatchRoot);
	if (Patches.IsEmpty())
	{
		return MakeErrorJson(TEXT("PatchJson requires a non-empty 'patches' array"));
	}

	bool bHasError = false;
	TArray<TSharedPtr<FJsonValue>> Diff = BuildPatchDiffArray(Unit, Patches, bHasError);

	TSharedPtr<FJsonObject> Plan = BuildPlanBase(TEXT("update_unit"));
	Plan->SetStringField(TEXT("target_path"), Unit->GetPathName());
	Plan->SetArrayField(TEXT("patches"), CopyPatchesToJsonValues(Patches));
	Plan->SetArrayField(TEXT("diff"), Diff);
	Plan->SetBoolField(TEXT("applicable"), !bHasError);

	FString PlanPath;
	if (!SavePlan(Plan, PlanPath, Error))
	{
		return MakeErrorJson(Error);
	}

	TSharedPtr<FJsonObject> Root = MakeSuccess();
	Root->SetStringField(TEXT("plan_id"), Plan->GetStringField(TEXT("plan_id")));
	Root->SetStringField(TEXT("plan_path"), PlanPath);
	Root->SetBoolField(TEXT("applicable"), !bHasError);
	Root->SetArrayField(TEXT("diff"), Diff);
	return ToJsonString(Root);
}

FString UMassBattleUnitMCPApi::MCP_UnitPlanMergeUpdate(const FString& UnitPath, const FString& UnitDataJson)
{
	TSharedPtr<FJsonObject> MergeRoot = ParseObject(UnitDataJson);
	if (!MergeRoot.IsValid())
	{
		return MakeErrorJson(TEXT("UnitDataJson is not valid JSON"));
	}

	FString Error;
	UMassBattleAgentConfigDataAsset* Unit = LoadUnit(UnitPath, Error);
	if (!Unit)
	{
		return MakeErrorJson(Error);
	}

	TSharedPtr<FJsonObject> Options = ExtractMergeOptions(MergeRoot);
	TArray<TSharedPtr<FJsonValue>> MergeErrors;
	TArray<TSharedPtr<FJsonObject>> Patches = BuildUnionMergePatches(Unit, MergeRoot, Options, MergeErrors);
	if (Patches.IsEmpty() && MergeErrors.IsEmpty())
	{
		return MakeErrorJson(TEXT("UnitDataJson does not contain any mergeable unit fields"));
	}

	bool bHasError = !MergeErrors.IsEmpty();
	TArray<TSharedPtr<FJsonValue>> Diff = BuildPatchDiffArray(Unit, Patches, bHasError);
	if (!MergeErrors.IsEmpty())
	{
		bHasError = true;
	}

	TSharedPtr<FJsonObject> Plan = BuildPlanBase(TEXT("merge_update_unit"));
	Plan->SetStringField(TEXT("target_path"), Unit->GetPathName());
	Plan->SetStringField(TEXT("merge_semantics"), TEXT("union: only JSON fields present in UnitDataJson are written; omitted fields remain unchanged."));
	Plan->SetArrayField(TEXT("patches"), CopyPatchesToJsonValues(Patches));
	Plan->SetArrayField(TEXT("diff"), Diff);
	Plan->SetArrayField(TEXT("merge_errors"), MergeErrors);
	Plan->SetBoolField(TEXT("applicable"), !bHasError);

	FString PlanPath;
	if (!SavePlan(Plan, PlanPath, Error))
	{
		return MakeErrorJson(Error);
	}

	TSharedPtr<FJsonObject> Root = MakeSuccess();
	Root->SetStringField(TEXT("plan_id"), Plan->GetStringField(TEXT("plan_id")));
	Root->SetStringField(TEXT("plan_path"), PlanPath);
	Root->SetStringField(TEXT("merge_semantics"), Plan->GetStringField(TEXT("merge_semantics")));
	Root->SetBoolField(TEXT("applicable"), !bHasError);
	Root->SetArrayField(TEXT("diff"), Diff);
	Root->SetArrayField(TEXT("merge_errors"), MergeErrors);
	return ToJsonString(Root);
}

FString UMassBattleUnitMCPApi::MCP_UnitMergeUpdate(const FString& UnitPath, const FString& UnitDataJson, bool bSaveAssets)
{
	const FString PlanResult = MCP_UnitPlanMergeUpdate(UnitPath, UnitDataJson);
	TSharedPtr<FJsonObject> PlanResultJson = ParseObject(PlanResult);
	if (!PlanResultJson.IsValid() || !PlanResultJson->GetBoolField(TEXT("success")))
	{
		return PlanResult;
	}

	bool bApplicable = false;
	PlanResultJson->TryGetBoolField(TEXT("applicable"), bApplicable);
	if (!bApplicable)
	{
		return PlanResult;
	}

	const FString PlanId = PlanResultJson->GetStringField(TEXT("plan_id"));
	return MCP_UnitApplyPlan(PlanId, bSaveAssets);
}

FString UMassBattleUnitMCPApi::MCP_UnitCreate(const FString& CreateSpecJson, bool bSaveAssets)
{
	TSharedPtr<FJsonObject> Spec = ParseObject(CreateSpecJson);
	if (!Spec.IsValid())
	{
		return MakeErrorJson(TEXT("CreateSpecJson is not valid JSON"));
	}

	FString TemplatePath;
	Spec->TryGetStringField(TEXT("template_unit"), TemplatePath);
	Spec->TryGetStringField(TEXT("source_unit"), TemplatePath);
	if (TemplatePath.IsEmpty())
	{
		FString TemplateError;
		TemplatePath = ResolveDefaultUnitTemplatePath(TemplateError);
		if (TemplatePath.IsEmpty())
		{
			return MakeErrorJson(TemplateError);
		}
	}

	FString AssetName;
	FString PackagePath;
	FString UnitPath;
	FString Error;
	if (!ResolveCreateTarget(Spec, AssetName, PackagePath, UnitPath, Error))
	{
		return MakeErrorJson(Error);
	}

	if (LoadObject<UObject>(nullptr, *UnitPath))
	{
		return MakeErrorJson(FString::Printf(TEXT("Target unit already exists: %s"), *UnitPath));
	}

	UObject* NewUnit = DuplicateAsset(TemplatePath, AssetName, PackagePath, Error);
	if (!NewUnit)
	{
		return MakeErrorJson(Error);
	}

	TArray<TSharedPtr<FJsonValue>> AppliedDiffs;
	TArray<TSharedPtr<FJsonValue>> MergeErrors;
	const TSharedPtr<FJsonObject>* InitialData = nullptr;
	if (Spec->TryGetObjectField(TEXT("unit_data"), InitialData) || Spec->TryGetObjectField(TEXT("initial_unit_data"), InitialData))
	{
		TSharedPtr<FJsonObject> Options = ExtractMergeOptions(*InitialData);
		TArray<TSharedPtr<FJsonObject>> Patches = BuildUnionMergePatches(Cast<UMassBattleAgentConfigDataAsset>(NewUnit), *InitialData, Options, MergeErrors);
		if (!MergeErrors.IsEmpty())
		{
			TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetBoolField(TEXT("success"), false);
			Root->SetStringField(TEXT("error"), TEXT("Create-time unit data contains non-mergeable fields"));
			Root->SetArrayField(TEXT("merge_errors"), MergeErrors);
			return ToJsonString(Root);
		}

		for (const TSharedPtr<FJsonObject>& Patch : Patches)
		{
			FPatchPreview Preview;
			if (!ApplyPatch(NewUnit, Patch, false, Preview))
			{
				return MakeErrorJson(Preview.Error);
			}
			AppliedDiffs.Add(MakeShared<FJsonValueObject>(PatchPreviewToJson(Preview)));
		}
	}

	if (bSaveAssets && !SaveAsset(NewUnit, Error))
	{
		return MakeErrorJson(Error);
	}

	TSharedPtr<FJsonObject> Audit = MakeShared<FJsonObject>();
	Audit->SetStringField(TEXT("timestamp_utc"), FDateTime::UtcNow().ToIso8601());
	Audit->SetStringField(TEXT("type"), TEXT("create_unit"));
	Audit->SetStringField(TEXT("template_path"), TemplatePath);
	Audit->SetStringField(TEXT("asset_path"), NewUnit->GetPathName());
	Audit->SetArrayField(TEXT("diff"), AppliedDiffs);
	AppendAuditRecord(Audit);

	TSharedPtr<FJsonObject> Root = MakeSuccess();
	Root->SetStringField(TEXT("operation"), TEXT("create_unit"));
	Root->SetStringField(TEXT("template_path"), TemplatePath);
	Root->SetStringField(TEXT("asset_path"), NewUnit->GetPathName());
	Root->SetBoolField(TEXT("created"), true);
	Root->SetBoolField(TEXT("saved"), bSaveAssets);
	Root->SetArrayField(TEXT("applied_diff"), AppliedDiffs);
	return ToJsonString(Root);
}

FString UMassBattleUnitMCPApi::MCP_UnitPreviewDiff(const FString& PlanId)
{
	FString Error;
	TSharedPtr<FJsonObject> Plan = LoadPlan(PlanId, Error);
	if (!Plan.IsValid())
	{
		return MakeErrorJson(Error);
	}

	TSharedPtr<FJsonObject> Root = MakeSuccess();
	FString PlanIdValue;
	FString Type;
	bool bApplicable = false;
	Plan->TryGetStringField(TEXT("plan_id"), PlanIdValue);
	Plan->TryGetStringField(TEXT("type"), Type);
	Plan->TryGetBoolField(TEXT("applicable"), bApplicable);
	Root->SetStringField(TEXT("plan_id"), PlanIdValue);
	Root->SetStringField(TEXT("type"), Type);
	Root->SetBoolField(TEXT("applicable"), bApplicable);

	const TArray<TSharedPtr<FJsonValue>>* Diff = nullptr;
	if (Plan->TryGetArrayField(TEXT("diff"), Diff))
	{
		Root->SetArrayField(TEXT("diff"), *Diff);
	}
	if (Plan->TryGetArrayField(TEXT("diff_from_template"), Diff))
	{
		Root->SetArrayField(TEXT("diff_from_template"), *Diff);
	}

	Root->SetObjectField(TEXT("plan"), Plan);
	return ToJsonString(Root);
}

FString UMassBattleUnitMCPApi::MCP_UnitApplyPlan(const FString& PlanId, bool bSaveAssets)
{
	FString Error;
	TSharedPtr<FJsonObject> Plan = LoadPlan(PlanId, Error);
	if (!Plan.IsValid())
	{
		return MakeErrorJson(Error);
	}

	bool bApplicable = false;
	Plan->TryGetBoolField(TEXT("applicable"), bApplicable);
	if (!bApplicable)
	{
		return MakeErrorJson(TEXT("Plan is not applicable. Inspect diff errors first."));
	}

	FString Type;
	Plan->TryGetStringField(TEXT("type"), Type);

	if (Type == TEXT("delete_unit"))
	{
		TSharedPtr<FJsonObject> DeleteRoot;
		if (!ApplyDeletePlan(Plan, PlanId, DeleteRoot, Error))
		{
			return MakeErrorJson(Error);
		}
		return ToJsonString(DeleteRoot);
	}

	UObject* Target = nullptr;
	if (Type == TEXT("update_unit") || Type == TEXT("merge_update_unit"))
	{
		FString TargetPath;
		Plan->TryGetStringField(TEXT("target_path"), TargetPath);
		Target = LoadUnit(TargetPath, Error);
		if (!Target)
		{
			return MakeErrorJson(Error);
		}
	}
	else
	{
		return MakeErrorJson(FString::Printf(TEXT("Unsupported plan type: %s"), *Type));
	}

	TArray<TSharedPtr<FJsonObject>> Patches;
	for (const TSharedPtr<FJsonValue>& Value : ParseArrayField(Plan, TEXT("patches")))
	{
		TSharedPtr<FJsonObject> Patch = Value->AsObject();
		if (Patch.IsValid())
		{
			Patches.Add(Patch);
		}
	}

	TArray<TSharedPtr<FJsonValue>> AppliedDiffs;
	for (const TSharedPtr<FJsonObject>& Patch : Patches)
	{
		FPatchPreview Preview;
		if (!ApplyPatch(Target, Patch, false, Preview))
		{
			return MakeErrorJson(Preview.Error);
		}
		AppliedDiffs.Add(MakeShared<FJsonValueObject>(PatchPreviewToJson(Preview)));
	}

	if (bSaveAssets && !SaveAsset(Target, Error))
	{
		return MakeErrorJson(Error);
	}

	TSharedPtr<FJsonObject> Audit = MakeShared<FJsonObject>();
	Audit->SetStringField(TEXT("timestamp_utc"), FDateTime::UtcNow().ToIso8601());
	Audit->SetStringField(TEXT("plan_id"), PlanId);
	Audit->SetStringField(TEXT("type"), Type);
	Audit->SetStringField(TEXT("asset_path"), Target->GetPathName());
	Audit->SetArrayField(TEXT("diff"), AppliedDiffs);
	AppendAuditRecord(Audit);

	TSharedPtr<FJsonObject> Root = MakeSuccess();
	Root->SetStringField(TEXT("asset_path"), Target->GetPathName());
	Root->SetBoolField(TEXT("saved"), bSaveAssets);
	Root->SetArrayField(TEXT("applied_diff"), AppliedDiffs);
	return ToJsonString(Root);
}

FString UMassBattleUnitMCPApi::MCP_UnitDeleteSoft(const FString& UnitPath, const FString& OptionsJson)
{
	TSharedPtr<FJsonObject> Options = ParseObject(OptionsJson);
	if (!Options.IsValid())
	{
		return MakeErrorJson(TEXT("OptionsJson is not valid JSON"));
	}

	bool bDryRun = true;
	Options->TryGetBoolField(TEXT("dry_run"), bDryRun);

	FString Error;
	UMassBattleAgentConfigDataAsset* Unit = LoadUnit(UnitPath, Error);
	if (!Unit)
	{
		return MakeErrorJson(Error);
	}

	IAssetRegistry& Registry = GetAssetRegistry();
	TArray<FName> Referencers;
	Registry.GetReferencers(Unit->GetOutermost()->GetFName(), Referencers);

	TArray<TSharedPtr<FJsonValue>> RefJson;
	for (const FName& Referencer : Referencers)
	{
		RefJson.Add(MakeShared<FJsonValueString>(Referencer.ToString()));
	}

	FString TrashRoot = TEXT("/Game/_Trash/MassBattle/Units");
	Options->TryGetStringField(TEXT("trash_root"), TrashRoot);
	const FString DateFolder = FDateTime::UtcNow().ToString(TEXT("%Y-%m-%d"));
	const FString NewPackagePath = TrashRoot / DateFolder;
	const FString NewName = Unit->GetName();

	bool bMoved = false;
	if (!bDryRun)
	{
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		TArray<FAssetRenameData> RenameData;
		RenameData.Emplace(Unit, NewPackagePath, NewName);
		bMoved = AssetTools.RenameAssets(RenameData);
		if (!bMoved)
		{
			return MakeErrorJson(FString::Printf(TEXT("Failed to move %s to %s/%s"), *UnitPath, *NewPackagePath, *NewName));
		}
	}

	TSharedPtr<FJsonObject> Root = MakeSuccess();
	Root->SetBoolField(TEXT("dry_run"), bDryRun);
	Root->SetBoolField(TEXT("moved"), bMoved);
	Root->SetStringField(TEXT("source_path"), UnitPath);
	Root->SetStringField(TEXT("trash_path"), NewPackagePath / NewName + TEXT(".") + NewName);
	Root->SetArrayField(TEXT("referencers"), RefJson);
	Root->SetStringField(TEXT("safety_note"), TEXT("This is a soft delete by move. Permanent deletion and reference replacement are intentionally not part of this tool."));
	return ToJsonString(Root);
}

FString UMassBattleUnitMCPApi::MCP_UnitPlanDelete(const FString& UnitPath, const FString& OptionsJson)
{
	TSharedPtr<FJsonObject> Options = ParseObject(OptionsJson);
	if (!Options.IsValid())
	{
		return MakeErrorJson(TEXT("OptionsJson is not valid JSON"));
	}

	FString Error;
	UMassBattleAgentConfigDataAsset* Unit = LoadUnit(UnitPath, Error);
	if (!Unit)
	{
		return MakeErrorJson(Error);
	}

	const FString DeleteMode = NormalizeDeleteMode(Options);
	TArray<TSharedPtr<FJsonValue>> Referencers = BuildReferencerJson(Unit);

	bool bAllowReferenced = DeleteMode == TEXT("soft");
	Options->TryGetBoolField(TEXT("allow_referenced"), bAllowReferenced);
	bool bForce = false;
	Options->TryGetBoolField(TEXT("force"), bForce);

	FString TrashRoot = TEXT("/Game/_Trash/MassBattle/Units");
	Options->TryGetStringField(TEXT("trash_root"), TrashRoot);
	const FString DateFolder = FDateTime::UtcNow().ToString(TEXT("%Y-%m-%d"));
	const FString TrashPackagePath = TrashRoot / DateFolder;
	const FString TrashAssetName = Unit->GetName();
	const FString TrashPath = TrashPackagePath / TrashAssetName + TEXT(".") + TrashAssetName;

	const bool bHasReferencers = !Referencers.IsEmpty();
	const bool bApplicable = DeleteMode == TEXT("soft") || !bHasReferencers || bAllowReferenced;

	TSharedPtr<FJsonObject> Plan = BuildPlanBase(TEXT("delete_unit"));
	Plan->SetStringField(TEXT("target_path"), Unit->GetPathName());
	Plan->SetStringField(TEXT("delete_mode"), DeleteMode);
	Plan->SetArrayField(TEXT("referencers"), Referencers);
	Plan->SetBoolField(TEXT("allow_referenced"), bAllowReferenced);
	Plan->SetBoolField(TEXT("force"), bForce);
	Plan->SetBoolField(TEXT("applicable"), bApplicable);
	Plan->SetStringField(TEXT("safety_note"), TEXT("Soft delete moves the asset to trash. Hard delete permanently deletes the asset and is blocked by referencers unless allow_referenced=true."));
	if (DeleteMode == TEXT("soft"))
	{
		Plan->SetStringField(TEXT("trash_package_path"), TrashPackagePath);
		Plan->SetStringField(TEXT("trash_asset_name"), TrashAssetName);
		Plan->SetStringField(TEXT("trash_path"), TrashPath);
	}
	if (!bApplicable)
	{
		Plan->SetStringField(TEXT("blocked_reason"), TEXT("Hard delete has referencers and allow_referenced=false"));
	}

	FString PlanPath;
	if (!SavePlan(Plan, PlanPath, Error))
	{
		return MakeErrorJson(Error);
	}

	TSharedPtr<FJsonObject> Root = MakeSuccess();
	Root->SetStringField(TEXT("plan_id"), Plan->GetStringField(TEXT("plan_id")));
	Root->SetStringField(TEXT("plan_path"), PlanPath);
	Root->SetStringField(TEXT("target_path"), Unit->GetPathName());
	Root->SetStringField(TEXT("delete_mode"), DeleteMode);
	Root->SetBoolField(TEXT("applicable"), bApplicable);
	Root->SetBoolField(TEXT("allow_referenced"), bAllowReferenced);
	Root->SetBoolField(TEXT("force"), bForce);
	Root->SetArrayField(TEXT("referencers"), Referencers);
	if (DeleteMode == TEXT("soft"))
	{
		Root->SetStringField(TEXT("trash_path"), TrashPath);
	}
	if (!bApplicable)
	{
		Root->SetStringField(TEXT("blocked_reason"), TEXT("Hard delete has referencers and allow_referenced=false"));
	}
	return ToJsonString(Root);
}

FString UMassBattleUnitMCPApi::MCP_UnitDelete(const FString& UnitPath, const FString& OptionsJson)
{
	TSharedPtr<FJsonObject> Options = ParseObject(OptionsJson);
	if (!Options.IsValid())
	{
		return MakeErrorJson(TEXT("OptionsJson is not valid JSON"));
	}

	bool bDryRun = true;
	Options->TryGetBoolField(TEXT("dry_run"), bDryRun);
	const FString PlanResult = MCP_UnitPlanDelete(UnitPath, OptionsJson);
	TSharedPtr<FJsonObject> PlanResultJson = ParseObject(PlanResult);
	if (!PlanResultJson.IsValid() || !PlanResultJson->GetBoolField(TEXT("success")))
	{
		return PlanResult;
	}

	if (bDryRun)
	{
		PlanResultJson->SetBoolField(TEXT("dry_run"), true);
		PlanResultJson->SetBoolField(TEXT("would_delete"), true);
		return ToJsonString(PlanResultJson);
	}

	bool bApplicable = false;
	PlanResultJson->TryGetBoolField(TEXT("applicable"), bApplicable);
	if (!bApplicable)
	{
		return PlanResult;
	}

	const FString PlanId = PlanResultJson->GetStringField(TEXT("plan_id"));
	return MCP_UnitApplyPlan(PlanId, true);
}

FString UMassBattleUnitMCPApi::MCP_UnitFindAssets(const FString& QueryJson)
{
	TSharedPtr<FJsonObject> Query = ParseObject(QueryJson);
	if (!Query.IsValid())
	{
		return MakeErrorJson(TEXT("QueryJson is not valid JSON"));
	}

	FString Text;
	Query->TryGetStringField(TEXT("query"), Text);
	Text = Text.ToLower();

	int32 Limit = 100;
	TryGetIntField(Query, TEXT("limit"), Limit);
	Limit = FMath::Clamp(Limit, 1, 1000);

	TArray<FName> Roots = ParseRoots(Query);
	TArray<FString> RootStrings;
	for (const FName& Root : Roots)
	{
		RootStrings.Add(Root.ToString());
	}

	IAssetRegistry& Registry = GetAssetRegistry();
	Registry.ScanPathsSynchronous(RootStrings, true);

	FARFilter Filter;
	Filter.bRecursivePaths = true;
	Filter.PackagePaths = Roots;
	AddObjectClassFilter(Filter, UMassBattleAgentConfigDataAsset::StaticClass());
	AddObjectClassFilter(Filter, UStaticMesh::StaticClass());
	AddObjectClassFilter(Filter, USkeletalMesh::StaticClass());
	AddObjectClassFilter(Filter, UNiagaraSystem::StaticClass());
	AddObjectClassFilter(Filter, UAnimToTextureDataAsset::StaticClass());
	AddObjectClassFilter(Filter, UBlueprint::StaticClass());
	Filter.bRecursiveClasses = true;

	TArray<FAssetData> Assets;
	Registry.GetAssets(Filter, Assets);
	Assets.Sort([](const FAssetData& A, const FAssetData& B)
	{
		return A.GetObjectPathString() < B.GetObjectPathString();
	});

	TArray<TSharedPtr<FJsonValue>> Results;
	for (const FAssetData& Asset : Assets)
	{
		const FString ObjectPath = Asset.GetObjectPathString();
		if (!Text.IsEmpty() && !ObjectPath.ToLower().Contains(Text))
		{
			continue;
		}
		if (Results.Num() >= Limit)
		{
			break;
		}

		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		Item->SetStringField(TEXT("path"), ObjectPath);
		Item->SetStringField(TEXT("class"), Asset.AssetClassPath.ToString());
		Item->SetStringField(TEXT("package_path"), Asset.PackagePath.ToString());
		Item->SetStringField(TEXT("style_family"), InferStyleFamily(ObjectPath));
		Results.Add(MakeShared<FJsonValueObject>(Item));
	}

	TSharedPtr<FJsonObject> Root = MakeSuccess();
	Root->SetNumberField(TEXT("count"), Results.Num());
	AddRootsToJson(Root, Query);
	Root->SetArrayField(TEXT("assets"), Results);
	return ToJsonString(Root);
}

FString UMassBattleUnitMCPApi::MCP_UnitGetApiStatus()
{
	TSharedPtr<FJsonObject> Root = MakeSuccess();
	Root->SetStringField(TEXT("api_name"), TEXT("MassBattleUnitMCPApi"));
	Root->SetStringField(TEXT("module"), TEXT("MassBattleEditorMCP"));
	Root->SetStringField(TEXT("dependency_note"), TEXT("Independent from UMGMCP. Any MCP transport can call these JSON UFUNCTION endpoints."));

	auto Tool = [](const FString& Name, const FString& Category, const FString& Description)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Name);
		Obj->SetStringField(TEXT("category"), Category);
		Obj->SetStringField(TEXT("description"), Description);
		return MakeShared<FJsonValueObject>(Obj);
	};

	TArray<TSharedPtr<FJsonValue>> Tools;
	Tools.Add(Tool(TEXT("MCP_UnitList"), TEXT("unit.query"), TEXT("List MassBattle unit config assets.")));
	Tools.Add(Tool(TEXT("MCP_UnitGet"), TEXT("unit.query"), TEXT("Get one unit with default ignore policy.")));
	Tools.Add(Tool(TEXT("MCP_UnitGetSchema"), TEXT("unit.schema"), TEXT("Expose editable schema and field roles.")));
	Tools.Add(Tool(TEXT("MCP_UnitExport"), TEXT("unit.export"), TEXT("Export balance fields to CSV/JSON.")));
	Tools.Add(Tool(TEXT("MCP_UnitCreate"), TEXT("unit.create"), TEXT("Create a unit from the default or specified template, then optionally apply initial unit data.")));
	Tools.Add(Tool(TEXT("MCP_UnitMergeUpdate"), TEXT("unit.write"), TEXT("Union-write partial source-aligned JSON to an existing unit and optionally save.")));
	Tools.Add(Tool(TEXT("MCP_UnitDeleteSoft"), TEXT("unit.lifecycle"), TEXT("Move a unit to trash after referencer scan.")));
	Tools.Add(Tool(TEXT("MCP_UnitDelete"), TEXT("unit.delete"), TEXT("Delete a unit explicitly; dry_run=true by default.")));
	Tools.Add(Tool(TEXT("MCP_UnitFindAssets"), TEXT("unit.assets"), TEXT("Find existing assets for unit generation.")));
	Tools.Add(Tool(TEXT("MCP_StyleSummarizeUnits"), TEXT("style.query"), TEXT("Summarize style organization.")));
	Tools.Add(Tool(TEXT("MCP_StylePlanOrganizeUnits"), TEXT("style.organize"), TEXT("Plan style-based folder organization.")));
	Tools.Add(Tool(TEXT("MCP_EditorListProfiles"), TEXT("unit_editor.profile"), TEXT("List style profiles and authoring recipes.")));
	Tools.Add(Tool(TEXT("MCP_EditorGetProfile"), TEXT("unit_editor.profile"), TEXT("Read one style profile or authoring recipe.")));
	Tools.Add(Tool(TEXT("MCP_EditorPlanUnitAuthoringWorkflow"), TEXT("unit_editor.workflow"), TEXT("Plan a staged workflow across prepare, animation update, VAT create/refresh, and organization.")));
	Tools.Add(Tool(TEXT("MCP_EditorApplyUnitAuthoringWorkflow"), TEXT("unit_editor.workflow"), TEXT("Apply a reviewed staged unit authoring workflow; dry_run=true by default.")));
	Tools.Add(Tool(TEXT("MCP_EditorPlanPreparePurchasedAsset"), TEXT("unit_editor.prepare"), TEXT("Plan discovery, official naming, and optional source-folder organization for a purchased skeletal asset pack.")));
	Tools.Add(Tool(TEXT("MCP_EditorApplyPreparePurchasedAsset"), TEXT("unit_editor.prepare"), TEXT("Apply a reviewed purchased-asset preparation plan; dry_run=true by default.")));
	Tools.Add(Tool(TEXT("MCP_EditorDiscoverCompatibleAnimations"), TEXT("unit_editor.animation"), TEXT("Discover compatible animation sequences across explicit and style-configured search roots.")));
	Tools.Add(Tool(TEXT("MCP_EditorPlanAddAnimationsToUnit"), TEXT("unit_editor.animation"), TEXT("Use MassBattleEditor functions to plan an AnimShared update for an existing unit.")));
	Tools.Add(Tool(TEXT("MCP_EditorValidateAddAnimationsToUnit"), TEXT("unit_editor.animation"), TEXT("Validate whether an animation-set edit can produce an applicable unit merge plan.")));
	Tools.Add(Tool(TEXT("MCP_EditorApplyAddAnimationsToUnit"), TEXT("unit_editor.animation"), TEXT("Plan and apply an AnimShared update for an existing unit.")));
	Tools.Add(Tool(TEXT("MCP_EditorPlanCreateVatUnit"), TEXT("unit_editor.create.diagnostic"), TEXT("Diagnostic: preview the MassBattleTools DoAll-equivalent VAT unit spec with resolved defaults and warnings.")));
	Tools.Add(Tool(TEXT("MCP_EditorValidateCreateVatUnit"), TEXT("unit_editor.create.diagnostic"), TEXT("Diagnostic: validate DoAll-equivalent VAT unit inputs without writing assets.")));
	Tools.Add(Tool(TEXT("MCP_EditorApplyCreateVatUnit"), TEXT("unit_editor.create"), TEXT("Primary non-selection DoAll-equivalent VAT unit authoring entry; defaults missing fields and returns warnings.")));
	Tools.Add(Tool(TEXT("MCP_EditorPlanCreateVatUnitFromSelection"), TEXT("unit_editor.create.diagnostic"), TEXT("Diagnostic: infer the DoAll spec from current selection or selected_assets and return it for review.")));
	Tools.Add(Tool(TEXT("MCP_EditorApplyCreateVatUnitFromSelection"), TEXT("unit_editor.create"), TEXT("Primary one-click current selection -> generate entry matching the MassBattleTools DoAll workflow.")));
	Tools.Add(Tool(TEXT("MCP_EditorPlanOrganizeUnitAssets"), TEXT("unit_editor.organize"), TEXT("Plan moving a unit and its editor-generated linked assets into the selected style layout.")));
	Tools.Add(Tool(TEXT("MCP_EditorApplyOrganizeUnitAssets"), TEXT("unit_editor.organize"), TEXT("Apply a reviewed linked-asset organization plan; dry_run=true by default.")));
	Root->SetArrayField(TEXT("tools"), Tools);
	Root->SetObjectField(TEXT("default_ignore_policy"), BuildIgnorePolicyJson());
	return ToJsonString(Root);
}

FString UMassBattleStyleMCPApi::MCP_StyleSummarizeUnits(const FString& OptionsJson)
{
	TSharedPtr<FJsonObject> Options = ParseObject(OptionsJson);
	if (!Options.IsValid())
	{
		return MakeErrorJson(TEXT("OptionsJson is not valid JSON"));
	}

	TMap<FString, int32> FamilyCounts;
	TMap<FString, int32> CategoryCounts;
	TMap<FString, int32> StyleIndexCounts;
	int32 UnitCount = 0;

	for (const FAssetData& Asset : ScanUnitAssets(Options))
	{
		UnitCount++;
		const FString ObjectPath = Asset.GetObjectPathString();
		FamilyCounts.FindOrAdd(InferStyleFamily(ObjectPath))++;
		CategoryCounts.FindOrAdd(InferPathCategory(ObjectPath))++;

		FString StyleIndex = TEXT("unknown");
		if (UObject* Loaded = Asset.GetAsset())
		{
			TryExportPath(Loaded, TEXT("StyleType.Index"), StyleIndex);
		}
		StyleIndexCounts.FindOrAdd(StyleIndex)++;
	}

	auto MapToObject = [](const TMap<FString, int32>& Map)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		for (const TPair<FString, int32>& Pair : Map)
		{
			Obj->SetNumberField(Pair.Key, Pair.Value);
		}
		return Obj;
	};

	TSharedPtr<FJsonObject> Root = MakeSuccess();
	AddRootsToJson(Root, Options);
	Root->SetNumberField(TEXT("unit_count"), UnitCount);
	Root->SetObjectField(TEXT("style_families"), MapToObject(FamilyCounts));
	Root->SetObjectField(TEXT("path_categories"), MapToObject(CategoryCounts));
	Root->SetObjectField(TEXT("style_indices"), MapToObject(StyleIndexCounts));
	return ToJsonString(Root);
}

FString UMassBattleStyleMCPApi::MCP_StylePlanOrganizeUnits(const FString& OptionsJson)
{
	TSharedPtr<FJsonObject> Options = ParseObject(OptionsJson);
	if (!Options.IsValid())
	{
		return MakeErrorJson(TEXT("OptionsJson is not valid JSON"));
	}

	FString TargetRoot = TEXT("/Game/MassBattle/Units");
	Options->TryGetStringField(TEXT("target_root"), TargetRoot);

	TArray<TSharedPtr<FJsonValue>> Moves;
	for (const FAssetData& Asset : ScanUnitAssets(Options))
	{
		const FString ObjectPath = Asset.GetObjectPathString();
		const FString Family = InferStyleFamily(ObjectPath);
		FString StyleIndex = TEXT("0");
		if (UObject* Loaded = Asset.GetAsset())
		{
			TryExportPath(Loaded, TEXT("StyleType.Index"), StyleIndex);
		}

		const FString DestinationPackagePath = TargetRoot / Family / (TEXT("Style") + StyleIndex);
		TSharedPtr<FJsonObject> Move = MakeShared<FJsonObject>();
		Move->SetStringField(TEXT("source_path"), ObjectPath);
		Move->SetStringField(TEXT("asset_name"), Asset.AssetName.ToString());
		Move->SetStringField(TEXT("style_family"), Family);
		Move->SetStringField(TEXT("style_index"), StyleIndex);
		Move->SetStringField(TEXT("destination_package_path"), DestinationPackagePath);
		Move->SetStringField(TEXT("destination_path"), DestinationPackagePath / Asset.AssetName.ToString() + TEXT(".") + Asset.AssetName.ToString());
		Moves.Add(MakeShared<FJsonValueObject>(Move));
	}

	TSharedPtr<FJsonObject> Root = MakeSuccess();
	Root->SetStringField(TEXT("plan_type"), TEXT("style_organize_units"));
	Root->SetStringField(TEXT("target_root"), TargetRoot);
	Root->SetBoolField(TEXT("dry_run_only"), true);
	Root->SetStringField(TEXT("note"), TEXT("This style MCP currently produces a reviewable move plan only; use AssetTools-based move support after confirming folder conventions."));
	Root->SetArrayField(TEXT("moves"), Moves);
	return ToJsonString(Root);
}
