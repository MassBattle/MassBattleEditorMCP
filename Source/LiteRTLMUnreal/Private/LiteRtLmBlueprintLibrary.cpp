// Copyright (c) 2025-2026 Winyunq. All rights reserved.
#include "LiteRtLmBlueprintLibrary.h"

#include "Misc/Paths.h"

FLiteRtLmConfig ULiteRtLmBlueprintLibrary::GetLiteRtLmAutoConfig()
{
    return FLiteRtLmUnrealApi::GetAutoConfig();
}

bool ULiteRtLmBlueprintLibrary::LoadLiteRtLmModel(const FLiteRtLmConfig& Config)
{
    return FLiteRtLmUnrealApi::LoadModel(Config);
}

bool ULiteRtLmBlueprintLibrary::LoadDefaultGemma4E4BModel()
{
    FLiteRtLmConfig Config = FLiteRtLmUnrealApi::GetAutoConfig();
    Config.ModelPath = FPaths::ProjectContentDir() / TEXT("Models/gemma-4-E4B-it.litertlm");
    return FLiteRtLmUnrealApi::LoadModel(Config);
}

void ULiteRtLmBlueprintLibrary::UnloadLiteRtLmModel()
{
    FLiteRtLmUnrealApi::UnloadModel();
}

bool ULiteRtLmBlueprintLibrary::IsLiteRtLmModelLoaded()
{
    return FLiteRtLmUnrealApi::IsModelLoaded();
}
