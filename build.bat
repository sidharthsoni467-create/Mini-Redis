@echo off
REM Native Windows build with MinGW-w64 g++.
REM Differences from the Linux build: link -lws2_32 for Winsock2, and drop
REM -pthread (MinGW's std::thread needs a POSIX-threads build of the toolchain,
REM which MSYS2's mingw-w64-x86_64-gcc is).

g++ -std=c++17 -O2 -Wall -Wextra -o miniredis-server.exe server.cpp store.cpp wal.cpp protocol.cpp -lws2_32
if errorlevel 1 exit /b 1

g++ -std=c++17 -O2 -Wall -Wextra -o miniredis-cli.exe client.cpp protocol.cpp -lws2_32
if errorlevel 1 exit /b 1

echo built: miniredis-server.exe  miniredis-cli.exe
