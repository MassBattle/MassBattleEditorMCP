// Copyright (c) 2025-2026 Winyunq. All rights reserved.
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "LiteRtLmUnrealApi.h"
#include "LiteRtLmBlueprintLibrary.generated.h"

/**
 * Blueprint helpers for model lifecycle.
 */
UCLASS()
class LITERTLMUNREAL_API ULiteRtLmBlueprintLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category = "LiteRT|Model")
    static FLiteRtLmConfig GetLiteRtLmAutoConfig();

    UFUNCTION(BlueprintCallable, Category = "LiteRT|Model")
    static bool LoadLiteRtLmModel(const FLiteRtLmConfig& Config);

    UFUNCTION(BlueprintCallable, Category = "LiteRT|Model")
    static bool LoadDefaultGemma4E4BModel();

    UFUNCTION(BlueprintCallable, Category = "LiteRT|Model")
    static void UnloadLiteRtLmModel();

    UFUNCTION(BlueprintPure, Category = "LiteRT|Model")
    static bool IsLiteRtLmModelLoaded();
};
