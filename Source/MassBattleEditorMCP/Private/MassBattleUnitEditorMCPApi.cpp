// Copyright (c) 2025-2026 Winyunq. All rights reserved.
#include "MassBattleUnitEditorMCPApi.h"

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AnimToTextureBPLibrary.h"
#include "AnimToTextureDataAsset.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "DataAssets/MassBattleAgentConfigDataAsset.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/Selection.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "MassBattleEditorMCPApi.h"
#include "MassBattleEditorStructs.h"
#include "MassBattleFuncLibEd.h"
#include "MassBattleUnitMCPApi.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "NiagaraSystem.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"

DEFINE_LOG_CATEGORY(LogMassBattleUnitEditorMCPApi);

namespace MassBattleUnitEditorMCP
{
static constexpr TCHAR DefaultVatParentMaterialPath[] = TEXT("/MassBattle/Core/AgentRenderer/VAT/M_VATMaster.M_VATMaster");
static constexpr TCHAR DefaultAgentRendererClassPath[] = TEXT("/MassBattle/Core/AgentRenderer/BP_AgentRenderer_Template.BP_AgentRenderer_Template_C");
static constexpr TCHAR DefaultAgentRendererNiagaraPath[] = TEXT("/MassBattle/Core/AgentRenderer/NS_AgentRenderer_Template.NS_AgentRenderer_Template");

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

static FString GetPluginRootDir()
{
	return FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("MassBattleEditorMCP"));
}

static FString GetProfileDir(const FString& ProfileType)
{
	if (ProfileType.Equals(TEXT("recipe"), ESearchCase::IgnoreCase) || ProfileType.Equals(TEXT("recipes"), ESearchCase::IgnoreCase))
	{
		return FPaths::Combine(GetPluginRootDir(), TEXT("Resources"), TEXT("UnitAuthoringRecipes"));
	}
	return FPaths::Combine(GetPluginRootDir(), TEXT("Resources"), TEXT("UnitManagementStyles"));
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

static FString FindProfileFile(const FString& ProfileType, const FString& ProfileId)
{
	const FString Directory = GetProfileDir(ProfileType);
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("*.json"), true, false);
	Files.Sort();

	const FString WantedId = ProfileId.IsEmpty() ? TEXT("default") : ProfileId;
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

static TSharedPtr<FJsonObject> ProfileSummaryFromFile(const FString& FilePath, const FString& ProfileType)
{
	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetStringField(TEXT("type"), ProfileType);
	Summary->SetStringField(TEXT("file"), FilePath);
	Summary->SetStringField(TEXT("file_name"), FPaths::GetCleanFilename(FilePath));

	TSharedPtr<FJsonObject> Config = LoadJsonFileObject(FilePath);
	if (Config.IsValid())
	{
		FString Id;
		FString DisplayName;
		FString Description;
		Config->TryGetStringField(TEXT("id"), Id);
		Config->TryGetStringField(TEXT("display_name"), DisplayName);
		Config->TryGetStringField(TEXT("description"), Description);
		Summary->SetStringField(TEXT("id"), Id);
		Summary->SetStringField(TEXT("display_name"), DisplayName);
		Summary->SetStringField(TEXT("description"), Description);
	}
	return Summary;
}

static FString JsonObjectFieldToString(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
{
	const TSharedPtr<FJsonObject>* Child = nullptr;
	if (Object.IsValid() && Object->TryGetObjectField(FieldName, Child) && Child && Child->IsValid())
	{
		return ToJsonString(*Child);
	}
	return FString();
}

static TSharedPtr<FJsonObject> Vector4ForMerge(const TSharedPtr<FJsonObject>& Source)
{
	TSharedPtr<FJsonObject> Vector = MakeShared<FJsonObject>();
	if (!Source.IsValid())
	{
		return Vector;
	}

	double X = 0.0;
	double Y = 0.0;
	double Z = 0.0;
	double W = 0.0;
	if (!Source->TryGetNumberField(TEXT("X"), X))
	{
		Source->TryGetNumberField(TEXT("AnimIndex"), X);
	}
	if (!Source->TryGetNumberField(TEXT("Y"), Y))
	{
		Source->TryGetNumberField(TEXT("PlayLength"), Y);
	}
	if (!Source->TryGetNumberField(TEXT("Z"), Z))
	{
		Source->TryGetNumberField(TEXT("StartFrame"), Z);
	}
	if (!Source->TryGetNumberField(TEXT("W"), W))
	{
		Source->TryGetNumberField(TEXT("EndFrame"), W);
	}

	Vector->SetNumberField(TEXT("X"), X);
	Vector->SetNumberField(TEXT("Y"), Y);
	Vector->SetNumberField(TEXT("Z"), Z);
	Vector->SetNumberField(TEXT("W"), W);
	return Vector;
}

static TSharedPtr<FJsonObject> ConvertAnimsDataForMerge(const TSharedPtr<FJsonObject>& AnimsData)
{
	TSharedPtr<FJsonObject> Converted = MakeShared<FJsonObject>();
	if (!AnimsData.IsValid())
	{
		return Converted;
	}

	for (const TPair<FString, TSharedPtr<FJsonValue>>& CategoryPair : AnimsData->Values)
	{
		if (!CategoryPair.Value.IsValid() || CategoryPair.Value->Type != EJson::Object)
		{
			continue;
		}

		TSharedPtr<FJsonObject> Category = MakeShared<FJsonObject>();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& AnimPair : CategoryPair.Value->AsObject()->Values)
		{
			if (AnimPair.Value.IsValid() && AnimPair.Value->Type == EJson::Object)
			{
				Category->SetObjectField(AnimPair.Key, Vector4ForMerge(AnimPair.Value->AsObject()));
			}
		}
		Converted->SetObjectField(CategoryPair.Key, Category);
	}
	return Converted;
}

static void MergeJsonObjects(TSharedPtr<FJsonObject> Target, const TSharedPtr<FJsonObject>& Source)
{
	if (!Target.IsValid() || !Source.IsValid())
	{
		return;
	}

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Source->Values)
	{
		TSharedPtr<FJsonValue> Existing = Target->TryGetField(Pair.Key);
		if (Existing.IsValid() && Existing->Type == EJson::Object && Pair.Value.IsValid() && Pair.Value->Type == EJson::Object)
		{
			MergeJsonObjects(Existing->AsObject(), Pair.Value->AsObject());
		}
		else
		{
			Target->SetField(Pair.Key, Pair.Value);
		}
	}
}

static TSharedPtr<FJsonObject> BuildAnimMergePatch(const TSharedPtr<FJsonObject>& Spec, const TSharedPtr<FJsonObject>& AnimsData)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> AnimShared = MakeShared<FJsonObject>();
	AnimShared->SetObjectField(TEXT("AnimData"), ConvertAnimsDataForMerge(AnimsData));
	Data->SetObjectField(TEXT("AnimShared"), AnimShared);
	Root->SetObjectField(TEXT("Data"), Data);

	const TSharedPtr<FJsonObject>* UnitPatch = nullptr;
	if (Spec.IsValid() && Spec->TryGetObjectField(TEXT("unit_patch"), UnitPatch) && UnitPatch && UnitPatch->IsValid())
	{
		MergeJsonObjects(Root, *UnitPatch);
	}

	TSharedPtr<FJsonObject> Options = MakeShared<FJsonObject>();
	Options->SetBoolField(TEXT("expected_before"), true);
	Root->SetObjectField(TEXT("options"), Options);
	return Root;
}

static bool TryGetObjectPathSpec(const TSharedPtr<FJsonObject>& Spec, const TArray<FString>& Keys, FString& OutPath)
{
	for (const FString& Key : Keys)
	{
		if (Spec.IsValid() && Spec->TryGetStringField(Key, OutPath) && !OutPath.IsEmpty())
		{
			return true;
		}
	}
	return false;
}

static FString JsonArrayFieldToString(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
{
	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (Object.IsValid() && Object->TryGetArrayField(FieldName, Array))
	{
		FString Output;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
		FJsonSerializer::Serialize(*Array, Writer);
		return Output;
	}
	return FString();
}

static bool JsonObjectHasAnyArrayItems(const TSharedPtr<FJsonObject>& Object)
{
	if (!Object.IsValid())
	{
		return false;
	}

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
	{
		if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Array && !Pair.Value->AsArray().IsEmpty())
		{
			return true;
		}
	}
	return false;
}

static bool FoundAnimsJsonHasEntries(const FString& FoundAnimsJson)
{
	return JsonObjectHasAnyArrayItems(ParseObject(FoundAnimsJson));
}

static TSharedPtr<FJsonObject> LoadProfileConfig(const FString& ProfileType, const FString& ProfileId)
{
	const FString File = FindProfileFile(ProfileType, ProfileId);
	return File.IsEmpty() ? nullptr : LoadJsonFileObject(File);
}

static TSharedPtr<FJsonObject> GetOrCreateObjectField(TSharedPtr<FJsonObject> Object, const FString& FieldName)
{
	const TSharedPtr<FJsonObject>* Existing = nullptr;
	if (Object.IsValid() && Object->TryGetObjectField(FieldName, Existing) && Existing && Existing->IsValid())
	{
		return *Existing;
	}

	TSharedPtr<FJsonObject> Created = MakeShared<FJsonObject>();
	if (Object.IsValid())
	{
		Object->SetObjectField(FieldName, Created);
	}
	return Created;
}

static FString PackagePathFromObjectPath(const FString& ObjectPath)
{
	FString PackageName = ObjectPath;
	FString Right;
	if (PackageName.Split(TEXT("."), &PackageName, &Right, ESearchCase::CaseSensitive, ESearchDir::FromStart))
	{
		// PackageName now contains the long package name.
	}

	FString PackagePath;
	FString AssetName;
	if (PackageName.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
	{
		return PackagePath;
	}
	return FString();
}

static FString MakeObjectPath(const FString& PackagePath, const FString& AssetName)
{
	if (PackagePath.IsEmpty() || AssetName.IsEmpty())
	{
		return FString();
	}
	return PackagePath / AssetName + TEXT(".") + AssetName;
}

static FString MakeGeneratedClassPath(const FString& PackagePath, const FString& AssetName)
{
	if (PackagePath.IsEmpty() || AssetName.IsEmpty())
	{
		return FString();
	}
	return PackagePath / AssetName + TEXT(".") + AssetName + TEXT("_C");
}

static FString AssetNameFromObjectPath(const FString& ObjectPath)
{
	FString PackagePath;
	FString AssetName;
	if (!ObjectPath.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
	{
		return FString();
	}

	FString ObjectName;
	if (AssetName.Split(TEXT("."), &AssetName, &ObjectName))
	{
		return ObjectName.EndsWith(TEXT("_C")) ? ObjectName.LeftChop(2) : ObjectName;
	}
	return AssetName;
}

static FString EnsureObjectPath(const FString& InPath)
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

static FString EnsureGeneratedClassPath(const FString& InPath)
{
	FString Path = EnsureObjectPath(InPath);
	if (Path.IsEmpty() || Path.EndsWith(TEXT("_C")))
	{
		return Path;
	}
	return Path + TEXT("_C");
}

static FString BlueprintPathFromGeneratedClassPath(const FString& ClassPath)
{
	FString Path = ClassPath;
	if (Path.EndsWith(TEXT("_C")))
	{
		Path.LeftChopInline(2);
	}
	return Path;
}

static bool AssetExists(const FString& ObjectPath)
{
	if (ObjectPath.IsEmpty())
	{
		return false;
	}

	FString AssetPath = EnsureObjectPath(ObjectPath);
	if (AssetPath.EndsWith(TEXT("_C")))
	{
		AssetPath = BlueprintPathFromGeneratedClassPath(AssetPath);
	}

	if (FindObject<UObject>(nullptr, *AssetPath))
	{
		return true;
	}

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	return AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(AssetPath)).IsValid();
}

static bool SaveAssetByPath(const FString& ObjectPath, FString& OutError)
{
	UObject* Object = FSoftObjectPath(EnsureObjectPath(ObjectPath)).TryLoad();
	if (!Object)
	{
		OutError = FString::Printf(TEXT("Failed to load asset for saving: %s"), *ObjectPath);
		return false;
	}

	UPackage* Package = Object->GetOutermost();
	if (!Package)
	{
		OutError = FString::Printf(TEXT("Asset has no package: %s"), *ObjectPath);
		return false;
	}

	Package->MarkPackageDirty();
	const FString PackageFileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (!UPackage::SavePackage(Package, Object, *PackageFileName, SaveArgs))
	{
		OutError = FString::Printf(TEXT("Failed to save package: %s"), *PackageFileName);
		return false;
	}
	return true;
}

static void AddStaticMeshMaterialPaths(const FString& StaticMeshPath, const FString& AllowedPackageRoot, TArray<FString>& OutPaths)
{
	UStaticMesh* Mesh = Cast<UStaticMesh>(FSoftObjectPath(EnsureObjectPath(StaticMeshPath)).TryLoad());
	if (!Mesh)
	{
		return;
	}

	for (const FStaticMaterial& Material : Mesh->GetStaticMaterials())
	{
		if (Material.MaterialInterface)
		{
			const FString MaterialPath = Material.MaterialInterface->GetPathName();
			if (AllowedPackageRoot.IsEmpty() || MaterialPath.StartsWith(AllowedPackageRoot / TEXT("")))
			{
				OutPaths.AddUnique(MaterialPath);
			}
		}
	}
}

static FString StringFieldOrDefault(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, const FString& DefaultValue);

static bool HasMaterialOverrides(const TSharedPtr<FJsonObject>& Spec)
{
	const TArray<TSharedPtr<FJsonValue>>* Overrides = nullptr;
	return Spec.IsValid()
		&& Spec->TryGetArrayField(TEXT("material_overrides"), Overrides)
		&& Overrides
		&& !Overrides->IsEmpty();
}

static FString ApplyStaticMeshMaterialOverrides(const FString& StaticMeshPath, const TSharedPtr<FJsonObject>& Spec)
{
	TSharedPtr<FJsonObject> Root = MakeSuccess();
	Root->SetStringField(TEXT("static_mesh"), StaticMeshPath);
	Root->SetNumberField(TEXT("applied_count"), 0);

	const TArray<TSharedPtr<FJsonValue>>* Overrides = nullptr;
	if (!Spec.IsValid() || !Spec->TryGetArrayField(TEXT("material_overrides"), Overrides) || !Overrides || Overrides->IsEmpty())
	{
		Root->SetBoolField(TEXT("applied"), false);
		return ToJsonString(Root);
	}

	UStaticMesh* Mesh = Cast<UStaticMesh>(FSoftObjectPath(EnsureObjectPath(StaticMeshPath)).TryLoad());
	if (!Mesh)
	{
		return MakeErrorJson(FString::Printf(TEXT("Failed to load StaticMesh for material_overrides: %s"), *StaticMeshPath));
	}

	TArray<TSharedPtr<FJsonValue>> Applied;
	TArray<TSharedPtr<FJsonValue>> Issues;
	int32 NextSequentialSlot = 0;

	auto AddIssueObject = [&Issues](const FString& Code, const FString& Message)
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("code"), Code);
		Issue->SetStringField(TEXT("message"), Message);
		Issues.Add(MakeShared<FJsonValueObject>(Issue));
	};

	auto ApplyToSlot = [&Applied, &AddIssueObject, Mesh](int32 SlotIndex, const FString& MaterialPath) -> bool
	{
		if (SlotIndex < 0 || SlotIndex >= Mesh->GetStaticMaterials().Num())
		{
			AddIssueObject(TEXT("slot_index_out_of_range"), FString::Printf(TEXT("slot_index %d is outside StaticMesh material slot range."), SlotIndex));
			return false;
		}

		if (MaterialPath.IsEmpty())
		{
			AddIssueObject(TEXT("missing_material"), FString::Printf(TEXT("material path is missing for slot_index %d."), SlotIndex));
			return false;
		}

		UMaterialInterface* Material = Cast<UMaterialInterface>(FSoftObjectPath(EnsureObjectPath(MaterialPath)).TryLoad());
		if (!Material)
		{
			AddIssueObject(TEXT("material_not_found"), FString::Printf(TEXT("material does not exist or failed to load: %s"), *MaterialPath));
			return false;
		}

		const FString SlotName = Mesh->GetStaticMaterials()[SlotIndex].MaterialSlotName.ToString();
		Mesh->SetMaterial(SlotIndex, Material);

		TSharedPtr<FJsonObject> AppliedItem = MakeShared<FJsonObject>();
		AppliedItem->SetNumberField(TEXT("slot_index"), SlotIndex);
		AppliedItem->SetStringField(TEXT("slot_name"), SlotName);
		AppliedItem->SetStringField(TEXT("material"), Material->GetPathName());
		Applied.Add(MakeShared<FJsonValueObject>(AppliedItem));
		return true;
	};

	for (const TSharedPtr<FJsonValue>& OverrideValue : *Overrides)
	{
		if (!OverrideValue.IsValid())
		{
			AddIssueObject(TEXT("invalid_override"), TEXT("material_overrides contains an invalid JSON value."));
			continue;
		}

		if (OverrideValue->Type == EJson::String)
		{
			if (ApplyToSlot(NextSequentialSlot, OverrideValue->AsString()))
			{
				++NextSequentialSlot;
			}
			continue;
		}

		if (OverrideValue->Type != EJson::Object)
		{
			AddIssueObject(TEXT("invalid_override_type"), TEXT("material_overrides items must be strings or objects."));
			continue;
		}

		TSharedPtr<FJsonObject> OverrideObject = OverrideValue->AsObject();
		FString MaterialPath = StringFieldOrDefault(OverrideObject, TEXT("material"), FString());
		MaterialPath = StringFieldOrDefault(OverrideObject, TEXT("material_path"), MaterialPath);
		MaterialPath = StringFieldOrDefault(OverrideObject, TEXT("material_interface"), MaterialPath);

		double SlotIndexNumber = 0.0;
		if (OverrideObject->TryGetNumberField(TEXT("slot_index"), SlotIndexNumber))
		{
			if (ApplyToSlot(static_cast<int32>(SlotIndexNumber), MaterialPath))
			{
				NextSequentialSlot = FMath::Max(NextSequentialSlot, static_cast<int32>(SlotIndexNumber) + 1);
			}
			continue;
		}

		FString SlotName;
		const bool bHasExactSlotName = OverrideObject->TryGetStringField(TEXT("slot_name"), SlotName) && !SlotName.IsEmpty();
		FString SlotNameContains;
		const bool bHasSlotNameContains = OverrideObject->TryGetStringField(TEXT("slot_name_contains"), SlotNameContains) && !SlotNameContains.IsEmpty();
		if (bHasExactSlotName || bHasSlotNameContains)
		{
			bool bMatched = false;
			const TArray<FStaticMaterial>& StaticMaterials = Mesh->GetStaticMaterials();
			for (int32 SlotIndex = 0; SlotIndex < StaticMaterials.Num(); ++SlotIndex)
			{
				const FString CurrentSlotName = StaticMaterials[SlotIndex].MaterialSlotName.ToString();
				const bool bExactMatch = bHasExactSlotName && CurrentSlotName.Equals(SlotName, ESearchCase::IgnoreCase);
				const bool bContainsMatch = bHasSlotNameContains && CurrentSlotName.Contains(SlotNameContains, ESearchCase::IgnoreCase);
				if (bExactMatch || bContainsMatch)
				{
					bMatched |= ApplyToSlot(SlotIndex, MaterialPath);
				}
			}

			if (!bMatched)
			{
				AddIssueObject(TEXT("slot_name_not_found"), FString::Printf(TEXT("No material slot matched slot_name='%s' slot_name_contains='%s'."), *SlotName, *SlotNameContains));
			}
			continue;
		}

		if (ApplyToSlot(NextSequentialSlot, MaterialPath))
		{
			++NextSequentialSlot;
		}
	}

	const bool bHadIssues = !Issues.IsEmpty();
	if (!Applied.IsEmpty())
	{
		Mesh->PostEditChange();
		Mesh->MarkPackageDirty();
	}

	Root->SetBoolField(TEXT("success"), !bHadIssues);
	Root->SetBoolField(TEXT("applied"), !Applied.IsEmpty());
	Root->SetNumberField(TEXT("applied_count"), Applied.Num());
	Root->SetArrayField(TEXT("applied_overrides"), Applied);
	Root->SetArrayField(TEXT("issues"), Issues);
	if (bHadIssues)
	{
		Root->SetStringField(TEXT("error"), TEXT("One or more material_overrides could not be applied."));
	}
	return ToJsonString(Root);
}

static FString StringFieldOrDefault(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, const FString& DefaultValue)
{
	FString Value;
	if (Object.IsValid() && Object->TryGetStringField(FieldName, Value) && !Value.IsEmpty())
	{
		return Value;
	}
	return DefaultValue;
}

static TArray<FString> StringArrayField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
{
	TArray<FString> Result;
	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (Object.IsValid() && Object->TryGetArrayField(FieldName, Array))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			const FString Text = Value.IsValid() ? Value->AsString() : FString();
			if (!Text.IsEmpty())
			{
				Result.Add(Text);
			}
		}
	}
	return Result;
}

static bool TryGetNumberByNames(const TSharedPtr<FJsonObject>& Object, const TArray<FString>& FieldNames, double& OutValue)
{
	if (!Object.IsValid())
	{
		return false;
	}

	for (const FString& FieldName : FieldNames)
	{
		if (Object->TryGetNumberField(FieldName, OutValue))
		{
			return true;
		}
	}
	return false;
}

static float FloatFieldByNamesOrDefault(const TSharedPtr<FJsonObject>& Object, const TArray<FString>& FieldNames, float DefaultValue)
{
	double Value = DefaultValue;
	return TryGetNumberByNames(Object, FieldNames, Value) ? static_cast<float>(Value) : DefaultValue;
}

static int32 IntFieldByNamesOrDefault(const TSharedPtr<FJsonObject>& Object, const TArray<FString>& FieldNames, int32 DefaultValue)
{
	double Value = DefaultValue;
	return TryGetNumberByNames(Object, FieldNames, Value) ? static_cast<int32>(Value) : DefaultValue;
}

static bool BoolFieldByNamesOrDefault(const TSharedPtr<FJsonObject>& Object, const TArray<FString>& FieldNames, bool bDefaultValue)
{
	if (!Object.IsValid())
	{
		return bDefaultValue;
	}

	bool bValue = bDefaultValue;
	for (const FString& FieldName : FieldNames)
	{
		if (Object->TryGetBoolField(FieldName, bValue))
		{
			return bValue;
		}
	}
	return bDefaultValue;
}

static void AppendAnimPath(UAnimSequence* AnimSequence, TArray<TObjectPtr<UAnimSequence>>& OutArray)
{
	if (AnimSequence)
	{
		OutArray.AddUnique(AnimSequence);
	}
}

static void AppendAnimArrayField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, TArray<TObjectPtr<UAnimSequence>>& OutArray)
{
	if (!Object.IsValid())
	{
		return;
	}

	FString SinglePath;
	if (Object->TryGetStringField(FieldName, SinglePath) && !SinglePath.IsEmpty())
	{
		AppendAnimPath(Cast<UAnimSequence>(FSoftObjectPath(EnsureObjectPath(SinglePath)).TryLoad()), OutArray);
		return;
	}

	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (!Object->TryGetArrayField(FieldName, Array) || !Array)
	{
		return;
	}

	for (const TSharedPtr<FJsonValue>& Value : *Array)
	{
		FString Path;
		if (Value.IsValid() && Value->TryGetString(Path) && !Path.IsEmpty())
		{
			AppendAnimPath(Cast<UAnimSequence>(FSoftObjectPath(EnsureObjectPath(Path)).TryLoad()), OutArray);
		}
	}
}

static void AppendAnimCategoryFields(const TSharedPtr<FJsonObject>& Object, const TArray<FString>& FieldNames, TArray<TObjectPtr<UAnimSequence>>& OutArray)
{
	for (const FString& FieldName : FieldNames)
	{
		AppendAnimArrayField(Object, FieldName, OutArray);
	}
}

static void AppendFoundAnimsFromObject(const TSharedPtr<FJsonObject>& Object, FFoundAnimSequences& OutAnims)
{
	AppendAnimCategoryFields(Object, { TEXT("Idle"), TEXT("idle"), TEXT("idle_anims"), TEXT("idle_animations") }, OutAnims.Idle);
	AppendAnimCategoryFields(Object, { TEXT("Move"), TEXT("move"), TEXT("move_anims"), TEXT("move_animations") }, OutAnims.Move);
	AppendAnimCategoryFields(Object, { TEXT("Fall"), TEXT("fall"), TEXT("fall_anims"), TEXT("fall_animations") }, OutAnims.Fall);
	AppendAnimCategoryFields(Object, { TEXT("Appear"), TEXT("appear"), TEXT("appear_anims"), TEXT("appear_animations"), TEXT("raise"), TEXT("raise_anims") }, OutAnims.Appear);
	AppendAnimCategoryFields(Object, { TEXT("Attack"), TEXT("attack"), TEXT("attack_anims"), TEXT("attack_animations") }, OutAnims.Attack);
	AppendAnimCategoryFields(Object, { TEXT("Hit"), TEXT("hit"), TEXT("hit_anims"), TEXT("hit_animations"), TEXT("hurt"), TEXT("hurt_anims") }, OutAnims.Hit);
	AppendAnimCategoryFields(Object, { TEXT("Death"), TEXT("death"), TEXT("death_anims"), TEXT("death_animations"), TEXT("lower"), TEXT("lower_anims") }, OutAnims.Death);
	AppendAnimCategoryFields(Object, { TEXT("Other"), TEXT("other"), TEXT("other_anims"), TEXT("other_animations") }, OutAnims.Other);
}

static FFoundAnimSequences BuildFoundAnimsForBake(const TSharedPtr<FJsonObject>& Discovery, const TSharedPtr<FJsonObject>& Spec)
{
	FFoundAnimSequences FoundAnims;

	const TSharedPtr<FJsonObject>* AnimDiscovery = nullptr;
	if (Discovery.IsValid() && Discovery->TryGetObjectField(TEXT("animations"), AnimDiscovery) && AnimDiscovery && AnimDiscovery->IsValid())
	{
		const TSharedPtr<FJsonObject>* AutoAnims = nullptr;
		if ((*AnimDiscovery)->TryGetObjectField(TEXT("anims"), AutoAnims) && AutoAnims && AutoAnims->IsValid())
		{
			AppendFoundAnimsFromObject(*AutoAnims, FoundAnims);
		}
	}

	AppendFoundAnimsFromObject(Spec, FoundAnims);

	for (const FString& FieldName : { TEXT("animations"), TEXT("found_anims"), TEXT("found_animations"), TEXT("anim_sequences") })
	{
		const TSharedPtr<FJsonObject>* AnimOverride = nullptr;
		if (Spec.IsValid() && Spec->TryGetObjectField(FieldName, AnimOverride) && AnimOverride && AnimOverride->IsValid())
		{
			AppendFoundAnimsFromObject(*AnimOverride, FoundAnims);
		}
	}

	return FoundAnims;
}

static FString SerializeFoundAnimsLocal(const FFoundAnimSequences& Anims)
{
	auto SerializeAnimArray = [](const TArray<TObjectPtr<UAnimSequence>>& AnimArray) -> TArray<TSharedPtr<FJsonValue>>
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const TObjectPtr<UAnimSequence>& Anim : AnimArray)
		{
			if (Anim)
			{
				Values.Add(MakeShared<FJsonValueString>(Anim->GetPathName()));
			}
		}
		return Values;
	};

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("Idle"), SerializeAnimArray(Anims.Idle));
	Root->SetArrayField(TEXT("Move"), SerializeAnimArray(Anims.Move));
	Root->SetArrayField(TEXT("Fall"), SerializeAnimArray(Anims.Fall));
	Root->SetArrayField(TEXT("Appear"), SerializeAnimArray(Anims.Appear));
	Root->SetArrayField(TEXT("Attack"), SerializeAnimArray(Anims.Attack));
	Root->SetArrayField(TEXT("Hit"), SerializeAnimArray(Anims.Hit));
	Root->SetArrayField(TEXT("Death"), SerializeAnimArray(Anims.Death));
	Root->SetArrayField(TEXT("Other"), SerializeAnimArray(Anims.Other));
	return ToJsonString(Root);
}

static void AppendAllFoundAnimSequences(const FFoundAnimSequences& FoundAnims, TArray<UAnimSequence*>& OutSequences)
{
	auto AppendArray = [&OutSequences](const TArray<TObjectPtr<UAnimSequence>>& Source)
	{
		for (const TObjectPtr<UAnimSequence>& Anim : Source)
		{
			if (Anim)
			{
				OutSequences.AddUnique(Anim.Get());
			}
		}
	};

	AppendArray(FoundAnims.Idle);
	AppendArray(FoundAnims.Move);
	AppendArray(FoundAnims.Fall);
	AppendArray(FoundAnims.Appear);
	AppendArray(FoundAnims.Attack);
	AppendArray(FoundAnims.Hit);
	AppendArray(FoundAnims.Death);
	AppendArray(FoundAnims.Other);
}

static bool FoundAnimsHasEntries(const FFoundAnimSequences& FoundAnims)
{
	return !FoundAnims.Idle.IsEmpty()
		|| !FoundAnims.Move.IsEmpty()
		|| !FoundAnims.Fall.IsEmpty()
		|| !FoundAnims.Appear.IsEmpty()
		|| !FoundAnims.Attack.IsEmpty()
		|| !FoundAnims.Hit.IsEmpty()
		|| !FoundAnims.Death.IsEmpty()
		|| !FoundAnims.Other.IsEmpty();
}

static EVATBakeMode ParseVatBakeMode(const TSharedPtr<FJsonObject>& Object, EVATBakeMode DefaultMode)
{
	if (!Object.IsValid())
	{
		return DefaultMode;
	}

	FString ModeText;
	if (Object->TryGetStringField(TEXT("Mode"), ModeText) || Object->TryGetStringField(TEXT("mode"), ModeText))
	{
		if (ModeText.Contains(TEXT("Bone"), ESearchCase::IgnoreCase))
		{
			return EVATBakeMode::BoneMode;
		}
		if (ModeText.Contains(TEXT("Vertex"), ESearchCase::IgnoreCase))
		{
			return EVATBakeMode::VertexMode;
		}
	}

	double ModeNumber = static_cast<double>(static_cast<uint8>(DefaultMode));
	if (Object->TryGetNumberField(TEXT("Mode"), ModeNumber) || Object->TryGetNumberField(TEXT("mode"), ModeNumber))
	{
		return static_cast<EVATBakeMode>(static_cast<uint8>(ModeNumber));
	}
	return DefaultMode;
}

static TArray<FLODDataEd> ParseLODSettingsForBake(const TSharedPtr<FJsonObject>& Spec, const TSharedPtr<FJsonObject>& Discovery)
{
	const TArray<TSharedPtr<FJsonValue>>* LodArray = nullptr;
	if (!Spec.IsValid() || !Spec->TryGetArrayField(TEXT("lod_settings"), LodArray))
	{
		const TSharedPtr<FJsonObject>* LodDiscovery = nullptr;
		if (Discovery.IsValid() && Discovery->TryGetObjectField(TEXT("lod_settings"), LodDiscovery) && LodDiscovery && LodDiscovery->IsValid())
		{
			(*LodDiscovery)->TryGetArrayField(TEXT("lod_settings"), LodArray);
		}
	}

	TArray<FLODDataEd> Settings;
	if (LodArray)
	{
		for (const TSharedPtr<FJsonValue>& Value : *LodArray)
		{
			if (!Value.IsValid() || Value->Type != EJson::Object)
			{
				continue;
			}

			TSharedPtr<FJsonObject> Item = Value->AsObject();
			FLODDataEd LOD;
			double Number = 0.0;
			if (Item->TryGetNumberField(TEXT("ScreenSize"), Number) || Item->TryGetNumberField(TEXT("screen_size"), Number))
			{
				LOD.ScreenSize = static_cast<float>(Number);
			}
			if (Item->TryGetNumberField(TEXT("LODIndex"), Number) || Item->TryGetNumberField(TEXT("lod_index"), Number))
			{
				LOD.LODIndex = static_cast<int32>(Number);
			}
			if (Item->TryGetNumberField(TEXT("AnimBlendLevel"), Number) || Item->TryGetNumberField(TEXT("anim_blend_level"), Number))
			{
				LOD.AnimBlendLevel = FMath::Clamp(static_cast<int32>(Number), 1, 3);
			}
			LOD.Mode = ParseVatBakeMode(Item, LOD.Mode);
			Settings.Add(LOD);
		}
	}

	if (Settings.IsEmpty())
	{
		FLODDataEd DefaultLOD;
		DefaultLOD.LODIndex = 0;
		DefaultLOD.ScreenSize = 0.0f;
		DefaultLOD.AnimBlendLevel = 2;
		DefaultLOD.Mode = EVATBakeMode::VertexMode;
		Settings.Add(DefaultLOD);
	}
	return Settings;
}

static int32 GetStaticMeshLODCount(UStaticMesh* StaticMesh)
{
	if (!StaticMesh)
	{
		return 0;
	}

	int32 LODCount = StaticMesh->GetNumSourceModels();
	if (LODCount <= 0 && StaticMesh->GetRenderData())
	{
		LODCount = StaticMesh->GetRenderData()->LODResources.Num();
	}
	return FMath::Max(0, LODCount);
}

static TSharedPtr<FJsonObject> BuildRenderLodObjectFromSettings(const TArray<FLODDataEd>& LODSettings)
{
	TSharedPtr<FJsonObject> RenderLOD = MakeShared<FJsonObject>();
	for (int32 Index = 0; Index < LODSettings.Num(); ++Index)
	{
		const FLODDataEd& LOD = LODSettings[Index];
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("LODIndex"), LOD.LODIndex);
		Data->SetNumberField(TEXT("ScreenSize"), LOD.ScreenSize);
		Data->SetNumberField(TEXT("AnimBlendLevel"), LOD.AnimBlendLevel);
		RenderLOD->SetObjectField(FString::Printf(TEXT("Data%d"), Index), Data);
	}
	return RenderLOD;
}

static void SetUnitPatchRenderLodFromSettings(TSharedPtr<FJsonObject> UnitPatch, const TArray<FLODDataEd>& LODSettings)
{
	if (!UnitPatch.IsValid() || LODSettings.IsEmpty())
	{
		return;
	}

	TSharedPtr<FJsonObject> Data = GetOrCreateObjectField(UnitPatch, TEXT("Data"));
	TSharedPtr<FJsonObject> LodShared = GetOrCreateObjectField(Data, TEXT("LODShared"));
	LodShared->SetObjectField(TEXT("RenderLOD"), BuildRenderLodObjectFromSettings(LODSettings));
}

static UTexture2D* CreateOrLoadTexture2DAsset(const FString& PackagePath, const FString& AssetName, TArray<FString>& OutSavePaths)
{
	const FString ObjectPath = MakeObjectPath(PackagePath, AssetName);
	if (UTexture2D* Existing = Cast<UTexture2D>(FSoftObjectPath(EnsureObjectPath(ObjectPath)).TryLoad()))
	{
		OutSavePaths.AddUnique(Existing->GetPathName());
		return Existing;
	}

	const FString PackageName = PackagePath / AssetName;
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return nullptr;
	}
	Package->FullyLoad();

	UTexture2D* Texture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
	if (!Texture)
	{
		return nullptr;
	}

	Texture->SRGB = false;
	Texture->CompressionSettings = TextureCompressionSettings::TC_HDR;
	Texture->Filter = TextureFilter::TF_Nearest;
	Texture->MipGenSettings = TextureMipGenSettings::TMGS_NoMipmaps;
	Texture->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(Texture);
	OutSavePaths.AddUnique(Texture->GetPathName());
	return Texture;
}

static UAnimToTextureDataAsset* CreateOrLoadAnimToTextureDataAsset(const FString& PackagePath, const FString& AssetName, TArray<FString>& OutSavePaths)
{
	const FString ObjectPath = MakeObjectPath(PackagePath, AssetName);
	if (UAnimToTextureDataAsset* Existing = Cast<UAnimToTextureDataAsset>(FSoftObjectPath(EnsureObjectPath(ObjectPath)).TryLoad()))
	{
		OutSavePaths.AddUnique(Existing->GetPathName());
		return Existing;
	}

	const FString PackageName = PackagePath / AssetName;
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return nullptr;
	}
	Package->FullyLoad();

	UAnimToTextureDataAsset* DataAsset = NewObject<UAnimToTextureDataAsset>(Package, *AssetName, RF_Public | RF_Standalone);
	if (!DataAsset)
	{
		return nullptr;
	}

	DataAsset->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(DataAsset);
	OutSavePaths.AddUnique(DataAsset->GetPathName());
	return DataAsset;
}

static void SetMaterialStaticSwitch(UMaterialInstanceConstant* MaterialInstance, const FName ParameterName, bool bValue)
{
	UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, ParameterName, bValue);
	UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, ParameterName, bValue, EMaterialParameterAssociation::LayerParameter);
}

static void SetMaterialTextureParameter(UMaterialInstanceConstant* MaterialInstance, const FName ParameterName, UTexture2D* Texture)
{
	if (!Texture)
	{
		return;
	}

	UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MaterialInstance, ParameterName, Texture);
	UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MaterialInstance, ParameterName, Texture, EMaterialParameterAssociation::LayerParameter);
}

static int32 UpdateVatMaterialsForLOD(UStaticMesh* StaticMesh, UAnimToTextureDataAsset* DataAsset, UTexture2D* AnimDataTexture, const FLODDataEd& LOD)
{
	if (!StaticMesh || !DataAsset)
	{
		return 0;
	}

	int32 UpdatedCount = 0;
	const FString LodToken = FString::Printf(TEXT("LOD%d"), LOD.LODIndex);
	for (const FStaticMaterial& StaticMaterial : StaticMesh->GetStaticMaterials())
	{
		if (!StaticMaterial.MaterialInterface)
		{
			continue;
		}

		const FString SlotName = StaticMaterial.MaterialSlotName.ToString();
		if (!SlotName.Contains(LodToken, ESearchCase::IgnoreCase))
		{
			continue;
		}

		UMaterialInstanceConstant* MaterialInstance = Cast<UMaterialInstanceConstant>(StaticMaterial.MaterialInterface);
		if (!MaterialInstance)
		{
			continue;
		}

		UAnimToTextureBPLibrary::UpdateMaterialInstanceFromDataAsset(DataAsset, MaterialInstance);
		UAnimToTextureBPLibrary::UpdateMaterialInstanceFromDataAsset(DataAsset, MaterialInstance, EMaterialParameterAssociation::LayerParameter);
		if (AnimDataTexture)
		{
			SetMaterialTextureParameter(MaterialInstance, TEXT("AnimDataTex"), AnimDataTexture);
		}
		if (LOD.Mode == EVATBakeMode::BoneMode)
		{
			SetMaterialTextureParameter(MaterialInstance, TEXT("BonePositionTexture"), DataAsset->BonePositionTexture.LoadSynchronous());
			SetMaterialTextureParameter(MaterialInstance, TEXT("BoneRotationTexture"), DataAsset->BoneRotationTexture.LoadSynchronous());
			SetMaterialTextureParameter(MaterialInstance, TEXT("BoneWeightsTexture"), DataAsset->BoneWeightTexture.LoadSynchronous());
		}
		else
		{
			SetMaterialTextureParameter(MaterialInstance, TEXT("PositionTexture"), DataAsset->VertexPositionTexture.LoadSynchronous());
			SetMaterialTextureParameter(MaterialInstance, TEXT("NormalTexture"), DataAsset->VertexNormalTexture.LoadSynchronous());
		}
		SetMaterialStaticSwitch(MaterialInstance, TEXT("BoneMode"), LOD.Mode == EVATBakeMode::BoneMode);
		SetMaterialStaticSwitch(MaterialInstance, TEXT("UseVAT"), true);
		SetMaterialStaticSwitch(MaterialInstance, TEXT("UseTwoInfluences"), true);
		SetMaterialStaticSwitch(MaterialInstance, TEXT("UseFourInfluences"), false);
		SetMaterialStaticSwitch(MaterialInstance, TEXT("UseBlend2"), LOD.AnimBlendLevel == 2 || LOD.AnimBlendLevel == 3);
		SetMaterialStaticSwitch(MaterialInstance, TEXT("UseBlend3"), LOD.AnimBlendLevel == 3);
		SetMaterialStaticSwitch(MaterialInstance, TEXT("DebugMode"), false);
		SetMaterialStaticSwitch(MaterialInstance, TEXT("LegacyAnimData"), false);
		UMaterialEditingLibrary::UpdateMaterialInstance(MaterialInstance);
		MaterialInstance->MarkPackageDirty();
		++UpdatedCount;
	}
	return UpdatedCount;
}

static TSharedPtr<FJsonObject> BakeVatWithMassBattleToolsFlow(
	const TSharedPtr<FJsonObject>& Spec,
	const TSharedPtr<FJsonObject>& Discovery,
	const FString& SkeletalMeshPath,
	const FString& StaticMeshPath,
	const FString& GeneratedPackagePath,
	const FString& AssetSlug,
	const FString& VatDataAssetName,
	TSharedPtr<FJsonObject> UnitPatch,
	TArray<FString>& OutSavePaths)
{
	TSharedPtr<FJsonObject> Root = MakeSuccess();

	USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(FSoftObjectPath(EnsureObjectPath(SkeletalMeshPath)).TryLoad());
	UStaticMesh* StaticMesh = Cast<UStaticMesh>(FSoftObjectPath(EnsureObjectPath(StaticMeshPath)).TryLoad());
	if (!SkeletalMesh || !StaticMesh)
	{
		Root->SetBoolField(TEXT("success"), false);
		Root->SetStringField(TEXT("error"), TEXT("Failed to load SkeletalMesh or StaticMesh for VAT bake."));
		return Root;
	}

	const float SampleRate = FloatFieldByNamesOrDefault(Spec, { TEXT("vat_sample_rate"), TEXT("sample_rate"), TEXT("VATSampleRate") }, 24.0f);
	const int32 UVChannel = FMath::Clamp(IntFieldByNamesOrDefault(Spec, { TEXT("vat_uv_channel"), TEXT("uv_channel"), TEXT("VATUVChannel") }, 1), 0, 3);
	const bool bEnforcePowerOfTwo = BoolFieldByNamesOrDefault(Spec, { TEXT("enforce_power_of_two"), TEXT("EnforcePowerOfTwo") }, false);
	const int32 MaxHeight = FMath::Max(1, IntFieldByNamesOrDefault(Spec, { TEXT("vat_max_height"), TEXT("max_height") }, 8192));
	const int32 MaxWidth = FMath::Max(1, IntFieldByNamesOrDefault(Spec, { TEXT("vat_max_width"), TEXT("max_width") }, 8192));

	UAnimToTextureDataAsset* DataAsset = CreateOrLoadAnimToTextureDataAsset(GeneratedPackagePath, VatDataAssetName, OutSavePaths);
	if (!DataAsset)
	{
		Root->SetBoolField(TEXT("success"), false);
		Root->SetStringField(TEXT("error"), TEXT("Failed to create or load AnimToTextureDataAsset."));
		return Root;
	}

	const FFoundAnimSequences FoundAnims = BuildFoundAnimsForBake(Discovery, Spec);
	TArray<UAnimSequence*> BakeSequences;
	AppendAllFoundAnimSequences(FoundAnims, BakeSequences);
	if (BakeSequences.IsEmpty())
	{
		Root->SetBoolField(TEXT("success"), false);
		Root->SetStringField(TEXT("error"), TEXT("No animation sequences were provided or discovered for VAT bake."));
		return Root;
	}

	DataAsset->Modify();
	DataAsset->SkeletalMesh = SkeletalMesh;
	DataAsset->StaticMesh = StaticMesh;
	DataAsset->UVChannel = UVChannel;
	DataAsset->bEnforcePowerOfTwo = bEnforcePowerOfTwo;
	DataAsset->Precision = EAnimToTexturePrecision::SixteenBits;
	DataAsset->MaxHeight = MaxHeight;
	DataAsset->MaxWidth = MaxWidth;
	DataAsset->SampleRate = SampleRate;
	DataAsset->AnimSequences.Reset();
	for (UAnimSequence* AnimSequence : BakeSequences)
	{
		FAnimToTextureAnimSequenceInfo Info;
		Info.bEnabled = true;
		Info.AnimSequence = AnimSequence;
		DataAsset->AnimSequences.Add(Info);
	}

	const TArray<FLODDataEd> RequestedLODSettings = ParseLODSettingsForBake(Spec, Discovery);
	const int32 StaticMeshLODCount = GetStaticMeshLODCount(StaticMesh);
	TArray<TSharedPtr<FJsonValue>> LodResults;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	auto AddBakeWarning = [](TArray<TSharedPtr<FJsonValue>>& TargetWarnings, const FString& Code, const FString& Message, const FString& Field)
	{
		TSharedPtr<FJsonObject> Warning = MakeShared<FJsonObject>();
		Warning->SetStringField(TEXT("severity"), TEXT("warning"));
		Warning->SetStringField(TEXT("code"), Code);
		Warning->SetStringField(TEXT("message"), Message);
		if (!Field.IsEmpty())
		{
			Warning->SetStringField(TEXT("field"), Field);
		}
		TargetWarnings.Add(MakeShared<FJsonValueObject>(Warning));
	};
	TArray<FLODDataEd> LODSettings;
	for (const FLODDataEd& LOD : RequestedLODSettings)
	{
		if (LOD.LODIndex < 0 || LOD.LODIndex >= StaticMeshLODCount)
		{
			TSharedPtr<FJsonObject> Skipped = MakeShared<FJsonObject>();
			Skipped->SetNumberField(TEXT("lod_index"), LOD.LODIndex);
			Skipped->SetStringField(TEXT("status"), TEXT("skipped_invalid_static_lod"));
			Skipped->SetStringField(TEXT("reason"), FString::Printf(TEXT("Generated StaticMesh has %d LOD(s); LOD %d cannot be baked."), StaticMeshLODCount, LOD.LODIndex));
			LodResults.Add(MakeShared<FJsonValueObject>(Skipped));
			AddBakeWarning(Warnings, TEXT("skipped_invalid_static_lod"), FString::Printf(TEXT("Skipped VAT bake for LOD %d because the generated StaticMesh has %d LOD(s)."), LOD.LODIndex, StaticMeshLODCount), TEXT("lod_settings"));
			continue;
		}
		LODSettings.Add(LOD);
	}

	if (LODSettings.IsEmpty() && StaticMeshLODCount > 0)
	{
		FLODDataEd FallbackLOD;
		if (!RequestedLODSettings.IsEmpty())
		{
			FallbackLOD = RequestedLODSettings[0];
		}
		FallbackLOD.LODIndex = 0;
		LODSettings.Add(FallbackLOD);
		AddBakeWarning(Warnings, TEXT("defaulted_bake_lod0"), TEXT("No requested LOD matched the generated StaticMesh; VAT bake fell back to LOD 0."), TEXT("lod_settings"));
	}

	if (LODSettings.IsEmpty())
	{
		Root->SetBoolField(TEXT("success"), false);
		Root->SetStringField(TEXT("error"), TEXT("Generated StaticMesh has no LODs available for VAT bake."));
		Root->SetNumberField(TEXT("static_lod_count"), StaticMeshLODCount);
		Root->SetArrayField(TEXT("lod_results"), LodResults);
		Root->SetArrayField(TEXT("warnings"), Warnings);
		return Root;
	}

	SetUnitPatchRenderLodFromSettings(UnitPatch, LODSettings);

	TArray<FAnimToTextureAnimInfo> AnimDataForTexture;
	UTexture2D* AnimDataTexture = nullptr;
	int32 TotalMaterialUpdateCount = 0;

	for (const FLODDataEd& LOD : LODSettings)
	{
		DataAsset->SkeletalLODIndex = LOD.LODIndex;
		DataAsset->StaticLODIndex = LOD.LODIndex;
		DataAsset->Mode = (LOD.Mode == EVATBakeMode::BoneMode) ? EAnimToTextureMode::Bone : EAnimToTextureMode::Vertex;

		const FString LodSuffix = FString::Printf(TEXT("_LOD%d"), LOD.LODIndex);
		if (LOD.Mode == EVATBakeMode::BoneMode)
		{
			DataAsset->BonePositionTexture = CreateOrLoadTexture2DAsset(GeneratedPackagePath, FString::Printf(TEXT("VAT_%s_BonePosition%s"), *AssetSlug, *LodSuffix), OutSavePaths);
			DataAsset->BoneRotationTexture = CreateOrLoadTexture2DAsset(GeneratedPackagePath, FString::Printf(TEXT("VAT_%s_BoneRotation%s"), *AssetSlug, *LodSuffix), OutSavePaths);
			DataAsset->BoneWeightTexture = CreateOrLoadTexture2DAsset(GeneratedPackagePath, FString::Printf(TEXT("VAT_%s_BoneWeights%s"), *AssetSlug, *LodSuffix), OutSavePaths);
		}
		else
		{
			DataAsset->VertexPositionTexture = CreateOrLoadTexture2DAsset(GeneratedPackagePath, FString::Printf(TEXT("VAT_%s_VertPosition%s"), *AssetSlug, *LodSuffix), OutSavePaths);
			DataAsset->VertexNormalTexture = CreateOrLoadTexture2DAsset(GeneratedPackagePath, FString::Printf(TEXT("VAT_%s_VertNormal%s"), *AssetSlug, *LodSuffix), OutSavePaths);
		}

		const bool bBakeSuccess = UAnimToTextureBPLibrary::AnimationToTexture(DataAsset);
		TSharedPtr<FJsonObject> LodResult = MakeShared<FJsonObject>();
		LodResult->SetNumberField(TEXT("lod_index"), LOD.LODIndex);
		LodResult->SetStringField(TEXT("mode"), LOD.Mode == EVATBakeMode::BoneMode ? TEXT("BoneMode") : TEXT("VertexMode"));
		LodResult->SetBoolField(TEXT("bake_success"), bBakeSuccess);
		if (!bBakeSuccess)
		{
			Root->SetBoolField(TEXT("success"), false);
			Root->SetStringField(TEXT("error"), FString::Printf(TEXT("AnimationToTexture failed for LOD %d."), LOD.LODIndex));
			LodResults.Add(MakeShared<FJsonValueObject>(LodResult));
			Root->SetArrayField(TEXT("lod_results"), LodResults);
			Root->SetArrayField(TEXT("warnings"), Warnings);
			return Root;
		}

		if (!AnimDataTexture)
		{
			AnimDataForTexture = DataAsset->Animations;
			AnimDataTexture = UMassBattleFuncLibEd::CreateAnimDataTexture(AnimDataForTexture, GeneratedPackagePath, TEXT("AnimDataTex_") + AssetSlug);
			if (AnimDataTexture)
			{
				OutSavePaths.AddUnique(AnimDataTexture->GetPathName());
			}
		}

		const int32 MaterialUpdateCount = UpdateVatMaterialsForLOD(StaticMesh, DataAsset, AnimDataTexture, LOD);
		TotalMaterialUpdateCount += MaterialUpdateCount;
		LodResult->SetNumberField(TEXT("material_update_count"), MaterialUpdateCount);
		LodResults.Add(MakeShared<FJsonValueObject>(LodResult));
	}

	DataAsset->MarkPackageDirty();
	StaticMesh->MarkPackageDirty();

	const FString FoundAnimsJson = SerializeFoundAnimsLocal(FoundAnims);
	const FString AnimsDataResult = UMassBattleEditorMCPApi::MCP_CreateAnimsDataFromSequences(DataAsset->GetPathName(), FoundAnimsJson);
	TSharedPtr<FJsonObject> AnimsDataJson = ParseObject(AnimsDataResult);
	if (!AnimsDataJson.IsValid() || !AnimsDataJson->GetBoolField(TEXT("success")))
	{
		Root->SetBoolField(TEXT("success"), false);
		Root->SetStringField(TEXT("error"), TEXT("VAT bake succeeded but CreateAnimsDataFromSequences failed."));
		Root->SetStringField(TEXT("anims_data_result"), AnimsDataResult);
		Root->SetArrayField(TEXT("lod_results"), LodResults);
		Root->SetArrayField(TEXT("warnings"), Warnings);
		return Root;
	}

	const TSharedPtr<FJsonObject>* AnimsData = nullptr;
	if (AnimsDataJson->TryGetObjectField(TEXT("anims_data"), AnimsData) && AnimsData && AnimsData->IsValid())
	{
		TSharedPtr<FJsonObject> Data = GetOrCreateObjectField(UnitPatch, TEXT("Data"));
		TSharedPtr<FJsonObject> AnimShared = GetOrCreateObjectField(Data, TEXT("AnimShared"));
		AnimShared->SetObjectField(TEXT("AnimData"), ConvertAnimsDataForMerge(*AnimsData));
	}

	Root->SetStringField(TEXT("data_asset_path"), DataAsset->GetPathName());
	Root->SetStringField(TEXT("anim_data_texture_path"), AnimDataTexture ? AnimDataTexture->GetPathName() : FString());
	Root->SetNumberField(TEXT("sample_rate"), SampleRate);
	Root->SetNumberField(TEXT("uv_channel"), UVChannel);
	Root->SetNumberField(TEXT("animation_count"), BakeSequences.Num());
	Root->SetNumberField(TEXT("static_lod_count"), StaticMeshLODCount);
	Root->SetNumberField(TEXT("requested_lod_count"), RequestedLODSettings.Num());
	Root->SetNumberField(TEXT("baked_lod_count"), LODSettings.Num());
	Root->SetNumberField(TEXT("material_update_count"), TotalMaterialUpdateCount);
	Root->SetArrayField(TEXT("lod_results"), LodResults);
	Root->SetArrayField(TEXT("warnings"), Warnings);
	Root->SetObjectField(TEXT("anims_data"), AnimsDataJson);
	return Root;
}

static void ApplyGeneratedUnitRuntimeAnimationDefaults(const FString& UnitPath, int32 SampleRate)
{
	UMassBattleAgentConfigDataAsset* Unit = Cast<UMassBattleAgentConfigDataAsset>(FSoftObjectPath(EnsureObjectPath(UnitPath)).TryLoad());
	if (!Unit)
	{
		return;
	}

	Unit->Modify();
	Unit->Animating.SampleRate = SampleRate;
	Unit->MarkPackageDirty();
}

static FString ResolveStyleFamily(const TSharedPtr<FJsonObject>& Spec, const TSharedPtr<FJsonObject>& Organization, const FString& SearchText)
{
	FString Family;
	if (Spec.IsValid() && Spec->TryGetStringField(TEXT("family"), Family) && !Family.IsEmpty())
	{
		return Family;
	}
	if (Spec.IsValid() && Spec->TryGetStringField(TEXT("style_family"), Family) && !Family.IsEmpty())
	{
		return Family;
	}

	const FString LowerSearch = SearchText.ToLower();
	const TSharedPtr<FJsonObject>* Families = nullptr;
	if (Organization.IsValid() && Organization->TryGetObjectField(TEXT("families"), Families) && Families && Families->IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& FamilyPair : (*Families)->Values)
		{
			if (!FamilyPair.Value.IsValid() || FamilyPair.Value->Type != EJson::Object)
			{
				continue;
			}

			for (const FString& Keyword : StringArrayField(FamilyPair.Value->AsObject(), TEXT("keywords")))
			{
				if (!Keyword.IsEmpty() && LowerSearch.Contains(Keyword.ToLower()))
				{
					return FamilyPair.Key;
				}
			}
		}
	}
	return TEXT("default");
}

static FString ResolveFamilyFolder(const TSharedPtr<FJsonObject>& Organization, const FString& Family)
{
	const TSharedPtr<FJsonObject>* Families = nullptr;
	if (!Organization.IsValid() || !Organization->TryGetObjectField(TEXT("families"), Families) || !Families || !Families->IsValid())
	{
		return FString();
	}

	const TSharedPtr<FJsonObject>* FamilyConfig = nullptr;
	if ((*Families)->TryGetObjectField(Family, FamilyConfig) && FamilyConfig && FamilyConfig->IsValid())
	{
		return StringFieldOrDefault(*FamilyConfig, TEXT("folder"), FString());
	}
	return FString();
}

static FString ResolveAssetName(const TSharedPtr<FJsonObject>& Spec, const TSharedPtr<FJsonObject>& AuthoringDefaults, const FString& FieldName, const FString& PrefixField, const FString& FallbackPrefix, const FString& AssetSlug)
{
	FString ExplicitName;
	if (Spec.IsValid() && Spec->TryGetStringField(FieldName, ExplicitName) && !ExplicitName.IsEmpty())
	{
		return ExplicitName;
	}

	const FString Prefix = StringFieldOrDefault(AuthoringDefaults, PrefixField, FallbackPrefix);
	return Prefix + AssetSlug;
}

static TSharedPtr<FJsonObject> MakeStep(const FString& Id, const FString& Tool, const FString& Status, const FString& Description)
{
	TSharedPtr<FJsonObject> Step = MakeShared<FJsonObject>();
	Step->SetStringField(TEXT("id"), Id);
	Step->SetStringField(TEXT("tool"), Tool);
	Step->SetStringField(TEXT("status"), Status);
	Step->SetStringField(TEXT("description"), Description);
	return Step;
}

static void AddStep(TArray<TSharedPtr<FJsonValue>>& Steps, const FString& Id, const FString& Tool, const FString& Status, const FString& Description)
{
	Steps.Add(MakeShared<FJsonValueObject>(MakeStep(Id, Tool, Status, Description)));
}

static TSharedPtr<FJsonObject> AddExecutionStep(TArray<TSharedPtr<FJsonValue>>& Steps, const FString& Id, const FString& Tool, const FString& Status, const FString& Description)
{
	TSharedPtr<FJsonObject> Step = MakeStep(Id, Tool, Status, Description);
	Steps.Add(MakeShared<FJsonValueObject>(Step));
	return Step;
}

static void SetStepResult(TSharedPtr<FJsonObject> Step, const FString& ResultJson)
{
	if (!Step.IsValid())
	{
		return;
	}

	TSharedPtr<FJsonObject> Result = ParseObject(ResultJson);
	if (Result.IsValid())
	{
		Step->SetObjectField(TEXT("result"), Result);
	}
	else if (!ResultJson.IsEmpty())
	{
		Step->SetStringField(TEXT("result"), ResultJson);
	}
}

static void AddStringArrayItem(TArray<TSharedPtr<FJsonValue>>& Array, const FString& Text)
{
	if (!Text.IsEmpty())
	{
		Array.Add(MakeShared<FJsonValueString>(Text));
	}
}

static TSharedPtr<FJsonObject> MakeIssue(const FString& Severity, const FString& Code, const FString& Message, const FString& Field = FString())
{
	TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
	Issue->SetStringField(TEXT("severity"), Severity);
	Issue->SetStringField(TEXT("code"), Code);
	Issue->SetStringField(TEXT("message"), Message);
	if (!Field.IsEmpty())
	{
		Issue->SetStringField(TEXT("field"), Field);
	}
	return Issue;
}

static void AddIssue(TArray<TSharedPtr<FJsonValue>>& Issues, const FString& Severity, const FString& Code, const FString& Message, const FString& Field = FString())
{
	Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(Severity, Code, Message, Field)));
}

static bool TryMakeIssueKey(const TSharedPtr<FJsonValue>& IssueValue, FString& OutKey)
{
	if (!IssueValue.IsValid() || IssueValue->Type != EJson::Object)
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Issue = IssueValue->AsObject();
	if (!Issue.IsValid())
	{
		return false;
	}

	FString Severity;
	FString Code;
	FString Field;
	FString Message;
	Issue->TryGetStringField(TEXT("severity"), Severity);
	Issue->TryGetStringField(TEXT("code"), Code);
	Issue->TryGetStringField(TEXT("field"), Field);
	Issue->TryGetStringField(TEXT("message"), Message);
	if (Severity.IsEmpty() && Code.IsEmpty() && Field.IsEmpty() && Message.IsEmpty())
	{
		return false;
	}

	OutKey = FString::Printf(TEXT("%s|%s|%s|%s"), *Severity, *Code, *Field, *Message);
	return true;
}

static bool HasIssueWithKey(const TArray<TSharedPtr<FJsonValue>>& Issues, const FString& Key)
{
	for (const TSharedPtr<FJsonValue>& Existing : Issues)
	{
		FString ExistingKey;
		if (TryMakeIssueKey(Existing, ExistingKey) && ExistingKey == Key)
		{
			return true;
		}
	}
	return false;
}

static void AddIssueValueUnique(TArray<TSharedPtr<FJsonValue>>& Issues, const TSharedPtr<FJsonValue>& IssueValue)
{
	FString Key;
	if (!TryMakeIssueKey(IssueValue, Key))
	{
		Issues.Add(IssueValue);
		return;
	}

	if (!HasIssueWithKey(Issues, Key))
	{
		Issues.Add(IssueValue);
	}
}

static void AddIssueUnique(TArray<TSharedPtr<FJsonValue>>& Issues, const FString& Severity, const FString& Code, const FString& Message, const FString& Field = FString())
{
	AddIssueValueUnique(Issues, MakeShared<FJsonValueObject>(MakeIssue(Severity, Code, Message, Field)));
}

static FString TexturePathOrEmpty(const UObject* Object)
{
	return Object ? Object->GetPathName() : FString();
}

static bool ParseOriginalTexturesJson(const FString& JsonString, TArray<FOriginalTextures>& OutTextures)
{
	OutTextures.Empty();
	if (JsonString.TrimStartAndEnd().IsEmpty())
	{
		return true;
	}

	TArray<TSharedPtr<FJsonValue>> JsonArray;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, JsonArray))
	{
		return false;
	}

	auto LoadTexture2D = [](const TSharedPtr<FJsonObject>& Object, const FString& FieldName) -> UTexture2D*
	{
		FString Path;
		if (!Object.IsValid() || !Object->TryGetStringField(FieldName, Path) || Path.IsEmpty())
		{
			return nullptr;
		}
		return Cast<UTexture2D>(FSoftObjectPath(EnsureObjectPath(Path)).TryLoad());
	};

	for (const TSharedPtr<FJsonValue>& Value : JsonArray)
	{
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			continue;
		}

		TSharedPtr<FJsonObject> Object = Value->AsObject();
		FOriginalTextures TextureSet;
		Object->TryGetStringField(TEXT("SlotName"), TextureSet.SlotName);
		Object->TryGetBoolField(TEXT("bUseARM"), TextureSet.bUseARM);
		TextureSet.ARM = LoadTexture2D(Object, TEXT("ARM"));
		TextureSet.BaseColor = LoadTexture2D(Object, TEXT("BaseColor"));
		TextureSet.Specular = LoadTexture2D(Object, TEXT("Specular"));
		TextureSet.Roughness = LoadTexture2D(Object, TEXT("Roughness"));
		TextureSet.Normal = LoadTexture2D(Object, TEXT("Normal"));
		TextureSet.Metallic = LoadTexture2D(Object, TEXT("Metallic"));
		TextureSet.Emissive = LoadTexture2D(Object, TEXT("Emissive"));
		TextureSet.Opacity = LoadTexture2D(Object, TEXT("Opacity"));
		TextureSet.AO = LoadTexture2D(Object, TEXT("AO"));
		OutTextures.Add(TextureSet);
	}

	return true;
}

static FString SerializeOriginalTexturesJson(const TArray<FOriginalTextures>& Textures)
{
	TArray<TSharedPtr<FJsonValue>> JsonArray;
	for (const FOriginalTextures& TextureSet : Textures)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("SlotName"), TextureSet.SlotName);
		Object->SetBoolField(TEXT("bUseARM"), TextureSet.bUseARM);
		Object->SetStringField(TEXT("ARM"), TexturePathOrEmpty(TextureSet.ARM));
		Object->SetStringField(TEXT("BaseColor"), TexturePathOrEmpty(TextureSet.BaseColor));
		Object->SetStringField(TEXT("Specular"), TexturePathOrEmpty(TextureSet.Specular));
		Object->SetStringField(TEXT("Roughness"), TexturePathOrEmpty(TextureSet.Roughness));
		Object->SetStringField(TEXT("Normal"), TexturePathOrEmpty(TextureSet.Normal));
		Object->SetStringField(TEXT("Metallic"), TexturePathOrEmpty(TextureSet.Metallic));
		Object->SetStringField(TEXT("Emissive"), TexturePathOrEmpty(TextureSet.Emissive));
		Object->SetStringField(TEXT("Opacity"), TexturePathOrEmpty(TextureSet.Opacity));
		Object->SetStringField(TEXT("AO"), TexturePathOrEmpty(TextureSet.AO));
		JsonArray.Add(MakeShared<FJsonValueObject>(Object));
	}

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(JsonArray, Writer);
	return Output;
}

static UTexture2D* AsTexture2D(UTexture* Texture)
{
	return Texture ? Cast<UTexture2D>(Texture) : nullptr;
}

static UTexture2D* FindTextureParameter2D(UMaterialInterface* Material, const TArray<FName>& ParameterNames)
{
	UMaterialInstanceConstant* MaterialInstance = Cast<UMaterialInstanceConstant>(Material);
	if (!MaterialInstance)
	{
		return nullptr;
	}

	for (const FName& ParameterName : ParameterNames)
	{
		if (UTexture2D* Texture = AsTexture2D(UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(MaterialInstance, ParameterName)))
		{
			return Texture;
		}
		if (UTexture2D* Texture = AsTexture2D(UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(MaterialInstance, ParameterName, EMaterialParameterAssociation::LayerParameter)))
		{
			return Texture;
		}
	}
	return nullptr;
}

static bool TextureNameMatchesAnyToken(UTexture2D* Texture, const TArray<FString>& Tokens)
{
	if (!Texture)
	{
		return false;
	}

	const FString TextureName = Texture->GetName();
	for (const FString& Token : Tokens)
	{
		if (!Token.IsEmpty() && TextureName.Contains(Token, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

static UTexture2D* FindUsedTexture2D(UMaterialInterface* Material, const TArray<FString>& NameTokens, bool bAllowSingleTextureFallback)
{
	if (!Material)
	{
		return nullptr;
	}

	TArray<UTexture*> UsedTextures = UMaterialEditingLibrary::GetMaterialUsedTextures(Material);
	TArray<UTexture2D*> UsedTexture2Ds;
	for (UTexture* UsedTexture : UsedTextures)
	{
		if (UTexture2D* Texture2D = AsTexture2D(UsedTexture))
		{
			UsedTexture2Ds.AddUnique(Texture2D);
		}
	}

	for (UTexture2D* Texture2D : UsedTexture2Ds)
	{
		if (TextureNameMatchesAnyToken(Texture2D, NameTokens))
		{
			return Texture2D;
		}
	}

	if (bAllowSingleTextureFallback && UsedTexture2Ds.Num() == 1)
	{
		return UsedTexture2Ds[0];
	}
	return nullptr;
}

static UTexture2D* ResolveSourceMaterialTexture2D(
	UMaterialInterface* Material,
	const TArray<FName>& ParameterNames,
	const TArray<FString>& NameTokens,
	bool bAllowSingleTextureFallback)
{
	if (UTexture2D* Texture = FindTextureParameter2D(Material, ParameterNames))
	{
		return Texture;
	}
	return FindUsedTexture2D(Material, NameTokens, bAllowSingleTextureFallback);
}

static int32 EnrichOriginalTexturesFromSkeletalMaterials(
	const FString& SkeletalMeshPath,
	TArray<FOriginalTextures>& OriginalTextures,
	TArray<TSharedPtr<FJsonValue>>& Warnings)
{
	USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(FSoftObjectPath(EnsureObjectPath(SkeletalMeshPath)).TryLoad());
	if (!SkeletalMesh)
	{
		return 0;
	}

	const TArray<FSkeletalMaterial>& SkeletalMaterials = SkeletalMesh->GetMaterials();
	int32 FilledCount = 0;
	while (OriginalTextures.Num() < SkeletalMaterials.Num())
	{
		OriginalTextures.AddDefaulted();
	}

	for (int32 SlotIndex = 0; SlotIndex < SkeletalMaterials.Num(); ++SlotIndex)
	{
		FOriginalTextures& TextureSet = OriginalTextures[SlotIndex];
		const FSkeletalMaterial& SkeletalMaterial = SkeletalMaterials[SlotIndex];
		if (TextureSet.SlotName.IsEmpty())
		{
			TextureSet.SlotName = SkeletalMaterial.MaterialSlotName.ToString();
		}

		UMaterialInterface* SourceMaterial = SkeletalMaterial.MaterialInterface;
		if (!SourceMaterial)
		{
			continue;
		}

		TArray<FString> FilledFields;
		auto FillIfMissing = [&](TObjectPtr<UTexture2D>& Target, const FString& FieldName, const TArray<FName>& ParameterNames, const TArray<FString>& NameTokens, bool bAllowSingleTextureFallback)
		{
			if (Target)
			{
				return;
			}
			if (UTexture2D* ResolvedTexture = ResolveSourceMaterialTexture2D(SourceMaterial, ParameterNames, NameTokens, bAllowSingleTextureFallback))
			{
				Target = ResolvedTexture;
				FilledFields.Add(FString::Printf(TEXT("%s=%s"), *FieldName, *ResolvedTexture->GetPathName()));
				++FilledCount;
			}
		};

		FillIfMissing(TextureSet.BaseColor, TEXT("BaseColor"),
			{ TEXT("BaseColorTex"), TEXT("BaseColorTexture"), TEXT("baseColorTexture"), TEXT("BaseColor"), TEXT("Albedo"), TEXT("Diffuse"), TEXT("DiffuseTex"), TEXT("ColorTex"), TEXT("Texture") },
			{ TEXT("BaseColor"), TEXT("Base_Color"), TEXT("Albedo"), TEXT("Diffuse"), TEXT("_D"), TEXT("_BC"), TEXT("Color") },
			true);
		FillIfMissing(TextureSet.Normal, TEXT("Normal"),
			{ TEXT("NormalTex"), TEXT("NormalTexture"), TEXT("normalTexture"), TEXT("Normal"), TEXT("NormalMap") },
			{ TEXT("Normal"), TEXT("_N"), TEXT("_Nor") },
			false);
		FillIfMissing(TextureSet.Roughness, TEXT("Roughness"),
			{ TEXT("RoughnessTex"), TEXT("RoughnessTexture"), TEXT("roughnessTexture"), TEXT("Roughness") },
			{ TEXT("Roughness"), TEXT("_R"), TEXT("_Rough") },
			false);
		FillIfMissing(TextureSet.Metallic, TEXT("Metallic"),
			{ TEXT("MetallicTex"), TEXT("MetallicTexture"), TEXT("metallicTexture"), TEXT("Metallic") },
			{ TEXT("Metallic"), TEXT("_M"), TEXT("_Metal") },
			false);
		FillIfMissing(TextureSet.ARM, TEXT("ARM"),
			{ TEXT("ARMTex"), TEXT("ARMTexture"), TEXT("OcclusionRoughnessMetallic"), TEXT("ORM"), TEXT("metallicRoughnessTexture") },
			{ TEXT("_ARM"), TEXT("_ORM"), TEXT("_MRA"), TEXT("MetallicRoughness") },
			false);
		if (TextureSet.ARM)
		{
			TextureSet.bUseARM = true;
		}
		FillIfMissing(TextureSet.Specular, TEXT("Specular"),
			{ TEXT("SpecularTex"), TEXT("SpecularTexture"), TEXT("Specular") },
			{ TEXT("Specular"), TEXT("_S"), TEXT("_Spec") },
			false);
		FillIfMissing(TextureSet.Emissive, TEXT("Emissive"),
			{ TEXT("EmissiveTex"), TEXT("EmissiveTexture"), TEXT("emissiveTexture"), TEXT("Emissive") },
			{ TEXT("Emissive"), TEXT("_E"), TEXT("_Emiss") },
			false);
		FillIfMissing(TextureSet.Opacity, TEXT("Opacity"),
			{ TEXT("OpacityTex"), TEXT("OpacityTexture"), TEXT("AlphaTexture"), TEXT("opacityTexture"), TEXT("Opacity"), TEXT("Alpha") },
			{ TEXT("Opacity"), TEXT("_O"), TEXT("_Mask"), TEXT("_Trans"), TEXT("Alpha") },
			false);
		FillIfMissing(TextureSet.AO, TEXT("AO"),
			{ TEXT("AOTex"), TEXT("AOTexture"), TEXT("OcclusionTexture"), TEXT("occlusionTexture"), TEXT("AmbientOcclusion") },
			{ TEXT("_AO"), TEXT("AmbientOcclusion"), TEXT("Occlusion") },
			false);

		if (!FilledFields.IsEmpty())
		{
			AddIssue(Warnings, TEXT("warning"), TEXT("defaulted_original_textures_from_source_material"),
				FString::Printf(TEXT("Original texture data for slot '%s' was incomplete; inherited %s from source material %s."),
					*TextureSet.SlotName,
					*FString::Join(FilledFields, TEXT(", ")),
					*SourceMaterial->GetPathName()),
				TEXT("textures"));
		}
	}

	return FilledCount;
}

static bool HasErrorIssue(const TArray<TSharedPtr<FJsonValue>>& Issues)
{
	for (const TSharedPtr<FJsonValue>& Value : Issues)
	{
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			continue;
		}

		FString Severity;
		if (Value->AsObject()->TryGetStringField(TEXT("severity"), Severity) && Severity.Equals(TEXT("error"), ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

static TSharedPtr<FJsonObject> MakeExecutionPreviewStep(const FString& Id, const FString& Status, const FString& Description)
{
	TSharedPtr<FJsonObject> Step = MakeShared<FJsonObject>();
	Step->SetStringField(TEXT("id"), Id);
	Step->SetStringField(TEXT("status"), Status);
	Step->SetStringField(TEXT("description"), Description);
	return Step;
}

static void AddExecutionPreview(TArray<TSharedPtr<FJsonValue>>& Steps, const FString& Id, const FString& Status, const FString& Description)
{
	Steps.Add(MakeShared<FJsonValueObject>(MakeExecutionPreviewStep(Id, Status, Description)));
}

static bool JsonObjectFieldBool(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, bool bDefault = false)
{
	bool bValue = bDefault;
	if (Object.IsValid())
	{
		Object->TryGetBoolField(FieldName, bValue);
	}
	return bValue;
}

static void CopyStringField(const TSharedPtr<FJsonObject>& Source, const TSharedPtr<FJsonObject>& Target, const FString& FieldName)
{
	if (!Source.IsValid() || !Target.IsValid())
	{
		return;
	}

	FString Value;
	if (Source->TryGetStringField(FieldName, Value))
	{
		Target->SetStringField(FieldName, Value);
	}
}

static void CopyBoolField(const TSharedPtr<FJsonObject>& Source, const TSharedPtr<FJsonObject>& Target, const FString& FieldName)
{
	if (!Source.IsValid() || !Target.IsValid())
	{
		return;
	}

	bool bValue = false;
	if (Source->TryGetBoolField(FieldName, bValue))
	{
		Target->SetBoolField(FieldName, bValue);
	}
}

static void CopyNumberField(const TSharedPtr<FJsonObject>& Source, const TSharedPtr<FJsonObject>& Target, const FString& FieldName)
{
	if (!Source.IsValid() || !Target.IsValid())
	{
		return;
	}

	double Value = 0.0;
	if (Source->TryGetNumberField(FieldName, Value))
	{
		Target->SetNumberField(FieldName, Value);
	}
}

static void CopyArrayField(const TSharedPtr<FJsonObject>& Source, const TSharedPtr<FJsonObject>& Target, const FString& FieldName)
{
	if (!Source.IsValid() || !Target.IsValid())
	{
		return;
	}

	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (Source->TryGetArrayField(FieldName, Array) && Array)
	{
		Target->SetArrayField(FieldName, *Array);
	}
}

static TSharedPtr<FJsonObject> MakeCompactCreateVatApplySummary(const TSharedPtr<FJsonObject>& ApplyJson)
{
	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	if (!ApplyJson.IsValid())
	{
		Summary->SetBoolField(TEXT("success"), false);
		Summary->SetStringField(TEXT("error"), TEXT("Apply result was not valid JSON."));
		return Summary;
	}

	CopyBoolField(ApplyJson, Summary, TEXT("success"));
	CopyBoolField(ApplyJson, Summary, TEXT("dry_run"));
	CopyBoolField(ApplyJson, Summary, TEXT("save_assets"));
	CopyBoolField(ApplyJson, Summary, TEXT("static_fallback_used"));
	CopyStringField(ApplyJson, Summary, TEXT("error"));

	const TSharedPtr<FJsonObject>* Plan = nullptr;
	if (ApplyJson->TryGetObjectField(TEXT("plan"), Plan) && Plan && Plan->IsValid())
	{
		CopyArrayField(*Plan, Summary, TEXT("warnings"));

		const TSharedPtr<FJsonObject>* Layout = nullptr;
		if ((*Plan)->TryGetObjectField(TEXT("resolved_layout"), Layout) && Layout && Layout->IsValid())
		{
			TSharedPtr<FJsonObject> LayoutSummary = MakeShared<FJsonObject>();
			const TArray<FString> LayoutFields = {
				TEXT("unit_name"),
				TEXT("target_package_path"),
				TEXT("target_unit_package_path"),
				TEXT("static_mesh_path"),
				TEXT("unit_path"),
				TEXT("renderer_class_path"),
				TEXT("vat_data_asset_path")
			};
			for (const FString& FieldName : LayoutFields)
			{
				CopyStringField(*Layout, LayoutSummary, FieldName);
			}
			Summary->SetObjectField(TEXT("resolved_layout"), LayoutSummary);
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* ExecutionSteps = nullptr;
	if (ApplyJson->TryGetArrayField(TEXT("execution_steps"), ExecutionSteps) && ExecutionSteps)
	{
		TSharedPtr<FJsonObject> StepStatuses = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> BakeSummary = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> UnitSummary = MakeShared<FJsonObject>();

		for (const TSharedPtr<FJsonValue>& Value : *ExecutionSteps)
		{
			if (!Value.IsValid() || Value->Type != EJson::Object)
			{
				continue;
			}

			const TSharedPtr<FJsonObject> Step = Value->AsObject();
			FString Id;
			FString Status;
			if (Step->TryGetStringField(TEXT("id"), Id) && Step->TryGetStringField(TEXT("status"), Status) && !Id.IsEmpty())
			{
				StepStatuses->SetStringField(Id, Status);
			}

			if (Id == TEXT("bake_vat_textures"))
			{
				const TSharedPtr<FJsonObject>* BakeResult = nullptr;
				if (Step->TryGetObjectField(TEXT("result"), BakeResult) && BakeResult && BakeResult->IsValid())
				{
					CopyBoolField(*BakeResult, BakeSummary, TEXT("success"));
					CopyNumberField(*BakeResult, BakeSummary, TEXT("sample_rate"));
					CopyNumberField(*BakeResult, BakeSummary, TEXT("animation_count"));
					CopyNumberField(*BakeResult, BakeSummary, TEXT("static_lod_count"));
					CopyNumberField(*BakeResult, BakeSummary, TEXT("baked_lod_count"));
					CopyNumberField(*BakeResult, BakeSummary, TEXT("material_update_count"));
					CopyStringField(*BakeResult, BakeSummary, TEXT("data_asset_path"));
					CopyStringField(*BakeResult, BakeSummary, TEXT("anim_data_texture_path"));
				}
			}
			else if (Id == TEXT("create_unit") || Id == TEXT("merge_unit_data"))
			{
				const TSharedPtr<FJsonObject>* StepResult = nullptr;
				if (Step->TryGetObjectField(TEXT("result"), StepResult) && StepResult && StepResult->IsValid())
				{
					CopyStringField(*StepResult, UnitSummary, TEXT("asset_path"));
					CopyBoolField(*StepResult, UnitSummary, TEXT("saved"));
					const TSharedPtr<FJsonObject>* ApplyResult = nullptr;
					if ((*StepResult)->TryGetObjectField(TEXT("apply_result"), ApplyResult) && ApplyResult && ApplyResult->IsValid())
					{
						CopyStringField(*ApplyResult, UnitSummary, TEXT("asset_path"));
						CopyBoolField(*ApplyResult, UnitSummary, TEXT("saved"));
					}
				}
			}
		}

		Summary->SetObjectField(TEXT("step_statuses"), StepStatuses);
		if (!BakeSummary->Values.IsEmpty())
		{
			Summary->SetObjectField(TEXT("vat_bake"), BakeSummary);
		}
		if (!UnitSummary->Values.IsEmpty())
		{
			Summary->SetObjectField(TEXT("unit"), UnitSummary);
		}
	}

	return Summary;
}

static bool TryGetStringByNames(const TSharedPtr<FJsonObject>& Object, const TArray<FString>& FieldNames, FString& OutValue)
{
	if (!Object.IsValid())
	{
		return false;
	}

	for (const FString& FieldName : FieldNames)
	{
		if (Object->TryGetStringField(FieldName, OutValue) && !OutValue.IsEmpty())
		{
			return true;
		}
	}
	return false;
}

static FString ResolvePathFromSpecOrDefault(
	const TSharedPtr<FJsonObject>& Spec,
	const TArray<FString>& SpecFieldNames,
	const TSharedPtr<FJsonObject>& AuthoringDefaults,
	const TArray<FString>& DefaultFieldNames,
	const FString& BuiltinDefault,
	const FString& WarningCode,
	const FString& FieldName,
	TArray<TSharedPtr<FJsonValue>>& Warnings)
{
	FString Path;
	if (TryGetObjectPathSpec(Spec, SpecFieldNames, Path))
	{
		return Path;
	}

	if (TryGetStringByNames(AuthoringDefaults, DefaultFieldNames, Path) && !Path.IsEmpty())
	{
		AddIssue(Warnings, TEXT("warning"), WarningCode, FString::Printf(TEXT("%s was not supplied; using style default: %s"), *FieldName, *Path), FieldName);
		return Path;
	}

	if (!BuiltinDefault.IsEmpty())
	{
		AddIssue(Warnings, TEXT("warning"), WarningCode, FString::Printf(TEXT("%s was not supplied; using built-in MassBattle default: %s"), *FieldName, *BuiltinDefault), FieldName);
		return BuiltinDefault;
	}

	return FString();
}

static bool ResolveDefaultTemplateUnitForFamily(
	const TSharedPtr<FJsonObject>& AuthoringDefaults,
	const FString& StyleFamily,
	FString& OutTemplateUnitPath,
	FString& OutSource)
{
	OutTemplateUnitPath.Reset();
	OutSource.Reset();
	if (!AuthoringDefaults.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* FamilyTemplates = nullptr;
	if (AuthoringDefaults->TryGetObjectField(TEXT("default_unit_templates"), FamilyTemplates) && FamilyTemplates && FamilyTemplates->IsValid())
	{
		TArray<FString> CandidateKeys;
		if (!StyleFamily.IsEmpty())
		{
			CandidateKeys.Add(StyleFamily);
			CandidateKeys.Add(StyleFamily.ToLower());
		}
		CandidateKeys.Add(TEXT("default"));

		for (const FString& Key : CandidateKeys)
		{
			if ((*FamilyTemplates)->TryGetStringField(Key, OutTemplateUnitPath) && !OutTemplateUnitPath.IsEmpty())
			{
				OutSource = TEXT("authoring_defaults.default_unit_templates.") + Key;
				return true;
			}
		}
	}

	if (AuthoringDefaults->TryGetStringField(TEXT("default_unit_template"), OutTemplateUnitPath) && !OutTemplateUnitPath.IsEmpty())
	{
		OutSource = TEXT("authoring_defaults.default_unit_template");
		return true;
	}

	return false;
}

static bool ContainsAnyKeyword(const FString& Text, const TArray<FString>& Keywords)
{
	for (const FString& Keyword : Keywords)
	{
		if (Text.Contains(Keyword, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

static void AddAnimToFallbackCategory(FFoundAnimSequences& FoundAnims, UAnimSequence* AnimSequence)
{
	if (!AnimSequence)
	{
		return;
	}

	const FString Name = AnimSequence->GetName();
	const int32 MaxPerCategory = 5;
	auto AddToCategory = [AnimSequence, MaxPerCategory](TArray<TObjectPtr<UAnimSequence>>& TargetArray)
	{
		if (TargetArray.Num() < MaxPerCategory)
		{
			TargetArray.AddUnique(AnimSequence);
		}
	};

	if (ContainsAnyKeyword(Name, { TEXT("Idle"), TEXT("Idling"), TEXT("Wave"), TEXT("Waving"), TEXT("Loop") }))
	{
		AddToCategory(FoundAnims.Idle);
	}
	else if (ContainsAnyKeyword(Name, { TEXT("Move"), TEXT("Walk"), TEXT("Run"), TEXT("Crawl"), TEXT("Fly"), TEXT("Swim"), TEXT("Dash"), TEXT("Sprint") }))
	{
		AddToCategory(FoundAnims.Move);
	}
	else if (ContainsAnyKeyword(Name, { TEXT("Fall"), TEXT("Falling"), TEXT("Jump"), TEXT("Land") }))
	{
		AddToCategory(FoundAnims.Fall);
	}
	else if (ContainsAnyKeyword(Name, { TEXT("Appear"), TEXT("Birth"), TEXT("Spawn"), TEXT("Born"), TEXT("Wake"), TEXT("Rise"), TEXT("Raise"), TEXT("Raised"), TEXT("Up") }))
	{
		AddToCategory(FoundAnims.Appear);
	}
	else if (ContainsAnyKeyword(Name, { TEXT("Attack"), TEXT("Fire"), TEXT("Firing"), TEXT("Shoot"), TEXT("Cast"), TEXT("Strike"), TEXT("Bash"), TEXT("Slash"), TEXT("Punch"), TEXT("Kick") }))
	{
		AddToCategory(FoundAnims.Attack);
	}
	else if (ContainsAnyKeyword(Name, { TEXT("Hit"), TEXT("BeingHit"), TEXT("Reaction"), TEXT("Damage"), TEXT("Hurt"), TEXT("Shake") }))
	{
		AddToCategory(FoundAnims.Hit);
	}
	else if (ContainsAnyKeyword(Name, { TEXT("Death"), TEXT("Die"), TEXT("Dying"), TEXT("Dead"), TEXT("Kill"), TEXT("Lower"), TEXT("Down"), TEXT("Capture"), TEXT("Captured") }))
	{
		AddToCategory(FoundAnims.Death);
	}
	else
	{
		AddToCategory(FoundAnims.Other);
	}
}

static IAssetRegistry& GetAssetRegistry();

static bool TryFillAnimDiscoveryWithCompatibleFallback(
	const FString& SkeletalMeshPath,
	const FString& SearchPath,
	const FString& FileName,
	TSharedPtr<FJsonObject> AnimJson,
	TArray<TSharedPtr<FJsonValue>>& Warnings)
{
	USkeletalMesh* Mesh = Cast<USkeletalMesh>(FSoftObjectPath(EnsureObjectPath(SkeletalMeshPath)).TryLoad());
	if (!Mesh || !Mesh->GetSkeleton() || SearchPath.IsEmpty() || !AnimJson.IsValid())
	{
		return false;
	}

	auto CollectCompatibleAnimations = [Mesh, &SearchPath](const FString& NameFilter, TArray<UAnimSequence*>& OutSequences)
	{
		const FString SafeSearchPath = SearchPath;
		TArray<FString> PathsToScan;
		PathsToScan.Add(SafeSearchPath);
		GetAssetRegistry().ScanPathsSynchronous(PathsToScan, true);

		FARFilter Filter;
		Filter.ClassPaths.Add(UAnimSequence::StaticClass()->GetClassPathName());
		Filter.PackagePaths.Add(*SafeSearchPath);
		Filter.bRecursivePaths = true;

		TArray<FAssetData> Assets;
		GetAssetRegistry().GetAssets(Filter, Assets);
		Assets.Sort([](const FAssetData& A, const FAssetData& B)
		{
			return A.GetSoftObjectPath().ToString() < B.GetSoftObjectPath().ToString();
		});

		for (const FAssetData& Asset : Assets)
		{
			const FString AssetName = Asset.AssetName.ToString();
			if (!NameFilter.IsEmpty() && !AssetName.Contains(NameFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}

			UAnimSequence* AnimSequence = Cast<UAnimSequence>(Asset.GetAsset());
			if (AnimSequence && AnimSequence->GetSkeleton() == Mesh->GetSkeleton())
			{
				OutSequences.AddUnique(AnimSequence);
			}
		}
	};

	TArray<UAnimSequence*> CompatibleAnimations;
	CollectCompatibleAnimations(FileName, CompatibleAnimations);
	bool bIgnoredNameFilter = false;
	if (CompatibleAnimations.IsEmpty() && !FileName.IsEmpty())
	{
		CollectCompatibleAnimations(FString(), CompatibleAnimations);
		bIgnoredNameFilter = !CompatibleAnimations.IsEmpty();
	}

	if (CompatibleAnimations.IsEmpty())
	{
		return false;
	}

	FFoundAnimSequences FallbackAnims;
	for (UAnimSequence* AnimSequence : CompatibleAnimations)
	{
		AddAnimToFallbackCategory(FallbackAnims, AnimSequence);
	}

	if (FallbackAnims.Idle.IsEmpty())
	{
		FallbackAnims.Idle.AddUnique(CompatibleAnimations[0]);
	}

	TSharedPtr<FJsonObject> FallbackAnimsObject = ParseObject(SerializeFoundAnimsLocal(FallbackAnims));
	if (!FallbackAnimsObject.IsValid())
	{
		return false;
	}

	AnimJson->SetObjectField(TEXT("anims"), FallbackAnimsObject);
	AnimJson->SetBoolField(TEXT("fallback_used"), true);
	AnimJson->SetNumberField(TEXT("fallback_candidate_count"), CompatibleAnimations.Num());
	AnimJson->SetBoolField(TEXT("fallback_ignored_name_filter"), bIgnoredNameFilter);

	const FString FilterNote = bIgnoredNameFilter
		? FString::Printf(TEXT(" The supplied animation_name_filter '%s' matched nothing and was ignored for fallback."), *FileName)
		: FString();
	AddIssue(Warnings, TEXT("warning"), TEXT("animation_discovery_fallback_used"), FString::Printf(TEXT("FindAndFillAnimSequences produced no categorized animations; MCP used %d same-skeleton AnimSequence asset(s) as a fallback.%s Provide an explicit animations map for exact state mapping."), CompatibleAnimations.Num(), *FilterNote), TEXT("animations"));
	return true;
}

struct FOrganizeAssetRef
{
	FString ObjectPath;
	FString PackageName;
	FString PackagePath;
	FString AssetName;
	FString ClassPath;
	FString DiscoveredBy;
	int32 Depth = 0;
};

static IAssetRegistry& GetAssetRegistry()
{
	return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
}

static FString PackageNameFromObjectPath(const FString& ObjectPath)
{
	FString PackageName = EnsureObjectPath(ObjectPath);
	FString ObjectName;
	if (PackageName.Split(TEXT("."), &PackageName, &ObjectName, ESearchCase::CaseSensitive, ESearchDir::FromStart))
	{
		return PackageName;
	}
	return PackageName;
}

static FString PackagePathFromPackageName(const FString& PackageName)
{
	FString PackagePath;
	FString AssetName;
	if (PackageName.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
	{
		return PackagePath;
	}
	return FString();
}

static bool IsGameContentPackage(const FString& PackageName)
{
	return PackageName.StartsWith(TEXT("/Game/")) || PackageName.Equals(TEXT("/Game"), ESearchCase::IgnoreCase);
}

static bool IsSupportedContentPackage(const FString& PackageName)
{
	return IsGameContentPackage(PackageName)
		|| PackageName.StartsWith(TEXT("/MassBattle/"))
		|| PackageName.Equals(TEXT("/MassBattle"), ESearchCase::IgnoreCase);
}

static bool IsPathUnderRoot(const FString& PackageName, const FString& Root)
{
	const FString CleanRoot = Root.TrimStartAndEnd();
	if (CleanRoot.IsEmpty())
	{
		return false;
	}
	return PackageName.Equals(CleanRoot, ESearchCase::IgnoreCase)
		|| PackageName.StartsWith(CleanRoot / TEXT(""), ESearchCase::IgnoreCase);
}

static void AddUniqueRoot(TArray<FString>& Roots, const FString& Root)
{
	const FString CleanRoot = Root.TrimStartAndEnd();
	if (!CleanRoot.IsEmpty())
	{
		Roots.AddUnique(CleanRoot);
	}
}

static void AddRootField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, TArray<FString>& Roots)
{
	FString Root;
	if (Object.IsValid() && Object->TryGetStringField(FieldName, Root))
	{
		AddUniqueRoot(Roots, Root);
	}
}

static void AddRootArrayField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, TArray<FString>& Roots)
{
	for (const FString& Root : StringArrayField(Object, FieldName))
	{
		AddUniqueRoot(Roots, Root);
	}
}

static void AddAnimationRootFields(const TSharedPtr<FJsonObject>& Object, TArray<FString>& Roots)
{
	AddRootField(Object, TEXT("animation_search_path"), Roots);
	AddRootField(Object, TEXT("animation_search_root"), Roots);
	AddRootField(Object, TEXT("source_search_path"), Roots);
	AddRootField(Object, TEXT("search_path"), Roots);
	AddRootArrayField(Object, TEXT("animation_search_paths"), Roots);
	AddRootArrayField(Object, TEXT("animation_search_roots"), Roots);
	AddRootArrayField(Object, TEXT("source_search_roots"), Roots);
	AddRootArrayField(Object, TEXT("search_paths"), Roots);
	AddRootArrayField(Object, TEXT("search_roots"), Roots);
}

static TSharedPtr<FJsonObject> MakeEmptyFoundAnimsObject()
{
	TSharedPtr<FJsonObject> Anims = MakeShared<FJsonObject>();
	for (const FString& Category : { TEXT("Idle"), TEXT("Move"), TEXT("Fall"), TEXT("Appear"), TEXT("Attack"), TEXT("Hit"), TEXT("Death"), TEXT("Other") })
	{
		Anims->SetArrayField(Category, TArray<TSharedPtr<FJsonValue>>());
	}
	return Anims;
}

static int32 CountJsonObjectArrayItems(const TSharedPtr<FJsonObject>& Object)
{
	int32 Count = 0;
	if (!Object.IsValid())
	{
		return Count;
	}

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
	{
		if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Array)
		{
			Count += Pair.Value->AsArray().Num();
		}
	}
	return Count;
}

static bool HasAnyObjectPathField(const TSharedPtr<FJsonObject>& Object, const TArray<FString>& FieldNames)
{
	FString Unused;
	return TryGetObjectPathSpec(Object, FieldNames, Unused);
}

static bool HasAnyField(const TSharedPtr<FJsonObject>& Object, const TArray<FString>& FieldNames)
{
	if (!Object.IsValid())
	{
		return false;
	}

	for (const FString& FieldName : FieldNames)
	{
		if (Object->HasField(FieldName))
		{
			return true;
		}
	}
	return false;
}

static FString GeneratedClassPathFromSelectedObject(UObject* Object)
{
	if (UBlueprint* Blueprint = Cast<UBlueprint>(Object))
	{
		return Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : FString();
	}
	if (UClass* Class = Cast<UClass>(Object))
	{
		return Class->GetPathName();
	}
	return FString();
}

static void AddSelectedAssetSummary(TArray<TSharedPtr<FJsonValue>>& SelectedAssets, UObject* Object, const FString& Role)
{
	if (!Object)
	{
		return;
	}

	TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetStringField(TEXT("path"), Object->GetPathName());
	Item->SetStringField(TEXT("class"), Object->GetClass() ? Object->GetClass()->GetName() : FString());
	Item->SetStringField(TEXT("name"), Object->GetName());
	if (!Role.IsEmpty())
	{
		Item->SetStringField(TEXT("role"), Role);
	}
	SelectedAssets.Add(MakeShared<FJsonValueObject>(Item));
}

static void CollectExplicitSelectedAssetObjects(const TSharedPtr<FJsonObject>& Options, TArray<UObject*>& OutObjects, TArray<TSharedPtr<FJsonValue>>& SelectionIssues)
{
	const TArray<TSharedPtr<FJsonValue>>* SelectedAssets = nullptr;
	if (!Options.IsValid() || !Options->TryGetArrayField(TEXT("selected_assets"), SelectedAssets) || !SelectedAssets)
	{
		return;
	}

	for (const TSharedPtr<FJsonValue>& Value : *SelectedAssets)
	{
		FString Path;
		if (Value.IsValid() && Value->Type == EJson::String)
		{
			Path = Value->AsString();
		}
		else if (Value.IsValid() && Value->Type == EJson::Object)
		{
			TSharedPtr<FJsonObject> Item = Value->AsObject();
			Item->TryGetStringField(TEXT("path"), Path);
			Item->TryGetStringField(TEXT("asset_path"), Path);
			Item->TryGetStringField(TEXT("object_path"), Path);
		}

		if (Path.IsEmpty())
		{
			AddIssue(SelectionIssues, TEXT("warning"), TEXT("selected_asset_path_missing"), TEXT("A selected_assets entry did not contain a loadable path."), TEXT("selected_assets"));
			continue;
		}

		UObject* Object = FSoftObjectPath(EnsureObjectPath(Path)).TryLoad();
		if (!Object)
		{
			AddIssue(SelectionIssues, TEXT("warning"), TEXT("selected_asset_not_loaded"), FString::Printf(TEXT("Selected asset could not be loaded: %s"), *Path), TEXT("selected_assets"));
			continue;
		}
		OutObjects.AddUnique(Object);
	}
}

static void CollectCurrentEditorSelectionObjects(TArray<UObject*>& OutObjects)
{
	if (!GEditor)
	{
		return;
	}

	USelection* Selection = GEditor->GetSelectedObjects();
	if (!Selection)
	{
		return;
	}

	for (FSelectionIterator It(*Selection); It; ++It)
	{
		if (UObject* Object = Cast<UObject>(*It))
		{
			OutObjects.AddUnique(Object);
		}
	}
}

static TSharedPtr<FJsonObject> BuildCreateVatUnitSpecFromSelection(const TSharedPtr<FJsonObject>& Options)
{
	TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
	if (Options.IsValid())
	{
		MergeJsonObjects(Spec, Options);
	}

	TArray<TSharedPtr<FJsonValue>> Warnings;
	TArray<TSharedPtr<FJsonValue>> SelectedAssets;
	TArray<UObject*> SelectionObjects;
	CollectExplicitSelectedAssetObjects(Options, SelectionObjects, Warnings);
	CollectCurrentEditorSelectionObjects(SelectionObjects);

	USkeletalMesh* SelectedSkeletalMesh = nullptr;
	UMassBattleAgentConfigDataAsset* SelectedUnit = nullptr;
	UAnimToTextureDataAsset* SelectedVatData = nullptr;
	UNiagaraSystem* SelectedNiagara = nullptr;
	FString SelectedRendererClassPath;
	TArray<UMaterialInterface*> SelectedMaterials;
	FFoundAnimSequences SelectedAnims;

	for (UObject* Object : SelectionObjects)
	{
		if (!Object)
		{
			continue;
		}

		if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Object))
		{
			if (!SelectedSkeletalMesh)
			{
				SelectedSkeletalMesh = SkeletalMesh;
			}
			AddSelectedAssetSummary(SelectedAssets, Object, TEXT("skeletal_mesh"));
			continue;
		}

		if (UMassBattleAgentConfigDataAsset* Unit = Cast<UMassBattleAgentConfigDataAsset>(Object))
		{
			if (!SelectedUnit)
			{
				SelectedUnit = Unit;
			}
			AddSelectedAssetSummary(SelectedAssets, Object, TEXT("unit"));
			continue;
		}

		if (UAnimSequence* AnimSequence = Cast<UAnimSequence>(Object))
		{
			AddAnimToFallbackCategory(SelectedAnims, AnimSequence);
			AddSelectedAssetSummary(SelectedAssets, Object, TEXT("animation"));
			continue;
		}

		if (UAnimToTextureDataAsset* VatData = Cast<UAnimToTextureDataAsset>(Object))
		{
			if (!SelectedVatData)
			{
				SelectedVatData = VatData;
			}
			AddSelectedAssetSummary(SelectedAssets, Object, TEXT("vat_data_asset"));
			continue;
		}

		if (UNiagaraSystem* Niagara = Cast<UNiagaraSystem>(Object))
		{
			if (!SelectedNiagara)
			{
				SelectedNiagara = Niagara;
			}
			AddSelectedAssetSummary(SelectedAssets, Object, TEXT("niagara_system"));
			continue;
		}

		if (UMaterialInterface* Material = Cast<UMaterialInterface>(Object))
		{
			SelectedMaterials.AddUnique(Material);
			AddSelectedAssetSummary(SelectedAssets, Object, TEXT("material"));
			continue;
		}

		const FString GeneratedClassPath = GeneratedClassPathFromSelectedObject(Object);
		if (!GeneratedClassPath.IsEmpty())
		{
			if (SelectedRendererClassPath.IsEmpty() && Object->GetName().Contains(TEXT("Renderer"), ESearchCase::IgnoreCase))
			{
				SelectedRendererClassPath = EnsureGeneratedClassPath(GeneratedClassPath);
			}
			AddSelectedAssetSummary(SelectedAssets, Object, TEXT("class_or_blueprint"));
			continue;
		}

		AddSelectedAssetSummary(SelectedAssets, Object, TEXT("ignored"));
	}

	if (SelectionObjects.IsEmpty())
	{
		AddIssue(Warnings, TEXT("warning"), TEXT("no_current_selection"), TEXT("No current editor selection or selected_assets were available; the generated spec will rely only on explicit options."), TEXT("selected_assets"));
	}

	if (SelectedSkeletalMesh && !HasAnyObjectPathField(Spec, { TEXT("skeletal_mesh"), TEXT("mesh"), TEXT("source_mesh") }))
	{
		Spec->SetStringField(TEXT("skeletal_mesh"), SelectedSkeletalMesh->GetPathName());
		AddIssue(Warnings, TEXT("warning"), TEXT("defaulted_skeletal_mesh_from_selection"), FString::Printf(TEXT("skeletal_mesh was inferred from selection: %s"), *SelectedSkeletalMesh->GetPathName()), TEXT("skeletal_mesh"));
	}

	if (SelectedUnit && !HasAnyObjectPathField(Spec, { TEXT("target_unit"), TEXT("unit_path"), TEXT("existing_unit"), TEXT("template_unit"), TEXT("source_unit") }))
	{
		const FString SelectedUnitRole = StringFieldOrDefault(Options, TEXT("selected_unit_role"), TEXT("target")).ToLower();
		if (SelectedUnitRole == TEXT("template") || BoolFieldByNamesOrDefault(Options, { TEXT("use_selected_unit_as_template") }, false))
		{
			Spec->SetStringField(TEXT("template_unit"), SelectedUnit->GetPathName());
			AddIssue(Warnings, TEXT("warning"), TEXT("defaulted_template_unit_from_selection"), FString::Printf(TEXT("template_unit was inferred from selected unit: %s"), *SelectedUnit->GetPathName()), TEXT("template_unit"));
		}
		else if (SelectedUnitRole != TEXT("ignore"))
		{
			Spec->SetStringField(TEXT("target_unit"), SelectedUnit->GetPathName());
			AddIssue(Warnings, TEXT("warning"), TEXT("defaulted_target_unit_from_selection"), FString::Printf(TEXT("target_unit was inferred from selected unit: %s"), *SelectedUnit->GetPathName()), TEXT("target_unit"));
		}
	}

	if (SelectedVatData && !HasAnyObjectPathField(Spec, { TEXT("data_asset"), TEXT("anim_to_texture_data_asset"), TEXT("vat_data_asset") }))
	{
		Spec->SetStringField(TEXT("data_asset"), SelectedVatData->GetPathName());
		AddIssue(Warnings, TEXT("warning"), TEXT("defaulted_vat_data_asset_from_selection"), FString::Printf(TEXT("data_asset was inferred from selection: %s"), *SelectedVatData->GetPathName()), TEXT("data_asset"));
	}

	if (SelectedNiagara && !HasAnyObjectPathField(Spec, { TEXT("niagara_system"), TEXT("niagara"), TEXT("niagara_system_asset") }))
	{
		Spec->SetStringField(TEXT("niagara_system"), SelectedNiagara->GetPathName());
		AddIssue(Warnings, TEXT("warning"), TEXT("defaulted_niagara_system_from_selection"), FString::Printf(TEXT("niagara_system was inferred from selection: %s"), *SelectedNiagara->GetPathName()), TEXT("niagara_system"));
	}

	if (!SelectedRendererClassPath.IsEmpty() && !HasAnyObjectPathField(Spec, { TEXT("source_renderer_class"), TEXT("renderer_template_class"), TEXT("template_renderer_class") }))
	{
		Spec->SetStringField(TEXT("source_renderer_class"), SelectedRendererClassPath);
		AddIssue(Warnings, TEXT("warning"), TEXT("defaulted_source_renderer_class_from_selection"), FString::Printf(TEXT("source_renderer_class was inferred from selection: %s"), *SelectedRendererClassPath), TEXT("source_renderer_class"));
	}

	if (!SelectedMaterials.IsEmpty() && !HasAnyField(Spec, { TEXT("material_overrides") }))
	{
		TArray<TSharedPtr<FJsonValue>> MaterialOverrides;
		for (UMaterialInterface* Material : SelectedMaterials)
		{
			if (!Material)
			{
				continue;
			}
			TSharedPtr<FJsonObject> Override = MakeShared<FJsonObject>();
			Override->SetStringField(TEXT("material"), Material->GetPathName());
			MaterialOverrides.Add(MakeShared<FJsonValueObject>(Override));
		}
		if (!MaterialOverrides.IsEmpty())
		{
			Spec->SetArrayField(TEXT("material_overrides"), MaterialOverrides);
			AddIssue(Warnings, TEXT("warning"), TEXT("defaulted_material_overrides_from_selection"), FString::Printf(TEXT("%d material override(s) were inferred from selection."), MaterialOverrides.Num()), TEXT("material_overrides"));
		}
	}

	if (FoundAnimsHasEntries(SelectedAnims) && !HasAnyField(Spec, { TEXT("animations"), TEXT("found_anims"), TEXT("found_animations"), TEXT("anim_sequences") }))
	{
		TSharedPtr<FJsonObject> AnimsObject = ParseObject(SerializeFoundAnimsLocal(SelectedAnims));
		if (AnimsObject.IsValid())
		{
			Spec->SetObjectField(TEXT("animations"), AnimsObject);
			AddIssue(Warnings, TEXT("warning"), TEXT("defaulted_animations_from_selection"), TEXT("animations were inferred from selected AnimSequence assets. Review state mapping before final art lock."), TEXT("animations"));
		}
	}

	if (!HasAnyField(Spec, { TEXT("vat_sample_rate"), TEXT("sample_rate"), TEXT("VATSampleRate") }))
	{
		Spec->SetNumberField(TEXT("vat_sample_rate"), 24.0);
		AddIssue(Warnings, TEXT("warning"), TEXT("defaulted_vat_sample_rate"), TEXT("vat_sample_rate was not supplied; using 24 Hz sampling. Runtime rendering can interpolate if supported."), TEXT("vat_sample_rate"));
	}

	TSharedPtr<FJsonObject> Root = MakeSuccess();
	Root->SetObjectField(TEXT("spec"), Spec);
	Root->SetArrayField(TEXT("selected_assets"), SelectedAssets);
	Root->SetArrayField(TEXT("warnings"), Warnings);
	return Root;
}

static TArray<FString> CollectAnimationSearchRoots(const TSharedPtr<FJsonObject>& Spec, const TSharedPtr<FJsonObject>& Style, const FString& SkeletalMeshPath)
{
	TArray<FString> Roots;
	AddAnimationRootFields(Spec, Roots);

	const FString MeshPackagePath = PackagePathFromObjectPath(SkeletalMeshPath);
	AddUniqueRoot(Roots, MeshPackagePath);

	const TSharedPtr<FJsonObject>* AuthoringDefaults = nullptr;
	if (Style.IsValid() && Style->TryGetObjectField(TEXT("authoring_defaults"), AuthoringDefaults) && AuthoringDefaults && AuthoringDefaults->IsValid())
	{
		AddAnimationRootFields(*AuthoringDefaults, Roots);
	}

	AddRootArrayField(Style, TEXT("project_scan_roots"), Roots);
	AddRootArrayField(Style, TEXT("default_scan_roots"), Roots);
	if (Roots.IsEmpty())
	{
		AddUniqueRoot(Roots, TEXT("/Game"));
	}
	return Roots;
}

static void AppendWarningsFromObject(TArray<TSharedPtr<FJsonValue>>& TargetWarnings, const TSharedPtr<FJsonObject>& Object, const FString& ArrayFieldName)
{
	const TArray<TSharedPtr<FJsonValue>>* SourceWarnings = nullptr;
	if (Object.IsValid() && Object->TryGetArrayField(ArrayFieldName, SourceWarnings) && SourceWarnings)
	{
		for (const TSharedPtr<FJsonValue>& Warning : *SourceWarnings)
		{
			AddIssueValueUnique(TargetWarnings, Warning);
		}
	}
}

static TSharedPtr<FJsonObject> BuildResolvedCreateVatUnitSpecFromSelection(const TSharedPtr<FJsonObject>& Options)
{
	TSharedPtr<FJsonObject> Selection = BuildCreateVatUnitSpecFromSelection(Options);
	if (!Selection.IsValid())
	{
		return nullptr;
	}

	TArray<TSharedPtr<FJsonValue>> Warnings;
	AppendWarningsFromObject(Warnings, Selection, TEXT("warnings"));

	const TSharedPtr<FJsonObject>* SpecPtr = nullptr;
	TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
	if (Selection->TryGetObjectField(TEXT("spec"), SpecPtr) && SpecPtr && SpecPtr->IsValid())
	{
		Spec = *SpecPtr;
	}

	if (!HasAnyField(Spec, { TEXT("animations"), TEXT("found_anims"), TEXT("found_animations"), TEXT("anim_sequences") }))
	{
		FString SkeletalMeshPath;
		if (TryGetObjectPathSpec(Spec, { TEXT("skeletal_mesh"), TEXT("mesh"), TEXT("source_mesh") }, SkeletalMeshPath) && !SkeletalMeshPath.IsEmpty())
		{
			const FString DiscoverResult = UMassBattleUnitEditorMCPApi::MCP_EditorDiscoverCompatibleAnimations(SkeletalMeshPath, ToJsonString(Spec));
			TSharedPtr<FJsonObject> DiscoverJson = ParseObject(DiscoverResult);
			if (DiscoverJson.IsValid())
			{
				Selection->SetObjectField(TEXT("discover_animations_result"), DiscoverJson);
				AppendWarningsFromObject(Warnings, DiscoverJson, TEXT("warnings"));
				bool bReady = false;
				DiscoverJson->TryGetBoolField(TEXT("ready"), bReady);
				const TSharedPtr<FJsonObject>* FoundAnims = nullptr;
				if (bReady && DiscoverJson->TryGetObjectField(TEXT("found_anims"), FoundAnims) && FoundAnims && FoundAnims->IsValid())
				{
					Spec->SetObjectField(TEXT("animations"), *FoundAnims);
					FString SelectedSearchPath;
					if (!HasAnyObjectPathField(Spec, { TEXT("animation_search_path"), TEXT("animation_search_root"), TEXT("source_search_path"), TEXT("search_path") })
						&& DiscoverJson->TryGetStringField(TEXT("selected_search_path"), SelectedSearchPath)
						&& !SelectedSearchPath.IsEmpty())
					{
						Spec->SetStringField(TEXT("animation_search_path"), SelectedSearchPath);
					}
					AddIssue(Warnings, TEXT("warning"), TEXT("defaulted_animations_from_search_roots"), TEXT("animations were inferred from style/search roots. Review the returned mapping if exact state semantics matter."), TEXT("animations"));
				}
				else
				{
					AddIssue(Warnings, TEXT("warning"), TEXT("no_animation_mapping_from_selection_or_roots"), TEXT("No selected or discoverable compatible animations were mapped. The create workflow may use static fallback unless animations are supplied."), TEXT("animations"));
				}
			}
			else
			{
				Selection->SetStringField(TEXT("discover_animations_error"), DiscoverResult);
				AddIssue(Warnings, TEXT("warning"), TEXT("animation_discovery_invalid_result"), TEXT("Animation discovery returned invalid JSON; continuing with the selected/default spec."), TEXT("animations"));
			}
		}
	}

	Selection->SetObjectField(TEXT("spec"), Spec);
	Selection->SetArrayField(TEXT("warnings"), Warnings);
	return Selection;
}

static bool PackageIsInRoots(const FString& PackageName, const TArray<FString>& Roots)
{
	for (const FString& Root : Roots)
	{
		if (IsPathUnderRoot(PackageName, Root))
		{
			return true;
		}
	}
	return Roots.IsEmpty();
}

static bool LooksLikeContentObjectPath(const FString& Text)
{
	const FString Path = Text.TrimStartAndEnd();
	return Path.Contains(TEXT("."))
		&& (Path.StartsWith(TEXT("/Game/")) || Path.StartsWith(TEXT("/MassBattle/")));
}

static void ExtractObjectPathsFromJsonValue(const TSharedPtr<FJsonValue>& Value, TArray<FString>& OutPaths);

static void ExtractObjectPathsFromJsonObject(const TSharedPtr<FJsonObject>& Object, TArray<FString>& OutPaths)
{
	if (!Object.IsValid())
	{
		return;
	}

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
	{
		ExtractObjectPathsFromJsonValue(Pair.Value, OutPaths);
	}
}

static void ExtractObjectPathsFromJsonValue(const TSharedPtr<FJsonValue>& Value, TArray<FString>& OutPaths)
{
	if (!Value.IsValid())
	{
		return;
	}

	if (Value->Type == EJson::String)
	{
		const FString Text = Value->AsString();
		if (LooksLikeContentObjectPath(Text))
		{
			OutPaths.AddUnique(EnsureObjectPath(Text));
		}
		return;
	}

	if (Value->Type == EJson::Object)
	{
		ExtractObjectPathsFromJsonObject(Value->AsObject(), OutPaths);
		return;
	}

	if (Value->Type == EJson::Array)
	{
		for (const TSharedPtr<FJsonValue>& Child : Value->AsArray())
		{
			ExtractObjectPathsFromJsonValue(Child, OutPaths);
		}
	}
}

static void AddAssetRefFromData(const FAssetData& AssetData, const FString& DiscoveredBy, int32 Depth, TArray<FOrganizeAssetRef>& OutAssets, TSet<FString>& SeenObjectPaths)
{
	const FString ObjectPath = AssetData.GetObjectPathString();
	if (ObjectPath.IsEmpty() || SeenObjectPaths.Contains(ObjectPath))
	{
		return;
	}

	SeenObjectPaths.Add(ObjectPath);
	FOrganizeAssetRef Ref;
	Ref.ObjectPath = ObjectPath;
	Ref.PackageName = AssetData.PackageName.ToString();
	Ref.PackagePath = AssetData.PackagePath.ToString();
	Ref.AssetName = AssetData.AssetName.ToString();
	Ref.ClassPath = AssetData.AssetClassPath.ToString();
	Ref.DiscoveredBy = DiscoveredBy;
	Ref.Depth = Depth;
	OutAssets.Add(Ref);
}

static void CollectAssetsForPackage(IAssetRegistry& Registry, FName PackageName, const FString& DiscoveredBy, int32 Depth, const TArray<FString>& ManagedRoots, bool bAlwaysIncludePackage, TArray<FOrganizeAssetRef>& OutAssets, TSet<FString>& SeenObjectPaths)
{
	const FString PackageString = PackageName.ToString();
	if (!IsSupportedContentPackage(PackageString))
	{
		return;
	}

	if (!bAlwaysIncludePackage && !PackageIsInRoots(PackageString, ManagedRoots))
	{
		return;
	}

	TArray<FAssetData> Assets;
	Registry.GetAssetsByPackageName(PackageName, Assets);
	Assets.Sort([](const FAssetData& A, const FAssetData& B)
	{
		return A.GetObjectPathString() < B.GetObjectPathString();
	});

	for (const FAssetData& AssetData : Assets)
	{
		AddAssetRefFromData(AssetData, DiscoveredBy, Depth, OutAssets, SeenObjectPaths);
	}
}

static void CollectManagedDependenciesRecursive(IAssetRegistry& Registry, FName PackageName, int32 Depth, int32 MaxDepth, const TArray<FString>& ManagedRoots, TArray<FOrganizeAssetRef>& OutAssets, TSet<FName>& VisitedPackages, TSet<FString>& SeenObjectPaths, bool bAlwaysIncludePackage)
{
	if (PackageName.IsNone() || Depth > MaxDepth || VisitedPackages.Contains(PackageName))
	{
		return;
	}

	VisitedPackages.Add(PackageName);
	CollectAssetsForPackage(Registry, PackageName, Depth == 0 ? TEXT("unit") : TEXT("dependency"), Depth, ManagedRoots, bAlwaysIncludePackage, OutAssets, SeenObjectPaths);

	if (Depth >= MaxDepth)
	{
		return;
	}

	TArray<FName> Dependencies;
	TArray<FName> SoftDependencies;
	Registry.GetDependencies(PackageName, Dependencies, UE::AssetRegistry::EDependencyCategory::Package, UE::AssetRegistry::EDependencyQuery::Hard);
	Registry.GetDependencies(PackageName, SoftDependencies, UE::AssetRegistry::EDependencyCategory::Package, UE::AssetRegistry::EDependencyQuery::Soft);
	Dependencies.Append(SoftDependencies);

	for (const FName& Dependency : Dependencies)
	{
		const FString DependencyPackage = Dependency.ToString();
		if (!IsSupportedContentPackage(DependencyPackage) || !PackageIsInRoots(DependencyPackage, ManagedRoots))
		{
			continue;
		}
		CollectManagedDependenciesRecursive(Registry, Dependency, Depth + 1, MaxDepth, ManagedRoots, OutAssets, VisitedPackages, SeenObjectPaths, false);
	}
}

static TArray<FOrganizeAssetRef> CollectLinkedUnitAssets(const FString& UnitPath, const TSharedPtr<FJsonObject>& Options, const TArray<FString>& ManagedRoots, int32 MaxDependencyDepth)
{
	TArray<FString> SeedObjectPaths;
	SeedObjectPaths.AddUnique(EnsureObjectPath(UnitPath));

	TSharedPtr<FJsonObject> UnitGetOptions = MakeShared<FJsonObject>();
	UnitGetOptions->SetStringField(TEXT("detail"), TEXT("full"));
	UnitGetOptions->SetBoolField(TEXT("include_defaults"), true);
	const FString UnitJson = UMassBattleUnitMCPApi::MCP_UnitGet(UnitPath, ToJsonString(UnitGetOptions));
	TSharedPtr<FJsonObject> UnitObject = ParseObject(UnitJson);
	if (UnitObject.IsValid() && JsonObjectFieldBool(UnitObject, TEXT("success"), false))
	{
		ExtractObjectPathsFromJsonObject(UnitObject, SeedObjectPaths);
	}

	const TArray<FString> ExtraAssets = StringArrayField(Options, TEXT("extra_assets"));
	for (const FString& ExtraAsset : ExtraAssets)
	{
		if (!ExtraAsset.IsEmpty())
		{
			SeedObjectPaths.AddUnique(EnsureObjectPath(ExtraAsset));
		}
	}

	IAssetRegistry& Registry = GetAssetRegistry();
	TArray<FOrganizeAssetRef> Assets;
	TSet<FName> VisitedPackages;
	TSet<FString> SeenObjectPaths;
	for (const FString& ObjectPath : SeedObjectPaths)
	{
		const FString PackageName = PackageNameFromObjectPath(ObjectPath);
		if (PackageName.IsEmpty())
		{
			continue;
		}
		CollectManagedDependenciesRecursive(Registry, FName(*PackageName), 0, MaxDependencyDepth, ManagedRoots, Assets, VisitedPackages, SeenObjectPaths, true);
	}

	Assets.Sort([](const FOrganizeAssetRef& A, const FOrganizeAssetRef& B)
	{
		return A.ObjectPath < B.ObjectPath;
	});
	return Assets;
}

static void AddSiblingAssetsBySlug(const FString& PackagePath, const FString& AssetSlug, TArray<FOrganizeAssetRef>& Assets)
{
	if (PackagePath.IsEmpty() || AssetSlug.IsEmpty())
	{
		return;
	}

	TSet<FString> SeenObjectPaths;
	for (const FOrganizeAssetRef& Asset : Assets)
	{
		SeenObjectPaths.Add(Asset.ObjectPath);
	}

	IAssetRegistry& Registry = GetAssetRegistry();
	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*PackagePath));
	Filter.bRecursivePaths = false;

	TArray<FAssetData> SiblingAssets;
	Registry.GetAssets(Filter, SiblingAssets);
	SiblingAssets.Sort([](const FAssetData& A, const FAssetData& B)
	{
		return A.GetObjectPathString() < B.GetObjectPathString();
	});

	const FString LowerSlug = AssetSlug.ToLower();
	for (const FAssetData& AssetData : SiblingAssets)
	{
		const FString LowerAssetName = AssetData.AssetName.ToString().ToLower();
		if (LowerAssetName.Contains(LowerSlug))
		{
			AddAssetRefFromData(AssetData, TEXT("sibling_slug"), 0, Assets, SeenObjectPaths);
		}
	}
}

static FString SanitizeNamePart(const FString& Name)
{
	FString Sanitized = UMassBattleEditorMCPApi::MCP_SanitizeForPath(Name);
	if (Sanitized.IsEmpty())
	{
		return TEXT("Asset");
	}
	return Sanitized;
}

static TSharedPtr<FJsonObject> MakeRenamePreview(const FString& SourcePath, const FString& DestinationPackagePath, const FString& DestinationAssetName, const FString& ReasonTag, bool bAllowPluginContent, TSet<FString>& PlannedDestinations)
{
	TSharedPtr<FJsonObject> Rename = MakeShared<FJsonObject>();
	const FString NormalizedSourcePath = EnsureObjectPath(SourcePath);
	const FString SourcePackageName = PackageNameFromObjectPath(NormalizedSourcePath);
	const FString SourcePackagePath = PackagePathFromPackageName(SourcePackageName);
	const FString DestinationPath = MakeObjectPath(DestinationPackagePath, DestinationAssetName);

	Rename->SetStringField(TEXT("source_path"), NormalizedSourcePath);
	Rename->SetStringField(TEXT("source_package_name"), SourcePackageName);
	Rename->SetStringField(TEXT("source_package_path"), SourcePackagePath);
	Rename->SetStringField(TEXT("destination_package_path"), DestinationPackagePath);
	Rename->SetStringField(TEXT("destination_asset_name"), DestinationAssetName);
	Rename->SetStringField(TEXT("destination_path"), DestinationPath);
	Rename->SetStringField(TEXT("reason"), ReasonTag);

	FString Status = TEXT("would_rename");
	FString BlockReason;
	if (NormalizedSourcePath.IsEmpty() || !AssetExists(NormalizedSourcePath))
	{
		Status = TEXT("blocked_missing_source");
		BlockReason = TEXT("Source asset does not exist or failed to load.");
	}
	else if (NormalizedSourcePath.Equals(DestinationPath, ESearchCase::IgnoreCase))
	{
		Status = TEXT("already_in_place");
		BlockReason = TEXT("Source asset already has the planned name and package path.");
	}
	else if (!bAllowPluginContent && !IsGameContentPackage(SourcePackageName))
	{
		Status = TEXT("blocked_plugin_content");
		BlockReason = TEXT("Preparing plugin content is blocked by default; copy assets into /Game or pass allow_plugin_content=true.");
	}
	else if (AssetExists(DestinationPath))
	{
		Status = TEXT("blocked_conflict");
		BlockReason = TEXT("Destination asset already exists; preparation does not overwrite assets.");
	}
	else if (PlannedDestinations.Contains(DestinationPath))
	{
		Status = TEXT("blocked_duplicate_destination");
		BlockReason = TEXT("Another planned rename has the same destination path.");
	}
	else
	{
		PlannedDestinations.Add(DestinationPath);
	}

	Rename->SetStringField(TEXT("status"), Status);
	if (!BlockReason.IsEmpty())
	{
		Rename->SetStringField(TEXT("block_reason"), BlockReason);
	}
	return Rename;
}

static int32 CountRenameStatus(const TArray<TSharedPtr<FJsonValue>>& Renames, const FString& Status)
{
	int32 Count = 0;
	for (const TSharedPtr<FJsonValue>& Value : Renames)
	{
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			continue;
		}
		FString RenameStatus;
		if (Value->AsObject()->TryGetStringField(TEXT("status"), RenameStatus) && RenameStatus.Equals(Status, ESearchCase::IgnoreCase))
		{
			Count++;
		}
	}
	return Count;
}

static bool HasBlockedRename(const TArray<TSharedPtr<FJsonValue>>& Renames)
{
	for (const TSharedPtr<FJsonValue>& Value : Renames)
	{
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			continue;
		}
		FString Status;
		if (Value->AsObject()->TryGetStringField(TEXT("status"), Status) && Status.StartsWith(TEXT("blocked"), ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

static void AddTextureRenamePreviews(const TSharedPtr<FJsonObject>& TexturesResult, const FString& AssetSlug, const FString& TargetPackagePath, bool bMoveToSourceFolder, bool bAllowPluginContent, TSet<FString>& PlannedDestinations, TArray<TSharedPtr<FJsonValue>>& Renames)
{
	const TArray<TSharedPtr<FJsonValue>>* Textures = nullptr;
	if (!TexturesResult.IsValid() || !TexturesResult->TryGetArrayField(TEXT("textures"), Textures) || !Textures)
	{
		return;
	}

	const TArray<TPair<FString, FString>> TextureFields = {
		{ TEXT("BaseColor"), TEXT("_Color") },
		{ TEXT("Normal"), TEXT("_Normal") },
		{ TEXT("Roughness"), TEXT("_Roughness") },
		{ TEXT("Metallic"), TEXT("_Metallic") },
		{ TEXT("Specular"), TEXT("_Specular") },
		{ TEXT("Emissive"), TEXT("_Emissive") },
		{ TEXT("Opacity"), TEXT("_Opacity") },
		{ TEXT("AO"), TEXT("_AO") },
		{ TEXT("ARM"), TEXT("_ARM") }
	};

	for (const TSharedPtr<FJsonValue>& TextureValue : *Textures)
	{
		if (!TextureValue.IsValid() || TextureValue->Type != EJson::Object)
		{
			continue;
		}

		TSharedPtr<FJsonObject> TextureObject = TextureValue->AsObject();
		const FString SlotName = SanitizeNamePart(StringFieldOrDefault(TextureObject, TEXT("SlotName"), TEXT("Slot")));
		for (const TPair<FString, FString>& TextureField : TextureFields)
		{
			FString SourcePath;
			if (!TextureObject->TryGetStringField(TextureField.Key, SourcePath) || SourcePath.IsEmpty())
			{
				continue;
			}

			const FString SourcePackagePath = PackagePathFromPackageName(PackageNameFromObjectPath(SourcePath));
			const FString DestinationPackagePath = bMoveToSourceFolder ? TargetPackagePath : SourcePackagePath;
			const FString DestinationAssetName = FString::Printf(TEXT("Tex_%s_%s%s"), *AssetSlug, *SlotName, *TextureField.Value);
			Renames.Add(MakeShared<FJsonValueObject>(MakeRenamePreview(SourcePath, DestinationPackagePath, SanitizeNamePart(DestinationAssetName), TEXT("original_texture"), bAllowPluginContent, PlannedDestinations)));
		}
	}
}

static void AddAnimRenamePreviews(const TSharedPtr<FJsonObject>& AnimResult, const FString& AssetSlug, const FString& TargetPackagePath, bool bMoveToSourceFolder, bool bAllowPluginContent, TSet<FString>& PlannedDestinations, TArray<TSharedPtr<FJsonValue>>& Renames)
{
	const TSharedPtr<FJsonObject>* Anims = nullptr;
	if (!AnimResult.IsValid() || !AnimResult->TryGetObjectField(TEXT("anims"), Anims) || !Anims || !Anims->IsValid())
	{
		return;
	}

	const TArray<FString> Categories = { TEXT("Idle"), TEXT("Move"), TEXT("Fall"), TEXT("Appear"), TEXT("Attack"), TEXT("Hit"), TEXT("Death"), TEXT("Other") };
	for (const FString& Category : Categories)
	{
		const TArray<TSharedPtr<FJsonValue>>* AnimArray = nullptr;
		if (!(*Anims)->TryGetArrayField(Category, AnimArray) || !AnimArray)
		{
			continue;
		}

		for (int32 Index = 0; Index < AnimArray->Num(); ++Index)
		{
			const FString SourcePath = (*AnimArray)[Index].IsValid() ? (*AnimArray)[Index]->AsString() : FString();
			if (SourcePath.IsEmpty())
			{
				continue;
			}

			const FString SourcePackagePath = PackagePathFromPackageName(PackageNameFromObjectPath(SourcePath));
			const FString DestinationPackagePath = bMoveToSourceFolder ? TargetPackagePath : SourcePackagePath;
			const FString DestinationAssetName = FString::Printf(TEXT("Anim_%s_%s_%d"), *AssetSlug, *Category, Index);
			Renames.Add(MakeShared<FJsonValueObject>(MakeRenamePreview(SourcePath, DestinationPackagePath, SanitizeNamePart(DestinationAssetName), TEXT("animation_sequence"), bAllowPluginContent, PlannedDestinations)));
		}
	}
}
} // namespace MassBattleUnitEditorMCP

FString UMassBattleUnitEditorMCPApi::MCP_EditorListProfiles(const FString& OptionsJson)
{
	TSharedPtr<FJsonObject> Options = MassBattleUnitEditorMCP::ParseObject(OptionsJson);
	if (!Options.IsValid())
	{
		return MassBattleUnitEditorMCP::MakeErrorJson(TEXT("OptionsJson is not valid JSON"));
	}

	TArray<TSharedPtr<FJsonValue>> Profiles;
	const TArray<TPair<FString, FString>> Types = {
		{ TEXT("style"), MassBattleUnitEditorMCP::GetProfileDir(TEXT("style")) },
		{ TEXT("recipe"), MassBattleUnitEditorMCP::GetProfileDir(TEXT("recipe")) }
	};

	for (const TPair<FString, FString>& Type : Types)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Type.Value, TEXT("*.json"), true, false);
		Files.Sort();
		for (const FString& File : Files)
		{
			Profiles.Add(MakeShared<FJsonValueObject>(MassBattleUnitEditorMCP::ProfileSummaryFromFile(File, Type.Key)));
		}
	}

	TSharedPtr<FJsonObject> Root = MassBattleUnitEditorMCP::MakeSuccess();
	Root->SetArrayField(TEXT("profiles"), Profiles);
	return MassBattleUnitEditorMCP::ToJsonString(Root);
}

FString UMassBattleUnitEditorMCPApi::MCP_EditorGetProfile(const FString& ProfileType, const FString& ProfileId)
{
	const FString File = MassBattleUnitEditorMCP::FindProfileFile(ProfileType, ProfileId);
	if (File.IsEmpty())
	{
		return MassBattleUnitEditorMCP::MakeErrorJson(FString::Printf(TEXT("Profile not found. type=%s id=%s"), *ProfileType, *ProfileId));
	}

	TSharedPtr<FJsonObject> Config = MassBattleUnitEditorMCP::LoadJsonFileObject(File);
	if (!Config.IsValid())
	{
		return MassBattleUnitEditorMCP::MakeErrorJson(FString::Printf(TEXT("Failed to parse profile file: %s"), *File));
	}

	TSharedPtr<FJsonObject> Root = MassBattleUnitEditorMCP::MakeSuccess();
	Root->SetStringField(TEXT("file"), File);
	Root->SetObjectField(TEXT("profile"), Config);
	return MassBattleUnitEditorMCP::ToJsonString(Root);
}

FString UMassBattleUnitEditorMCPApi::MCP_EditorPlanUnitAuthoringWorkflow(const FString& SpecJson)
{
	TSharedPtr<FJsonObject> Spec = MassBattleUnitEditorMCP::ParseObject(SpecJson);
	if (!Spec.IsValid())
	{
		return MassBattleUnitEditorMCP::MakeErrorJson(TEXT("SpecJson is not valid JSON"));
	}

	FString WorkflowId = TEXT("update_existing_unit");
	Spec->TryGetStringField(TEXT("workflow_id"), WorkflowId);
	Spec->TryGetStringField(TEXT("workflow"), WorkflowId);

	FString TargetUnitPath;
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("target_unit"), TEXT("unit_path"), TEXT("existing_unit") }, TargetUnitPath);
	FString TemplateUnitPath;
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("template_unit"), TEXT("source_unit") }, TemplateUnitPath);
	FString SkeletalMeshPath;
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("skeletal_mesh"), TEXT("mesh"), TEXT("source_mesh") }, SkeletalMeshPath);

	const bool bFullWorkflow = WorkflowId.Equals(TEXT("full"), ESearchCase::IgnoreCase)
		|| WorkflowId.Equals(TEXT("full_unit_authoring"), ESearchCase::IgnoreCase);

	bool bIncludePrepare = !SkeletalMeshPath.IsEmpty();
	Spec->TryGetBoolField(TEXT("include_prepare"), bIncludePrepare);
	Spec->TryGetBoolField(TEXT("prepare_asset"), bIncludePrepare);

	bool bIncludeAddAnimations = !TargetUnitPath.IsEmpty();
	Spec->TryGetBoolField(TEXT("include_add_animations"), bIncludeAddAnimations);
	Spec->TryGetBoolField(TEXT("update_animations"), bIncludeAddAnimations);

	bool bIncludeCreateVat = bFullWorkflow || !TemplateUnitPath.IsEmpty();
	Spec->TryGetBoolField(TEXT("include_create_vat"), bIncludeCreateVat);
	Spec->TryGetBoolField(TEXT("create_vat"), bIncludeCreateVat);
	Spec->TryGetBoolField(TEXT("refresh_vat"), bIncludeCreateVat);

	bool bIncludeOrganize = !TargetUnitPath.IsEmpty();
	Spec->TryGetBoolField(TEXT("include_organize"), bIncludeOrganize);
	Spec->TryGetBoolField(TEXT("organize_assets"), bIncludeOrganize);

	TArray<TSharedPtr<FJsonValue>> Steps;
	TArray<TSharedPtr<FJsonValue>> Issues;
	FString ResolvedUnitPath = TargetUnitPath;

	auto AddPlanStep = [&](const FString& Id, const FString& Tool, const FString& Status, const FString& Description, const FString& ResultJson)
	{
		TSharedPtr<FJsonObject> Step = MassBattleUnitEditorMCP::MakeStep(Id, Tool, Status, Description);
		MassBattleUnitEditorMCP::SetStepResult(Step, ResultJson);
		Steps.Add(MakeShared<FJsonValueObject>(Step));
		return Step;
	};

	if (bIncludePrepare)
	{
		const FString PrepareResult = MCP_EditorPlanPreparePurchasedAsset(SpecJson);
		TSharedPtr<FJsonObject> PrepareJson = MassBattleUnitEditorMCP::ParseObject(PrepareResult);
		const bool bPrepareSuccess = PrepareJson.IsValid() && MassBattleUnitEditorMCP::JsonObjectFieldBool(PrepareJson, TEXT("success"), false);
		const bool bPrepareApplicable = bPrepareSuccess && MassBattleUnitEditorMCP::JsonObjectFieldBool(PrepareJson, TEXT("applicable"), false);
		AddPlanStep(TEXT("prepare_purchased_asset"), TEXT("MCP_EditorPlanPreparePurchasedAsset"), bPrepareApplicable ? TEXT("ready") : TEXT("blocked"), TEXT("Discover and prepare purchased source assets before MassBattle authoring."), PrepareResult);
		if (!bPrepareApplicable)
		{
			MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("prepare_not_applicable"), TEXT("Purchased-asset preparation plan is not applicable."));
		}
	}
	else
	{
		MassBattleUnitEditorMCP::AddStep(Steps, TEXT("prepare_purchased_asset"), TEXT("MCP_EditorPlanPreparePurchasedAsset"), TEXT("skipped"), TEXT("include_prepare=false or skeletal_mesh is missing."));
	}

	if (bIncludeCreateVat)
	{
		const FString CreateValidationResult = MCP_EditorValidateCreateVatUnit(SpecJson);
		TSharedPtr<FJsonObject> CreateValidation = MassBattleUnitEditorMCP::ParseObject(CreateValidationResult);
		const bool bCreateSuccess = CreateValidation.IsValid() && MassBattleUnitEditorMCP::JsonObjectFieldBool(CreateValidation, TEXT("success"), false);
		const bool bCreateValid = bCreateSuccess && MassBattleUnitEditorMCP::JsonObjectFieldBool(CreateValidation, TEXT("valid"), false);
		AddPlanStep(TEXT("create_or_refresh_vat_unit"), TEXT("MCP_EditorValidateCreateVatUnit"), bCreateValid ? TEXT("ready") : TEXT("blocked"), TEXT("Validate VAT mesh/material/renderer/unit-data authoring workflow."), CreateValidationResult);
		if (!bCreateValid)
		{
			MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("create_vat_not_valid"), TEXT("VAT unit creation/refresh workflow is not valid."));
		}

		const TSharedPtr<FJsonObject>* CreatePlan = nullptr;
		if (CreateValidation.IsValid() && CreateValidation->TryGetObjectField(TEXT("plan"), CreatePlan) && CreatePlan && CreatePlan->IsValid())
		{
			const TSharedPtr<FJsonObject>* Layout = nullptr;
			if ((*CreatePlan)->TryGetObjectField(TEXT("resolved_layout"), Layout) && Layout && Layout->IsValid())
			{
				FString PlannedUnitPath;
				if ((*Layout)->TryGetStringField(TEXT("unit_path"), PlannedUnitPath) && !PlannedUnitPath.IsEmpty() && ResolvedUnitPath.IsEmpty())
				{
					ResolvedUnitPath = PlannedUnitPath;
				}
			}
		}
	}
	else
	{
		MassBattleUnitEditorMCP::AddStep(Steps, TEXT("create_or_refresh_vat_unit"), TEXT("MCP_EditorValidateCreateVatUnit"), TEXT("skipped"), TEXT("include_create_vat=false."));
	}

	if (bIncludeAddAnimations)
	{
		if (ResolvedUnitPath.IsEmpty())
		{
			MassBattleUnitEditorMCP::AddStep(Steps, TEXT("add_animations_to_unit"), TEXT("MCP_EditorValidateAddAnimationsToUnit"), TEXT("blocked"), TEXT("target_unit is required to plan animation update."));
			MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("missing_target_unit_for_animation"), TEXT("target_unit is required to add animations to an existing unit."), TEXT("target_unit"));
		}
		else
		{
			const FString AnimValidationResult = MCP_EditorValidateAddAnimationsToUnit(ResolvedUnitPath, SpecJson);
			TSharedPtr<FJsonObject> AnimValidation = MassBattleUnitEditorMCP::ParseObject(AnimValidationResult);
			const bool bAnimSuccess = AnimValidation.IsValid() && MassBattleUnitEditorMCP::JsonObjectFieldBool(AnimValidation, TEXT("success"), false);
			const bool bAnimValid = bAnimSuccess && MassBattleUnitEditorMCP::JsonObjectFieldBool(AnimValidation, TEXT("valid"), false);
			AddPlanStep(TEXT("add_animations_to_unit"), TEXT("MCP_EditorValidateAddAnimationsToUnit"), bAnimValid ? TEXT("ready") : TEXT("blocked"), TEXT("Validate AnimShared merge plan for the target unit."), AnimValidationResult);
			if (!bAnimValid)
			{
				MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("animation_update_not_valid"), TEXT("Animation update workflow is not valid."));
			}
		}
	}
	else
	{
		MassBattleUnitEditorMCP::AddStep(Steps, TEXT("add_animations_to_unit"), TEXT("MCP_EditorValidateAddAnimationsToUnit"), TEXT("skipped"), TEXT("include_add_animations=false."));
	}

	if (bIncludeOrganize)
	{
		if (ResolvedUnitPath.IsEmpty())
		{
			MassBattleUnitEditorMCP::AddStep(Steps, TEXT("organize_unit_assets"), TEXT("MCP_EditorPlanOrganizeUnitAssets"), TEXT("blocked"), TEXT("A resolved unit path is required for linked-asset organization."));
			MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("missing_unit_for_organization"), TEXT("A resolved unit path is required for linked-asset organization."));
		}
		else if (!MassBattleUnitEditorMCP::AssetExists(ResolvedUnitPath))
		{
			TSharedPtr<FJsonObject> Step = MassBattleUnitEditorMCP::MakeStep(TEXT("organize_unit_assets"), TEXT("MCP_EditorPlanOrganizeUnitAssets"), TEXT("planned_after_unit_write"), TEXT("Unit does not exist yet; organize after create/apply writes the unit asset."));
			Step->SetStringField(TEXT("unit_path"), ResolvedUnitPath);
			Steps.Add(MakeShared<FJsonValueObject>(Step));
		}
		else
		{
			const FString OrganizePlanResult = MCP_EditorPlanOrganizeUnitAssets(ResolvedUnitPath, SpecJson);
			TSharedPtr<FJsonObject> OrganizePlan = MassBattleUnitEditorMCP::ParseObject(OrganizePlanResult);
			const bool bOrganizeSuccess = OrganizePlan.IsValid() && MassBattleUnitEditorMCP::JsonObjectFieldBool(OrganizePlan, TEXT("success"), false);
			const bool bOrganizeApplicable = bOrganizeSuccess && MassBattleUnitEditorMCP::JsonObjectFieldBool(OrganizePlan, TEXT("applicable"), false);
			AddPlanStep(TEXT("organize_unit_assets"), TEXT("MCP_EditorPlanOrganizeUnitAssets"), bOrganizeApplicable ? TEXT("ready") : TEXT("blocked"), TEXT("Plan linked generated/source asset organization for the resolved unit."), OrganizePlanResult);
			if (!bOrganizeApplicable)
			{
				MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("organization_not_applicable"), TEXT("Unit linked-asset organization plan is not applicable."));
			}
		}
	}
	else
	{
		MassBattleUnitEditorMCP::AddStep(Steps, TEXT("organize_unit_assets"), TEXT("MCP_EditorPlanOrganizeUnitAssets"), TEXT("skipped"), TEXT("include_organize=false."));
	}

	TSharedPtr<FJsonObject> Root = MassBattleUnitEditorMCP::MakeSuccess();
	Root->SetStringField(TEXT("workflow_id"), WorkflowId);
	Root->SetStringField(TEXT("resolved_unit_path"), ResolvedUnitPath);
	Root->SetBoolField(TEXT("include_prepare"), bIncludePrepare);
	Root->SetBoolField(TEXT("include_create_vat"), bIncludeCreateVat);
	Root->SetBoolField(TEXT("include_add_animations"), bIncludeAddAnimations);
	Root->SetBoolField(TEXT("include_organize"), bIncludeOrganize);
	Root->SetBoolField(TEXT("applicable"), !MassBattleUnitEditorMCP::HasErrorIssue(Issues));
	Root->SetArrayField(TEXT("issues"), Issues);
	Root->SetArrayField(TEXT("steps"), Steps);
	return MassBattleUnitEditorMCP::ToJsonString(Root);
}

FString UMassBattleUnitEditorMCPApi::MCP_EditorApplyUnitAuthoringWorkflow(const FString& SpecJson, bool bSaveAssets)
{
	TSharedPtr<FJsonObject> Spec = MassBattleUnitEditorMCP::ParseObject(SpecJson);
	if (!Spec.IsValid())
	{
		return MassBattleUnitEditorMCP::MakeErrorJson(TEXT("SpecJson is not valid JSON"));
	}

	const FString PlanResult = MCP_EditorPlanUnitAuthoringWorkflow(SpecJson);
	TSharedPtr<FJsonObject> Plan = MassBattleUnitEditorMCP::ParseObject(PlanResult);
	if (!Plan.IsValid() || !MassBattleUnitEditorMCP::JsonObjectFieldBool(Plan, TEXT("success"), false))
	{
		return PlanResult;
	}

	bool bDryRun = true;
	Spec->TryGetBoolField(TEXT("dry_run"), bDryRun);
	Spec->TryGetBoolField(TEXT("preview_only"), bDryRun);
	bool bAllowPartial = false;
	Spec->TryGetBoolField(TEXT("allow_partial"), bAllowPartial);

	TSharedPtr<FJsonObject> Root = MassBattleUnitEditorMCP::MakeSuccess();
	Root->SetStringField(TEXT("editor_workflow"), TEXT("MassBattleEditor staged unit authoring workflow apply"));
	Root->SetBoolField(TEXT("dry_run"), bDryRun);
	Root->SetBoolField(TEXT("save_assets"), bSaveAssets);
	Root->SetObjectField(TEXT("plan"), Plan);
	if (bDryRun)
	{
		Root->SetStringField(TEXT("note"), TEXT("dry_run=true; no assets or unit data were modified."));
		return MassBattleUnitEditorMCP::ToJsonString(Root);
	}

	if (!bAllowPartial && !MassBattleUnitEditorMCP::JsonObjectFieldBool(Plan, TEXT("applicable"), false))
	{
		Root->SetBoolField(TEXT("success"), false);
		Root->SetStringField(TEXT("error"), TEXT("Workflow plan is not applicable; inspect plan.issues. Pass allow_partial=true only for deliberate partial execution."));
		return MassBattleUnitEditorMCP::ToJsonString(Root);
	}

	bool bIncludePrepare = false;
	bool bIncludeCreateVat = false;
	bool bIncludeAddAnimations = false;
	bool bIncludeOrganize = false;
	Plan->TryGetBoolField(TEXT("include_prepare"), bIncludePrepare);
	Plan->TryGetBoolField(TEXT("include_create_vat"), bIncludeCreateVat);
	Plan->TryGetBoolField(TEXT("include_add_animations"), bIncludeAddAnimations);
	Plan->TryGetBoolField(TEXT("include_organize"), bIncludeOrganize);
	FString ResolvedUnitPath;
	Plan->TryGetStringField(TEXT("resolved_unit_path"), ResolvedUnitPath);

	Spec->SetBoolField(TEXT("dry_run"), false);
	Spec->SetBoolField(TEXT("preview_only"), false);
	const FString ApplySpecJson = MassBattleUnitEditorMCP::ToJsonString(Spec);

	TArray<TSharedPtr<FJsonValue>> ExecutionSteps;
	auto AddApplyStep = [&](const FString& Id, const FString& Tool, const FString& Description, const FString& ResultJson)
	{
		TSharedPtr<FJsonObject> Result = MassBattleUnitEditorMCP::ParseObject(ResultJson);
		const bool bStepSuccess = Result.IsValid() && MassBattleUnitEditorMCP::JsonObjectFieldBool(Result, TEXT("success"), false);
		TSharedPtr<FJsonObject> Step = MassBattleUnitEditorMCP::MakeStep(Id, Tool, bStepSuccess ? TEXT("done") : TEXT("failed"), Description);
		MassBattleUnitEditorMCP::SetStepResult(Step, ResultJson);
		ExecutionSteps.Add(MakeShared<FJsonValueObject>(Step));
		if (!bStepSuccess)
		{
			Root->SetBoolField(TEXT("success"), false);
		}
		return bStepSuccess;
	};

	if (bIncludePrepare)
	{
		const FString PrepareResult = MCP_EditorApplyPreparePurchasedAsset(ApplySpecJson, bSaveAssets);
		if (!AddApplyStep(TEXT("prepare_purchased_asset"), TEXT("MCP_EditorApplyPreparePurchasedAsset"), TEXT("Prepare purchased source assets."), PrepareResult) && !bAllowPartial)
		{
			Root->SetArrayField(TEXT("execution_steps"), ExecutionSteps);
			return MassBattleUnitEditorMCP::ToJsonString(Root);
		}
	}

	if (bIncludeCreateVat)
	{
		const FString CreateResult = MCP_EditorApplyCreateVatUnit(ApplySpecJson, bSaveAssets);
		if (!AddApplyStep(TEXT("create_or_refresh_vat_unit"), TEXT("MCP_EditorApplyCreateVatUnit"), TEXT("Create or refresh VAT MassBattle unit data."), CreateResult) && !bAllowPartial)
		{
			Root->SetArrayField(TEXT("execution_steps"), ExecutionSteps);
			return MassBattleUnitEditorMCP::ToJsonString(Root);
		}
	}

	if (bIncludeAddAnimations && !ResolvedUnitPath.IsEmpty())
	{
		const FString AnimResult = MCP_EditorApplyAddAnimationsToUnit(ResolvedUnitPath, ApplySpecJson, bSaveAssets);
		if (!AddApplyStep(TEXT("add_animations_to_unit"), TEXT("MCP_EditorApplyAddAnimationsToUnit"), TEXT("Apply AnimShared update to the resolved unit."), AnimResult) && !bAllowPartial)
		{
			Root->SetArrayField(TEXT("execution_steps"), ExecutionSteps);
			return MassBattleUnitEditorMCP::ToJsonString(Root);
		}
	}

	if (bIncludeOrganize && !ResolvedUnitPath.IsEmpty() && MassBattleUnitEditorMCP::AssetExists(ResolvedUnitPath))
	{
		const FString OrganizeResult = MCP_EditorApplyOrganizeUnitAssets(ResolvedUnitPath, ApplySpecJson, bSaveAssets);
		if (!AddApplyStep(TEXT("organize_unit_assets"), TEXT("MCP_EditorApplyOrganizeUnitAssets"), TEXT("Organize linked source/generated assets for the resolved unit."), OrganizeResult) && !bAllowPartial)
		{
			Root->SetArrayField(TEXT("execution_steps"), ExecutionSteps);
			return MassBattleUnitEditorMCP::ToJsonString(Root);
		}
	}

	Root->SetArrayField(TEXT("execution_steps"), ExecutionSteps);
	return MassBattleUnitEditorMCP::ToJsonString(Root);
}

FString UMassBattleUnitEditorMCPApi::MCP_EditorPlanPreparePurchasedAsset(const FString& SpecJson)
{
	TSharedPtr<FJsonObject> Spec = MassBattleUnitEditorMCP::ParseObject(SpecJson);
	if (!Spec.IsValid())
	{
		return MassBattleUnitEditorMCP::MakeErrorJson(TEXT("SpecJson is not valid JSON"));
	}

	FString SkeletalMeshPath;
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("skeletal_mesh"), TEXT("mesh"), TEXT("source_mesh") }, SkeletalMeshPath);
	if (SkeletalMeshPath.IsEmpty())
	{
		return MassBattleUnitEditorMCP::MakeErrorJson(TEXT("skeletal_mesh is required"));
	}

	FString RecipeId = TEXT("prepare_purchased_asset");
	Spec->TryGetStringField(TEXT("recipe_id"), RecipeId);
	TSharedPtr<FJsonObject> Recipe = MassBattleUnitEditorMCP::LoadProfileConfig(TEXT("recipe"), RecipeId);

	FString StyleId = TEXT("default");
	if (Recipe.IsValid())
	{
		Recipe->TryGetStringField(TEXT("style_profile"), StyleId);
	}
	Spec->TryGetStringField(TEXT("style_profile"), StyleId);
	TSharedPtr<FJsonObject> Style = MassBattleUnitEditorMCP::LoadProfileConfig(TEXT("style"), StyleId);
	if (!Style.IsValid())
	{
		return MassBattleUnitEditorMCP::MakeErrorJson(FString::Printf(TEXT("Style profile not found or invalid: %s"), *StyleId));
	}

	const TSharedPtr<FJsonObject>* AuthoringDefaultsPtr = nullptr;
	TSharedPtr<FJsonObject> AuthoringDefaults = MakeShared<FJsonObject>();
	if (Style->TryGetObjectField(TEXT("authoring_defaults"), AuthoringDefaultsPtr) && AuthoringDefaultsPtr && AuthoringDefaultsPtr->IsValid())
	{
		AuthoringDefaults = *AuthoringDefaultsPtr;
	}

	const TSharedPtr<FJsonObject>* OrganizationPtr = nullptr;
	TSharedPtr<FJsonObject> Organization = MakeShared<FJsonObject>();
	if (Style->TryGetObjectField(TEXT("organization"), OrganizationPtr) && OrganizationPtr && OrganizationPtr->IsValid())
	{
		Organization = *OrganizationPtr;
	}

	FString SourcePackagePath = MassBattleUnitEditorMCP::PackagePathFromObjectPath(SkeletalMeshPath);
	FString AssetSlug;
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("unit_name") }, AssetSlug);
	if (AssetSlug.IsEmpty())
	{
		AssetSlug = MassBattleUnitEditorMCP::AssetNameFromObjectPath(SkeletalMeshPath);
	}
	AssetSlug = MassBattleUnitEditorMCP::SanitizeNamePart(AssetSlug);

	FString TextureSearchPath;
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("texture_search_path"), TEXT("source_search_path"), TEXT("search_path") }, TextureSearchPath);
	if (TextureSearchPath.IsEmpty())
	{
		TextureSearchPath = SourcePackagePath;
	}

	FString AnimationSearchPath;
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("animation_search_path"), TEXT("source_search_path"), TEXT("search_path") }, AnimationSearchPath);
	if (AnimationSearchPath.IsEmpty())
	{
		AnimationSearchPath = SourcePackagePath;
	}
	FString TextureNameFilter;
	Spec->TryGetStringField(TEXT("texture_name_filter"), TextureNameFilter);
	Spec->TryGetStringField(TEXT("texture_asset_filter"), TextureNameFilter);
	FString AnimationNameFilter;
	Spec->TryGetStringField(TEXT("animation_name_filter"), AnimationNameFilter);
	Spec->TryGetStringField(TEXT("file_name"), AnimationNameFilter);
	Spec->TryGetStringField(TEXT("anim_asset_filter"), AnimationNameFilter);

	const FString StyleFamily = MassBattleUnitEditorMCP::ResolveStyleFamily(Spec, Organization, AssetSlug + TEXT(" ") + SkeletalMeshPath + TEXT(" ") + SourcePackagePath);
	const FString FamilyFolder = MassBattleUnitEditorMCP::ResolveFamilyFolder(Organization, StyleFamily);
	FString OutputRoot = MassBattleUnitEditorMCP::StringFieldOrDefault(Organization, TEXT("target_root"), TEXT("/Game/Unit/Actor"));
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("output_root"), TEXT("target_root") }, OutputRoot);
	bool bUseFamilyFolder = true;
	Spec->TryGetBoolField(TEXT("use_family_folder"), bUseFamilyFolder);
	if (bUseFamilyFolder && !FamilyFolder.IsEmpty())
	{
		OutputRoot = OutputRoot / FamilyFolder;
	}

	FString SourceFolderName = MassBattleUnitEditorMCP::StringFieldOrDefault(AuthoringDefaults, TEXT("source_folder_name"), TEXT("Source_{unit_name}"));
	Spec->TryGetStringField(TEXT("source_folder_name"), SourceFolderName);
	SourceFolderName.ReplaceInline(TEXT("{unit_name}"), *AssetSlug);
	FString TargetSourcePackagePath = OutputRoot / SourceFolderName;
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("target_source_package_path"), TEXT("prepared_package_path") }, TargetSourcePackagePath);

	bool bMoveToSourceFolder = true;
	Spec->TryGetBoolField(TEXT("move_to_source_folder"), bMoveToSourceFolder);
	bool bAllowPluginContent = false;
	Spec->TryGetBoolField(TEXT("allow_plugin_content"), bAllowPluginContent);
	bool bRenameSkeletalMesh = true;
	Spec->TryGetBoolField(TEXT("rename_skeletal_mesh"), bRenameSkeletalMesh);
	bool bRenameTextures = true;
	Spec->TryGetBoolField(TEXT("rename_textures"), bRenameTextures);
	bool bRenameAnimations = true;
	Spec->TryGetBoolField(TEXT("rename_animations"), bRenameAnimations);

	TArray<TSharedPtr<FJsonValue>> Steps;
	TArray<TSharedPtr<FJsonValue>> Issues;
	TArray<TSharedPtr<FJsonValue>> Renames;
	TSet<FString> PlannedDestinations;
	TSharedPtr<FJsonObject> Discovery = MakeShared<FJsonObject>();

	if (!MassBattleUnitEditorMCP::AssetExists(SkeletalMeshPath))
	{
		MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("skeletal_mesh_not_found"), FString::Printf(TEXT("SkeletalMesh does not exist or failed to load: %s"), *SkeletalMeshPath), TEXT("skeletal_mesh"));
	}

	if (bRenameSkeletalMesh)
	{
		const FString MeshDestinationPackagePath = bMoveToSourceFolder ? TargetSourcePackagePath : SourcePackagePath;
		const FString MeshDestinationAssetName = MassBattleUnitEditorMCP::SanitizeNamePart(TEXT("SKM_") + AssetSlug);
		Renames.Add(MakeShared<FJsonValueObject>(MassBattleUnitEditorMCP::MakeRenamePreview(SkeletalMeshPath, MeshDestinationPackagePath, MeshDestinationAssetName, TEXT("skeletal_mesh"), bAllowPluginContent, PlannedDestinations)));
	}

	if (bRenameTextures)
	{
		const FString TextureResult = UMassBattleEditorMCPApi::MCP_FindAndFillOriginalTextures(SkeletalMeshPath, TextureSearchPath, TextureNameFilter);
		TSharedPtr<FJsonObject> TextureJson = MassBattleUnitEditorMCP::ParseObject(TextureResult);
		if (TextureJson.IsValid() && MassBattleUnitEditorMCP::JsonObjectFieldBool(TextureJson, TEXT("success"), false))
		{
			Discovery->SetObjectField(TEXT("textures"), TextureJson);
			MassBattleUnitEditorMCP::AddTextureRenamePreviews(TextureJson, AssetSlug, TargetSourcePackagePath, bMoveToSourceFolder, bAllowPluginContent, PlannedDestinations, Renames);
			MassBattleUnitEditorMCP::AddStep(Steps, TEXT("find_textures"), TEXT("MCP_FindAndFillOriginalTextures"), TEXT("done"), TEXT("Texture candidates were discovered from the purchased skeletal mesh."));
		}
		else
		{
			Discovery->SetStringField(TEXT("textures_error"), TextureResult);
			MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("warning"), TEXT("texture_discovery_failed"), TEXT("Texture discovery failed; texture rename plan was skipped."), TEXT("texture_search_path"));
			MassBattleUnitEditorMCP::AddStep(Steps, TEXT("find_textures"), TEXT("MCP_FindAndFillOriginalTextures"), TEXT("blocked"), TEXT("Texture discovery failed."));
		}
	}

	if (bRenameAnimations)
	{
		const FString AnimResult = UMassBattleEditorMCPApi::MCP_FindAndFillAnimSequences(SkeletalMeshPath, AnimationSearchPath, AnimationNameFilter);
		TSharedPtr<FJsonObject> AnimJson = MassBattleUnitEditorMCP::ParseObject(AnimResult);
		if (AnimJson.IsValid() && MassBattleUnitEditorMCP::JsonObjectFieldBool(AnimJson, TEXT("success"), false))
		{
			Discovery->SetObjectField(TEXT("animations"), AnimJson);
			MassBattleUnitEditorMCP::AddAnimRenamePreviews(AnimJson, AssetSlug, TargetSourcePackagePath, bMoveToSourceFolder, bAllowPluginContent, PlannedDestinations, Renames);
			if (!MassBattleUnitEditorMCP::FoundAnimsJsonHasEntries(MassBattleUnitEditorMCP::JsonObjectFieldToString(AnimJson, TEXT("anims"))))
			{
				MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("warning"), TEXT("no_animation_sequences_found"), TEXT("No compatible animation sequences were found; animation rename plan is empty."), TEXT("animation_search_path"));
			}
			MassBattleUnitEditorMCP::AddStep(Steps, TEXT("find_animations"), TEXT("MCP_FindAndFillAnimSequences"), TEXT("done"), TEXT("Compatible animation sequences were discovered and categorized."));
		}
		else
		{
			Discovery->SetStringField(TEXT("animations_error"), AnimResult);
			MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("warning"), TEXT("animation_discovery_failed"), TEXT("Animation discovery failed; animation rename plan was skipped."), TEXT("animation_search_path"));
			MassBattleUnitEditorMCP::AddStep(Steps, TEXT("find_animations"), TEXT("MCP_FindAndFillAnimSequences"), TEXT("blocked"), TEXT("Animation discovery failed."));
		}
	}

	const int32 RenameCount = MassBattleUnitEditorMCP::CountRenameStatus(Renames, TEXT("would_rename"));
	const int32 AlreadyInPlaceCount = MassBattleUnitEditorMCP::CountRenameStatus(Renames, TEXT("already_in_place"));
	const bool bHasBlockedRenames = MassBattleUnitEditorMCP::HasBlockedRename(Renames);
	if (bHasBlockedRenames)
	{
		MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("rename_plan_blocked"), TEXT("One or more planned renames are blocked; inspect renames before applying."));
	}

	MassBattleUnitEditorMCP::AddStep(Steps, TEXT("rename_and_move_assets"), TEXT("AssetTools.RenameAssets"), RenameCount > 0 ? TEXT("planned") : TEXT("skipped"), TEXT("Rename and optionally move source assets using official MassBattleEditor naming conventions."));

	TSharedPtr<FJsonObject> Layout = MakeShared<FJsonObject>();
	Layout->SetStringField(TEXT("style_profile"), StyleId);
	Layout->SetStringField(TEXT("recipe_id"), RecipeId);
	Layout->SetStringField(TEXT("style_family"), StyleFamily);
	Layout->SetStringField(TEXT("unit_name"), AssetSlug);
	Layout->SetStringField(TEXT("source_package_path"), SourcePackagePath);
	Layout->SetStringField(TEXT("texture_search_path"), TextureSearchPath);
	Layout->SetStringField(TEXT("texture_name_filter"), TextureNameFilter);
	Layout->SetStringField(TEXT("animation_search_path"), AnimationSearchPath);
	Layout->SetStringField(TEXT("animation_name_filter"), AnimationNameFilter);
	Layout->SetStringField(TEXT("target_source_package_path"), TargetSourcePackagePath);
	Layout->SetBoolField(TEXT("move_to_source_folder"), bMoveToSourceFolder);

	TSharedPtr<FJsonObject> Root = MassBattleUnitEditorMCP::MakeSuccess();
	Root->SetStringField(TEXT("editor_workflow"), TEXT("MassBattleEditor purchased skeletal asset preparation plan"));
	Root->SetBoolField(TEXT("applicable"), !MassBattleUnitEditorMCP::HasErrorIssue(Issues));
	Root->SetObjectField(TEXT("resolved_layout"), Layout);
	Root->SetObjectField(TEXT("discovery"), Discovery);
	Root->SetArrayField(TEXT("renames"), Renames);
	Root->SetArrayField(TEXT("issues"), Issues);
	Root->SetArrayField(TEXT("steps"), Steps);
	Root->SetNumberField(TEXT("rename_count"), RenameCount);
	Root->SetNumberField(TEXT("already_in_place_count"), AlreadyInPlaceCount);
	return MassBattleUnitEditorMCP::ToJsonString(Root);
}

FString UMassBattleUnitEditorMCPApi::MCP_EditorApplyPreparePurchasedAsset(const FString& SpecJson, bool bSaveAssets)
{
	TSharedPtr<FJsonObject> Spec = MassBattleUnitEditorMCP::ParseObject(SpecJson);
	if (!Spec.IsValid())
	{
		return MassBattleUnitEditorMCP::MakeErrorJson(TEXT("SpecJson is not valid JSON"));
	}

	const FString PlanResult = MCP_EditorPlanPreparePurchasedAsset(SpecJson);
	TSharedPtr<FJsonObject> Plan = MassBattleUnitEditorMCP::ParseObject(PlanResult);
	if (!Plan.IsValid() || !MassBattleUnitEditorMCP::JsonObjectFieldBool(Plan, TEXT("success"), false))
	{
		return PlanResult;
	}

	bool bDryRun = true;
	Spec->TryGetBoolField(TEXT("dry_run"), bDryRun);
	Spec->TryGetBoolField(TEXT("preview_only"), bDryRun);

	TSharedPtr<FJsonObject> Root = MassBattleUnitEditorMCP::MakeSuccess();
	Root->SetStringField(TEXT("editor_workflow"), TEXT("MassBattleEditor purchased skeletal asset preparation apply"));
	Root->SetBoolField(TEXT("dry_run"), bDryRun);
	Root->SetBoolField(TEXT("save_assets"), bSaveAssets);
	Root->SetObjectField(TEXT("plan"), Plan);
	if (bDryRun)
	{
		Root->SetStringField(TEXT("note"), TEXT("dry_run=true; no source assets were renamed or moved."));
		return MassBattleUnitEditorMCP::ToJsonString(Root);
	}

	if (!MassBattleUnitEditorMCP::JsonObjectFieldBool(Plan, TEXT("applicable"), false))
	{
		Root->SetBoolField(TEXT("success"), false);
		Root->SetStringField(TEXT("error"), TEXT("Preparation plan is not applicable; inspect plan.issues and plan.renames before applying."));
		return MassBattleUnitEditorMCP::ToJsonString(Root);
	}

	const TArray<TSharedPtr<FJsonValue>>* Renames = nullptr;
	if (!Plan->TryGetArrayField(TEXT("renames"), Renames) || !Renames)
	{
		Root->SetBoolField(TEXT("success"), false);
		Root->SetStringField(TEXT("error"), TEXT("Preparation plan did not contain renames."));
		return MassBattleUnitEditorMCP::ToJsonString(Root);
	}

	TArray<FAssetRenameData> RenameData;
	TArray<FString> DestinationPaths;
	TArray<TSharedPtr<FJsonValue>> ExecutionSteps;
	for (const TSharedPtr<FJsonValue>& RenameValue : *Renames)
	{
		if (!RenameValue.IsValid() || RenameValue->Type != EJson::Object)
		{
			continue;
		}

		TSharedPtr<FJsonObject> Rename = RenameValue->AsObject();
		FString Status;
		Rename->TryGetStringField(TEXT("status"), Status);
		if (!Status.Equals(TEXT("would_rename"), ESearchCase::IgnoreCase))
		{
			continue;
		}

		FString SourcePath;
		FString DestinationPackagePath;
		FString DestinationAssetName;
		FString DestinationPath;
		Rename->TryGetStringField(TEXT("source_path"), SourcePath);
		Rename->TryGetStringField(TEXT("destination_package_path"), DestinationPackagePath);
		Rename->TryGetStringField(TEXT("destination_asset_name"), DestinationAssetName);
		Rename->TryGetStringField(TEXT("destination_path"), DestinationPath);

		UObject* SourceObject = FSoftObjectPath(MassBattleUnitEditorMCP::EnsureObjectPath(SourcePath)).TryLoad();
		TSharedPtr<FJsonObject> Step = MassBattleUnitEditorMCP::MakeStep(TEXT("rename_asset"), TEXT("AssetTools.RenameAssets"), SourceObject ? TEXT("queued") : TEXT("failed"), SourcePath);
		Step->SetStringField(TEXT("source_path"), SourcePath);
		Step->SetStringField(TEXT("destination_path"), DestinationPath);
		if (!SourceObject)
		{
			Step->SetStringField(TEXT("error"), TEXT("Failed to load source asset."));
			ExecutionSteps.Add(MakeShared<FJsonValueObject>(Step));
			Root->SetBoolField(TEXT("success"), false);
			Root->SetArrayField(TEXT("execution_steps"), ExecutionSteps);
			return MassBattleUnitEditorMCP::ToJsonString(Root);
		}

		RenameData.Emplace(SourceObject, DestinationPackagePath, DestinationAssetName);
		DestinationPaths.Add(DestinationPath);
		ExecutionSteps.Add(MakeShared<FJsonValueObject>(Step));
	}

	if (!RenameData.IsEmpty())
	{
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		const bool bRenamed = AssetTools.RenameAssets(RenameData);
		Root->SetBoolField(TEXT("renamed"), bRenamed);
		Root->SetNumberField(TEXT("renamed_count"), bRenamed ? RenameData.Num() : 0);
		if (!bRenamed)
		{
			Root->SetBoolField(TEXT("success"), false);
			Root->SetStringField(TEXT("error"), TEXT("AssetTools.RenameAssets failed."));
			Root->SetArrayField(TEXT("execution_steps"), ExecutionSteps);
			return MassBattleUnitEditorMCP::ToJsonString(Root);
		}

		for (const TSharedPtr<FJsonValue>& StepValue : ExecutionSteps)
		{
			if (StepValue.IsValid() && StepValue->Type == EJson::Object)
			{
				StepValue->AsObject()->SetStringField(TEXT("status"), TEXT("done"));
			}
		}
	}
	else
	{
		Root->SetBoolField(TEXT("renamed"), false);
		Root->SetNumberField(TEXT("renamed_count"), 0);
	}

	if (bSaveAssets)
	{
		TArray<TSharedPtr<FJsonValue>> SaveResults;
		for (const FString& DestinationPath : DestinationPaths)
		{
			FString SaveError;
			TSharedPtr<FJsonObject> SaveResult = MakeShared<FJsonObject>();
			SaveResult->SetStringField(TEXT("path"), DestinationPath);
			const bool bSaved = MassBattleUnitEditorMCP::SaveAssetByPath(DestinationPath, SaveError);
			SaveResult->SetBoolField(TEXT("saved"), bSaved);
			if (!bSaved)
			{
				SaveResult->SetStringField(TEXT("error"), SaveError);
				Root->SetBoolField(TEXT("success"), false);
			}
			SaveResults.Add(MakeShared<FJsonValueObject>(SaveResult));
		}
		Root->SetArrayField(TEXT("save_results"), SaveResults);
	}

	Root->SetArrayField(TEXT("execution_steps"), ExecutionSteps);
	return MassBattleUnitEditorMCP::ToJsonString(Root);
}

FString UMassBattleUnitEditorMCPApi::MCP_EditorDiscoverCompatibleAnimations(const FString& SkeletalMeshPath, const FString& OptionsJson)
{
	TSharedPtr<FJsonObject> Options = MassBattleUnitEditorMCP::ParseObject(OptionsJson);
	if (!Options.IsValid())
	{
		return MassBattleUnitEditorMCP::MakeErrorJson(TEXT("OptionsJson is not valid JSON"));
	}

	if (SkeletalMeshPath.IsEmpty())
	{
		return MassBattleUnitEditorMCP::MakeErrorJson(TEXT("SkeletalMeshPath is required"));
	}

	FString StyleId = TEXT("default");
	Options->TryGetStringField(TEXT("style_profile"), StyleId);
	TSharedPtr<FJsonObject> Style = MassBattleUnitEditorMCP::LoadProfileConfig(TEXT("style"), StyleId);

	FString FileName;
	Options->TryGetStringField(TEXT("file_name"), FileName);
	Options->TryGetStringField(TEXT("animation_name_filter"), FileName);

	bool bIncludeEmptyResults = false;
	Options->TryGetBoolField(TEXT("include_empty_results"), bIncludeEmptyResults);
	bool bIncludeAllAnimPayloads = false;
	Options->TryGetBoolField(TEXT("include_all_animation_payloads"), bIncludeAllAnimPayloads);

	int32 MaxSearchRoots = 12;
	double MaxSearchRootsNumber = 0.0;
	if (Options->TryGetNumberField(TEXT("max_search_roots"), MaxSearchRootsNumber))
	{
		MaxSearchRoots = FMath::Clamp(static_cast<int32>(MaxSearchRootsNumber), 1, 64);
	}

	const TArray<FString> SearchRoots = MassBattleUnitEditorMCP::CollectAnimationSearchRoots(Options, Style, SkeletalMeshPath);
	TArray<TSharedPtr<FJsonValue>> CandidateRoots;
	TArray<TSharedPtr<FJsonValue>> SearchResults;
	TArray<TSharedPtr<FJsonValue>> Warnings;

	TSharedPtr<FJsonObject> SelectedAnims;
	TSharedPtr<FJsonObject> SelectedResult;
	FString SelectedSearchPath;
	int32 SelectedAnimationCount = 0;
	int32 CheckedRootCount = 0;
	int32 TotalAnimationCount = 0;

	for (int32 Index = 0; Index < SearchRoots.Num(); ++Index)
	{
		const FString& SearchRoot = SearchRoots[Index];
		TSharedPtr<FJsonObject> Candidate = MakeShared<FJsonObject>();
		Candidate->SetStringField(TEXT("search_path"), SearchRoot);

		if (Index >= MaxSearchRoots)
		{
			Candidate->SetStringField(TEXT("status"), TEXT("skipped_max_search_roots"));
			CandidateRoots.Add(MakeShared<FJsonValueObject>(Candidate));
			continue;
		}

		++CheckedRootCount;
		const FString FindResult = UMassBattleEditorMCPApi::MCP_FindAndFillAnimSequences(SkeletalMeshPath, SearchRoot, FileName);
		TSharedPtr<FJsonObject> FindJson = MassBattleUnitEditorMCP::ParseObject(FindResult);

		bool bFindSuccess = false;
		if (FindJson.IsValid())
		{
			FindJson->TryGetBoolField(TEXT("success"), bFindSuccess);
		}

		int32 AnimationCount = 0;
		const TSharedPtr<FJsonObject>* FoundAnims = nullptr;
		if (FindJson.IsValid() && FindJson->TryGetObjectField(TEXT("anims"), FoundAnims) && FoundAnims && FoundAnims->IsValid())
		{
			AnimationCount = MassBattleUnitEditorMCP::CountJsonObjectArrayItems(*FoundAnims);
		}
		if (bFindSuccess && AnimationCount == 0 && FindJson.IsValid())
		{
			MassBattleUnitEditorMCP::TryFillAnimDiscoveryWithCompatibleFallback(SkeletalMeshPath, SearchRoot, FileName, FindJson, Warnings);
			FoundAnims = nullptr;
			if (FindJson->TryGetObjectField(TEXT("anims"), FoundAnims) && FoundAnims && FoundAnims->IsValid())
			{
				AnimationCount = MassBattleUnitEditorMCP::CountJsonObjectArrayItems(*FoundAnims);
			}
		}
		TotalAnimationCount += AnimationCount;

		Candidate->SetBoolField(TEXT("checked"), true);
		Candidate->SetBoolField(TEXT("success"), bFindSuccess);
		Candidate->SetNumberField(TEXT("animation_count"), AnimationCount);
		Candidate->SetStringField(TEXT("status"), bFindSuccess ? (AnimationCount > 0 ? TEXT("found") : TEXT("empty")) : TEXT("error"));
		CandidateRoots.Add(MakeShared<FJsonValueObject>(Candidate));

		if (bFindSuccess && AnimationCount > 0 && !SelectedAnims.IsValid() && FoundAnims && FoundAnims->IsValid())
		{
			SelectedAnims = *FoundAnims;
			SelectedResult = FindJson;
			SelectedSearchPath = SearchRoot;
			SelectedAnimationCount = AnimationCount;
		}

		if (bIncludeEmptyResults || AnimationCount > 0 || !bFindSuccess)
		{
			TSharedPtr<FJsonObject> ResultItem = MakeShared<FJsonObject>();
			ResultItem->SetStringField(TEXT("search_path"), SearchRoot);
			ResultItem->SetBoolField(TEXT("success"), bFindSuccess);
			ResultItem->SetNumberField(TEXT("animation_count"), AnimationCount);
			if (FindJson.IsValid())
			{
				FString Error;
				if (FindJson->TryGetStringField(TEXT("error"), Error))
				{
					ResultItem->SetStringField(TEXT("error"), Error);
				}
				if ((AnimationCount > 0 || bIncludeAllAnimPayloads) && FoundAnims && FoundAnims->IsValid())
				{
					ResultItem->SetObjectField(TEXT("anims"), *FoundAnims);
				}
			}
			else
			{
				ResultItem->SetStringField(TEXT("error"), TEXT("FindAndFillAnimSequences returned invalid JSON."));
			}
			SearchResults.Add(MakeShared<FJsonValueObject>(ResultItem));
		}
	}

	if (!SelectedAnims.IsValid())
	{
		MassBattleUnitEditorMCP::AddIssueUnique(Warnings, TEXT("warning"), TEXT("no_compatible_animation_sequences_found"), FString::Printf(TEXT("No compatible AnimSequence assets were found in %d checked animation search root(s). The unit can still be created, but VAT animation needs explicit animations later."), CheckedRootCount), TEXT("animations"));
	}

	TSharedPtr<FJsonObject> Root = MassBattleUnitEditorMCP::MakeSuccess();
	Root->SetStringField(TEXT("skeletal_mesh"), SkeletalMeshPath);
	Root->SetStringField(TEXT("style_profile"), StyleId);
	Root->SetStringField(TEXT("file_name"), FileName);
	Root->SetBoolField(TEXT("ready"), SelectedAnims.IsValid());
	Root->SetStringField(TEXT("selected_search_path"), SelectedSearchPath);
	Root->SetNumberField(TEXT("selected_animation_count"), SelectedAnimationCount);
	Root->SetNumberField(TEXT("total_animation_count"), TotalAnimationCount);
	Root->SetNumberField(TEXT("checked_root_count"), CheckedRootCount);
	Root->SetNumberField(TEXT("candidate_root_count"), SearchRoots.Num());
	Root->SetArrayField(TEXT("candidate_roots"), CandidateRoots);
	Root->SetArrayField(TEXT("results"), SearchResults);
	Root->SetArrayField(TEXT("warnings"), Warnings);
	Root->SetObjectField(TEXT("found_anims"), SelectedAnims.IsValid() ? SelectedAnims : MassBattleUnitEditorMCP::MakeEmptyFoundAnimsObject());
	if (SelectedResult.IsValid())
	{
		Root->SetObjectField(TEXT("selected_result"), SelectedResult);
	}
	return MassBattleUnitEditorMCP::ToJsonString(Root);
}

FString UMassBattleUnitEditorMCPApi::MCP_EditorPlanAddAnimationsToUnit(const FString& UnitPath, const FString& SpecJson)
{
	TSharedPtr<FJsonObject> Spec = MassBattleUnitEditorMCP::ParseObject(SpecJson);
	if (!Spec.IsValid())
	{
		return MassBattleUnitEditorMCP::MakeErrorJson(TEXT("SpecJson is not valid JSON"));
	}

	TSharedPtr<FJsonObject> Root = MassBattleUnitEditorMCP::MakeSuccess();
	Root->SetStringField(TEXT("unit_path"), UnitPath);
	Root->SetStringField(TEXT("editor_workflow"), TEXT("MassBattleEditor.FindAndFillAnimSequences -> MassBattleEditor.CreateAnimsDataFromSequences -> UnitPlanMergeUpdate"));

	FString FoundAnimsJson = MassBattleUnitEditorMCP::JsonObjectFieldToString(Spec, TEXT("found_anims"));
	if (FoundAnimsJson.IsEmpty())
	{
		FString SkeletalMeshPath;
		MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("skeletal_mesh"), TEXT("mesh"), TEXT("target_mesh") }, SkeletalMeshPath);

		if (!SkeletalMeshPath.IsEmpty())
		{
			const FString DiscoverResult = MCP_EditorDiscoverCompatibleAnimations(SkeletalMeshPath, SpecJson);
			TSharedPtr<FJsonObject> DiscoverJson = MassBattleUnitEditorMCP::ParseObject(DiscoverResult);
			if (!DiscoverJson.IsValid() || !DiscoverJson->GetBoolField(TEXT("success")))
			{
				return DiscoverResult;
			}
			Root->SetObjectField(TEXT("discover_animations_result"), DiscoverJson);

			const TSharedPtr<FJsonObject>* SelectedResult = nullptr;
			if (DiscoverJson->TryGetObjectField(TEXT("selected_result"), SelectedResult) && SelectedResult && SelectedResult->IsValid())
			{
				Root->SetObjectField(TEXT("find_animations_result"), *SelectedResult);
			}
			FoundAnimsJson = MassBattleUnitEditorMCP::JsonObjectFieldToString(DiscoverJson, TEXT("found_anims"));
		}
	}

	if (FoundAnimsJson.IsEmpty())
	{
		Root->SetBoolField(TEXT("ready_to_plan"), false);
		Root->SetStringField(TEXT("missing"), TEXT("found_anims or skeletal_mesh + animation_search_path"));
		return MassBattleUnitEditorMCP::ToJsonString(Root);
	}

	bool bAllowEmptyAnims = false;
	Spec->TryGetBoolField(TEXT("allow_empty_anims"), bAllowEmptyAnims);
	if (!bAllowEmptyAnims && !MassBattleUnitEditorMCP::FoundAnimsJsonHasEntries(FoundAnimsJson))
	{
		Root->SetBoolField(TEXT("ready_to_plan"), false);
		Root->SetStringField(TEXT("missing"), TEXT("non-empty found_anims"));
		Root->SetStringField(TEXT("reason"), TEXT("No animation sequences were found. Empty animation sets are blocked by default to avoid overwriting AnimShared with invalid -1 slots."));
		TSharedPtr<FJsonObject> FoundAnims = MassBattleUnitEditorMCP::ParseObject(FoundAnimsJson);
		if (FoundAnims.IsValid())
		{
			Root->SetObjectField(TEXT("found_anims"), FoundAnims);
		}
		return MassBattleUnitEditorMCP::ToJsonString(Root);
	}

	FString DataAssetPath;
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("data_asset"), TEXT("anim_to_texture_data_asset"), TEXT("vat_data_asset") }, DataAssetPath);
	if (DataAssetPath.IsEmpty())
	{
		Root->SetBoolField(TEXT("ready_to_plan"), false);
		Root->SetStringField(TEXT("missing"), TEXT("data_asset / anim_to_texture_data_asset"));
		TSharedPtr<FJsonObject> FoundAnims = MassBattleUnitEditorMCP::ParseObject(FoundAnimsJson);
		if (FoundAnims.IsValid())
		{
			Root->SetObjectField(TEXT("found_anims"), FoundAnims);
		}
		return MassBattleUnitEditorMCP::ToJsonString(Root);
	}

	const FString AnimsDataResult = UMassBattleEditorMCPApi::MCP_CreateAnimsDataFromSequences(DataAssetPath, FoundAnimsJson);
	TSharedPtr<FJsonObject> AnimsDataJson = MassBattleUnitEditorMCP::ParseObject(AnimsDataResult);
	if (!AnimsDataJson.IsValid() || !AnimsDataJson->GetBoolField(TEXT("success")))
	{
		return AnimsDataResult;
	}
	Root->SetObjectField(TEXT("create_anims_data_result"), AnimsDataJson);

	bool bAllowUnresolvedAnimationData = false;
	Spec->TryGetBoolField(TEXT("allow_unresolved_animation_data"), bAllowUnresolvedAnimationData);
	bool bHasResolvedAnimationData = true;
	if (AnimsDataJson->TryGetBoolField(TEXT("has_resolved_animation_data"), bHasResolvedAnimationData) && !bHasResolvedAnimationData && !bAllowUnresolvedAnimationData)
	{
		Root->SetBoolField(TEXT("ready_to_plan"), false);
		Root->SetStringField(TEXT("missing"), TEXT("anim_to_texture_data_asset containing selected animations"));
		Root->SetStringField(TEXT("reason"), TEXT("Compatible animation sequences were found, but none of them are included in the provided AnimToTextureDataAsset. Refresh or recreate the VAT data before merging AnimShared."));
		return MassBattleUnitEditorMCP::ToJsonString(Root);
	}

	const TSharedPtr<FJsonObject>* AnimsData = nullptr;
	if (!AnimsDataJson->TryGetObjectField(TEXT("anims_data"), AnimsData) || !AnimsData || !AnimsData->IsValid())
	{
		return MassBattleUnitEditorMCP::MakeErrorJson(TEXT("CreateAnimsDataFromSequences did not return anims_data"));
	}

	TSharedPtr<FJsonObject> MergePatch = MassBattleUnitEditorMCP::BuildAnimMergePatch(Spec, *AnimsData);
	const FString MergePlanResult = UMassBattleUnitMCPApi::MCP_UnitPlanMergeUpdate(UnitPath, MassBattleUnitEditorMCP::ToJsonString(MergePatch));
	TSharedPtr<FJsonObject> MergePlanJson = MassBattleUnitEditorMCP::ParseObject(MergePlanResult);
	if (!MergePlanJson.IsValid() || !MergePlanJson->GetBoolField(TEXT("success")))
	{
		return MergePlanResult;
	}

	bool bApplicable = false;
	MergePlanJson->TryGetBoolField(TEXT("applicable"), bApplicable);
	FString PlanId;
	MergePlanJson->TryGetStringField(TEXT("plan_id"), PlanId);
	Root->SetBoolField(TEXT("ready_to_plan"), true);
	Root->SetBoolField(TEXT("applicable"), bApplicable);
	Root->SetStringField(TEXT("plan_id"), PlanId);
	Root->SetObjectField(TEXT("merge_patch"), MergePatch);
	Root->SetObjectField(TEXT("merge_plan"), MergePlanJson);
	return MassBattleUnitEditorMCP::ToJsonString(Root);
}

FString UMassBattleUnitEditorMCPApi::MCP_EditorValidateAddAnimationsToUnit(const FString& UnitPath, const FString& SpecJson)
{
	const FString PlanResult = MCP_EditorPlanAddAnimationsToUnit(UnitPath, SpecJson);
	TSharedPtr<FJsonObject> Plan = MassBattleUnitEditorMCP::ParseObject(PlanResult);
	if (!Plan.IsValid())
	{
		return MassBattleUnitEditorMCP::MakeErrorJson(TEXT("MCP_EditorPlanAddAnimationsToUnit returned invalid JSON"));
	}

	TArray<TSharedPtr<FJsonValue>> Issues;
	bool bPlanSuccess = false;
	Plan->TryGetBoolField(TEXT("success"), bPlanSuccess);
	if (!bPlanSuccess)
	{
		FString Error;
		Plan->TryGetStringField(TEXT("error"), Error);
		MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("plan_failed"), Error.IsEmpty() ? TEXT("Animation edit plan failed.") : Error);
	}

	bool bReadyToPlan = false;
	if (!Plan->TryGetBoolField(TEXT("ready_to_plan"), bReadyToPlan))
	{
		bReadyToPlan = bPlanSuccess && Plan->HasField(TEXT("plan_id"));
	}
	if (!bReadyToPlan)
	{
		FString Missing;
		FString Reason;
		Plan->TryGetStringField(TEXT("missing"), Missing);
		Plan->TryGetStringField(TEXT("reason"), Reason);
		MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("not_ready_to_plan"), Reason.IsEmpty() ? TEXT("Animation edit is not ready to produce a merge plan.") : Reason, Missing);
	}

	bool bApplicable = false;
	if (Plan->TryGetBoolField(TEXT("applicable"), bApplicable) && !bApplicable)
	{
		MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("merge_plan_not_applicable"), TEXT("Generated animation merge plan is not applicable."));
	}

	TSharedPtr<FJsonObject> Root = MassBattleUnitEditorMCP::MakeSuccess();
	Root->SetStringField(TEXT("validation_type"), TEXT("add_animations_to_unit"));
	Root->SetStringField(TEXT("unit_path"), UnitPath);
	Root->SetBoolField(TEXT("valid"), !MassBattleUnitEditorMCP::HasErrorIssue(Issues));
	Root->SetArrayField(TEXT("issues"), Issues);
	Root->SetObjectField(TEXT("plan_result"), Plan);
	return MassBattleUnitEditorMCP::ToJsonString(Root);
}

FString UMassBattleUnitEditorMCPApi::MCP_EditorApplyAddAnimationsToUnit(const FString& UnitPath, const FString& SpecJson, bool bSaveAssets)
{
	const FString PlanResult = MCP_EditorPlanAddAnimationsToUnit(UnitPath, SpecJson);
	TSharedPtr<FJsonObject> PlanJson = MassBattleUnitEditorMCP::ParseObject(PlanResult);
	if (!PlanJson.IsValid() || !PlanJson->GetBoolField(TEXT("success")))
	{
		return PlanResult;
	}

	bool bApplicable = false;
	PlanJson->TryGetBoolField(TEXT("applicable"), bApplicable);
	if (!bApplicable)
	{
		return PlanResult;
	}

	FString PlanId;
	PlanJson->TryGetStringField(TEXT("plan_id"), PlanId);
	if (PlanId.IsEmpty())
	{
		return PlanResult;
	}
	return UMassBattleUnitMCPApi::MCP_UnitApplyPlan(PlanId, bSaveAssets);
}

FString UMassBattleUnitEditorMCPApi::MCP_EditorPlanCreateVatUnitFromSelection(const FString& OptionsJson)
{
	TSharedPtr<FJsonObject> Options = MassBattleUnitEditorMCP::ParseObject(OptionsJson);
	if (!Options.IsValid())
	{
		return MassBattleUnitEditorMCP::MakeErrorJson(TEXT("OptionsJson is not valid JSON"));
	}

	TSharedPtr<FJsonObject> Selection = MassBattleUnitEditorMCP::BuildResolvedCreateVatUnitSpecFromSelection(Options);
	if (!Selection.IsValid())
	{
		return MassBattleUnitEditorMCP::MakeErrorJson(TEXT("Failed to build create VAT unit spec from selection."));
	}

	const TSharedPtr<FJsonObject>* SpecPtr = nullptr;
	if (!Selection->TryGetObjectField(TEXT("spec"), SpecPtr) || !SpecPtr || !SpecPtr->IsValid())
	{
		return MassBattleUnitEditorMCP::MakeErrorJson(TEXT("Selection resolver did not produce a spec."));
	}

	const FString PlanResult = MCP_EditorPlanCreateVatUnit(MassBattleUnitEditorMCP::ToJsonString(*SpecPtr));
	TSharedPtr<FJsonObject> PlanJson = MassBattleUnitEditorMCP::ParseObject(PlanResult);

	TSharedPtr<FJsonObject> Root = MassBattleUnitEditorMCP::MakeSuccess();
	Root->SetStringField(TEXT("editor_workflow"), TEXT("MassBattleEditor selection -> VAT skeletal unit authoring plan"));
	Root->SetObjectField(TEXT("selection"), Selection);
	Root->SetObjectField(TEXT("resolved_spec"), *SpecPtr);
	if (PlanJson.IsValid())
	{
		Root->SetObjectField(TEXT("create_plan"), PlanJson);
		Root->SetBoolField(TEXT("ready_to_apply"), MassBattleUnitEditorMCP::JsonObjectFieldBool(PlanJson, TEXT("applicable"), false));
	}
	else
	{
		Root->SetBoolField(TEXT("success"), false);
		Root->SetStringField(TEXT("error"), TEXT("MCP_EditorPlanCreateVatUnit returned invalid JSON."));
		Root->SetStringField(TEXT("create_plan_raw"), PlanResult);
	}
	return MassBattleUnitEditorMCP::ToJsonString(Root);
}

FString UMassBattleUnitEditorMCPApi::MCP_EditorApplyCreateVatUnitFromSelection(const FString& OptionsJson, bool bSaveAssets)
{
	TSharedPtr<FJsonObject> Options = MassBattleUnitEditorMCP::ParseObject(OptionsJson);
	if (!Options.IsValid())
	{
		return MassBattleUnitEditorMCP::MakeErrorJson(TEXT("OptionsJson is not valid JSON"));
	}

	TSharedPtr<FJsonObject> Selection = MassBattleUnitEditorMCP::BuildResolvedCreateVatUnitSpecFromSelection(Options);
	if (!Selection.IsValid())
	{
		return MassBattleUnitEditorMCP::MakeErrorJson(TEXT("Failed to build create VAT unit spec from selection."));
	}

	const TSharedPtr<FJsonObject>* SpecPtr = nullptr;
	if (!Selection->TryGetObjectField(TEXT("spec"), SpecPtr) || !SpecPtr || !SpecPtr->IsValid())
	{
		return MassBattleUnitEditorMCP::MakeErrorJson(TEXT("Selection resolver did not produce a spec."));
	}

	const bool bCompactResponse = MassBattleUnitEditorMCP::BoolFieldByNamesOrDefault(Options, { TEXT("compact_response") }, false);
	const FString ApplyResult = MCP_EditorApplyCreateVatUnit(MassBattleUnitEditorMCP::ToJsonString(*SpecPtr), bSaveAssets);
	TSharedPtr<FJsonObject> ApplyJson = MassBattleUnitEditorMCP::ParseObject(ApplyResult);

	TSharedPtr<FJsonObject> Root = MassBattleUnitEditorMCP::MakeSuccess();
	Root->SetStringField(TEXT("editor_workflow"), TEXT("MassBattleEditor selection -> VAT skeletal unit authoring apply"));
	Root->SetBoolField(TEXT("save_assets"), bSaveAssets);
	Root->SetBoolField(TEXT("compact_response"), bCompactResponse);
	if (ApplyJson.IsValid())
	{
		Root->SetBoolField(TEXT("success"), MassBattleUnitEditorMCP::JsonObjectFieldBool(ApplyJson, TEXT("success"), false));
		if (bCompactResponse)
		{
			MassBattleUnitEditorMCP::CopyArrayField(Selection, Root, TEXT("selected_assets"));
			MassBattleUnitEditorMCP::CopyArrayField(Selection, Root, TEXT("warnings"));
			Root->SetObjectField(TEXT("apply_summary"), MassBattleUnitEditorMCP::MakeCompactCreateVatApplySummary(ApplyJson));
		}
		else
		{
			Root->SetObjectField(TEXT("selection"), Selection);
			Root->SetObjectField(TEXT("resolved_spec"), *SpecPtr);
			Root->SetObjectField(TEXT("apply_result"), ApplyJson);
		}
	}
	else
	{
		Root->SetBoolField(TEXT("success"), false);
		Root->SetStringField(TEXT("error"), TEXT("MCP_EditorApplyCreateVatUnit returned invalid JSON."));
		Root->SetStringField(TEXT("apply_result_raw"), ApplyResult);
	}
	return MassBattleUnitEditorMCP::ToJsonString(Root);
}

FString UMassBattleUnitEditorMCPApi::MCP_EditorPlanCreateVatUnit(const FString& SpecJson)
{
	TSharedPtr<FJsonObject> Spec = MassBattleUnitEditorMCP::ParseObject(SpecJson);
	if (!Spec.IsValid())
	{
		return MassBattleUnitEditorMCP::MakeErrorJson(TEXT("SpecJson is not valid JSON"));
	}

	FString RecipeId = TEXT("vat_skeletal_unit");
	Spec->TryGetStringField(TEXT("recipe_id"), RecipeId);
	TSharedPtr<FJsonObject> Recipe = MassBattleUnitEditorMCP::LoadProfileConfig(TEXT("recipe"), RecipeId);
	if (!Recipe.IsValid())
	{
		return MassBattleUnitEditorMCP::MakeErrorJson(FString::Printf(TEXT("Recipe not found or invalid: %s"), *RecipeId));
	}

	FString StyleId = TEXT("default");
	Recipe->TryGetStringField(TEXT("style_profile"), StyleId);
	Spec->TryGetStringField(TEXT("style_profile"), StyleId);
	TSharedPtr<FJsonObject> Style = MassBattleUnitEditorMCP::LoadProfileConfig(TEXT("style"), StyleId);

	const TSharedPtr<FJsonObject>* AuthoringDefaultsPtr = nullptr;
	TSharedPtr<FJsonObject> AuthoringDefaults = MakeShared<FJsonObject>();
	if (Style.IsValid() && Style->TryGetObjectField(TEXT("authoring_defaults"), AuthoringDefaultsPtr) && AuthoringDefaultsPtr && AuthoringDefaultsPtr->IsValid())
	{
		AuthoringDefaults = *AuthoringDefaultsPtr;
	}

	const TSharedPtr<FJsonObject>* OrganizationPtr = nullptr;
	TSharedPtr<FJsonObject> Organization = MakeShared<FJsonObject>();
	if (Style.IsValid() && Style->TryGetObjectField(TEXT("organization"), OrganizationPtr) && OrganizationPtr && OrganizationPtr->IsValid())
	{
		Organization = *OrganizationPtr;
	}

	FString SkeletalMeshPath;
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("skeletal_mesh"), TEXT("mesh"), TEXT("source_mesh") }, SkeletalMeshPath);

	FString AssetSlug;
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("unit_name") }, AssetSlug);
	if (AssetSlug.IsEmpty() && !SkeletalMeshPath.IsEmpty())
	{
		FString PackagePath;
		FString AssetName;
		if (SkeletalMeshPath.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
		{
			FString ObjectName;
			if (!AssetName.Split(TEXT("."), &AssetSlug, &ObjectName))
			{
				AssetSlug = AssetName;
			}
		}
	}
	AssetSlug = UMassBattleEditorMCPApi::MCP_SanitizeForPath(AssetSlug);
	if (AssetSlug.IsEmpty())
	{
		AssetSlug = TEXT("Unit");
	}

	FString SourcePackagePath = MassBattleUnitEditorMCP::PackagePathFromObjectPath(SkeletalMeshPath);
	FString TextureSearchPath;
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("texture_search_path"), TEXT("source_search_path"), TEXT("search_path") }, TextureSearchPath);
	if (TextureSearchPath.IsEmpty())
	{
		TextureSearchPath = SourcePackagePath;
	}
	FString AnimationSearchPath;
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("animation_search_path"), TEXT("source_search_path"), TEXT("search_path") }, AnimationSearchPath);
	if (AnimationSearchPath.IsEmpty())
	{
		AnimationSearchPath = SourcePackagePath;
	}
	TArray<TSharedPtr<FJsonValue>> Warnings;
	FString AnimationNameFilter;
	bool bAnimationNameFilterSupplied = false;
	if (Spec->HasField(TEXT("animation_name_filter")))
	{
		Spec->TryGetStringField(TEXT("animation_name_filter"), AnimationNameFilter);
		bAnimationNameFilterSupplied = true;
	}
	else if (Spec->HasField(TEXT("file_name")))
	{
		Spec->TryGetStringField(TEXT("file_name"), AnimationNameFilter);
		bAnimationNameFilterSupplied = true;
	}
	if (!bAnimationNameFilterSupplied)
	{
		MassBattleUnitEditorMCP::AddIssue(Warnings, TEXT("warning"), TEXT("defaulted_animation_name_filter"), TEXT("animation_name_filter was not supplied; using an empty filter so all same-skeleton AnimSequence assets in animation_search_path can be considered."), TEXT("animation_name_filter"));
	}

	const FString StyleFamily = MassBattleUnitEditorMCP::ResolveStyleFamily(Spec, Organization, AssetSlug + TEXT(" ") + SkeletalMeshPath + TEXT(" ") + SourcePackagePath);
	const FString FamilyFolder = MassBattleUnitEditorMCP::ResolveFamilyFolder(Organization, StyleFamily);

	FString OutputRoot = MassBattleUnitEditorMCP::StringFieldOrDefault(Organization, TEXT("target_root"), TEXT("/Game/Unit/Actor"));
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("output_root"), TEXT("generated_root") }, OutputRoot);
	bool bUseFamilyFolder = true;
	Spec->TryGetBoolField(TEXT("use_family_folder"), bUseFamilyFolder);
	if (bUseFamilyFolder && !FamilyFolder.IsEmpty())
	{
		OutputRoot = OutputRoot / FamilyFolder;
	}

	FString GeneratedFolderName = MassBattleUnitEditorMCP::StringFieldOrDefault(AuthoringDefaults, TEXT("generated_folder_name"), TEXT("Gen_{unit_name}"));
	GeneratedFolderName.ReplaceInline(TEXT("{unit_name}"), *AssetSlug);

	FString GeneratedPackagePath = OutputRoot / GeneratedFolderName;
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("target_package_path") }, GeneratedPackagePath);

	const FString StaticMeshName = MassBattleUnitEditorMCP::ResolveAssetName(Spec, AuthoringDefaults, TEXT("static_mesh_name"), TEXT("static_mesh_prefix"), TEXT("SM_"), AssetSlug);
	const FString UnitAssetName = MassBattleUnitEditorMCP::ResolveAssetName(Spec, AuthoringDefaults, TEXT("unit_asset_name"), TEXT("unit_asset_prefix"), TEXT("AgentConfig_"), AssetSlug);
	const FString RendererAssetName = MassBattleUnitEditorMCP::ResolveAssetName(Spec, AuthoringDefaults, TEXT("renderer_asset_name"), TEXT("renderer_prefix"), TEXT("Renderer_"), AssetSlug);
	const FString VatDataName = MassBattleUnitEditorMCP::ResolveAssetName(Spec, AuthoringDefaults, TEXT("vat_data_name"), TEXT("vat_data_prefix"), TEXT("VAT_"), AssetSlug);
	const FString MaterialName = MassBattleUnitEditorMCP::ResolveAssetName(Spec, AuthoringDefaults, TEXT("material_asset_name"), TEXT("material_prefix"), TEXT(""), AssetSlug);

	FString UnitPackagePath = GeneratedPackagePath;
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("target_unit_package_path") }, UnitPackagePath);
	FString RendererPackagePath = GeneratedPackagePath;
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("renderer_package_path") }, RendererPackagePath);

	FString PlannedRendererClassPath;
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("renderer_class"), TEXT("generated_renderer_class"), TEXT("new_renderer_class") }, PlannedRendererClassPath);
	if (PlannedRendererClassPath.IsEmpty())
	{
		PlannedRendererClassPath = MassBattleUnitEditorMCP::MakeGeneratedClassPath(RendererPackagePath, RendererAssetName);
	}
	else
	{
		PlannedRendererClassPath = MassBattleUnitEditorMCP::EnsureGeneratedClassPath(PlannedRendererClassPath);
	}

	FString ParentMaterialPath = MassBattleUnitEditorMCP::ResolvePathFromSpecOrDefault(
		Spec,
		{ TEXT("parent_material") },
		AuthoringDefaults,
		{ TEXT("default_parent_material"), TEXT("parent_material") },
		MassBattleUnitEditorMCP::DefaultVatParentMaterialPath,
		TEXT("defaulted_parent_material"),
		TEXT("parent_material"),
		Warnings);
	FString SourceRendererClassPath = MassBattleUnitEditorMCP::ResolvePathFromSpecOrDefault(
		Spec,
		{ TEXT("source_renderer_class"), TEXT("renderer_template_class"), TEXT("template_renderer_class") },
		AuthoringDefaults,
		{ TEXT("default_source_renderer_class"), TEXT("source_renderer_class"), TEXT("renderer_template_class") },
		MassBattleUnitEditorMCP::DefaultAgentRendererClassPath,
		TEXT("defaulted_source_renderer_class"),
		TEXT("source_renderer_class"),
		Warnings);
	SourceRendererClassPath = MassBattleUnitEditorMCP::EnsureGeneratedClassPath(SourceRendererClassPath);
	FString NiagaraSystemPath = MassBattleUnitEditorMCP::ResolvePathFromSpecOrDefault(
		Spec,
		{ TEXT("niagara_system"), TEXT("niagara"), TEXT("niagara_system_asset") },
		AuthoringDefaults,
		{ TEXT("default_niagara_system"), TEXT("niagara_system") },
		MassBattleUnitEditorMCP::DefaultAgentRendererNiagaraPath,
		TEXT("defaulted_niagara_system"),
		TEXT("niagara_system"),
		Warnings);

	TSharedPtr<FJsonObject> Layout = MakeShared<FJsonObject>();
	Layout->SetStringField(TEXT("style_profile"), StyleId);
	Layout->SetStringField(TEXT("recipe_id"), RecipeId);
	Layout->SetStringField(TEXT("style_family"), StyleFamily);
	Layout->SetStringField(TEXT("unit_name"), AssetSlug);
	Layout->SetStringField(TEXT("target_package_path"), GeneratedPackagePath);
	Layout->SetStringField(TEXT("target_unit_package_path"), UnitPackagePath);
	Layout->SetStringField(TEXT("static_mesh_path"), MassBattleUnitEditorMCP::MakeObjectPath(GeneratedPackagePath, StaticMeshName));
	Layout->SetStringField(TEXT("unit_path"), MassBattleUnitEditorMCP::MakeObjectPath(UnitPackagePath, UnitAssetName));
	Layout->SetStringField(TEXT("renderer_class_path"), PlannedRendererClassPath);
	Layout->SetStringField(TEXT("vat_data_asset_path"), MassBattleUnitEditorMCP::MakeObjectPath(GeneratedPackagePath, VatDataName));
	Layout->SetStringField(TEXT("static_mesh_asset_name"), StaticMeshName);
	Layout->SetStringField(TEXT("unit_asset_name"), UnitAssetName);
	Layout->SetStringField(TEXT("renderer_asset_name"), RendererAssetName);
	Layout->SetStringField(TEXT("material_asset_name"), MaterialName);
	Layout->SetStringField(TEXT("material_name_prefix"), MaterialName);
	Layout->SetStringField(TEXT("animation_name_filter"), AnimationNameFilter);
	Layout->SetStringField(TEXT("parent_material"), ParentMaterialPath);
	Layout->SetStringField(TEXT("source_renderer_class"), SourceRendererClassPath);
	Layout->SetStringField(TEXT("niagara_system"), NiagaraSystemPath);

	TArray<TSharedPtr<FJsonValue>> Steps;
	TArray<TSharedPtr<FJsonValue>> Missing;
	if (SkeletalMeshPath.IsEmpty())
	{
		MassBattleUnitEditorMCP::AddStringArrayItem(Missing, TEXT("skeletal_mesh"));
	}

	TSharedPtr<FJsonObject> Discovery = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> UnitPatch = MakeShared<FJsonObject>();
	const TSharedPtr<FJsonObject>* RecipeDefaultPatch = nullptr;
	if (Recipe->TryGetObjectField(TEXT("default_unit_patch"), RecipeDefaultPatch) && RecipeDefaultPatch && RecipeDefaultPatch->IsValid())
	{
		MassBattleUnitEditorMCP::MergeJsonObjects(UnitPatch, *RecipeDefaultPatch);
	}

	TSharedPtr<FJsonObject> GeneratedData = MakeShared<FJsonObject>();

	if (!SkeletalMeshPath.IsEmpty())
	{
		if (!TextureSearchPath.IsEmpty())
		{
			const FString TextureResult = UMassBattleEditorMCPApi::MCP_FindAndFillOriginalTextures(SkeletalMeshPath, TextureSearchPath, AssetSlug);
			TSharedPtr<FJsonObject> TextureJson = MassBattleUnitEditorMCP::ParseObject(TextureResult);
			if (!TextureJson.IsValid() || !TextureJson->GetBoolField(TEXT("success")))
			{
				return TextureResult;
			}
			Discovery->SetObjectField(TEXT("textures"), TextureJson);
			MassBattleUnitEditorMCP::AddStep(Steps, TEXT("find_textures"), TEXT("MCP_FindAndFillOriginalTextures"), TEXT("done"), TEXT("Source texture candidates were discovered from the skeletal mesh."));
		}
		else
		{
			MassBattleUnitEditorMCP::AddStep(Steps, TEXT("find_textures"), TEXT("MCP_FindAndFillOriginalTextures"), TEXT("blocked"), TEXT("texture_search_path is missing."));
		}

		if (!AnimationSearchPath.IsEmpty())
		{
			const FString AnimResult = UMassBattleEditorMCPApi::MCP_FindAndFillAnimSequences(SkeletalMeshPath, AnimationSearchPath, AnimationNameFilter);
			TSharedPtr<FJsonObject> AnimJson = MassBattleUnitEditorMCP::ParseObject(AnimResult);
			if (!AnimJson.IsValid() || !AnimJson->GetBoolField(TEXT("success")))
			{
				return AnimResult;
			}
			const FString FoundAnimsJson = MassBattleUnitEditorMCP::JsonObjectFieldToString(AnimJson, TEXT("anims"));
			if (!MassBattleUnitEditorMCP::FoundAnimsJsonHasEntries(FoundAnimsJson))
			{
				MassBattleUnitEditorMCP::TryFillAnimDiscoveryWithCompatibleFallback(SkeletalMeshPath, AnimationSearchPath, AnimationNameFilter, AnimJson, Warnings);
			}
			Discovery->SetObjectField(TEXT("animations"), AnimJson);
			MassBattleUnitEditorMCP::AddStep(Steps, TEXT("find_animations"), TEXT("MCP_FindAndFillAnimSequences"), TEXT("done"), TEXT("Animation sequence candidates were discovered and categorized."));
		}
		else
		{
			MassBattleUnitEditorMCP::AddStep(Steps, TEXT("find_animations"), TEXT("MCP_FindAndFillAnimSequences"), TEXT("blocked"), TEXT("animation_search_path is missing."));
		}

		const FString LodResult = UMassBattleEditorMCPApi::MCP_FindAndFillLODSettings(SkeletalMeshPath);
		TSharedPtr<FJsonObject> LodJson = MassBattleUnitEditorMCP::ParseObject(LodResult);
		if (!LodJson.IsValid() || !LodJson->GetBoolField(TEXT("success")))
		{
			return LodResult;
		}
		Discovery->SetObjectField(TEXT("lod_settings"), LodJson);
		MassBattleUnitEditorMCP::AddStep(Steps, TEXT("find_lod_settings"), TEXT("MCP_FindAndFillLODSettings"), TEXT("done"), TEXT("LOD settings were inferred from the skeletal mesh."));

		const FString LodArrayJson = MassBattleUnitEditorMCP::JsonArrayFieldToString(LodJson, TEXT("lod_settings"));
		if (!LodArrayJson.IsEmpty())
		{
			const FString LodsDataResult = UMassBattleEditorMCPApi::MCP_ConvertLODSettingsToLODsData(LodArrayJson);
			TSharedPtr<FJsonObject> LodsDataJson = MassBattleUnitEditorMCP::ParseObject(LodsDataResult);
			if (!LodsDataJson.IsValid() || !LodsDataJson->GetBoolField(TEXT("success")))
			{
				return LodsDataResult;
			}
			Discovery->SetObjectField(TEXT("lods_data"), LodsDataJson);

			const TSharedPtr<FJsonObject>* LodsData = nullptr;
			if (LodsDataJson->TryGetObjectField(TEXT("lods_data"), LodsData) && LodsData && LodsData->IsValid())
			{
				TSharedPtr<FJsonObject> LodShared = MassBattleUnitEditorMCP::GetOrCreateObjectField(GeneratedData, TEXT("LODShared"));
				LodShared->SetObjectField(TEXT("RenderLOD"), *LodsData);
			}
			MassBattleUnitEditorMCP::AddStep(Steps, TEXT("convert_lod_data"), TEXT("MCP_ConvertLODSettingsToLODsData"), TEXT("done"), TEXT("LOD editor settings were converted to FLODShared.RenderLOD data."));
		}
	}

	FString DataAssetPath;
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("data_asset"), TEXT("anim_to_texture_data_asset"), TEXT("vat_data_asset") }, DataAssetPath);
	if (DataAssetPath.IsEmpty())
	{
		DataAssetPath = MassBattleUnitEditorMCP::MakeObjectPath(GeneratedPackagePath, VatDataName);
	}

	bool bAllowEmptyAnims = false;
	Spec->TryGetBoolField(TEXT("allow_empty_anims"), bAllowEmptyAnims);

	const TSharedPtr<FJsonObject>* AnimDiscovery = nullptr;
	if (Discovery->TryGetObjectField(TEXT("animations"), AnimDiscovery) && AnimDiscovery && AnimDiscovery->IsValid())
	{
		const FString FoundAnimsJson = MassBattleUnitEditorMCP::JsonObjectFieldToString(*AnimDiscovery, TEXT("anims"));
		if (!FoundAnimsJson.IsEmpty() && !DataAssetPath.IsEmpty() && (bAllowEmptyAnims || MassBattleUnitEditorMCP::FoundAnimsJsonHasEntries(FoundAnimsJson)))
		{
			const FString AnimsDataResult = UMassBattleEditorMCPApi::MCP_CreateAnimsDataFromSequences(DataAssetPath, FoundAnimsJson);
			TSharedPtr<FJsonObject> AnimsDataJson = MassBattleUnitEditorMCP::ParseObject(AnimsDataResult);
			if (AnimsDataJson.IsValid() && AnimsDataJson->GetBoolField(TEXT("success")))
			{
				Discovery->SetObjectField(TEXT("anims_data"), AnimsDataJson);
				const TSharedPtr<FJsonObject>* AnimsData = nullptr;
				if (AnimsDataJson->TryGetObjectField(TEXT("anims_data"), AnimsData) && AnimsData && AnimsData->IsValid())
				{
					TSharedPtr<FJsonObject> AnimShared = MassBattleUnitEditorMCP::GetOrCreateObjectField(GeneratedData, TEXT("AnimShared"));
					AnimShared->SetObjectField(TEXT("AnimData"), MassBattleUnitEditorMCP::ConvertAnimsDataForMerge(*AnimsData));
				}
				MassBattleUnitEditorMCP::AddStep(Steps, TEXT("create_anim_data"), TEXT("MCP_CreateAnimsDataFromSequences"), TEXT("done"), TEXT("Animation candidates were converted to FAnimShared.AnimData."));
			}
			else
			{
				Discovery->SetStringField(TEXT("anims_data_warning"), AnimsDataResult);
				MassBattleUnitEditorMCP::AddStep(Steps, TEXT("create_anim_data"), TEXT("MCP_CreateAnimsDataFromSequences"), TEXT("blocked"), TEXT("VAT data asset is missing or does not load yet; run after VAT bake data exists."));
			}
		}
		else if (!FoundAnimsJson.IsEmpty())
		{
			Discovery->SetStringField(TEXT("anims_data_warning"), TEXT("No animation sequences were found. Empty animation sets are skipped by default; pass allow_empty_anims=true to force."));
			MassBattleUnitEditorMCP::AddStep(Steps, TEXT("create_anim_data"), TEXT("MCP_CreateAnimsDataFromSequences"), TEXT("skipped"), TEXT("No animation sequences were found, so AnimShared.AnimData was not generated."));
		}
	}

	TSharedPtr<FJsonObject> Visualize = MassBattleUnitEditorMCP::GetOrCreateObjectField(GeneratedData, TEXT("Visualize"));
	Visualize->SetStringField(TEXT("RendererClass"), PlannedRendererClassPath);

	TSharedPtr<FJsonObject> GeneratedRoot = MakeShared<FJsonObject>();
	GeneratedRoot->SetObjectField(TEXT("Data"), GeneratedData);
	MassBattleUnitEditorMCP::MergeJsonObjects(UnitPatch, GeneratedRoot);

	const TSharedPtr<FJsonObject>* ExtraUnitPatch = nullptr;
	if (Spec->TryGetObjectField(TEXT("unit_patch"), ExtraUnitPatch) && ExtraUnitPatch && ExtraUnitPatch->IsValid())
	{
		MassBattleUnitEditorMCP::MergeJsonObjects(UnitPatch, *ExtraUnitPatch);
	}

	TSharedPtr<FJsonObject> Options = MakeShared<FJsonObject>();
	Options->SetBoolField(TEXT("expected_before"), true);
	UnitPatch->SetObjectField(TEXT("options"), Options);

	FString ExistingUnitPath;
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("target_unit"), TEXT("unit_path"), TEXT("existing_unit") }, ExistingUnitPath);
	TSharedPtr<FJsonObject> ExistingMergePlan;
	if (!ExistingUnitPath.IsEmpty())
	{
		const FString MergePlanResult = UMassBattleUnitMCPApi::MCP_UnitPlanMergeUpdate(ExistingUnitPath, MassBattleUnitEditorMCP::ToJsonString(UnitPatch));
		ExistingMergePlan = MassBattleUnitEditorMCP::ParseObject(MergePlanResult);
		if (ExistingMergePlan.IsValid())
		{
			Discovery->SetObjectField(TEXT("existing_unit_merge_plan"), ExistingMergePlan);
		}
	}

	FString TemplateUnitPath;
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("template_unit"), TEXT("source_unit") }, TemplateUnitPath);
	bool bTemplateUnitDefaulted = false;
	FString TemplateUnitDefaultSource;
	if (ExistingUnitPath.IsEmpty() && TemplateUnitPath.IsEmpty())
	{
		bTemplateUnitDefaulted = MassBattleUnitEditorMCP::ResolveDefaultTemplateUnitForFamily(AuthoringDefaults, StyleFamily, TemplateUnitPath, TemplateUnitDefaultSource);
		if (bTemplateUnitDefaulted)
		{
			MassBattleUnitEditorMCP::AddIssue(Warnings, TEXT("warning"), TEXT("defaulted_template_unit"), FString::Printf(TEXT("template_unit was not supplied; using %s: %s"), *TemplateUnitDefaultSource, *TemplateUnitPath), TEXT("template_unit"));
		}
		else
		{
			MassBattleUnitEditorMCP::AddIssue(Warnings, TEXT("warning"), TEXT("missing_template_unit_default"), TEXT("template_unit was not supplied and no default template is configured. MCP_UnitCreate will try its own default; if that is empty, apply will fail before creating the unit."), TEXT("template_unit"));
		}
	}
	TSharedPtr<FJsonObject> UnitCreateSpec;
	if (ExistingUnitPath.IsEmpty())
	{
		UnitCreateSpec = MakeShared<FJsonObject>();
		if (!TemplateUnitPath.IsEmpty())
		{
			UnitCreateSpec->SetStringField(TEXT("template_unit"), TemplateUnitPath);
			UnitCreateSpec->SetBoolField(TEXT("template_unit_defaulted"), bTemplateUnitDefaulted);
			if (!TemplateUnitDefaultSource.IsEmpty())
			{
				UnitCreateSpec->SetStringField(TEXT("template_unit_default_source"), TemplateUnitDefaultSource);
			}
		}
		UnitCreateSpec->SetStringField(TEXT("asset_name"), UnitAssetName);
		UnitCreateSpec->SetStringField(TEXT("package_path"), UnitPackagePath);
		Discovery->SetObjectField(TEXT("unit_create_spec"), UnitCreateSpec);
	}

	MassBattleUnitEditorMCP::AddStep(Steps, TEXT("convert_mesh"), TEXT("MCP_ConvertSkeletalMeshToStaticMeshWithLODs"), SkeletalMeshPath.IsEmpty() ? TEXT("blocked") : TEXT("planned"), TEXT("Convert the source skeletal mesh into the planned MassBattle static mesh asset."));
	MassBattleUnitEditorMCP::AddStep(Steps, TEXT("create_materials"), TEXT("MCP_CreateMaterialInstanceForStaticMeshWithLODs"), ParentMaterialPath.IsEmpty() ? TEXT("blocked") : TEXT("planned"), TEXT("Create material instances for the generated static mesh LOD material slots."));
	MassBattleUnitEditorMCP::AddStep(Steps, TEXT("apply_material_overrides"), TEXT("MCP_EditorApplyCreateVatUnit.material_overrides"), MassBattleUnitEditorMCP::HasMaterialOverrides(Spec) ? TEXT("planned") : TEXT("skipped"), TEXT("Optionally override generated StaticMesh material slots with explicit materials."));
	MassBattleUnitEditorMCP::AddStep(Steps, TEXT("create_vat_data_and_textures"), TEXT("MassBattleTools.CreateDataAsset/CreateVATTextures"), TEXT("planned"), TEXT("Create or reuse AnimToTextureDataAsset and VAT Texture2D assets."));
	MassBattleUnitEditorMCP::AddStep(Steps, TEXT("bake_vat_textures"), TEXT("UAnimToTextureBPLibrary.AnimationToTexture"), TEXT("planned"), TEXT("Bake animation frames into VAT textures using the MassBattleTools DoAll flow."));
	MassBattleUnitEditorMCP::AddStep(Steps, TEXT("update_vat_materials"), TEXT("UAnimToTextureBPLibrary.UpdateMaterialInstanceFromDataAsset"), TEXT("planned"), TEXT("Write baked VAT and AnimData texture parameters into generated material instances."));
	MassBattleUnitEditorMCP::AddStep(Steps, TEXT("duplicate_renderer"), TEXT("MCP_DuplicateClassAsset"), TEXT("planned"), TEXT("Duplicate or reuse a renderer Blueprint class for the unit subtype."));
	MassBattleUnitEditorMCP::AddStep(Steps, TEXT("set_renderer_defaults"), TEXT("MCP_SetClassDefaultProperties"), TEXT("planned"), TEXT("Set renderer CDO mesh, Niagara system, and SubType after generated assets exist."));
	MassBattleUnitEditorMCP::AddStep(Steps, TEXT("merge_unit_data"), TEXT("MCP_UnitPlanMergeUpdate"), ExistingUnitPath.IsEmpty() ? TEXT("planned_after_clone") : TEXT("done"), TEXT("Union-merge Visualize, LODShared, AnimShared, and any user unit_patch fields."));

	TSharedPtr<FJsonObject> Root = MassBattleUnitEditorMCP::MakeSuccess();
	Root->SetStringField(TEXT("editor_workflow"), TEXT("MassBattleEditor VAT skeletal unit authoring plan"));
	Root->SetBoolField(TEXT("ready_to_discover"), SkeletalMeshPath.IsEmpty() == false);
	Root->SetBoolField(TEXT("applicable"), Missing.IsEmpty());
	Root->SetObjectField(TEXT("resolved_layout"), Layout);
	Root->SetObjectField(TEXT("discovery"), Discovery);
	Root->SetObjectField(TEXT("unit_patch"), UnitPatch);
	Root->SetArrayField(TEXT("steps"), Steps);
	Root->SetArrayField(TEXT("missing"), Missing);
	Root->SetArrayField(TEXT("warnings"), Warnings);
	return MassBattleUnitEditorMCP::ToJsonString(Root);
}

FString UMassBattleUnitEditorMCPApi::MCP_EditorValidateCreateVatUnit(const FString& SpecJson)
{
	TSharedPtr<FJsonObject> Spec = MassBattleUnitEditorMCP::ParseObject(SpecJson);
	if (!Spec.IsValid())
	{
		return MassBattleUnitEditorMCP::MakeErrorJson(TEXT("SpecJson is not valid JSON"));
	}

	const FString PlanResult = MCP_EditorPlanCreateVatUnit(SpecJson);
	TSharedPtr<FJsonObject> Plan = MassBattleUnitEditorMCP::ParseObject(PlanResult);
	if (!Plan.IsValid())
	{
		return MassBattleUnitEditorMCP::MakeErrorJson(TEXT("MCP_EditorPlanCreateVatUnit returned invalid JSON"));
	}

	TArray<TSharedPtr<FJsonValue>> Issues;
	TArray<TSharedPtr<FJsonValue>> ExecutionPreview;
	bool bPlanSuccess = false;
	Plan->TryGetBoolField(TEXT("success"), bPlanSuccess);
	if (!bPlanSuccess)
	{
		FString Error;
		Plan->TryGetStringField(TEXT("error"), Error);
		MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("plan_failed"), Error.IsEmpty() ? TEXT("VAT unit plan failed.") : Error);
	}

	const TSharedPtr<FJsonObject>* LayoutPtr = nullptr;
	const TSharedPtr<FJsonObject>* DiscoveryPtr = nullptr;
	const TSharedPtr<FJsonObject> Layout = Plan->TryGetObjectField(TEXT("resolved_layout"), LayoutPtr) && LayoutPtr && LayoutPtr->IsValid() ? *LayoutPtr : nullptr;
	const TSharedPtr<FJsonObject> Discovery = Plan->TryGetObjectField(TEXT("discovery"), DiscoveryPtr) && DiscoveryPtr && DiscoveryPtr->IsValid() ? *DiscoveryPtr : nullptr;
	const TArray<TSharedPtr<FJsonValue>>* PlanWarnings = nullptr;
	if (Plan->TryGetArrayField(TEXT("warnings"), PlanWarnings) && PlanWarnings)
	{
		for (const TSharedPtr<FJsonValue>& Warning : *PlanWarnings)
		{
			Issues.Add(Warning);
		}
	}

	if (!Layout.IsValid())
	{
		MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("missing_layout"), TEXT("Plan did not contain resolved_layout."));
	}

	FString SkeletalMeshPath;
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("skeletal_mesh"), TEXT("mesh"), TEXT("source_mesh") }, SkeletalMeshPath);
	FString TargetUnitPath;
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("target_unit"), TEXT("unit_path"), TEXT("existing_unit") }, TargetUnitPath);
	FString TemplateUnitPath;
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("template_unit"), TEXT("source_unit") }, TemplateUnitPath);
	FString ParentMaterialPath;
	if (Layout.IsValid())
	{
		ParentMaterialPath = MassBattleUnitEditorMCP::StringFieldOrDefault(Layout, TEXT("parent_material"), FString());
	}
	if (ParentMaterialPath.IsEmpty())
	{
		MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("parent_material") }, ParentMaterialPath);
	}
	FString SourceRendererClassPath;
	if (Layout.IsValid())
	{
		SourceRendererClassPath = MassBattleUnitEditorMCP::StringFieldOrDefault(Layout, TEXT("source_renderer_class"), FString());
	}
	if (SourceRendererClassPath.IsEmpty())
	{
		MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("source_renderer_class"), TEXT("renderer_template_class"), TEXT("template_renderer_class") }, SourceRendererClassPath);
	}
	SourceRendererClassPath = MassBattleUnitEditorMCP::EnsureGeneratedClassPath(SourceRendererClassPath);
	FString NiagaraSystemPath;
	if (Layout.IsValid())
	{
		NiagaraSystemPath = MassBattleUnitEditorMCP::StringFieldOrDefault(Layout, TEXT("niagara_system"), FString());
	}
	if (NiagaraSystemPath.IsEmpty())
	{
		MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("niagara_system"), TEXT("niagara"), TEXT("niagara_system_asset") }, NiagaraSystemPath);
	}

	const FString StaticMeshPath = Layout.IsValid() ? MassBattleUnitEditorMCP::StringFieldOrDefault(Layout, TEXT("static_mesh_path"), FString()) : FString();
	const FString RendererClassPath = Layout.IsValid() ? MassBattleUnitEditorMCP::EnsureGeneratedClassPath(MassBattleUnitEditorMCP::StringFieldOrDefault(Layout, TEXT("renderer_class_path"), FString())) : FString();
	const FString PlannedUnitPath = Layout.IsValid() ? MassBattleUnitEditorMCP::StringFieldOrDefault(Layout, TEXT("unit_path"), FString()) : FString();
	const TSharedPtr<FJsonObject>* UnitCreateSpecPtr = nullptr;
	const bool bHasUnitCreateSpec = Discovery.IsValid() && Discovery->TryGetObjectField(TEXT("unit_create_spec"), UnitCreateSpecPtr) && UnitCreateSpecPtr && UnitCreateSpecPtr->IsValid();
	if (TemplateUnitPath.IsEmpty() && bHasUnitCreateSpec)
	{
		(*UnitCreateSpecPtr)->TryGetStringField(TEXT("template_unit"), TemplateUnitPath);
	}

	bool bOverwriteExisting = false;
	Spec->TryGetBoolField(TEXT("overwrite_existing"), bOverwriteExisting);
	bool bRefreshMaterials = bOverwriteExisting;
	Spec->TryGetBoolField(TEXT("refresh_materials"), bRefreshMaterials);

	if (SkeletalMeshPath.IsEmpty())
	{
		MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("missing_skeletal_mesh"), TEXT("skeletal_mesh is required."), TEXT("skeletal_mesh"));
		MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("convert_mesh"), TEXT("blocked"), TEXT("skeletal_mesh is required."));
	}
	else if (!MassBattleUnitEditorMCP::AssetExists(SkeletalMeshPath))
	{
		MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("skeletal_mesh_not_found"), FString::Printf(TEXT("SkeletalMesh does not exist or failed to load: %s"), *SkeletalMeshPath), TEXT("skeletal_mesh"));
		MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("convert_mesh"), TEXT("blocked"), TEXT("SkeletalMesh cannot be loaded."));
	}
	else if (!StaticMeshPath.IsEmpty() && MassBattleUnitEditorMCP::AssetExists(StaticMeshPath))
	{
		if (bOverwriteExisting)
		{
			MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("warning"), TEXT("static_mesh_will_overwrite"), FString::Printf(TEXT("StaticMesh exists and overwrite_existing=true: %s"), *StaticMeshPath), TEXT("overwrite_existing"));
			MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("convert_mesh"), TEXT("would_overwrite"), StaticMeshPath);
		}
		else
		{
			MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("info"), TEXT("static_mesh_will_skip"), FString::Printf(TEXT("StaticMesh exists and will be reused: %s"), *StaticMeshPath));
			MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("convert_mesh"), TEXT("skipped_existing"), StaticMeshPath);
		}
	}
	else
	{
		MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("convert_mesh"), TEXT("would_create"), StaticMeshPath);
	}

	if (ParentMaterialPath.IsEmpty())
	{
		MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("warning"), TEXT("missing_parent_material"), TEXT("parent_material is missing; material instance creation will be skipped."), TEXT("parent_material"));
		MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("create_materials"), TEXT("blocked"), TEXT("parent_material is required to create material instances."));
	}
	else if (!MassBattleUnitEditorMCP::AssetExists(ParentMaterialPath))
	{
		MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("parent_material_not_found"), FString::Printf(TEXT("parent_material does not exist or failed to load: %s"), *ParentMaterialPath), TEXT("parent_material"));
		MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("create_materials"), TEXT("blocked"), TEXT("parent_material cannot be loaded."));
	}
	else if (!bRefreshMaterials && !bOverwriteExisting && !StaticMeshPath.IsEmpty() && MassBattleUnitEditorMCP::AssetExists(StaticMeshPath))
	{
		MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("create_materials"), TEXT("skipped_existing"), TEXT("StaticMesh exists and refresh_materials=false."));
	}
	else
	{
		MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("create_materials"), TEXT("would_run"), TEXT("Material instances can be created."));
		FString OriginalTexturesJson;
		const TSharedPtr<FJsonObject>* TexturesResult = nullptr;
		if (Discovery.IsValid() && Discovery->TryGetObjectField(TEXT("textures"), TexturesResult) && TexturesResult && TexturesResult->IsValid())
		{
			OriginalTexturesJson = MassBattleUnitEditorMCP::JsonArrayFieldToString(*TexturesResult, TEXT("textures"));
		}
		TArray<FOriginalTextures> OriginalTexturesArray;
		if (MassBattleUnitEditorMCP::ParseOriginalTexturesJson(OriginalTexturesJson, OriginalTexturesArray))
		{
			TArray<TSharedPtr<FJsonValue>> TextureInheritanceWarnings;
			if (MassBattleUnitEditorMCP::EnrichOriginalTexturesFromSkeletalMaterials(SkeletalMeshPath, OriginalTexturesArray, TextureInheritanceWarnings) > 0)
			{
				for (const TSharedPtr<FJsonValue>& Warning : TextureInheritanceWarnings)
				{
					MassBattleUnitEditorMCP::AddIssueValueUnique(Issues, Warning);
				}
			}
		}
	}

	if (MassBattleUnitEditorMCP::HasMaterialOverrides(Spec))
	{
		const TArray<TSharedPtr<FJsonValue>>* Overrides = nullptr;
		Spec->TryGetArrayField(TEXT("material_overrides"), Overrides);
		bool bMaterialOverridesValid = true;
		int32 ValidOverrideCount = 0;
		if (Overrides)
		{
			for (int32 Index = 0; Index < Overrides->Num(); ++Index)
			{
				const TSharedPtr<FJsonValue>& OverrideValue = (*Overrides)[Index];
				FString MaterialPath;
				if (OverrideValue.IsValid() && OverrideValue->Type == EJson::String)
				{
					MaterialPath = OverrideValue->AsString();
				}
				else if (OverrideValue.IsValid() && OverrideValue->Type == EJson::Object)
				{
					TSharedPtr<FJsonObject> OverrideObject = OverrideValue->AsObject();
					MaterialPath = MassBattleUnitEditorMCP::StringFieldOrDefault(OverrideObject, TEXT("material"), FString());
					MaterialPath = MassBattleUnitEditorMCP::StringFieldOrDefault(OverrideObject, TEXT("material_path"), MaterialPath);
					MaterialPath = MassBattleUnitEditorMCP::StringFieldOrDefault(OverrideObject, TEXT("material_interface"), MaterialPath);
				}

				if (MaterialPath.IsEmpty())
				{
					bMaterialOverridesValid = false;
					MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("material_override_missing_material"), FString::Printf(TEXT("material_overrides[%d] does not specify a material path."), Index), TEXT("material_overrides"));
				}
				else if (!MassBattleUnitEditorMCP::AssetExists(MaterialPath))
				{
					bMaterialOverridesValid = false;
					MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("material_override_material_not_found"), FString::Printf(TEXT("material_overrides[%d] material does not exist or failed to load: %s"), Index, *MaterialPath), TEXT("material_overrides"));
				}
				else
				{
					++ValidOverrideCount;
				}
			}
		}
		MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("apply_material_overrides"), bMaterialOverridesValid ? TEXT("would_run") : TEXT("blocked"), FString::Printf(TEXT("%d material override(s) supplied."), ValidOverrideCount));
	}
	else
	{
		MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("apply_material_overrides"), TEXT("skipped"), TEXT("No material_overrides were supplied."));
	}

	const bool bBakeVat = MassBattleUnitEditorMCP::BoolFieldByNamesOrDefault(Spec, { TEXT("bake_vat"), TEXT("refresh_vat_data"), TEXT("run_anim_to_texture") }, true);
	MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("create_vat_data_and_textures"), bBakeVat ? TEXT("would_run") : TEXT("skipped"), TEXT("Create or reuse AnimToTextureDataAsset and VAT Texture2D assets."));
	MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("bake_vat_textures"), bBakeVat ? TEXT("would_run") : TEXT("skipped"), TEXT("Run UAnimToTextureBPLibrary::AnimationToTexture for each LOD setting."));
	MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("update_vat_materials"), bBakeVat ? TEXT("would_run") : TEXT("skipped"), TEXT("Update VAT material instance parameters from the baked DataAsset."));

	if (!RendererClassPath.IsEmpty() && MassBattleUnitEditorMCP::AssetExists(RendererClassPath))
	{
		MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("duplicate_renderer"), TEXT("skipped_existing"), RendererClassPath);
		MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("set_renderer_defaults"), TEXT("would_run"), TEXT("Renderer class exists and can receive defaults."));
	}
	else if (SourceRendererClassPath.IsEmpty())
	{
		MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("missing_source_renderer_class"), TEXT("source_renderer_class is required when the planned renderer class does not already exist."), TEXT("source_renderer_class"));
		MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("duplicate_renderer"), TEXT("blocked"), TEXT("source_renderer_class is missing."));
		MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("set_renderer_defaults"), TEXT("blocked"), TEXT("Renderer class is missing."));
	}
	else if (!MassBattleUnitEditorMCP::AssetExists(SourceRendererClassPath))
	{
		MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("source_renderer_class_not_found"), FString::Printf(TEXT("source_renderer_class does not exist or failed to load: %s"), *SourceRendererClassPath), TEXT("source_renderer_class"));
		MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("duplicate_renderer"), TEXT("blocked"), TEXT("source_renderer_class cannot be loaded."));
		MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("set_renderer_defaults"), TEXT("blocked"), TEXT("Renderer class is missing."));
	}
	else
	{
		MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("duplicate_renderer"), TEXT("would_create"), RendererClassPath);
		MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("set_renderer_defaults"), TEXT("would_run_after_duplicate"), TEXT("Renderer defaults can be set after duplication."));
	}

	if (!NiagaraSystemPath.IsEmpty() && !MassBattleUnitEditorMCP::AssetExists(NiagaraSystemPath))
	{
		MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("niagara_system_not_found"), FString::Printf(TEXT("niagara_system does not exist or failed to load: %s"), *NiagaraSystemPath), TEXT("niagara_system"));
	}

	if (TargetUnitPath.IsEmpty() && !bHasUnitCreateSpec)
	{
		MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("missing_unit_target"), TEXT("target_unit or generated unit_create_spec is required to write unit data."), TEXT("target_unit"));
		MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("merge_unit_data"), TEXT("blocked"), TEXT("No unit target or create spec was resolved."));
	}
	else if (!TargetUnitPath.IsEmpty())
	{
		if (!MassBattleUnitEditorMCP::AssetExists(TargetUnitPath))
		{
			MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("target_unit_not_found"), FString::Printf(TEXT("target_unit does not exist or failed to load: %s"), *TargetUnitPath), TEXT("target_unit"));
			MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("merge_unit_data"), TEXT("blocked"), TEXT("target_unit cannot be loaded."));
		}
		else
		{
			const TSharedPtr<FJsonObject>* ExistingMergePlan = nullptr;
			if (Discovery.IsValid() && Discovery->TryGetObjectField(TEXT("existing_unit_merge_plan"), ExistingMergePlan) && ExistingMergePlan && ExistingMergePlan->IsValid() && MassBattleUnitEditorMCP::JsonObjectFieldBool(*ExistingMergePlan, TEXT("applicable"), false))
			{
				MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("merge_unit_data"), TEXT("would_apply_plan"), TEXT("Existing-unit merge plan is applicable."));
			}
			else
			{
				MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("existing_unit_merge_not_applicable"), TEXT("Existing-unit merge plan is missing or not applicable."));
				MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("merge_unit_data"), TEXT("blocked"), TEXT("Existing-unit merge plan is not applicable."));
			}
		}
	}
	else
	{
		if (!TemplateUnitPath.IsEmpty() && !MassBattleUnitEditorMCP::AssetExists(TemplateUnitPath))
		{
			MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("template_unit_not_found"), FString::Printf(TEXT("template_unit does not exist or failed to load: %s"), *TemplateUnitPath), TEXT("template_unit"));
			MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("clone_unit"), TEXT("blocked"), TEXT("template_unit cannot be loaded."));
		}
		else
		{
			if (TemplateUnitPath.IsEmpty())
			{
				MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("warning"), TEXT("unit_create_will_use_mcp_default_template"), TEXT("unit_create_spec has no template_unit; MCP_UnitCreate will resolve authoring_defaults.default_unit_template at apply time."), TEXT("template_unit"));
			}

			if (!PlannedUnitPath.IsEmpty() && MassBattleUnitEditorMCP::AssetExists(PlannedUnitPath))
			{
				if (bOverwriteExisting)
				{
					MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("warning"), TEXT("planned_unit_will_overwrite"), FString::Printf(TEXT("Planned unit exists and overwrite_existing=true: %s"), *PlannedUnitPath), TEXT("overwrite_existing"));
					MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("clone_unit"), TEXT("skipped_existing"), TEXT("Existing planned unit will be reused."));
					MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("merge_unit_data"), TEXT("would_apply_to_existing"), TEXT("Unit patch will be merged into the existing planned unit."));
				}
				else
				{
					MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("planned_unit_exists"), FString::Printf(TEXT("Planned unit already exists; clone_unit does not overwrite unit DataAssets: %s"), *PlannedUnitPath), TEXT("unit_path"));
					MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("clone_unit"), TEXT("blocked"), TEXT("Planned unit already exists; choose a new unit_asset_name/package_path or delete the existing unit first."));
				}
			}
			else
			{
				MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("clone_unit"), TEXT("would_create"), PlannedUnitPath);
				MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("merge_unit_data"), TEXT("would_apply_after_clone"), TEXT("Clone merge plan can be generated after cloning."));
			}
		}
	}

	FString AnimsDataWarning;
	if (Discovery.IsValid() && Discovery->TryGetStringField(TEXT("anims_data_warning"), AnimsDataWarning) && !AnimsDataWarning.IsEmpty())
	{
		MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("warning"), TEXT("anim_data_not_generated"), AnimsDataWarning);
	}

	TSharedPtr<FJsonObject> Root = MassBattleUnitEditorMCP::MakeSuccess();
	Root->SetStringField(TEXT("validation_type"), TEXT("create_vat_unit"));
	Root->SetBoolField(TEXT("valid"), !MassBattleUnitEditorMCP::HasErrorIssue(Issues));
	Root->SetArrayField(TEXT("issues"), Issues);
	Root->SetArrayField(TEXT("execution_preview"), ExecutionPreview);
	Root->SetObjectField(TEXT("plan"), Plan);
	return MassBattleUnitEditorMCP::ToJsonString(Root);
}

FString UMassBattleUnitEditorMCPApi::MCP_EditorApplyCreateVatUnit(const FString& SpecJson, bool bSaveAssets)
{
	TSharedPtr<FJsonObject> Spec = MassBattleUnitEditorMCP::ParseObject(SpecJson);
	if (!Spec.IsValid())
	{
		return MassBattleUnitEditorMCP::MakeErrorJson(TEXT("SpecJson is not valid JSON"));
	}

	const FString PlanResult = MCP_EditorPlanCreateVatUnit(SpecJson);
	TSharedPtr<FJsonObject> Plan = MassBattleUnitEditorMCP::ParseObject(PlanResult);
	if (!Plan.IsValid() || !Plan->GetBoolField(TEXT("success")))
	{
		return PlanResult;
	}

	bool bDryRun = false;
	Spec->TryGetBoolField(TEXT("dry_run"), bDryRun);
	Spec->TryGetBoolField(TEXT("preview_only"), bDryRun);

	TSharedPtr<FJsonObject> Root = MassBattleUnitEditorMCP::MakeSuccess();
	Root->SetStringField(TEXT("editor_workflow"), TEXT("MassBattleTools DoAll VAT skeletal unit authoring apply"));
	Root->SetBoolField(TEXT("dry_run"), bDryRun);
	Root->SetBoolField(TEXT("save_assets"), bSaveAssets);
	Root->SetObjectField(TEXT("plan"), Plan);
	const bool bCompactResponse = MassBattleUnitEditorMCP::BoolFieldByNamesOrDefault(Spec, { TEXT("compact_response") }, false);
	Root->SetBoolField(TEXT("compact_response"), bCompactResponse);

	TArray<TSharedPtr<FJsonValue>> ExecutionSteps;
	if (bDryRun)
	{
		const FString ValidationResult = MCP_EditorValidateCreateVatUnit(SpecJson);
		TSharedPtr<FJsonObject> Validation = MassBattleUnitEditorMCP::ParseObject(ValidationResult);
		if (Validation.IsValid())
		{
			Root->SetObjectField(TEXT("validation"), Validation);
			const TArray<TSharedPtr<FJsonValue>>* Preview = nullptr;
			if (Validation->TryGetArrayField(TEXT("execution_preview"), Preview) && Preview)
			{
				ExecutionSteps = *Preview;
			}
		}
		MassBattleUnitEditorMCP::AddExecutionStep(ExecutionSteps, TEXT("dry_run"), TEXT("MCP_EditorValidateCreateVatUnit"), TEXT("skipped"), TEXT("dry_run=true; no assets were modified."));
		Root->SetArrayField(TEXT("execution_steps"), ExecutionSteps);
		return MassBattleUnitEditorMCP::ToJsonString(Root);
	}

	const TSharedPtr<FJsonObject>* LayoutPtr = nullptr;
	if (!Plan->TryGetObjectField(TEXT("resolved_layout"), LayoutPtr) || !LayoutPtr || !LayoutPtr->IsValid())
	{
		return MassBattleUnitEditorMCP::MakeErrorJson(TEXT("Plan did not contain resolved_layout"));
	}
	const TSharedPtr<FJsonObject> Layout = *LayoutPtr;

	const TSharedPtr<FJsonObject>* DiscoveryPtr = nullptr;
	TSharedPtr<FJsonObject> Discovery = MakeShared<FJsonObject>();
	if (Plan->TryGetObjectField(TEXT("discovery"), DiscoveryPtr) && DiscoveryPtr && DiscoveryPtr->IsValid())
	{
		Discovery = *DiscoveryPtr;
	}

	const TSharedPtr<FJsonObject>* UnitPatchPtr = nullptr;
	TSharedPtr<FJsonObject> UnitPatch = MakeShared<FJsonObject>();
	if (Plan->TryGetObjectField(TEXT("unit_patch"), UnitPatchPtr) && UnitPatchPtr && UnitPatchPtr->IsValid())
	{
		UnitPatch = *UnitPatchPtr;
	}

	FString SkeletalMeshPath;
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("skeletal_mesh"), TEXT("mesh"), TEXT("source_mesh") }, SkeletalMeshPath);
	FString StaticMeshPath = MassBattleUnitEditorMCP::StringFieldOrDefault(Layout, TEXT("static_mesh_path"), FString());
	const FString GeneratedPackagePath = MassBattleUnitEditorMCP::StringFieldOrDefault(Layout, TEXT("target_package_path"), FString());
	const FString AssetSlug = MassBattleUnitEditorMCP::StringFieldOrDefault(Layout, TEXT("unit_name"), MassBattleUnitEditorMCP::AssetNameFromObjectPath(StaticMeshPath));
	const FString StaticMeshAssetName = MassBattleUnitEditorMCP::StringFieldOrDefault(Layout, TEXT("static_mesh_asset_name"), MassBattleUnitEditorMCP::AssetNameFromObjectPath(StaticMeshPath));
	const FString MaterialAssetName = MassBattleUnitEditorMCP::StringFieldOrDefault(Layout, TEXT("material_asset_name"), MassBattleUnitEditorMCP::StringFieldOrDefault(Layout, TEXT("unit_name"), StaticMeshAssetName));
	const FString VatDataAssetPath = MassBattleUnitEditorMCP::StringFieldOrDefault(Layout, TEXT("vat_data_asset_path"), FString());
	const FString VatDataAssetName = MassBattleUnitEditorMCP::AssetNameFromObjectPath(VatDataAssetPath).IsEmpty() ? TEXT("VAT_") + AssetSlug : MassBattleUnitEditorMCP::AssetNameFromObjectPath(VatDataAssetPath);
	FString RendererClassPath = MassBattleUnitEditorMCP::EnsureGeneratedClassPath(MassBattleUnitEditorMCP::StringFieldOrDefault(Layout, TEXT("renderer_class_path"), FString()));
	const FString RendererAssetName = MassBattleUnitEditorMCP::StringFieldOrDefault(Layout, TEXT("renderer_asset_name"), MassBattleUnitEditorMCP::AssetNameFromObjectPath(RendererClassPath));
	const FString RendererPackagePath = MassBattleUnitEditorMCP::PackagePathFromObjectPath(RendererClassPath);
	TArray<FString> GeneratedVatSavePaths;
	int32 GeneratedVatSampleRate = 24;

	bool bOverwriteExisting = false;
	Spec->TryGetBoolField(TEXT("overwrite_existing"), bOverwriteExisting);
	bool bGenerateLightmapUVs = true;
	Spec->TryGetBoolField(TEXT("generate_lightmap_uvs"), bGenerateLightmapUVs);
	double LightmapIndexNumber = 0.0;
	Spec->TryGetNumberField(TEXT("lightmap_index"), LightmapIndexNumber);
	const int32 LightmapIndex = static_cast<int32>(LightmapIndexNumber);

	bool bStaticMeshCreatedThisRun = false;
	if (SkeletalMeshPath.IsEmpty())
	{
		MassBattleUnitEditorMCP::AddExecutionStep(ExecutionSteps, TEXT("convert_mesh"), TEXT("MCP_ConvertSkeletalMeshToStaticMeshWithLODs"), TEXT("blocked"), TEXT("skeletal_mesh is required."));
		Root->SetBoolField(TEXT("success"), false);
		Root->SetArrayField(TEXT("execution_steps"), ExecutionSteps);
		return MassBattleUnitEditorMCP::ToJsonString(Root);
	}
	else if (!bOverwriteExisting && MassBattleUnitEditorMCP::AssetExists(StaticMeshPath))
	{
		MassBattleUnitEditorMCP::AddExecutionStep(ExecutionSteps, TEXT("convert_mesh"), TEXT("MCP_ConvertSkeletalMeshToStaticMeshWithLODs"), TEXT("skipped_existing"), TEXT("StaticMesh already exists and overwrite_existing=false."));
	}
	else
	{
		TSharedPtr<FJsonObject> Step = MassBattleUnitEditorMCP::AddExecutionStep(ExecutionSteps, TEXT("convert_mesh"), TEXT("MCP_ConvertSkeletalMeshToStaticMeshWithLODs"), TEXT("running"), TEXT("Converting source SkeletalMesh to MassBattle StaticMesh."));
		const FString ConvertResult = UMassBattleEditorMCPApi::MCP_ConvertSkeletalMeshToStaticMeshWithLODs(SkeletalMeshPath, GeneratedPackagePath, StaticMeshAssetName, LightmapIndex, bGenerateLightmapUVs);
		MassBattleUnitEditorMCP::SetStepResult(Step, ConvertResult);
		TSharedPtr<FJsonObject> ConvertJson = MassBattleUnitEditorMCP::ParseObject(ConvertResult);
		if (!ConvertJson.IsValid() || !ConvertJson->GetBoolField(TEXT("success")))
		{
			Step->SetStringField(TEXT("status"), TEXT("failed"));
			Root->SetBoolField(TEXT("success"), false);
			Root->SetArrayField(TEXT("execution_steps"), ExecutionSteps);
			return MassBattleUnitEditorMCP::ToJsonString(Root);
		}
		Step->SetStringField(TEXT("status"), TEXT("done"));
		ConvertJson->TryGetStringField(TEXT("static_mesh_path"), StaticMeshPath);
		bStaticMeshCreatedThisRun = true;
	}

	FString ParentMaterialPath;
	ParentMaterialPath = MassBattleUnitEditorMCP::StringFieldOrDefault(Layout, TEXT("parent_material"), FString());
	if (ParentMaterialPath.IsEmpty())
	{
		MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("parent_material") }, ParentMaterialPath);
	}
	bool bRefreshMaterials = bOverwriteExisting;
	Spec->TryGetBoolField(TEXT("refresh_materials"), bRefreshMaterials);
	if (ParentMaterialPath.IsEmpty())
	{
		MassBattleUnitEditorMCP::AddExecutionStep(ExecutionSteps, TEXT("create_materials"), TEXT("MCP_CreateMaterialInstanceForStaticMeshWithLODs"), TEXT("blocked"), TEXT("parent_material is required to create material instances."));
	}
	else if (!bStaticMeshCreatedThisRun && !bRefreshMaterials && bOverwriteExisting == false && MassBattleUnitEditorMCP::AssetExists(StaticMeshPath))
	{
		MassBattleUnitEditorMCP::AddExecutionStep(ExecutionSteps, TEXT("create_materials"), TEXT("MCP_CreateMaterialInstanceForStaticMeshWithLODs"), TEXT("skipped_existing"), TEXT("StaticMesh already existed and refresh_materials=false."));
	}
	else
	{
		const TSharedPtr<FJsonObject>* TexturesResult = nullptr;
		FString OriginalTexturesJson;
		if (Discovery->TryGetObjectField(TEXT("textures"), TexturesResult) && TexturesResult && TexturesResult->IsValid())
		{
			OriginalTexturesJson = MassBattleUnitEditorMCP::JsonArrayFieldToString(*TexturesResult, TEXT("textures"));
		}

		TArray<TSharedPtr<FJsonValue>> TextureInheritanceWarnings;
		int32 InheritedTextureCount = 0;
		TArray<FOriginalTextures> OriginalTexturesArray;
		if (MassBattleUnitEditorMCP::ParseOriginalTexturesJson(OriginalTexturesJson, OriginalTexturesArray))
		{
			InheritedTextureCount = MassBattleUnitEditorMCP::EnrichOriginalTexturesFromSkeletalMaterials(SkeletalMeshPath, OriginalTexturesArray, TextureInheritanceWarnings);
			OriginalTexturesJson = MassBattleUnitEditorMCP::SerializeOriginalTexturesJson(OriginalTexturesArray);
		}

		TSharedPtr<FJsonObject> Step = MassBattleUnitEditorMCP::AddExecutionStep(ExecutionSteps, TEXT("create_materials"), TEXT("MCP_CreateMaterialInstanceForStaticMeshWithLODs"), TEXT("running"), TEXT("Creating material instances and assigning them to the generated StaticMesh."));
		Step->SetNumberField(TEXT("inherited_texture_count"), InheritedTextureCount);
		if (!TextureInheritanceWarnings.IsEmpty())
		{
			Step->SetArrayField(TEXT("warnings"), TextureInheritanceWarnings);
		}
		const FString MaterialResult = UMassBattleEditorMCPApi::MCP_CreateMaterialInstanceForStaticMeshWithLODs(StaticMeshPath, GeneratedPackagePath, MaterialAssetName, ParentMaterialPath, OriginalTexturesJson);
		MassBattleUnitEditorMCP::SetStepResult(Step, MaterialResult);
		TSharedPtr<FJsonObject> MaterialJson = MassBattleUnitEditorMCP::ParseObject(MaterialResult);
		if (!MaterialJson.IsValid() || !MaterialJson->GetBoolField(TEXT("success")))
		{
			Step->SetStringField(TEXT("status"), TEXT("failed"));
			Root->SetBoolField(TEXT("success"), false);
			Root->SetArrayField(TEXT("execution_steps"), ExecutionSteps);
			return MassBattleUnitEditorMCP::ToJsonString(Root);
		}
		Step->SetStringField(TEXT("status"), TEXT("done"));
	}

	if (MassBattleUnitEditorMCP::HasMaterialOverrides(Spec))
	{
		TSharedPtr<FJsonObject> Step = MassBattleUnitEditorMCP::AddExecutionStep(ExecutionSteps, TEXT("apply_material_overrides"), TEXT("MCP_EditorApplyCreateVatUnit.material_overrides"), TEXT("running"), TEXT("Applying explicit material overrides to the generated StaticMesh."));
		const FString OverrideResult = MassBattleUnitEditorMCP::ApplyStaticMeshMaterialOverrides(StaticMeshPath, Spec);
		MassBattleUnitEditorMCP::SetStepResult(Step, OverrideResult);
		TSharedPtr<FJsonObject> OverrideJson = MassBattleUnitEditorMCP::ParseObject(OverrideResult);
		if (!OverrideJson.IsValid() || !OverrideJson->GetBoolField(TEXT("success")))
		{
			Step->SetStringField(TEXT("status"), TEXT("failed"));
			Root->SetBoolField(TEXT("success"), false);
			Root->SetArrayField(TEXT("execution_steps"), ExecutionSteps);
			return MassBattleUnitEditorMCP::ToJsonString(Root);
		}
		Step->SetStringField(TEXT("status"), TEXT("done"));
	}
	else
	{
		MassBattleUnitEditorMCP::AddExecutionStep(ExecutionSteps, TEXT("apply_material_overrides"), TEXT("MCP_EditorApplyCreateVatUnit.material_overrides"), TEXT("skipped"), TEXT("No material_overrides were supplied."));
	}

	const bool bBakeVat = MassBattleUnitEditorMCP::BoolFieldByNamesOrDefault(Spec, { TEXT("bake_vat"), TEXT("refresh_vat_data"), TEXT("run_anim_to_texture") }, true);
	const bool bAllowStaticFallback = MassBattleUnitEditorMCP::BoolFieldByNamesOrDefault(Spec, { TEXT("allow_static_fallback"), TEXT("allow_missing_anims"), TEXT("allow_no_vat") }, true);
	bool bVatBakeCompleted = false;
	if (bBakeVat)
	{
		TSharedPtr<FJsonObject> Step = MassBattleUnitEditorMCP::AddExecutionStep(ExecutionSteps, TEXT("bake_vat_textures"), TEXT("MassBattleTools.CreateVATTextures -> AnimationToTexture -> UpdateMaterialInstance"), TEXT("running"), TEXT("Baking VAT textures and updating generated material instances."));
		TSharedPtr<FJsonObject> BakeResult = MassBattleUnitEditorMCP::BakeVatWithMassBattleToolsFlow(Spec, Discovery, SkeletalMeshPath, StaticMeshPath, GeneratedPackagePath, AssetSlug, VatDataAssetName, UnitPatch, GeneratedVatSavePaths);
		Step->SetObjectField(TEXT("result"), BakeResult);
		if (!BakeResult.IsValid() || !BakeResult->GetBoolField(TEXT("success")))
		{
			FString BakeError;
			if (BakeResult.IsValid())
			{
				BakeResult->TryGetStringField(TEXT("error"), BakeError);
			}
			if (bAllowStaticFallback && BakeError.Contains(TEXT("No animation sequences"), ESearchCase::IgnoreCase))
			{
				Step->SetStringField(TEXT("status"), TEXT("warning_static_fallback"));
				Step->SetStringField(TEXT("warning"), TEXT("No animation sequences were available; continuing unit creation without refreshed VAT animation data. Provide animations or animation_search_path to complete animated VAT output."));
				Root->SetBoolField(TEXT("static_fallback_used"), true);
			}
			else
			{
				Step->SetStringField(TEXT("status"), TEXT("failed"));
				Root->SetBoolField(TEXT("success"), false);
				Root->SetArrayField(TEXT("execution_steps"), ExecutionSteps);
				return MassBattleUnitEditorMCP::ToJsonString(Root);
			}
		}
		else
		{
			double SampleRateNumber = 24.0;
			BakeResult->TryGetNumberField(TEXT("sample_rate"), SampleRateNumber);
			GeneratedVatSampleRate = static_cast<int32>(SampleRateNumber);
			bVatBakeCompleted = true;
			Step->SetStringField(TEXT("status"), TEXT("done"));
		}
	}
	else
	{
		MassBattleUnitEditorMCP::AddExecutionStep(ExecutionSteps, TEXT("bake_vat_textures"), TEXT("MassBattleTools.CreateVATTextures -> AnimationToTexture"), TEXT("skipped"), TEXT("bake_vat=false."));
	}

	FString SourceRendererClassPath;
	SourceRendererClassPath = MassBattleUnitEditorMCP::StringFieldOrDefault(Layout, TEXT("source_renderer_class"), FString());
	if (SourceRendererClassPath.IsEmpty())
	{
		MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("source_renderer_class"), TEXT("renderer_template_class"), TEXT("template_renderer_class") }, SourceRendererClassPath);
	}
	SourceRendererClassPath = MassBattleUnitEditorMCP::EnsureGeneratedClassPath(SourceRendererClassPath);
	if (MassBattleUnitEditorMCP::AssetExists(RendererClassPath))
	{
		MassBattleUnitEditorMCP::AddExecutionStep(ExecutionSteps, TEXT("duplicate_renderer"), TEXT("MCP_DuplicateClassAsset"), TEXT("skipped_existing"), TEXT("Renderer class already exists or renderer_class was supplied."));
	}
	else if (SourceRendererClassPath.IsEmpty())
	{
		MassBattleUnitEditorMCP::AddExecutionStep(ExecutionSteps, TEXT("duplicate_renderer"), TEXT("MCP_DuplicateClassAsset"), TEXT("blocked"), TEXT("source_renderer_class is required when the planned renderer class does not already exist."));
	}
	else
	{
		TSharedPtr<FJsonObject> Step = MassBattleUnitEditorMCP::AddExecutionStep(ExecutionSteps, TEXT("duplicate_renderer"), TEXT("MCP_DuplicateClassAsset"), TEXT("running"), TEXT("Duplicating renderer Blueprint class."));
		const FString DuplicateResult = UMassBattleEditorMCPApi::MCP_DuplicateClassAsset(SourceRendererClassPath, RendererAssetName, RendererPackagePath);
		MassBattleUnitEditorMCP::SetStepResult(Step, DuplicateResult);
		TSharedPtr<FJsonObject> DuplicateJson = MassBattleUnitEditorMCP::ParseObject(DuplicateResult);
		if (!DuplicateJson.IsValid() || !DuplicateJson->GetBoolField(TEXT("success")))
		{
			Step->SetStringField(TEXT("status"), TEXT("failed"));
			Root->SetBoolField(TEXT("success"), false);
			Root->SetArrayField(TEXT("execution_steps"), ExecutionSteps);
			return MassBattleUnitEditorMCP::ToJsonString(Root);
		}
		Step->SetStringField(TEXT("status"), TEXT("done"));
		DuplicateJson->TryGetStringField(TEXT("class_path"), RendererClassPath);
		RendererClassPath = MassBattleUnitEditorMCP::EnsureGeneratedClassPath(RendererClassPath);
	}

	FString NiagaraSystemPath;
	NiagaraSystemPath = MassBattleUnitEditorMCP::StringFieldOrDefault(Layout, TEXT("niagara_system"), FString());
	if (NiagaraSystemPath.IsEmpty())
	{
		MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("niagara_system"), TEXT("niagara"), TEXT("niagara_system_asset") }, NiagaraSystemPath);
	}
	int32 SubType = 0;
	double SubTypeNumber = 0.0;
	if (Spec->TryGetNumberField(TEXT("subtype"), SubTypeNumber) || Spec->TryGetNumberField(TEXT("sub_type"), SubTypeNumber))
	{
		SubType = static_cast<int32>(SubTypeNumber);
	}
	if (!RendererClassPath.IsEmpty() && MassBattleUnitEditorMCP::AssetExists(RendererClassPath))
	{
		TSharedPtr<FJsonObject> Step = MassBattleUnitEditorMCP::AddExecutionStep(ExecutionSteps, TEXT("set_renderer_defaults"), TEXT("MCP_SetClassDefaultProperties"), TEXT("running"), TEXT("Setting renderer CDO mesh, Niagara system, and SubType."));
		const FString DefaultsResult = UMassBattleEditorMCPApi::MCP_SetClassDefaultProperties(RendererClassPath, StaticMeshPath, NiagaraSystemPath, SubType, bSaveAssets);
		MassBattleUnitEditorMCP::SetStepResult(Step, DefaultsResult);
		TSharedPtr<FJsonObject> DefaultsJson = MassBattleUnitEditorMCP::ParseObject(DefaultsResult);
		if (!DefaultsJson.IsValid() || !DefaultsJson->GetBoolField(TEXT("success")))
		{
			Step->SetStringField(TEXT("status"), TEXT("failed"));
			Root->SetBoolField(TEXT("success"), false);
			Root->SetArrayField(TEXT("execution_steps"), ExecutionSteps);
			return MassBattleUnitEditorMCP::ToJsonString(Root);
		}
		Step->SetStringField(TEXT("status"), TEXT("done"));
	}
	else
	{
		MassBattleUnitEditorMCP::AddExecutionStep(ExecutionSteps, TEXT("set_renderer_defaults"), TEXT("MCP_SetClassDefaultProperties"), TEXT("blocked"), TEXT("Renderer class is missing."));
		bool bAllowMissingRenderer = false;
		Spec->TryGetBoolField(TEXT("allow_missing_renderer"), bAllowMissingRenderer);
		if (!bAllowMissingRenderer)
		{
			Root->SetBoolField(TEXT("success"), false);
			Root->SetArrayField(TEXT("execution_steps"), ExecutionSteps);
			return MassBattleUnitEditorMCP::ToJsonString(Root);
		}
	}

	FString TargetUnitPath;
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("target_unit"), TEXT("unit_path"), TEXT("existing_unit") }, TargetUnitPath);
	const FString PlannedUnitPath = MassBattleUnitEditorMCP::StringFieldOrDefault(Layout, TEXT("unit_path"), FString());
	FString FinalUnitPath;

	if (!TargetUnitPath.IsEmpty())
	{
		TSharedPtr<FJsonObject> MergeStep = MassBattleUnitEditorMCP::AddExecutionStep(ExecutionSteps, TEXT("merge_unit_data"), TEXT("MCP_UnitPlanMergeUpdate -> MCP_UnitApplyPlan"), TEXT("running"), TEXT("Creating and applying merge plan for the target unit."));
		const FString MergePlanResult = UMassBattleUnitMCPApi::MCP_UnitPlanMergeUpdate(TargetUnitPath, MassBattleUnitEditorMCP::ToJsonString(UnitPatch));
		TSharedPtr<FJsonObject> MergePlanJson = MassBattleUnitEditorMCP::ParseObject(MergePlanResult);
		if (!MergePlanJson.IsValid() || !MergePlanJson->GetBoolField(TEXT("success")))
		{
			MassBattleUnitEditorMCP::SetStepResult(MergeStep, MergePlanResult);
			MergeStep->SetStringField(TEXT("status"), TEXT("failed"));
			Root->SetBoolField(TEXT("success"), false);
			Root->SetArrayField(TEXT("execution_steps"), ExecutionSteps);
			return MassBattleUnitEditorMCP::ToJsonString(Root);
		}

		FString MergePlanId;
		MergePlanJson->TryGetStringField(TEXT("plan_id"), MergePlanId);
		const FString MergeApplyResult = UMassBattleUnitMCPApi::MCP_UnitApplyPlan(MergePlanId, bSaveAssets);
		TSharedPtr<FJsonObject> MergeResult = MakeShared<FJsonObject>();
		MergeResult->SetObjectField(TEXT("merge_plan"), MergePlanJson);
		TSharedPtr<FJsonObject> MergeApplyJson = MassBattleUnitEditorMCP::ParseObject(MergeApplyResult);
		if (MergeApplyJson.IsValid())
		{
			MergeResult->SetObjectField(TEXT("apply_result"), MergeApplyJson);
		}
		MergeStep->SetObjectField(TEXT("result"), MergeResult);
		MergeStep->SetStringField(TEXT("status"), MergeApplyJson.IsValid() && MergeApplyJson->GetBoolField(TEXT("success")) ? TEXT("done") : TEXT("failed"));
		if (!MergeApplyJson.IsValid() || !MergeApplyJson->GetBoolField(TEXT("success")))
		{
			Root->SetBoolField(TEXT("success"), false);
			Root->SetArrayField(TEXT("execution_steps"), ExecutionSteps);
			return MassBattleUnitEditorMCP::ToJsonString(Root);
		}
		FinalUnitPath = TargetUnitPath;
	}
	else
	{
		const TSharedPtr<FJsonObject>* UnitCreateSpec = nullptr;
		if (Discovery->TryGetObjectField(TEXT("unit_create_spec"), UnitCreateSpec) && UnitCreateSpec && UnitCreateSpec->IsValid())
		{
			const bool bUseExistingPlannedUnit = bOverwriteExisting && !PlannedUnitPath.IsEmpty() && MassBattleUnitEditorMCP::AssetExists(PlannedUnitPath);
			if (bUseExistingPlannedUnit)
			{
				MassBattleUnitEditorMCP::AddExecutionStep(ExecutionSteps, TEXT("create_unit"), TEXT("MCP_UnitCreate"), TEXT("skipped_existing"), TEXT("Planned unit already exists and overwrite_existing=true; applying merge patch to the existing unit."));
			}
			else
			{
				TSharedPtr<FJsonObject> CreateStep = MassBattleUnitEditorMCP::AddExecutionStep(ExecutionSteps, TEXT("create_unit"), TEXT("MCP_UnitCreate"), TEXT("running"), TEXT("Creating the new unit DataAsset."));
				const FString CreateResult = UMassBattleUnitMCPApi::MCP_UnitCreate(MassBattleUnitEditorMCP::ToJsonString(*UnitCreateSpec), bSaveAssets);
				MassBattleUnitEditorMCP::SetStepResult(CreateStep, CreateResult);
				TSharedPtr<FJsonObject> CreateJson = MassBattleUnitEditorMCP::ParseObject(CreateResult);
				CreateStep->SetStringField(TEXT("status"), CreateJson.IsValid() && CreateJson->GetBoolField(TEXT("success")) ? TEXT("done") : TEXT("failed"));
				if (!CreateJson.IsValid() || !CreateJson->GetBoolField(TEXT("success")))
				{
					Root->SetBoolField(TEXT("success"), false);
					Root->SetArrayField(TEXT("execution_steps"), ExecutionSteps);
					return MassBattleUnitEditorMCP::ToJsonString(Root);
				}
			}

			TSharedPtr<FJsonObject> MergeStep = MassBattleUnitEditorMCP::AddExecutionStep(ExecutionSteps, TEXT("merge_unit_data"), TEXT("MCP_UnitPlanMergeUpdate -> MCP_UnitApplyPlan"), TEXT("running"), TEXT("Creating and applying merge plan for the planned unit."));
			const FString MergePlanResult = UMassBattleUnitMCPApi::MCP_UnitPlanMergeUpdate(PlannedUnitPath, MassBattleUnitEditorMCP::ToJsonString(UnitPatch));
			TSharedPtr<FJsonObject> MergePlanJson = MassBattleUnitEditorMCP::ParseObject(MergePlanResult);
			if (!MergePlanJson.IsValid() || !MergePlanJson->GetBoolField(TEXT("success")))
			{
				MassBattleUnitEditorMCP::SetStepResult(MergeStep, MergePlanResult);
				MergeStep->SetStringField(TEXT("status"), TEXT("failed"));
				Root->SetBoolField(TEXT("success"), false);
				Root->SetArrayField(TEXT("execution_steps"), ExecutionSteps);
				return MassBattleUnitEditorMCP::ToJsonString(Root);
			}

			FString MergePlanId;
			MergePlanJson->TryGetStringField(TEXT("plan_id"), MergePlanId);
			const FString MergeApplyResult = UMassBattleUnitMCPApi::MCP_UnitApplyPlan(MergePlanId, bSaveAssets);
			TSharedPtr<FJsonObject> MergeResult = MakeShared<FJsonObject>();
			MergeResult->SetObjectField(TEXT("merge_plan"), MergePlanJson);
			TSharedPtr<FJsonObject> MergeApplyJson = MassBattleUnitEditorMCP::ParseObject(MergeApplyResult);
			if (MergeApplyJson.IsValid())
			{
				MergeResult->SetObjectField(TEXT("apply_result"), MergeApplyJson);
			}
			MergeStep->SetObjectField(TEXT("result"), MergeResult);
			MergeStep->SetStringField(TEXT("status"), MergeApplyJson.IsValid() && MergeApplyJson->GetBoolField(TEXT("success")) ? TEXT("done") : TEXT("failed"));
			if (!MergeApplyJson.IsValid() || !MergeApplyJson->GetBoolField(TEXT("success")))
			{
				Root->SetBoolField(TEXT("success"), false);
				Root->SetArrayField(TEXT("execution_steps"), ExecutionSteps);
				return MassBattleUnitEditorMCP::ToJsonString(Root);
			}
			FinalUnitPath = PlannedUnitPath;
		}
		else
		{
			MassBattleUnitEditorMCP::AddExecutionStep(ExecutionSteps, TEXT("create_unit"), TEXT("MCP_UnitCreate"), TEXT("blocked"), TEXT("Plan did not contain a unit_create_spec."));
			Root->SetBoolField(TEXT("success"), false);
			Root->SetArrayField(TEXT("execution_steps"), ExecutionSteps);
			return MassBattleUnitEditorMCP::ToJsonString(Root);
		}
	}

	if (bVatBakeCompleted && !FinalUnitPath.IsEmpty())
	{
		TSharedPtr<FJsonObject> Step = MassBattleUnitEditorMCP::AddExecutionStep(ExecutionSteps, TEXT("set_runtime_animation_defaults"), TEXT("MassBattleTools.CreateDataAsset.SetMembers(Animating)"), TEXT("running"), TEXT("Writing Animating.SampleRate directly like the editor utility widget."));
		MassBattleUnitEditorMCP::ApplyGeneratedUnitRuntimeAnimationDefaults(FinalUnitPath, GeneratedVatSampleRate);
		Step->SetStringField(TEXT("status"), TEXT("done"));
		Step->SetStringField(TEXT("unit_path"), FinalUnitPath);
		Step->SetNumberField(TEXT("sample_rate"), GeneratedVatSampleRate);
	}

	if (bSaveAssets)
	{
		TArray<FString> SavePaths = { StaticMeshPath, MassBattleUnitEditorMCP::BlueprintPathFromGeneratedClassPath(RendererClassPath) };
		MassBattleUnitEditorMCP::AddStaticMeshMaterialPaths(StaticMeshPath, GeneratedPackagePath, SavePaths);
		for (const FString& VatSavePath : GeneratedVatSavePaths)
		{
			SavePaths.AddUnique(VatSavePath);
		}
		if (!FinalUnitPath.IsEmpty())
		{
			SavePaths.AddUnique(FinalUnitPath);
		}
		for (const FString& SavePath : SavePaths)
		{
			if (SavePath.IsEmpty() || !MassBattleUnitEditorMCP::AssetExists(SavePath))
			{
				continue;
			}

			FString SaveError;
			TSharedPtr<FJsonObject> SaveStep = MassBattleUnitEditorMCP::AddExecutionStep(ExecutionSteps, TEXT("save_asset"), TEXT("UPackage::SavePackage"), TEXT("running"), SavePath);
			if (MassBattleUnitEditorMCP::SaveAssetByPath(SavePath, SaveError))
			{
				SaveStep->SetStringField(TEXT("status"), TEXT("done"));
			}
			else
			{
				SaveStep->SetStringField(TEXT("status"), TEXT("failed"));
				SaveStep->SetStringField(TEXT("error"), SaveError);
				Root->SetBoolField(TEXT("success"), false);
				Root->SetArrayField(TEXT("execution_steps"), ExecutionSteps);
				return MassBattleUnitEditorMCP::ToJsonString(Root);
			}
		}
	}

	Root->SetArrayField(TEXT("execution_steps"), ExecutionSteps);
	if (bCompactResponse)
	{
		return MassBattleUnitEditorMCP::ToJsonString(MassBattleUnitEditorMCP::MakeCompactCreateVatApplySummary(Root));
	}
	return MassBattleUnitEditorMCP::ToJsonString(Root);
}

FString UMassBattleUnitEditorMCPApi::MCP_EditorPlanOrganizeUnitAssets(const FString& UnitPath, const FString& OptionsJson)
{
	TSharedPtr<FJsonObject> Options = MassBattleUnitEditorMCP::ParseObject(OptionsJson);
	if (!Options.IsValid())
	{
		return MassBattleUnitEditorMCP::MakeErrorJson(TEXT("OptionsJson is not valid JSON"));
	}

	if (!MassBattleUnitEditorMCP::AssetExists(UnitPath))
	{
		return MassBattleUnitEditorMCP::MakeErrorJson(FString::Printf(TEXT("Unit asset does not exist or failed to load: %s"), *UnitPath));
	}

	FString StyleId = TEXT("default");
	Options->TryGetStringField(TEXT("style_profile"), StyleId);
	TSharedPtr<FJsonObject> Style = MassBattleUnitEditorMCP::LoadProfileConfig(TEXT("style"), StyleId);
	if (!Style.IsValid())
	{
		return MassBattleUnitEditorMCP::MakeErrorJson(FString::Printf(TEXT("Style profile not found or invalid: %s"), *StyleId));
	}

	const TSharedPtr<FJsonObject>* AuthoringDefaultsPtr = nullptr;
	TSharedPtr<FJsonObject> AuthoringDefaults = MakeShared<FJsonObject>();
	if (Style->TryGetObjectField(TEXT("authoring_defaults"), AuthoringDefaultsPtr) && AuthoringDefaultsPtr && AuthoringDefaultsPtr->IsValid())
	{
		AuthoringDefaults = *AuthoringDefaultsPtr;
	}

	const TSharedPtr<FJsonObject>* OrganizationPtr = nullptr;
	TSharedPtr<FJsonObject> Organization = MakeShared<FJsonObject>();
	if (Style->TryGetObjectField(TEXT("organization"), OrganizationPtr) && OrganizationPtr && OrganizationPtr->IsValid())
	{
		Organization = *OrganizationPtr;
	}

	const FString UnitAssetName = MassBattleUnitEditorMCP::AssetNameFromObjectPath(UnitPath);
	FString AssetSlug = MassBattleUnitEditorMCP::StringFieldOrDefault(Options, TEXT("unit_name"), UnitAssetName);
	const FString UnitAssetPrefix = MassBattleUnitEditorMCP::StringFieldOrDefault(AuthoringDefaults, TEXT("unit_asset_prefix"), TEXT("AgentConfig_"));
	if (!UnitAssetPrefix.IsEmpty() && AssetSlug.StartsWith(UnitAssetPrefix))
	{
		AssetSlug.RightChopInline(UnitAssetPrefix.Len());
	}
	AssetSlug = UMassBattleEditorMCPApi::MCP_SanitizeForPath(AssetSlug);
	if (AssetSlug.IsEmpty())
	{
		AssetSlug = UnitAssetName;
	}

	const FString CurrentUnitPackageName = MassBattleUnitEditorMCP::PackageNameFromObjectPath(UnitPath);
	const FString CurrentUnitPackagePath = MassBattleUnitEditorMCP::PackagePathFromPackageName(CurrentUnitPackageName);
	const FString StyleFamily = MassBattleUnitEditorMCP::ResolveStyleFamily(Options, Organization, UnitPath + TEXT(" ") + AssetSlug + TEXT(" ") + CurrentUnitPackagePath);
	const FString FamilyFolder = MassBattleUnitEditorMCP::ResolveFamilyFolder(Organization, StyleFamily);

	FString OutputRoot = MassBattleUnitEditorMCP::StringFieldOrDefault(Organization, TEXT("target_root"), TEXT("/Game/Unit/Actor"));
	Options->TryGetStringField(TEXT("target_root"), OutputRoot);
	bool bUseFamilyFolder = true;
	Options->TryGetBoolField(TEXT("use_family_folder"), bUseFamilyFolder);
	if (bUseFamilyFolder && !FamilyFolder.IsEmpty())
	{
		OutputRoot = OutputRoot / FamilyFolder;
	}

	FString GeneratedFolderName = MassBattleUnitEditorMCP::StringFieldOrDefault(AuthoringDefaults, TEXT("generated_folder_name"), TEXT("Gen_{unit_name}"));
	Options->TryGetStringField(TEXT("generated_folder_name"), GeneratedFolderName);
	Options->TryGetStringField(TEXT("target_folder_name"), GeneratedFolderName);
	GeneratedFolderName.ReplaceInline(TEXT("{unit_name}"), *AssetSlug);

	FString TargetPackagePath = OutputRoot / GeneratedFolderName;
	Options->TryGetStringField(TEXT("target_package_path"), TargetPackagePath);

	TArray<FString> ManagedRoots;
	MassBattleUnitEditorMCP::AddUniqueRoot(ManagedRoots, CurrentUnitPackagePath);
	MassBattleUnitEditorMCP::AddUniqueRoot(ManagedRoots, MassBattleUnitEditorMCP::StringFieldOrDefault(Organization, TEXT("target_root"), FString()));
	for (const FString& ProjectRoot : MassBattleUnitEditorMCP::StringArrayField(Style, TEXT("project_scan_roots")))
	{
		MassBattleUnitEditorMCP::AddUniqueRoot(ManagedRoots, ProjectRoot);
	}
	for (const FString& ManagedRoot : MassBattleUnitEditorMCP::StringArrayField(Options, TEXT("managed_roots")))
	{
		MassBattleUnitEditorMCP::AddUniqueRoot(ManagedRoots, ManagedRoot);
	}

	bool bIncludeDependencies = true;
	Options->TryGetBoolField(TEXT("include_dependencies"), bIncludeDependencies);
	int32 MaxDependencyDepth = bIncludeDependencies ? 2 : 0;
	double DepthNumber = static_cast<double>(MaxDependencyDepth);
	if (Options->TryGetNumberField(TEXT("dependency_depth"), DepthNumber) || Options->TryGetNumberField(TEXT("recursive_dependency_depth"), DepthNumber))
	{
		MaxDependencyDepth = FMath::Clamp(static_cast<int32>(DepthNumber), 0, 5);
	}

	bool bAllowPluginContent = false;
	Options->TryGetBoolField(TEXT("allow_plugin_content"), bAllowPluginContent);

	TArray<MassBattleUnitEditorMCP::FOrganizeAssetRef> LinkedAssets = MassBattleUnitEditorMCP::CollectLinkedUnitAssets(UnitPath, Options, ManagedRoots, MaxDependencyDepth);
	bool bIncludeSiblingAssets = true;
	Options->TryGetBoolField(TEXT("include_sibling_assets"), bIncludeSiblingAssets);
	if (bIncludeSiblingAssets)
	{
		MassBattleUnitEditorMCP::AddSiblingAssetsBySlug(CurrentUnitPackagePath, AssetSlug, LinkedAssets);
		LinkedAssets.Sort([](const MassBattleUnitEditorMCP::FOrganizeAssetRef& A, const MassBattleUnitEditorMCP::FOrganizeAssetRef& B)
		{
			return A.ObjectPath < B.ObjectPath;
		});
	}
	TArray<TSharedPtr<FJsonValue>> Moves;
	TSet<FString> PlannedDestinations;
	int32 MoveCount = 0;
	int32 AlreadyInPlaceCount = 0;
	int32 BlockedCount = 0;

	for (const MassBattleUnitEditorMCP::FOrganizeAssetRef& Asset : LinkedAssets)
	{
		const FString DestinationPath = MassBattleUnitEditorMCP::MakeObjectPath(TargetPackagePath, Asset.AssetName);
		FString Status = TEXT("would_move");
		FString Reason;
		if (Asset.PackagePath.Equals(TargetPackagePath, ESearchCase::IgnoreCase) || Asset.ObjectPath.Equals(DestinationPath, ESearchCase::IgnoreCase))
		{
			Status = TEXT("already_in_place");
			Reason = TEXT("Asset is already in the target package path.");
			AlreadyInPlaceCount++;
		}
		else if (!bAllowPluginContent && !MassBattleUnitEditorMCP::IsGameContentPackage(Asset.PackageName))
		{
			Status = TEXT("blocked_plugin_content");
			Reason = TEXT("Moving plugin content is blocked by default; pass allow_plugin_content=true to override.");
			BlockedCount++;
		}
		else if (MassBattleUnitEditorMCP::AssetExists(DestinationPath))
		{
			Status = TEXT("blocked_conflict");
			Reason = TEXT("Destination asset already exists; this organizer does not overwrite assets.");
			BlockedCount++;
		}
		else if (PlannedDestinations.Contains(DestinationPath))
		{
			Status = TEXT("blocked_duplicate_destination");
			Reason = TEXT("Another planned move has the same destination path.");
			BlockedCount++;
		}
		else
		{
			MoveCount++;
			PlannedDestinations.Add(DestinationPath);
		}

		TSharedPtr<FJsonObject> Move = MakeShared<FJsonObject>();
		Move->SetStringField(TEXT("source_path"), Asset.ObjectPath);
		Move->SetStringField(TEXT("source_package_name"), Asset.PackageName);
		Move->SetStringField(TEXT("source_package_path"), Asset.PackagePath);
		Move->SetStringField(TEXT("asset_name"), Asset.AssetName);
		Move->SetStringField(TEXT("class"), Asset.ClassPath);
		Move->SetStringField(TEXT("destination_package_path"), TargetPackagePath);
		Move->SetStringField(TEXT("destination_path"), DestinationPath);
		Move->SetStringField(TEXT("status"), Status);
		Move->SetStringField(TEXT("discovered_by"), Asset.DiscoveredBy);
		Move->SetNumberField(TEXT("dependency_depth"), Asset.Depth);
		if (!Reason.IsEmpty())
		{
			Move->SetStringField(TEXT("reason"), Reason);
		}
		Moves.Add(MakeShared<FJsonValueObject>(Move));
	}

	TArray<TSharedPtr<FJsonValue>> ManagedRootJson;
	for (const FString& RootPath : ManagedRoots)
	{
		ManagedRootJson.Add(MakeShared<FJsonValueString>(RootPath));
	}

	TSharedPtr<FJsonObject> Root = MassBattleUnitEditorMCP::MakeSuccess();
	Root->SetStringField(TEXT("plan_type"), TEXT("unit_asset_organization"));
	Root->SetStringField(TEXT("unit_path"), UnitPath);
	Root->SetStringField(TEXT("style_profile"), StyleId);
	Root->SetStringField(TEXT("style_family"), StyleFamily);
	Root->SetStringField(TEXT("unit_name"), AssetSlug);
	Root->SetStringField(TEXT("target_package_path"), TargetPackagePath);
	Root->SetNumberField(TEXT("dependency_depth"), MaxDependencyDepth);
	Root->SetBoolField(TEXT("include_sibling_assets"), bIncludeSiblingAssets);
	Root->SetBoolField(TEXT("allow_plugin_content"), bAllowPluginContent);
	Root->SetBoolField(TEXT("applicable"), BlockedCount == 0);
	Root->SetNumberField(TEXT("asset_count"), LinkedAssets.Num());
	Root->SetNumberField(TEXT("move_count"), MoveCount);
	Root->SetNumberField(TEXT("already_in_place_count"), AlreadyInPlaceCount);
	Root->SetNumberField(TEXT("blocked_count"), BlockedCount);
	Root->SetArrayField(TEXT("managed_roots"), ManagedRootJson);
	Root->SetArrayField(TEXT("moves"), Moves);
	return MassBattleUnitEditorMCP::ToJsonString(Root);
}

FString UMassBattleUnitEditorMCPApi::MCP_EditorApplyOrganizeUnitAssets(const FString& UnitPath, const FString& OptionsJson, bool bSaveAssets)
{
	TSharedPtr<FJsonObject> Options = MassBattleUnitEditorMCP::ParseObject(OptionsJson);
	if (!Options.IsValid())
	{
		return MassBattleUnitEditorMCP::MakeErrorJson(TEXT("OptionsJson is not valid JSON"));
	}

	const FString PlanResult = MCP_EditorPlanOrganizeUnitAssets(UnitPath, OptionsJson);
	TSharedPtr<FJsonObject> Plan = MassBattleUnitEditorMCP::ParseObject(PlanResult);
	if (!Plan.IsValid() || !MassBattleUnitEditorMCP::JsonObjectFieldBool(Plan, TEXT("success"), false))
	{
		return PlanResult;
	}

	bool bDryRun = true;
	Options->TryGetBoolField(TEXT("dry_run"), bDryRun);
	Options->TryGetBoolField(TEXT("preview_only"), bDryRun);

	TSharedPtr<FJsonObject> Root = MassBattleUnitEditorMCP::MakeSuccess();
	Root->SetStringField(TEXT("editor_workflow"), TEXT("MassBattleEditor unit linked-asset organization apply"));
	Root->SetBoolField(TEXT("dry_run"), bDryRun);
	Root->SetBoolField(TEXT("save_assets"), bSaveAssets);
	Root->SetObjectField(TEXT("plan"), Plan);

	if (bDryRun)
	{
		Root->SetStringField(TEXT("note"), TEXT("dry_run=true; no assets were moved."));
		return MassBattleUnitEditorMCP::ToJsonString(Root);
	}

	if (!MassBattleUnitEditorMCP::JsonObjectFieldBool(Plan, TEXT("applicable"), false))
	{
		Root->SetBoolField(TEXT("success"), false);
		Root->SetStringField(TEXT("error"), TEXT("Organization plan has blocked moves; inspect plan.moves before applying."));
		return MassBattleUnitEditorMCP::ToJsonString(Root);
	}

	const TArray<TSharedPtr<FJsonValue>>* Moves = nullptr;
	if (!Plan->TryGetArrayField(TEXT("moves"), Moves) || !Moves)
	{
		Root->SetBoolField(TEXT("success"), false);
		Root->SetStringField(TEXT("error"), TEXT("Organization plan did not contain moves."));
		return MassBattleUnitEditorMCP::ToJsonString(Root);
	}

	TArray<FAssetRenameData> RenameData;
	TArray<FString> DestinationPaths;
	TArray<TSharedPtr<FJsonValue>> ExecutionSteps;
	for (const TSharedPtr<FJsonValue>& MoveValue : *Moves)
	{
		if (!MoveValue.IsValid() || MoveValue->Type != EJson::Object)
		{
			continue;
		}

		TSharedPtr<FJsonObject> Move = MoveValue->AsObject();
		FString Status;
		Move->TryGetStringField(TEXT("status"), Status);
		if (!Status.Equals(TEXT("would_move"), ESearchCase::IgnoreCase))
		{
			continue;
		}

		FString SourcePath;
		FString DestinationPackagePath;
		FString DestinationPath;
		FString AssetName;
		Move->TryGetStringField(TEXT("source_path"), SourcePath);
		Move->TryGetStringField(TEXT("destination_package_path"), DestinationPackagePath);
		Move->TryGetStringField(TEXT("destination_path"), DestinationPath);
		Move->TryGetStringField(TEXT("asset_name"), AssetName);

		UObject* SourceObject = FSoftObjectPath(MassBattleUnitEditorMCP::EnsureObjectPath(SourcePath)).TryLoad();
		TSharedPtr<FJsonObject> Step = MassBattleUnitEditorMCP::MakeStep(TEXT("move_asset"), TEXT("AssetTools.RenameAssets"), SourceObject ? TEXT("queued") : TEXT("failed"), SourcePath);
		Step->SetStringField(TEXT("source_path"), SourcePath);
		Step->SetStringField(TEXT("destination_path"), DestinationPath);
		if (!SourceObject)
		{
			Step->SetStringField(TEXT("error"), TEXT("Failed to load source asset."));
			ExecutionSteps.Add(MakeShared<FJsonValueObject>(Step));
			Root->SetBoolField(TEXT("success"), false);
			Root->SetArrayField(TEXT("execution_steps"), ExecutionSteps);
			return MassBattleUnitEditorMCP::ToJsonString(Root);
		}

		RenameData.Emplace(SourceObject, DestinationPackagePath, AssetName);
		DestinationPaths.Add(DestinationPath);
		ExecutionSteps.Add(MakeShared<FJsonValueObject>(Step));
	}

	if (RenameData.IsEmpty())
	{
		Root->SetNumberField(TEXT("moved_count"), 0);
		Root->SetArrayField(TEXT("execution_steps"), ExecutionSteps);
		return MassBattleUnitEditorMCP::ToJsonString(Root);
	}

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	const bool bMoved = AssetTools.RenameAssets(RenameData);
	Root->SetBoolField(TEXT("moved"), bMoved);
	Root->SetNumberField(TEXT("moved_count"), bMoved ? RenameData.Num() : 0);
	if (!bMoved)
	{
		Root->SetBoolField(TEXT("success"), false);
		Root->SetStringField(TEXT("error"), TEXT("AssetTools.RenameAssets failed."));
		Root->SetArrayField(TEXT("execution_steps"), ExecutionSteps);
		return MassBattleUnitEditorMCP::ToJsonString(Root);
	}

	for (const TSharedPtr<FJsonValue>& StepValue : ExecutionSteps)
	{
		if (StepValue.IsValid() && StepValue->Type == EJson::Object)
		{
			StepValue->AsObject()->SetStringField(TEXT("status"), TEXT("done"));
		}
	}

	if (bSaveAssets)
	{
		TArray<TSharedPtr<FJsonValue>> SaveResults;
		for (const FString& DestinationPath : DestinationPaths)
		{
			FString SaveError;
			TSharedPtr<FJsonObject> SaveResult = MakeShared<FJsonObject>();
			SaveResult->SetStringField(TEXT("path"), DestinationPath);
			const bool bSaved = MassBattleUnitEditorMCP::SaveAssetByPath(DestinationPath, SaveError);
			SaveResult->SetBoolField(TEXT("saved"), bSaved);
			if (!bSaved)
			{
				SaveResult->SetStringField(TEXT("error"), SaveError);
				Root->SetBoolField(TEXT("success"), false);
			}
			SaveResults.Add(MakeShared<FJsonValueObject>(SaveResult));
		}
		Root->SetArrayField(TEXT("save_results"), SaveResults);
	}

	Root->SetArrayField(TEXT("execution_steps"), ExecutionSteps);
	return MassBattleUnitEditorMCP::ToJsonString(Root);
}

FString UMassBattleUnitEditorMCPApi::MCP_EditorGetStatus()
{
	TArray<TSharedPtr<FJsonValue>> Tools;
	auto Tool = [](const FString& Name, const FString& Category, const FString& Description)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Name);
		Obj->SetStringField(TEXT("category"), Category);
		Obj->SetStringField(TEXT("description"), Description);
		return MakeShared<FJsonValueObject>(Obj);
	};

	Tools.Add(Tool(TEXT("MCP_EditorListProfiles"), TEXT("unit_editor.profile"), TEXT("List style profiles and authoring recipes used by the MCP editor.")));
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

	TSharedPtr<FJsonObject> Root = MassBattleUnitEditorMCP::MakeSuccess();
	Root->SetStringField(TEXT("api_name"), TEXT("MassBattleUnitEditorMCPApi"));
	Root->SetStringField(TEXT("description"), TEXT("MCP orchestration layer over MassBattleFrame/Source/MassBattleEditor. This is the non-UI counterpart of the MassBattleTools editor widget."));
	Root->SetArrayField(TEXT("tools"), Tools);
	return MassBattleUnitEditorMCP::ToJsonString(Root);
}
