#include "MassBattleMCPBridge.h"

#include "Async/Async.h"
#include "BatchEffects/MassBattleEffectAssetMCPApi.h"
#include "BatchEffects/MassBattleNiagaraMCPApi.h"
#include "Dom/JsonObject.h"
#include "HAL/RunnableThread.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "MassBattleEditorMCP.h"
#include "MassBattleEditorMCPApi.h"
#include "MassBattleMCPServerRunnable.h"
#include "MassBattleUnitEditorMCPApi.h"
#include "MassBattleUnitMCPApi.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SocketSubsystem.h"

bool UMassBattleMCPBridge::bGlobalServerStarted = false;

namespace
{
	FString ErrorJson(const FString& Error)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetBoolField(TEXT("success"), false);
		Root->SetStringField(TEXT("error"), Error);

		FString Output;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
		FJsonSerializer::Serialize(Root, Writer);
		return Output;
	}

	FString StringParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Name, const FString& DefaultValue = TEXT(""))
	{
		FString Value;
		if (Params.IsValid() && Params->TryGetStringField(Name, Value))
		{
			return Value;
		}
		return DefaultValue;
	}

	bool BoolParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Name, bool bDefaultValue = false)
	{
		bool bValue = bDefaultValue;
		if (Params.IsValid())
		{
			Params->TryGetBoolField(Name, bValue);
		}
		return bValue;
	}

	int32 IntParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Name, int32 DefaultValue = 0)
	{
		int32 Value = DefaultValue;
		if (Params.IsValid())
		{
			Params->TryGetNumberField(Name, Value);
		}
		return Value;
	}

	float FloatParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Name, float DefaultValue = 0.0f)
	{
		double Value = DefaultValue;
		if (Params.IsValid())
		{
			Params->TryGetNumberField(Name, Value);
		}
		return static_cast<float>(Value);
	}

	FString JsonParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Name, const FString& DefaultValue = TEXT("{}"))
	{
		if (!Params.IsValid())
		{
			return DefaultValue;
		}

		FString StringValue;
		if (Params->TryGetStringField(Name, StringValue))
		{
			return StringValue.IsEmpty() ? DefaultValue : StringValue;
		}

		const TSharedPtr<FJsonObject>* ObjectValue = nullptr;
		if (Params->TryGetObjectField(Name, ObjectValue) && ObjectValue && ObjectValue->IsValid())
		{
			FString Output;
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
			FJsonSerializer::Serialize(ObjectValue->ToSharedRef(), Writer);
			return Output;
		}

		return DefaultValue;
	}
}

void UMassBattleMCPBridge::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	StartServer();
}

void UMassBattleMCPBridge::Deinitialize()
{
	StopServer();
	Super::Deinitialize();
}

void UMassBattleMCPBridge::StartServer()
{
	if (bIsRunning || bGlobalServerStarted)
	{
		return;
	}

	if (!FIPv4Address::Parse(TEXT("127.0.0.1"), ServerAddress))
	{
		UE_LOG(LogMassBattleEditorMCP, Error, TEXT("MassBattle MCP bridge failed to parse server address."));
		return;
	}

	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem)
	{
		UE_LOG(LogMassBattleEditorMCP, Error, TEXT("MassBattle MCP bridge failed to get socket subsystem."));
		return;
	}

	ListenerSocket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("MassBattleMCPListener"), false);
	if (!ListenerSocket)
	{
		UE_LOG(LogMassBattleEditorMCP, Error, TEXT("MassBattle MCP bridge failed to create listener socket."));
		return;
	}

	ListenerSocket->SetReuseAddr(true);
	ListenerSocket->SetNonBlocking(true);

	const FIPv4Endpoint Endpoint(ServerAddress, Port);
	if (!ListenerSocket->Bind(*Endpoint.ToInternetAddr()) || !ListenerSocket->Listen(8))
	{
		UE_LOG(LogMassBattleEditorMCP, Error, TEXT("MassBattle MCP bridge failed to listen on %s:%d."), *ServerAddress.ToString(), Port);
		SocketSubsystem->DestroySocket(ListenerSocket);
		ListenerSocket = nullptr;
		return;
	}

	ServerRunnable = new FMassBattleMCPServerRunnable(this, ListenerSocket);
	ServerThread = FRunnableThread::Create(ServerRunnable, TEXT("MassBattleMCPServerThread"), 0, TPri_Normal);
	if (!ServerThread)
	{
		delete ServerRunnable;
		ServerRunnable = nullptr;
		SocketSubsystem->DestroySocket(ListenerSocket);
		ListenerSocket = nullptr;
		return;
	}

	bIsRunning = true;
	bGlobalServerStarted = true;
	UE_LOG(LogMassBattleEditorMCP, Display, TEXT("MassBattle MCP bridge listening on %s:%d."), *ServerAddress.ToString(), Port);
}

void UMassBattleMCPBridge::StopServer()
{
	if (!bIsRunning && !ServerThread && !ListenerSocket)
	{
		return;
	}

	bIsRunning = false;
	bGlobalServerStarted = false;

	if (ServerRunnable)
	{
		ServerRunnable->Stop();
	}

	if (ServerThread)
	{
		ServerThread->WaitForCompletion();
		delete ServerThread;
		ServerThread = nullptr;
	}

	delete ServerRunnable;
	ServerRunnable = nullptr;

	if (ListenerSocket)
	{
		if (ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
		{
			SocketSubsystem->DestroySocket(ListenerSocket);
		}
		ListenerSocket = nullptr;
	}
}

FString UMassBattleMCPBridge::ExecuteCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	if (IsInGameThread())
	{
		return InternalExecuteCommand(CommandType, Params);
	}

	TPromise<FString> Promise;
	TFuture<FString> Future = Promise.GetFuture();
	AsyncTask(ENamedThreads::GameThread, [this, CommandType, Params, Promise = MoveTemp(Promise)]() mutable
	{
		Promise.SetValue(InternalExecuteCommand(CommandType, Params));
	});

	if (Future.WaitFor(FTimespan::FromSeconds(120.0)))
	{
		return Future.Get();
	}

	return ErrorJson(FString::Printf(TEXT("Timed out executing command on the game thread: %s"), *CommandType));
}

FString UMassBattleMCPBridge::InternalExecuteCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	if (CommandType == TEXT("ping"))
	{
		return TEXT("{\"success\":true,\"message\":\"pong\"}");
	}

	if (CommandType == TEXT("MCP_UnitGetApiStatus")) { return UMassBattleUnitMCPApi::MCP_UnitGetApiStatus(); }
	if (CommandType == TEXT("MCP_UnitList")) { return UMassBattleUnitMCPApi::MCP_UnitList(JsonParam(Params, TEXT("OptionsJson"))); }
	if (CommandType == TEXT("MCP_UnitGet")) { return UMassBattleUnitMCPApi::MCP_UnitGet(StringParam(Params, TEXT("UnitPath")), JsonParam(Params, TEXT("OptionsJson"))); }
	if (CommandType == TEXT("MCP_UnitGetSchema")) { return UMassBattleUnitMCPApi::MCP_UnitGetSchema(JsonParam(Params, TEXT("OptionsJson"))); }
	if (CommandType == TEXT("MCP_UnitExport")) { return UMassBattleUnitMCPApi::MCP_UnitExport(JsonParam(Params, TEXT("OptionsJson"))); }
	if (CommandType == TEXT("MCP_UnitMergeUpdate")) { return UMassBattleUnitMCPApi::MCP_UnitMergeUpdate(StringParam(Params, TEXT("UnitPath")), JsonParam(Params, TEXT("UnitDataJson")), BoolParam(Params, TEXT("bSaveAssets"))); }
	if (CommandType == TEXT("MCP_UnitCreate")) { return UMassBattleUnitMCPApi::MCP_UnitCreate(JsonParam(Params, TEXT("CreateSpecJson")), BoolParam(Params, TEXT("bSaveAssets"), true)); }
	if (CommandType == TEXT("MCP_UnitFindAssets")) { return UMassBattleUnitMCPApi::MCP_UnitFindAssets(JsonParam(Params, TEXT("QueryJson"))); }
	if (CommandType == TEXT("MCP_UnitDeleteSoft")) { return UMassBattleUnitMCPApi::MCP_UnitDeleteSoft(StringParam(Params, TEXT("UnitPath")), JsonParam(Params, TEXT("OptionsJson"))); }
	if (CommandType == TEXT("MCP_UnitDelete")) { return UMassBattleUnitMCPApi::MCP_UnitDelete(StringParam(Params, TEXT("UnitPath")), JsonParam(Params, TEXT("OptionsJson"))); }
	if (CommandType == TEXT("MCP_StyleSummarizeUnits")) { return UMassBattleStyleMCPApi::MCP_StyleSummarizeUnits(JsonParam(Params, TEXT("OptionsJson"))); }
	if (CommandType == TEXT("MCP_StylePlanOrganizeUnits")) { return UMassBattleStyleMCPApi::MCP_StylePlanOrganizeUnits(JsonParam(Params, TEXT("OptionsJson"))); }

	if (CommandType == TEXT("MCP_EditorGetStatus")) { return UMassBattleUnitEditorMCPApi::MCP_EditorGetStatus(); }
	if (CommandType == TEXT("MCP_EditorListProfiles")) { return UMassBattleUnitEditorMCPApi::MCP_EditorListProfiles(JsonParam(Params, TEXT("OptionsJson"))); }
	if (CommandType == TEXT("MCP_EditorGetProfile")) { return UMassBattleUnitEditorMCPApi::MCP_EditorGetProfile(StringParam(Params, TEXT("ProfileType")), StringParam(Params, TEXT("ProfileId"))); }
	if (CommandType == TEXT("MCP_EditorPlanCreateVatUnit")) { return UMassBattleUnitEditorMCPApi::MCP_EditorPlanCreateVatUnit(JsonParam(Params, TEXT("SpecJson"))); }
	if (CommandType == TEXT("MCP_EditorValidateCreateVatUnit")) { return UMassBattleUnitEditorMCPApi::MCP_EditorValidateCreateVatUnit(JsonParam(Params, TEXT("SpecJson"))); }
	if (CommandType == TEXT("MCP_EditorApplyCreateVatUnit")) { return UMassBattleUnitEditorMCPApi::MCP_EditorApplyCreateVatUnit(JsonParam(Params, TEXT("SpecJson")), BoolParam(Params, TEXT("bSaveAssets"))); }
	if (CommandType == TEXT("MCP_EditorPlanCreateVatUnitFromSelection")) { return UMassBattleUnitEditorMCPApi::MCP_EditorPlanCreateVatUnitFromSelection(JsonParam(Params, TEXT("OptionsJson"))); }
	if (CommandType == TEXT("MCP_EditorApplyCreateVatUnitFromSelection")) { return UMassBattleUnitEditorMCPApi::MCP_EditorApplyCreateVatUnitFromSelection(JsonParam(Params, TEXT("OptionsJson")), BoolParam(Params, TEXT("bSaveAssets"), true)); }
	if (CommandType == TEXT("MCP_EditorPlanAddAnimationsToUnit")) { return UMassBattleUnitEditorMCPApi::MCP_EditorPlanAddAnimationsToUnit(StringParam(Params, TEXT("UnitPath")), JsonParam(Params, TEXT("SpecJson"))); }
	if (CommandType == TEXT("MCP_EditorValidateAddAnimationsToUnit")) { return UMassBattleUnitEditorMCPApi::MCP_EditorValidateAddAnimationsToUnit(StringParam(Params, TEXT("UnitPath")), JsonParam(Params, TEXT("SpecJson"))); }
	if (CommandType == TEXT("MCP_EditorApplyAddAnimationsToUnit")) { return UMassBattleUnitEditorMCPApi::MCP_EditorApplyAddAnimationsToUnit(StringParam(Params, TEXT("UnitPath")), JsonParam(Params, TEXT("SpecJson")), BoolParam(Params, TEXT("bSaveAssets"))); }
	if (CommandType == TEXT("MCP_EditorPlanOrganizeUnitAssets")) { return UMassBattleUnitEditorMCPApi::MCP_EditorPlanOrganizeUnitAssets(StringParam(Params, TEXT("UnitPath")), JsonParam(Params, TEXT("OptionsJson"))); }
	if (CommandType == TEXT("MCP_EditorApplyOrganizeUnitAssets")) { return UMassBattleUnitEditorMCPApi::MCP_EditorApplyOrganizeUnitAssets(StringParam(Params, TEXT("UnitPath")), JsonParam(Params, TEXT("OptionsJson")), BoolParam(Params, TEXT("bSaveAssets"), true)); }

	if (CommandType == TEXT("MCP_EffectAssetGetApiStatus")) { return UMassBattleEffectAssetMCPApi::MCP_EffectAssetGetApiStatus(); }
	if (CommandType == TEXT("MCP_EffectAssetQuery")) { return UMassBattleEffectAssetMCPApi::MCP_EffectAssetQuery(JsonParam(Params, TEXT("QueryJson"))); }
	if (CommandType == TEXT("MCP_EffectAssetReadSummary")) { return UMassBattleEffectAssetMCPApi::MCP_EffectAssetReadSummary(StringParam(Params, TEXT("AssetPath")), JsonParam(Params, TEXT("OptionsJson"))); }
	if (CommandType == TEXT("MCP_EffectAssetExportText")) { return UMassBattleEffectAssetMCPApi::MCP_EffectAssetExportText(StringParam(Params, TEXT("AssetPath")), JsonParam(Params, TEXT("OptionsJson"))); }
	if (CommandType == TEXT("MCP_EffectAssetSoftDelete")) { return UMassBattleEffectAssetMCPApi::MCP_EffectAssetSoftDelete(StringParam(Params, TEXT("AssetPath")), JsonParam(Params, TEXT("OptionsJson"))); }
	if (CommandType == TEXT("MCP_EffectDuplicateAsset")) { return UMassBattleEffectAssetMCPApi::MCP_EffectDuplicateAsset(StringParam(Params, TEXT("SourceAssetPath")), StringParam(Params, TEXT("NewAssetName")), StringParam(Params, TEXT("PackagePath")), BoolParam(Params, TEXT("bSaveAssets"))); }
	if (CommandType == TEXT("MCP_BatchFxReadRendererDefaults")) { return UMassBattleEffectAssetMCPApi::MCP_BatchFxReadRendererDefaults(StringParam(Params, TEXT("TargetClassPath"))); }
	if (CommandType == TEXT("MCP_BatchFxSetRendererDefaults")) { return UMassBattleEffectAssetMCPApi::MCP_BatchFxSetRendererDefaults(StringParam(Params, TEXT("TargetClassPath")), StringParam(Params, TEXT("NiagaraSystemPath")), StringParam(Params, TEXT("NdcBurstFxPath")), IntParam(Params, TEXT("SubType")), IntParam(Params, TEXT("RenderBatchSize")), FloatParam(Params, TEXT("PoolingCooldown")), BoolParam(Params, TEXT("bSaveAssets"))); }

	if (CommandType == TEXT("MCP_NiagaraGetApiStatus")) { return UMassBattleNiagaraMCPApi::MCP_NiagaraGetApiStatus(); }
	if (CommandType == TEXT("MCP_NiagaraQuery")) { return UMassBattleNiagaraMCPApi::MCP_NiagaraQuery(JsonParam(Params, TEXT("QueryJson"))); }
	if (CommandType == TEXT("MCP_NiagaraReadSummary")) { return UMassBattleNiagaraMCPApi::MCP_NiagaraReadSummary(StringParam(Params, TEXT("SystemPath")), JsonParam(Params, TEXT("OptionsJson"))); }
	if (CommandType == TEXT("MCP_NiagaraReadModule")) { return UMassBattleNiagaraMCPApi::MCP_NiagaraReadModule(StringParam(Params, TEXT("SystemPath")), JsonParam(Params, TEXT("SelectorJson"))); }
	if (CommandType == TEXT("MCP_NiagaraReadAll")) { return UMassBattleNiagaraMCPApi::MCP_NiagaraReadAll(StringParam(Params, TEXT("SystemPath")), JsonParam(Params, TEXT("OptionsJson"))); }
	if (CommandType == TEXT("MCP_NiagaraExportText")) { return UMassBattleNiagaraMCPApi::MCP_NiagaraExportText(StringParam(Params, TEXT("SystemPath")), JsonParam(Params, TEXT("OptionsJson"))); }
	if (CommandType == TEXT("MCP_NiagaraMergeWrite")) { return UMassBattleNiagaraMCPApi::MCP_NiagaraMergeWrite(StringParam(Params, TEXT("SystemPath")), JsonParam(Params, TEXT("PatchJson")), BoolParam(Params, TEXT("bSaveAssets"))); }
	if (CommandType == TEXT("MCP_NiagaraSetModulePin")) { return UMassBattleNiagaraMCPApi::MCP_NiagaraSetModulePin(StringParam(Params, TEXT("SystemPath")), JsonParam(Params, TEXT("SelectorJson")), StringParam(Params, TEXT("PinName")), StringParam(Params, TEXT("ValueText")), BoolParam(Params, TEXT("bSaveAssets"))); }
	if (CommandType == TEXT("MCP_NiagaraSetEmitterEnabled")) { return UMassBattleNiagaraMCPApi::MCP_NiagaraSetEmitterEnabled(StringParam(Params, TEXT("SystemPath")), StringParam(Params, TEXT("EmitterName")), BoolParam(Params, TEXT("bEnabled")), BoolParam(Params, TEXT("bSaveAssets"))); }
	if (CommandType == TEXT("MCP_NiagaraDelete")) { return UMassBattleNiagaraMCPApi::MCP_NiagaraDelete(StringParam(Params, TEXT("SystemPath")), JsonParam(Params, TEXT("DeleteJson")), BoolParam(Params, TEXT("bSaveAssets"))); }

	if (CommandType == TEXT("MCP_DuplicateClassAsset")) { return UMassBattleEditorMCPApi::MCP_DuplicateClassAsset(StringParam(Params, TEXT("SourceClassPath")), StringParam(Params, TEXT("NewClassName")), StringParam(Params, TEXT("PackagePath"))); }
	if (CommandType == TEXT("MCP_SetClassDefaultProperties")) { return UMassBattleEditorMCPApi::MCP_SetClassDefaultProperties(StringParam(Params, TEXT("TargetClassPath")), StringParam(Params, TEXT("AgentMeshPath")), StringParam(Params, TEXT("NiagaraSystemPath")), IntParam(Params, TEXT("SubType"))); }

	return ErrorJson(FString::Printf(TEXT("Unknown MassBattle MCP command: %s"), *CommandType));
}
