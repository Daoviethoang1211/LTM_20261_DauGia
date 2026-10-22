#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
make clean
make CFLAGS='-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -MMD -MP -fsanitize=address,undefined -fno-omit-frame-pointer' test
make clean
make
