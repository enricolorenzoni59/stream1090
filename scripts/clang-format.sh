#!/usr/bin/env bash

set -euo pipefail

readonly EXPECTED_VERSION="22.1.7"

usage() {
    echo "Usage: $0 --check|--write" >&2
    exit 2
}

[[ $# -eq 1 ]] || usage

case "$1" in
    --check)
        format_args=(--dry-run --Werror)
        ;;
    --write)
        format_args=(-i)
        ;;
    *)
        usage
        ;;
esac

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
cd "${repo_root}"

if [[ -n "${CLANG_FORMAT:-}" ]]; then
    formatter="${CLANG_FORMAT}"
elif command -v clang-format-22 >/dev/null 2>&1; then
    formatter="$(command -v clang-format-22)"
elif command -v clang-format >/dev/null 2>&1; then
    formatter="$(command -v clang-format)"
else
    echo "clang-format ${EXPECTED_VERSION} was not found" >&2
    exit 1
fi

version="$("${formatter}" --version)"
case "${version}" in
    *"version ${EXPECTED_VERSION}"*) ;;
    *)
        echo "Expected clang-format ${EXPECTED_VERSION}, found: ${version}" >&2
        exit 1
        ;;
esac

files=()
while IFS= read -r file; do
    case "${file}" in
        thirdparty/*) continue ;;
    esac
    files+=("${file}")
done < <(git ls-files -- '*.c' '*.cc' '*.cpp' '*.cxx' '*.h' '*.hh' '*.hpp' '*.hxx')

if [[ ${#files[@]} -eq 0 ]]; then
    echo "No tracked first-party C/C++ files found" >&2
    exit 1
fi

"${formatter}" --style=file "${format_args[@]}" "${files[@]}"
