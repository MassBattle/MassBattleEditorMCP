// Copyright (c) 2025-2026 Winyunq. All rights reserved.

#include "MassBattleUnitEditorMCPApi.h"

#include "AnimToTextureDataAsset.h"
#include "Animation/AnimSequence.h"
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
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialEditingLibrary.h"
#include "MeshDescription.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SkeletalMeshAttributes.h"
#include "SkeletalMeshOperations.h"
#include "SkinnedAssetCompiler.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "StaticMeshResources.h"
#include "StaticToSkeletalMeshConverter.h"
#include "StaticParameterSet.h"
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

static TSharedPtr<FJsonObject> MakeCanonicalSoldierAnimations()
{
	const FString AnimationRoot = TEXT("/Game/Unit/Action/Solider/");
	TSharedPtr<FJsonObject> Animations = MakeShared<FJsonObject>();
	auto SetCategory = [&](const TCHAR* Category, std::initializer_list<const TCHAR*> AssetNames)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const TCHAR* AssetName : AssetNames)
		{
			Values.Add(MakeShared<FJsonValueString>(AnimationRoot + AssetName));
		}
		Animations->SetArrayField(Category, Values);
	};

	SetCategory(TEXT("Idle"), { TEXT("IdleSolider") });
	SetCategory(TEXT("Move"), { TEXT("MoveSolider") });
	SetCategory(TEXT("Appear"), { TEXT("AppearSolider") });
	SetCategory(TEXT("Attack"), { TEXT("AttackSolider") });
	SetCategory(TEXT("Hit"), { TEXT("HitSolider") });
	SetCategory(TEXT("Death"), {
		TEXT("DeathSolider_A"),
		TEXT("DeathSolider_B"),
		TEXT("DeathSolider_C"),
		TEXT("DeathSolider_D"),
		TEXT("DeathSolider_E") });
	return Animations;
}

static int32 CountAnimationEntries(const TSharedPtr<FJsonObject>& Animations)
{
	int32 Count = 0;
	if (!Animations.IsValid())
	{
		return Count;
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Animations->Values)
	{
		if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Array)
		{
			Count += Pair.Value->AsArray().Num();
		}
	}
	return Count;
}

static TSharedPtr<FJsonObject> ResolveActorDefaultAnimations(
	const TSharedPtr<FJsonObject>& ResolvedSpec,
	const FActorAssemblyContext& Context,
	TArray<TSharedPtr<FJsonValue>>& Issues)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	const TSharedPtr<FJsonObject>* ExplicitAnimations = nullptr;
	if (ResolvedSpec.IsValid()
		&& ResolvedSpec->TryGetObjectField(TEXT("animations"), ExplicitAnimations)
		&& ExplicitAnimations
		&& ExplicitAnimations->IsValid()
		&& CountAnimationEntries(*ExplicitAnimations) > 0)
	{
		Result->SetStringField(TEXT("source"), TEXT("spec.animations"));
		Result->SetNumberField(TEXT("animation_count"), CountAnimationEntries(*ExplicitAnimations));
		Result->SetBoolField(TEXT("defaulted"), false);
		return Result;
	}

	bool bUseDefaults = true;
	if (ResolvedSpec.IsValid())
	{
		ResolvedSpec->TryGetBoolField(TEXT("use_default_soldier_animations"), bUseDefaults);
	}
	if (!bUseDefaults)
	{
		Result->SetStringField(TEXT("source"), TEXT("disabled"));
		Result->SetNumberField(TEXT("animation_count"), 0);
		Result->SetBoolField(TEXT("defaulted"), false);
		return Result;
	}

	TSharedPtr<FJsonObject> Animations = MakeCanonicalSoldierAnimations();
	ResolvedSpec->SetObjectField(TEXT("animations"), Animations);
	USkeleton* ExpectedSkeleton = nullptr;
	if (Context.RootSkeletalComponent && Context.RootSkeletalComponent->GetSkeletalMeshAsset())
	{
		ExpectedSkeleton = Context.RootSkeletalComponent->GetSkeletalMeshAsset()->GetSkeleton();
	}

	int32 ValidCount = 0;
	TArray<TSharedPtr<FJsonValue>> Assets;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Animations->Values)
	{
		for (const TSharedPtr<FJsonValue>& Value : Pair.Value->AsArray())
		{
			const FString Path = Value->AsString();
			UAnimSequence* Sequence = Cast<UAnimSequence>(FSoftObjectPath(NormalizeObjectPath(Path)).TryLoad());
			const bool bLoadable = Sequence != nullptr;
			const bool bSkeletonMatches = bLoadable && (!ExpectedSkeleton || Sequence->GetSkeleton() == ExpectedSkeleton);
			TSharedPtr<FJsonObject> Asset = MakeShared<FJsonObject>();
			Asset->SetStringField(TEXT("category"), Pair.Key);
			Asset->SetStringField(TEXT("path"), Path);
			Asset->SetBoolField(TEXT("loadable"), bLoadable);
			Asset->SetBoolField(TEXT("skeleton_matches"), bSkeletonMatches);
			Assets.Add(MakeShared<FJsonValueObject>(Asset));
			if (!bLoadable)
			{
				AddIssue(Issues, TEXT("error"), TEXT("default_soldier_animation_missing"),
					FString::Printf(TEXT("Canonical soldier animation is not loadable: %s"), *Path), TEXT("animations"));
			}
			else if (!bSkeletonMatches)
			{
				AddIssue(Issues, TEXT("error"), TEXT("default_soldier_animation_skeleton_mismatch"),
					FString::Printf(TEXT("Canonical soldier animation does not use the Actor root skeleton: %s"), *Path), TEXT("animations"));
			}
			else
			{
				++ValidCount;
			}
		}
	}

	AddIssue(Issues, TEXT("warning"), TEXT("defaulted_canonical_soldier_animations"),
		TEXT("animations was omitted; Actor VAT authoring selected all 10 canonical /Game/Unit/Action/Solider animations (Idle, Move, Appear, Attack, Hit, Death A-E)."),
		TEXT("animations"));
	Result->SetStringField(TEXT("source"), TEXT("canonical_soldier_profile"));
	Result->SetStringField(TEXT("root"), TEXT("/Game/Unit/Action/Solider"));
	Result->SetNumberField(TEXT("animation_count"), CountAnimationEntries(Animations));
	Result->SetNumberField(TEXT("valid_animation_count"), ValidCount);
	Result->SetBoolField(TEXT("defaulted"), true);
	Result->SetArrayField(TEXT("assets"), Assets);
	return Result;
}

static TSharedPtr<FJsonObject> ResolveActorVatSampleRate(
	const TSharedPtr<FJsonObject>& ResolvedSpec,
	TArray<TSharedPtr<FJsonValue>>& Issues)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	double SampleRate = 0.0;
	if (ResolvedSpec.IsValid()
		&& ResolvedSpec->TryGetNumberField(TEXT("vat_sample_rate"), SampleRate)
		&& SampleRate > 0.0)
	{
		Result->SetStringField(TEXT("source"), TEXT("spec.vat_sample_rate"));
		Result->SetNumberField(TEXT("sample_rate"), SampleRate);
		Result->SetBoolField(TEXT("defaulted"), false);
		return Result;
	}

	constexpr double DefaultVatSampleRate = 24.0;
	ResolvedSpec->SetNumberField(TEXT("vat_sample_rate"), DefaultVatSampleRate);
	AddIssue(Issues, TEXT("warning"), TEXT("defaulted_vat_sample_rate"),
		TEXT("vat_sample_rate was omitted; Actor VAT authoring selected the MassBattle MCP style default of 24 Hz."),
		TEXT("vat_sample_rate"));
	Result->SetStringField(TEXT("source"), TEXT("massbattle_mcp_style_default"));
	Result->SetNumberField(TEXT("sample_rate"), DefaultVatSampleRate);
	Result->SetBoolField(TEXT("defaulted"), true);
	return Result;
}

static TSharedPtr<FJsonObject> ResolveActorVatLodDefaults(
	const TSharedPtr<FJsonObject>& ResolvedSpec,
	TArray<TSharedPtr<FJsonValue>>& Issues)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	const TArray<TSharedPtr<FJsonValue>>* ExplicitLods = nullptr;
	if (ResolvedSpec.IsValid()
		&& ResolvedSpec->TryGetArrayField(TEXT("lod_settings"), ExplicitLods)
		&& ExplicitLods
		&& !ExplicitLods->IsEmpty())
	{
		Result->SetStringField(TEXT("source"), TEXT("spec.lod_settings"));
		Result->SetNumberField(TEXT("lod_count"), ExplicitLods->Num());
		Result->SetBoolField(TEXT("defaulted"), false);
		return Result;
	}

	TSharedPtr<FJsonObject> LOD0 = MakeShared<FJsonObject>();
	LOD0->SetNumberField(TEXT("LODIndex"), 0);
	LOD0->SetNumberField(TEXT("ScreenSize"), 0.0);
	LOD0->SetNumberField(TEXT("AnimBlendLevel"), 2);
	LOD0->SetStringField(TEXT("Mode"), TEXT("BoneMode"));
	TArray<TSharedPtr<FJsonValue>> Lods;
	Lods.Add(MakeShared<FJsonValueObject>(LOD0));
	ResolvedSpec->SetArrayField(TEXT("lod_settings"), Lods);
	AddIssue(Issues, TEXT("warning"), TEXT("defaulted_actor_vat_bone_mode"),
		TEXT("lod_settings was omitted; Actor VAT authoring selected LOD0 BoneMode with animation blend level 2."),
		TEXT("lod_settings"));
	Result->SetStringField(TEXT("source"), TEXT("actor_animation_safe_default"));
	Result->SetNumberField(TEXT("lod_count"), 1);
	Result->SetStringField(TEXT("mode"), TEXT("BoneMode"));
	Result->SetNumberField(TEXT("animation_blend_level"), 2);
	Result->SetBoolField(TEXT("defaulted"), true);
	return Result;
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
		TEXT("assembled_skeletal_mesh_name"), TEXT("assembled_skeletal_mesh_path"),
		TEXT("use_default_soldier_animations") })
	{
		Resolved->RemoveField(Field);
	}
	Resolved->SetStringField(TEXT("skeletal_mesh"), AssembledMeshPath);
	return Resolved;
}

static TSharedPtr<FJsonObject> BuildSkinWeightAudit(USkeletalMesh* SkeletalMesh)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("valid"), false);
	if (!SkeletalMesh)
	{
		Result->SetStringField(TEXT("error"), TEXT("SkeletalMesh is null."));
		return Result;
	}

	FSkinnedAssetCompilingManager::Get().FinishCompilation({ SkeletalMesh });
	const FSkeletalMeshRenderData* RenderData = SkeletalMesh->GetResourceForRendering();
	if (!RenderData || RenderData->LODRenderData.IsEmpty())
	{
		Result->SetStringField(TEXT("error"), TEXT("SkeletalMesh has no render LOD data."));
		return Result;
	}

	const FSkeletalMeshLODRenderData& LOD = RenderData->LODRenderData[0];
	const FSkinWeightVertexBuffer* SkinWeights = LOD.GetSkinWeightVertexBuffer();
	if (!SkinWeights || SkinWeights->GetNumVertices() == 0)
	{
		Result->SetStringField(TEXT("error"), TEXT("LOD0 has no readable skin-weight vertices."));
		return Result;
	}

	int32 WeightedVertexCount = 0;
	int32 NonRootWeightedVertexCount = 0;
	int32 MultiInfluenceVertexCount = 0;
	int32 InvalidSectionVertexCount = 0;
	int32 InvalidBoneMapInfluenceCount = 0;
	TSet<int32> UsedMeshBones;
	const uint32 VertexCount = FMath::Min<uint32>(LOD.GetNumVertices(), SkinWeights->GetNumVertices());
	for (uint32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex)
	{
		int32 SectionIndex = INDEX_NONE;
		int32 SectionVertexIndex = INDEX_NONE;
		LOD.GetSectionFromVertexIndex(static_cast<int32>(VertexIndex), SectionIndex, SectionVertexIndex);
		if (!LOD.RenderSections.IsValidIndex(SectionIndex))
		{
			++InvalidSectionVertexCount;
			continue;
		}

		const FSkelMeshRenderSection& Section = LOD.RenderSections[SectionIndex];
		uint32 VertexWeightOffset = 0;
		uint32 VertexInfluenceCount = 0;
		SkinWeights->GetVertexInfluenceOffsetCount(VertexIndex, VertexWeightOffset, VertexInfluenceCount);
		int32 PositiveInfluenceCount = 0;
		bool bHasNonRootInfluence = false;
		for (uint32 InfluenceIndex = 0; InfluenceIndex < VertexInfluenceCount; ++InfluenceIndex)
		{
			if (SkinWeights->GetBoneWeight(VertexIndex, InfluenceIndex) == 0)
			{
				continue;
			}
			++PositiveInfluenceCount;
			const uint32 SectionBoneIndex = SkinWeights->GetBoneIndex(VertexIndex, InfluenceIndex);
			if (!Section.BoneMap.IsValidIndex(static_cast<int32>(SectionBoneIndex)))
			{
				++InvalidBoneMapInfluenceCount;
				continue;
			}
			const int32 MeshBoneIndex = Section.BoneMap[SectionBoneIndex];
			UsedMeshBones.Add(MeshBoneIndex);
			bHasNonRootInfluence |= MeshBoneIndex > 0;
		}
		if (PositiveInfluenceCount > 0)
		{
			++WeightedVertexCount;
		}
		if (PositiveInfluenceCount > 1)
		{
			++MultiInfluenceVertexCount;
		}
		if (bHasNonRootInfluence)
		{
			++NonRootWeightedVertexCount;
		}
	}

	TArray<int32> SortedBoneIndices = UsedMeshBones.Array();
	SortedBoneIndices.Sort();
	TArray<TSharedPtr<FJsonValue>> UsedBones;
	const FReferenceSkeleton& ReferenceSkeleton = SkeletalMesh->GetRefSkeleton();
	for (const int32 BoneIndex : SortedBoneIndices)
	{
		TSharedPtr<FJsonObject> Bone = MakeShared<FJsonObject>();
		Bone->SetNumberField(TEXT("index"), BoneIndex);
		Bone->SetStringField(TEXT("name"), ReferenceSkeleton.IsValidIndex(BoneIndex)
			? ReferenceSkeleton.GetBoneName(BoneIndex).ToString()
			: FString());
		UsedBones.Add(MakeShared<FJsonValueObject>(Bone));
	}

	Result->SetBoolField(TEXT("valid"), InvalidSectionVertexCount == 0 && InvalidBoneMapInfluenceCount == 0);
	Result->SetStringField(TEXT("skeletal_mesh"), SkeletalMesh->GetPathName());
	Result->SetNumberField(TEXT("vertex_count"), VertexCount);
	Result->SetNumberField(TEXT("weighted_vertex_count"), WeightedVertexCount);
	Result->SetNumberField(TEXT("non_root_weighted_vertex_count"), NonRootWeightedVertexCount);
	Result->SetNumberField(TEXT("multi_influence_vertex_count"), MultiInfluenceVertexCount);
	Result->SetNumberField(TEXT("non_root_weighted_vertex_ratio"), VertexCount > 0
		? static_cast<double>(NonRootWeightedVertexCount) / static_cast<double>(VertexCount)
		: 0.0);
	Result->SetNumberField(TEXT("used_mesh_bone_count"), UsedMeshBones.Num());
	Result->SetNumberField(TEXT("invalid_section_vertex_count"), InvalidSectionVertexCount);
	Result->SetNumberField(TEXT("invalid_bone_map_influence_count"), InvalidBoneMapInfluenceCount);
	Result->SetBoolField(TEXT("has_deforming_skin_weights"), NonRootWeightedVertexCount > 0 && UsedMeshBones.Num() > 1);
	Result->SetArrayField(TEXT("used_mesh_bones"), UsedBones);
	return Result;
}

static int32 CountChangedFramePairs(
	const TArray64<uint8>& MipData,
	int64 FrameByteCount,
	int32 StartFrame,
	int32 EndFrame)
{
	if (FrameByteCount <= 0 || StartFrame < 0 || EndFrame <= StartFrame)
	{
		return 0;
	}
	int32 ChangedPairs = 0;
	for (int32 Frame = StartFrame + 1; Frame <= EndFrame; ++Frame)
	{
		const int64 PreviousOffset = static_cast<int64>(Frame - 1) * FrameByteCount;
		const int64 CurrentOffset = static_cast<int64>(Frame) * FrameByteCount;
		if (CurrentOffset + FrameByteCount > MipData.Num())
		{
			break;
		}
		if (FMemory::Memcmp(MipData.GetData() + PreviousOffset, MipData.GetData() + CurrentOffset, FrameByteCount) != 0)
		{
			++ChangedPairs;
		}
	}
	return ChangedPairs;
}

static TSharedPtr<FJsonObject> BuildVertexMotionAudit(UAnimToTextureDataAsset* DataAsset)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("valid"), false);
	if (!DataAsset)
	{
		Result->SetStringField(TEXT("error"), TEXT("AnimToTextureDataAsset is null."));
		return Result;
	}

	const bool bBoneMode = DataAsset->Mode == EAnimToTextureMode::Bone;
	UTexture2D* PositionTexture = bBoneMode
		? DataAsset->BonePositionTexture.LoadSynchronous()
		: DataAsset->VertexPositionTexture.LoadSynchronous();
	if (!PositionTexture || !PositionTexture->Source.IsValid())
	{
		Result->SetStringField(TEXT("error"), bBoneMode
			? TEXT("Bone position texture has no readable source mip.")
			: TEXT("Vertex position texture has no readable source mip."));
		return Result;
	}

	TArray64<uint8> MipData;
	if (!PositionTexture->Source.GetMipData(MipData, 0))
	{
		Result->SetStringField(TEXT("error"), bBoneMode
			? TEXT("Failed to read bone position texture mip 0.")
			: TEXT("Failed to read vertex position texture mip 0."));
		return Result;
	}

	const int64 Width = PositionTexture->Source.GetSizeX();
	const int64 Height = PositionTexture->Source.GetSizeY();
	const int64 BytesPerPixel = PositionTexture->Source.GetBytesPerPixel();
	const int32 RowsPerFrame = bBoneMode ? DataAsset->BoneRowsPerFrame : DataAsset->VertexRowsPerFrame;
	const int32 FrameOffset = bBoneMode ? 1 : 0;
	const int32 DeclaredTextureFrameCount = DataAsset->NumFrames + FrameOffset;
	const int64 FrameByteCount = Width * FMath::Max(1, RowsPerFrame) * BytesPerPixel;
	const int32 AvailableFrameCount = FrameByteCount > 0
		? FMath::Min<int32>(DeclaredTextureFrameCount, static_cast<int32>(MipData.Num() / FrameByteCount))
		: 0;
	const int32 ChangedFramePairs = CountChangedFramePairs(
		MipData,
		FrameByteCount,
		FrameOffset,
		FMath::Min(FrameOffset + DataAsset->NumFrames - 1, AvailableFrameCount - 1));

	TArray<TSharedPtr<FJsonValue>> AnimationAudits;
	for (int32 Index = 0; Index < DataAsset->Animations.Num(); ++Index)
	{
		const FAnimToTextureAnimInfo& AnimInfo = DataAsset->Animations[Index];
		TSharedPtr<FJsonObject> Animation = MakeShared<FJsonObject>();
		Animation->SetNumberField(TEXT("index"), Index);
		if (DataAsset->AnimSequences.IsValidIndex(Index) && DataAsset->AnimSequences[Index].AnimSequence)
		{
			Animation->SetStringField(TEXT("animation"), DataAsset->AnimSequences[Index].AnimSequence->GetPathName());
		}
		Animation->SetNumberField(TEXT("start_frame"), AnimInfo.StartFrame);
		Animation->SetNumberField(TEXT("end_frame"), AnimInfo.EndFrame);
		const int32 AnimationChangedPairs = CountChangedFramePairs(
			MipData,
			FrameByteCount,
			FrameOffset + AnimInfo.StartFrame,
			FMath::Min(FrameOffset + AnimInfo.EndFrame, AvailableFrameCount - 1));
		Animation->SetNumberField(TEXT("changed_frame_pairs"), AnimationChangedPairs);
		Animation->SetBoolField(TEXT("has_motion"), AnimationChangedPairs > 0);
		Animation->SetBoolField(bBoneMode ? TEXT("has_bone_motion") : TEXT("has_vertex_motion"), AnimationChangedPairs > 0);
		AnimationAudits.Add(MakeShared<FJsonValueObject>(Animation));
	}

	Result->SetBoolField(TEXT("valid"), AvailableFrameCount == DeclaredTextureFrameCount && DataAsset->NumFrames > 0);
	Result->SetStringField(TEXT("motion_domain"), bBoneMode ? TEXT("bone") : TEXT("vertex"));
	Result->SetStringField(TEXT("position_texture"), PositionTexture->GetPathName());
	Result->SetNumberField(TEXT("width"), Width);
	Result->SetNumberField(TEXT("height"), Height);
	Result->SetNumberField(TEXT("bytes_per_pixel"), BytesPerPixel);
	Result->SetNumberField(TEXT("source_mip_bytes"), MipData.Num());
	Result->SetNumberField(TEXT("rows_per_frame"), RowsPerFrame);
	Result->SetNumberField(TEXT("frame_offset"), FrameOffset);
	Result->SetNumberField(TEXT("declared_frame_count"), DataAsset->NumFrames);
	Result->SetNumberField(TEXT("declared_texture_frame_count"), DeclaredTextureFrameCount);
	Result->SetNumberField(TEXT("available_frame_count"), AvailableFrameCount);
	Result->SetNumberField(TEXT("changed_frame_pairs"), ChangedFramePairs);
	Result->SetBoolField(TEXT("has_motion"), ChangedFramePairs > 0);
	Result->SetBoolField(bBoneMode ? TEXT("has_bone_motion") : TEXT("has_vertex_motion"), ChangedFramePairs > 0);
	Result->SetArrayField(TEXT("animations"), AnimationAudits);
	return Result;
}

static TSharedPtr<FJsonObject> BuildVatUvAudit(UStaticMesh* StaticMesh, int32 LODIndex, int32 UVChannel)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("valid"), false);
	if (!StaticMesh || !StaticMesh->GetRenderData() || !StaticMesh->GetRenderData()->LODResources.IsValidIndex(LODIndex))
	{
		Result->SetStringField(TEXT("error"), TEXT("StaticMesh VAT LOD is not readable."));
		return Result;
	}

	const FStaticMeshLODResources& LOD = StaticMesh->GetRenderData()->LODResources[LODIndex];
	const FStaticMeshVertexBuffer& Vertices = LOD.VertexBuffers.StaticMeshVertexBuffer;
	const int32 TexCoordCount = Vertices.GetNumTexCoords();
	const uint32 VertexCount = Vertices.GetNumVertices();
	Result->SetStringField(TEXT("static_mesh"), StaticMesh->GetPathName());
	Result->SetNumberField(TEXT("lod_index"), LODIndex);
	Result->SetNumberField(TEXT("uv_channel"), UVChannel);
	Result->SetNumberField(TEXT("tex_coord_count"), TexCoordCount);
	Result->SetNumberField(TEXT("vertex_count"), VertexCount);
	if (UVChannel < 0 || UVChannel >= TexCoordCount || VertexCount == 0)
	{
		Result->SetStringField(TEXT("error"), TEXT("Configured VAT UV channel is missing from the StaticMesh LOD."));
		return Result;
	}

	FVector2f MinUV(FLT_MAX, FLT_MAX);
	FVector2f MaxUV(-FLT_MAX, -FLT_MAX);
	int32 NonFiniteCount = 0;
	for (uint32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex)
	{
		const FVector2f UV = Vertices.GetVertexUV(VertexIndex, UVChannel);
		if (!FMath::IsFinite(UV.X) || !FMath::IsFinite(UV.Y))
		{
			++NonFiniteCount;
			continue;
		}
		MinUV.X = FMath::Min(MinUV.X, UV.X);
		MinUV.Y = FMath::Min(MinUV.Y, UV.Y);
		MaxUV.X = FMath::Max(MaxUV.X, UV.X);
		MaxUV.Y = FMath::Max(MaxUV.Y, UV.Y);
	}
	const bool bHasVariation = MaxUV.X > MinUV.X || MaxUV.Y > MinUV.Y;
	Result->SetBoolField(TEXT("valid"), NonFiniteCount == 0 && bHasVariation);
	Result->SetNumberField(TEXT("non_finite_uv_count"), NonFiniteCount);
	Result->SetNumberField(TEXT("min_u"), MinUV.X);
	Result->SetNumberField(TEXT("min_v"), MinUV.Y);
	Result->SetNumberField(TEXT("max_u"), MaxUV.X);
	Result->SetNumberField(TEXT("max_v"), MaxUV.Y);
	Result->SetBoolField(TEXT("has_uv_variation"), bHasVariation);
	return Result;
}

static TSharedPtr<FJsonObject> BuildMaterialParameterAssociationAudit(
	UMaterialInstanceConstant* MaterialInstance,
	EMaterialParameterAssociation Association)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Switches = MakeShared<FJsonObject>();
	for (const FName Name : {
		FName(TEXT("BoneMode")), FName(TEXT("UseVAT")), FName(TEXT("LegacyAnimData")),
		FName(TEXT("AutoPlay")), FName(TEXT("UseUV0")), FName(TEXT("UseUV1")),
		FName(TEXT("UseUV2")), FName(TEXT("UseUV3")), FName(TEXT("UseTwoInfluences")),
		FName(TEXT("UseFourInfluences")), FName(TEXT("UseBlend2")), FName(TEXT("UseBlend3")) })
	{
		Switches->SetBoolField(Name.ToString(), UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(
			MaterialInstance, Name, Association));
	}

	TSharedPtr<FJsonObject> Textures = MakeShared<FJsonObject>();
	for (const FName Name : {
		FName(TEXT("PositionTexture")), FName(TEXT("NormalTexture")),
		FName(TEXT("BonePositionTexture")), FName(TEXT("BoneRotationTexture")),
		FName(TEXT("BoneWeightsTexture")), FName(TEXT("AnimDataTex")) })
	{
		UTexture* Texture = UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(MaterialInstance, Name, Association);
		Textures->SetStringField(Name.ToString(), Texture ? Texture->GetPathName() : FString());
	}

	TSharedPtr<FJsonObject> Scalars = MakeShared<FJsonObject>();
	for (const FName Name : {
		FName(TEXT("RowsPerFrame")), FName(TEXT("BoneWeightsRowsPerFrame")),
		FName(TEXT("NumBones")), FName(TEXT("NumFrames")), FName(TEXT("SampleRate")) })
	{
		Scalars->SetNumberField(Name.ToString(), UMaterialEditingLibrary::GetMaterialInstanceScalarParameterValue(
			MaterialInstance, Name, Association));
	}
	Result->SetObjectField(TEXT("switches"), Switches);
	Result->SetObjectField(TEXT("textures"), Textures);
	Result->SetObjectField(TEXT("scalars"), Scalars);
	return Result;
}

static bool MaterialHasTextureValue(UMaterialInstanceConstant* Instance, const FName Name, UTexture* Expected)
{
	if (!Instance || !Expected)
	{
		return false;
	}
	return UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(
		Instance, Name, EMaterialParameterAssociation::GlobalParameter) == Expected
		|| UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(
			Instance, Name, EMaterialParameterAssociation::LayerParameter) == Expected;
}

static bool MaterialHasScalarValue(UMaterialInstanceConstant* Instance, const FName Name, float Expected)
{
	if (!Instance)
	{
		return false;
	}
	const float GlobalValue = UMaterialEditingLibrary::GetMaterialInstanceScalarParameterValue(
		Instance, Name, EMaterialParameterAssociation::GlobalParameter);
	const float LayerValue = UMaterialEditingLibrary::GetMaterialInstanceScalarParameterValue(
		Instance, Name, EMaterialParameterAssociation::LayerParameter);
	return FMath::IsNearlyEqual(GlobalValue, Expected) || FMath::IsNearlyEqual(LayerValue, Expected);
}

static TSharedPtr<FJsonObject> BuildVatMaterialAudit(UStaticMesh* StaticMesh, UAnimToTextureDataAsset* DataAsset)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Materials;
	int32 MaterialInstanceCount = 0;
	int32 ValidMaterialCount = 0;
	const bool bBoneMode = DataAsset && DataAsset->Mode == EAnimToTextureMode::Bone;
	TMap<FName, bool> ExpectedSwitches;
	ExpectedSwitches.Add(TEXT("UseVAT"), true);
	ExpectedSwitches.Add(TEXT("BoneMode"), bBoneMode);
	ExpectedSwitches.Add(TEXT("LegacyAnimData"), false);
	ExpectedSwitches.Add(TEXT("UseUV0"), DataAsset && DataAsset->UVChannel == 0);
	ExpectedSwitches.Add(TEXT("UseUV1"), DataAsset && DataAsset->UVChannel == 1);
	ExpectedSwitches.Add(TEXT("UseUV2"), DataAsset && DataAsset->UVChannel == 2);
	ExpectedSwitches.Add(TEXT("UseUV3"), DataAsset && DataAsset->UVChannel == 3);
	ExpectedSwitches.Add(TEXT("UseTwoInfluences"), bBoneMode && DataAsset->NumBoneInfluences == EAnimToTextureNumBoneInfluences::Two);
	ExpectedSwitches.Add(TEXT("UseFourInfluences"), bBoneMode && DataAsset->NumBoneInfluences == EAnimToTextureNumBoneInfluences::Four);
	if (StaticMesh)
	{
		for (int32 SlotIndex = 0; SlotIndex < StaticMesh->GetStaticMaterials().Num(); ++SlotIndex)
		{
			const FStaticMaterial& Slot = StaticMesh->GetStaticMaterials()[SlotIndex];
			TSharedPtr<FJsonObject> Material = MakeShared<FJsonObject>();
			Material->SetNumberField(TEXT("slot_index"), SlotIndex);
			Material->SetStringField(TEXT("slot_name"), Slot.MaterialSlotName.ToString());
			Material->SetStringField(TEXT("material"), AssetPath(Slot.MaterialInterface));
			UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Slot.MaterialInterface);
			Material->SetBoolField(TEXT("is_material_instance_constant"), Instance != nullptr);
			if (Instance)
			{
				++MaterialInstanceCount;
				Material->SetStringField(TEXT("parent"), AssetPath(Instance->Parent));
				Material->SetObjectField(TEXT("global"), BuildMaterialParameterAssociationAudit(
					Instance, EMaterialParameterAssociation::GlobalParameter));
				Material->SetObjectField(TEXT("layer"), BuildMaterialParameterAssociationAudit(
					Instance, EMaterialParameterAssociation::LayerParameter));
				TArray<TSharedPtr<FJsonValue>> ActualStaticSwitches;
				TSet<FName> FoundCriticalSwitches;
				int32 StaticSwitchMismatchCount = 0;
				const FStaticParameterSet StaticParameters = Instance->GetStaticParameters();
				for (const FStaticSwitchParameter& StaticSwitch : StaticParameters.StaticSwitchParameters)
				{
					const FMaterialParameterInfo& ParameterInfo = StaticSwitch.ParameterInfo;
					const bool bValue = StaticSwitch.Value;
					TSharedPtr<FJsonObject> Switch = MakeShared<FJsonObject>();
					Switch->SetStringField(TEXT("name"), ParameterInfo.Name.ToString());
					Switch->SetNumberField(TEXT("association"), static_cast<int32>(ParameterInfo.Association));
					Switch->SetNumberField(TEXT("index"), ParameterInfo.Index);
					Switch->SetBoolField(TEXT("resolved"), true);
					Switch->SetBoolField(TEXT("overridden"), StaticSwitch.bOverride);
					Switch->SetBoolField(TEXT("value"), bValue);
					if (const bool* Expected = ExpectedSwitches.Find(ParameterInfo.Name))
					{
						FoundCriticalSwitches.Add(ParameterInfo.Name);
						Switch->SetBoolField(TEXT("expected"), *Expected);
						const bool bMatches = bValue == *Expected;
						Switch->SetBoolField(TEXT("matches_expected"), bMatches);
						if (!bMatches)
						{
							++StaticSwitchMismatchCount;
						}
					}
					ActualStaticSwitches.Add(MakeShared<FJsonValueObject>(Switch));
				}
				Material->SetArrayField(TEXT("actual_static_switches"), ActualStaticSwitches);

				const bool bCriticalSwitchesValid = FoundCriticalSwitches.Num() == ExpectedSwitches.Num()
					&& StaticSwitchMismatchCount == 0;
				bool bTexturesValid = DataAsset != nullptr;
				if (DataAsset)
				{
					bTexturesValid = MaterialHasTextureValue(Instance, TEXT("AnimDataTex"),
						UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(
							Instance, TEXT("AnimDataTex"), EMaterialParameterAssociation::LayerParameter));
					if (bBoneMode)
					{
						bTexturesValid = bTexturesValid
							&& MaterialHasTextureValue(Instance, TEXT("BonePositionTexture"), DataAsset->BonePositionTexture.LoadSynchronous())
							&& MaterialHasTextureValue(Instance, TEXT("BoneRotationTexture"), DataAsset->BoneRotationTexture.LoadSynchronous())
							&& MaterialHasTextureValue(Instance, TEXT("BoneWeightsTexture"), DataAsset->BoneWeightTexture.LoadSynchronous());
					}
					else
					{
						bTexturesValid = bTexturesValid
							&& MaterialHasTextureValue(Instance, TEXT("PositionTexture"), DataAsset->VertexPositionTexture.LoadSynchronous())
							&& MaterialHasTextureValue(Instance, TEXT("NormalTexture"), DataAsset->VertexNormalTexture.LoadSynchronous());
					}
				}
				const bool bScalarsValid = DataAsset
					&& MaterialHasScalarValue(Instance, TEXT("NumFrames"), DataAsset->NumFrames)
					&& MaterialHasScalarValue(Instance, TEXT("SampleRate"), DataAsset->SampleRate);
				const bool bMaterialValid = bCriticalSwitchesValid && bTexturesValid && bScalarsValid;
				Material->SetNumberField(TEXT("critical_static_switch_count"), FoundCriticalSwitches.Num());
				Material->SetNumberField(TEXT("critical_static_switch_mismatch_count"), StaticSwitchMismatchCount);
				Material->SetBoolField(TEXT("critical_static_switches_valid"), bCriticalSwitchesValid);
				Material->SetBoolField(TEXT("vat_textures_valid"), bTexturesValid);
				Material->SetBoolField(TEXT("vat_scalars_valid"), bScalarsValid);
				Material->SetBoolField(TEXT("valid"), bMaterialValid);
				if (bMaterialValid)
				{
					++ValidMaterialCount;
				}
			}
			Materials.Add(MakeShared<FJsonValueObject>(Material));
		}
	}
	Result->SetBoolField(TEXT("valid"), StaticMesh != nullptr
		&& MaterialInstanceCount == Materials.Num()
		&& MaterialInstanceCount > 0
		&& ValidMaterialCount == MaterialInstanceCount);
	Result->SetNumberField(TEXT("material_count"), Materials.Num());
	Result->SetNumberField(TEXT("material_instance_count"), MaterialInstanceCount);
	Result->SetNumberField(TEXT("valid_material_count"), ValidMaterialCount);
	Result->SetArrayField(TEXT("materials"), Materials);
	return Result;
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
	Result->SetObjectField(TEXT("skin_weights"), BuildSkinWeightAudit(AssembledMesh));
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

FString UMassBattleUnitEditorMCPApi::MCP_EditorInspectVatAnimation(const FString& SpecJson)
{
	TSharedPtr<FJsonObject> Spec = MassBattleActorUnitEditorMCP::ParseObject(SpecJson);
	if (!Spec.IsValid())
	{
		return MassBattleActorUnitEditorMCP::MakeError(TEXT("SpecJson is not valid JSON."));
	}

	FString VatDataPath = MassBattleActorUnitEditorMCP::GetStringField(Spec, TEXT("vat_data_asset"));
	if (VatDataPath.IsEmpty())
	{
		VatDataPath = MassBattleActorUnitEditorMCP::GetStringField(Spec, TEXT("anim_to_texture_data_asset"));
	}
	UAnimToTextureDataAsset* VatData = MassBattleActorUnitEditorMCP::LoadAsset<UAnimToTextureDataAsset>(VatDataPath);
	if (!VatData)
	{
		return MassBattleActorUnitEditorMCP::MakeError(FString::Printf(
			TEXT("vat_data_asset is not a loadable AnimToTextureDataAsset: %s"), *VatDataPath));
	}

	FString SkeletalMeshPath = MassBattleActorUnitEditorMCP::GetStringField(Spec, TEXT("skeletal_mesh"));
	USkeletalMesh* SkeletalMesh = SkeletalMeshPath.IsEmpty()
		? VatData->SkeletalMesh.LoadSynchronous()
		: MassBattleActorUnitEditorMCP::LoadAsset<USkeletalMesh>(SkeletalMeshPath);
	FString StaticMeshPath = MassBattleActorUnitEditorMCP::GetStringField(Spec, TEXT("static_mesh"));
	UStaticMesh* StaticMesh = StaticMeshPath.IsEmpty()
		? VatData->StaticMesh.LoadSynchronous()
		: MassBattleActorUnitEditorMCP::LoadAsset<UStaticMesh>(StaticMeshPath);

	TSharedPtr<FJsonObject> SkinWeights = MassBattleActorUnitEditorMCP::BuildSkinWeightAudit(SkeletalMesh);
	TSharedPtr<FJsonObject> VertexMotion = MassBattleActorUnitEditorMCP::BuildVertexMotionAudit(VatData);
	TSharedPtr<FJsonObject> VatUv = MassBattleActorUnitEditorMCP::BuildVatUvAudit(StaticMesh, VatData->StaticLODIndex, VatData->UVChannel);
	TSharedPtr<FJsonObject> VatMaterials = MassBattleActorUnitEditorMCP::BuildVatMaterialAudit(StaticMesh, VatData);

	TSet<FString> CanonicalPaths;
	TSharedPtr<FJsonObject> CanonicalAnimations = MassBattleActorUnitEditorMCP::MakeCanonicalSoldierAnimations();
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : CanonicalAnimations->Values)
	{
		for (const TSharedPtr<FJsonValue>& Value : Pair.Value->AsArray())
		{
			CanonicalPaths.Add(MassBattleActorUnitEditorMCP::NormalizeObjectPath(Value->AsString()));
		}
	}
	TSet<FString> BakedPaths;
	TArray<TSharedPtr<FJsonValue>> BakedAnimations;
	for (int32 Index = 0; Index < VatData->AnimSequences.Num(); ++Index)
	{
		const FAnimToTextureAnimSequenceInfo& Info = VatData->AnimSequences[Index];
		if (!Info.bEnabled || !Info.AnimSequence)
		{
			continue;
		}
		const FString Path = Info.AnimSequence->GetPathName();
		BakedPaths.Add(MassBattleActorUnitEditorMCP::NormalizeObjectPath(Path));
		TSharedPtr<FJsonObject> Animation = MakeShared<FJsonObject>();
		Animation->SetNumberField(TEXT("index"), Index);
		Animation->SetStringField(TEXT("path"), Path);
		BakedAnimations.Add(MakeShared<FJsonValueObject>(Animation));
	}
	bool bCanonicalProfile = BakedPaths.Num() == CanonicalPaths.Num();
	if (bCanonicalProfile)
	{
		for (const FString& CanonicalPath : CanonicalPaths)
		{
			if (!BakedPaths.Contains(CanonicalPath))
			{
				bCanonicalProfile = false;
				break;
			}
		}
	}

	bool bSkinValid = false;
	bool bHasDeformingWeights = false;
	SkinWeights->TryGetBoolField(TEXT("valid"), bSkinValid);
	SkinWeights->TryGetBoolField(TEXT("has_deforming_skin_weights"), bHasDeformingWeights);
	bool bMotionValid = false;
	bool bHasMotion = false;
	VertexMotion->TryGetBoolField(TEXT("valid"), bMotionValid);
	VertexMotion->TryGetBoolField(TEXT("has_motion"), bHasMotion);
	bool bUvValid = false;
	VatUv->TryGetBoolField(TEXT("valid"), bUvValid);
	bool bMaterialsValid = false;
	VatMaterials->TryGetBoolField(TEXT("valid"), bMaterialsValid);

	TArray<TSharedPtr<FJsonValue>> Issues;
	if (!bSkinValid)
	{
		MassBattleActorUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("invalid_skin_weight_data"),
			TEXT("The assembled SkeletalMesh has invalid or unreadable LOD0 skin-weight data."), TEXT("skeletal_mesh"));
	}
	else if (!bHasDeformingWeights)
	{
		MassBattleActorUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("no_deforming_skin_weights"),
			TEXT("The assembled SkeletalMesh has no non-root deforming skin weights; baked skeletal animation would remain rigid."), TEXT("skeletal_mesh"));
	}
	if (!bMotionValid || !bHasMotion)
	{
		MassBattleActorUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("vat_position_frames_are_static"),
			TEXT("The VAT position texture does not contain readable frame-to-frame animation motion."), TEXT("vat_data_asset"));
	}
	if (!bUvValid)
	{
		MassBattleActorUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("invalid_vat_uv_channel"),
			TEXT("The generated StaticMesh does not contain a usable configured VAT lookup UV channel."), TEXT("static_mesh"));
	}
	if (!bMaterialsValid)
	{
		MassBattleActorUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("invalid_vat_material_parameters"),
			TEXT("One or more generated material instances do not have the required VAT mode, UV, influence, texture, or frame parameters."), TEXT("static_mesh"));
	}
	if (!bCanonicalProfile)
	{
		MassBattleActorUnitEditorMCP::AddIssue(Issues, TEXT("warning"), TEXT("noncanonical_soldier_animation_profile"),
			TEXT("The VAT data asset does not contain exactly the 10 canonical /Game/Unit/Action/Solider animations."), TEXT("vat_data_asset"));
	}

	const bool bHealthy = !MassBattleActorUnitEditorMCP::HasErrors(Issues);
	TSharedPtr<FJsonObject> Result = MassBattleActorUnitEditorMCP::MakeResult(true);
	Result->SetStringField(TEXT("inspection_type"), TEXT("vat_animation_readiness"));
	Result->SetBoolField(TEXT("healthy"), bHealthy);
	Result->SetStringField(TEXT("vat_data_asset"), VatData->GetPathName());
	Result->SetStringField(TEXT("mode"), VatData->Mode == EAnimToTextureMode::Vertex ? TEXT("VertexMode") : TEXT("BoneMode"));
	Result->SetStringField(TEXT("skeletal_mesh"), SkeletalMesh ? SkeletalMesh->GetPathName() : FString());
	Result->SetStringField(TEXT("static_mesh"), StaticMesh ? StaticMesh->GetPathName() : FString());
	Result->SetNumberField(TEXT("sample_rate"), VatData->SampleRate);
	Result->SetNumberField(TEXT("animation_count"), BakedPaths.Num());
	Result->SetNumberField(TEXT("frame_count"), VatData->NumFrames);
	Result->SetBoolField(TEXT("uses_canonical_soldier_profile"), bCanonicalProfile);
	Result->SetArrayField(TEXT("baked_animations"), BakedAnimations);
	Result->SetObjectField(TEXT("skin_weights"), SkinWeights);
	Result->SetObjectField(TEXT("vat_motion"), VertexMotion);
	Result->SetObjectField(TEXT("vertex_motion"), VertexMotion);
	Result->SetObjectField(TEXT("vat_uv"), VatUv);
	Result->SetObjectField(TEXT("vat_materials"), VatMaterials);
	Result->SetArrayField(TEXT("issues"), Issues);
	return MassBattleActorUnitEditorMCP::ToJsonString(Result);
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
	TSharedPtr<FJsonObject> ResolvedActorSpec = MakeShared<FJsonObject>();
	ResolvedActorSpec->Values = Spec->Values;
	TSharedPtr<FJsonObject> AnimationResolution = MassBattleActorUnitEditorMCP::ResolveActorDefaultAnimations(ResolvedActorSpec, Context, Issues);
	TSharedPtr<FJsonObject> SampleRateResolution = MassBattleActorUnitEditorMCP::ResolveActorVatSampleRate(ResolvedActorSpec, Issues);
	TSharedPtr<FJsonObject> LodResolution = MassBattleActorUnitEditorMCP::ResolveActorVatLodDefaults(ResolvedActorSpec, Issues);
	MassBattleActorUnitEditorMCP::AddRequiredVatSpecIssues(ResolvedActorSpec, Issues);

	FString PackageName;
	FString AssetName;
	FString ObjectPath;
	FString PathError;
	if (!MassBattleActorUnitEditorMCP::ResolveAssemblyOutput(ResolvedActorSpec, PackageName, AssetName, ObjectPath, PathError))
	{
		MassBattleActorUnitEditorMCP::AddIssue(Issues, TEXT("error"), TEXT("invalid_assembly_output"), PathError, TEXT("assembled_skeletal_mesh_path"));
	}

	bool bOverwriteExisting = false;
	ResolvedActorSpec->TryGetBoolField(TEXT("overwrite_existing"), bOverwriteExisting);
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

	TSharedPtr<FJsonObject> ResolvedVatSpec = MassBattleActorUnitEditorMCP::MakeResolvedVatSpec(ResolvedActorSpec, ObjectPath);
	const bool bApplicable = !MassBattleActorUnitEditorMCP::HasErrors(Issues);
	TSharedPtr<FJsonObject> Result = MassBattleActorUnitEditorMCP::MakeResult(true);
	Result->SetStringField(TEXT("editor_workflow"), TEXT("Actor component assembly -> strict VAT unit authoring plan"));
	Result->SetBoolField(TEXT("applicable"), bApplicable);
	Result->SetStringField(TEXT("assembled_skeletal_mesh"), ObjectPath);
	Result->SetStringField(TEXT("assembly_status"), bAssemblyExists ? (bOverwriteExisting ? TEXT("would_overwrite") : TEXT("blocked_existing")) : TEXT("would_create"));
	Result->SetObjectField(TEXT("inspection"), Inspection);
	Result->SetObjectField(TEXT("animation_resolution"), AnimationResolution);
	Result->SetObjectField(TEXT("vat_sample_rate_resolution"), SampleRateResolution);
	Result->SetObjectField(TEXT("vat_lod_resolution"), LodResolution);
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
	TSharedPtr<FJsonObject> ResolvedVatSpec = MakeShared<FJsonObject>();
	const TSharedPtr<FJsonObject>* PlannedVatSpec = nullptr;
	if (Plan->TryGetObjectField(TEXT("resolved_vat_spec"), PlannedVatSpec) && PlannedVatSpec && PlannedVatSpec->IsValid())
	{
		ResolvedVatSpec->Values = (*PlannedVatSpec)->Values;
		ResolvedVatSpec->SetStringField(TEXT("skeletal_mesh"), AssembledMeshPath);
	}
	else
	{
		ResolvedVatSpec = MassBattleActorUnitEditorMCP::MakeResolvedVatSpec(Spec, AssembledMeshPath);
	}
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
