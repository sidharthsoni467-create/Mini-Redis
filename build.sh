#!/usr/bin/env bash
# Convenience script holding the exact g++ commands. This is NOT a build system
# and is not required -- you can paste either line by hand.
set -euo pipefail

g++ -std=c++17 -O2 -Wall -Wextra -pthread -o miniredis-server \
    server.cpp store.cpp wal.cpp protocol.cpp

g++ -std=c++17 -O2 -Wall -Wextra -pthread -o miniredis-cli \
    client.cpp protocol.cpp

echo "built: miniredis-server  miniredis-cli"
