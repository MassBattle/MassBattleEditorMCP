// Copyright (c) 2025-2026 Winyunq. All rights reserved.
#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "MassBattleEditorMCPCommandlet.generated.h"

/**
 * Commandlet entrypoint for invoking MassBattleEditorMCP tools in a fresh editor-cmd process.
 *
 * Usage:
 * UnrealEditor-Cmd.exe Project.uproject -run=MassBattleEditorMCP -InputFile=... -OutputFile=...
 */
UCLASS()
class MASSBATTLEEDITORMCP_API UMassBattleEditorMCPCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UMassBattleEditorMCPCommandlet();

	virtual int32 Main(const FString& Params) override;
};
