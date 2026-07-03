#include "MassBattleMCPServerRunnable.h"

#include "MassBattleEditorMCP.h"
#include "MassBattleMCPBridge.h"
#include "SocketSubsystem.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

FMassBattleMCPServerRunnable::FMassBattleMCPServerRunnable(UMassBattleMCPBridge* InBridge, FSocket* InListenerSocket)
	: Bridge(InBridge)
	, ListenerSocket(InListenerSocket)
{
}

bool FMassBattleMCPServerRunnable::Init()
{
	return Bridge != nullptr && ListenerSocket != nullptr;
}

uint32 FMassBattleMCPServerRunnable::Run()
{
	while (bRunning)
	{
		bool bPending = false;
		if (ListenerSocket && ListenerSocket->HasPendingConnection(bPending) && bPending)
		{
			ClientSocket = ListenerSocket->Accept(TEXT("MassBattleMCPClient"));
			if (ClientSocket)
			{
				HandleClientConnection(ClientSocket);
				if (ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
				{
					SocketSubsystem->DestroySocket(ClientSocket);
				}
				ClientSocket = nullptr;
			}
		}

		FPlatformProcess::Sleep(0.05f);
	}

	return 0;
}

void FMassBattleMCPServerRunnable::Stop()
{
	bRunning = false;
}

void FMassBattleMCPServerRunnable::Exit()
{
}

void FMassBattleMCPServerRunnable::HandleClientConnection(FSocket* InClientSocket)
{
	if (!InClientSocket)
	{
		return;
	}

	InClientSocket->SetNonBlocking(true);

	const int32 MaxBufferSize = 4096;
	uint8 Buffer[MaxBufferSize];
	TArray<uint8> PendingData;
	const FDateTime Deadline = FDateTime::UtcNow() + FTimespan::FromSeconds(5.0);

	while (bRunning && InClientSocket)
	{
		int32 BytesRead = 0;
		const bool bReadSuccess = InClientSocket->Recv(Buffer, MaxBufferSize, BytesRead, ESocketReceiveFlags::None);

		if (BytesRead > 0)
		{
			for (int32 Index = 0; Index < BytesRead; ++Index)
			{
				if (Buffer[Index] == 0)
				{
					if (PendingData.Num() > 0)
					{
						PendingData.Add(0);
						const FString Message = UTF8_TO_TCHAR(reinterpret_cast<const char*>(PendingData.GetData()));
						ProcessMessage(InClientSocket, Message);
						PendingData.Empty();
					}
					return;
				}

				PendingData.Add(Buffer[Index]);
			}
		}
		else if (!bReadSuccess)
		{
			if (FDateTime::UtcNow() > Deadline)
			{
				break;
			}
			FPlatformProcess::Sleep(0.001f);
		}
		else if (BytesRead == 0)
		{
			break;
		}
		else
		{
			FPlatformProcess::Sleep(0.001f);
		}
	}
}

void FMassBattleMCPServerRunnable::ProcessMessage(FSocket* InClientSocket, const FString& Message)
{
	if (!InClientSocket)
	{
		return;
	}

	TSharedPtr<FJsonObject> JsonMessage;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Message);
	if (!FJsonSerializer::Deserialize(Reader, JsonMessage) || !JsonMessage.IsValid())
	{
		return;
	}

	FString CommandType;
	if (!JsonMessage->TryGetStringField(TEXT("command"), CommandType))
	{
		return;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	const TSharedPtr<FJsonValue> ParamsValue = JsonMessage->TryGetField(TEXT("params"));
	if (ParamsValue.IsValid() && ParamsValue->Type == EJson::Object)
	{
		Params = ParamsValue->AsObject();
	}

	const FString Response = Bridge ? Bridge->ExecuteCommand(CommandType, Params) : TEXT("{\"success\":false,\"error\":\"MassBattle MCP bridge is unavailable\"}");

	int32 BytesSent = 0;
	const FTCHARToUTF8 Utf8Response(*Response);
	if (Utf8Response.Length() > 0)
	{
		InClientSocket->Send(reinterpret_cast<const uint8*>(Utf8Response.Get()), Utf8Response.Length(), BytesSent);
	}

	uint8 Delimiter = 0;
	InClientSocket->Send(&Delimiter, 1, BytesSent);
}
