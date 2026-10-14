#!/bin/sh
# Download large archives in bounded ranges; check every range before joining.
set -eu
url=$1
output=$2
size=$(curl -fsSLI --retry 3 "$url" | awk 'tolower($1) == "content-length:" {gsub("\r", "", $2); size=$2} END {print size}')
case "$size" in
    ''|*[!0-9]*) echo "Missing archive size: $url" >&2; exit 1 ;;
esac
parts_dir=$(mktemp -d)
trap 'rm -rf "$parts_dir"' EXIT HUP INT TERM
export url size parts_dir
echo "Downloading $output ($size bytes) in 16 parts"
seq 0 15 | xargs -P 16 -I '{}' sh -eu -c '
    index=$1
    chunk=$(( (size + 15) / 16 ))
    start=$(( index * chunk ))
    end=$(( start + chunk - 1 ))
    [ "$end" -lt "$size" ] || end=$((size - 1))
    part=$(printf "%s/%02d" "$parts_dir" "$index")
    expected=$((end - start + 1))
    for attempt in 1 2 3 4; do
        if curl -fsSL --retry 3 --connect-timeout 20 --max-time 1200 \
            --range "$start-$end" "$url" -o "$part"; then
            actual=$(wc -c < "$part")
            if [ "$actual" -eq "$expected" ]; then
                echo "Downloaded part $index"
                exit 0
            fi
            echo "Incorrect size for archive part $index: $actual != $expected" >&2
        fi
        sleep 2
    done
    echo "Failed to download archive part $index" >&2
    exit 1
' sh '{}'
cat "$parts_dir"/* > "$output"
[ "$(wc -c < "$output")" -eq "$size" ]
