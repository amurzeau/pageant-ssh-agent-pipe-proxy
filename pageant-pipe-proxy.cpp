#include <stdint.h>
#include <stdio.h>
#include <strsafe.h>
#include <tchar.h>
#include <windows.h>

#define AGENT_MAX_MSGLEN 262144
#define AGENT_COPYDATA_ID 0x804e50ba

DWORD WINAPI InstanceThread(LPVOID);
VOID GetAnswerToRequest(void*, DWORD pchRequestBytes, void*, DWORD* pchReplyBytes);

void print_help(TCHAR* argv[], LPCTSTR lpszPipename) {
	_tprintf(TEXT("Usage: %s [pipe_path]\n\n pipe_path: path to a pipe, defaults to %s\n"), argv[0], lpszPipename);
}

int _tmain(void) {
	BOOL fConnected = FALSE;
	DWORD dwThreadId = 0;
	HANDLE hPipe = INVALID_HANDLE_VALUE, hThread = NULL;
	LPCTSTR pipeRequiredPrefix = TEXT("\\\\.");
	LPCTSTR lpszPipename = TEXT("\\\\.\\pipe\\openssh-ssh-agent");

	if(__argc == 2) {
		if(_tcsncmp(__targv[1], pipeRequiredPrefix, _tcslen(pipeRequiredPrefix)) != 0) {
			_tprintf(TEXT("Invalid argument %s, must start with %s if present\n"), __targv[1], pipeRequiredPrefix);
			print_help(__targv, lpszPipename);
			return 1;
		}

		lpszPipename = __targv[1];
	} else if(__argc > 2) {
		print_help(__targv, lpszPipename);
		return 1;
	}

	// The main loop creates an instance of the named pipe and
	// then waits for a client to connect to it. When the client
	// connects, a thread is created to handle communications
	// with that client, and this loop is free to wait for the
	// next client connect request. It is an infinite loop.

	for(;;) {
		_tprintf(TEXT("pageant pipe server: Main thread awaiting client connection on %s\n"), lpszPipename);
		hPipe = CreateNamedPipe(lpszPipename,              // pipe name
		                        PIPE_ACCESS_DUPLEX,        // read/write access
		                        PIPE_TYPE_BYTE |           // message type pipe
		                            PIPE_READMODE_BYTE |   // message-read mode
		                            PIPE_WAIT,             // blocking mode
		                        PIPE_UNLIMITED_INSTANCES,  // max. instances
		                        AGENT_MAX_MSGLEN,          // output buffer size
		                        AGENT_MAX_MSGLEN,          // input buffer size
		                        0,                         // client time-out
		                        NULL);                     // default security attribute

		if(hPipe == INVALID_HANDLE_VALUE) {
			_tprintf(TEXT("CreateNamedPipe failed, GLE=%lu.\n"), GetLastError());
			return -1;
		}

		// Wait for the client to connect; if it succeeds,
		// the function returns a nonzero value. If the function
		// returns zero, GetLastError returns ERROR_PIPE_CONNECTED.

		fConnected = ConnectNamedPipe(hPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);

		if(fConnected) {
			printf("Client connected, creating a processing thread.\n");

			// Create a thread for this client.
			hThread = CreateThread(NULL,            // no security attribute
			                       0,               // default stack size
			                       InstanceThread,  // thread proc
			                       (LPVOID) hPipe,  // thread parameter
			                       0,               // not suspended
			                       &dwThreadId);    // returns thread ID

			if(hThread == NULL) {
				_tprintf(TEXT("CreateThread failed, GLE=%lu.\n"), GetLastError());
				return -1;
			} else
				CloseHandle(hThread);
		} else
			// The client could not connect, so close the pipe.
			CloseHandle(hPipe);
	}

	return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR pCmdLine, int nCmdShow) {
	(void) hInstance;
	(void) hPrevInstance;
	(void) pCmdLine;
	(void) nCmdShow;

	return _tmain();
}

uint32_t readu32(const void* buffer) {
	const uint8_t* buffer_char = (const uint8_t*) buffer;
	return (buffer_char[0] << 24) | (buffer_char[1] << 16) | (buffer_char[2] << 8) | (buffer_char[3] << 0);
}

DWORD WINAPI InstanceThread(LPVOID lpvParam)
// This routine is a thread processing function to read from and reply to a client
// via the open pipe connection passed from the main loop. Note this allows
// the main loop to continue executing, potentially creating more threads of
// of this procedure to run concurrently, depending on the number of incoming
// client connections.
{
	HANDLE hHeap = GetProcessHeap();
	char* pchRequest = (char*) HeapAlloc(hHeap, 0, AGENT_MAX_MSGLEN);
	char* pchReply = (char*) HeapAlloc(hHeap, 0, AGENT_MAX_MSGLEN);

	DWORD cbBytesRead = 0, cbReplyBytes = 0, cbWritten = 0;
	BOOL fSuccess = FALSE;
	HANDLE hPipe = NULL;

	// Do some extra error checking since the app will keep running even if this
	// thread fails.

	if(lpvParam == NULL) {
		printf("\nERROR - Pipe Server Failure:\n");
		printf("   InstanceThread got an unexpected NULL value in lpvParam.\n");
		printf("   InstanceThread exitting.\n");
		if(pchReply != NULL)
			HeapFree(hHeap, 0, pchReply);
		if(pchRequest != NULL)
			HeapFree(hHeap, 0, pchRequest);
		return (DWORD) -1;
	}

	if(pchRequest == NULL) {
		printf("\nERROR - Pipe Server Failure:\n");
		printf("   InstanceThread got an unexpected NULL heap allocation.\n");
		printf("   InstanceThread exitting.\n");
		if(pchReply != NULL)
			HeapFree(hHeap, 0, pchReply);
		return (DWORD) -1;
	}

	if(pchReply == NULL) {
		printf("\nERROR - Pipe Server Failure:\n");
		printf("   InstanceThread got an unexpected NULL heap allocation.\n");
		printf("   InstanceThread exitting.\n");
		if(pchRequest != NULL)
			HeapFree(hHeap, 0, pchRequest);
		return (DWORD) -1;
	}

	// Print verbose messages. In production code, this should be for debugging only.
	printf("InstanceThread created, receiving and processing messages.\n");

	// The thread's parameter is a handle to a pipe object instance.

	hPipe = (HANDLE) lpvParam;

	// Loop until done reading
	while(1) {
		int byteRead = 0;
		int remainingBytes = AGENT_MAX_MSGLEN;
		do {
			// Read client requests from the pipe. This simplistic code only allows messages
			// up to AGENT_MAX_MSGLEN characters in length.
			fSuccess = ReadFile(hPipe,                  // handle to pipe
			                    pchRequest + byteRead,  // buffer to receive data
			                    remainingBytes,         // size of buffer
			                    &cbBytesRead,           // number of bytes read
			                    NULL);                  // not overlapped I/O

			if(!fSuccess || cbBytesRead == 0) {
				if(GetLastError() == ERROR_BROKEN_PIPE) {
					_tprintf(TEXT("InstanceThread: client disconnected.\n"));
				} else {
					_tprintf(TEXT("InstanceThread ReadFile failed, GLE=%lu.\n"), GetLastError());
				}
				break;
			} else {
				byteRead += cbBytesRead;
				if(byteRead > 4) {
					remainingBytes = readu32(pchRequest) + 4 - byteRead;
				}
			}
		} while(remainingBytes > 0);

		if(!fSuccess) {
			break;
		}

		// Process the incoming message.
		GetAnswerToRequest(pchRequest, byteRead, pchReply, &cbReplyBytes);

		// Write the reply to the pipe.
		fSuccess = WriteFile(hPipe,         // handle to pipe
		                     pchReply,      // buffer to write from
		                     cbReplyBytes,  // number of bytes to write
		                     &cbWritten,    // number of bytes written
		                     NULL);         // not overlapped I/O

		if(!fSuccess || cbReplyBytes != cbWritten) {
			_tprintf(TEXT("InstanceThread WriteFile failed, GLE=%lu.\n"), GetLastError());
			break;
		}
	}

	// Flush the pipe to allow the client to read the pipe's contents
	// before disconnecting. Then disconnect the pipe, and close the
	// handle to this pipe instance.

	FlushFileBuffers(hPipe);
	DisconnectNamedPipe(hPipe);
	CloseHandle(hPipe);

	HeapFree(hHeap, 0, pchRequest);
	HeapFree(hHeap, 0, pchReply);

	printf("InstanceThread exiting.\n");
	return 1;
}

VOID GetAnswerToRequest(void* pchRequest, DWORD pchRequestBytes, void* pchReply, DWORD* pchReplyBytes) {
	char mapName[128];
	sprintf_s(mapName, sizeof(mapName), "PageantRequest%08lx", GetCurrentThreadId());
	mapName[sizeof(mapName) - 1] = 0;

	*pchReplyBytes = 0;

	printf("Sending %lu bytes to pageant\n", pchRequestBytes);
	for(DWORD i = 0; i < pchRequestBytes; i++) {
		printf("%02x ", ((const uint8_t*) pchRequest)[i]);
	}
	printf("\n");

	HWND pageantHwnd = FindWindow("Pageant", "Pageant");
	if(pageantHwnd == INVALID_HANDLE_VALUE) {
		printf("Failed to find Pageant window: %lu\n", GetLastError());
	}

	HANDLE fileMap = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, AGENT_MAX_MSGLEN, mapName);
	if(fileMap == INVALID_HANDLE_VALUE) {
		printf("Failed to create file mapping: %lu\n", GetLastError());
		return;
	}

	uint8_t* sharedMemory = (uint8_t*) MapViewOfFile(fileMap, FILE_MAP_WRITE, 0, 0, 0);

	memcpy_s(sharedMemory, AGENT_MAX_MSGLEN, pchRequest, pchRequestBytes);

	COPYDATASTRUCT cds;
	cds.dwData = AGENT_COPYDATA_ID;
	cds.cbData = (DWORD) (strlen(mapName) + 1);
	cds.lpData = mapName;
	LRESULT result = SendMessage(pageantHwnd, WM_COPYDATA, 0, (LPARAM) &cds);
	if(result == FALSE) {
		printf("SendMessage failed: %lu\n", GetLastError());
	}

	DWORD replyLen =
	    (sharedMemory[0] << 24) | (sharedMemory[1] << 16) | (sharedMemory[2] << 8) | (sharedMemory[3] << 0);

	replyLen += 4;

	if(replyLen > AGENT_MAX_MSGLEN) {
		printf("Invalid reply size: %lu (0x%x)\n", replyLen, replyLen);
		replyLen = AGENT_MAX_MSGLEN;
	}

	*pchReplyBytes = replyLen;
	memcpy_s(pchReply, AGENT_MAX_MSGLEN, sharedMemory, *pchReplyBytes);

	printf("Read %lu bytes from pageant\n", replyLen);
	for(DWORD i = 0; i < replyLen; i++) {
		printf("%02x ", ((const uint8_t*) pchReply)[i]);
	}
	printf("\n");

	UnmapViewOfFile(sharedMemory);
	CloseHandle(fileMap);
}
