# pageant-ssh-agent-pipe-proxy
Pipe Win32 OpenSSL agent requests (on `\\.\pipe\openssh-ssh-agent`) to Git Bash ssh-agent or pageant

This repository generate Windows binaries to be able to use OpenSSH_for_Windows with:

 - Git Bash ssh-agent
 - Pageant

These tools will:

 - Listen on the default OpenSSH_for_Windows agent socket at \\.\pipe\openssh-ssh-agent
 - Forward any request to either Git Bash ssh-agent (through SSH_AUTH_SOCK unix domain socket) or Pageant

These tools were made to be able to have a ssh agent with Visual Studio Code + Remote - Containers
and Docker Desktop without having OpenSSH_for_Windows agent service running.

# Usage

## Forwarding to pageant

- Run pageant
- Run pageant-pipe-proxy.exe

Now, you can use OpenSSH_for_Windows' ssh-add and it will use pageant to store keys.

## Forwarding to Git Bash's ssh-agent

- Run in Git Bash:
  `ssh-agent bash -c 'SSH_AUTH_SOCK=$(cygpath -w $SSH_AUTH_SOCK) winpty ./ssh-agent-pipe-proxy.exe'`

Now, you can use OpenSSH_for_Windows' ssh-add and it will use ssh-agent to store keys.

Note: `SSH_AUTH_SOCK=$(cygpath -w $SSH_AUTH_SOCK)` is required because SSH_AUTH_SOCK contains something
like /tmp/... which Windows doesn't understand. A future version could replace /tmp/ with the content
of the TMP environment variable to handle it correctly out of the box.

## Listening on a custom pipe

It is possible to listen on a custom pipe intead of the default `\\.\pipe\openssh-ssh-agent` by putting
the custom pipe path as the first argument to `pageant-pipe-proxy.exe` or `ssh-agent-pipe-proxy.exe`.
For example: `pageant-pipe-proxy.exe \\.\pipe\custom-pipe`
Note: in Git Bash, you need to escape '\' or put the pipe path in single quotes.

In that case, you should set SSH_AUTH_SOCK so OpenSSH_for_Windows will use that one instead of its default:
```bat
rem Using OpenSSH_for_Windows' ssh-add
set SSH_AUTH_SOCK=\\.\pipe\custom-pipe
ssh-add -l
```

# Binaries

See here: https://github.com/amurzeau/pageant-ssh-agent-pipe-proxy/releases

# Build instructions

To build, you need a compiler targeting Windows and cmake:
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target package --config RelWithDebInfo
```
