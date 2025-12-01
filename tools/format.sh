#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_DIR="${ROOT_DIR}/src"
EXCLUDE_DIR="${SRC_DIR}/include"

# Find source files excluding thirdparty
find_sources() {
	find "${SRC_DIR}" \
		-path "${EXCLUDE_DIR}" -prune -o \
		-type f \( -name "*.c" -o -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \) -print
}

echo "Formatting sources (excluding thirdparty)..."
find_sources | tr '\n' '\0' | xargs -0 -r clang-format -i

# Remove whole-line // comments (excluding thirdparty) while leaving inline comments
echo "Stripping whole-line // comments (excluding thirdparty)..."
find_sources | while IFS= read -r f; do
	sed -i '/^[[:space:]]*\/\//d' "$f"
    echo "$f"
done
