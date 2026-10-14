#!/bin/sh
# Fetch the exact gitlink revisions without cloning each submodule's history.
set -eu
git config --file .gitmodules --get-regexp '^submodule\..*\.path$' |
while read -r key path; do
    revision=$(git ls-tree HEAD "$path" | awk '$1 == "160000" {print $3}')
    [ -n "$revision" ] || continue
    url=$(git config --file .gitmodules --get "${key%.path}.url")
    repository=${url#https://github.com/}
    repository=${repository%.git}
    if [ "$repository" = "$url" ]; then
        echo "Unsupported submodule URL: $url" >&2
        exit 1
    fi
    echo "Fetching $path at $revision"
    curl -fSL --retry 3 --connect-timeout 20 --max-time 600 \
        "https://codeload.github.com/$repository/tar.gz/$revision" -o /tmp/submodule.tar.gz
    mkdir -p "$path"
    tar -xzf /tmp/submodule.tar.gz --strip-components=1 -C "$path"
    rm /tmp/submodule.tar.gz
done
