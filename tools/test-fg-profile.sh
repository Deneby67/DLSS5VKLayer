#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/fg
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror \
  common/fg_profile.cpp test_layer/fg_profile_test.cpp -o build/fg/fg-profile-test
scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
build/fg/fg-profile-test "$scratch"
