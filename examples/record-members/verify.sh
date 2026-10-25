#!/bin/sh
set -eu
cd -- "$(dirname -- "$0")"
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
env HOME="$scratch" NO_COLOR=1 TERM=dumb ./run.sh < demo.repl > "$scratch/output" 2>&1
cat "$scratch/output"
if grep -q 'error:' "$scratch/output"; then exit 1; fi
for expected in \
  'private demo::Record::own_ : int' \
  'public demo::Record::run : int (int) const' \
  'public demo::Record::run : double (double)' \
  'private demo::Base::secret_ : int' \
  'protected demo::Base::adjust : void ()' \
  'public static demo::Base::count : int' \
  'public demo::Base::value : demo::Value' \
  'private demo::Choice::tag_ : char' \
  'public demo::Choice::clear : void ()'; do
  grep -Fq "$expected" "$scratch/output"
done
echo 'PASS: record methods, fields, access filters, parents and unions.'
