#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .native
"${CXX:-g++}" -std=c++17 -O1 -g -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer -Iinclude tests/core_test.cpp -o .native/core_test
.native/core_test
"${CXX:-g++}" -std=c++17 -O1 -g -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer -Itests/fakes -Iinclude tests/hardware_test.cpp -o .native/hardware_test
.native/hardware_test
