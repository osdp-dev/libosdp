#!/bin/bash
#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

refs_str="$(git rev-list --parents -n1 HEAD)"; refs=($refs_str)

merge_head=${refs[0]}
base_head=${refs[1]}
branch_head=${refs[2]}

files=$(git diff --name-only ${base_head} | grep -E ".*\.(cpp|cc|c\+\+|cxx|c|h|hpp)$")
if [ -z "${files}" ]; then
       echo "No source code to check for formatting."
       exit 0
fi

diff=$(git diff -U0 ${base_head} -- ${files} | ./scripts/clang-format-diff.py -p1)

if [[ -z "${diff}" ]]; then
       echo "All source code in PR properly formatted."
       exit 0
fi

echo -e "\nFound formatting errors!\n\n"

echo "${diff}"

echo "\n\nWarning: found some clang format issues!"
echo "You can run 'clang-format --style=file -i FILE_YOU_MODIFED' fix these issues!"
exit 0
