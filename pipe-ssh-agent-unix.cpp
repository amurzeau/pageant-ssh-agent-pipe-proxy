#include <winsock2.h>
#include <ws2tcpip.h>

#include <stdint.h>
#include <stdio.h>
#include <strsafe.h>
#include <tchar.h>
#include <windows.h>

#include <memory>
#include <vector>

#define AGENT_MAX_MSGLEN 2621440
#define AGENT_COPYDATA_ID 0x804e50ba

template<class F> class final_act {
public:
	explicit final_act(F f) noexcept : f_(std::move(f)), invoke_(true) {}

	final_act(final_act&& other) noexcept : f_(std::move(other.f_)), invoke_(other.invoke_) { other.invoke_ = false; }

	final_act(const final_act&) = delete;
	final_act& operator=(const final_act&) = delete;

	~final_act() noexcept {
		if(invoke_)
			f_();
	}

private:
	F f_;
	bool invoke_;
};

struct thread_data {
	HANDLE hPipe;
	SOCKET sock;
};

DWORD WINAPI InstanceThread(LPVOID lpvData);
SOCKET connect_unix_socket(void);

void print_help(TCHAR* argv[], LPCTSTR lpszPipename) {
	_tprintf(TEXT("Usage: %s [pipe_path]\n\n pipe_path: path to a pipe, defaults to %s\n"), argv[0], lpszPipename);
}

int _tmain(void) {
	WSADATA wsaData;
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

	// Initialize Winsock
	WSAStartup(MAKEWORD(2, 2), &wsaData);

	// The main loop creates an instance of the named pipe and
	// then waits for a client to connect to it. When the client
	// connects, a thread is created to handle communications
	// with that client, and this loop is free to wait for the
	// next client connect request. It is an infinite loop.

	for(;;) {
		_tprintf(TEXT("pageant pipe server: Main thread awaiting client connection on %s\n"), lpszPipename);
		hPipe = CreateNamedPipe(lpszPipename,                          // pipe name
		                        PIPE_ACCESS_DUPLEX,                    // read/write access
		                        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE |  // message type pipe
		                            PIPE_WAIT,                         // blocking mode
		                        PIPE_UNLIMITED_INSTANCES,              // max. instances
		                        AGENT_MAX_MSGLEN,                      // output buffer size
		                        AGENT_MAX_MSGLEN,                      // input buffer size
		                        0,                                     // client time-out
		                        NULL);                                 // default security attribute

		if(hPipe == INVALID_HANDLE_VALUE) {
			_tprintf(TEXT("CreateNamedPipe failed, GLE=%lu.\n"), GetLastError());
			return -1;
		}

		// Wait for the client to connect; if it succeeds,
		// the function returns a nonzero value. If the function
		// returns zero, GetLastError returns ERROR_PIPE_CONNECTED.

		fConnected = ConnectNamedPipe(hPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);

		if(!fConnected) {
			CloseHandle(hPipe);
			continue;
		}

		printf("Client connected, creating a processing thread.\n");

		// Create a read thread for this client.
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

template<typename T> int32_t readAgentMessage(T readFunction, void* buffer, int32_t maxSize) {
	uint8_t* buffer_char = (uint8_t*) buffer;

	int32_t byteRead = 0;
	do {
		int32_t result = readFunction(buffer_char + byteRead, maxSize - byteRead);
		if(result < 0) {
			printf("Failed to read agent message: %d\n", result);
			return result;
		} else if(result == 0) {
			// EOF
			return 0;
		}

		printf("Read %d + %d bytes from ssh-agent\n", result, byteRead);
		for(int32_t i = 0; i < result; i++) {
			printf("%02x ", *(buffer_char + byteRead + i));
		}
		printf("\n");

		byteRead += result;

	} while(byteRead < 4 || byteRead < (int32_t) readu32(buffer_char) + 4);

	return byteRead;
}

DWORD WINAPI InstanceThread(LPVOID lpvData)
// This routine is a thread processing function to read from and reply to a client
// via the open pipe connection passed from the main loop. Note this allows
// the main loop to continue executing, potentially creating more threads of
// of this procedure to run concurrently, depending on the number of incoming
// client connections.
{
	std::vector<char> pchRequest(AGENT_MAX_MSGLEN);
	std::vector<char> pchReply(AGENT_MAX_MSGLEN);

	DWORD cbWritten = 0;
	BOOL fSuccess = FALSE;
	HANDLE hPipe = (HANDLE) lpvData;
	int result;

	final_act closePipe([&hPipe]() {
		if(hPipe) {
			FlushFileBuffers(hPipe);
			DisconnectNamedPipe(hPipe);
			CloseHandle(hPipe);
		}
	});

	// Do some extra error checking since the app will keep running even if this
	// thread fails.

	if(hPipe == NULL) {
		printf("\nERROR - Pipe Server Failure:\n");
		printf("   InstanceThread got an unexpected NULL value in lpvParam.\n");
		printf("   InstanceThread exitting.\n");
		return (DWORD) -1;
	}

	SOCKET sock = connect_unix_socket();
	if(sock == INVALID_SOCKET) {
		printf("Error: cannot connect to upstream ssh-agent\n");
		return (DWORD) -2;
	}
	final_act closeSock([&sock]() {
		if(sock) {
			closesocket(sock);
		}
	});

	// Print verbose messages. In production code, this should be for debugging only.
	printf("InstanceThread created, receiving and processing messages.\n");

	// Loop until done reading
	while(1) {
		int32_t byteRead = readAgentMessage(
		    [hPipe](void* buffer, int32_t size) {
			    DWORD cbBytesRead = 0;
			    BOOL fSuccess = ReadFile(hPipe,         // handle to pipe
			                             buffer,        // buffer to receive data
			                             size,          // size of buffer
			                             &cbBytesRead,  // number of bytes read
			                             NULL);         // not overlapped I/O

			    if(fSuccess) {
				    return (int32_t) cbBytesRead;
			    } else {
				    DWORD lastError = GetLastError();
				    if(lastError == ERROR_BROKEN_PIPE)
					    return 0;
				    else
					    return -(int32_t) lastError;
			    }
		    },
		    &pchRequest[0],
		    pchRequest.size());

		if(byteRead <= 0)
			break;

		printf("Sending %d bytes to ssh-agent\n", byteRead);
		for(int i = 0; i < byteRead; i++) {
			printf("%02x ", pchRequest[i]);
		}
		printf("\n");
		result = send(sock, &pchRequest[0], byteRead, 0);
		if(result != (int) byteRead) {
			printf("Failed to send query data to socket : %lu\n", GetLastError());
			break;
		}

		byteRead = readAgentMessage(
		    [sock](void* buffer, int32_t size) {
			    int result = recv(sock, (char*) buffer, size, 0);
			    if(result < 0)
				    return -(int32_t) GetLastError();
			    else
				    return result;
		    },
		    &pchReply[0],
		    pchReply.size());

		if(byteRead <= 0) {
			printf("ssh-agent connection closed\n");
			break;
		}

		// Write the reply to the pipe.
		fSuccess = WriteFile(hPipe,         // handle to pipe
		                     &pchReply[0],  // buffer to write from
		                     byteRead,      // number of bytes to write
		                     &cbWritten,    // number of bytes written
		                     NULL);         // not overlapped I/O

		if(!fSuccess || cbWritten == 0) {
			_tprintf(TEXT("InstanceThread WriteFile failed, GLE=%lu.\n"), GetLastError());
			break;
		}
	}

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

SOCKET connect_unix_socket(void) {
	HANDLE fileHandle;
	DWORD lastError;
	TCHAR sshAuthSocket[256];
	char buffer[128];
	int result;
	DWORD bytesRead;
	const char* SOCKET_COOKIE = "!<socket >";

	if(GetEnvironmentVariable(TEXT("SSH_AUTH_SOCK"), sshAuthSocket, sizeof(sshAuthSocket) / sizeof(sshAuthSocket[0])) ==
	   0) {
		_tprintf(TEXT("Missing SSH_AUTH_SOCK env variable\n"));
		return INVALID_SOCKET;
	}

	_tprintf(TEXT("Handling query, connecting to upstream on %s\n"), sshAuthSocket);

	do {
		fileHandle =
		    CreateFile(sshAuthSocket, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		lastError = GetLastError();

		if(fileHandle != INVALID_HANDLE_VALUE || lastError != ERROR_SHARING_VIOLATION) {
			break;
		}
	} while(1);

	if(fileHandle == INVALID_HANDLE_VALUE) {
		_tprintf(TEXT("Failed to open file %s: %lu\n"), sshAuthSocket, lastError);
		return INVALID_SOCKET;
	}

	result = ReadFile(fileHandle, buffer, sizeof(buffer) - 1, &bytesRead, NULL);
	CloseHandle(fileHandle);

	if(!result) {
		_tprintf(TEXT("Failed to read file %s: %lu\n"), sshAuthSocket, GetLastError());
		return INVALID_SOCKET;
	}

	buffer[bytesRead] = 0;

	if(memcmp(buffer, SOCKET_COOKIE, strlen(SOCKET_COOKIE)) != 0) {
		printf("Failed to find cookie %s in %s\n", SOCKET_COOKIE, buffer);
		return INVALID_SOCKET;
	}

	uint16_t port;
	char type;
	uint32_t cookie[4];

	sscanf(buffer + strlen(SOCKET_COOKIE),
	       "%hu %c %08x-%08x-%08x-%08x",
	       &port,
	       &type,
	       &cookie[0],
	       &cookie[1],
	       &cookie[2],
	       &cookie[3]);

	printf("Connecting to upstream ssh-agent at 127.0.0.1:%u, type: %c, cookie: %08x-%08x-%08x-%08x\n",
	       port,
	       type,
	       cookie[0],
	       cookie[1],
	       cookie[2],
	       cookie[3]);

	SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if(sock == INVALID_SOCKET) {
		printf("Failed to open socket: %lu\n", GetLastError());
		return INVALID_SOCKET;
	}

	struct sockaddr_in address;

	address.sin_family = AF_INET;
	inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
	address.sin_port = htons(port);

	result = connect(sock, (const struct sockaddr*) &address, sizeof(address));
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
	       cookie[0],
	       cookie[1],
	       cookie[2],
	       cookie[3]);

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

	printf("Received from ssh-agent: pid: %u, uid: %u, gid: %u\n", ids.pid, ids.uid, ids.gid);

	return sock;

cleanup:
	closesocket(sock);

	return INVALID_SOCKET;
}
