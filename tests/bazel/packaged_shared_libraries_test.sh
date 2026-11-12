#!/bin/bash
set -euo pipefail

mode="$1"
original="$2"
read -ra libraries <<< "$3"
test "${#libraries[@]}" -eq 2
original_sections=$(readelf -SW "${original}")
[[ "${original_sections}" == *".debug_info"* ]]
[[ "${original_sections}" == *".symtab"* ]]

for library in "${libraries[@]}"; do
  sections=$(readelf -SW "${library}")
  if [[ "${mode}" == "stripped" ]]; then
    [[ "${sections}" != *".debug_"* ]]
    [[ "${sections}" != *".symtab"* ]]
    test "$(stat -c %s "${library}")" -lt "$(stat -c %s "${original}")"
  else
    cmp "${original}" "${library}"
  fi
  dynamic=$(readelf -d "${library}")
  [[ "${dynamic}" == *"[libpackage_strip_fixture.so.1]"* ]]
  python3 - "${library}" <<'PY'
import ctypes
import os
import sys

library = ctypes.CDLL(os.path.abspath(sys.argv[1]))
assert library.DatasystemPackageStripProbe() == 1
PY
done
