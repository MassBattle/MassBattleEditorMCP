#include "MassBattleMCPServerRunnable.h"

#include "MassBattleEditorMCP.h"
#include "MassBattleMCPBridge.h"
#include "SocketSubsystem.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	bool SendAll(FSocket* Socket, const uint8* Data, int32 TotalBytes)
	{
		if (!Socket || !Data || TotalBytes < 0)
		{
			return false;
		}

		int32 TotalSent = 0;
		const FDateTime Deadline = FDateTime::UtcNow() + FTimespan::FromSeconds(30.0);
		while (TotalSent < TotalBytes)
		{
			int32 BytesSent = 0;
			const bool bSent = Socket->Send(Data + TotalSent, TotalBytes - TotalSent, BytesSent);
			if (bSent && BytesSent > 0)
			{
				TotalSent += BytesSent;
				continue;
			}
			if (!bSent)
			{
				return false;
			}

			if (FDateTime::UtcNow() > Deadline)
			{
				return false;
			}
			FPlatformProcess::Sleep(0.001f);
		}
		return true;
	}

	void SendResponse(FSocket* Socket, const FString& Response)
	{
		if (!Socket)
		{
			return;
		}

		const FTCHARToUTF8 Utf8Response(*Response);
		if (Utf8Response.Length() > 0)
		{
			SendAll(Socket, reinterpret_cast<const uint8*>(Utf8Response.Get()), Utf8Response.Length());
		}

		uint8 Delimiter = 0;
		SendAll(Socket, &Delimiter, 1);
	}

	FString MakeServerErrorJson(const FString& Error)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetBoolField(TEXT("success"), false);
		Root->SetStringField(TEXT("error"), Error);

		FString Output;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
		FJsonSerializer::Serialize(Root, Writer);
		return Output;
	}
}

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

	// Accepted sockets can report a zero-byte non-blocking read before the client's first
	// payload arrives on Windows. Wait for readability and then use blocking reads so a
	// valid MCP request is not mistaken for an orderly disconnect.
	InClientSocket->SetNonBlocking(false);

	const int32 MaxBufferSize = 4096;
	uint8 Buffer[MaxBufferSize];
	TArray<uint8> PendingData;
	const FDateTime Deadline = FDateTime::UtcNow() + FTimespan::FromSeconds(5.0);

	while (bRunning && InClientSocket)
	{
		const FTimespan Remaining = Deadline - FDateTime::UtcNow();
		if (Remaining <= FTimespan::Zero()
			|| !InClientSocket->Wait(ESocketWaitConditions::WaitForRead, Remaining))
		{
			break;
		}

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
			break;
		}
		else if (BytesRead == 0)
		{
			break;
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
		SendResponse(InClientSocket, MakeServerErrorJson(TEXT("Request was not valid JSON.")));
		return;
	}

	FString CommandType;
	if (!JsonMessage->TryGetStringField(TEXT("command"), CommandType))
	{
		SendResponse(InClientSocket, MakeServerErrorJson(TEXT("Request JSON must contain a command string.")));
		return;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	const TSharedPtr<FJsonValue> ParamsValue = JsonMessage->TryGetField(TEXT("params"));
	if (ParamsValue.IsValid() && ParamsValue->Type == EJson::Object)
	{
		Params = ParamsValue->AsObject();
	}

	const FString Response = Bridge ? Bridge->ExecuteCommand(CommandType, Params) : TEXT("{\"success\":false,\"error\":\"MassBattle MCP bridge is unavailable\"}");
	SendResponse(InClientSocket, Response);
}
