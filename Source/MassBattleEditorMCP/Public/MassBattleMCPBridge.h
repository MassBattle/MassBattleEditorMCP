#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Sockets.h"
#include "MassBattleMCPBridge.generated.h"

class FMassBattleMCPServerRunnable;
class FJsonObject;

UCLASS()
class MASSBATTLEEDITORMCP_API UMassBattleMCPBridge : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	FString ExecuteCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
	FString InternalExecuteCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

	void StartServer();
	void StopServer();

	bool bIsRunning = false;
	FSocket* ListenerSocket = nullptr;
	FRunnableThread* ServerThread = nullptr;
	FMassBattleMCPServerRunnable* ServerRunnable = nullptr;
	FIPv4Address ServerAddress;
	int32 Port = 55258;

	static bool bGlobalServerStarted;
};
