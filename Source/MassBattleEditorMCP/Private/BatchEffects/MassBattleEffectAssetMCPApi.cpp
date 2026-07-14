// Copyright (c) 2025-2026 Winyunq. All rights reserved.
#include "BatchEffects/MassBattleEffectAssetMCPApi.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "HAL/FileManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "NiagaraDataChannelPublic.h"
#include "NiagaraSystem.h"
#include "ObjectTools.h"
#include "Particles/ParticleEmitter.h"
#include "Particles/ParticleLODLevel.h"
#include "Particles/ParticleModule.h"
#include "Particles/ParticleModuleRequired.h"
#include "Particles/ParticleSystem.h"
#include "Particles/Spawn/ParticleModuleSpawn.h"
#include "Particles/TypeData/ParticleModuleTypeDataBase.h"
#include "Renderers/MassBattleFxRenderer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Sound/SoundBase.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY(LogMassBattleEffectAssetMCPApi);

namespace MassBattleEffectAssetMCP
{
static TSet<FString> UnsavedMCPDuplicatePaths;

static FString ToJsonString(const TSharedPtr<FJsonObject>& Obj)
{
	FString Output;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
	return Output;
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

static FString NormalizeClassPath(FString Path)
{
	Path = NormalizeObjectPath(Path);
	if (Path.StartsWith(TEXT("/")) && !Path.EndsWith(TEXT("_C")))
	{
		Path += TEXT("_C");
	}
	return Path;
}

static UObject* LoadAnyObject(const FString& AssetPath, FString& OutError)
{
	const FString ObjectPath = NormalizeObjectPath(AssetPath);
	if (ObjectPath.IsEmpty())
	{
		OutError = TEXT("AssetPath is empty");
		return nullptr;
	}

	UObject* Object = FSoftObjectPath(ObjectPath).TryLoad();
	if (!Object)
	{
		Object = LoadObject<UObject>(nullptr, *ObjectPath);
	}
	if (!Object)
	{
		OutError = FString::Printf(TEXT("Failed to load asset: %s"), *ObjectPath);
	}
	return Object;
}

static FString GetSavedExportDir()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MassBattleEditorMCP"), TEXT("EffectText"));
}

static bool SaveLoadedAsset(UObject* Asset, FString& OutError)
{
	if (!Asset)
	{
		OutError = TEXT("Asset is null");
		return false;
	}

	UPackage* Package = Asset->GetOutermost();
	if (!Package)
	{
		OutError = TEXT("Asset has no package");
		return false;
	}

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

static TSharedPtr<FJsonObject> AssetDataToJson(const FAssetData& AssetData)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
	Obj->SetStringField(TEXT("class"), AssetData.AssetClassPath.ToString());
	Obj->SetStringField(TEXT("package"), AssetData.PackageName.ToString());
	Obj->SetStringField(TEXT("path"), AssetData.GetObjectPathString());
	return Obj;
}

static FString PropertyToString(FProperty* Property, const void* Container)
{
	if (!Property || !Container)
	{
		return FString();
	}

	FString Value;
	Property->ExportText_InContainer(0, Value, Container, Container, nullptr, PPF_None);
	constexpr int32 MaxLen = 768;
	if (Value.Len() > MaxLen)
	{
		Value = Value.Left(MaxLen) + TEXT("...");
	}
	return Value;
}

static TSharedPtr<FJsonObject> ReflectObject(UObject* Object, int32 MaxProperties)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	if (!Object)
	{
		return Obj;
	}

	Obj->SetStringField(TEXT("name"), Object->GetName());
	Obj->SetStringField(TEXT("class"), Object->GetClass()->GetPathName());

	TArray<TSharedPtr<FJsonValue>> Properties;
	int32 Count = 0;
	for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
	{
		FProperty* Property = *It;
		if (!Property || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_EditorOnly))
		{
			continue;
		}
		if (MaxProperties > 0 && Count >= MaxProperties)
		{
			break;
		}

		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("name"), Property->GetName());
		Prop->SetStringField(TEXT("type"), Property->GetClass()->GetName());

		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			FScriptArrayHelper Helper(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(Object));
			Prop->SetNumberField(TEXT("num"), Helper.Num());
			Prop->SetStringField(TEXT("value"), PropertyToString(Property, Object));
		}
		else if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			UObject* ValueObject = ObjectProperty->GetObjectPropertyValue_InContainer(Object);
			Prop->SetStringField(TEXT("value"), ValueObject ? ValueObject->GetPathName() : TEXT(""));
		}
		else
		{
			Prop->SetStringField(TEXT("value"), PropertyToString(Property, Object));
		}

		Properties.Add(MakeShared<FJsonValueObject>(Prop));
		++Count;
	}

	Obj->SetArrayField(TEXT("properties"), Properties);
	Obj->SetNumberField(TEXT("property_count_returned"), Count);
	return Obj;
}

static void AddDependencyArrays(UObject* Object, TSharedPtr<FJsonObject>& Root, int32 Limit)
{
	if (!Object || Limit <= 0)
	{
		return;
	}

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& Registry = AssetRegistryModule.Get();

	TArray<FName> Dependencies;
	TArray<FName> SoftDependencies;
	const FName PackageName = Object->GetOutermost()->GetFName();
	Registry.GetDependencies(PackageName, Dependencies, UE::AssetRegistry::EDependencyCategory::Package, UE::AssetRegistry::EDependencyQuery::Hard);
	Registry.GetDependencies(PackageName, SoftDependencies, UE::AssetRegistry::EDependencyCategory::Package, UE::AssetRegistry::EDependencyQuery::Soft);

	auto NamesToJson = [Limit](const TArray<FName>& Names) -> TArray<TSharedPtr<FJsonValue>>
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		for (int32 i = 0; i < Names.Num() && i < Limit; ++i)
		{
			Array.Add(MakeShared<FJsonValueString>(Names[i].ToString()));
		}
		return Array;
	};

	Root->SetArrayField(TEXT("hard_dependencies"), NamesToJson(Dependencies));
	Root->SetArrayField(TEXT("soft_dependencies"), NamesToJson(SoftDependencies));

	auto MissingProjectPackagesToJson = [&Registry, Limit](const TArray<FName>& Names) -> TArray<TSharedPtr<FJsonValue>>
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		for (const FName& Name : Names)
		{
			if (Array.Num() >= Limit)
			{
				break;
			}

			const FString PackagePath = Name.ToString();
			if (!PackagePath.StartsWith(TEXT("/Game/")))
			{
				continue;
			}

			TArray<FAssetData> Assets;
			Registry.GetAssetsByPackageName(Name, Assets, true);
			if (Assets.IsEmpty())
			{
				Array.Add(MakeShared<FJsonValueString>(PackagePath));
			}
		}
		return Array;
	};

	TArray<TSharedPtr<FJsonValue>> MissingHardDependencies = MissingProjectPackagesToJson(Dependencies);
	TArray<TSharedPtr<FJsonValue>> MissingSoftDependencies = MissingProjectPackagesToJson(SoftDependencies);
	Root->SetArrayField(TEXT("missing_hard_dependencies"), MissingHardDependencies);
	Root->SetArrayField(TEXT("missing_soft_dependencies"), MissingSoftDependencies);
	Root->SetNumberField(TEXT("missing_hard_dependency_count"), MissingHardDependencies.Num());
	Root->SetNumberField(TEXT("missing_soft_dependency_count"), MissingSoftDependencies.Num());
}

static void AddReferencerArrays(UObject* Object, TSharedPtr<FJsonObject>& Root, int32 Limit)
{
	if (!Object || Limit <= 0)
	{
		return;
	}

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& Registry = AssetRegistryModule.Get();

	TArray<FName> HardReferencers;
	TArray<FName> SoftReferencers;
	const FName PackageName = Object->GetOutermost()->GetFName();
	Registry.GetReferencers(PackageName, HardReferencers, UE::AssetRegistry::EDependencyCategory::Package, UE::AssetRegistry::EDependencyQuery::Hard);
	Registry.GetReferencers(PackageName, SoftReferencers, UE::AssetRegistry::EDependencyCategory::Package, UE::AssetRegistry::EDependencyQuery::Soft);

	auto NamesToJson = [Limit](const TArray<FName>& Names) -> TArray<TSharedPtr<FJsonValue>>
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		for (int32 i = 0; i < Names.Num() && i < Limit; ++i)
		{
			Array.Add(MakeShared<FJsonValueString>(Names[i].ToString()));
		}
		return Array;
	};

	Root->SetArrayField(TEXT("hard_referencers"), NamesToJson(HardReferencers));
	Root->SetArrayField(TEXT("soft_referencers"), NamesToJson(SoftReferencers));
	Root->SetNumberField(TEXT("hard_referencer_count"), HardReferencers.Num());
	Root->SetNumberField(TEXT("soft_referencer_count"), SoftReferencers.Num());
}

static TSharedPtr<FJsonObject> ParticleModuleToJson(UObject* Module, bool bIncludeProperties, int32 MaxProperties)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	if (!Module)
	{
		Obj->SetStringField(TEXT("class"), TEXT("null"));
		return Obj;
	}

	Obj->SetStringField(TEXT("name"), Module->GetName());
	Obj->SetStringField(TEXT("class"), Module->GetClass()->GetName());
	Obj->SetStringField(TEXT("path"), Module->GetPathName());
	if (bIncludeProperties)
	{
		Obj->SetObjectField(TEXT("reflected"), ReflectObject(Module, MaxProperties));
	}
	return Obj;
}

static TSharedPtr<FJsonObject> BuildParticleSystemSummary(UParticleSystem* ParticleSystem, bool bIncludeModuleProperties, int32 MaxLods, int32 MaxModuleProperties)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_kind"), TEXT("cascade_particle_system"));
	Root->SetNumberField(TEXT("emitter_count"), ParticleSystem ? ParticleSystem->Emitters.Num() : 0);
	Root->SetNumberField(TEXT("update_time_fps"), ParticleSystem ? ParticleSystem->UpdateTime_FPS : 0.0f);
	Root->SetNumberField(TEXT("warmup_time"), ParticleSystem ? ParticleSystem->WarmupTime : 0.0f);
	Root->SetNumberField(TEXT("seconds_before_inactive"), ParticleSystem ? ParticleSystem->SecondsBeforeInactive : 0.0f);

	TArray<TSharedPtr<FJsonValue>> Emitters;
	if (!ParticleSystem)
	{
		Root->SetArrayField(TEXT("emitters"), Emitters);
		return Root;
	}

	for (int32 EmitterIndex = 0; EmitterIndex < ParticleSystem->Emitters.Num(); ++EmitterIndex)
	{
		UParticleEmitter* Emitter = ParticleSystem->Emitters[EmitterIndex];
		TSharedPtr<FJsonObject> EmitterObj = MakeShared<FJsonObject>();
		EmitterObj->SetNumberField(TEXT("index"), EmitterIndex);
		if (!Emitter)
		{
			EmitterObj->SetStringField(TEXT("class"), TEXT("null"));
			Emitters.Add(MakeShared<FJsonValueObject>(EmitterObj));
			continue;
		}

		EmitterObj->SetStringField(TEXT("name"), Emitter->GetEmitterName().ToString());
		EmitterObj->SetStringField(TEXT("class"), Emitter->GetClass()->GetName());
		EmitterObj->SetNumberField(TEXT("lod_count"), Emitter->LODLevels.Num());

		TArray<TSharedPtr<FJsonValue>> LodArray;
		const int32 NumLodsToRead = MaxLods > 0 ? FMath::Min(MaxLods, Emitter->LODLevels.Num()) : Emitter->LODLevels.Num();
		for (int32 LodIndex = 0; LodIndex < NumLodsToRead; ++LodIndex)
		{
			UParticleLODLevel* Lod = Emitter->LODLevels[LodIndex];
			TSharedPtr<FJsonObject> LodObj = MakeShared<FJsonObject>();
			LodObj->SetNumberField(TEXT("index"), LodIndex);
			if (!Lod)
			{
				LodObj->SetStringField(TEXT("class"), TEXT("null"));
				LodArray.Add(MakeShared<FJsonValueObject>(LodObj));
				continue;
			}

			LodObj->SetBoolField(TEXT("enabled"), Lod->bEnabled != 0);
			LodObj->SetObjectField(TEXT("required_module"), ParticleModuleToJson(Lod->RequiredModule.Get(), bIncludeModuleProperties, MaxModuleProperties));
			LodObj->SetObjectField(TEXT("spawn_module"), ParticleModuleToJson(Lod->SpawnModule.Get(), bIncludeModuleProperties, MaxModuleProperties));
			LodObj->SetObjectField(TEXT("type_data_module"), ParticleModuleToJson(Lod->TypeDataModule.Get(), bIncludeModuleProperties, MaxModuleProperties));

			TArray<TSharedPtr<FJsonValue>> Modules;
			for (UParticleModule* Module : Lod->Modules)
			{
				Modules.Add(MakeShared<FJsonValueObject>(ParticleModuleToJson(Module, bIncludeModuleProperties, MaxModuleProperties)));
			}
			LodObj->SetArrayField(TEXT("modules"), Modules);
			LodArray.Add(MakeShared<FJsonValueObject>(LodObj));
		}
		EmitterObj->SetArrayField(TEXT("lods"), LodArray);
		Emitters.Add(MakeShared<FJsonValueObject>(EmitterObj));
	}

	Root->SetArrayField(TEXT("emitters"), Emitters);
	return Root;
}

static TSharedPtr<FJsonObject> BuildAssetSummary(UObject* Object, const TSharedPtr<FJsonObject>& Options)
{
	bool bIncludeReflected = false;
	bool bIncludeDependencies = true;
	bool bIncludeReferencers = false;
	int32 MaxDependencyCount = 80;
	int32 MaxReferencerCount = 80;
	int32 MaxProperties = 64;
	int32 MaxModuleProperties = 32;
	int32 MaxLods = 1;
	bool bIncludeModuleProperties = false;
	if (Options.IsValid())
	{
		Options->TryGetBoolField(TEXT("include_reflected"), bIncludeReflected);
		Options->TryGetBoolField(TEXT("include_dependencies"), bIncludeDependencies);
		Options->TryGetBoolField(TEXT("include_referencers"), bIncludeReferencers);
		Options->TryGetNumberField(TEXT("max_dependencies"), MaxDependencyCount);
		Options->TryGetNumberField(TEXT("max_referencers"), MaxReferencerCount);
		Options->TryGetNumberField(TEXT("max_properties"), MaxProperties);
		Options->TryGetNumberField(TEXT("max_module_properties"), MaxModuleProperties);
		Options->TryGetNumberField(TEXT("max_lods"), MaxLods);
		Options->TryGetBoolField(TEXT("include_module_properties"), bIncludeModuleProperties);
	}

	TSharedPtr<FJsonObject> Root = MakeSuccessObject();
	if (!Object)
	{
		Root->SetStringField(TEXT("asset_kind"), TEXT("null"));
		return Root;
	}

	Root->SetStringField(TEXT("path"), Object->GetPathName());
	Root->SetStringField(TEXT("package"), Object->GetOutermost() ? Object->GetOutermost()->GetName() : TEXT(""));
	Root->SetStringField(TEXT("class"), Object->GetClass()->GetPathName());

	if (UParticleSystem* ParticleSystem = Cast<UParticleSystem>(Object))
	{
		Root->SetObjectField(TEXT("cascade"), BuildParticleSystemSummary(ParticleSystem, bIncludeModuleProperties, MaxLods, MaxModuleProperties));
	}
	else if (UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(Object))
	{
		Root->SetStringField(TEXT("asset_kind"), TEXT("niagara_system"));
		Root->SetStringField(TEXT("recommended_reader"), TEXT("MCP_NiagaraReadSummary or MCP_NiagaraExportText"));
		Root->SetBoolField(TEXT("ready_to_run"), NiagaraSystem->IsReadyToRun());
		Root->SetNumberField(TEXT("warmup_time"), NiagaraSystem->GetWarmupTime());
	}
	else if (UMaterialInterface* Material = Cast<UMaterialInterface>(Object))
	{
		Root->SetStringField(TEXT("asset_kind"), TEXT("material"));
		Root->SetStringField(TEXT("base_material"), Material->GetBaseMaterial() ? Material->GetBaseMaterial()->GetPathName() : TEXT(""));
	}
	else if (UBlueprint* Blueprint = Cast<UBlueprint>(Object))
	{
		Root->SetStringField(TEXT("asset_kind"), TEXT("blueprint"));
		Root->SetStringField(TEXT("generated_class"), Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : TEXT(""));
		Root->SetStringField(TEXT("parent_class"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT(""));
	}
	else
	{
		Root->SetStringField(TEXT("asset_kind"), TEXT("generic"));
	}

	if (bIncludeReflected)
	{
		Root->SetObjectField(TEXT("reflected"), ReflectObject(Object, MaxProperties));
	}
	if (bIncludeDependencies)
	{
		AddDependencyArrays(Object, Root, MaxDependencyCount);
	}
	if (bIncludeReferencers)
	{
		AddReferencerArrays(Object, Root, MaxReferencerCount);
	}
	return Root;
}

static TArray<UClass*> DefaultVisualClasses()
{
	return {
		UNiagaraSystem::StaticClass(),
		UParticleSystem::StaticClass(),
		UMaterial::StaticClass(),
		UMaterialInstance::StaticClass(),
		UBlueprint::StaticClass(),
		UStaticMesh::StaticClass(),
		UTexture::StaticClass(),
		USoundBase::StaticClass()
	};
}

static UClass* ResolveClassAlias(const FString& Alias)
{
	const FString Lower = Alias.ToLower();
	if (Lower == TEXT("niagara") || Lower == TEXT("niagarasystem")) return UNiagaraSystem::StaticClass();
	if (Lower == TEXT("cascade") || Lower == TEXT("particlesystem") || Lower == TEXT("particle")) return UParticleSystem::StaticClass();
	if (Lower == TEXT("material")) return UMaterial::StaticClass();
	if (Lower == TEXT("materialinstance") || Lower == TEXT("mi")) return UMaterialInstance::StaticClass();
	if (Lower == TEXT("blueprint") || Lower == TEXT("bp")) return UBlueprint::StaticClass();
	if (Lower == TEXT("staticmesh") || Lower == TEXT("mesh")) return UStaticMesh::StaticClass();
	if (Lower == TEXT("texture")) return UTexture::StaticClass();
	if (Lower == TEXT("sound")) return USoundBase::StaticClass();
	return nullptr;
}

static UClass* LoadFxRendererClass(const FString& TargetClassPath, FString& OutError)
{
	UClass* TargetClass = LoadObject<UClass>(nullptr, *NormalizeClassPath(TargetClassPath));
	if (!TargetClass)
	{
		UObject* MaybeBlueprintObject = nullptr;
		FString LoadError;
		MaybeBlueprintObject = LoadAnyObject(TargetClassPath, LoadError);
		if (UBlueprint* Blueprint = Cast<UBlueprint>(MaybeBlueprintObject))
		{
			TargetClass = Blueprint->GeneratedClass;
		}
	}

	if (!TargetClass)
	{
		OutError = FString::Printf(TEXT("Failed to load target class: %s"), *TargetClassPath);
		return nullptr;
	}
	if (!TargetClass->IsChildOf(AMassBattleFxRenderer::StaticClass()))
	{
		OutError = FString::Printf(TEXT("Target class is not a subclass of AMassBattleFxRenderer: %s"), *TargetClass->GetPathName());
		return nullptr;
	}
	return TargetClass;
}

static TSharedPtr<FJsonObject> FxRendererDefaultsToJson(const UClass* TargetClass, const AMassBattleFxRenderer* CDO)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	if (!TargetClass || !CDO)
	{
		return Obj;
	}

	Obj->SetStringField(TEXT("class_path"), TargetClass->GetPathName());
	Obj->SetStringField(TEXT("blueprint_asset"), TEXT(""));
	if (const UBlueprintGeneratedClass* BPClass = Cast<UBlueprintGeneratedClass>(TargetClass))
	{
		if (const UBlueprint* Blueprint = Cast<UBlueprint>(BPClass->ClassGeneratedBy))
		{
			Obj->SetStringField(TEXT("blueprint_asset"), Blueprint->GetPathName());
		}
	}
	Obj->SetStringField(TEXT("cdo_path"), CDO->GetPathName());
	Obj->SetStringField(TEXT("niagara_system"), CDO->NiagaraSystemAsset ? CDO->NiagaraSystemAsset->GetPathName() : TEXT(""));
	Obj->SetStringField(TEXT("ndc_burst_fx"), CDO->NDC_BurstFx ? CDO->NDC_BurstFx->GetPathName() : TEXT(""));
	Obj->SetNumberField(TEXT("subtype"), CDO->SubType.Index);
	Obj->SetNumberField(TEXT("render_batch_size"), CDO->RenderBatchSize);
	Obj->SetNumberField(TEXT("pooling_cooldown"), CDO->PoollingCoolDown);
	Obj->SetStringField(TEXT("edit_scope"), TEXT("blueprint_class_defaults"));
	return Obj;
}
}

FString UMassBattleEffectAssetMCPApi::MCP_EffectAssetGetApiStatus()
{
	using namespace MassBattleEffectAssetMCP;

	TSharedPtr<FJsonObject> Root = MakeSuccessObject();
	Root->SetStringField(TEXT("api_name"), TEXT("MassBattleEffectAssetMCP"));
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

	Tools.Add(Tool(TEXT("MCP_EffectAssetQuery"), TEXT("effect_asset.query"), TEXT("Query visual effect-related assets across unknown Marketplace asset types."), TEXT("QueryJson")));
	Tools.Add(Tool(TEXT("MCP_EffectAssetReadSummary"), TEXT("effect_asset.read"), TEXT("Read a typed summary for Niagara, Cascade, material, Blueprint, or generic assets, including missing project dependency warnings."), TEXT("AssetPath, OptionsJson")));
	Tools.Add(Tool(TEXT("MCP_EffectAssetExportText"), TEXT("effect_asset.text"), TEXT("Write a deterministic text dump for close reading."), TEXT("AssetPath, OptionsJson")));
	Tools.Add(Tool(TEXT("MCP_EffectAssetSoftDelete"), TEXT("effect_asset.lifecycle"), TEXT("Plan moving an unreferenced generic asset to trash; apply is blocked by default unless allow_unsafe_asset_move=true is supplied."), TEXT("AssetPath, OptionsJson")));
	Tools.Add(Tool(TEXT("MCP_EffectDuplicateAsset"), TEXT("effect_asset.write"), TEXT("Duplicate an arbitrary asset into a package path."), TEXT("SourceAssetPath, NewAssetName, PackagePath, bSaveAssets")));
	Tools.Add(Tool(TEXT("MCP_EffectDiscardUnsavedDuplicate"), TEXT("effect_asset.rollback"), TEXT("Discard only an unsaved in-memory duplicate created by this MCP session; persisted assets are always rejected."), TEXT("AssetPath")));
	Tools.Add(Tool(TEXT("MCP_BatchFxReadRendererDefaults"), TEXT("batch_fx.read"), TEXT("Read AMassBattleFxRenderer Blueprint CDO defaults used by newly placed actors."), TEXT("TargetClassPath")));
	Tools.Add(Tool(TEXT("MCP_BatchFxSetRendererDefaults"), TEXT("batch_fx.write"), TEXT("Set AMassBattleFxRenderer Blueprint CDO defaults: Niagara, NDC_BurstFx, SubType, batch size, and pooling cooldown."), TEXT("TargetClassPath, NiagaraSystemPath, NdcBurstFxPath, SubType, RenderBatchSize, PoolingCooldown, bSaveAssets")));
	Root->SetArrayField(TEXT("tools"), Tools);
	return ToJsonString(Root);
}

FString UMassBattleEffectAssetMCPApi::MCP_EffectAssetQuery(const FString& QueryJson)
{
	using namespace MassBattleEffectAssetMCP;

	TSharedPtr<FJsonObject> Query = ParseObject(QueryJson);
	if (!Query.IsValid())
	{
		return MakeErrorJson(TEXT("QueryJson must be a JSON object"));
	}

	FString Text;
	FString RootPath = TEXT("/Game");
	int32 Limit = 100;
	bool bRecursiveClasses = true;
	Query->TryGetStringField(TEXT("query"), Text);
	Query->TryGetStringField(TEXT("path"), RootPath);
	Query->TryGetStringField(TEXT("root"), RootPath);
	Query->TryGetNumberField(TEXT("limit"), Limit);
	Query->TryGetBoolField(TEXT("recursive_classes"), bRecursiveClasses);
	Limit = FMath::Clamp(Limit, 1, 2000);

	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*RootPath));
	Filter.bRecursivePaths = true;
	Filter.bRecursiveClasses = bRecursiveClasses;

	bool bAllClasses = false;
	const TArray<TSharedPtr<FJsonValue>>* Classes = nullptr;
	if (Query->TryGetArrayField(TEXT("classes"), Classes))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Classes)
		{
			const FString ClassName = Value.IsValid() ? Value->AsString() : FString();
			if (ClassName.Equals(TEXT("all"), ESearchCase::IgnoreCase))
			{
				bAllClasses = true;
				break;
			}
			if (UClass* Class = ResolveClassAlias(ClassName))
			{
				Filter.ClassPaths.Add(Class->GetClassPathName());
			}
		}
	}
	else
	{
		for (UClass* Class : DefaultVisualClasses())
		{
			Filter.ClassPaths.Add(Class->GetClassPathName());
		}
	}

	if (bAllClasses)
	{
		Filter.ClassPaths.Reset();
	}

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	TArray<FAssetData> Assets;
	AssetRegistryModule.Get().GetAssets(Filter, Assets);
	Assets.Sort([](const FAssetData& A, const FAssetData& B)
	{
		return A.GetObjectPathString() < B.GetObjectPathString();
	});

	const FString TextLower = Text.ToLower();
	TArray<TSharedPtr<FJsonValue>> Results;
	for (const FAssetData& AssetData : Assets)
	{
		const FString ObjectPath = AssetData.GetObjectPathString();
		if (!TextLower.IsEmpty()
			&& !ObjectPath.ToLower().Contains(TextLower)
			&& !AssetData.AssetName.ToString().ToLower().Contains(TextLower)
			&& !AssetData.AssetClassPath.ToString().ToLower().Contains(TextLower))
		{
			continue;
		}

		Results.Add(MakeShared<FJsonValueObject>(AssetDataToJson(AssetData)));
		if (Results.Num() >= Limit)
		{
			break;
		}
	}

	TSharedPtr<FJsonObject> Root = MakeSuccessObject();
	Root->SetStringField(TEXT("query"), Text);
	Root->SetStringField(TEXT("root"), RootPath);
	Root->SetNumberField(TEXT("count"), Results.Num());
	Root->SetArrayField(TEXT("assets"), Results);
	return ToJsonString(Root);
}

FString UMassBattleEffectAssetMCPApi::MCP_EffectAssetReadSummary(const FString& AssetPath, const FString& OptionsJson)
{
	using namespace MassBattleEffectAssetMCP;

	TSharedPtr<FJsonObject> Options = ParseObject(OptionsJson);
	if (!Options.IsValid())
	{
		return MakeErrorJson(TEXT("OptionsJson must be a JSON object"));
	}

	FString LoadError;
	UObject* Object = LoadAnyObject(AssetPath, LoadError);
	if (!Object)
	{
		return MakeErrorJson(LoadError);
	}

	return ToJsonString(BuildAssetSummary(Object, Options));
}

FString UMassBattleEffectAssetMCPApi::MCP_EffectAssetExportText(const FString& AssetPath, const FString& OptionsJson)
{
	using namespace MassBattleEffectAssetMCP;

	TSharedPtr<FJsonObject> Options = ParseObject(OptionsJson);
	if (!Options.IsValid())
	{
		return MakeErrorJson(TEXT("OptionsJson must be a JSON object"));
	}

	bool bWriteFile = true;
	Options->TryGetBoolField(TEXT("write_file"), bWriteFile);
	Options->SetBoolField(TEXT("include_reflected"), true);
	Options->SetBoolField(TEXT("include_dependencies"), true);

	FString LoadError;
	UObject* Object = LoadAnyObject(AssetPath, LoadError);
	if (!Object)
	{
		return MakeErrorJson(LoadError);
	}

	TSharedPtr<FJsonObject> Summary = BuildAssetSummary(Object, Options);

	FString Text;
	Text += FString::Printf(TEXT("EffectAsset: %s\n"), *Object->GetPathName());
	Text += FString::Printf(TEXT("Class: %s\n"), *Object->GetClass()->GetPathName());
	Text += TEXT("\nJSON:\n");
	Text += ToJsonString(Summary);
	Text += LINE_TERMINATOR;

	TSharedPtr<FJsonObject> Root = MakeSuccessObject();
	Root->SetStringField(TEXT("text"), Text);
	if (bWriteFile)
	{
		IFileManager::Get().MakeDirectory(*GetSavedExportDir(), true);
		const FString AssetName = FPackageName::GetLongPackageAssetName(Object->GetOutermost()->GetName());
		const FString OutputPath = FPaths::Combine(GetSavedExportDir(), AssetName + TEXT("_effect.txt"));
		FFileHelper::SaveStringToFile(Text, *OutputPath);
		Root->SetStringField(TEXT("text_path"), OutputPath);
	}
	return ToJsonString(Root);
}

FString UMassBattleEffectAssetMCPApi::MCP_EffectAssetSoftDelete(const FString& AssetPath, const FString& OptionsJson)
{
	using namespace MassBattleEffectAssetMCP;

	TSharedPtr<FJsonObject> Options = ParseObject(OptionsJson);
	if (!Options.IsValid())
	{
		return MakeErrorJson(TEXT("OptionsJson must be a JSON object"));
	}

	FString LoadError;
	UObject* Object = LoadAnyObject(AssetPath, LoadError);
	if (!Object)
	{
		return MakeErrorJson(LoadError);
	}

	bool bDryRun = true;
	bool bAllowReferenced = false;
	bool bAllowUnsafeAssetMove = false;
	Options->TryGetBoolField(TEXT("dry_run"), bDryRun);
	Options->TryGetBoolField(TEXT("preview_only"), bDryRun);
	Options->TryGetBoolField(TEXT("allow_referenced"), bAllowReferenced);
	Options->TryGetBoolField(TEXT("allow_unsafe_asset_move"), bAllowUnsafeAssetMove);
	Options->TryGetBoolField(TEXT("force"), bAllowUnsafeAssetMove);

	TSharedPtr<FJsonObject> Root = MakeSuccessObject();
	Root->SetBoolField(TEXT("dry_run"), bDryRun);
	Root->SetStringField(TEXT("source_path"), Object->GetPathName());
	AddReferencerArrays(Object, Root, 200);

	const TArray<TSharedPtr<FJsonValue>>* HardRefs = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* SoftRefs = nullptr;
	const bool bHasHardRefs = Root->TryGetArrayField(TEXT("hard_referencers"), HardRefs) && HardRefs && !HardRefs->IsEmpty();
	const bool bHasSoftRefs = Root->TryGetArrayField(TEXT("soft_referencers"), SoftRefs) && SoftRefs && !SoftRefs->IsEmpty();
	const bool bBlocked = (bHasHardRefs || bHasSoftRefs) && !bAllowReferenced;
	Root->SetBoolField(TEXT("applicable"), !bBlocked);
	if (bBlocked)
	{
		Root->SetStringField(TEXT("blocked_reason"), TEXT("Asset has referencers; replace references or pass allow_referenced=true before moving."));
	}

	FString TrashPackagePath;
	Options->TryGetStringField(TEXT("trash_package_path"), TrashPackagePath);
	if (TrashPackagePath.TrimStartAndEnd().IsEmpty())
	{
		FString TrashRoot = TEXT("/Game/_Trash/MassBattle/Assets");
		Options->TryGetStringField(TEXT("trash_root"), TrashRoot);
		const FString DateFolder = FDateTime::UtcNow().ToString(TEXT("%Y-%m-%d"));
		TrashPackagePath = TrashRoot / DateFolder;
	}
	const FString TrashAssetName = Object->GetName();
	const FString TrashPath = TrashPackagePath / TrashAssetName + TEXT(".") + TrashAssetName;
	Root->SetStringField(TEXT("trash_path"), TrashPath);

	if (bDryRun || bBlocked)
	{
		Root->SetBoolField(TEXT("moved"), false);
		return ToJsonString(Root);
	}

	if (!bAllowUnsafeAssetMove)
	{
		Root->SetBoolField(TEXT("success"), false);
		Root->SetBoolField(TEXT("moved"), false);
		Root->SetBoolField(TEXT("applicable"), false);
		Root->SetStringField(TEXT("blocked_reason"), TEXT("AssetTools.RenameAssets can crash active MassEntity editor sessions; rerun in a safe editor state or pass allow_unsafe_asset_move=true after reviewing referencers."));
		return ToJsonString(Root);
	}

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	TArray<FAssetRenameData> RenameData;
	RenameData.Emplace(Object, TrashPackagePath, TrashAssetName);
	const bool bMoved = AssetToolsModule.Get().RenameAssets(RenameData);
	Root->SetBoolField(TEXT("moved"), bMoved);
	if (!bMoved)
	{
		Root->SetBoolField(TEXT("success"), false);
		Root->SetStringField(TEXT("error"), TEXT("AssetTools.RenameAssets failed."));
	}
	return ToJsonString(Root);
}

FString UMassBattleEffectAssetMCPApi::MCP_EffectDuplicateAsset(const FString& SourceAssetPath, const FString& NewAssetName, const FString& PackagePath, bool bSaveAssets)
{
	using namespace MassBattleEffectAssetMCP;

	FString LoadError;
	UObject* Source = LoadAnyObject(SourceAssetPath, LoadError);
	if (!Source)
	{
		return MakeErrorJson(LoadError);
	}
	if (NewAssetName.TrimStartAndEnd().IsEmpty() || PackagePath.TrimStartAndEnd().IsEmpty())
	{
		return MakeErrorJson(TEXT("NewAssetName and PackagePath are required"));
	}

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	UObject* NewAsset = AssetToolsModule.Get().DuplicateAsset(NewAssetName, PackagePath, Source);
	if (!NewAsset)
	{
		return MakeErrorJson(TEXT("AssetTools.DuplicateAsset returned null"));
	}

	FString SaveError;
	bool bSaved = false;
	if (bSaveAssets)
	{
		bSaved = SaveLoadedAsset(NewAsset, SaveError);
		if (!bSaved)
		{
			return MakeErrorJson(SaveError);
		}
	}
	else
	{
		UnsavedMCPDuplicatePaths.Add(NewAsset->GetPathName());
	}

	TSharedPtr<FJsonObject> Root = MakeSuccessObject();
	Root->SetStringField(TEXT("asset_path"), NewAsset->GetPathName());
	Root->SetStringField(TEXT("package"), NewAsset->GetOutermost() ? NewAsset->GetOutermost()->GetName() : TEXT(""));
	Root->SetBoolField(TEXT("saved"), bSaved);
	return ToJsonString(Root);
}

FString UMassBattleEffectAssetMCPApi::MCP_EffectDiscardUnsavedDuplicate(const FString& AssetPath)
{
	using namespace MassBattleEffectAssetMCP;

	const FString ObjectPath = NormalizeObjectPath(AssetPath);
	if (!UnsavedMCPDuplicatePaths.Contains(ObjectPath))
	{
		return MakeErrorJson(TEXT("Asset is not an unsaved duplicate created by MCP_EffectDuplicateAsset in this editor session"));
	}

	UObject* Object = FindObject<UObject>(nullptr, *ObjectPath);
	if (!Object || !Object->IsAsset())
	{
		UnsavedMCPDuplicatePaths.Remove(ObjectPath);
		return MakeErrorJson(TEXT("Tracked unsaved duplicate is no longer a loaded asset"));
	}

	UPackage* Package = Object->GetOutermost();
	const FString PackageName = Package ? Package->GetName() : FString();
	FString ExistingFilename;
	if (!Package || !PackageName.StartsWith(TEXT("/Game/")))
	{
		return MakeErrorJson(TEXT("Unsaved duplicate must belong to a /Game/ package"));
	}
	if (FPackageName::DoesPackageExist(PackageName, &ExistingFilename))
	{
		UnsavedMCPDuplicatePaths.Remove(ObjectPath);
		return MakeErrorJson(FString::Printf(
			TEXT("Refusing rollback because the asset package exists on disk: %s"),
			*ExistingFilename));
	}
	if (!Package->IsDirty())
	{
		return MakeErrorJson(TEXT("Refusing rollback because the tracked package is not dirty"));
	}

	TArray<UObject*> ObjectsToDiscard;
	ObjectsToDiscard.Add(Object);
	const int32 DeletedCount = ObjectTools::DeleteObjectsUnchecked(ObjectsToDiscard);
	const bool bDiscarded = DeletedCount == 1;
	if (bDiscarded)
	{
		UnsavedMCPDuplicatePaths.Remove(ObjectPath);
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("success"), bDiscarded);
	Root->SetBoolField(TEXT("discarded"), bDiscarded);
	Root->SetStringField(TEXT("asset_path"), ObjectPath);
	Root->SetStringField(TEXT("package"), PackageName);
	Root->SetNumberField(TEXT("deleted_count"), DeletedCount);
	if (!bDiscarded)
	{
		Root->SetStringField(TEXT("error"), TEXT("ObjectTools failed to discard the unsaved duplicate"));
	}
	return ToJsonString(Root);
}

FString UMassBattleEffectAssetMCPApi::MCP_BatchFxReadRendererDefaults(const FString& TargetClassPath)
{
	using namespace MassBattleEffectAssetMCP;

	FString ClassError;
	UClass* TargetClass = LoadFxRendererClass(TargetClassPath, ClassError);
	if (!TargetClass)
	{
		return MakeErrorJson(ClassError);
	}

	AMassBattleFxRenderer* CDO = TargetClass->GetDefaultObject<AMassBattleFxRenderer>();
	if (!CDO)
	{
		return MakeErrorJson(TEXT("Failed to get AMassBattleFxRenderer CDO"));
	}

	TSharedPtr<FJsonObject> Root = MakeSuccessObject();
	Root->SetObjectField(TEXT("defaults"), FxRendererDefaultsToJson(TargetClass, CDO));
	return ToJsonString(Root);
}

FString UMassBattleEffectAssetMCPApi::MCP_BatchFxSetRendererDefaults(const FString& TargetClassPath, const FString& NiagaraSystemPath, const FString& NdcBurstFxPath, int32 SubType, int32 RenderBatchSize, float PoolingCooldown, bool bSaveAssets)
{
	using namespace MassBattleEffectAssetMCP;

	FString ClassError;
	UClass* TargetClass = LoadFxRendererClass(TargetClassPath, ClassError);
	if (!TargetClass)
	{
		return MakeErrorJson(ClassError);
	}

	UNiagaraSystem* NiagaraSystem = nullptr;
	if (!NiagaraSystemPath.TrimStartAndEnd().IsEmpty())
	{
		NiagaraSystem = LoadObject<UNiagaraSystem>(nullptr, *NormalizeObjectPath(NiagaraSystemPath));
		if (!NiagaraSystem)
		{
			return MakeErrorJson(FString::Printf(TEXT("Failed to load NiagaraSystem: %s"), *NiagaraSystemPath));
		}
	}

	UNiagaraDataChannelAsset* NdcBurstFx = nullptr;
	if (!NdcBurstFxPath.TrimStartAndEnd().IsEmpty())
	{
		NdcBurstFx = LoadObject<UNiagaraDataChannelAsset>(nullptr, *NormalizeObjectPath(NdcBurstFxPath));
		if (!NdcBurstFx)
		{
			return MakeErrorJson(FString::Printf(TEXT("Failed to load NiagaraDataChannelAsset: %s"), *NdcBurstFxPath));
		}
	}

	AMassBattleFxRenderer* CDO = TargetClass->GetDefaultObject<AMassBattleFxRenderer>();
	if (!CDO)
	{
		return MakeErrorJson(TEXT("Failed to get AMassBattleFxRenderer CDO"));
	}

	CDO->Modify();
	if (NiagaraSystem)
	{
		CDO->NiagaraSystemAsset = NiagaraSystem;
	}
	if (NdcBurstFx)
	{
		CDO->NDC_BurstFx = NdcBurstFx;
	}
	if (SubType >= 0)
	{
		CDO->SubType.Index = SubType;
	}
	if (RenderBatchSize > 0)
	{
		CDO->RenderBatchSize = RenderBatchSize;
	}
	if (PoolingCooldown >= 0.0f)
	{
		CDO->PoollingCoolDown = PoolingCooldown;
	}
	CDO->MarkPackageDirty();

	if (UBlueprintGeneratedClass* BPClass = Cast<UBlueprintGeneratedClass>(TargetClass))
	{
		if (UBlueprint* Blueprint = Cast<UBlueprint>(BPClass->ClassGeneratedBy))
		{
			FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
			Blueprint->MarkPackageDirty();
		}
	}

	bool bSaved = false;
	if (bSaveAssets)
	{
		UObject* SaveTarget = TargetClass;
		if (UBlueprintGeneratedClass* BPClass = Cast<UBlueprintGeneratedClass>(TargetClass))
		{
			if (UBlueprint* Blueprint = Cast<UBlueprint>(BPClass->ClassGeneratedBy))
			{
				SaveTarget = Blueprint;
			}
		}

		FString SaveError;
		bSaved = SaveLoadedAsset(SaveTarget, SaveError);
		if (!bSaved)
		{
			return MakeErrorJson(SaveError);
		}
	}

	TSharedPtr<FJsonObject> Root = MakeSuccessObject();
	Root->SetStringField(TEXT("class_path"), TargetClass->GetPathName());
	Root->SetStringField(TEXT("niagara_system"), CDO->NiagaraSystemAsset ? CDO->NiagaraSystemAsset->GetPathName() : TEXT(""));
	Root->SetStringField(TEXT("ndc_burst_fx"), CDO->NDC_BurstFx ? CDO->NDC_BurstFx->GetPathName() : TEXT(""));
	Root->SetNumberField(TEXT("subtype"), CDO->SubType.Index);
	Root->SetNumberField(TEXT("render_batch_size"), CDO->RenderBatchSize);
	Root->SetNumberField(TEXT("pooling_cooldown"), CDO->PoollingCoolDown);
	Root->SetBoolField(TEXT("saved"), bSaved);
	return ToJsonString(Root);
}
