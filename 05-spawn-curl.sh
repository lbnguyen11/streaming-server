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

LOOP=${1:-512}
for i in $(seq 1 "${LOOP}");
do
curl -i -X GET http://0.0.0.0:8080/stream > /dev/null 2>&1 &
done
