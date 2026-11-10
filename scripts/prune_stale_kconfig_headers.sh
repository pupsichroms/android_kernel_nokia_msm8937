#!/usr/bin/env bash
set -euo pipefail

config_file="${1:?final kernel .config is required}"
headers_root="${2:?generated include/config directory is required}"

[[ -f ${config_file} ]] || {
  echo "missing final kernel config: ${config_file}" >&2
  exit 1
}
[[ -d ${headers_root} && ${headers_root} != / ]] || {
  echo "unsafe or missing generated header root: ${headers_root}" >&2
  exit 1
}

while IFS= read -r -d '' header; do
  relative="${header#"${headers_root}/"}"
  path_symbol="$(printf '%s' "${relative%.h}" | tr '/-' '__')"
  upper_symbol="$(printf '%s' "${path_symbol}" | tr '[:lower:]' '[:upper:]')"
  if ! grep -q "^CONFIG_${upper_symbol}=" "${config_file}" \
     && ! grep -q "^CONFIG_${path_symbol}=" "${config_file}"; then
    rm -f -- "${header}"
  fi
done < <(find "${headers_root}" -type f -name '*.h' -print0)

find "${headers_root}" -depth -type d -empty -delete
