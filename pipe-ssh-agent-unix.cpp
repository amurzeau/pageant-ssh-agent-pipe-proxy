#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>
#include <stdio.h>
#include <tchar.h>
#include <strsafe.h>
#include <stdint.h>


#define AGENT_MAX_MSGLEN 2621440
#define AGENT_COPYDATA_ID 0x804e50ba

struct thread_data {
	HANDLE hPipe;
	SOCKET sock;
};

DWORD WINAPI InstanceThread(LPVOID lpvData);
SOCKET connect_unix_socket(void);

void print_help(TCHAR *argv[], LPCTSTR lpszPipename) {
	_tprintf( TEXT("Usage: %s [pipe_path]\n\n pipe_path: path to a pipe, defaults to %s\n"),
	         argv[0],
	         lpszPipename);
}

int _tmain(void)
{
	WSADATA wsaData;
	BOOL   fConnected = FALSE;
	DWORD  dwThreadId = 0;
	HANDLE hPipe = INVALID_HANDLE_VALUE, hThread = NULL;
	LPCTSTR pipeRequiredPrefix = TEXT("\\\\.");
	LPCTSTR lpszPipename = TEXT("\\\\.\\pipe\\openssh-ssh-agent");
	HANDLE hHeap      = GetProcessHeap();

	if(__argc == 2) {
		if (_tcsncmp(__targv[1], pipeRequiredPrefix, _tcslen(pipeRequiredPrefix)) != 0) {
			_tprintf( TEXT("Invalid argument %s, must start with %s if present\n"),
			         __targv[1],
			         pipeRequiredPrefix);
			print_help(__targv, lpszPipename);
			return 1;
		}

		lpszPipename = __targv[1];
	} else if(__argc > 2) {
		print_help(__targv, lpszPipename);
		return 1;
	}


	// Initialize Winsock
	WSAStartup(MAKEWORD(2,2), &wsaData);

	// The main loop creates an instance of the named pipe and
	// then waits for a client to connect to it. When the client
	// connects, a thread is created to handle communications
	// with that client, and this loop is free to wait for the
	// next client connect request. It is an infinite loop.

	for (;;)
	{
		_tprintf( TEXT("pageant pipe server: Main thread awaiting client connection on %s\n"), lpszPipename);
		hPipe = CreateNamedPipe(
		    lpszPipename,             // pipe name
		    PIPE_ACCESS_DUPLEX,       // read/write access
		    PIPE_TYPE_BYTE |
		    PIPE_READMODE_BYTE |       // message type pipe
		    PIPE_WAIT,                // blocking mode
		    PIPE_UNLIMITED_INSTANCES, // max. instances
		    AGENT_MAX_MSGLEN,                  // output buffer size
		    AGENT_MAX_MSGLEN,                  // input buffer size
		    0,                        // client time-out
		    NULL);                    // default security attribute

		if (hPipe == INVALID_HANDLE_VALUE)
		{
			_tprintf(TEXT("CreateNamedPipe failed, GLE=%lu.\n"), GetLastError());
			return -1;
		}

		// Wait for the client to connect; if it succeeds,
		// the function returns a nonzero value. If the function
		// returns zero, GetLastError returns ERROR_PIPE_CONNECTED.

		fConnected = ConnectNamedPipe(hPipe, NULL) ?
		                 TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);

		if (!fConnected)
		{
			CloseHandle(hPipe);
			continue;
		}

		printf("Client connected, creating a processing thread.\n");

		SOCKET sock = connect_unix_socket();
		if(sock == INVALID_SOCKET) {
			printf("Error: cannot connect to upstream ssh-agent\n");
			CloseHandle(hPipe);
			continue;
		}

		struct thread_data* data = (struct thread_data*) HeapAlloc(hHeap, 0, sizeof(*data));
		data->hPipe = hPipe;
		data->sock = sock;

		// Create a read thread for this client.
		hThread = CreateThread(
			NULL,              // no security attribute
			0,                 // default stack size
			InstanceThread,    // thread proc
			(LPVOID) data,    // thread parameter
			0,                 // not suspended
			&dwThreadId);      // returns thread ID

		if (hThread == NULL)
		{
			_tprintf(TEXT("CreateThread failed, GLE=%lu.\n"), GetLastError());
			return -1;
		}
		else CloseHandle(hThread);
	}

	return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR pCmdLine, int nCmdShow) {
	(void)hInstance;
	(void)hPrevInstance;
	(void)pCmdLine;
	(void)nCmdShow;

	return _tmain();
}

uint32_t readu32(const void* buffer) {
	const uint8_t* buffer_char = (const uint8_t*) buffer;
	return (buffer_char[0] << 24) |
	       (buffer_char[1] << 16) |
	       (buffer_char[2] << 8) |
	       (buffer_char[3] << 0);
}

DWORD WINAPI InstanceThread(LPVOID lpvData)
// This routine is a thread processing function to read from and reply to a client
// via the open pipe connection passed from the main loop. Note this allows
// the main loop to continue executing, potentially creating more threads of
// of this procedure to run concurrently, depending on the number of incoming
// client connections.
{
	struct thread_data* data = (struct thread_data*) lpvData;
	HANDLE hHeap      = GetProcessHeap();
	char* pchRequest = (char*)HeapAlloc(hHeap, 0, AGENT_MAX_MSGLEN);
	char* pchReply   = (char*)HeapAlloc(hHeap, 0, AGENT_MAX_MSGLEN);

	DWORD cbBytesRead = 0, cbWritten = 0;
	BOOL fSuccess = FALSE;
	HANDLE hPipe  = NULL;
	SOCKET sock;
	int result;

	// Do some extra error checking since the app will keep running even if this
	// thread fails.

	if (data == NULL)
	{
		printf( "\nERROR - Pipe Server Failure:\n");
		printf( "   InstanceThread got an unexpected NULL value in lpvParam.\n");
		printf( "   InstanceThread exitting.\n");
		if (pchReply != NULL) HeapFree(hHeap, 0, pchReply);
		if (pchRequest != NULL) HeapFree(hHeap, 0, pchRequest);
		return (DWORD)-1;
	}

	if (pchRequest == NULL)
	{
		printf( "\nERROR - Pipe Server Failure:\n");
		printf( "   InstanceThread got an unexpected NULL heap allocation.\n");
		printf( "   InstanceThread exitting.\n");
		if (pchReply != NULL) HeapFree(hHeap, 0, pchReply);
		return (DWORD)-1;
	}

	if (pchReply == NULL)
	{
		printf( "\nERROR - Pipe Server Failure:\n");
		printf( "   InstanceThread got an unexpected NULL heap allocation.\n");
		printf( "   InstanceThread exitting.\n");
		if (pchRequest != NULL) HeapFree(hHeap, 0, pchRequest);
		return (DWORD)-1;
	}

	// Print verbose messages. In production code, this should be for debugging only.
	printf("InstanceThread created, receiving and processing messages.\n");

	// The thread's parameter is a handle to a pipe object instance.

	hPipe = data->hPipe;
	sock = data->sock;

	HeapFree(hHeap, 0, data);

	// Loop until done reading
	while (1)
	{
		//if(isReading) {
		    int byteRead = 0;
		    do {
			    // Read client requests from the pipe. This simplistic code only allows messages
			    // up to AGENT_MAX_MSGLEN characters in length.
			    fSuccess = ReadFile(
			        hPipe,        // handle to pipe
			        pchRequest + byteRead,    // buffer to receive data
			        AGENT_MAX_MSGLEN, // size of buffer
			        &cbBytesRead, // number of bytes read
			        NULL);        // not overlapped I/O

			    if (!fSuccess || cbBytesRead == 0)
			    {
				    if (GetLastError() == ERROR_BROKEN_PIPE)
				    {
					    _tprintf(TEXT("InstanceThread: client disconnected.\n"));
				    }
				    else
				    {
					    _tprintf(TEXT("InstanceThread ReadFile failed, GLE=%lu.\n"), GetLastError());
				    }
				    break;
			    } else {
				    byteRead += cbBytesRead;
			    }
		    } while(byteRead < 4 || byteRead < readu32(pchRequest) + 4);

		    if (!fSuccess)
		    {
			    break;
		    }

			printf("Sending %lu bytes to ssh-agent\n", byteRead);
			for(DWORD i = 0; i < byteRead; i++) {
				printf("%02x ", ((const char*) pchRequest)[i]);
			}
			printf("\n");
			result = send(sock, (const char*) pchRequest, byteRead, 0);
		    if(result != (int)byteRead) {
				printf("Failed to send query data to socket : %lu\n", GetLastError());
				break;
			}
		// } else {
			//Write to pipe
		    byteRead = 0;
		    do {
				result = recv(sock, (char*) pchReply + byteRead, AGENT_MAX_MSGLEN, 0);
				if(result < 0) {
					printf("Failed to recv query data to socket : %lu\n", GetLastError());
					break;
				}
			    printf("Read %d + %d bytes from ssh-agent\n", result, byteRead);
			    for(int i = 0; i < result; i++) {
				    printf("%02x ", ((const char*) pchReply + byteRead)[i]);
			    }
			    printf("\n");

			    byteRead += result;

		    } while(byteRead < 4 || byteRead < readu32(pchReply) + 4);

		    if (result < 0)
		    {
			    break;
		    }

			if(byteRead == 0) {
				printf("ssh-agent connection closed\n");
				break;
			}

			// Write the reply to the pipe.
			fSuccess = WriteFile(
				hPipe,        // handle to pipe
				pchReply,     // buffer to write from
				byteRead, // number of bytes to write
				&cbWritten,   // number of bytes written
				NULL);        // not overlapped I/O

			if (!fSuccess || cbWritten == 0)
			{
				_tprintf(TEXT("InstanceThread WriteFile failed, GLE=%lu.\n"), GetLastError());
				break;
			}
		//}
	}

	// Flush the pipe to allow the client to read the pipe's contents
	// before disconnecting. Then disconnect the pipe, and close the
	// handle to this pipe instance.

	closesocket(sock);

	FlushFileBuffers(hPipe);
	DisconnectNamedPipe(hPipe);
	CloseHandle(hPipe);

	HeapFree(hHeap, 0, pchRequest);
	HeapFree(hHeap, 0, pchReply);

	printf("InstanceThread exiting.\n");
	return 1;
}

int recv_full(SOCKET sock, char* buffer, int size, int flags) {
	int result;
	int totalRead = 0;

	do {
		result = recv(sock, buffer + totalRead, size - totalRead, flags);
		if(result > 0)
			totalRead += result;
	} while(totalRead < size && result > 0);

	return result;
}


SOCKET connect_unix_socket(void)
{
	HANDLE fileHandle;
	DWORD lastError;
	TCHAR sshAuthSocket[256];
	char buffer[128];
	int result;
	DWORD bytesRead;
	const char* SOCKET_COOKIE = "!<socket >";

	if(GetEnvironmentVariable(TEXT("SSH_AUTH_SOCK"), sshAuthSocket, sizeof(sshAuthSocket)/sizeof(sshAuthSocket[0])) == 0) {
		_tprintf( TEXT("Missing SSH_AUTH_SOCK env variable\n"));
		return INVALID_SOCKET;
	}

	_tprintf( TEXT("Handling query, connecting to upstream on %s\n"), sshAuthSocket);

	do {
		fileHandle = CreateFile (sshAuthSocket,
		                        GENERIC_READ,
		                        FILE_SHARE_READ,
		                        NULL,
		                        OPEN_EXISTING,
		                        FILE_ATTRIBUTE_NORMAL,
		                        NULL);
		lastError = GetLastError();

		if (fileHandle != INVALID_HANDLE_VALUE || lastError != ERROR_SHARING_VIOLATION) {
			break;
		}
	} while(1);

	if(fileHandle == INVALID_HANDLE_VALUE) {
		_tprintf( TEXT("Failed to open file %s: %lu\n"), sshAuthSocket, lastError);
		return INVALID_SOCKET;
	}


	result = ReadFile(fileHandle, buffer, sizeof(buffer) - 1, &bytesRead, NULL);
	CloseHandle(fileHandle);

	if(!result) {
		_tprintf( TEXT("Failed to read file %s: %lu\n"), sshAuthSocket, GetLastError());
		return INVALID_SOCKET;
	}

	buffer[bytesRead] = 0;

	if(memcmp(buffer, SOCKET_COOKIE, strlen(SOCKET_COOKIE)) != 0) {
		printf( "Failed to find cookie %s in %s\n", SOCKET_COOKIE, buffer);
		return INVALID_SOCKET;
	}

	uint16_t port;
	char type;
	uint32_t cookie[4];

	sscanf (buffer + strlen (SOCKET_COOKIE), "%hu %c %08x-%08x-%08x-%08x",
	       &port,
	       &type,
	       &cookie[0], &cookie[1], &cookie[2], &cookie[3]);

	printf("Connecting to upstream ssh-agent at 127.0.0.1:%u, type: %c, cookie: %08x-%08x-%08x-%08x\n",
	       port,
	       type,
	       cookie[0], cookie[1], cookie[2], cookie[3]);

	SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if(sock == INVALID_SOCKET) {
		printf("Failed to open socket: %lu\n", GetLastError());
		return INVALID_SOCKET;
	}

	struct sockaddr_in address;

	address.sin_family = AF_INET;
	inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
	address.sin_port = htons(port);

	result = connect(sock, (const struct sockaddr*)&address, sizeof(address));
	if(result < 0) {
		printf("Failed to connect socket to 127.0.0.1:%u : %lu\n", port, GetLastError());
		goto cleanup;
	}

	result = send(sock, (const char*) cookie, sizeof(cookie), 0);
	if(result < 0) {
		printf("Failed to send GUID to 127.0.0.1:%u : %lu\n", port, GetLastError());
		goto cleanup;
	}

	result = recv_full(sock, (char*) cookie, sizeof(cookie), 0);
	if(result < 0) {
		printf("Failed to recv GUID to 127.0.0.1:%u : %lu\n", port, GetLastError());
		goto cleanup;
	}
	printf("Received from ssh-agent: port %u, type: %c, cookie: %08x-%08x-%08x-%08x\n",
	       port,
	       type,
	       cookie[0], cookie[1], cookie[2], cookie[3]);

	struct id_data {
		uint32_t pid;
		uint32_t uid;
		uint32_t gid;
	};

	struct id_data ids;
	ids.pid = GetCurrentProcessId();
	ids.uid = ids.gid = 0;

	result = send(sock, (const char*) &ids, sizeof(ids), 0);
	if(result < 0) {
		printf("Failed to send user IDs to 127.0.0.1:%u : %lu\n", port, GetLastError());
		goto cleanup;
	}

	result = recv_full(sock, (char*) &ids, sizeof(ids), 0);
	if(result < 0) {
		printf("Failed to recv user IDs to 127.0.0.1:%u : %lu\n", port, GetLastError());
		goto cleanup;
	}

	printf("Received from ssh-agent: pid: %u, uid: %u, gid: %u\n",
	       ids.pid,
	       ids.uid,
	       ids.gid);

	return sock;

cleanup:
	closesocket(sock);

	return INVALID_SOCKET;
}

