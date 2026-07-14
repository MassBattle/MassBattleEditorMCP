// Copyright (c) 2025-2026 Winyunq. All rights reserved.

#include "MassBattleUnitEditorMCPApi.h"

#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/MeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SkeletalMeshAttributes.h"
#include "SkeletalMeshOperations.h"
#include "SkinnedAssetCompiler.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "StaticToSkeletalMeshConverter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"

namespace MassBattleActorUnitEditorMCP
{
static FString ToJsonString(const TSharedPtr<FJsonObject>& Object)
{
	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	if (Object.IsValid())
	{
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
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
	return FJsonSerializer::Deserialize(Reader, Object) && Object.IsValid() ? Object : nullptr;
}

static TSharedPtr<FJsonObject> MakeResult(bool bSuccess)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), bSuccess);
	return Result;
}

static FString MakeError(const FString& Error)
{
	TSharedPtr<FJsonObject> Result = MakeResult(false);
	Result->SetStringField(TEXT("error"), Error);
	return ToJsonString(Result);
}

static void AddIssue(
	TArray<TSharedPtr<FJsonValue>>& Issues,
	const FString& Severity,
	const FString& Code,
	const FString& Message,
	const FString& Field = FString())
{
	TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
	Issue->SetStringField(TEXT("severity"), Severity);
	Issue->SetStringField(TEXT("code"), Code);
	Issue->SetStringField(TEXT("message"), Message);
	if (!Field.IsEmpty())
	{
		Issue->SetStringField(TEXT("field"), Field);
	}
	Issues.Add(MakeShared<FJsonValueObject>(Issue));
}

static bool HasErrors(const TArray<TSharedPtr<FJsonValue>>& Issues)
{
	for (const TSharedPtr<FJsonValue>& Value : Issues)
	{
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			continue;
		}
		FString Severity;
		if (Value->AsObject()->TryGetStringField(TEXT("severity"), Severity)
			&& Severity.Equals(TEXT("error"), ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
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
	if (Path.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd)
		&& !AssetName.IsEmpty())
	{
		return Path + TEXT(".") + AssetName;
	}
	return Path;
}

static bool AssetExistsWithoutLoading(const FString& InPath)
{
	const FString ObjectPath = NormalizeObjectPath(InPath);
	if (ObjectPath.IsEmpty())
	{
		return false;
	}

	if (FindObject<UObject>(nullptr, *ObjectPath))
	{
		return true;
	}

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	if (AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid())
	{
		return true;
	}

	const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
	return !PackageName.IsEmpty() && FPackageName::DoesPackageExist(PackageName);
}

template <typename AssetType>
static AssetType* LoadAsset(const FString& Path)
{
	if (Path.TrimStartAndEnd().IsEmpty())
	{
		return nullptr;
	}
	return Cast<AssetType>(FSoftObjectPath(NormalizeObjectPath(Path)).TryLoad());
}

static UClass* LoadActorClass(const FString& ActorPath)
{
	const FString NormalizedPath = NormalizeObjectPath(ActorPath);
	if (NormalizedPath.IsEmpty())
	{
		return nullptr;
	}

	if (UClass* Class = Cast<UClass>(FSoftObjectPath(NormalizedPath).TryLoad()))
	{
		return Class->IsChildOf(AActor::StaticClass()) ? Class : nullptr;
	}

	if (UBlueprint* Blueprint = Cast<UBlueprint>(FSoftObjectPath(NormalizedPath).TryLoad()))
	{
		return Blueprint->GeneratedClass && Blueprint->GeneratedClass->IsChildOf(AActor::StaticClass())
			? Blueprint->GeneratedClass
			: nullptr;
	}

	const FString GeneratedClassPath = NormalizedPath.EndsWith(TEXT("_C"))
		? NormalizedPath
		: NormalizedPath + TEXT("_C");
	UClass* GeneratedClass = Cast<UClass>(FSoftObjectPath(GeneratedClassPath).TryLoad());
	return GeneratedClass && GeneratedClass->IsChildOf(AActor::StaticClass()) ? GeneratedClass : nullptr;
}

static FString GetStringField(const TSharedPtr<FJsonObject>& Object, const FString& Name)
{
	FString Value;
	if (Object.IsValid())
	{
		Object->TryGetStringField(Name, Value);
	}
	return Value;
}

static FString AssetPath(const UObject* Object)
{
	return Object ? Object->GetPathName() : FString();
}

static TSharedPtr<FJsonObject> VectorJson(const FVector& Value)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("x"), Value.X);
	Object->SetNumberField(TEXT("y"), Value.Y);
	Object->SetNumberField(TEXT("z"), Value.Z);
	return Object;
}

static TSharedPtr<FJsonObject> TransformJson(const FTransform& Transform)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetObjectField(TEXT("translation"), VectorJson(Transform.GetTranslation()));
	Object->SetObjectField(TEXT("rotation"), VectorJson(Transform.Rotator().Euler()));
	Object->SetObjectField(TEXT("scale"), VectorJson(Transform.GetScale3D()));
	return Object;
}

struct FActorAssemblyContext
{
	AActor* Actor = nullptr;
	USkeletalMeshComponent* RootSkeletalComponent = nullptr;
	TArray<USkeletalMeshComponent*> SkeletalComponents;
	TArray<UStaticMeshComponent*> StaticComponents;
	TMap<FString, TSharedPtr<FJsonObject>> OverridesByComponent;
	TArray<TSharedPtr<FJsonValue>> ComponentDescriptions;
	TArray<TSharedPtr<FJsonValue>> Issues;

	~FActorAssemblyContext()
	{
		if (Actor && IsValid(Actor))
		{
			Actor->Destroy();
		}
	}
};

static UActorComponent* FindComponentByName(AActor* Actor, const FString& ComponentName)
{
	if (!Actor || ComponentName.IsEmpty())
	{
		return nullptr;
	}

	TInlineComponentArray<UActorComponent*> Components(Actor);
	for (UActorComponent* Component : Components)
	{
		if (Component && Component->GetName().Equals(ComponentName, ESearchCase::IgnoreCase))
		{
			return Component;
		}
	}
	return nullptr;
}

static bool ApplyComponentOverrides(const TSharedPtr<FJsonObject>& Spec, FActorAssemblyContext& Context)
{
	const TArray<TSharedPtr<FJsonValue>>* Overrides = nullptr;
	if (!Spec.IsValid() || !Spec->TryGetArrayField(TEXT("component_overrides"), Overrides) || !Overrides)
	{
		return true;
	}

	for (int32 OverrideIndex = 0; OverrideIndex < Overrides->Num(); ++OverrideIndex)
	{
		const TSharedPtr<FJsonValue>& Value = (*Overrides)[OverrideIndex];
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			AddIssue(Context.Issues, TEXT("error"), TEXT("invalid_component_override"),
				FString::Printf(TEXT("component_overrides[%d] must be an object."), OverrideIndex),
				TEXT("component_overrides"));
			continue;
		}

		TSharedPtr<FJsonObject> Override = Value->AsObject();
		FString ComponentName = GetStringField(Override, TEXT("component"));
		if (ComponentName.IsEmpty())
		{
			ComponentName = GetStringField(Override, TEXT("component_name"));
		}
		if (ComponentName.IsEmpty())
		{
			AddIssue(Context.Issues, TEXT("error"), TEXT("missing_component_name"),
				FString::Printf(TEXT("component_overrides[%d] is missing component."), OverrideIndex),
				TEXT("component_overrides"));
			continue;
		}

		UActorComponent* Component = FindComponentByName(Context.Actor, ComponentName);
		if (!Component)
		{
			AddIssue(Context.Issues, TEXT("error"), TEXT("actor_component_not_found"),
				FString::Printf(TEXT("Actor component was not found: %s"), *ComponentName),
				TEXT("component_overrides"));
			continue;
		}
		Context.OverridesByComponent.Add(Component->GetName().ToLower(), Override);

		bool bClearMesh = false;
		Override->TryGetBoolField(TEXT("clear_mesh"), bClearMesh);
		if (USkeletalMeshComponent* SkeletalComponent = Cast<USkeletalMeshComponent>(Component))
		{
			if (bClearMesh)
			{
				SkeletalComponent->SetSkeletalMesh(nullptr);
			}
			if (Override->HasField(TEXT("skeletal_mesh")) || Override->HasField(TEXT("mesh")))
			{
				FString MeshPath = GetStringField(Override, TEXT("skeletal_mesh"));
				if (MeshPath.IsEmpty())
				{
					MeshPath = GetStringField(Override, TEXT("mesh"));
				}
				USkeletalMesh* Mesh = MeshPath.IsEmpty() ? nullptr : LoadAsset<USkeletalMesh>(MeshPath);
				if (!MeshPath.IsEmpty() && !Mesh)
				{
					AddIssue(Context.Issues, TEXT("error"), TEXT("skeletal_mesh_not_found"),
						FString::Printf(TEXT("SkeletalMesh override failed to load for %s: %s"), *ComponentName, *MeshPath),
						TEXT("component_overrides"));
				}
				else
				{
					SkeletalComponent->SetSkeletalMesh(Mesh);
				}
			}
		}
		else if (UStaticMeshComponent* StaticComponent = Cast<UStaticMeshComponent>(Component))
		{
			if (bClearMesh)
			{
				StaticComponent->SetStaticMesh(nullptr);
			}
			if (Override->HasField(TEXT("static_mesh")) || Override->HasField(TEXT("mesh")))
			{
				FString MeshPath = GetStringField(Override, TEXT("static_mesh"));
				if (MeshPath.IsEmpty())
				{
					MeshPath = GetStringField(Override, TEXT("mesh"));
				}
				UStaticMesh* Mesh = MeshPath.IsEmpty() ? nullptr : LoadAsset<UStaticMesh>(MeshPath);
				if (!MeshPath.IsEmpty() && !Mesh)
				{
					AddIssue(Context.Issues, TEXT("error"), TEXT("static_mesh_not_found"),
						FString::Printf(TEXT("StaticMesh override failed to load for %s: %s"), *ComponentName, *MeshPath),
						TEXT("component_overrides"));
				}
				else
				{
					StaticComponent->SetStaticMesh(Mesh);
				}
			}
		}
		else if (bClearMesh || Override->HasField(TEXT("skeletal_mesh")) || Override->HasField(TEXT("static_mesh")) || Override->HasField(TEXT("mesh")))
		{
			AddIssue(Context.Issues, TEXT("error"), TEXT("component_is_not_mesh_component"),
				FString::Printf(TEXT("Component %s cannot accept a mesh override."), *ComponentName),
				TEXT("component_overrides"));
		}

		if (USceneComponent* SceneComponent = Cast<USceneComponent>(Component))
		{
			bool bVisible = true;
			if (Override->TryGetBoolField(TEXT("visible"), bVisible))
			{
				SceneComponent->SetVisibility(bVisible, true);
				SceneComponent->SetHiddenInGame(!bVisible, true);
			}
		}

		if (UMeshComponent* MeshComponent = Cast<UMeshComponent>(Component))
		{
			const TArray<TSharedPtr<FJsonValue>>* Materials = nullptr;
			if (Override->TryGetArrayField(TEXT("materials"), Materials) && Materials)
			{
				for (int32 MaterialIndex = 0; MaterialIndex < Materials->Num(); ++MaterialIndex)
				{
					const FString MaterialPath = (*Materials)[MaterialIndex].IsValid() ? (*Materials)[MaterialIndex]->AsString() : FString();
					UMaterialInterface* Material = MaterialPath.IsEmpty() ? nullptr : LoadAsset<UMaterialInterface>(MaterialPath);
					if (!MaterialPath.IsEmpty() && !Material)
					{
						AddIssue(Context.Issues, TEXT("error"), TEXT("material_not_found"),
							FString::Printf(TEXT("Material override failed to load for %s[%d]: %s"), *ComponentName, MaterialIndex, *MaterialPath),
							TEXT("component_overrides"));
						continue;
					}
					MeshComponent->SetMaterial(MaterialIndex, Material);
				}
			}
		}
	}
	return !HasErrors(Context.Issues);
}

static FString ResolveBindBone(const UStaticMeshComponent* StaticComponent, const FActorAssemblyContext& Context)
{
	if (!StaticComponent || !Context.RootSkeletalComponent)
	{
		return FString();
	}

	if (const TSharedPtr<FJsonObject>* Override = Context.OverridesByComponent.Find(StaticComponent->GetName().ToLower()))
	{
		const FString ExplicitBindBone = GetStringField(*Override, TEXT("bind_bone"));
		if (!ExplicitBindBone.IsEmpty())
		{
			return ExplicitBindBone;
		}
	}

	const FName SocketName = StaticComponent->GetAttachSocketName();
	const FName SocketBoneName = SocketName.IsNone()
		? NAME_None
		: Context.RootSkeletalComponent->GetSocketBoneName(SocketName);
	if (!SocketBoneName.IsNone())
	{
		return SocketBoneName.ToString();
	}

	USkeletalMesh* RootMesh = Context.RootSkeletalComponent->GetSkeletalMeshAsset();
	return RootMesh && RootMesh->GetRefSkeleton().GetRawBoneNum() > 0
		? RootMesh->GetRefSkeleton().GetRawRefBoneInfo()[0].Name.ToString()
		: FString();
}

static void DescribeComponents(FActorAssemblyContext& Context)
{
	TInlineComponentArray<UMeshComponent*> MeshComponents(Context.Actor);
	MeshComponents.Sort([](const UMeshComponent& A, const UMeshComponent& B)
	{
		return A.GetName() < B.GetName();
	});

	for (UMeshComponent* MeshComponent : MeshComponents)
	{
		TSharedPtr<FJsonObject> Description = MakeShared<FJsonObject>();
		Description->SetStringField(TEXT("component"), MeshComponent->GetName());
		Description->SetStringField(TEXT("component_class"), MeshComponent->GetClass()->GetPathName());
		Description->SetBoolField(TEXT("visible"), MeshComponent->IsVisible());

		if (const USceneComponent* SceneComponent = Cast<USceneComponent>(MeshComponent))
		{
			Description->SetStringField(TEXT("attach_parent"), SceneComponent->GetAttachParent() ? SceneComponent->GetAttachParent()->GetName() : FString());
			Description->SetStringField(TEXT("attach_socket"), SceneComponent->GetAttachSocketName().ToString());
			if (Context.RootSkeletalComponent)
			{
				Description->SetObjectField(TEXT("relative_to_root"), TransformJson(
					SceneComponent->GetComponentTransform().GetRelativeTransform(Context.RootSkeletalComponent->GetComponentTransform())));
			}
		}

		if (const USkeletalMeshComponent* SkeletalComponent = Cast<USkeletalMeshComponent>(MeshComponent))
		{
			Description->SetStringField(TEXT("mesh_type"), TEXT("SkeletalMesh"));
			Description->SetStringField(TEXT("mesh"), AssetPath(SkeletalComponent->GetSkeletalMeshAsset()));
			Description->SetBoolField(TEXT("included"), Context.SkeletalComponents.Contains(const_cast<USkeletalMeshComponent*>(SkeletalComponent)));
			Description->SetBoolField(TEXT("root_skeletal_component"), SkeletalComponent == Context.RootSkeletalComponent);
		}
		else if (const UStaticMeshComponent* StaticComponent = Cast<UStaticMeshComponent>(MeshComponent))
		{
			Description->SetStringField(TEXT("mesh_type"), TEXT("StaticMesh"));
			Description->SetStringField(TEXT("mesh"), AssetPath(StaticComponent->GetStaticMesh()));
			Description->SetBoolField(TEXT("included"), Context.StaticComponents.Contains(const_cast<UStaticMeshComponent*>(StaticComponent)));
			Description->SetStringField(TEXT("bind_bone"), ResolveBindBone(StaticComponent, Context));
		}

		TArray<TSharedPtr<FJsonValue>> Materials;
		for (int32 MaterialIndex = 0; MaterialIndex < MeshComponent->GetNumMaterials(); ++MaterialIndex)
		{
			Materials.Add(MakeShared<FJsonValueString>(AssetPath(MeshComponent->GetMaterial(MaterialIndex))));
		}
		Description->SetArrayField(TEXT("materials"), Materials);
		Context.ComponentDescriptions.Add(MakeShared<FJsonValueObject>(Description));
	}
}

static bool SpawnAndInspect(const TSharedPtr<FJsonObject>& Spec, FActorAssemblyContext& Context)
{
	FString ActorPath = GetStringField(Spec, TEXT("actor_class"));
	if (ActorPath.IsEmpty())
	{
		ActorPath = GetStringField(Spec, TEXT("actor"));
	}
	if (ActorPath.IsEmpty())
	{
		AddIssue(Context.Issues, TEXT("error"), TEXT("missing_actor_class"), TEXT("actor_class is required."), TEXT("actor_class"));
		return false;
	}

	UClass* ActorClass = LoadActorClass(ActorPath);
	if (!ActorClass)
	{
		AddIssue(Context.Issues, TEXT("error"), TEXT("actor_class_not_found"),
			FString::Printf(TEXT("Actor class or Blueprint could not be loaded: %s"), *ActorPath),
			TEXT("actor_class"));
		return false;
	}
	if (!GEditor)
	{
		AddIssue(Context.Issues, TEXT("error"), TEXT("editor_unavailable"), TEXT("GEditor is unavailable; Actor assembly requires an editor world."));
		return false;
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		AddIssue(Context.Issues, TEXT("error"), TEXT("editor_world_unavailable"), TEXT("The editor world is unavailable; Actor assembly cannot spawn the source Actor."));
		return false;
	}

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.ObjectFlags |= RF_Transient;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParameters.bHideFromSceneOutliner = true;
	Context.Actor = World->SpawnActor<AActor>(ActorClass, FTransform::Identity, SpawnParameters);
	if (!Context.Actor)
	{
		AddIssue(Context.Issues, TEXT("error"), TEXT("actor_spawn_failed"), FString::Printf(TEXT("Failed to spawn Actor class: %s"), *ActorClass->GetPathName()));
		return false;
	}

	ApplyComponentOverrides(Spec, Context);
	Context.Actor->UpdateComponentTransforms();

	TInlineComponentArray<USkeletalMeshComponent*> AllSkeletalComponents(Context.Actor);
	AllSkeletalComponents.Sort([](const USkeletalMeshComponent& A, const USkeletalMeshComponent& B)
	{
		return A.GetName() < B.GetName();
	});
	for (USkeletalMeshComponent* Component : AllSkeletalComponents)
	{
		if (Component && Component->IsVisible() && Component->GetSkeletalMeshAsset())
		{
			Context.SkeletalComponents.Add(Component);
		}
	}

	FString RequestedRoot = GetStringField(Spec, TEXT("root_component"));
	if (!RequestedRoot.IsEmpty())
	{
		Context.RootSkeletalComponent = Cast<USkeletalMeshComponent>(FindComponentByName(Context.Actor, RequestedRoot));
		if (!Context.RootSkeletalComponent || !Context.RootSkeletalComponent->GetSkeletalMeshAsset())
		{
			AddIssue(Context.Issues, TEXT("error"), TEXT("invalid_root_component"),
				FString::Printf(TEXT("root_component is not a populated SkeletalMeshComponent: %s"), *RequestedRoot),
				TEXT("root_component"));
		}
	}
	if (!Context.RootSkeletalComponent)
	{
		Context.RootSkeletalComponent = Cast<USkeletalMeshComponent>(Context.Actor->GetRootComponent());
		if (Context.RootSkeletalComponent && !Context.RootSkeletalComponent->GetSkeletalMeshAsset())
		{
			Context.RootSkeletalComponent = nullptr;
		}
	}
	if (!Context.RootSkeletalComponent && !Context.SkeletalComponents.IsEmpty())
	{
		Context.RootSkeletalComponent = Context.SkeletalComponents[0];
	}
	if (!Context.RootSkeletalComponent)
	{
		AddIssue(Context.Issues, TEXT("error"), TEXT("missing_root_skeletal_component"), TEXT("The configured Actor has no visible SkeletalMeshComponent with a mesh."));
		DescribeComponents(Context);
		return false;
	}

	Context.SkeletalComponents.Remove(Context.RootSkeletalComponent);
	Context.SkeletalComponents.Insert(Context.RootSkeletalComponent, 0);
	USkeletalMesh* RootMesh = Context.RootSkeletalComponent->GetSkeletalMeshAsset();
	USkeleton* RootSkeleton = RootMesh ? RootMesh->GetSkeleton() : nullptr;
	if (!RootMesh || !RootSkeleton)
	{
		AddIssue(Context.Issues, TEXT("error"), TEXT("root_skeleton_missing"), TEXT("The root SkeletalMesh must reference a Skeleton."));
	}

	for (USkeletalMeshComponent* Component : Context.SkeletalComponents)
	{
		USkeletalMesh* Mesh = Component->GetSkeletalMeshAsset();
		if (Component != Context.RootSkeletalComponent)
		{
			const FTransform RelativeTransform = Component->GetComponentTransform().GetRelativeTransform(Context.RootSkeletalComponent->GetComponentTransform());
			if (!RelativeTransform.Equals(FTransform::Identity, 0.001f))
			{
				AddIssue(Context.Issues, TEXT("error"), TEXT("transformed_modular_skeletal_component"),
					FString::Printf(TEXT("Skeletal component %s has a non-identity transform relative to the root. Modular skeletal parts must share root model space."), *Component->GetName()),
					TEXT("component_overrides"));
			}
		}
		if (RootSkeleton && Mesh && Mesh->GetSkeleton() && !RootSkeleton->IsCompatibleForEditor(Mesh->GetSkeleton()))
		{
			AddIssue(Context.Issues, TEXT("error"), TEXT("incompatible_component_skeleton"),
				FString::Printf(TEXT("Skeletal component %s uses an incompatible Skeleton: %s"), *Component->GetName(), *Mesh->GetSkeleton()->GetPathName()),
				TEXT("component_overrides"));
		}
	}

	TInlineComponentArray<UStaticMeshComponent*> AllStaticComponents(Context.Actor);
	AllStaticComponents.Sort([](const UStaticMeshComponent& A, const UStaticMeshComponent& B)
	{
		return A.GetName() < B.GetName();
	});
	for (UStaticMeshComponent* Component : AllStaticComponents)
	{
		if (!Component || !Component->IsVisible() || !Component->GetStaticMesh())
		{
			continue;
		}
		Context.StaticComponents.Add(Component);
		const FString BindBone = ResolveBindBone(Component, Context);
		if (BindBone.IsEmpty() || !RootMesh || RootMesh->GetRefSkeleton().FindBoneIndex(FName(BindBone)) == INDEX_NONE)
		{
			AddIssue(Context.Issues, TEXT("error"), TEXT("static_component_bind_bone_not_found"),
				FString::Printf(TEXT("Static component %s cannot resolve a valid bind bone (resolved '%s')."), *Component->GetName(), *BindBone),
				TEXT("component_overrides"));
		}
		else if (Component->GetAttachSocketName().IsNone()
			&& !Context.OverridesByComponent.Contains(Component->GetName().ToLower()))
		{
			AddIssue(Context.Issues, TEXT("warning"), TEXT("static_component_bound_to_root"),
				FString::Printf(TEXT("Static component %s has no attach socket or explicit bind_bone and will bind to root bone %s."), *Component->GetName(), *BindBone));
		}
	}

	DescribeComponents(Context);
	return !HasErrors(Context.Issues);
}

static void AddRequiredVatSpecIssues(const TSharedPtr<FJsonObject>& Spec, TArray<TSharedPtr<FJsonValue>>& Issues)
{
	auto RequireString = [&](const FString& Name)
	{
		if (GetStringField(Spec, Name).IsEmpty())
		{
			AddIssue(Issues, TEXT("error"), TEXT("missing_required_field"), Name + TEXT(" is required for strict VAT creation."), Name);
		}
	};
	RequireString(TEXT("unit_name"));
	RequireString(TEXT("target_package_path"));
	RequireString(TEXT("parent_material"));
	RequireString(TEXT("source_renderer_class"));
	RequireString(TEXT("niagara_system"));

	double SampleRate = 0.0;
	if (!Spec.IsValid() || !Spec->TryGetNumberField(TEXT("vat_sample_rate"), SampleRate) || SampleRate <= 0.0)
	{
		AddIssue(Issues, TEXT("error"), TEXT("missing_required_field"), TEXT("vat_sample_rate must be greater than zero."), TEXT("vat_sample_rate"));
	}

	const TSharedPtr<FJsonObject>* Animations = nullptr;
	if (!Spec.IsValid() || !Spec->TryGetObjectField(TEXT("animations"), Animations) || !Animations || !Animations->IsValid() || (*Animations)->Values.IsEmpty())
	{
		AddIssue(Issues, TEXT("error"), TEXT("missing_required_field"), TEXT("animations must contain at least one animation category."), TEXT("animations"));
	}

	if (GetStringField(Spec, TEXT("target_unit")).IsEmpty())
	{
		RequireString(TEXT("template_unit"));
		RequireString(TEXT("target_unit_package_path"));
		double Subtype = 0.0;
		if (!Spec.IsValid() || !Spec->TryGetNumberField(TEXT("subtype"), Subtype))
		{
			AddIssue(Issues, TEXT("error"), TEXT("missing_required_field"), TEXT("subtype is required when target_unit is not supplied."), TEXT("subtype"));
		}
	}
}

static bool ResolveAssemblyOutput(
	const TSharedPtr<FJsonObject>& Spec,
	FString& OutPackageName,
	FString& OutAssetName,
	FString& OutObjectPath,
	FString& OutError)
{
	FString ExplicitPath = GetStringField(Spec, TEXT("assembled_skeletal_mesh_path"));
	if (!ExplicitPath.IsEmpty())
	{
		OutObjectPath = NormalizeObjectPath(ExplicitPath);
		OutPackageName = FPackageName::ObjectPathToPackageName(OutObjectPath);
		OutAssetName = FPackageName::ObjectPathToObjectName(OutObjectPath);
	}
	else
	{
		const FString TargetPackagePath = GetStringField(Spec, TEXT("target_package_path"));
		FString UnitName = ObjectTools::SanitizeObjectName(GetStringField(Spec, TEXT("unit_name")));
		OutAssetName = GetStringField(Spec, TEXT("assembled_skeletal_mesh_name"));
		if (OutAssetName.IsEmpty())
		{
			OutAssetName = TEXT("SKM_") + UnitName + TEXT("_Assembled");
		}
		OutAssetName = ObjectTools::SanitizeObjectName(OutAssetName);
		OutPackageName = TargetPackagePath / OutAssetName;
		OutObjectPath = OutPackageName + TEXT(".") + OutAssetName;
	}

	FText PackagePathReason;
	if (OutAssetName.IsEmpty() || !FPackageName::IsValidLongPackageName(OutPackageName, true, &PackagePathReason))
	{
		if (OutError.IsEmpty())
		{
			OutError = PackagePathReason.IsEmpty()
				? TEXT("assembled_skeletal_mesh path is invalid.")
				: PackagePathReason.ToString();
		}
		return false;
	}
	return true;
}

static TSharedPtr<FJsonObject> BuildInspectionResult(const TSharedPtr<FJsonObject>& Spec, FActorAssemblyContext& Context)
{
	const bool bCanAssemble = SpawnAndInspect(Spec, Context);
	TSharedPtr<FJsonObject> Result = MakeResult(true);
	Result->SetStringField(TEXT("inspection_type"), TEXT("actor_unit_assembly"));
	Result->SetStringField(TEXT("actor_class"), Context.Actor ? Context.Actor->GetClass()->GetPathName() : GetStringField(Spec, TEXT("actor_class")));
	Result->SetStringField(TEXT("root_component"), Context.RootSkeletalComponent ? Context.RootSkeletalComponent->GetName() : FString());
	Result->SetNumberField(TEXT("included_skeletal_components"), Context.SkeletalComponents.Num());
	Result->SetNumberField(TEXT("included_static_components"), Context.StaticComponents.Num());
	Result->SetBoolField(TEXT("can_assemble"), bCanAssemble);
	Result->SetArrayField(TEXT("components"), Context.ComponentDescriptions);
	Result->SetArrayField(TEXT("issues"), Context.Issues);
	return Result;
}

static TSharedPtr<FJsonObject> MakeResolvedVatSpec(const TSharedPtr<FJsonObject>& Spec, const FString& AssembledMeshPath)
{
	TSharedPtr<FJsonObject> Resolved = MakeShared<FJsonObject>();
	Resolved->Values = Spec->Values;
	for (const FString& Field : {
		TEXT("actor_class"), TEXT("actor"), TEXT("component_overrides"), TEXT("root_component"),
		TEXT("assembled_skeletal_mesh_name"), TEXT("assembled_skeletal_mesh_path") })
	{
		Resolved->RemoveField(Field);
	}
	Resolved->SetStringField(TEXT("skeletal_mesh"), AssembledMeshPath);
	return Resolved;
}

static USkeletalMesh* DuplicateSkeletalComponentMesh(USkeletalMeshComponent* Component, USkeleton* RootSkeleton)
{
	USkeletalMesh* Source = Component ? Component->GetSkeletalMeshAsset() : nullptr;
	if (!Source)
	{
		return nullptr;
	}

	USkeletalMesh* Copy = DuplicateObject<USkeletalMesh>(Source, GetTransientPackage());
	if (!Copy)
	{
		return nullptr;
	}
	Copy->ClearFlags(RF_Public | RF_Standalone);
	Copy->SetFlags(RF_Transient);
	if (RootSkeleton)
	{
		Copy->SetSkeleton(RootSkeleton);
	}

	TArray<FSkeletalMaterial> Materials = Copy->GetMaterials();
	const int32 MaterialCount = FMath::Max(Materials.Num(), Component->GetNumMaterials());
	Materials.SetNum(MaterialCount);
	for (int32 MaterialIndex = 0; MaterialIndex < MaterialCount; ++MaterialIndex)
	{
		if (UMaterialInterface* EffectiveMaterial = Component->GetMaterial(MaterialIndex))
		{
			Materials[MaterialIndex].MaterialInterface = EffectiveMaterial;
		}
	}
	Copy->SetMaterials(Materials);
	return Copy;
}

static USkeletalMesh* ConvertStaticComponentToSkeletal(
	UStaticMeshComponent* Component,
	USkeletalMeshComponent* RootComponent,
	const FName BindBone,
	FString& OutError)
{
	UStaticMesh* Source = Component ? Component->GetStaticMesh() : nullptr;
	USkeletalMesh* RootMesh = RootComponent ? RootComponent->GetSkeletalMeshAsset() : nullptr;
	if (!Source || !RootMesh)
	{
		OutError = TEXT("Static component or root SkeletalMesh is missing.");
		return nullptr;
	}

	UStaticMesh* TransformedStaticMesh = DuplicateObject<UStaticMesh>(Source, GetTransientPackage());
	if (!TransformedStaticMesh)
	{
		OutError = FString::Printf(TEXT("Failed to duplicate StaticMesh: %s"), *Source->GetPathName());
		return nullptr;
	}
	TransformedStaticMesh->ClearFlags(RF_Public | RF_Standalone);
	TransformedStaticMesh->SetFlags(RF_Transient);
	for (int32 MaterialIndex = 0; MaterialIndex < Component->GetNumMaterials(); ++MaterialIndex)
	{
		TransformedStaticMesh->SetMaterial(MaterialIndex, Component->GetMaterial(MaterialIndex));
	}

	const FTransform ComponentToRoot = Component->GetComponentTransform().GetRelativeTransform(RootComponent->GetComponentTransform());
	bool bTransformedAnyLod = false;
	for (int32 LodIndex = 0; LodIndex < TransformedStaticMesh->GetNumSourceModels(); ++LodIndex)
	{
		FMeshDescription* MeshDescription = TransformedStaticMesh->GetMeshDescription(LodIndex);
		if (!MeshDescription)
		{
			continue;
		}
		// ModifyMeshDescription returns transaction state, not editability. Transient
		// duplicates legitimately return false, so load first and do not branch on it.
		TransformedStaticMesh->ModifyMeshDescription(LodIndex, false);
		FStaticMeshOperations::ApplyTransform(*MeshDescription, ComponentToRoot, true);
		TransformedStaticMesh->CommitMeshDescription(LodIndex);
		bTransformedAnyLod = true;
	}
	if (!bTransformedAnyLod)
	{
		OutError = FString::Printf(TEXT("StaticMesh has no editable source MeshDescription: %s"), *Source->GetPathName());
		return nullptr;
	}
	TransformedStaticMesh->Build(true);

	USkeletalMesh* ConvertedMesh = NewObject<USkeletalMesh>(GetTransientPackage(), NAME_None, RF_Transient);
	if (!FStaticToSkeletalMeshConverter::InitializeSkeletalMeshFromStaticMesh(
		ConvertedMesh,
		TransformedStaticMesh,
		RootMesh->GetRefSkeleton(),
		BindBone))
	{
		OutError = FString::Printf(TEXT("Failed to convert StaticMesh %s and bind it to bone %s."), *Source->GetPathName(), *BindBone.ToString());
		return nullptr;
	}
	ConvertedMesh->SetSkeleton(RootMesh->GetSkeleton());
	return ConvertedMesh;
}

struct FAssemblyMaterialRegistry
{
	TMap<FName, UMaterialInterface*> MaterialsBySlot;

	FName ResolveSlot(FName RequestedSlot, UMaterialInterface* Material)
	{
		FString BaseName = RequestedSlot.IsNone()
			? (Material ? Material->GetName() : TEXT("Material"))
			: RequestedSlot.ToString();
		BaseName = ObjectTools::SanitizeObjectName(BaseName);
		if (BaseName.IsEmpty())
		{
			BaseName = TEXT("Material");
		}

		FName Candidate(*BaseName);
		if (UMaterialInterface** Existing = MaterialsBySlot.Find(Candidate))
		{
			if (*Existing == Material)
			{
				return Candidate;
			}

			const FString MaterialSuffix = ObjectTools::SanitizeObjectName(Material ? Material->GetName() : TEXT("None"));
			for (int32 SuffixIndex = 1; ; ++SuffixIndex)
			{
				const FString Suffix = SuffixIndex == 1
					? MaterialSuffix
					: FString::Printf(TEXT("%s_%d"), *MaterialSuffix, SuffixIndex);
				Candidate = FName(*(BaseName + TEXT("_") + Suffix));
				UMaterialInterface** SuffixedExisting = MaterialsBySlot.Find(Candidate);
				if (!SuffixedExisting)
				{
					break;
				}
				if (*SuffixedExisting == Material)
				{
					return Candidate;
				}
			}
		}

		MaterialsBySlot.Add(Candidate, Material);
		return Candidate;
	}
};

static int32 ResolveSourceMaterialIndex(const FName SlotName, const TArray<FSkeletalMaterial>& Materials, int32 PolygonGroupIndex)
{
	for (int32 MaterialIndex = 0; MaterialIndex < Materials.Num(); ++MaterialIndex)
	{
		const FSkeletalMaterial& Material = Materials[MaterialIndex];
		if (Material.MaterialSlotName == SlotName
#if WITH_EDITORONLY_DATA
			|| Material.ImportedMaterialSlotName == SlotName
#endif
		)
		{
			return MaterialIndex;
		}
	}

	if (Materials.IsValidIndex(PolygonGroupIndex))
	{
		return PolygonGroupIndex;
	}
	return Materials.Num() == 1 ? 0 : INDEX_NONE;
}

static void NormalizeMaterialSlots(
	FMeshDescription& MeshDescription,
	const USkeletalMesh* SourceMesh,
	FAssemblyMaterialRegistry& Registry)
{
	FStaticMeshAttributes Attributes(MeshDescription);
	Attributes.Register();
	TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
	const TArray<FSkeletalMaterial>& Materials = SourceMesh->GetMaterials();

	for (const FPolygonGroupID PolygonGroupID : MeshDescription.PolygonGroups().GetElementIDs())
	{
		const FName SourceSlot = SlotNames[PolygonGroupID];
		const int32 MaterialIndex = ResolveSourceMaterialIndex(SourceSlot, Materials, PolygonGroupID.GetValue());
		UMaterialInterface* Material = Materials.IsValidIndex(MaterialIndex)
			? Materials[MaterialIndex].MaterialInterface
			: nullptr;
		FName RequestedSlot = SourceSlot;
		if (RequestedSlot.IsNone() && Materials.IsValidIndex(MaterialIndex))
		{
			RequestedSlot = Materials[MaterialIndex].MaterialSlotName;
		}
		SlotNames[PolygonGroupID] = Registry.ResolveSlot(RequestedSlot, Material);
	}
}

static bool BuildBoneRemap(
	const USkeletalMesh* SourceMesh,
	const FReferenceSkeleton& TargetReferenceSkeleton,
	TArray<FBoneIndexType>& OutRemap,
	FString& OutError)
{
	const FReferenceSkeleton& SourceReferenceSkeleton = SourceMesh->GetRefSkeleton();
	OutRemap.Reset(SourceReferenceSkeleton.GetRawBoneNum());
	for (int32 SourceBoneIndex = 0; SourceBoneIndex < SourceReferenceSkeleton.GetRawBoneNum(); ++SourceBoneIndex)
	{
		const FName BoneName = SourceReferenceSkeleton.GetRawRefBoneInfo()[SourceBoneIndex].Name;
		const int32 TargetBoneIndex = TargetReferenceSkeleton.FindBoneIndex(BoneName);
		if (TargetBoneIndex == INDEX_NONE)
		{
			OutError = FString::Printf(
				TEXT("Source mesh %s contains bone %s, which is missing from the root reference skeleton."),
				*SourceMesh->GetPathName(),
				*BoneName.ToString());
			return false;
		}
		OutRemap.Add(static_cast<FBoneIndexType>(TargetBoneIndex));
	}
	return true;
}

static bool MergeSkeletalMeshDescriptions(
	const TArray<USkeletalMesh*>& MeshesToMerge,
	const FReferenceSkeleton& TargetReferenceSkeleton,
	FMeshDescription& OutMeshDescription,
	TArray<FSkeletalMaterial>& OutMaterials,
	FString& OutError)
{
	if (MeshesToMerge.IsEmpty())
	{
		OutError = TEXT("No skeletal mesh parts were supplied for editor-source assembly.");
		return false;
	}

	FAssemblyMaterialRegistry MaterialRegistry;
	bool bHasMergedMesh = false;
	for (USkeletalMesh* SourceMesh : MeshesToMerge)
	{
		if (!SourceMesh)
		{
			OutError = TEXT("A null skeletal mesh part was supplied for editor-source assembly.");
			return false;
		}

		FMeshDescription SourceDescription;
		if (!SourceMesh->CloneMeshDescription(0, SourceDescription))
		{
			OutError = FString::Printf(
				TEXT("Skeletal mesh part has no editable LOD0 MeshDescription: %s"),
				*SourceMesh->GetPathName());
			return false;
		}

		FSkeletalMeshAttributes SourceAttributes(SourceDescription);
		SourceAttributes.Register(true);
		FElementIDRemappings SourceRemappings;
		SourceDescription.Compact(SourceRemappings);
		NormalizeMaterialSlots(SourceDescription, SourceMesh, MaterialRegistry);

		if (!bHasMergedMesh)
		{
			OutMeshDescription = MoveTemp(SourceDescription);
			bHasMergedMesh = true;
			continue;
		}

		const int32 TargetVertexOffset = OutMeshDescription.Vertices().GetArraySize();
		FStaticMeshOperations::FAppendSettings GeometrySettings;
		FStaticMeshOperations::AppendMeshDescription(SourceDescription, OutMeshDescription, GeometrySettings);

		FSkeletalMeshOperations::FSkeletalMeshAppendSettings SkinSettings;
		SkinSettings.SourceVertexIDOffset = TargetVertexOffset;
		SkinSettings.bAppendVertexAttributes = true;
		if (!BuildBoneRemap(SourceMesh, TargetReferenceSkeleton, SkinSettings.SourceRemapBoneIndex, OutError))
		{
			return false;
		}
		FSkeletalMeshOperations::AppendSkinWeight(SourceDescription, OutMeshDescription, SkinSettings);
	}

	bool bInfluenceCountLimitHit = false;
	FSkeletalMeshOperations::ValidateAndFixInfluences(OutMeshDescription, bInfluenceCountLimitHit);
	if (bInfluenceCountLimitHit)
	{
		OutError = TEXT("Merged mesh exceeds the supported per-vertex bone influence count.");
		return false;
	}

	FElementIDRemappings MergedRemappings;
	OutMeshDescription.Compact(MergedRemappings);
	OutMeshDescription.BuildIndexers();

	FStaticMeshConstAttributes MergedAttributes(OutMeshDescription);
	TPolygonGroupAttributesConstRef<FName> MergedSlotNames = MergedAttributes.GetPolygonGroupMaterialSlotNames();
	TSet<FName> AddedSlots;
	for (const FPolygonGroupID PolygonGroupID : OutMeshDescription.PolygonGroups().GetElementIDs())
	{
		const FName SlotName = MergedSlotNames[PolygonGroupID];
		if (AddedSlots.Contains(SlotName))
		{
			continue;
		}
		AddedSlots.Add(SlotName);
		UMaterialInterface* const* Material = MaterialRegistry.MaterialsBySlot.Find(SlotName);
		OutMaterials.Add(FSkeletalMaterial(Material ? *Material : nullptr, SlotName, SlotName));
	}

	if (OutMeshDescription.Vertices().Num() == 0 || OutMeshDescription.Triangles().Num() == 0)
	{
		OutError = TEXT("Merged editor MeshDescription contains no renderable geometry.");
		return false;
	}
	return true;
}

static USkeletalMesh* CreatePersistentAssembledSkeletalMesh(
	UPackage* Package,
	const FString& AssetName,
	USkeletalMesh* RootMesh,
	const TArray<USkeletalMesh*>& MeshesToMerge,
	USkeletalMesh* ExistingMesh,
	FString& OutError)
{
	if (!Package || !RootMesh)
	{
		OutError = TEXT("Assembly package or root SkeletalMesh is missing.");
		return nullptr;
	}

	FMeshDescription MergedMeshDescription;
	TArray<FSkeletalMaterial> MergedMaterials;
	if (!MergeSkeletalMeshDescriptions(
		MeshesToMerge,
		RootMesh->GetRefSkeleton(),
		MergedMeshDescription,
		MergedMaterials,
		OutError))
	{
		return nullptr;
	}

	USkeletalMesh* AssembledMesh = ExistingMesh;
	if (AssembledMesh)
	{
		AssembledMesh->Modify();
		AssembledMesh->PreEditChange(nullptr);
		AssembledMesh->Clear();
		AssembledMesh->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
	}
	else
	{
		AssembledMesh = NewObject<USkeletalMesh>(
			Package,
			*AssetName,
			RF_Public | RF_Standalone | RF_Transactional);
	}
	if (!AssembledMesh)
	{
		OutError = FString::Printf(TEXT("Failed to allocate assembled SkeletalMesh: %s"), *AssetName);
		return nullptr;
	}

	if (!ExistingMesh)
	{
		AssembledMesh->PreEditChange(nullptr);
	}
	AssembledMesh->SetSkeleton(RootMesh->GetSkeleton());
	AssembledMesh->SetRefSkeleton(RootMesh->GetRefSkeleton());
	AssembledMesh->CalculateInvRefMatrices();
	AssembledMesh->SetMaterials(MergedMaterials);

	FSkeletalMeshModel* ImportedModel = AssembledMesh->GetImportedModel();
	if (!ImportedModel)
	{
		OutError = TEXT("Failed to allocate SkeletalMesh imported model storage.");
		return nullptr;
	}
	ImportedModel->LODModels.Add(new FSkeletalMeshLODModel);
	FSkeletalMeshLODInfo& LODInfo = AssembledMesh->AddLODInfo();
	if (const FSkeletalMeshLODInfo* RootLODInfo = RootMesh->GetLODInfo(0))
	{
		LODInfo.BuildSettings = RootLODInfo->BuildSettings;
		LODInfo.ScreenSize = RootLODInfo->ScreenSize;
		LODInfo.LODHysteresis = RootLODInfo->LODHysteresis;
	}
	LODInfo.ReductionSettings.NumOfTrianglesPercentage = 1.0f;
	LODInfo.ReductionSettings.MaxNumOfTriangles = MAX_uint32;
	LODInfo.ReductionSettings.MaxNumOfTrianglesPercentage = MAX_uint32;
	LODInfo.ReductionSettings.NumOfVertPercentage = 1.0f;
	LODInfo.ReductionSettings.MaxNumOfVerts = MAX_uint32;
	LODInfo.ReductionSettings.MaxNumOfVertsPercentage = MAX_uint32;
	LODInfo.ReductionSettings.MaxDeviationPercentage = 0.0f;
	LODInfo.bAllowCPUAccess = true;

	if (!AssembledMesh->CreateMeshDescription(0, MoveTemp(MergedMeshDescription)))
	{
		OutError = TEXT("Failed to create LOD0 source MeshDescription on the assembled SkeletalMesh.");
		return nullptr;
	}
	USkeletalMesh::FCommitMeshDescriptionParams CommitParams;
	CommitParams.bForceUpdate = true;
	if (!AssembledMesh->CommitMeshDescription(0, CommitParams))
	{
		OutError = TEXT("Failed to commit LOD0 source MeshDescription on the assembled SkeletalMesh.");
		return nullptr;
	}

	AssembledMesh->PostEditChange();
	FSkinnedAssetCompilingManager::Get().FinishCompilation({ AssembledMesh });
	FSkeletalMeshRenderData* RenderData = AssembledMesh->GetResourceForRendering();
	if (!AssembledMesh->HasMeshDescription(0) || !RenderData || RenderData->LODRenderData.IsEmpty())
	{
		OutError = TEXT("Assembled SkeletalMesh did not retain both editable source data and render LOD data after build.");
		return nullptr;
	}

	AssembledMesh->MarkPackageDirty();
	return AssembledMesh;
}

static TSharedPtr<FJsonObject> CreateAssembledSkeletalMesh(const TSharedPtr<FJsonObject>& Spec, bool bSaveAssets)
{
	FActorAssemblyContext Context;
	TSharedPtr<FJsonObject> Inspection = BuildInspectionResult(Spec, Context);
	bool bCanAssemble = false;
	Inspection->TryGetBoolField(TEXT("can_assemble"), bCanAssemble);
	if (!bCanAssemble)
	{
		TSharedPtr<FJsonObject> Result = MakeResult(false);
		Result->SetStringField(TEXT("error"), TEXT("Actor assembly inspection failed; no asset was written."));
		Result->SetObjectField(TEXT("inspection"), Inspection);
		return Result;
	}

	FString PackageName;
	FString AssetName;
	FString ObjectPath;
	FString PathError;
	if (!ResolveAssemblyOutput(Spec, PackageName, AssetName, ObjectPath, PathError))
	{
		TSharedPtr<FJsonObject> Result = MakeResult(false);
		Result->SetStringField(TEXT("error"), PathError);
		Result->SetObjectField(TEXT("inspection"), Inspection);
		return Result;
	}

	bool bOverwriteExisting = false;
	Spec->TryGetBoolField(TEXT("overwrite_existing"), bOverwriteExisting);
	USkeletalMesh* ExistingMesh = LoadAsset<USkeletalMesh>(ObjectPath);
	if (ExistingMesh && !bOverwriteExisting)
	{
		TSharedPtr<FJsonObject> Result = MakeResult(false);
		Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Assembled SkeletalMesh already exists; pass overwrite_existing=true to replace it: %s"), *ObjectPath));
		Result->SetObjectField(TEXT("inspection"), Inspection);
		return Result;
	}
	UPackage* Package = ExistingMesh ? ExistingMesh->GetOutermost() : CreatePackage(*PackageName);
	if (!Package)
	{
		TSharedPtr<FJsonObject> Result = MakeResult(false);
		Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Failed to create assembled SkeletalMesh package: %s"), *PackageName));
		Result->SetObjectField(TEXT("inspection"), Inspection);
		return Result;
	}

	USkeletalMesh* RootMesh = Context.RootSkeletalComponent->GetSkeletalMeshAsset();
	USkeleton* RootSkeleton = RootMesh->GetSkeleton();
	TArray<USkeletalMesh*> MeshesToMerge;
	for (USkeletalMeshComponent* Component : Context.SkeletalComponents)
	{
		USkeletalMesh* Copy = DuplicateSkeletalComponentMesh(Component, RootSkeleton);
		if (!Copy)
		{
			TSharedPtr<FJsonObject> Result = MakeResult(false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Failed to prepare skeletal component: %s"), *Component->GetName()));
			Result->SetObjectField(TEXT("inspection"), Inspection);
			return Result;
		}
		MeshesToMerge.Add(Copy);
	}

	for (UStaticMeshComponent* Component : Context.StaticComponents)
	{
		const FName BindBone(ResolveBindBone(Component, Context));
		FString ConvertError;
		USkeletalMesh* ConvertedMesh = ConvertStaticComponentToSkeletal(Component, Context.RootSkeletalComponent, BindBone, ConvertError);
		if (!ConvertedMesh)
		{
			TSharedPtr<FJsonObject> Result = MakeResult(false);
			Result->SetStringField(TEXT("error"), ConvertError);
			Result->SetStringField(TEXT("failed_component"), Component->GetName());
			Result->SetObjectField(TEXT("inspection"), Inspection);
			return Result;
		}
		MeshesToMerge.Add(ConvertedMesh);
	}

	FString AssemblyError;
	USkeletalMesh* AssembledMesh = CreatePersistentAssembledSkeletalMesh(
		Package,
		AssetName,
		RootMesh,
		MeshesToMerge,
		ExistingMesh,
		AssemblyError);
	if (!AssembledMesh)
	{
		TSharedPtr<FJsonObject> Result = MakeResult(false);
		Result->SetStringField(TEXT("error"), AssemblyError.IsEmpty() ? TEXT("Persistent SkeletalMesh assembly failed.") : AssemblyError);
		Result->SetObjectField(TEXT("inspection"), Inspection);
		return Result;
	}
	if (!ExistingMesh)
	{
		FAssetRegistryModule::AssetCreated(AssembledMesh);
	}

	if (bSaveAssets)
	{
		const FString PackageFilename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		if (!UPackage::SavePackage(Package, AssembledMesh, *PackageFilename, SaveArgs))
		{
			TSharedPtr<FJsonObject> Result = MakeResult(false);
			Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Failed to save assembled SkeletalMesh package: %s"), *PackageName));
			Result->SetObjectField(TEXT("inspection"), Inspection);
			return Result;
		}
	}

	TArray<TSharedPtr<FJsonValue>> MaterialSlots;
	for (const FSkeletalMaterial& Material : AssembledMesh->GetMaterials())
	{
		TSharedPtr<FJsonObject> Slot = MakeShared<FJsonObject>();
		Slot->SetStringField(TEXT("slot_name"), Material.MaterialSlotName.ToString());
		Slot->SetStringField(TEXT("material"), AssetPath(Material.MaterialInterface));
		MaterialSlots.Add(MakeShared<FJsonValueObject>(Slot));
	}

	TSharedPtr<FJsonObject> Result = MakeResult(true);
	Result->SetStringField(TEXT("assembled_skeletal_mesh"), AssembledMesh->GetPathName());
	Result->SetStringField(TEXT("assembly_method"), TEXT("persistent_editor_mesh_description"));
	Result->SetNumberField(TEXT("source_skeletal_components"), Context.SkeletalComponents.Num());
	Result->SetNumberField(TEXT("source_static_components"), Context.StaticComponents.Num());
	Result->SetNumberField(TEXT("source_model_count"), AssembledMesh->GetNumSourceModels());
	Result->SetBoolField(TEXT("has_editable_mesh_description"), AssembledMesh->HasMeshDescription(0));
	if (const FSkeletalMeshRenderData* RenderData = AssembledMesh->GetResourceForRendering())
	{
		Result->SetNumberField(TEXT("render_lod_count"), RenderData->LODRenderData.Num());
		if (!RenderData->LODRenderData.IsEmpty())
		{
			const FSkeletalMeshLODRenderData& LOD0 = RenderData->LODRenderData[0];
			int32 TriangleCount = 0;
			for (const FSkelMeshRenderSection& Section : LOD0.RenderSections)
			{
				TriangleCount += Section.NumTriangles;
			}
			Result->SetNumberField(TEXT("lod0_vertex_count"), LOD0.GetNumVertices());
			Result->SetNumberField(TEXT("lod0_triangle_count"), TriangleCount);
		}
	}
	Result->SetNumberField(TEXT("material_slot_count"), MaterialSlots.Num());
	Result->SetArrayField(TEXT("material_slots"), MaterialSlots);
	Result->SetBoolField(TEXT("saved"), bSaveAssets);
	Result->SetObjectField(TEXT("inspection"), Inspection);
	return Result;
}
} // namespace MassBattleActorUnitEditorMCP

FString UMassBattleUnitEditorMCPApi::MCP_EditorInspectActorAssembly(const FString& ActorPath, const FString& OptionsJson)
{
	TSharedPtr<FJsonObject> Spec = MassBattleActorUnitEditorMCP::ParseObject(OptionsJson);
	if (!Spec.IsValid())
	{
		return MassBattleActorUnitEditorMCP::MakeError(TEXT("OptionsJson is not valid JSON."));
	}
	if (!ActorPath.TrimStartAndEnd().IsEmpty())
	{
		Spec->SetStringField(TEXT("actor_class"), ActorPath);
	}

	MassBattleActorUnitEditorMCP::FActorAssemblyContext Context;
	return MassBattleActorUnitEditorMCP::ToJsonString(MassBattleActorUnitEditorMCP::BuildInspectionResult(Spec, Context));
}

FString UMassBattleUnitEditorMCPApi::MCP_EditorPlanCreateVatUnitFromActor(const FString& SpecJson)
{
	TSharedPtr<FJsonObject> Spec = MassBattleActorUnitEditorMCP::ParseObject(SpecJson);
	if (!Spec.IsValid())
	{
		return MassBattleActorUnitEditorMCP::MakeError(TEXT("SpecJson is not valid JSON."));
	}

	MassBattleActorUnitEditorMCP::FActorAssemblyContext Context;
	TSharedPtr<FJsonObject> Inspection = MassBattleActorUnitEditorMCP::BuildInspectionResult(Spec, Context);
	TArray<TSharedPtr<FJsonValue>> Issues = Context.Issues;
	MassBattleActorUnitEditorMCP::AddRequiredVatSpecIssues(Spec, Issues);

	FString PackageName;
	FString AssetName;
	FString ObjectPath;
	FString PathError;
	if (!MassBattleActorUnitEditorMCP::ResolveAssemblyOutput(Spec, PackageName, AssetName, ObjectPath, PathError))
	{
		MassBattleActorUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("invalid_assembly_output"), PathError, TEXT("assembled_skeletal_mesh_path"));
	}

	bool bOverwriteExisting = false;
	Spec->TryGetBoolField(TEXT("overwrite_existing"), bOverwriteExisting);
	const bool bAssemblyExists = MassBattleActorUnitEditorMCP::AssetExistsWithoutLoading(ObjectPath);
	if (bAssemblyExists && !bOverwriteExisting)
	{
		MassBattleActorUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("assembled_skeletal_mesh_exists"),
			FString::Printf(TEXT("Assembled SkeletalMesh exists and overwrite_existing is false: %s"), *ObjectPath),
			TEXT("overwrite_existing"));
	}
	else if (bAssemblyExists)
	{
		MassBattleActorUnitEditorMCP::AddIssue(Issues, TEXT("warning"), TEXT("assembled_skeletal_mesh_will_overwrite"),
			FString::Printf(TEXT("Existing assembled SkeletalMesh will be replaced: %s"), *ObjectPath),
			TEXT("overwrite_existing"));
	}

	TSharedPtr<FJsonObject> ResolvedVatSpec = MassBattleActorUnitEditorMCP::MakeResolvedVatSpec(Spec, ObjectPath);
	const bool bApplicable = !MassBattleActorUnitEditorMCP::HasErrors(Issues);
	TSharedPtr<FJsonObject> Result = MassBattleActorUnitEditorMCP::MakeResult(true);
	Result->SetStringField(TEXT("editor_workflow"), TEXT("Actor component assembly -> strict VAT unit authoring plan"));
	Result->SetBoolField(TEXT("applicable"), bApplicable);
	Result->SetStringField(TEXT("assembled_skeletal_mesh"), ObjectPath);
	Result->SetStringField(TEXT("assembly_status"), bAssemblyExists ? (bOverwriteExisting ? TEXT("would_overwrite") : TEXT("blocked_existing")) : TEXT("would_create"));
	Result->SetObjectField(TEXT("inspection"), Inspection);
	Result->SetObjectField(TEXT("resolved_vat_spec"), ResolvedVatSpec);
	Result->SetArrayField(TEXT("issues"), Issues);
	return MassBattleActorUnitEditorMCP::ToJsonString(Result);
}

FString UMassBattleUnitEditorMCPApi::MCP_EditorApplyCreateVatUnitFromActor(const FString& SpecJson, bool bSaveAssets)
{
	TSharedPtr<FJsonObject> Spec = MassBattleActorUnitEditorMCP::ParseObject(SpecJson);
	if (!Spec.IsValid())
	{
		return MassBattleActorUnitEditorMCP::MakeError(TEXT("SpecJson is not valid JSON."));
	}

	const FString PlanResultString = MCP_EditorPlanCreateVatUnitFromActor(SpecJson);
	TSharedPtr<FJsonObject> Plan = MassBattleActorUnitEditorMCP::ParseObject(PlanResultString);
	bool bApplicable = false;
	if (!Plan.IsValid() || !Plan->TryGetBoolField(TEXT("applicable"), bApplicable) || !bApplicable)
	{
		TSharedPtr<FJsonObject> Result = MassBattleActorUnitEditorMCP::MakeResult(false);
		Result->SetStringField(TEXT("error"), TEXT("Actor VAT unit plan is not applicable; no assets were written."));
		if (Plan.IsValid())
		{
			Result->SetObjectField(TEXT("plan"), Plan);
		}
		else
		{
			Result->SetStringField(TEXT("plan_raw"), PlanResultString);
		}
		return MassBattleActorUnitEditorMCP::ToJsonString(Result);
	}

	bool bDryRun = false;
	Spec->TryGetBoolField(TEXT("dry_run"), bDryRun);
	Spec->TryGetBoolField(TEXT("preview_only"), bDryRun);
	if (bDryRun)
	{
		return PlanResultString;
	}

	TSharedPtr<FJsonObject> AssemblyResult = MassBattleActorUnitEditorMCP::CreateAssembledSkeletalMesh(Spec, bSaveAssets);
	if (!AssemblyResult.IsValid() || !AssemblyResult->GetBoolField(TEXT("success")))
	{
		TSharedPtr<FJsonObject> Result = MassBattleActorUnitEditorMCP::MakeResult(false);
		Result->SetStringField(TEXT("error"), TEXT("Actor assembly failed; the VAT workflow was not started."));
		Result->SetObjectField(TEXT("plan"), Plan);
		if (AssemblyResult.IsValid())
		{
			Result->SetObjectField(TEXT("assembly"), AssemblyResult);
		}
		return MassBattleActorUnitEditorMCP::ToJsonString(Result);
	}

	FString AssembledMeshPath;
	AssemblyResult->TryGetStringField(TEXT("assembled_skeletal_mesh"), AssembledMeshPath);
	TSharedPtr<FJsonObject> ResolvedVatSpec = MassBattleActorUnitEditorMCP::MakeResolvedVatSpec(Spec, AssembledMeshPath);
	const FString VatResultString = MCP_EditorApplyCreateVatUnit(MassBattleActorUnitEditorMCP::ToJsonString(ResolvedVatSpec), bSaveAssets);
	TSharedPtr<FJsonObject> VatResult = MassBattleActorUnitEditorMCP::ParseObject(VatResultString);
	const bool bVatSuccess = VatResult.IsValid() && VatResult->GetBoolField(TEXT("success"));

	TSharedPtr<FJsonObject> Result = MassBattleActorUnitEditorMCP::MakeResult(bVatSuccess);
	Result->SetStringField(TEXT("editor_workflow"), TEXT("Actor component assembly -> strict VAT unit authoring apply"));
	Result->SetBoolField(TEXT("save_assets"), bSaveAssets);
	Result->SetObjectField(TEXT("plan"), Plan);
	Result->SetObjectField(TEXT("assembly"), AssemblyResult);
	Result->SetObjectField(TEXT("resolved_vat_spec"), ResolvedVatSpec);
	if (VatResult.IsValid())
	{
		Result->SetObjectField(TEXT("vat_result"), VatResult);
	}
	else
	{
		Result->SetStringField(TEXT("vat_result_raw"), VatResultString);
	}
	if (!bVatSuccess)
	{
		Result->SetStringField(TEXT("error"), TEXT("The assembled SkeletalMesh was created, but strict VAT unit authoring failed."));
	}
	return MassBattleActorUnitEditorMCP::ToJsonString(Result);
}
