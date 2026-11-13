#!/usr/bin/env bash
# Writes the title block date of each board's schematic and PCB: the day of
# the last commit that changed the file beyond its title block, so release
# revisions and these date updates don't count as edits. Uncommitted changes
# don't count either. Run by .github/workflows/kicad-dates.yml.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

# Day of the last commit that changed more than the title block of file $1
edit_date() {
    local log sha day
    log="$(git log --format='%H %cs' -- "$1")"
    while read -r sha day; do
        [ -n "$sha" ] || continue
        if git show --format= --unified=0 "$sha" -- "$1" | awk '
            /^(\+\+\+|---) / { next }
            /^[-+]/ && !/^[-+][ \t]*(\((title_block|title|date|rev|company|comment [0-9]+)( .*)?|\))[ \t]*$/ { real = 1 }
            END { exit !real }'; then
            echo "$day"
            return
        fi
    done <<< "$log"
}

git ls-files -z -- ':(glob)hardware/*/*.kicad_sch' ':(glob)hardware/*/*.kicad_pcb' |
    while IFS= read -r -d '' file; do
        day="$(edit_date "$file")"
        if [ -n "$day" ]; then
            perl -i -pe "s/\(date \"[^\"]*\"\)/(date \"$day\")/" "$file"
        fi
    done
