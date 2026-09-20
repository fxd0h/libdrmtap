#!/usr/bin/env bash
# Fail if the helper search list documented in include/drmtap.h disagrees with the
# list the library actually searches in src/privilege_helper.c.
#
# This check exists because that comment was wrong from the project's FIRST commit
# until 0.5.7: it advertised $DRMTAP_HELPER_PATH, which no code has ever read, and
# <exe_dir>/drmtap-helper, which is not searched, while omitting four paths that
# are. The list is security-relevant - the first executable match is exec'd with no
# check of its owner or mode - so a reader hardening a deployment from the header
# would have locked down two directories and left four open. Nothing compared the
# two lists, so nothing caught it for eleven releases.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
hdr="$root/include/drmtap.h"
src="$root/src/privilege_helper.c"

# Documented: the numbered "N. /path" lines inside the helper_path comment.
doc=$(sed -n '/Path to the privileged helper binary/,/const char \*helper_path;/p' "$hdr" \
    | sed -nE 's@^[[:space:]]*\*[[:space:]]+[0-9]+\.[[:space:]]+(/[^[:space:]]+)[[:space:]]*$@\1@p')

# Actual: the string literals in the helper_search_paths[] array, in order. Slot 0 is
# the configured path (a NULL placeholder), so only literals count.
code=$(sed -n '/helper_search_paths\[\] = {/,/^};/p' "$src" \
    | sed -nE 's@^[[:space:]]*"(/[^"]+)",?[[:space:]]*$@\1@p')

if [ -z "$doc" ] || [ -z "$code" ]; then
    echo "check-helper-paths: could not extract a list (doc=$(echo "$doc" | grep -c .), code=$(echo "$code" | grep -c .))" >&2
    echo "  the comment or the array moved; fix this script rather than deleting the check" >&2
    exit 1
fi

if ! diff <(echo "$doc") <(echo "$code") > /tmp/helper-paths.diff 2>&1; then
    echo "check-helper-paths: MISMATCH between include/drmtap.h and src/privilege_helper.c" >&2
    echo "  < documented    > actually searched" >&2
    sed 's/^/  /' /tmp/helper-paths.diff >&2
    exit 1
fi

echo "check-helper-paths: header documents the $(echo "$code" | grep -c .) paths the library searches, in order"
