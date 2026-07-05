// Copyright (c) 2025-2026 Winyunq. All rights reserved.
#include "MassBattleEditorMCPApi.h"
#include "MassBattleFuncLibEd.h"
#include "MassBattleEditorStructs.h"
#include "Fragments/Animation.h"
#include "Fragments/Render.h"
#include "Fragments/LOD.h"
#include "AnimToTextureDataAsset.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/SkeletalMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "MeshUtilities.h"
#include "MeshDescriptionBuilder.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "NiagaraSystem.h"
#include "Materials/MaterialInterface.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Renderers/MassBattleAgentRenderer.h"
#include "StaticMeshAttributes.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Texture2D.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY(LogMassBattleEditorMCPApi);

// ==================== 内部辅助函数 ====================

FString UMassBattleEditorMCPApi::MakeErrorJson(const FString& ErrorMessage)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("success"), false);
	Root->SetStringField(TEXT("error"), ErrorMessage);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	return OutputString;
}

FString UMassBattleEditorMCPApi::MakeSuccessJson()
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("success"), true);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	return OutputString;
}

namespace
{
static FString ObjectPathOrEmpty(const UObject* Object)
{
	return Object ? Object->GetPathName() : FString();
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

static void SetMaterialTextureParameterForAllAssociations(UMaterialInstanceConstant* MaterialInstance, const FName TextureParameterName, UTexture2D* Texture)
{
	if (!MaterialInstance || !Texture)
	{
		return;
	}

	UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MaterialInstance, TextureParameterName, Texture);
	UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MaterialInstance, TextureParameterName, Texture, EMaterialParameterAssociation::LayerParameter);
}

static void SetMaterialStaticSwitchForAllAssociations(UMaterialInstanceConstant* MaterialInstance, const FName SwitchParameterName, const bool bValue)
{
	if (!MaterialInstance || SwitchParameterName.IsNone())
	{
		return;
	}

	UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, SwitchParameterName, bValue);
	UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, SwitchParameterName, bValue, EMaterialParameterAssociation::LayerParameter);
}

static const FOriginalTextures* ResolveOriginalTextureSetForStaticMaterial(const FStaticMaterial& StaticMaterial, const int32 SlotIndex, const TArray<FOriginalTextures>& OriginalTexturesArray)
{
	if (OriginalTexturesArray.IsEmpty())
	{
		return nullptr;
	}

	const FString SlotName = StaticMaterial.MaterialSlotName.ToString();
	FString BaseNamePart;
	FString IndexPart;
	if (SlotName.Split(TEXT("__"), &BaseNamePart, &IndexPart, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
	{
		const int32 TextureIndex = FCString::Atoi(*IndexPart);
		if (OriginalTexturesArray.IsValidIndex(TextureIndex))
		{
			return &OriginalTexturesArray[TextureIndex];
		}
	}

	for (const FOriginalTextures& TextureSet : OriginalTexturesArray)
	{
		if (!TextureSet.SlotName.IsEmpty() && SlotName.StartsWith(TextureSet.SlotName, ESearchCase::IgnoreCase))
		{
			return &TextureSet;
		}
	}

	if (OriginalTexturesArray.Num() == 1)
	{
		return &OriginalTexturesArray[0];
	}

	if (OriginalTexturesArray.IsValidIndex(SlotIndex))
	{
		return &OriginalTexturesArray[SlotIndex];
	}

	return nullptr;
}

static int32 ApplyOriginalTexturesToGeneratedVatMaterials(UStaticMesh* StaticMesh, const TArray<FOriginalTextures>& OriginalTexturesArray)
{
	if (!StaticMesh || OriginalTexturesArray.IsEmpty())
	{
		return 0;
	}

	int32 UpdatedMaterialCount = 0;
	const TArray<FStaticMaterial>& StaticMaterials = StaticMesh->GetStaticMaterials();
	for (int32 SlotIndex = 0; SlotIndex < StaticMaterials.Num(); ++SlotIndex)
	{
		const FStaticMaterial& StaticMaterial = StaticMaterials[SlotIndex];
		UMaterialInstanceConstant* MaterialInstance = Cast<UMaterialInstanceConstant>(StaticMaterial.MaterialInterface);
		const FOriginalTextures* TextureSet = ResolveOriginalTextureSetForStaticMaterial(StaticMaterial, SlotIndex, OriginalTexturesArray);
		if (!MaterialInstance || !TextureSet)
		{
			continue;
		}

		bool bUpdated = false;
		auto ApplyTextureParam = [&](const FName TextureParameterName, const FName SwitchParameterName, UTexture2D* Texture)
		{
			if (!Texture)
			{
				return;
			}

			SetMaterialTextureParameterForAllAssociations(MaterialInstance, TextureParameterName, Texture);
			SetMaterialStaticSwitchForAllAssociations(MaterialInstance, SwitchParameterName, true);
			bUpdated = true;
		};

		ApplyTextureParam(TEXT("AOTex"), TEXT("UseAOTex"), TextureSet->AO.Get());
		ApplyTextureParam(TEXT("ARMTex"), TEXT("UseARMTex"), TextureSet->ARM.Get());
		ApplyTextureParam(TEXT("BaseColorTex"), TEXT("UseColorTex"), TextureSet->BaseColor.Get());
		ApplyTextureParam(TEXT("SpecularTex"), TEXT("UseSpecularTex"), TextureSet->Specular.Get());
		ApplyTextureParam(TEXT("EmissiveTex"), TEXT("UseEmissiveTex"), TextureSet->Emissive.Get());
		ApplyTextureParam(TEXT("NormalTex"), TEXT("UseNormalTex"), TextureSet->Normal.Get());
		ApplyTextureParam(TEXT("OpacityTex"), TEXT("UseOpacityTex"), TextureSet->Opacity.Get());
		ApplyTextureParam(TEXT("RoughnessTex"), TEXT("UseRoughnessTex"), TextureSet->Roughness.Get());
		ApplyTextureParam(TEXT("MetallicTex"), TEXT("UseMetallicTex"), TextureSet->Metallic.Get());

		if (bUpdated)
		{
			UMaterialEditingLibrary::UpdateMaterialInstance(MaterialInstance);
			MaterialInstance->PostEditChange();
			MaterialInstance->MarkPackageDirty();
			++UpdatedMaterialCount;
		}
	}

	if (UpdatedMaterialCount > 0)
	{
		StaticMesh->PostEditChange();
		StaticMesh->MarkPackageDirty();
	}

	return UpdatedMaterialCount;
}

static void ApplyMassBattleStaticMeshSlotLayout(UStaticMesh* OutStaticMesh, USkeletalMesh* SkeletalMesh, int32 LightmapIndex, bool bGenerateLightmapUVs)
{
	if (!OutStaticMesh || !SkeletalMesh)
	{
		return;
	}

	const int32 NumLODs = OutStaticMesh->GetNumSourceModels();
	TArray<FStaticMaterial> FinalMaterials;
	FMeshSectionInfoMap FinalSectionInfoMap;
	int32 MaterialOffset = 0;

	const FMeshSectionInfoMap OriginalSectionInfoMap = OutStaticMesh->GetSectionInfoMap();
	const TArray<FStaticMaterial> OriginalMaterials = OutStaticMesh->GetStaticMaterials();
	const TArray<FSkeletalMaterial>& SkeletalMaterials = SkeletalMesh->GetMaterials();

	for (int32 LODIdx = 0; LODIdx < NumLODs; ++LODIdx)
	{
		FStaticMeshSourceModel& SourceModel = OutStaticMesh->GetSourceModel(LODIdx);
		SourceModel.BuildSettings.bGenerateLightmapUVs = bGenerateLightmapUVs;
		if (bGenerateLightmapUVs)
		{
			SourceModel.BuildSettings.DstLightmapIndex = LightmapIndex;
		}

		const int32 NumSections = OriginalSectionInfoMap.GetSectionNumber(LODIdx);
		for (int32 SectionIdx = 0; SectionIdx < NumSections; ++SectionIdx)
		{
			const FMeshSectionInfo OriginalSectionInfo = OriginalSectionInfoMap.Get(LODIdx, SectionIdx);
			const int32 SkelMatIndex = OriginalSectionInfo.MaterialIndex;
			FStaticMaterial NewMaterial;
			if (OriginalMaterials.IsValidIndex(OriginalSectionInfo.MaterialIndex))
			{
				NewMaterial = OriginalMaterials[OriginalSectionInfo.MaterialIndex];
			}

			FString SkeletalSlotName = TEXT("Unnamed");
			if (SkeletalMaterials.IsValidIndex(SkelMatIndex))
			{
				SkeletalSlotName = SkeletalMaterials[SkelMatIndex].MaterialSlotName.ToString();
			}
			NewMaterial.MaterialSlotName = FName(*FString::Printf(TEXT("%s_LOD%d_%d__%d"), *SkeletalSlotName, LODIdx, SectionIdx, SkelMatIndex));
			FinalMaterials.Add(NewMaterial);
		}

		for (int32 SectionIdx = 0; SectionIdx < NumSections; ++SectionIdx)
		{
			FMeshSectionInfo NewSectionInfo = OriginalSectionInfoMap.Get(LODIdx, SectionIdx);
			NewSectionInfo.MaterialIndex = MaterialOffset + SectionIdx;
			FinalSectionInfoMap.Set(LODIdx, SectionIdx, NewSectionInfo);
		}
		MaterialOffset += NumSections;
	}

	if (!FinalMaterials.IsEmpty())
	{
		OutStaticMesh->Modify();
		OutStaticMesh->SetStaticMaterials(FinalMaterials);
		OutStaticMesh->GetSectionInfoMap() = FinalSectionInfoMap;
		OutStaticMesh->Build();
		OutStaticMesh->PostEditChange();
		OutStaticMesh->MarkPackageDirty();
	}
}

static UStaticMesh* ConvertSkeletalMeshToStaticMeshMcpFallback(USkeletalMesh* SkeletalMesh, const FString& OutputPackagePath, const FString& OutputAssetName, int32 LightmapIndex, bool bGenerateLightmapUVs, FString& OutError)
{
	if (!SkeletalMesh)
	{
		OutError = TEXT("SkeletalMesh is null.");
		return nullptr;
	}

	const FString SafePackagePath = UMassBattleEditorMCPApi::MCP_SanitizeForPath(OutputPackagePath);
	const FString SafeAssetName = UMassBattleEditorMCPApi::MCP_SanitizeForPath(OutputAssetName);
	if (SafePackagePath.IsEmpty() || SafeAssetName.IsEmpty())
	{
		OutError = FString::Printf(TEXT("OutputPackagePath or OutputAssetName is empty after sanitization: %s / %s"), *OutputPackagePath, *OutputAssetName);
		return nullptr;
	}

	const FString PackageName = SafePackagePath / SafeAssetName;
	if (!FPackageName::IsValidLongPackageName(PackageName))
	{
		OutError = FString::Printf(TEXT("PackageName is not a valid long package name: %s"), *PackageName);
		return nullptr;
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		OutError = TEXT("No editor world available for fallback conversion.");
		return nullptr;
	}

	AActor* Actor = World->SpawnActor<AActor>();
	if (!Actor)
	{
		OutError = TEXT("Failed to spawn temporary actor for fallback conversion.");
		return nullptr;
	}

	USkeletalMeshComponent* MeshComponent = NewObject<USkeletalMeshComponent>(Actor);
	UStaticMesh* OutStaticMesh = nullptr;
	if (MeshComponent)
	{
		MeshComponent->SetSkeletalMesh(SkeletalMesh);
		MeshComponent->SetVisibility(true, true);
		MeshComponent->RegisterComponentWithWorld(World);
		MeshComponent->SetComponentToWorld(FTransform::Identity);
		MeshComponent->TickComponent(0.0f, ELevelTick::LEVELTICK_All, nullptr);
		MeshComponent->RefreshBoneTransforms();
		MeshComponent->FinalizeBoneTransform();
		MeshComponent->UpdateComponentToWorld();
		MeshComponent->RecreateRenderState_Concurrent();

		TArray<UMeshComponent*> MeshComponents = { MeshComponent };
		IMeshUtilities& MeshUtilities = FModuleManager::LoadModuleChecked<IMeshUtilities>(TEXT("MeshUtilities"));
		OutStaticMesh = MeshUtilities.ConvertMeshesToStaticMesh(MeshComponents, FTransform::Identity, PackageName);
		if (OutStaticMesh)
		{
			ApplyMassBattleStaticMeshSlotLayout(OutStaticMesh, SkeletalMesh, LightmapIndex, bGenerateLightmapUVs);
		}
		else
		{
			OutError = FString::Printf(TEXT("Fallback ConvertMeshesToStaticMesh returned nullptr for %s."), *SkeletalMesh->GetName());
		}

		MeshComponent->UnregisterComponent();
		MeshComponent->DestroyComponent();
	}
	else
	{
		OutError = TEXT("Failed to allocate temporary SkeletalMeshComponent.");
	}

	Actor->Destroy();
	return OutStaticMesh;
}

static FString BuildMassBattleStaticMeshSlotName(USkeletalMesh* SkeletalMesh, int32 LODIndex, int32 SectionIndex, int32 MaterialIndex)
{
	FString SkeletalSlotName = TEXT("Unnamed");
	if (SkeletalMesh && SkeletalMesh->GetMaterials().IsValidIndex(MaterialIndex))
	{
		SkeletalSlotName = SkeletalMesh->GetMaterials()[MaterialIndex].MaterialSlotName.ToString();
	}
	return FString::Printf(TEXT("%s_LOD%d_%d__%d"), *SkeletalSlotName, LODIndex, SectionIndex, MaterialIndex);
}

static UStaticMesh* BuildStaticMeshFromSkeletalRenderDataFallback(USkeletalMesh* SkeletalMesh, const FString& OutputPackagePath, const FString& OutputAssetName, int32 LightmapIndex, bool bGenerateLightmapUVs, FString& OutError)
{
	if (!SkeletalMesh)
	{
		OutError = TEXT("SkeletalMesh is null.");
		return nullptr;
	}

	FSkeletalMeshRenderData* RenderData = SkeletalMesh->GetResourceForRendering();
	if (!RenderData || RenderData->LODRenderData.IsEmpty())
	{
		OutError = FString::Printf(TEXT("SkeletalMesh '%s' has no render LOD data."), *SkeletalMesh->GetName());
		return nullptr;
	}

	const FString SafePackagePath = UMassBattleEditorMCPApi::MCP_SanitizeForPath(OutputPackagePath);
	const FString SafeAssetName = UMassBattleEditorMCPApi::MCP_SanitizeForPath(OutputAssetName);
	if (SafePackagePath.IsEmpty() || SafeAssetName.IsEmpty())
	{
		OutError = FString::Printf(TEXT("OutputPackagePath or OutputAssetName is empty after sanitization: %s / %s"), *OutputPackagePath, *OutputAssetName);
		return nullptr;
	}

	const FString PackageName = SafePackagePath / SafeAssetName;
	if (!FPackageName::IsValidLongPackageName(PackageName))
	{
		OutError = FString::Printf(TEXT("PackageName is not a valid long package name: %s"), *PackageName);
		return nullptr;
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("Failed to create package: %s"), *PackageName);
		return nullptr;
	}
	Package->FullyLoad();

	const int32 SourceLODIndex = 0;
	const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[SourceLODIndex];
	const uint32 VertexCount = LODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();
	if (VertexCount == 0)
	{
		OutError = FString::Printf(TEXT("SkeletalMesh '%s' LOD0 has no vertices."), *SkeletalMesh->GetName());
		return nullptr;
	}
	if (!LODData.MultiSizeIndexContainer.IsIndexBufferValid() || !LODData.MultiSizeIndexContainer.GetIndexBuffer() || LODData.MultiSizeIndexContainer.GetIndexBuffer()->Num() <= 0)
	{
		OutError = FString::Printf(TEXT("SkeletalMesh '%s' LOD0 has no CPU-readable index buffer."), *SkeletalMesh->GetName());
		return nullptr;
	}

	const FRawStaticIndexBuffer16or32Interface* IndexBuffer = LODData.MultiSizeIndexContainer.GetIndexBuffer();
	const FStaticMeshVertexBuffer& StaticMeshVertexBuffer = LODData.StaticVertexBuffers.StaticMeshVertexBuffer;
	const int32 NumTexCoords = FMath::Clamp<int32>((int32)StaticMeshVertexBuffer.GetNumTexCoords(), 1, MAX_STATIC_TEXCOORDS);

	FMeshDescription MeshDescription;
	FStaticMeshAttributes Attributes(MeshDescription);
	Attributes.Register();

	FMeshDescriptionBuilder Builder;
	Builder.SetMeshDescription(&MeshDescription);
	Builder.EnablePolyGroups();
	Builder.SetNumUVLayers(NumTexCoords);
	Builder.ReserveNewVertices((int32)VertexCount);

	TArray<FVertexID> VertexIDs;
	VertexIDs.Reserve((int32)VertexCount);
	for (uint32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex)
	{
		VertexIDs.Add(Builder.AppendVertex((FVector)LODData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(VertexIndex)));
	}

	TArray<FStaticMaterial> StaticMaterials;
	int32 BuiltTriangleCount = 0;
	for (int32 SectionIndex = 0; SectionIndex < LODData.RenderSections.Num(); ++SectionIndex)
	{
		const FSkelMeshRenderSection& Section = LODData.RenderSections[SectionIndex];
		if (!Section.IsValid())
		{
			continue;
		}

		const int32 MaterialIndex = Section.MaterialIndex;
		const FString SlotNameString = BuildMassBattleStaticMeshSlotName(SkeletalMesh, SourceLODIndex, SectionIndex, MaterialIndex);
		const FName SlotName(*SlotNameString);
		const FPolygonGroupID PolygonGroupID = Builder.AppendPolygonGroup(SlotName);

		UMaterialInterface* MaterialInterface = nullptr;
		if (SkeletalMesh->GetMaterials().IsValidIndex(MaterialIndex))
		{
			MaterialInterface = SkeletalMesh->GetMaterials()[MaterialIndex].MaterialInterface;
		}

		FStaticMaterial StaticMaterial(MaterialInterface);
		StaticMaterial.MaterialSlotName = SlotName;
		StaticMaterial.ImportedMaterialSlotName = SlotName;
		StaticMaterials.Add(StaticMaterial);

		for (uint32 TriangleIndex = 0; TriangleIndex < Section.NumTriangles; ++TriangleIndex)
		{
			const uint32 IndexBase = Section.BaseIndex + TriangleIndex * 3;
			const uint32 VertexIndex0 = IndexBuffer->Get(IndexBase);
			const uint32 VertexIndex1 = IndexBuffer->Get(IndexBase + 1);
			const uint32 VertexIndex2 = IndexBuffer->Get(IndexBase + 2);
			if (!VertexIDs.IsValidIndex((int32)VertexIndex0) || !VertexIDs.IsValidIndex((int32)VertexIndex1) || !VertexIDs.IsValidIndex((int32)VertexIndex2))
			{
				continue;
			}

			auto MakeInstance = [&](uint32 VertexIndex) -> FVertexInstanceID
			{
				const FVertexInstanceID InstanceID = Builder.AppendInstance(VertexIDs[(int32)VertexIndex]);
				const FVector Normal = (FVector)StaticMeshVertexBuffer.VertexTangentZ(VertexIndex);
				const FVector Tangent = (FVector)StaticMeshVertexBuffer.VertexTangentX(VertexIndex);
				Builder.SetInstanceTangentSpace(InstanceID, Normal, Tangent, 1.0f);
				for (int32 UVIndex = 0; UVIndex < NumTexCoords; ++UVIndex)
				{
					Builder.SetInstanceUV(InstanceID, (FVector2D)StaticMeshVertexBuffer.GetVertexUV(VertexIndex, UVIndex), UVIndex);
				}
				return InstanceID;
			};

			const FVertexInstanceID Instance0 = MakeInstance(VertexIndex0);
			const FVertexInstanceID Instance1 = MakeInstance(VertexIndex1);
			const FVertexInstanceID Instance2 = MakeInstance(VertexIndex2);
			const FTriangleID TriangleID = Builder.AppendTriangle(Instance0, Instance1, Instance2, PolygonGroupID);
			Builder.SetPolyGroupID(TriangleID, SectionIndex);
			++BuiltTriangleCount;
		}
	}

	if (BuiltTriangleCount == 0)
	{
		OutError = FString::Printf(TEXT("SkeletalMesh '%s' LOD0 produced no triangles."), *SkeletalMesh->GetName());
		return nullptr;
	}

	UStaticMesh* OutStaticMesh = NewObject<UStaticMesh>(Package, *SafeAssetName, RF_Public | RF_Standalone);
	if (!OutStaticMesh)
	{
		OutError = FString::Printf(TEXT("Failed to allocate StaticMesh: %s"), *PackageName);
		return nullptr;
	}

	OutStaticMesh->SetStaticMaterials(StaticMaterials);
	OutStaticMesh->SetLightingGuid();
	OutStaticMesh->NeverStream = true;
	OutStaticMesh->bAllowCPUAccess = true;

	TArray<const FMeshDescription*> MeshDescriptions;
	MeshDescriptions.Add(&MeshDescription);
	UStaticMesh::FBuildMeshDescriptionsParams BuildParams;
	BuildParams.bBuildSimpleCollision = false;
	BuildParams.bFastBuild = false;
	BuildParams.bAllowCpuAccess = true;
	BuildParams.bMarkPackageDirty = true;
	if (!OutStaticMesh->BuildFromMeshDescriptions(MeshDescriptions, BuildParams))
	{
		OutError = FString::Printf(TEXT("BuildFromMeshDescriptions failed for %s."), *PackageName);
		return nullptr;
	}

	if (OutStaticMesh->GetNumSourceModels() > 0)
	{
		FStaticMeshSourceModel& SourceModel = OutStaticMesh->GetSourceModel(0);
		SourceModel.BuildSettings.bGenerateLightmapUVs = bGenerateLightmapUVs;
		if (bGenerateLightmapUVs)
		{
			SourceModel.BuildSettings.DstLightmapIndex = LightmapIndex;
		}
	}

	OutStaticMesh->PostEditChange();
	OutStaticMesh->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(OutStaticMesh);
	return OutStaticMesh;
}
}

/// 辅助: 获取 UObject 资产路径字符串
static FString GetAssetPathString(const UObject* Obj)
{
	if (!Obj) return TEXT("");
	return Obj->GetPathName();
}

// ==================== JSON 序列化/反序列化 ====================

FString UMassBattleEditorMCPApi::SerializeOriginalTexturesToJson(const TArray<FOriginalTextures>& Textures)
{
	TArray<TSharedPtr<FJsonValue>> JsonArray;

	for (const FOriginalTextures& Tex : Textures)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("SlotName"), Tex.SlotName);
		Obj->SetBoolField(TEXT("bUseARM"), Tex.bUseARM);
		Obj->SetStringField(TEXT("ARM"), GetAssetPathString(Tex.ARM));
		Obj->SetStringField(TEXT("BaseColor"), GetAssetPathString(Tex.BaseColor));
		Obj->SetStringField(TEXT("Specular"), GetAssetPathString(Tex.Specular));
		Obj->SetStringField(TEXT("Roughness"), GetAssetPathString(Tex.Roughness));
		Obj->SetStringField(TEXT("Normal"), GetAssetPathString(Tex.Normal));
		Obj->SetStringField(TEXT("Metallic"), GetAssetPathString(Tex.Metallic));
		Obj->SetStringField(TEXT("Emissive"), GetAssetPathString(Tex.Emissive));
		Obj->SetStringField(TEXT("Opacity"), GetAssetPathString(Tex.Opacity));
		Obj->SetStringField(TEXT("AO"), GetAssetPathString(Tex.AO));
		JsonArray.Add(MakeShared<FJsonValueObject>(Obj));
	}

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(JsonArray, Writer);
	return OutputString;
}

bool UMassBattleEditorMCPApi::ParseOriginalTexturesFromJson(const FString& JsonString, TArray<FOriginalTextures>& OutTextures)
{
	OutTextures.Empty();

	TArray<TSharedPtr<FJsonValue>> JsonArray;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, JsonArray))
	{
		UE_LOG(LogMassBattleEditorMCPApi, Error, TEXT("ParseOriginalTexturesFromJson: Failed to parse JSON"));
		return false;
	}

	for (const TSharedPtr<FJsonValue>& Val : JsonArray)
	{
		const TSharedPtr<FJsonObject>* Obj;
		if (!Val->TryGetObject(Obj)) continue;

		FOriginalTextures Tex;
		Tex.SlotName = (*Obj)->GetStringField(TEXT("SlotName"));
		Tex.bUseARM = (*Obj)->GetBoolField(TEXT("bUseARM"));

		/// 加载纹理资产
		auto LoadTex = [](const FString& Path) -> UTexture2D*
		{
			if (Path.IsEmpty()) return nullptr;
			return LoadObject<UTexture2D>(nullptr, *Path);
		};

		Tex.ARM = LoadTex((*Obj)->GetStringField(TEXT("ARM")));
		Tex.BaseColor = LoadTex((*Obj)->GetStringField(TEXT("BaseColor")));
		Tex.Specular = LoadTex((*Obj)->GetStringField(TEXT("Specular")));
		Tex.Roughness = LoadTex((*Obj)->GetStringField(TEXT("Roughness")));
		Tex.Normal = LoadTex((*Obj)->GetStringField(TEXT("Normal")));
		Tex.Metallic = LoadTex((*Obj)->GetStringField(TEXT("Metallic")));
		Tex.Emissive = LoadTex((*Obj)->GetStringField(TEXT("Emissive")));
		Tex.Opacity = LoadTex((*Obj)->GetStringField(TEXT("Opacity")));
		Tex.AO = LoadTex((*Obj)->GetStringField(TEXT("AO")));

		OutTextures.Add(Tex);
	}

	return true;
}

FString UMassBattleEditorMCPApi::SerializeFoundAnimsToJson(const FFoundAnimSequences& Anims)
{
	/// 辅助: 将 TObjectPtr<UAnimSequence> 数组序列化为路径字符串 JSON 数组
	auto SerializeAnimArray = [](const TArray<TObjectPtr<UAnimSequence>>& AnimArray) -> TArray<TSharedPtr<FJsonValue>>
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const TObjectPtr<UAnimSequence>& Anim : AnimArray)
		{
			if (Anim)
			{
				Arr.Add(MakeShared<FJsonValueString>(Anim->GetPathName()));
			}
		}
		return Arr;
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

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	return OutputString;
}

bool UMassBattleEditorMCPApi::ParseFoundAnimsFromJson(const FString& JsonString, FFoundAnimSequences& OutAnims)
{
	OutAnims = FFoundAnimSequences();

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogMassBattleEditorMCPApi, Error, TEXT("ParseFoundAnimsFromJson: Failed to parse JSON"));
		return false;
	}

	/// 辅助: 将路径字符串 JSON 数组解析为 TObjectPtr<UAnimSequence> 数组
	auto ParseAnimArray = [](const TSharedPtr<FJsonObject>& Obj, const FString& FieldName, TArray<TObjectPtr<UAnimSequence>>& OutArray)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr;
		if (Obj->TryGetArrayField(FieldName, Arr))
		{
			for (const TSharedPtr<FJsonValue>& Val : *Arr)
			{
				FString Path;
				if (Val->TryGetString(Path) && !Path.IsEmpty())
				{
					UAnimSequence* AnimSeq = LoadObject<UAnimSequence>(nullptr, *Path);
					if (AnimSeq)
					{
						OutArray.Add(AnimSeq);
					}
				}
			}
		}
	};

	ParseAnimArray(Root, TEXT("Idle"), OutAnims.Idle);
	ParseAnimArray(Root, TEXT("Move"), OutAnims.Move);
	ParseAnimArray(Root, TEXT("Fall"), OutAnims.Fall);
	ParseAnimArray(Root, TEXT("Appear"), OutAnims.Appear);
	ParseAnimArray(Root, TEXT("Attack"), OutAnims.Attack);
	ParseAnimArray(Root, TEXT("Hit"), OutAnims.Hit);
	ParseAnimArray(Root, TEXT("Death"), OutAnims.Death);
	ParseAnimArray(Root, TEXT("Other"), OutAnims.Other);

	return true;
}

// ==================== MCP API 实现 ====================

FString UMassBattleEditorMCPApi::MCP_DuplicateClassAsset(const FString& SourceClassPath, const FString& NewClassName, const FString& PackagePath)
{
	/// 加载源蓝图类
	UClass* SourceClass = LoadObject<UClass>(nullptr, *SourceClassPath);
	if (!SourceClass)
	{
		return MakeErrorJson(FString::Printf(TEXT("Failed to load source class: %s"), *SourceClassPath));
	}

	/// 获取 WorldContext
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return MakeErrorJson(TEXT("No editor world available"));
	}

	/// 调用原始函数
	UClass* NewClass = UMassBattleFuncLibEd::DuplicateClassAsset(World, SourceClass, NewClassName, PackagePath);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	if (NewClass)
	{
		Root->SetBoolField(TEXT("success"), true);
		Root->SetStringField(TEXT("class_path"), NewClass->GetPathName());
	}
	else
	{
		Root->SetBoolField(TEXT("success"), false);
		Root->SetStringField(TEXT("error"), TEXT("DuplicateClassAsset returned nullptr"));
	}

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	return OutputString;
}

FString UMassBattleEditorMCPApi::MCP_SetClassDefaultProperties(const FString& TargetClassPath, const FString& AgentMeshPath, const FString& NiagaraSystemPath, int32 SubType, bool bSaveAssets)
{
	/// 加载目标蓝图类
	UClass* TargetClass = LoadObject<UClass>(nullptr, *TargetClassPath);
	if (!TargetClass)
	{
		return MakeErrorJson(FString::Printf(TEXT("Failed to load target class: %s"), *TargetClassPath));
	}

	/// 验证类型
	if (!TargetClass->IsChildOf(AMassBattleAgentRenderer::StaticClass()))
	{
		return MakeErrorJson(FString::Printf(TEXT("Target class is not a subclass of AMassBattleAgentRenderer: %s"), *TargetClassPath));
	}

	/// 加载可选资产
	UStaticMesh* AgentMesh = nullptr;
	if (!AgentMeshPath.IsEmpty())
	{
		AgentMesh = LoadObject<UStaticMesh>(nullptr, *AgentMeshPath);
		if (!AgentMesh)
		{
			return MakeErrorJson(FString::Printf(TEXT("Failed to load StaticMesh: %s"), *AgentMeshPath));
		}
	}

	UNiagaraSystem* NiagaraSystem = nullptr;
	if (!NiagaraSystemPath.IsEmpty())
	{
		NiagaraSystem = LoadObject<UNiagaraSystem>(nullptr, *NiagaraSystemPath);
		if (!NiagaraSystem)
		{
			return MakeErrorJson(FString::Printf(TEXT("Failed to load NiagaraSystem: %s"), *NiagaraSystemPath));
		}
	}

	/// 获取 WorldContext
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return MakeErrorJson(TEXT("No editor world available"));
	}

	/// 调用原始函数
	UMassBattleFuncLibEd::SetClassDefaultProperties(World, TargetClass, AgentMesh, NiagaraSystem, SubType);

	AMassBattleAgentRenderer* CDO = TargetClass->GetDefaultObject<AMassBattleAgentRenderer>();
	UObject* SaveTarget = TargetClass;
	FString BlueprintPath;
	if (const UBlueprintGeneratedClass* BPClass = Cast<UBlueprintGeneratedClass>(TargetClass))
	{
		if (UBlueprint* Blueprint = Cast<UBlueprint>(BPClass->ClassGeneratedBy))
		{
			SaveTarget = Blueprint;
			BlueprintPath = Blueprint->GetPathName();
		}
	}

	bool bSaved = false;
	FString SaveError;
	if (bSaveAssets)
	{
		if (SaveLoadedAsset(SaveTarget, SaveError))
		{
			bSaved = true;
		}
		else
		{
			return MakeErrorJson(SaveError);
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("success"), true);
	Root->SetStringField(TEXT("target_class"), TargetClass->GetPathName());
	Root->SetStringField(TEXT("blueprint_path"), BlueprintPath);
	Root->SetStringField(TEXT("agent_mesh"), CDO ? ObjectPathOrEmpty(CDO->AgentMesh) : FString());
	Root->SetStringField(TEXT("niagara_system"), CDO ? ObjectPathOrEmpty(CDO->NiagaraSystemAsset) : FString());
	Root->SetNumberField(TEXT("subtype"), CDO ? CDO->SubType.Index : SubType);
	Root->SetBoolField(TEXT("save_assets"), bSaveAssets);
	Root->SetBoolField(TEXT("saved"), bSaved);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	return OutputString;
}

FString UMassBattleEditorMCPApi::MCP_ConvertSkeletalMeshToStaticMeshWithLODs(const FString& SkeletalMeshPath, const FString& OutputPackagePath, const FString& OutputAssetName, int32 LightmapIndex, bool bGenerateLightmapUVs)
{
	/// 加载 SkeletalMesh
	USkeletalMesh* SkeletalMesh = LoadObject<USkeletalMesh>(nullptr, *SkeletalMeshPath);
	if (!SkeletalMesh)
	{
		return MakeErrorJson(FString::Printf(TEXT("Failed to load SkeletalMesh: %s"), *SkeletalMeshPath));
	}

	UStaticMesh* Result = nullptr;
	FString ConversionMethod;
	FString FallbackError;
	if (IsRunningCommandlet())
	{
		Result = BuildStaticMeshFromSkeletalRenderDataFallback(SkeletalMesh, OutputPackagePath, OutputAssetName, LightmapIndex, bGenerateLightmapUVs, FallbackError);
		ConversionMethod = TEXT("MCP fallback BuildFromSkeletalRenderData");
	}
	if (!Result)
	{
		/// 调用原始函数
		Result = UMassBattleFuncLibEd::ConvertSkeletalMeshToStaticMeshWithLODs(SkeletalMesh, OutputPackagePath, OutputAssetName, LightmapIndex, bGenerateLightmapUVs);
		ConversionMethod = TEXT("MassBattleFuncLibEd::ConvertSkeletalMeshToStaticMeshWithLODs");
	}
	if (!Result)
	{
		FString ComponentFallbackError;
		Result = ConvertSkeletalMeshToStaticMeshMcpFallback(SkeletalMesh, OutputPackagePath, OutputAssetName, LightmapIndex, bGenerateLightmapUVs, ComponentFallbackError);
		ConversionMethod = TEXT("MCP fallback ConvertMeshesToStaticMesh");
		if (!ComponentFallbackError.IsEmpty())
		{
			FallbackError = FallbackError.IsEmpty() ? ComponentFallbackError : FString::Printf(TEXT("%s; %s"), *FallbackError, *ComponentFallbackError);
		}
	}
	if (!Result && !IsRunningCommandlet())
	{
		FString RenderDataFallbackError;
		Result = BuildStaticMeshFromSkeletalRenderDataFallback(SkeletalMesh, OutputPackagePath, OutputAssetName, LightmapIndex, bGenerateLightmapUVs, RenderDataFallbackError);
		ConversionMethod = TEXT("MCP fallback BuildFromSkeletalRenderData");
		if (!RenderDataFallbackError.IsEmpty())
		{
			FallbackError = FallbackError.IsEmpty() ? RenderDataFallbackError : FString::Printf(TEXT("%s; %s"), *FallbackError, *RenderDataFallbackError);
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	if (Result)
	{
		Root->SetBoolField(TEXT("success"), true);
		Root->SetStringField(TEXT("static_mesh_path"), Result->GetPathName());
		Root->SetStringField(TEXT("conversion_method"), ConversionMethod);
	}
	else
	{
		Root->SetBoolField(TEXT("success"), false);
		Root->SetStringField(TEXT("error"), FallbackError.IsEmpty() ? TEXT("ConvertSkeletalMeshToStaticMeshWithLODs returned nullptr") : FallbackError);
		Root->SetStringField(TEXT("conversion_method"), ConversionMethod);
	}

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	return OutputString;
}

FString UMassBattleEditorMCPApi::MCP_CreateMaterialInstanceForStaticMeshWithLODs(const FString& StaticMeshPath, const FString& PackagePath, const FString& AssetName, const FString& ParentMaterialPath, const FString& OriginalTexturesJson)
{
	/// 加载 StaticMesh
	UStaticMesh* InStaticMesh = LoadObject<UStaticMesh>(nullptr, *StaticMeshPath);
	if (!InStaticMesh)
	{
		return MakeErrorJson(FString::Printf(TEXT("Failed to load StaticMesh: %s"), *StaticMeshPath));
	}

	/// 加载父材质
	UMaterialInterface* ParentMaterial = LoadObject<UMaterialInterface>(nullptr, *ParentMaterialPath);
	if (!ParentMaterial)
	{
		return MakeErrorJson(FString::Printf(TEXT("Failed to load ParentMaterial: %s"), *ParentMaterialPath));
	}

	/// 解析 OriginalTextures JSON
	TArray<FOriginalTextures> OriginalTexturesArray;
	if (!OriginalTexturesJson.IsEmpty())
	{
		if (!ParseOriginalTexturesFromJson(OriginalTexturesJson, OriginalTexturesArray))
		{
			return MakeErrorJson(TEXT("Failed to parse OriginalTexturesJson"));
		}
	}

	/// 调用原始函数
	UMassBattleFuncLibEd::CreateMaterialInstanceForStaticMeshWithLODs(InStaticMesh, PackagePath, AssetName, ParentMaterial, OriginalTexturesArray);

	const int32 MaterialTextureOverrideCount = ApplyOriginalTexturesToGeneratedVatMaterials(InStaticMesh, OriginalTexturesArray);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("success"), true);
	Root->SetNumberField(TEXT("material_texture_override_count"), MaterialTextureOverrideCount);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	return OutputString;
}

FString UMassBattleEditorMCPApi::MCP_FindAndFillOriginalTextures(const FString& SkeletalMeshPath, const FString& SearchPath, const FString& AssetName)
{
	/// 加载 SkeletalMesh
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *SkeletalMeshPath);
	if (!Mesh)
	{
		return MakeErrorJson(FString::Printf(TEXT("Failed to load SkeletalMesh: %s"), *SkeletalMeshPath));
	}

	if (!SearchPath.IsEmpty())
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		TArray<FString> PathsToScan;
		PathsToScan.Add(SearchPath);
		AssetRegistryModule.Get().ScanPathsSynchronous(PathsToScan, true);
	}

	/// 调用原始函数
	TArray<FOriginalTextures> EmptyTextures;
	TArray<FOriginalTextures> Result = UMassBattleFuncLibEd::FindAndFillOriginalTextures(Mesh, EmptyTextures, SearchPath, AssetName);

	/// 构造返回 JSON
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("success"), true);

	/// 序列化纹理数据
	FString TexturesJsonStr = SerializeOriginalTexturesToJson(Result);
	TArray<TSharedPtr<FJsonValue>> TexturesArray;
	TSharedRef<TJsonReader<>> TexReader = TJsonReaderFactory<>::Create(TexturesJsonStr);
	FJsonSerializer::Deserialize(TexReader, TexturesArray);
	Root->SetArrayField(TEXT("textures"), TexturesArray);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	return OutputString;
}

FString UMassBattleEditorMCPApi::MCP_FindAndFillAnimSequences(const FString& SkeletalMeshPath, const FString& SearchPath, const FString& FileName)
{
	/// 加载 SkeletalMesh
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *SkeletalMeshPath);
	if (!Mesh)
	{
		return MakeErrorJson(FString::Printf(TEXT("Failed to load SkeletalMesh: %s"), *SkeletalMeshPath));
	}

	if (!SearchPath.IsEmpty())
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		TArray<FString> PathsToScan;
		PathsToScan.Add(SearchPath);
		AssetRegistryModule.Get().ScanPathsSynchronous(PathsToScan, true);
	}

	/// 调用原始函数
	FFoundAnimSequences EmptyAnims;
	FFoundAnimSequences Result = UMassBattleFuncLibEd::FindAndFillAnimSequences(Mesh, EmptyAnims, SearchPath, FileName);

	/// 构造返回 JSON
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("success"), true);

	/// 内嵌动画数据
	FString AnimsJsonStr = SerializeFoundAnimsToJson(Result);
	TSharedPtr<FJsonObject> AnimsObj;
	TSharedRef<TJsonReader<>> AnimReader = TJsonReaderFactory<>::Create(AnimsJsonStr);
	FJsonSerializer::Deserialize(AnimReader, AnimsObj);
	Root->SetObjectField(TEXT("anims"), AnimsObj);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	return OutputString;
}

FString UMassBattleEditorMCPApi::MCP_FindAndFillLODSettings(const FString& SkeletalMeshPath)
{
	/// 加载 SkeletalMesh
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *SkeletalMeshPath);
	if (!Mesh)
	{
		return MakeErrorJson(FString::Printf(TEXT("Failed to load SkeletalMesh: %s"), *SkeletalMeshPath));
	}

	/// 调用原始函数
	TArray<FLODDataEd> EmptySettings;
	TArray<FLODDataEd> Result = UMassBattleFuncLibEd::FindAndFillLODSettings(Mesh, EmptySettings);

	/// 构造返回 JSON
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("success"), true);

	TArray<TSharedPtr<FJsonValue>> LODArray;
	for (const FLODDataEd& LOD : Result)
	{
		TSharedPtr<FJsonObject> LODObj = MakeShared<FJsonObject>();
		LODObj->SetNumberField(TEXT("ScreenSize"), LOD.ScreenSize);
		LODObj->SetNumberField(TEXT("LODIndex"), LOD.LODIndex);
		LODObj->SetNumberField(TEXT("AnimBlendLevel"), LOD.AnimBlendLevel);
		LODObj->SetNumberField(TEXT("Mode"), static_cast<int32>(LOD.Mode));
		LODArray.Add(MakeShared<FJsonValueObject>(LODObj));
	}
	Root->SetArrayField(TEXT("lod_settings"), LODArray);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	return OutputString;
}

FString UMassBattleEditorMCPApi::MCP_CreateAnimDataTexture(const FString& AnimsInfoJson, const FString& PackagePath, const FString& AssetName)
{
	/// 解析动画帧信息 JSON
	TArray<FAnimToTextureAnimInfo> Anims;

	TArray<TSharedPtr<FJsonValue>> JsonArray;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(AnimsInfoJson);
	if (!FJsonSerializer::Deserialize(Reader, JsonArray))
	{
		return MakeErrorJson(TEXT("Failed to parse AnimsInfoJson"));
	}

	for (const TSharedPtr<FJsonValue>& Val : JsonArray)
	{
		const TSharedPtr<FJsonObject>* Obj;
		if (!Val->TryGetObject(Obj)) continue;

		FAnimToTextureAnimInfo Info;
		Info.StartFrame = (*Obj)->GetIntegerField(TEXT("StartFrame"));
		Info.EndFrame = (*Obj)->GetIntegerField(TEXT("EndFrame"));
		Anims.Add(Info);
	}

	/// 调用原始函数
	UTexture2D* Result = UMassBattleFuncLibEd::CreateAnimDataTexture(Anims, PackagePath, AssetName);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	if (Result)
	{
		Root->SetBoolField(TEXT("success"), true);
		Root->SetStringField(TEXT("texture_path"), Result->GetPathName());
	}
	else
	{
		Root->SetBoolField(TEXT("success"), false);
		Root->SetStringField(TEXT("error"), TEXT("CreateAnimDataTexture returned nullptr"));
	}

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	return OutputString;
}

FString UMassBattleEditorMCPApi::MCP_CreateAnimsDataFromSequences(const FString& DataAssetPath, const FString& FoundAnimsJson)
{
	/// 加载 DataAsset
	UAnimToTextureDataAsset* DataAsset = LoadObject<UAnimToTextureDataAsset>(nullptr, *DataAssetPath);
	if (!DataAsset)
	{
		return MakeErrorJson(FString::Printf(TEXT("Failed to load AnimToTextureDataAsset: %s"), *DataAssetPath));
	}

	/// 解析动画序列 JSON
	FFoundAnimSequences FoundAnims;
	if (!ParseFoundAnimsFromJson(FoundAnimsJson, FoundAnims))
	{
		return MakeErrorJson(TEXT("Failed to parse FoundAnimsJson"));
	}

	int32 FoundAnimationCount = 0;
	int32 ResolvedAnimationCount = 0;
	int32 UnresolvedAnimationCount = 0;
	TArray<TSharedPtr<FJsonValue>> ResolvedAnimations;
	TArray<TSharedPtr<FJsonValue>> UnresolvedAnimations;

	auto CountAnimArray = [&](const TArray<TObjectPtr<UAnimSequence>>& SourceAnims, const FString& Category)
	{
		for (const TObjectPtr<UAnimSequence>& AnimPtr : SourceAnims)
		{
			UAnimSequence* AnimSeq = AnimPtr.Get();
			if (!AnimSeq)
			{
				continue;
			}

			++FoundAnimationCount;
			const int32 FoundIndex = DataAsset->GetIndexFromAnimSequence(AnimSeq);
			const bool bResolved = FoundIndex != INDEX_NONE && DataAsset->Animations.IsValidIndex(FoundIndex);
			TSharedPtr<FJsonObject> AnimInfo = MakeShared<FJsonObject>();
			AnimInfo->SetStringField(TEXT("category"), Category);
			AnimInfo->SetStringField(TEXT("name"), AnimSeq->GetName());
			AnimInfo->SetStringField(TEXT("path"), AnimSeq->GetPathName());
			AnimInfo->SetNumberField(TEXT("anim_index"), FoundIndex);
			if (bResolved)
			{
				++ResolvedAnimationCount;
				const auto& DataAssetAnim = DataAsset->Animations[FoundIndex];
				AnimInfo->SetNumberField(TEXT("start_frame"), DataAssetAnim.StartFrame);
				AnimInfo->SetNumberField(TEXT("end_frame"), DataAssetAnim.EndFrame);
				ResolvedAnimations.Add(MakeShared<FJsonValueObject>(AnimInfo));
			}
			else
			{
				++UnresolvedAnimationCount;
				UnresolvedAnimations.Add(MakeShared<FJsonValueObject>(AnimInfo));
			}
		}
	};

	CountAnimArray(FoundAnims.Idle, TEXT("Idle"));
	CountAnimArray(FoundAnims.Move, TEXT("Move"));
	CountAnimArray(FoundAnims.Fall, TEXT("Fall"));
	CountAnimArray(FoundAnims.Appear, TEXT("Appear"));
	CountAnimArray(FoundAnims.Attack, TEXT("Attack"));
	CountAnimArray(FoundAnims.Hit, TEXT("Hit"));
	CountAnimArray(FoundAnims.Death, TEXT("Death"));
	CountAnimArray(FoundAnims.Other, TEXT("Other"));

	/// 调用原始函数
	FAnimsData Result = UMassBattleFuncLibEd::CreateAnimsDataFromSequences(DataAsset, FoundAnims);

	/// 序列化 FAnimsData 为 JSON
	/// 将每个 FAnimData 的 Anim0-Anim4 序列化为 Vector4 数组
	auto SerializeAnimData = [](const FAnimData& Data) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

		auto Vec4ToJson = [](const FVector4f& V) -> TSharedPtr<FJsonObject>
		{
			TSharedPtr<FJsonObject> VObj = MakeShared<FJsonObject>();
			VObj->SetNumberField(TEXT("AnimIndex"), V.X);
			VObj->SetNumberField(TEXT("PlayLength"), V.Y);
			VObj->SetNumberField(TEXT("StartFrame"), V.Z);
			VObj->SetNumberField(TEXT("EndFrame"), V.W);
			return VObj;
		};

		Obj->SetObjectField(TEXT("Anim0"), Vec4ToJson(Data.Anim0));
		Obj->SetObjectField(TEXT("Anim1"), Vec4ToJson(Data.Anim1));
		Obj->SetObjectField(TEXT("Anim2"), Vec4ToJson(Data.Anim2));
		Obj->SetObjectField(TEXT("Anim3"), Vec4ToJson(Data.Anim3));
		Obj->SetObjectField(TEXT("Anim4"), Vec4ToJson(Data.Anim4));
		return Obj;
	};

	TSharedPtr<FJsonObject> AnimsDataObj = MakeShared<FJsonObject>();
	AnimsDataObj->SetObjectField(TEXT("IdleAnimData"), SerializeAnimData(Result.IdleAnimData));
	AnimsDataObj->SetObjectField(TEXT("MoveAnimData"), SerializeAnimData(Result.MoveAnimData));
	AnimsDataObj->SetObjectField(TEXT("FallAnimData"), SerializeAnimData(Result.FallAnimData));
	AnimsDataObj->SetObjectField(TEXT("AppearAnimData"), SerializeAnimData(Result.AppearAnimData));
	AnimsDataObj->SetObjectField(TEXT("AttackAnimData"), SerializeAnimData(Result.AttackAnimData));
	AnimsDataObj->SetObjectField(TEXT("HitAnimData"), SerializeAnimData(Result.HitAnimData));
	AnimsDataObj->SetObjectField(TEXT("DeathAnimData"), SerializeAnimData(Result.DeathAnimData));
	AnimsDataObj->SetObjectField(TEXT("OtherAnimData"), SerializeAnimData(Result.OtherAnimData));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("success"), true);
	Root->SetObjectField(TEXT("anims_data"), AnimsDataObj);
	Root->SetNumberField(TEXT("found_animation_count"), FoundAnimationCount);
	Root->SetNumberField(TEXT("resolved_animation_count"), ResolvedAnimationCount);
	Root->SetNumberField(TEXT("unresolved_animation_count"), UnresolvedAnimationCount);
	Root->SetBoolField(TEXT("has_resolved_animation_data"), ResolvedAnimationCount > 0);
	Root->SetArrayField(TEXT("resolved_animations"), ResolvedAnimations);
	Root->SetArrayField(TEXT("unresolved_animations"), UnresolvedAnimations);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	return OutputString;
}

FString UMassBattleEditorMCPApi::MCP_ConvertLODSettingsToLODsData(const FString& LODSettingsJson)
{
	/// 解析 LODSettings JSON
	TArray<TSharedPtr<FJsonValue>> JsonArray;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(LODSettingsJson);
	if (!FJsonSerializer::Deserialize(Reader, JsonArray))
	{
		return MakeErrorJson(TEXT("Failed to parse LODSettingsJson"));
	}

	TArray<FLODDataEd> LODSettings;
	for (const TSharedPtr<FJsonValue>& Val : JsonArray)
	{
		const TSharedPtr<FJsonObject>* Obj;
		if (!Val->TryGetObject(Obj)) continue;

		FLODDataEd Setting;
		Setting.ScreenSize = (*Obj)->GetNumberField(TEXT("ScreenSize"));
		Setting.LODIndex = (*Obj)->GetIntegerField(TEXT("LODIndex"));
		Setting.AnimBlendLevel = (*Obj)->GetIntegerField(TEXT("AnimBlendLevel"));
		Setting.Mode = static_cast<EVATBakeMode>((*Obj)->GetIntegerField(TEXT("Mode")));
		LODSettings.Add(Setting);
	}

	/// 调用原始函数
	FLODsData Result = UMassBattleFuncLibEd::ConvertLODSettingsToLODsData(LODSettings);

	/// 序列化 FLODsData 为 JSON
	auto SerializeLODData = [](const FLODData& Data) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("LODIndex"), Data.LODIndex);
		Obj->SetNumberField(TEXT("ScreenSize"), Data.ScreenSize);
		Obj->SetNumberField(TEXT("AnimBlendLevel"), Data.AnimBlendLevel);
		return Obj;
	};

	TSharedPtr<FJsonObject> LODsDataObj = MakeShared<FJsonObject>();
	LODsDataObj->SetObjectField(TEXT("Data0"), SerializeLODData(Result.Data0));
	LODsDataObj->SetObjectField(TEXT("Data1"), SerializeLODData(Result.Data1));
	LODsDataObj->SetObjectField(TEXT("Data2"), SerializeLODData(Result.Data2));
	LODsDataObj->SetObjectField(TEXT("Data3"), SerializeLODData(Result.Data3));
	LODsDataObj->SetObjectField(TEXT("Data4"), SerializeLODData(Result.Data4));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("success"), true);
	Root->SetObjectField(TEXT("lods_data"), LODsDataObj);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	return OutputString;
}

FString UMassBattleEditorMCPApi::MCP_ValidateAnimSequences(const FString& FoundAnimsJson)
{
	/// 解析动画序列 JSON
	FFoundAnimSequences FoundAnims;
	if (!ParseFoundAnimsFromJson(FoundAnimsJson, FoundAnims))
	{
		return MakeErrorJson(TEXT("Failed to parse FoundAnimsJson"));
	}

	/// 调用原始函数 (注意: 原始函数内部会弹出 MessageDialog，MCP 环境下可能需要处理)
	FFoundAnimSequences ValidatedAnims = UMassBattleFuncLibEd::ValidateAnimSequences(FoundAnims);

	/// 构造返回 JSON
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("success"), true);

	/// 序列化验证后的动画数据
	FString AnimsJsonStr = SerializeFoundAnimsToJson(ValidatedAnims);
	TSharedPtr<FJsonObject> AnimsObj;
	TSharedRef<TJsonReader<>> AnimReader = TJsonReaderFactory<>::Create(AnimsJsonStr);
	FJsonSerializer::Deserialize(AnimReader, AnimsObj);
	Root->SetObjectField(TEXT("validated_anims"), AnimsObj);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	return OutputString;
}

FString UMassBattleEditorMCPApi::MCP_RenameSkeletalMesh(const FString& SkeletalMeshPath, const FString& OriginalTexturesJson, const FString& AssetName)
{
	/// 加载 SkeletalMesh
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *SkeletalMeshPath);
	if (!Mesh)
	{
		return MakeErrorJson(FString::Printf(TEXT("Failed to load SkeletalMesh: %s"), *SkeletalMeshPath));
	}

	/// 解析 OriginalTextures JSON
	TArray<FOriginalTextures> OriginalTexturesArray;
	if (!OriginalTexturesJson.IsEmpty())
	{
		if (!ParseOriginalTexturesFromJson(OriginalTexturesJson, OriginalTexturesArray))
		{
			return MakeErrorJson(TEXT("Failed to parse OriginalTexturesJson"));
		}
	}

	/// 调用原始函数
	UMassBattleFuncLibEd::RenameSkeletalMesh(Mesh, OriginalTexturesArray, AssetName);

	return MakeSuccessJson();
}

FString UMassBattleEditorMCPApi::MCP_RenameOriginalTextures(const FString& OriginalTexturesJson, const FString& AssetName)
{
	/// 解析 OriginalTextures JSON
	TArray<FOriginalTextures> OriginalTexturesArray;
	if (!ParseOriginalTexturesFromJson(OriginalTexturesJson, OriginalTexturesArray))
	{
		return MakeErrorJson(TEXT("Failed to parse OriginalTexturesJson"));
	}

	/// 调用原始函数
	UMassBattleFuncLibEd::RenameOriginalTextures(OriginalTexturesArray, AssetName);

	return MakeSuccessJson();
}

FString UMassBattleEditorMCPApi::MCP_RenameAnimSequences(const FString& FoundAnimsJson, const FString& AssetName)
{
	/// 解析动画序列 JSON
	FFoundAnimSequences FoundAnims;
	if (!ParseFoundAnimsFromJson(FoundAnimsJson, FoundAnims))
	{
		return MakeErrorJson(TEXT("Failed to parse FoundAnimsJson"));
	}

	/// 调用原始函数
	UMassBattleFuncLibEd::RenameAnimSequences(FoundAnims, AssetName);

	return MakeSuccessJson();
}

FString UMassBattleEditorMCPApi::MCP_SanitizeForPath(const FString& InString)
{
	return UMassBattleFuncLibEd::SanitizeForPath(InString);
}

FString UMassBattleEditorMCPApi::MCP_GetApiStatus()
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("api_name"), TEXT("MassBattleEditorMCP"));
	Root->SetStringField(TEXT("version"), TEXT("1.0.0"));
	Root->SetStringField(TEXT("module"), TEXT("MassBattleEditorMCP"));

	/// 工具列表
	TArray<TSharedPtr<FJsonValue>> Tools;

	auto AddTool = [&](const FString& Name, const FString& Description, const FString& ParamsDesc, const FString Category = TEXT("asset.pipeline"))
	{
		TSharedPtr<FJsonObject> Tool = MakeShared<FJsonObject>();
		Tool->SetStringField(TEXT("name"), Name);
		Tool->SetStringField(TEXT("category"), Category);
		Tool->SetStringField(TEXT("description"), Description);
		Tool->SetStringField(TEXT("parameters"), ParamsDesc);
		Tools.Add(MakeShared<FJsonValueObject>(Tool));
	};

	AddTool(TEXT("MCP_DuplicateClassAsset"),
		TEXT("复制蓝图类资产"),
		TEXT("SourceClassPath, NewClassName, PackagePath"));

	AddTool(TEXT("MCP_SetClassDefaultProperties"),
		TEXT("设置 MassBattleAgentRenderer 蓝图类的默认属性"),
		TEXT("TargetClassPath, AgentMeshPath, NiagaraSystemPath, SubType, bSaveAssets"));

	AddTool(TEXT("MCP_ConvertSkeletalMeshToStaticMeshWithLODs"),
		TEXT("将 SkeletalMesh 转换为带 LOD 的 StaticMesh"),
		TEXT("SkeletalMeshPath, OutputPackagePath, OutputAssetName, LightmapIndex, bGenerateLightmapUVs"));

	AddTool(TEXT("MCP_CreateMaterialInstanceForStaticMeshWithLODs"),
		TEXT("为 StaticMesh 的每个 LOD 创建材质实例"),
		TEXT("StaticMeshPath, PackagePath, AssetName, ParentMaterialPath, OriginalTexturesJson"));

	AddTool(TEXT("MCP_FindAndFillOriginalTextures"),
		TEXT("根据 SkeletalMesh 的材质槽自动查找并填充原始纹理"),
		TEXT("SkeletalMeshPath, SearchPath, AssetName"));

	AddTool(TEXT("MCP_FindAndFillAnimSequences"),
		TEXT("根据 SkeletalMesh 的骨架自动查找并分类动画序列"),
		TEXT("SkeletalMeshPath, SearchPath, FileName"));

	AddTool(TEXT("MCP_FindAndFillLODSettings"),
		TEXT("根据 SkeletalMesh 的 LOD 信息自动填充 LOD 设置"),
		TEXT("SkeletalMeshPath"));

	AddTool(TEXT("MCP_CreateAnimDataTexture"),
		TEXT("创建存储动画帧数据的 Texture2D (40x1 RGBA16F)"),
		TEXT("AnimsInfoJson, PackagePath, AssetName"));

	AddTool(TEXT("MCP_CreateAnimsDataFromSequences"),
		TEXT("使用 VAT DataAsset 将动画序列转换为 FAnimsData"),
		TEXT("DataAssetPath, FoundAnimsJson"));

	AddTool(TEXT("MCP_ConvertLODSettingsToLODsData"),
		TEXT("将 LOD 编辑器设置转换为运行时 LODs 数据"),
		TEXT("LODSettingsJson"));

	AddTool(TEXT("MCP_ValidateAnimSequences"),
		TEXT("验证动画序列 (截断超过5个的分类，检查总帧数)"),
		TEXT("FoundAnimsJson"));

	AddTool(TEXT("MCP_RenameSkeletalMesh"),
		TEXT("重命名 SkeletalMesh 资产"),
		TEXT("SkeletalMeshPath, OriginalTexturesJson, AssetName"));

	AddTool(TEXT("MCP_RenameOriginalTextures"),
		TEXT("重命名原始纹理资产"),
		TEXT("OriginalTexturesJson, AssetName"));

	AddTool(TEXT("MCP_RenameAnimSequences"),
		TEXT("重命名动画序列资产"),
		TEXT("FoundAnimsJson, AssetName"));

	AddTool(TEXT("MCP_SanitizeForPath"),
		TEXT("清理字符串中不允许用于路径的字符"),
		TEXT("InString"));

	AddTool(TEXT("MCP_UnitList"),
		TEXT("返回 MassBattle 单位 JSON；默认 simple、自动省略默认/运行时/系统字段"),
		TEXT("OptionsJson"),
		TEXT("unit.query"));

	AddTool(TEXT("MCP_UnitGet"),
		TEXT("按源码属性名返回指定单位 JSON；object/detail/include_defaults 可显式请求复杂对象或完整信息"),
		TEXT("UnitPath, OptionsJson"),
		TEXT("unit.query"));

	AddTool(TEXT("MCP_UnitGetSchema"),
		TEXT("返回 UMassBattleAgentConfigDataAsset 可编辑字段 Schema 和默认忽略策略"),
		TEXT("OptionsJson"),
		TEXT("unit.schema"));

	AddTool(TEXT("MCP_UnitExport"),
		TEXT("导出单位平衡/查询表到 JSON 或 CSV"),
		TEXT("OptionsJson"),
		TEXT("unit.export"));

	AddTool(TEXT("MCP_UnitMergeUpdate"),
		TEXT("对已有单位并集写入局部 JSON，可选择保存资产"),
		TEXT("UnitPath, UnitDataJson, bSaveAssets"),
		TEXT("unit.write"));

	AddTool(TEXT("MCP_UnitCreate"),
		TEXT("创建新单位；未指定模板时使用默认单位模板，可带初始单位数据"),
		TEXT("CreateSpecJson, bSaveAssets"),
		TEXT("unit.create"));

	AddTool(TEXT("MCP_UnitDeleteSoft"),
		TEXT("软删除单位：移动到 Trash 路径并返回引用者"),
		TEXT("UnitPath, OptionsJson"),
		TEXT("unit.delete"));

	AddTool(TEXT("MCP_UnitDelete"),
		TEXT("显式删除单位；默认 dry_run=true"),
		TEXT("UnitPath, OptionsJson"),
		TEXT("unit.delete"));

	AddTool(TEXT("MCP_UnitFindAssets"),
		TEXT("检索可用于生成单位的 Mesh、Niagara、AnimToTexture 和现有单位资产"),
		TEXT("QueryJson"),
		TEXT("unit.generate"));

	AddTool(TEXT("MCP_NiagaraQuery"),
		TEXT("按路径/名称检索 Niagara System 资产，作为特效参考入口"),
		TEXT("QueryJson"),
		TEXT("niagara.query"));

	AddTool(TEXT("MCP_NiagaraReadSummary"),
		TEXT("读取 Niagara System 的系统、Emitter、Renderer、用户参数和模块摘要"),
		TEXT("SystemPath, OptionsJson"),
		TEXT("niagara.read"));

	AddTool(TEXT("MCP_NiagaraReadModule"),
		TEXT("精读一个 Niagara FunctionCall 模块节点及其 Pins"),
		TEXT("SystemPath, SelectorJson"),
		TEXT("niagara.read"));

	AddTool(TEXT("MCP_NiagaraReadAll"),
		TEXT("读取 Niagara System 的完整反射属性和全部模块节点"),
		TEXT("SystemPath, OptionsJson"),
		TEXT("niagara.read"));

	AddTool(TEXT("MCP_NiagaraExportText"),
		TEXT("把 Niagara System 转成可读文本并可写入 Saved/MassBattleEditorMCP/NiagaraText"),
		TEXT("SystemPath, OptionsJson"),
		TEXT("niagara.text"));

	AddTool(TEXT("MCP_NiagaraMergeWrite"),
		TEXT("对 Niagara System/EmitterData/Renderer 做并集属性写入，不负责删除"),
		TEXT("SystemPath, PatchJson, bSaveAssets"),
		TEXT("niagara.write"));

	AddTool(TEXT("MCP_NiagaraSetModulePin"),
		TEXT("写一个 Niagara FunctionCall 模块输入 Pin 的默认值；默认拒绝已连接 Pin"),
		TEXT("SystemPath, SelectorJson, PinName, ValueText, bSaveAssets"),
		TEXT("niagara.write"));

	AddTool(TEXT("MCP_NiagaraDelete"),
		TEXT("显式删除 Niagara 目标：renderer、user_parameter，或禁用 emitter"),
		TEXT("SystemPath, DeleteJson, bSaveAssets"),
		TEXT("niagara.delete"));

	AddTool(TEXT("MCP_EffectAssetQuery"),
		TEXT("检索未知 Marketplace 特效相关资产，可按 Niagara/Cascade/Material/Blueprint 等类型过滤"),
		TEXT("QueryJson"),
		TEXT("effect_asset.query"));

	AddTool(TEXT("MCP_EffectAssetReadSummary"),
		TEXT("读取未知特效资产摘要；包含依赖、引用和缺失项目依赖警告"),
		TEXT("AssetPath, OptionsJson"),
		TEXT("effect_asset.read"));

	AddTool(TEXT("MCP_EffectAssetExportText"),
		TEXT("把未知特效资产摘要和反射属性导出为可读文本"),
		TEXT("AssetPath, OptionsJson"),
		TEXT("effect_asset.text"));

	AddTool(TEXT("MCP_EffectAssetSoftDelete"),
		TEXT("检查引用后规划软移动到 _Trash；默认阻止运行态危险移动，需 allow_unsafe_asset_move=true 才执行"),
		TEXT("AssetPath, OptionsJson"),
		TEXT("effect_asset.lifecycle"));

	AddTool(TEXT("MCP_EffectDuplicateAsset"),
		TEXT("复制任意特效相关资产到目标目录"),
		TEXT("SourceAssetPath, NewAssetName, PackagePath, bSaveAssets"),
		TEXT("effect_asset.write"));

	AddTool(TEXT("MCP_BatchFxSetRendererDefaults"),
		TEXT("设置 MassBattleFxRenderer 蓝图默认属性：Niagara、NDC_BurstFx、SubType、批大小和池化冷却"),
		TEXT("TargetClassPath, NiagaraSystemPath, NdcBurstFxPath, SubType, RenderBatchSize, PoolingCooldown, bSaveAssets"),
		TEXT("batch_fx.write"));

	AddTool(TEXT("MCP_StyleSummarizeUnits"),
		TEXT("按 StyleType、路径类别和推断风格族汇总单位"),
		TEXT("OptionsJson"),
		TEXT("style.organization"));

	AddTool(TEXT("MCP_StylePlanOrganizeUnits"),
		TEXT("生成按风格组织单位资产的移动计划，不直接移动资产"),
		TEXT("OptionsJson"),
		TEXT("style.organization"));

	AddTool(TEXT("MCP_EditorListProfiles"),
		TEXT("列出 MCP 单位编辑器使用的 style profile 与 authoring recipe"),
		TEXT("OptionsJson"),
		TEXT("unit_editor.profile"));

	AddTool(TEXT("MCP_EditorGetProfile"),
		TEXT("读取一个单位管理 style 或单位创作/编辑 recipe"),
		TEXT("ProfileType, ProfileId"),
		TEXT("unit_editor.profile"));

	AddTool(TEXT("MCP_EditorPlanUnitAuthoringWorkflow"),
		TEXT("把购买素材准备、动画更新、VAT 创建/刷新和关联资产整理串成一个可审查工作流计划"),
		TEXT("SpecJson"),
		TEXT("unit_editor.workflow"));

	AddTool(TEXT("MCP_EditorApplyUnitAuthoringWorkflow"),
		TEXT("应用已审查的单位创作工作流；SpecJson 默认 dry_run=true"),
		TEXT("SpecJson, bSaveAssets"),
		TEXT("unit_editor.workflow"));

	AddTool(TEXT("MCP_EditorPlanPreparePurchasedAsset"),
		TEXT("为新购买的 SkeletalMesh 素材包生成发现、官方命名和源资产归档计划"),
		TEXT("SpecJson"),
		TEXT("unit_editor.prepare"));

	AddTool(TEXT("MCP_EditorApplyPreparePurchasedAsset"),
		TEXT("应用已审查的购买素材准备计划；SpecJson 默认 dry_run=true"),
		TEXT("SpecJson, bSaveAssets"),
		TEXT("unit_editor.prepare"));

	AddTool(TEXT("MCP_EditorDiscoverCompatibleAnimations"),
		TEXT("按显式路径和 style 默认候选根检索与目标 SkeletalMesh 兼容的 AnimSequence"),
		TEXT("SkeletalMeshPath, OptionsJson"),
		TEXT("unit_editor.animation"));

	AddTool(TEXT("MCP_EditorPlanAddAnimationsToUnit"),
		TEXT("调用 MassBattleEditor 动画检索与 AnimData 转换函数，为已有单位生成 AnimShared 并集写计划"),
		TEXT("UnitPath, SpecJson"),
		TEXT("unit_editor.animation"));

	AddTool(TEXT("MCP_EditorValidateAddAnimationsToUnit"),
		TEXT("验证已有单位动画组编辑是否能生成可应用的并集写计划"),
		TEXT("UnitPath, SpecJson"),
		TEXT("unit_editor.animation"));

	AddTool(TEXT("MCP_EditorApplyAddAnimationsToUnit"),
		TEXT("生成并在可应用时执行已有单位 AnimShared 编辑计划"),
		TEXT("UnitPath, SpecJson, bSaveAssets"),
		TEXT("unit_editor.animation"));

	AddTool(TEXT("MCP_EditorPlanCreateVatUnit"),
		TEXT("诊断入口：预览与 MassBattleTools DoAll 等价的 VAT 单位生成计划；严格执行仍要求完整 canonical 输入"),
		TEXT("SpecJson"),
		TEXT("unit_editor.create.diagnostic"));

	AddTool(TEXT("MCP_EditorValidateCreateVatUnit"),
		TEXT("诊断入口：验证 DoAll 等价 VAT 单位生成输入，不写资产"),
		TEXT("SpecJson"),
		TEXT("unit_editor.create.diagnostic"));

	AddTool(TEXT("MCP_EditorApplyCreateVatUnit"),
		TEXT("主入口：执行非 selection 的 MassBattleTools DoAll 等价 VAT 单位生成；要求完整 canonical 输入并在写资产前验证"),
		TEXT("SpecJson, bSaveAssets"),
		TEXT("unit_editor.create"));

	AddTool(TEXT("MCP_EditorPlanCreateVatUnitFromSelection"),
		TEXT("诊断入口：从当前编辑器选择或 selected_assets 推导 DoAll spec 并返回可审查计划"),
		TEXT("OptionsJson"),
		TEXT("unit_editor.create.diagnostic"));

	AddTool(TEXT("MCP_EditorApplyCreateVatUnitFromSelection"),
		TEXT("主入口：从当前编辑器选择或 selected_assets 一键执行 MassBattleTools DoAll 等价生成流程"),
		TEXT("OptionsJson, bSaveAssets"),
		TEXT("unit_editor.create"));

	AddTool(TEXT("MCP_EditorPlanOrganizeUnitAssets"),
		TEXT("按单位管理 style 规划移动单位及其编辑器生成的关联资产"),
		TEXT("UnitPath, OptionsJson"),
		TEXT("unit_editor.organize"));

	AddTool(TEXT("MCP_EditorApplyOrganizeUnitAssets"),
		TEXT("应用已审查的单位关联资产整理计划；OptionsJson 默认 dry_run=true"),
		TEXT("UnitPath, OptionsJson, bSaveAssets"),
		TEXT("unit_editor.organize"));

	Root->SetArrayField(TEXT("tools"), Tools);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	return OutputString;
}
