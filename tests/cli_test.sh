#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 Kevin H. Patterson
#
# Runs the zar and unzar commands themselves, to check how options combine.
#
#   cli_test.sh ZAR UNZAR

set -u
absolute() { case $1 in /*) echo "$1" ;; *) echo "$PWD/$1" ;; esac; }
zar=$(absolute "$1")
unzar=$(absolute "$2")
work=$(mktemp -d "${TMPDIR:-/tmp}/zar-cli-test.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT
failures=0

fail()
{
    echo "FAIL: $*" >&2
    failures=$((failures + 1))
}

# expect STATUS DESCRIPTION COMMAND...: runs COMMAND in a fresh empty
# directory, $work/run, with output in $work/out and $work/err.
expect()
{
    want=$1 what=$2
    shift 2
    rm -rf "$work/run" && mkdir "$work/run"
    (cd "$work/run" && "$@") >"$work/out" 2>"$work/err"
    got=$?
    [ "$got" -eq "$want" ] || fail "$what: exit $got, wanted $want: $(cat "$work/err")"
}

wrote_nothing() { [ -z "$(ls -A "$work/run")" ] || fail "$1: wrote files"; }
extracted() { [ -f "$work/run/proj/sub/b.txt" ] || fail "$1: didn't extract"; }
listed() { grep -q 'b\.txt' "$work/out" || fail "$1: didn't list"; }
not_listed() { [ ! -s "$work/out" ] || fail "$1: listed"; }
said() { grep -q "$2" "$work/err" || fail "$1: no '$2' in: $(cat "$work/err")"; }

mkdir -p "$work/proj/sub"
echo hello >"$work/proj/a.txt"
echo world >"$work/proj/sub/b.txt"
archive=$work/proj.zar

# zar: combined flags, and -l's level attached or separate.
expect 0 "zar -yvl19" "$zar" -yvl19 -o "$archive" "$work/proj"
said "zar -yvl19" 'b\.txt'
expect 0 "zar -myl 19 -j2" "$zar" -myl 19 -j2 -o "$archive" "$work/proj"
expect 0 "zar -mvyj2 -l3" "$zar" -mvyj2 -l3 -o "$work/two.zar" "$work/proj" "$work/proj.zar"
said "zar -mvyj2 -l3" 'proj\.zar'
expect 2 "zar without -y" "$zar" -v -o "$archive" "$work/proj"
said "zar without -y" 'exists'

# A listing on its own only scans.
expect 0 "unzar -l" "$unzar" -l "$archive"
listed "unzar -l"; wrote_nothing "unzar -l"
expect 0 "unzar --list" "$unzar" --list "$archive"
listed "unzar --list"; wrote_nothing "unzar --list"
expect 0 "unzar -lv" "$unzar" -lv "$archive"
said "unzar -lv" '^scanned '; wrote_nothing "unzar -lv"

# -x adds extraction to a listing, and is the default without one.
expect 0 "unzar -lx" "$unzar" -lx "$archive"
listed "unzar -lx"; extracted "unzar -lx"
expect 0 "unzar --list --extract" "$unzar" --list --extract "$archive"
listed "unzar --list --extract"; extracted "unzar --list --extract"
expect 0 "unzar -lxvh" "$unzar" -lxvh "$archive"
listed "unzar -lxvh"; extracted "unzar -lxvh"; said "unzar -lxvh" '^extracted '
expect 0 "unzar" "$unzar" "$archive"
not_listed "unzar"; extracted "unzar"
expect 0 "unzar -x" "$unzar" -x "$archive"
not_listed "unzar -x"; extracted "unzar -x"
expect 0 "unzar -xj2 -C" "$unzar" -xj2 -C out "$archive"
[ -f "$work/run/out/proj/a.txt" ] || fail "unzar -xj2 -C: didn't extract into out"

# Other modes.
expect 0 "unzar -ltv" "$unzar" -ltv "$archive"
listed "unzar -ltv"; wrote_nothing "unzar -ltv"; said "unzar -ltv" '^tested '
expect 0 "unzar -l --scan" "$unzar" -l --scan "$archive"
listed "unzar -l --scan"; wrote_nothing "unzar -l --scan"
expect 0 "unzar -t" "$unzar" -t "$archive"
not_listed "unzar -t"; wrote_nothing "unzar -t"

# At most one mode, though repeating one is fine.
for modes in "-xt" "-x --scan" "-lt --scan" "--extract -t"; do
    # shellcheck disable=SC2086
    expect 2 "unzar $modes" "$unzar" $modes "$archive"
    said "unzar $modes" 'only one of'; wrote_nothing "unzar $modes"
done
expect 0 "unzar -xx" "$unzar" -xx "$archive"
extracted "unzar -xx"

if [ "$failures" -ne 0 ]; then
    echo "$failures failures" >&2
    exit 1
fi
echo "cli_test: all passed"
