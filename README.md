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

# Binaries

See here: https://github.com/amurzeau/pageant-ssh-agent-pipe-proxy/releases

# Build instructions

To build, you need a compiler targeting Windows and cmake:
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target package --config RelWithDebInfo
```
