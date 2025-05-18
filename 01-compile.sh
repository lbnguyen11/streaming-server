#!/bin/bash

# Define debug trap
debug() {
  # print a '+' for every element in BASH_LINENO, similar to PS4's behavior
  printf '%s' "${BASH_LINENO[@]/*/+}"
  # Then print the current command, colored
  printf ' \e[36m%s\e[0m\n' "$BASH_COMMAND"
}
trap debug DEBUG
shopt -s extdebug # necessary for the DEBUG trap to carry into functions

# CYAN=$(tput setaf 6)
# RESET=$(tput sgr0)
# exec 3> >(exec sed -E -u "s@^[+]+.*\$@${CYAN}&${RESET}@")
# exec 1>&3
# BASH_XTRACEFD=3
# set -x

# #Debug build:
# echo "[INFO](BEG) Start Debug build!"
# cmake -B build/Debug -DCMAKE_BUILD_TYPE=Debug
# cmake --build build/Debug
# echo "[INFO](END) Start Debug build!"

BUILD_DIR=./build
# mkdir ${BUILD_DIR}
cmake -DCODE_COVERAGE=ON -B ${BUILD_DIR}
cd ${BUILD_DIR} || exit
rm -rf CMakeFiles/ *.gcda *.gcno *.gcov
cmake --build .