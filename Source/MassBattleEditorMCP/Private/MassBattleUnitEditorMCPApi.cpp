// Copyright (c) 2025-2026 Winyunq. All rights reserved.
#include "MassBattleUnitEditorMCPApi.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "MassBattleEditorMCPApi.h"
#include "MassBattleUnitMCPApi.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"

DEFINE_LOG_CATEGORY(LogMassBattleUnitEditorMCPApi);

namespace MassBattleUnitEditorMCP
{
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
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("asset_slug"), TEXT("unit_slug"), TEXT("file_name") }, AssetSlug);
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

	FString SourceFolderName = MassBattleUnitEditorMCP::StringFieldOrDefault(AuthoringDefaults, TEXT("source_folder_name"), TEXT("Source_{asset_slug}"));
	Spec->TryGetStringField(TEXT("source_folder_name"), SourceFolderName);
	SourceFolderName.ReplaceInline(TEXT("{asset_slug}"), *AssetSlug);
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
	Layout->SetStringField(TEXT("asset_slug"), AssetSlug);
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
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("asset_slug"), TEXT("unit_slug"), TEXT("file_name") }, AssetSlug);
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

	FString GeneratedFolderName = MassBattleUnitEditorMCP::StringFieldOrDefault(AuthoringDefaults, TEXT("generated_folder_name"), TEXT("Gen_{asset_slug}"));
	GeneratedFolderName.ReplaceInline(TEXT("{asset_slug}"), *AssetSlug);

	FString GeneratedPackagePath = OutputRoot / GeneratedFolderName;
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("generated_package_path"), TEXT("output_package_path"), TEXT("package_path") }, GeneratedPackagePath);

	const FString StaticMeshName = MassBattleUnitEditorMCP::ResolveAssetName(Spec, AuthoringDefaults, TEXT("static_mesh_name"), TEXT("static_mesh_prefix"), TEXT("SM_"), AssetSlug);
	const FString UnitAssetName = MassBattleUnitEditorMCP::ResolveAssetName(Spec, AuthoringDefaults, TEXT("unit_asset_name"), TEXT("unit_asset_prefix"), TEXT("AgentConfig_"), AssetSlug);
	const FString RendererAssetName = MassBattleUnitEditorMCP::ResolveAssetName(Spec, AuthoringDefaults, TEXT("renderer_asset_name"), TEXT("renderer_prefix"), TEXT("Renderer_"), AssetSlug);
	const FString VatDataName = MassBattleUnitEditorMCP::ResolveAssetName(Spec, AuthoringDefaults, TEXT("vat_data_name"), TEXT("vat_data_prefix"), TEXT("VAT_"), AssetSlug);
	const FString MaterialName = MassBattleUnitEditorMCP::ResolveAssetName(Spec, AuthoringDefaults, TEXT("material_asset_name"), TEXT("material_prefix"), TEXT(""), AssetSlug);

	FString UnitPackagePath = GeneratedPackagePath;
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("unit_package_path") }, UnitPackagePath);
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

	TSharedPtr<FJsonObject> Layout = MakeShared<FJsonObject>();
	Layout->SetStringField(TEXT("style_profile"), StyleId);
	Layout->SetStringField(TEXT("recipe_id"), RecipeId);
	Layout->SetStringField(TEXT("style_family"), StyleFamily);
	Layout->SetStringField(TEXT("asset_slug"), AssetSlug);
	Layout->SetStringField(TEXT("generated_package_path"), GeneratedPackagePath);
	Layout->SetStringField(TEXT("unit_package_path"), UnitPackagePath);
	Layout->SetStringField(TEXT("static_mesh_path"), MassBattleUnitEditorMCP::MakeObjectPath(GeneratedPackagePath, StaticMeshName));
	Layout->SetStringField(TEXT("unit_path"), MassBattleUnitEditorMCP::MakeObjectPath(UnitPackagePath, UnitAssetName));
	Layout->SetStringField(TEXT("renderer_class_path"), PlannedRendererClassPath);
	Layout->SetStringField(TEXT("vat_data_asset_path"), MassBattleUnitEditorMCP::MakeObjectPath(GeneratedPackagePath, VatDataName));
	Layout->SetStringField(TEXT("static_mesh_asset_name"), StaticMeshName);
	Layout->SetStringField(TEXT("unit_asset_name"), UnitAssetName);
	Layout->SetStringField(TEXT("renderer_asset_name"), RendererAssetName);
	Layout->SetStringField(TEXT("material_asset_name"), MaterialName);
	Layout->SetStringField(TEXT("material_name_prefix"), MaterialName);

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
			const FString AnimResult = UMassBattleEditorMCPApi::MCP_FindAndFillAnimSequences(SkeletalMeshPath, AnimationSearchPath, AssetSlug);
			TSharedPtr<FJsonObject> AnimJson = MassBattleUnitEditorMCP::ParseObject(AnimResult);
			if (!AnimJson.IsValid() || !AnimJson->GetBoolField(TEXT("success")))
			{
				return AnimResult;
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
	TSharedPtr<FJsonObject> UnitCreateSpec;
	if (!TemplateUnitPath.IsEmpty())
	{
		UnitCreateSpec = MakeShared<FJsonObject>();
		UnitCreateSpec->SetStringField(TEXT("template_unit"), TemplateUnitPath);
		UnitCreateSpec->SetStringField(TEXT("asset_name"), UnitAssetName);
		UnitCreateSpec->SetStringField(TEXT("package_path"), UnitPackagePath);
		Discovery->SetObjectField(TEXT("unit_create_spec"), UnitCreateSpec);
	}

	FString ParentMaterialPath;
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("parent_material") }, ParentMaterialPath);
	MassBattleUnitEditorMCP::AddStep(Steps, TEXT("convert_mesh"), TEXT("MCP_ConvertSkeletalMeshToStaticMeshWithLODs"), SkeletalMeshPath.IsEmpty() ? TEXT("blocked") : TEXT("planned"), TEXT("Convert the source skeletal mesh into the planned MassBattle static mesh asset."));
	MassBattleUnitEditorMCP::AddStep(Steps, TEXT("create_materials"), TEXT("MCP_CreateMaterialInstanceForStaticMeshWithLODs"), ParentMaterialPath.IsEmpty() ? TEXT("blocked") : TEXT("planned"), TEXT("Create material instances for the generated static mesh LOD material slots."));
	MassBattleUnitEditorMCP::AddStep(Steps, TEXT("apply_material_overrides"), TEXT("MCP_EditorApplyCreateVatUnit.material_overrides"), MassBattleUnitEditorMCP::HasMaterialOverrides(Spec) ? TEXT("planned") : TEXT("skipped"), TEXT("Optionally override generated StaticMesh material slots with explicit materials."));
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
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("parent_material") }, ParentMaterialPath);
	FString SourceRendererClassPath;
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("source_renderer_class"), TEXT("renderer_template_class"), TEXT("template_renderer_class") }, SourceRendererClassPath);
	SourceRendererClassPath = MassBattleUnitEditorMCP::EnsureGeneratedClassPath(SourceRendererClassPath);
	FString NiagaraSystemPath;
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("niagara_system"), TEXT("niagara"), TEXT("niagara_system_asset") }, NiagaraSystemPath);

	const FString StaticMeshPath = Layout.IsValid() ? MassBattleUnitEditorMCP::StringFieldOrDefault(Layout, TEXT("static_mesh_path"), FString()) : FString();
	const FString RendererClassPath = Layout.IsValid() ? MassBattleUnitEditorMCP::EnsureGeneratedClassPath(MassBattleUnitEditorMCP::StringFieldOrDefault(Layout, TEXT("renderer_class_path"), FString())) : FString();
	const FString PlannedUnitPath = Layout.IsValid() ? MassBattleUnitEditorMCP::StringFieldOrDefault(Layout, TEXT("unit_path"), FString()) : FString();

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

	if (TargetUnitPath.IsEmpty() && TemplateUnitPath.IsEmpty())
	{
		MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("missing_unit_target"), TEXT("target_unit or template_unit is required to write unit data."), TEXT("target_unit"));
		MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("merge_unit_data"), TEXT("blocked"), TEXT("No unit target/template was supplied."));
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
		if (!MassBattleUnitEditorMCP::AssetExists(TemplateUnitPath))
		{
			MassBattleUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("template_unit_not_found"), FString::Printf(TEXT("template_unit does not exist or failed to load: %s"), *TemplateUnitPath), TEXT("template_unit"));
			MassBattleUnitEditorMCP::AddExecutionPreview(ExecutionPreview, TEXT("clone_unit"), TEXT("blocked"), TEXT("template_unit cannot be loaded."));
		}
		else
		{
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
	Root->SetStringField(TEXT("editor_workflow"), TEXT("MassBattleEditor VAT skeletal unit authoring apply"));
	Root->SetBoolField(TEXT("dry_run"), bDryRun);
	Root->SetBoolField(TEXT("save_assets"), bSaveAssets);
	Root->SetObjectField(TEXT("plan"), Plan);

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
	const FString GeneratedPackagePath = MassBattleUnitEditorMCP::StringFieldOrDefault(Layout, TEXT("generated_package_path"), FString());
	const FString StaticMeshAssetName = MassBattleUnitEditorMCP::StringFieldOrDefault(Layout, TEXT("static_mesh_asset_name"), MassBattleUnitEditorMCP::AssetNameFromObjectPath(StaticMeshPath));
	const FString MaterialAssetName = MassBattleUnitEditorMCP::StringFieldOrDefault(Layout, TEXT("material_asset_name"), MassBattleUnitEditorMCP::StringFieldOrDefault(Layout, TEXT("asset_slug"), StaticMeshAssetName));
	FString RendererClassPath = MassBattleUnitEditorMCP::EnsureGeneratedClassPath(MassBattleUnitEditorMCP::StringFieldOrDefault(Layout, TEXT("renderer_class_path"), FString()));
	const FString RendererAssetName = MassBattleUnitEditorMCP::StringFieldOrDefault(Layout, TEXT("renderer_asset_name"), MassBattleUnitEditorMCP::AssetNameFromObjectPath(RendererClassPath));
	const FString RendererPackagePath = MassBattleUnitEditorMCP::PackagePathFromObjectPath(RendererClassPath);

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
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("parent_material") }, ParentMaterialPath);
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

		TSharedPtr<FJsonObject> Step = MassBattleUnitEditorMCP::AddExecutionStep(ExecutionSteps, TEXT("create_materials"), TEXT("MCP_CreateMaterialInstanceForStaticMeshWithLODs"), TEXT("running"), TEXT("Creating material instances and assigning them to the generated StaticMesh."));
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

	FString SourceRendererClassPath;
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("source_renderer_class"), TEXT("renderer_template_class"), TEXT("template_renderer_class") }, SourceRendererClassPath);
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
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("niagara_system"), TEXT("niagara"), TEXT("niagara_system_asset") }, NiagaraSystemPath);
	int32 SubType = 0;
	double SubTypeNumber = 0.0;
	if (Spec->TryGetNumberField(TEXT("subtype"), SubTypeNumber) || Spec->TryGetNumberField(TEXT("sub_type"), SubTypeNumber))
	{
		SubType = static_cast<int32>(SubTypeNumber);
	}
	if (!RendererClassPath.IsEmpty() && MassBattleUnitEditorMCP::AssetExists(RendererClassPath))
	{
		TSharedPtr<FJsonObject> Step = MassBattleUnitEditorMCP::AddExecutionStep(ExecutionSteps, TEXT("set_renderer_defaults"), TEXT("MCP_SetClassDefaultProperties"), TEXT("running"), TEXT("Setting renderer CDO mesh, Niagara system, and SubType."));
		const FString DefaultsResult = UMassBattleEditorMCPApi::MCP_SetClassDefaultProperties(RendererClassPath, StaticMeshPath, NiagaraSystemPath, SubType);
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
	FString TemplateUnitPath;
	MassBattleUnitEditorMCP::TryGetObjectPathSpec(Spec, { TEXT("template_unit"), TEXT("source_unit") }, TemplateUnitPath);
	const FString PlannedUnitPath = MassBattleUnitEditorMCP::StringFieldOrDefault(Layout, TEXT("unit_path"), FString());

	if (!TargetUnitPath.IsEmpty())
	{
		const TSharedPtr<FJsonObject>* ExistingMergePlan = nullptr;
		if (Discovery->TryGetObjectField(TEXT("existing_unit_merge_plan"), ExistingMergePlan) && ExistingMergePlan && ExistingMergePlan->IsValid())
		{
			FString PlanId;
			(*ExistingMergePlan)->TryGetStringField(TEXT("plan_id"), PlanId);
			TSharedPtr<FJsonObject> Step = MassBattleUnitEditorMCP::AddExecutionStep(ExecutionSteps, TEXT("merge_unit_data"), TEXT("MCP_UnitApplyPlan"), TEXT("running"), TEXT("Applying existing-unit merge plan."));
			const FString ApplyResult = UMassBattleUnitMCPApi::MCP_UnitApplyPlan(PlanId, bSaveAssets);
			MassBattleUnitEditorMCP::SetStepResult(Step, ApplyResult);
			TSharedPtr<FJsonObject> ApplyJson = MassBattleUnitEditorMCP::ParseObject(ApplyResult);
			Step->SetStringField(TEXT("status"), ApplyJson.IsValid() && ApplyJson->GetBoolField(TEXT("success")) ? TEXT("done") : TEXT("failed"));
			if (!ApplyJson.IsValid() || !ApplyJson->GetBoolField(TEXT("success")))
			{
				Root->SetBoolField(TEXT("success"), false);
				Root->SetArrayField(TEXT("execution_steps"), ExecutionSteps);
				return MassBattleUnitEditorMCP::ToJsonString(Root);
			}
		}
		else
		{
			MassBattleUnitEditorMCP::AddExecutionStep(ExecutionSteps, TEXT("merge_unit_data"), TEXT("MCP_UnitApplyPlan"), TEXT("blocked"), TEXT("Plan did not contain an existing_unit_merge_plan."));
		}
	}
	else if (!TemplateUnitPath.IsEmpty())
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
		}
		else
		{
			MassBattleUnitEditorMCP::AddExecutionStep(ExecutionSteps, TEXT("create_unit"), TEXT("MCP_UnitCreate"), TEXT("blocked"), TEXT("Plan did not contain a unit_create_spec."));
		}
	}
	else
	{
		MassBattleUnitEditorMCP::AddExecutionStep(ExecutionSteps, TEXT("merge_unit_data"), TEXT("MCP_UnitPlanMergeUpdate"), TEXT("blocked"), TEXT("target_unit or template_unit is required to write unit data."));
		Root->SetBoolField(TEXT("success"), false);
		Root->SetArrayField(TEXT("execution_steps"), ExecutionSteps);
		return MassBattleUnitEditorMCP::ToJsonString(Root);
	}

	if (bSaveAssets)
	{
		TArray<FString> SavePaths = { StaticMeshPath, MassBattleUnitEditorMCP::BlueprintPathFromGeneratedClassPath(RendererClassPath) };
		MassBattleUnitEditorMCP::AddStaticMeshMaterialPaths(StaticMeshPath, GeneratedPackagePath, SavePaths);
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
	FString AssetSlug = MassBattleUnitEditorMCP::StringFieldOrDefault(Options, TEXT("asset_slug"), UnitAssetName);
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

	FString GeneratedFolderName = MassBattleUnitEditorMCP::StringFieldOrDefault(AuthoringDefaults, TEXT("generated_folder_name"), TEXT("Gen_{asset_slug}"));
	Options->TryGetStringField(TEXT("generated_folder_name"), GeneratedFolderName);
	Options->TryGetStringField(TEXT("target_folder_name"), GeneratedFolderName);
	GeneratedFolderName.ReplaceInline(TEXT("{asset_slug}"), *AssetSlug);

	FString TargetPackagePath = OutputRoot / GeneratedFolderName;
	Options->TryGetStringField(TEXT("target_package_path"), TargetPackagePath);
	Options->TryGetStringField(TEXT("output_package_path"), TargetPackagePath);

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
	Root->SetStringField(TEXT("asset_slug"), AssetSlug);
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
	Tools.Add(Tool(TEXT("MCP_EditorPlanCreateVatUnit"), TEXT("unit_editor.create"), TEXT("Plan the MassBattleEditor VAT skeletal mesh authoring workflow without writing generated assets.")));
	Tools.Add(Tool(TEXT("MCP_EditorValidateCreateVatUnit"), TEXT("unit_editor.create"), TEXT("Validate required inputs, asset conflicts, and execution readiness for VAT unit authoring.")));
	Tools.Add(Tool(TEXT("MCP_EditorApplyCreateVatUnit"), TEXT("unit_editor.create"), TEXT("Execute the MassBattleEditor VAT skeletal mesh authoring workflow with dry_run and overwrite safety options.")));
	Tools.Add(Tool(TEXT("MCP_EditorPlanOrganizeUnitAssets"), TEXT("unit_editor.organize"), TEXT("Plan moving a unit and its editor-generated linked assets into the selected style layout.")));
	Tools.Add(Tool(TEXT("MCP_EditorApplyOrganizeUnitAssets"), TEXT("unit_editor.organize"), TEXT("Apply a reviewed linked-asset organization plan; dry_run=true by default.")));

	TSharedPtr<FJsonObject> Root = MassBattleUnitEditorMCP::MakeSuccess();
	Root->SetStringField(TEXT("api_name"), TEXT("MassBattleUnitEditorMCPApi"));
	Root->SetStringField(TEXT("description"), TEXT("MCP orchestration layer over MassBattleFrame/Source/MassBattleEditor. This is the non-UI counterpart of the MassBattleTools editor widget."));
	Root->SetArrayField(TEXT("tools"), Tools);
	return MassBattleUnitEditorMCP::ToJsonString(Root);
}
