#!/usr/bin/env bash
# Unit-tests the control logic (box_logic.h) on a PC with a fake clock. Needs only g++.
set -euo pipefail
cd "$(dirname "$0")"
g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined -I../parcel_box_main test_box_logic.cpp -o /tmp/pb_logic_test
/tmp/pb_logic_test
