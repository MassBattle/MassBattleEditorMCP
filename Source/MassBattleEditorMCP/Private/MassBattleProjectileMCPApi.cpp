// Copyright (c) 2025-2026 Winyunq. All rights reserved.
#include "MassBattleProjectileMCPApi.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "DataAssets/MassBattleAgentConfigDataAsset.h"
#include "DataAssets/MassBattleProjectileConfigDataAsset.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Factories/DataAssetFactory.h"
#include "IAssetTools.h"
#include "JsonObjectConverter.h"
#include "Misc/DateTime.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY(LogMassBattleProjectileMCPApi);

namespace MassBattleProjectileMCP
{
static constexpr int32 DefaultListLimit = 200;
static constexpr int32 MaxListLimit = 2000;
static constexpr int32 DefaultSchemaDepth = 5;

struct FMergeOptions
{
	bool bAppendArrays = false;
	bool bReplaceArrays = false;
	bool bValidate = true;
	bool bBlockOnValidationError = true;
};

struct FValidationIssue
{
	FString Severity;
	FString Code;
	FString Path;
	FString Message;
};

static FString ToJsonString(const TSharedPtr<FJsonObject>& Object)
{
	FString Output;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	if (Object.IsValid())
	{
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}
	return Output;
}

static FString JsonValueToString(const TSharedPtr<FJsonValue>& Value)
{
	TSharedPtr<FJsonObject> Wrapper = MakeShared<FJsonObject>();
	Wrapper->SetField(TEXT("value"), Value.IsValid() ? Value : MakeShared<FJsonValueNull>());
	return ToJsonString(Wrapper);
}

static TSharedPtr<FJsonObject> ParseObject(const FString& Json)
{
	if (Json.TrimStartAndEnd().IsEmpty())
	{
		return MakeShared<FJsonObject>();
	}

	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
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

	if (Path.StartsWith(TEXT("/")) && !Path.Contains(TEXT(".")))
	{
		const FString AssetName = FPackageName::GetLongPackageAssetName(Path);
		if (!AssetName.IsEmpty())
		{
			Path = FString::Printf(TEXT("%s.%s"), *Path, *AssetName);
		}
	}
	return Path;
}

static UMassBattleProjectileConfigDataAsset* LoadProjectile(const FString& Path, FString& OutError)
{
	const FString ObjectPath = NormalizeObjectPath(Path);
	if (ObjectPath.IsEmpty())
	{
		OutError = TEXT("ProjectilePath is empty");
		return nullptr;
	}

	UMassBattleProjectileConfigDataAsset* Asset = Cast<UMassBattleProjectileConfigDataAsset>(FSoftObjectPath(ObjectPath).TryLoad());
	if (!Asset)
	{
		Asset = LoadObject<UMassBattleProjectileConfigDataAsset>(nullptr, *ObjectPath);
	}
	if (!Asset)
	{
		OutError = FString::Printf(TEXT("Failed to load UMassBattleProjectileConfigDataAsset: %s"), *ObjectPath);
	}
	return Asset;
}

static bool SaveAsset(UObject* Asset, FString& OutError)
{
	if (!Asset || !Asset->GetOutermost())
	{
		OutError = TEXT("Projectile asset or package is null");
		return false;
	}

	UPackage* Package = Asset->GetOutermost();
	const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (!UPackage::SavePackage(Package, Asset, *Filename, SaveArgs))
	{
		OutError = FString::Printf(TEXT("Failed to save package: %s"), *Filename);
		return false;
	}
	return true;
}

static IAssetRegistry& GetAssetRegistry()
{
	return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
}

static TArray<FName> ReadRoots(const TSharedPtr<FJsonObject>& Options)
{
	TArray<FName> Roots;
	const TArray<TSharedPtr<FJsonValue>>* RootArray = nullptr;
	if (Options.IsValid() && Options->TryGetArrayField(TEXT("roots"), RootArray))
	{
		for (const TSharedPtr<FJsonValue>& Value : *RootArray)
		{
			const FString Root = Value.IsValid() ? Value->AsString() : FString();
			if (Root.StartsWith(TEXT("/")))
			{
				Roots.AddUnique(FName(*Root));
			}
		}
	}

	FString SingleRoot;
	if (Options.IsValid() && (Options->TryGetStringField(TEXT("root"), SingleRoot) || Options->TryGetStringField(TEXT("path"), SingleRoot)))
	{
		if (SingleRoot.StartsWith(TEXT("/")))
		{
			Roots.AddUnique(FName(*SingleRoot));
		}
	}

	if (Roots.IsEmpty())
	{
		Roots.Add(FName(TEXT("/Game")));
		Roots.Add(FName(TEXT("/MassBattle")));
	}
	return Roots;
}

static TArray<FAssetData> ScanProjectileAssets(const TSharedPtr<FJsonObject>& Options)
{
	const TArray<FName> Roots = ReadRoots(Options);
	TArray<FString> RootStrings;
	for (const FName& Root : Roots)
	{
		RootStrings.Add(Root.ToString());
	}

	IAssetRegistry& Registry = GetAssetRegistry();
	Registry.ScanPathsSynchronous(RootStrings, true);

	FARFilter Filter;
	Filter.PackagePaths = Roots;
	Filter.ClassPaths.Add(UMassBattleProjectileConfigDataAsset::StaticClass()->GetClassPathName());
	Filter.bRecursivePaths = true;
	Filter.bRecursiveClasses = true;

	TArray<FAssetData> Assets;
	Registry.GetAssets(Filter, Assets);
	Assets.Sort([](const FAssetData& A, const FAssetData& B)
	{
		return A.GetObjectPathString() < B.GetObjectPathString();
	});
	return Assets;
}

static FString MoveModeName(const EProjectileMoveMode Mode)
{
	const UEnum* Enum = StaticEnum<EProjectileMoveMode>();
	return Enum ? Enum->GetNameStringByValue(static_cast<int64>(Mode)) : FString::FromInt(static_cast<int32>(Mode));
}

static FString DamageModeName(const EProjectileDamageMode Mode)
{
	const UEnum* Enum = StaticEnum<EProjectileDamageMode>();
	return Enum ? Enum->GetNameStringByValue(static_cast<int64>(Mode)) : FString::FromInt(static_cast<int32>(Mode));
}

static TSharedPtr<FJsonObject> ProjectileDataToJson(UMassBattleProjectileConfigDataAsset* Asset, bool bIncludeInactive)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	if (!Asset)
	{
		return Data;
	}

	constexpr int64 SkipFlags = CPF_Transient | CPF_Deprecated | CPF_EditorOnly;
	FJsonObjectConverter::UStructToJsonObject(
		Asset->GetClass(),
		Asset,
		Data.ToSharedRef(),
		0,
		SkipFlags,
		nullptr,
		EJsonObjectConversionFlags::SkipStandardizeCase);

	if (!bIncludeInactive)
	{
		static const TArray<FString> MovementFields = {
			TEXT("ProjectileMove_Static"), TEXT("ProjectileMove_Interped"), TEXT("ProjectileMove_Ballistic"), TEXT("ProjectileMove_Tracking")
		};
		const FString ActiveMovement = TEXT("ProjectileMove_") + MoveModeName(Asset->MovementMode);
		for (const FString& Field : MovementFields)
		{
			if (Field != ActiveMovement)
			{
				Data->RemoveField(Field);
			}
		}

		static const TArray<FString> DamageFields = {
			TEXT("Damage_Point"), TEXT("Damage_Radial"), TEXT("Damage_Beam"),
			TEXT("Debuff_Point"), TEXT("Debuff_Radial"), TEXT("Debuff_Beam")
		};
		const FString ActiveDamage = TEXT("Damage_") + DamageModeName(Asset->DamageMode);
		const FString ActiveDebuff = TEXT("Debuff_") + DamageModeName(Asset->DamageMode);
		for (const FString& Field : DamageFields)
		{
			if (Field != ActiveDamage && Field != ActiveDebuff)
			{
				Data->RemoveField(Field);
			}
		}
	}
	return Data;
}

static TSharedPtr<FJsonObject> AssetSummary(const FAssetData& AssetData, bool bLoad)
{
	TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
	Item->SetStringField(TEXT("path"), AssetData.GetObjectPathString());
	Item->SetStringField(TEXT("package"), AssetData.PackageName.ToString());
	Item->SetStringField(TEXT("class"), AssetData.AssetClassPath.ToString());
	if (bLoad)
	{
		if (UMassBattleProjectileConfigDataAsset* Asset = Cast<UMassBattleProjectileConfigDataAsset>(AssetData.GetAsset()))
		{
			Item->SetStringField(TEXT("movement_mode"), MoveModeName(Asset->MovementMode));
			Item->SetStringField(TEXT("damage_mode"), DamageModeName(Asset->DamageMode));
			Item->SetNumberField(TEXT("lifespan"), Asset->ProjectileParams.LifeSpan);
			Item->SetNumberField(TEXT("collision_radius"), Asset->ProjectileParams.Radius);
		}
	}
	return Item;
}

static bool IsEditableProperty(const FProperty* Property)
{
	return Property
		&& !Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_EditorOnly | CPF_DisableEditOnInstance);
}

static UEnum* GetPropertyEnum(FProperty* Property)
{
	if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
	{
		return EnumProperty->GetEnum();
	}
	if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
	{
		return ByteProperty->Enum;
	}
	return nullptr;
}

static TArray<TSharedPtr<FJsonValue>> EnumValuesToJson(UEnum* Enum)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	if (!Enum)
	{
		return Values;
	}
	for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
	{
		if (!Enum->HasMetaData(TEXT("Hidden"), Index))
		{
			Values.Add(MakeShared<FJsonValueString>(Enum->GetNameStringByIndex(Index)));
		}
	}
	return Values;
}

static void AddSchemaFields(TArray<TSharedPtr<FJsonValue>>& Fields, UStruct* Struct, const FString& Prefix, int32 Depth)
{
	if (!Struct || Depth < 0)
	{
		return;
	}

	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		FProperty* Property = *It;
		if (!IsEditableProperty(Property))
		{
			continue;
		}

		const FString Name = Property->GetAuthoredName();
		const FString Path = Prefix.IsEmpty() ? Name : Prefix + TEXT(".") + Name;
		TSharedPtr<FJsonObject> Field = MakeShared<FJsonObject>();
		Field->SetStringField(TEXT("path"), Path);
		Field->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
		Field->SetBoolField(TEXT("editable"), true);
		Field->SetStringField(TEXT("category"), Property->GetMetaData(TEXT("Category")));
		Field->SetStringField(TEXT("tooltip"), Property->GetMetaData(TEXT("ToolTip")));
		const FString EditCondition = Property->GetMetaData(TEXT("EditCondition"));
		if (!EditCondition.IsEmpty())
		{
			Field->SetStringField(TEXT("edit_condition"), EditCondition);
		}
		if (UEnum* Enum = GetPropertyEnum(Property))
		{
			Field->SetArrayField(TEXT("enum_values"), EnumValuesToJson(Enum));
		}
		Fields.Add(MakeShared<FJsonValueObject>(Field));

		if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			AddSchemaFields(Fields, StructProperty->Struct, Path, Depth - 1);
		}
		else if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			if (FStructProperty* InnerStruct = CastField<FStructProperty>(ArrayProperty->Inner))
			{
				AddSchemaFields(Fields, InnerStruct->Struct, Path + TEXT("[]"), Depth - 1);
			}
		}
	}
}

static void ReadMergeOptions(const TSharedPtr<FJsonObject>& Root, FMergeOptions& Out)
{
	const TSharedPtr<FJsonObject>* OptionsObject = nullptr;
	TSharedPtr<FJsonObject> Options = Root;
	if (Root.IsValid() && Root->TryGetObjectField(TEXT("options"), OptionsObject) && OptionsObject && OptionsObject->IsValid())
	{
		Options = *OptionsObject;
	}
	if (!Options.IsValid())
	{
		return;
	}

	Options->TryGetBoolField(TEXT("append_arrays"), Out.bAppendArrays);
	Options->TryGetBoolField(TEXT("replace_arrays"), Out.bReplaceArrays);
	Options->TryGetBoolField(TEXT("validate"), Out.bValidate);
	Options->TryGetBoolField(TEXT("block_on_validation_error"), Out.bBlockOnValidationError);
	FString ArrayMode;
	if (Options->TryGetStringField(TEXT("array_mode"), ArrayMode))
	{
		Out.bReplaceArrays = ArrayMode.Equals(TEXT("replace"), ESearchCase::IgnoreCase);
	}
}

static TSharedPtr<FJsonObject> ExtractPatchData(const TSharedPtr<FJsonObject>& Root, bool bCreateSpec)
{
	if (!Root.IsValid())
	{
		return nullptr;
	}

	const TSharedPtr<FJsonObject>* Data = nullptr;
	for (const TCHAR* Field : { TEXT("data"), TEXT("projectile_data"), TEXT("initial_data") })
	{
		if (Root->TryGetObjectField(Field, Data) && Data && Data->IsValid())
		{
			return *Data;
		}
	}

	TSharedPtr<FJsonObject> Direct = MakeShared<FJsonObject>();
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Root->Values)
	{
		if (Pair.Key == TEXT("options") || (bCreateSpec && (
			Pair.Key == TEXT("asset_name") || Pair.Key == TEXT("name") || Pair.Key == TEXT("package_path")
			|| Pair.Key == TEXT("target_package_path") || Pair.Key == TEXT("template_path") || Pair.Key == TEXT("template"))))
		{
			continue;
		}
		Direct->SetField(Pair.Key, Pair.Value);
	}
	return Direct;
}

static bool ApplyMergeValue(
	FProperty* Property,
	void* ValuePtr,
	const TSharedPtr<FJsonValue>& Value,
	const FMergeOptions& Options,
	const FString& Path,
	TArray<FString>& ChangedPaths,
	TArray<FString>& Errors)
{
	if (!Property || !ValuePtr || !Value.IsValid())
	{
		Errors.Add(FString::Printf(TEXT("%s: invalid property or JSON value"), *Path));
		return false;
	}
	if (!IsEditableProperty(Property))
	{
		Errors.Add(FString::Printf(TEXT("%s: property is not editable"), *Path));
		return false;
	}
	if (Value->Type == EJson::Null)
	{
		Errors.Add(FString::Printf(TEXT("%s: null reset is not implicit; provide an explicit source value"), *Path));
		return false;
	}

	if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
	{
		if (Value->Type == EJson::Object)
		{
			bool bOk = true;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Value->AsObject()->Values)
			{
				FProperty* Child = StructProperty->Struct->FindPropertyByName(FName(*Pair.Key));
				if (!Child)
				{
					Errors.Add(FString::Printf(TEXT("%s.%s: property does not exist"), *Path, *Pair.Key));
					bOk = false;
					continue;
				}
				bOk &= ApplyMergeValue(
					Child,
					Child->ContainerPtrToValuePtr<void>(ValuePtr),
					Pair.Value,
					Options,
					Path + TEXT(".") + Pair.Key,
					ChangedPaths,
					Errors);
			}
			return bOk;
		}
	}

	if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
	{
		if (Value->Type != EJson::Array)
		{
			Errors.Add(FString::Printf(TEXT("%s: array property requires a JSON array"), *Path));
			return false;
		}
		if (!Options.bReplaceArrays)
		{
			FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
			bool bOk = true;
			const TArray<TSharedPtr<FJsonValue>>& Values = Value->AsArray();
			for (int32 Index = 0; Index < Values.Num(); ++Index)
			{
				if (!Helper.IsValidIndex(Index))
				{
					if (!Options.bAppendArrays || Index != Helper.Num())
					{
						Errors.Add(FString::Printf(TEXT("%s[%d]: index requires options.append_arrays=true"), *Path, Index));
						bOk = false;
						continue;
					}
					Helper.AddValue();
				}
				bOk &= ApplyMergeValue(
					ArrayProperty->Inner,
					Helper.GetRawPtr(Index),
					Values[Index],
					Options,
					FString::Printf(TEXT("%s[%d]"), *Path, Index),
					ChangedPaths,
					Errors);
			}
			return bOk;
		}
	}

	const FString Before = JsonValueToString(FJsonObjectConverter::UPropertyToJsonValue(Property, ValuePtr, 0, CPF_Transient | CPF_Deprecated | CPF_EditorOnly, nullptr, nullptr, EJsonObjectConversionFlags::SkipStandardizeCase));
	FText FailureReason;
	if (!FJsonObjectConverter::JsonValueToUProperty(
		Value,
		Property,
		ValuePtr,
		0,
		CPF_Transient | CPF_Deprecated | CPF_EditorOnly,
		false,
		&FailureReason))
	{
		Errors.Add(FString::Printf(TEXT("%s: %s"), *Path, *FailureReason.ToString()));
		return false;
	}
	const FString After = JsonValueToString(FJsonObjectConverter::UPropertyToJsonValue(Property, ValuePtr, 0, CPF_Transient | CPF_Deprecated | CPF_EditorOnly, nullptr, nullptr, EJsonObjectConversionFlags::SkipStandardizeCase));
	if (Before != After)
	{
		ChangedPaths.AddUnique(Path);
	}
	return true;
}

static bool ApplySourceAlignedPatch(
	UMassBattleProjectileConfigDataAsset* Asset,
	const TSharedPtr<FJsonObject>& Data,
	const FMergeOptions& Options,
	TArray<FString>& ChangedPaths,
	TArray<FString>& Errors)
{
	if (!Asset || !Data.IsValid())
	{
		Errors.Add(TEXT("Projectile asset or patch data is invalid"));
		return false;
	}

	bool bOk = true;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Data->Values)
	{
		FProperty* Property = Asset->GetClass()->FindPropertyByName(FName(*Pair.Key));
		if (!Property)
		{
			Errors.Add(FString::Printf(TEXT("%s: top-level property does not exist"), *Pair.Key));
			bOk = false;
			continue;
		}
		bOk &= ApplyMergeValue(
			Property,
			Property->ContainerPtrToValuePtr<void>(Asset),
			Pair.Value,
			Options,
			Pair.Key,
			ChangedPaths,
			Errors);
	}
	return bOk;
}

static TArray<TSharedPtr<FJsonValue>> StringsToJson(const TArray<FString>& Strings)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FString& String : Strings)
	{
		Values.Add(MakeShared<FJsonValueString>(String));
	}
	return Values;
}

static bool AnyTrigger(const FProjectileTriggerConditions& Trigger)
{
	return Trigger.bOnArrival || Trigger.bOnNoLifeSpan || Trigger.bOnNoHealth || Trigger.bOnHitObstacle || Trigger.bOnHitEntity;
}

static void AddIssue(TArray<FValidationIssue>& Issues, const TCHAR* Severity, const TCHAR* Code, const TCHAR* Path, const FString& Message)
{
	Issues.Add({ Severity, Code, Path, Message });
}

static FString EffectiveNiagaraPath(const FFxConfig_Attack& Fx)
{
	const FSoftObjectPath SoftPath = Fx.SoftNiagaraAsset.ToSoftObjectPath();
	if (SoftPath.IsValid())
	{
		return SoftPath.ToString();
	}
	return Fx.NiagaraAsset ? Fx.NiagaraAsset->GetPathName() : FString();
}

static FString EffectiveCascadePath(const FFxConfig_Attack& Fx)
{
	const FSoftObjectPath SoftPath = Fx.SoftCascadeAsset.ToSoftObjectPath();
	if (SoftPath.IsValid())
	{
		return SoftPath.ToString();
	}
	return Fx.CascadeAsset ? Fx.CascadeAsset->GetPathName() : FString();
}

static bool EquivalentEnabledFx(const FFxConfig_Attack& A, const FFxConfig_Attack& B)
{
	return A.bEnable && B.bEnable
		&& A.SubType == B.SubType
		&& A.StyleType == B.StyleType
		&& EffectiveNiagaraPath(A) == EffectiveNiagaraPath(B)
		&& EffectiveCascadePath(A) == EffectiveCascadePath(B)
		&& A.Transform.Equals(B.Transform)
		&& A.bAttached == B.bAttached
		&& A.SpawnOrigin == B.SpawnOrigin
		&& A.Quantity == B.Quantity
		&& FMath::IsNearlyEqual(A.Delay, B.Delay)
		&& FMath::IsNearlyEqual(A.LifeSpan, B.LifeSpan)
		&& A.BindToAnimIndex == B.BindToAnimIndex
		&& A.bDespawnWhenNoParent == B.bDespawnWhenNoParent
		&& A.SpawnProbability.Equals(B.SpawnProbability)
		&& A.AgentBehaviorState == B.AgentBehaviorState;
}

static bool EnabledSpawnFxEquivalent(const FProjectileSpawnContent& A, const FProjectileSpawnContent& B)
{
	TArray<const FFxConfig_Attack*> EnabledA;
	TArray<const FFxConfig_Attack*> EnabledB;
	for (const FFxConfig_Attack& Fx : A.SpawnFx)
	{
		if (Fx.bEnable)
		{
			EnabledA.Add(&Fx);
		}
	}
	for (const FFxConfig_Attack& Fx : B.SpawnFx)
	{
		if (Fx.bEnable)
		{
			EnabledB.Add(&Fx);
		}
	}
	if (EnabledA.Num() != EnabledB.Num() || EnabledA.IsEmpty())
	{
		return false;
	}
	for (int32 Index = 0; Index < EnabledA.Num(); ++Index)
	{
		if (!EquivalentEnabledFx(*EnabledA[Index], *EnabledB[Index]))
		{
			return false;
		}
	}
	return true;
}

static bool HasEnabledFx(const FProjectileSpawnContent& Content)
{
	for (const FFxConfig_Attack& Fx : Content.SpawnFx)
	{
		if (Fx.bEnable)
		{
			return true;
		}
	}
	return false;
}

static void ValidateLifecycleContent(
	UMassBattleProjectileConfigDataAsset* Asset,
	const FProjectileSpawnContent& Content,
	const FString& Prefix,
	bool bTerminal,
	TArray<FValidationIssue>& Issues)
{
	for (int32 Index = 0; Index < Content.SpawnFx.Num(); ++Index)
	{
		const FFxConfig_Attack& Fx = Content.SpawnFx[Index];
		if (!Fx.bEnable)
		{
			continue;
		}
		const FString Path = FString::Printf(TEXT("%s.SpawnFx[%d]"), *Prefix, Index);
		if (!Fx.SoftNiagaraAsset.IsNull() || !Fx.SoftCascadeAsset.IsNull())
		{
			AddIssue(Issues, TEXT("warning"), TEXT("unbatched_lifecycle_fx"), *Path,
				TEXT("Ordinary Niagara/Cascade asset is populated; this lifecycle FX is unbatched or hybrid, not a pure MassBattle Batch FX path."));
		}
		if (Fx.SubType == EESubType::None && Fx.SoftNiagaraAsset.IsNull() && Fx.SoftCascadeAsset.IsNull())
		{
			AddIssue(Issues, TEXT("warning"), TEXT("fx_has_no_visual_backend"), *Path,
				TEXT("Enabled FX has neither a Batch SubType nor an ordinary Niagara/Cascade asset."));
		}
		if (bTerminal && Fx.bAttached)
		{
			AddIssue(Issues, TEXT("warning"), TEXT("terminal_fx_is_attached"), *Path,
				TEXT("Hit/removal FX is attached; terminal impact visuals are normally Burst events."));
		}
		if (Fx.Quantity != 1)
		{
			AddIssue(Issues, TEXT("warning"), TEXT("logical_fx_quantity_not_one"), *Path,
				TEXT("Quantity multiplies logical FX events. Keep it at 1 and author particle count inside Niagara unless multiple logical instances are intentional."));
		}
	}

	const FString AssetPackage = Asset && Asset->GetOutermost() ? Asset->GetOutermost()->GetName() : FString();
	for (int32 Index = 0; Index < Content.SpawnProjectile.Num(); ++Index)
	{
		const FProjectileConfig& Child = Content.SpawnProjectile[Index];
		if (!Child.bEnable || Child.ProjectileConfigDataAsset.IsNull())
		{
			continue;
		}
		const FString Path = FString::Printf(TEXT("%s.SpawnProjectile[%d]"), *Prefix, Index);
		const FSoftObjectPath SoftPath = Child.ProjectileConfigDataAsset.ToSoftObjectPath();
		if (SoftPath.GetLongPackageName() == AssetPackage)
		{
			AddIssue(Issues, TEXT("error"), TEXT("direct_projectile_recursion"), *Path,
				TEXT("Projectile lifecycle content directly spawns the same projectile DataAsset."));
		}
		else if (!SoftPath.IsValid() || !SoftPath.TryLoad())
		{
			AddIssue(Issues, TEXT("error"), TEXT("missing_child_projectile"), *Path,
				FString::Printf(TEXT("Child projectile asset is not loadable: %s"), *SoftPath.ToString()));
		}
		else
		{
			AddIssue(Issues, TEXT("warning"), TEXT("submunition_requires_depth_review"), *Path,
				FString::Printf(TEXT("Lifecycle content spawns %d child projectile instance(s); review recursion depth and representative load."), Child.Quantity));
		}
	}
}

static TArray<TSharedPtr<FJsonValue>> BuildReferencers(UMassBattleProjectileConfigDataAsset* Asset)
{
	TArray<TSharedPtr<FJsonValue>> Results;
	if (!Asset || !Asset->GetOutermost())
	{
		return Results;
	}

	IAssetRegistry& Registry = GetAssetRegistry();
	TArray<FName> ReferencerPackages;
	Registry.GetReferencers(Asset->GetOutermost()->GetFName(), ReferencerPackages);
	for (const FName& PackageName : ReferencerPackages)
	{
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPackageName(PackageName, Assets, true);
		for (const FAssetData& Referencer : Assets)
		{
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("path"), Referencer.GetObjectPathString());
			Item->SetStringField(TEXT("class"), Referencer.AssetClassPath.ToString());
			Results.Add(MakeShared<FJsonValueObject>(Item));
		}
	}
	return Results;
}

static void ValidateLinkedUnitDamagePaths(UMassBattleProjectileConfigDataAsset* Asset, TArray<FValidationIssue>& Issues, TArray<TSharedPtr<FJsonValue>>& LinkedUnits)
{
	if (!Asset || !Asset->GetOutermost())
	{
		return;
	}

	IAssetRegistry& Registry = GetAssetRegistry();
	TArray<FName> ReferencerPackages;
	Registry.GetReferencers(Asset->GetOutermost()->GetFName(), ReferencerPackages);
	const FString ProjectilePackage = Asset->GetOutermost()->GetName();
	for (const FName& PackageName : ReferencerPackages)
	{
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPackageName(PackageName, Assets, true);
		for (const FAssetData& Referencer : Assets)
		{
			UMassBattleAgentConfigDataAsset* Unit = Cast<UMassBattleAgentConfigDataAsset>(Referencer.GetAsset());
			if (!Unit)
			{
				continue;
			}

			bool bUsesProjectile = false;
			for (const FProjectileConfig& Entry : Unit->Attack.SpawnProjectile)
			{
				if (Entry.bEnable && !Entry.ProjectileConfigDataAsset.IsNull()
					&& Entry.ProjectileConfigDataAsset.ToSoftObjectPath().GetLongPackageName() == ProjectilePackage)
				{
					bUsesProjectile = true;
					break;
				}
			}
			if (!bUsesProjectile)
			{
				continue;
			}

			LinkedUnits.Add(MakeShared<FJsonValueString>(Unit->GetPathName()));
			if (Unit->Attack.TimeOfHitAction == EAttackMode::ApplyDMG || Unit->Attack.TimeOfHitAction == EAttackMode::SuicideATK)
			{
				AddIssue(Issues, TEXT("warning"), TEXT("agent_and_projectile_damage_paths"), TEXT("Attack.TimeOfHitAction"),
					FString::Printf(TEXT("Unit %s applies Agent hit-time damage and launches this projectile. Disable Agent damage for a normal projectile-owned attack, or document the intentional two-part damage."), *Unit->GetPathName()));
			}
		}
	}
}

static TSharedPtr<FJsonObject> BuildValidation(UMassBattleProjectileConfigDataAsset* Asset, bool bIncludeReferencers)
{
	TArray<FValidationIssue> Issues;
	TArray<TSharedPtr<FJsonValue>> LinkedUnits;
	if (!Asset)
	{
		AddIssue(Issues, TEXT("error"), TEXT("null_projectile"), TEXT(""), TEXT("Projectile asset is null."));
	}
	else
	{
		const FProjectileParams& Params = Asset->ProjectileParams;
		if (Params.Radius < 0.0f)
		{
			AddIssue(Issues, TEXT("error"), TEXT("negative_collision_radius"), TEXT("ProjectileParams.Radius"), TEXT("Collision radius cannot be negative."));
		}
		if (Params.LifeSpan <= 0.0f)
		{
			AddIssue(Issues, TEXT("warning"), TEXT("non_positive_lifespan"), TEXT("ProjectileParams.LifeSpan"), TEXT("Projectile will expire immediately or has no usable flight window."));
		}
		if (Params.Health <= 0 && Params.DamageRepetitionMode != EDamageRepetitionMode::None)
		{
			AddIssue(Issues, TEXT("error"), TEXT("non_positive_projectile_health"), TEXT("ProjectileParams.Health"), TEXT("A damaging projectile must start with positive collision health."));
		}
		if (Params.DmgCoolDown < 0.0f)
		{
			AddIssue(Issues, TEXT("error"), TEXT("negative_damage_cooldown"), TEXT("ProjectileParams.DmgCoolDown"), TEXT("Damage cooldown cannot be negative."));
		}

		switch (Asset->MovementMode)
		{
		case EProjectileMoveMode::Static:
			break;
		case EProjectileMoveMode::Interped:
			if (Asset->ProjectileMove_Interped.Speed <= 0.0f)
			{
				AddIssue(Issues, TEXT("error"), TEXT("invalid_interped_speed"), TEXT("ProjectileMove_Interped.Speed"), TEXT("Interped movement requires Speed > 0."));
			}
			break;
		case EProjectileMoveMode::Ballistic:
			if (Asset->ProjectileMove_Ballistic.MaxSpeed <= 0.0f)
			{
				AddIssue(Issues, TEXT("error"), TEXT("invalid_ballistic_max_speed"), TEXT("ProjectileMove_Ballistic.MaxSpeed"), TEXT("Ballistic movement requires MaxSpeed > 0."));
			}
			if (Asset->ProjectileMove_Ballistic.Iterations <= 0)
			{
				AddIssue(Issues, TEXT("error"), TEXT("invalid_ballistic_iterations"), TEXT("ProjectileMove_Ballistic.Iterations"), TEXT("Ballistic solver requires at least one iteration."));
			}
			if (Asset->ProjectileMove_Ballistic.SolveMode == EProjectileSolveMode::FromSpeed && Asset->ProjectileMove_Ballistic.Speed <= 0.0f)
			{
				AddIssue(Issues, TEXT("error"), TEXT("invalid_ballistic_launch_speed"), TEXT("ProjectileMove_Ballistic.Speed"), TEXT("FromSpeed ballistic solving requires Speed > 0."));
			}
			break;
		case EProjectileMoveMode::Tracking:
			if (Asset->ProjectileMove_Tracking.Speed <= 0.0f || Asset->ProjectileMove_Tracking.MaxSpeed <= 0.0f)
			{
				AddIssue(Issues, TEXT("error"), TEXT("invalid_tracking_speed"), TEXT("ProjectileMove_Tracking"), TEXT("Tracking movement requires positive Speed and MaxSpeed."));
			}
			if (Asset->ProjectileMove_Tracking.MaxSpeed < Asset->ProjectileMove_Tracking.Speed)
			{
				AddIssue(Issues, TEXT("warning"), TEXT("tracking_max_below_initial"), TEXT("ProjectileMove_Tracking.MaxSpeed"), TEXT("MaxSpeed is below initial Speed; runtime clamping/deceleration should be reviewed."));
			}
			if (Asset->ProjectileMove_Tracking.LateralAcceleration < 0.0f)
			{
				AddIssue(Issues, TEXT("error"), TEXT("negative_lateral_acceleration"), TEXT("ProjectileMove_Tracking.LateralAcceleration"), TEXT("LateralAcceleration cannot be negative."));
			}
			break;
		}

		if (Params.DamageRepetitionMode != EDamageRepetitionMode::None && !AnyTrigger(Params.ApplyDmgConditions))
		{
			AddIssue(Issues, TEXT("error"), TEXT("damage_has_no_apply_condition"), TEXT("ProjectileParams.ApplyDmgConditions"), TEXT("Damaging projectile has no condition that can apply damage."));
		}
		if (Params.DamageRepetitionMode == EDamageRepetitionMode::None && AnyTrigger(Params.ApplyDmgConditions))
		{
			AddIssue(Issues, TEXT("warning"), TEXT("apply_conditions_ignored"), TEXT("ProjectileParams.DamageRepetitionMode"), TEXT("DamageRepetitionMode=None disables projectile damage and the current OnHit damage-processing path."));
		}
		if (!AnyTrigger(Params.RemovalConditions))
		{
			AddIssue(Issues, TEXT("warning"), TEXT("projectile_has_no_removal_condition"), TEXT("ProjectileParams.RemovalConditions"), TEXT("Projectile has no configured removal condition and may remain active indefinitely."));
		}
		if (Params.ApplyDmgConditions.bOnHitEntity && !Params.RemovalConditions.bOnHitEntity)
		{
			if (Params.Health == 1 && Params.RemovalConditions.bOnNoHealth)
			{
				AddIssue(Issues, TEXT("warning"), TEXT("entity_hit_removes_indirectly"), TEXT("ProjectileParams.RemovalConditions.bOnHitEntity"), TEXT("Entity hit removes indirectly through Health=1 and bOnNoHealth. Set bOnHitEntity explicitly when impact itself is the intended terminal rule."));
			}
			else
			{
				AddIssue(Issues, TEXT("warning"), TEXT("entity_hit_does_not_remove"), TEXT("ProjectileParams.RemovalConditions.bOnHitEntity"), TEXT("Entity-hit damage does not explicitly remove the projectile; review penetration/multiple-hit intent."));
			}
		}
		if (Params.ApplyDmgConditions.bOnHitEntity && Params.Health == 1
			&& Params.DamageRepetitionMode != EDamageRepetitionMode::OnceForever
			&& Params.DamageRepetitionMode != EDamageRepetitionMode::None)
		{
			AddIssue(Issues, TEXT("warning"), TEXT("one_hit_repetition_mode"), TEXT("ProjectileParams.DamageRepetitionMode"), TEXT("A conventional one-hit projectile normally uses OnceForever."));
		}

		if (Asset->DamageMode == EProjectileDamageMode::Radial && Asset->Damage_Radial.DmgRadius <= 0.0f)
		{
			AddIssue(Issues, TEXT("warning"), TEXT("radial_damage_has_zero_radius"), TEXT("Damage_Radial.DmgRadius"), TEXT("Radial mode has zero radius and behaves as a single-target-sized result."));
		}
		if (Asset->DamageMode == EProjectileDamageMode::Beam)
		{
			if (Asset->Damage_Beam.BeamLength <= 0.0f)
			{
				AddIssue(Issues, TEXT("error"), TEXT("invalid_beam_length"), TEXT("Damage_Beam.BeamLength"), TEXT("Beam damage requires BeamLength > 0."));
			}
			if (Asset->Damage_Beam.DmgRadius < 0.0f)
			{
				AddIssue(Issues, TEXT("error"), TEXT("negative_beam_radius"), TEXT("Damage_Beam.DmgRadius"), TEXT("Beam radius cannot be negative."));
			}
		}

		ValidateLifecycleContent(Asset, Asset->ProjectileSpawn.OnBirth, TEXT("ProjectileSpawn.OnBirth"), false, Issues);
		ValidateLifecycleContent(Asset, Asset->ProjectileSpawn.OnHit, TEXT("ProjectileSpawn.OnHit"), true, Issues);
		ValidateLifecycleContent(Asset, Asset->ProjectileSpawn.OnRemoval, TEXT("ProjectileSpawn.OnRemoval"), true, Issues);

		if (Params.DamageRepetitionMode == EDamageRepetitionMode::None && Asset->ProjectileSpawn.OnHit.bEnable && HasEnabledFx(Asset->ProjectileSpawn.OnHit))
		{
			AddIssue(Issues, TEXT("warning"), TEXT("on_hit_will_not_run_without_damage"), TEXT("ProjectileSpawn.OnHit"), TEXT("Current OnHit content runs in the damage-processing branch; DamageRepetitionMode=None prevents that path."));
		}

		const bool bHitFx = Asset->ProjectileSpawn.OnHit.bEnable && HasEnabledFx(Asset->ProjectileSpawn.OnHit);
		const bool bRemovalFx = Asset->ProjectileSpawn.OnRemoval.bEnable && HasEnabledFx(Asset->ProjectileSpawn.OnRemoval);
		if (bHitFx && bRemovalFx && EnabledSpawnFxEquivalent(Asset->ProjectileSpawn.OnHit, Asset->ProjectileSpawn.OnRemoval))
		{
			AddIssue(Issues, TEXT("error"), TEXT("duplicate_on_hit_on_removal_fx"), TEXT("ProjectileSpawn"),
				TEXT("OnHit and OnRemoval contain the same enabled FX array. A damage hit can remove the projectile in the same simulation tick and spawn the full effect twice."));
		}

		if (bIncludeReferencers)
		{
			ValidateLinkedUnitDamagePaths(Asset, Issues, LinkedUnits);
		}
	}

	int32 ErrorCount = 0;
	int32 WarningCount = 0;
	TArray<TSharedPtr<FJsonValue>> IssueJson;
	for (const FValidationIssue& Issue : Issues)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("severity"), Issue.Severity);
		Item->SetStringField(TEXT("code"), Issue.Code);
		Item->SetStringField(TEXT("path"), Issue.Path);
		Item->SetStringField(TEXT("message"), Issue.Message);
		IssueJson.Add(MakeShared<FJsonValueObject>(Item));
		if (Issue.Severity == TEXT("error"))
		{
			++ErrorCount;
		}
		else if (Issue.Severity == TEXT("warning"))
		{
			++WarningCount;
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("valid"), ErrorCount == 0);
	Result->SetNumberField(TEXT("error_count"), ErrorCount);
	Result->SetNumberField(TEXT("warning_count"), WarningCount);
	Result->SetArrayField(TEXT("issues"), IssueJson);
	Result->SetArrayField(TEXT("linked_units"), LinkedUnits);
	if (Asset)
	{
		Result->SetStringField(TEXT("asset_path"), Asset->GetPathName());
		Result->SetStringField(TEXT("movement_mode"), MoveModeName(Asset->MovementMode));
		Result->SetStringField(TEXT("damage_mode"), DamageModeName(Asset->DamageMode));
	}
	return Result;
}

static bool ValidationHasErrors(const TSharedPtr<FJsonObject>& Validation)
{
	int32 ErrorCount = 0;
	return Validation.IsValid() && Validation->TryGetNumberField(TEXT("error_count"), ErrorCount) && ErrorCount > 0;
}

static UMassBattleProjectileConfigDataAsset* MakePreview(UMassBattleProjectileConfigDataAsset* Source)
{
	if (!Source)
	{
		return nullptr;
	}
	const FName Name = MakeUniqueObjectName(GetTransientPackage(), Source->GetClass(), TEXT("MCP_ProjectilePreview"));
	return DuplicateObject<UMassBattleProjectileConfigDataAsset>(Source, GetTransientPackage(), Name);
}

static TSharedPtr<FJsonObject> BuildWriteFailure(
	const FString& Error,
	const TArray<FString>& MergeErrors,
	const TSharedPtr<FJsonObject>& Validation)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("success"), false);
	Root->SetStringField(TEXT("error"), Error);
	Root->SetArrayField(TEXT("merge_errors"), StringsToJson(MergeErrors));
	if (Validation.IsValid())
	{
		Root->SetObjectField(TEXT("validation"), Validation);
	}
	return Root;
}

static TArray<TSharedPtr<FJsonValue>> RootNamesToJson(const TSharedPtr<FJsonObject>& Options)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FName& Root : ReadRoots(Options))
	{
		Values.Add(MakeShared<FJsonValueString>(Root.ToString()));
	}
	return Values;
}
} // namespace MassBattleProjectileMCP

FString UMassBattleProjectileMCPApi::MCP_ProjectileGetApiStatus()
{
	using namespace MassBattleProjectileMCP;
	TSharedPtr<FJsonObject> Root = MakeSuccess();
	Root->SetStringField(TEXT("api_name"), TEXT("MassBattleProjectileMCPApi"));
	Root->SetStringField(TEXT("version"), TEXT("1.0.0"));
	Root->SetStringField(TEXT("asset_class"), UMassBattleProjectileConfigDataAsset::StaticClass()->GetPathName());
	Root->SetStringField(TEXT("write_contract"), TEXT("source-aligned union write; omitted fields stay unchanged; array append/replace must be explicit"));

	auto Tool = [](const TCHAR* Name, const TCHAR* Category, const TCHAR* Description, const TCHAR* Parameters)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Name);
		Item->SetStringField(TEXT("category"), Category);
		Item->SetStringField(TEXT("description"), Description);
		Item->SetStringField(TEXT("parameters"), Parameters);
		return MakeShared<FJsonValueObject>(Item);
	};

	TArray<TSharedPtr<FJsonValue>> Tools;
	Tools.Add(Tool(TEXT("MCP_ProjectileList"), TEXT("projectile.query"), TEXT("List projectile DataAssets."), TEXT("OptionsJson")));
	Tools.Add(Tool(TEXT("MCP_ProjectileQuery"), TEXT("projectile.query"), TEXT("Query projectile DataAssets by path/name text."), TEXT("QueryJson")));
	Tools.Add(Tool(TEXT("MCP_ProjectileGet"), TEXT("projectile.read"), TEXT("Read simple active-only or full source-aligned projectile data."), TEXT("ProjectilePath, OptionsJson")));
	Tools.Add(Tool(TEXT("MCP_ProjectileGetSchema"), TEXT("projectile.schema"), TEXT("Read editable fields, enums, tooltips, and conditional roles."), TEXT("OptionsJson")));
	Tools.Add(Tool(TEXT("MCP_ProjectileCreate"), TEXT("projectile.create"), TEXT("Create a projectile DataAsset from a template or class default and optional source-aligned data."), TEXT("CreateSpecJson, bSaveAssets")));
	Tools.Add(Tool(TEXT("MCP_ProjectileWrite"), TEXT("projectile.write"), TEXT("Union-write partial projectile JSON after transient preflight and validation."), TEXT("ProjectilePath, PatchJson, bSaveAssets")));
	Tools.Add(Tool(TEXT("MCP_ProjectileValidate"), TEXT("projectile.validate"), TEXT("Validate movement, damage, triggers, linked unit damage ownership, lifecycle FX, and double explosions."), TEXT("ProjectilePath, OptionsJson")));
	Tools.Add(Tool(TEXT("MCP_ProjectileDelete"), TEXT("projectile.delete"), TEXT("Plan or execute soft/hard deletion; dry_run=true by default."), TEXT("ProjectilePath, OptionsJson")));
	Root->SetArrayField(TEXT("tools"), Tools);
	return ToJsonString(Root);
}

FString UMassBattleProjectileMCPApi::MCP_ProjectileList(const FString& OptionsJson)
{
	using namespace MassBattleProjectileMCP;
	TSharedPtr<FJsonObject> Options = ParseObject(OptionsJson);
	if (!Options.IsValid())
	{
		return MakeErrorJson(TEXT("OptionsJson must be a JSON object"));
	}

	FString Query;
	Options->TryGetStringField(TEXT("query"), Query);
	Query = Query.ToLower();
	int32 Limit = DefaultListLimit;
	Options->TryGetNumberField(TEXT("limit"), Limit);
	Limit = FMath::Clamp(Limit, 1, MaxListLimit);
	bool bLoad = true;
	Options->TryGetBoolField(TEXT("load_fields"), bLoad);

	const TArray<FAssetData> Scanned = ScanProjectileAssets(Options);
	TArray<TSharedPtr<FJsonValue>> Assets;
	for (const FAssetData& AssetData : Scanned)
	{
		const FString Path = AssetData.GetObjectPathString();
		if (!Query.IsEmpty() && !Path.ToLower().Contains(Query) && !AssetData.AssetName.ToString().ToLower().Contains(Query))
		{
			continue;
		}
		Assets.Add(MakeShared<FJsonValueObject>(AssetSummary(AssetData, bLoad)));
		if (Assets.Num() >= Limit)
		{
			break;
		}
	}

	TSharedPtr<FJsonObject> Root = MakeSuccess();
	Root->SetNumberField(TEXT("count"), Assets.Num());
	Root->SetNumberField(TEXT("total_scanned"), Scanned.Num());
	Root->SetArrayField(TEXT("roots"), RootNamesToJson(Options));
	Root->SetArrayField(TEXT("projectiles"), Assets);
	return ToJsonString(Root);
}

FString UMassBattleProjectileMCPApi::MCP_ProjectileQuery(const FString& QueryJson)
{
	using namespace MassBattleProjectileMCP;
	return MCP_ProjectileList(QueryJson);
}

FString UMassBattleProjectileMCPApi::MCP_ProjectileGet(const FString& ProjectilePath, const FString& OptionsJson)
{
	using namespace MassBattleProjectileMCP;
	TSharedPtr<FJsonObject> Options = ParseObject(OptionsJson);
	if (!Options.IsValid())
	{
		return MakeErrorJson(TEXT("OptionsJson must be a JSON object"));
	}

	FString Error;
	UMassBattleProjectileConfigDataAsset* Asset = LoadProjectile(ProjectilePath, Error);
	if (!Asset)
	{
		return MakeErrorJson(Error);
	}

	FString View = TEXT("simple");
	Options->TryGetStringField(TEXT("view"), View);
	Options->TryGetStringField(TEXT("detail"), View);
	bool bIncludeInactive = View.Equals(TEXT("full"), ESearchCase::IgnoreCase);
	Options->TryGetBoolField(TEXT("include_inactive"), bIncludeInactive);
	bool bIncludeValidation = true;
	Options->TryGetBoolField(TEXT("include_validation"), bIncludeValidation);
	bool bIncludeReferencers = false;
	Options->TryGetBoolField(TEXT("include_referencers"), bIncludeReferencers);

	TSharedPtr<FJsonObject> Root = MakeSuccess();
	Root->SetStringField(TEXT("asset_name"), Asset->GetName());
	Root->SetStringField(TEXT("object_path"), Asset->GetPathName());
	Root->SetStringField(TEXT("class"), Asset->GetClass()->GetPathName());
	Root->SetStringField(TEXT("view"), bIncludeInactive ? TEXT("full") : TEXT("simple"));
	Root->SetStringField(TEXT("movement_mode"), MoveModeName(Asset->MovementMode));
	Root->SetStringField(TEXT("damage_mode"), DamageModeName(Asset->DamageMode));
	Root->SetObjectField(TEXT("data"), ProjectileDataToJson(Asset, bIncludeInactive));
	if (bIncludeValidation)
	{
		Root->SetObjectField(TEXT("validation"), BuildValidation(Asset, bIncludeReferencers));
	}
	if (bIncludeReferencers)
	{
		Root->SetArrayField(TEXT("referencers"), BuildReferencers(Asset));
	}
	return ToJsonString(Root);
}

FString UMassBattleProjectileMCPApi::MCP_ProjectileGetSchema(const FString& OptionsJson)
{
	using namespace MassBattleProjectileMCP;
	TSharedPtr<FJsonObject> Options = ParseObject(OptionsJson);
	if (!Options.IsValid())
	{
		return MakeErrorJson(TEXT("OptionsJson must be a JSON object"));
	}
	int32 Depth = DefaultSchemaDepth;
	Options->TryGetNumberField(TEXT("depth"), Depth);
	Depth = FMath::Clamp(Depth, 0, 8);

	TArray<TSharedPtr<FJsonValue>> Fields;
	AddSchemaFields(Fields, UMassBattleProjectileConfigDataAsset::StaticClass(), FString(), Depth);

	TSharedPtr<FJsonObject> Conditional = MakeShared<FJsonObject>();
	Conditional->SetStringField(TEXT("Static"), TEXT("ProjectileMove_Static"));
	Conditional->SetStringField(TEXT("Interped"), TEXT("ProjectileMove_Interped"));
	Conditional->SetStringField(TEXT("Ballistic"), TEXT("ProjectileMove_Ballistic"));
	Conditional->SetStringField(TEXT("Tracking"), TEXT("ProjectileMove_Tracking"));
	TSharedPtr<FJsonObject> DamageConditional = MakeShared<FJsonObject>();
	DamageConditional->SetStringField(TEXT("Point"), TEXT("Damage_Point + Debuff_Point"));
	DamageConditional->SetStringField(TEXT("Radial"), TEXT("Damage_Radial + Debuff_Radial"));
	DamageConditional->SetStringField(TEXT("Beam"), TEXT("Damage_Beam + Debuff_Beam"));

	TSharedPtr<FJsonObject> Root = MakeSuccess();
	Root->SetStringField(TEXT("class"), UMassBattleProjectileConfigDataAsset::StaticClass()->GetPathName());
	Root->SetNumberField(TEXT("field_count"), Fields.Num());
	Root->SetArrayField(TEXT("fields"), Fields);
	Root->SetObjectField(TEXT("movement_mode_fields"), Conditional);
	Root->SetObjectField(TEXT("damage_mode_fields"), DamageConditional);
	Root->SetStringField(TEXT("array_write_rule"), TEXT("Default is merge-by-index with append rejected. Use options.append_arrays=true to append or options.array_mode=replace to replace explicitly."));
	Root->SetStringField(TEXT("damage_ownership_rule"), TEXT("Normal projectile-owned attacks use Agent Attack.TimeOfHitAction=None; the projectile DataAsset owns damage/debuff."));
	Root->SetStringField(TEXT("lifecycle_rule"), TEXT("Do not put the same full explosion in OnHit and OnRemoval; both can execute in one simulation tick."));
	return ToJsonString(Root);
}

FString UMassBattleProjectileMCPApi::MCP_ProjectileWrite(const FString& ProjectilePath, const FString& PatchJson, bool bSaveAssets)
{
	using namespace MassBattleProjectileMCP;
	TSharedPtr<FJsonObject> Patch = ParseObject(PatchJson);
	if (!Patch.IsValid())
	{
		return MakeErrorJson(TEXT("PatchJson must be a JSON object"));
	}

	FString Error;
	UMassBattleProjectileConfigDataAsset* Asset = LoadProjectile(ProjectilePath, Error);
	if (!Asset)
	{
		return MakeErrorJson(Error);
	}

	FMergeOptions Options;
	ReadMergeOptions(Patch, Options);
	TSharedPtr<FJsonObject> Data = ExtractPatchData(Patch, false);
	if (!Data.IsValid() || Data->Values.IsEmpty())
	{
		return MakeErrorJson(TEXT("PatchJson has no projectile data fields"));
	}

	UMassBattleProjectileConfigDataAsset* Preview = MakePreview(Asset);
	if (!Preview)
	{
		return MakeErrorJson(TEXT("Failed to create transient projectile write preview"));
	}
	TArray<FString> PreviewChanges;
	TArray<FString> MergeErrors;
	if (!ApplySourceAlignedPatch(Preview, Data, Options, PreviewChanges, MergeErrors) || !MergeErrors.IsEmpty())
	{
		return ToJsonString(BuildWriteFailure(TEXT("Projectile patch preflight failed; target was not mutated"), MergeErrors, nullptr));
	}

	TSharedPtr<FJsonObject> PreviewValidation = Options.bValidate ? BuildValidation(Preview, false) : nullptr;
	if (Options.bValidate && Options.bBlockOnValidationError && ValidationHasErrors(PreviewValidation))
	{
		return ToJsonString(BuildWriteFailure(TEXT("Projectile patch failed validation; target was not mutated"), MergeErrors, PreviewValidation));
	}

	Asset->Modify();
	TArray<FString> AppliedChanges;
	TArray<FString> ApplyErrors;
	if (!ApplySourceAlignedPatch(Asset, Data, Options, AppliedChanges, ApplyErrors) || !ApplyErrors.IsEmpty())
	{
		return ToJsonString(BuildWriteFailure(TEXT("Unexpected failure applying a preflighted projectile patch"), ApplyErrors, nullptr));
	}
	Asset->MarkPackageDirty();
	bool bSaved = false;
	if (bSaveAssets)
	{
		if (!SaveAsset(Asset, Error))
		{
			return MakeErrorJson(Error);
		}
		bSaved = true;
	}

	TSharedPtr<FJsonObject> Root = MakeSuccess();
	Root->SetStringField(TEXT("asset_path"), Asset->GetPathName());
	Root->SetBoolField(TEXT("saved"), bSaved);
	Root->SetArrayField(TEXT("changed_paths"), StringsToJson(AppliedChanges));
	Root->SetObjectField(TEXT("data"), ProjectileDataToJson(Asset, false));
	Root->SetObjectField(TEXT("validation"), BuildValidation(Asset, true));
	return ToJsonString(Root);
}

FString UMassBattleProjectileMCPApi::MCP_ProjectileCreate(const FString& CreateSpecJson, bool bSaveAssets)
{
	using namespace MassBattleProjectileMCP;
	TSharedPtr<FJsonObject> Spec = ParseObject(CreateSpecJson);
	if (!Spec.IsValid())
	{
		return MakeErrorJson(TEXT("CreateSpecJson must be a JSON object"));
	}

	FString AssetName;
	FString PackagePath;
	FString TemplatePath;
	Spec->TryGetStringField(TEXT("asset_name"), AssetName);
	if (AssetName.IsEmpty()) Spec->TryGetStringField(TEXT("name"), AssetName);
	Spec->TryGetStringField(TEXT("package_path"), PackagePath);
	if (PackagePath.IsEmpty()) Spec->TryGetStringField(TEXT("target_package_path"), PackagePath);
	Spec->TryGetStringField(TEXT("template_path"), TemplatePath);
	if (TemplatePath.IsEmpty()) Spec->TryGetStringField(TEXT("template"), TemplatePath);
	AssetName.TrimStartAndEndInline();
	PackagePath.TrimStartAndEndInline();
	if (AssetName.IsEmpty() || PackagePath.IsEmpty() || !PackagePath.StartsWith(TEXT("/")))
	{
		return MakeErrorJson(TEXT("asset_name and an Unreal package_path beginning with '/' are required"));
	}

	FString Error;
	UMassBattleProjectileConfigDataAsset* Source = nullptr;
	if (!TemplatePath.IsEmpty())
	{
		Source = LoadProjectile(TemplatePath, Error);
		if (!Source)
		{
			return MakeErrorJson(Error);
		}
	}
	else
	{
		Source = GetMutableDefault<UMassBattleProjectileConfigDataAsset>();
	}

	FMergeOptions Options;
	ReadMergeOptions(Spec, Options);
	TSharedPtr<FJsonObject> Data = ExtractPatchData(Spec, true);
	UMassBattleProjectileConfigDataAsset* Preview = MakePreview(Source);
	if (!Preview)
	{
		return MakeErrorJson(TEXT("Failed to create transient projectile create preview"));
	}
	TArray<FString> PreviewChanges;
	TArray<FString> MergeErrors;
	if (Data.IsValid() && !Data->Values.IsEmpty()
		&& (!ApplySourceAlignedPatch(Preview, Data, Options, PreviewChanges, MergeErrors) || !MergeErrors.IsEmpty()))
	{
		return ToJsonString(BuildWriteFailure(TEXT("Projectile create preflight failed; no target asset was created"), MergeErrors, nullptr));
	}
	TSharedPtr<FJsonObject> PreviewValidation = Options.bValidate ? BuildValidation(Preview, false) : nullptr;
	if (Options.bValidate && Options.bBlockOnValidationError && ValidationHasErrors(PreviewValidation))
	{
		return ToJsonString(BuildWriteFailure(TEXT("Projectile create preflight failed validation; no target asset was created"), MergeErrors, PreviewValidation));
	}

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UMassBattleProjectileConfigDataAsset* NewAsset = nullptr;
	if (!TemplatePath.IsEmpty())
	{
		NewAsset = Cast<UMassBattleProjectileConfigDataAsset>(AssetTools.DuplicateAsset(AssetName, PackagePath, Source));
	}
	else
	{
		UDataAssetFactory* Factory = NewObject<UDataAssetFactory>();
		Factory->DataAssetClass = UMassBattleProjectileConfigDataAsset::StaticClass();
		NewAsset = Cast<UMassBattleProjectileConfigDataAsset>(AssetTools.CreateAsset(AssetName, PackagePath, UMassBattleProjectileConfigDataAsset::StaticClass(), Factory));
	}
	if (!NewAsset)
	{
		return MakeErrorJson(TEXT("AssetTools failed to create the projectile DataAsset (the target may already exist)"));
	}

	TArray<FString> AppliedChanges;
	TArray<FString> ApplyErrors;
	if (Data.IsValid() && !Data->Values.IsEmpty()
		&& (!ApplySourceAlignedPatch(NewAsset, Data, Options, AppliedChanges, ApplyErrors) || !ApplyErrors.IsEmpty()))
	{
		return ToJsonString(BuildWriteFailure(TEXT("Unexpected failure applying preflighted create data"), ApplyErrors, nullptr));
	}
	NewAsset->MarkPackageDirty();
	bool bSaved = false;
	if (bSaveAssets)
	{
		if (!SaveAsset(NewAsset, Error))
		{
			return MakeErrorJson(Error);
		}
		bSaved = true;
	}

	TSharedPtr<FJsonObject> Root = MakeSuccess();
	Root->SetStringField(TEXT("asset_path"), NewAsset->GetPathName());
	Root->SetStringField(TEXT("template_path"), TemplatePath);
	Root->SetBoolField(TEXT("saved"), bSaved);
	Root->SetArrayField(TEXT("changed_paths"), StringsToJson(AppliedChanges));
	Root->SetObjectField(TEXT("data"), ProjectileDataToJson(NewAsset, false));
	Root->SetObjectField(TEXT("validation"), BuildValidation(NewAsset, true));
	return ToJsonString(Root);
}

FString UMassBattleProjectileMCPApi::MCP_ProjectileValidate(const FString& ProjectilePath, const FString& OptionsJson)
{
	using namespace MassBattleProjectileMCP;
	TSharedPtr<FJsonObject> Options = ParseObject(OptionsJson);
	if (!Options.IsValid())
	{
		return MakeErrorJson(TEXT("OptionsJson must be a JSON object"));
	}
	FString Error;
	UMassBattleProjectileConfigDataAsset* Asset = LoadProjectile(ProjectilePath, Error);
	if (!Asset)
	{
		return MakeErrorJson(Error);
	}
	bool bIncludeReferencers = true;
	Options->TryGetBoolField(TEXT("include_referencers"), bIncludeReferencers);
	TSharedPtr<FJsonObject> Root = MakeSuccess();
	Root->SetObjectField(TEXT("validation"), BuildValidation(Asset, bIncludeReferencers));
	if (bIncludeReferencers)
	{
		Root->SetArrayField(TEXT("referencers"), BuildReferencers(Asset));
	}
	return ToJsonString(Root);
}

FString UMassBattleProjectileMCPApi::MCP_ProjectileDelete(const FString& ProjectilePath, const FString& OptionsJson)
{
	using namespace MassBattleProjectileMCP;
	TSharedPtr<FJsonObject> Options = ParseObject(OptionsJson);
	if (!Options.IsValid())
	{
		return MakeErrorJson(TEXT("OptionsJson must be a JSON object"));
	}
	FString Error;
	UMassBattleProjectileConfigDataAsset* Asset = LoadProjectile(ProjectilePath, Error);
	if (!Asset)
	{
		return MakeErrorJson(Error);
	}

	bool bDryRun = true;
	bool bAllowReferenced = false;
	bool bAllowUnsafeMove = false;
	bool bAllowHardDelete = false;
	Options->TryGetBoolField(TEXT("dry_run"), bDryRun);
	Options->TryGetBoolField(TEXT("allow_referenced"), bAllowReferenced);
	Options->TryGetBoolField(TEXT("allow_unsafe_asset_move"), bAllowUnsafeMove);
	Options->TryGetBoolField(TEXT("allow_hard_delete"), bAllowHardDelete);
	FString Mode = TEXT("soft");
	Options->TryGetStringField(TEXT("mode"), Mode);
	Mode = Mode.ToLower();
	if (Mode == TEXT("permanent")) Mode = TEXT("hard");
	if (Mode != TEXT("hard")) Mode = TEXT("soft");

	TArray<TSharedPtr<FJsonValue>> Referencers = BuildReferencers(Asset);
	const bool bBlockedByReferences = !Referencers.IsEmpty() && !bAllowReferenced;
	TSharedPtr<FJsonObject> Root = MakeSuccess();
	Root->SetBoolField(TEXT("dry_run"), bDryRun);
	Root->SetStringField(TEXT("mode"), Mode);
	Root->SetStringField(TEXT("source_path"), Asset->GetPathName());
	Root->SetArrayField(TEXT("referencers"), Referencers);
	Root->SetBoolField(TEXT("applicable"), !bBlockedByReferences);
	if (bBlockedByReferences)
	{
		Root->SetStringField(TEXT("blocked_reason"), TEXT("Projectile has referencers; replace references or pass allow_referenced=true after review."));
	}

	if (Mode == TEXT("soft"))
	{
		FString TrashRoot = TEXT("/Game/_Trash/MassBattle/Projectiles");
		Options->TryGetStringField(TEXT("trash_root"), TrashRoot);
		const FString TrashPackagePath = TrashRoot / FDateTime::UtcNow().ToString(TEXT("%Y-%m-%d"));
		const FString TrashPath = TrashPackagePath / Asset->GetName() + TEXT(".") + Asset->GetName();
		Root->SetStringField(TEXT("trash_path"), TrashPath);
		if (bDryRun || bBlockedByReferences)
		{
			Root->SetBoolField(TEXT("moved"), false);
			return ToJsonString(Root);
		}
		if (!bAllowUnsafeMove)
		{
			Root->SetBoolField(TEXT("success"), false);
			Root->SetBoolField(TEXT("applicable"), false);
			Root->SetStringField(TEXT("blocked_reason"), TEXT("Pass allow_unsafe_asset_move=true only after reviewing active editor state and referencers."));
			return ToJsonString(Root);
		}

		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		TArray<FAssetRenameData> RenameData;
		RenameData.Emplace(Asset, TrashPackagePath, Asset->GetName());
		const bool bMoved = AssetTools.RenameAssets(RenameData);
		Root->SetBoolField(TEXT("moved"), bMoved);
		Root->SetBoolField(TEXT("success"), bMoved);
		if (!bMoved) Root->SetStringField(TEXT("error"), TEXT("AssetTools.RenameAssets failed"));
		return ToJsonString(Root);
	}

	if (bDryRun || bBlockedByReferences)
	{
		Root->SetBoolField(TEXT("deleted"), false);
		return ToJsonString(Root);
	}
	if (!bAllowHardDelete)
	{
		Root->SetBoolField(TEXT("success"), false);
		Root->SetBoolField(TEXT("applicable"), false);
		Root->SetStringField(TEXT("blocked_reason"), TEXT("Hard delete requires allow_hard_delete=true."));
		return ToJsonString(Root);
	}

	TArray<FAssetData> AssetsToDelete = { FAssetData(Asset) };
	const int32 DeletedCount = ObjectTools::DeleteAssets(AssetsToDelete, false);
	Root->SetNumberField(TEXT("deleted_count"), DeletedCount);
	Root->SetBoolField(TEXT("deleted"), DeletedCount > 0);
	Root->SetBoolField(TEXT("success"), DeletedCount > 0);
	if (DeletedCount <= 0) Root->SetStringField(TEXT("error"), TEXT("ObjectTools failed to delete the projectile asset"));
	return ToJsonString(Root);
}
