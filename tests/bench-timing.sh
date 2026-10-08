#!/bin/sh
# Both hyperfine report schemas yield the same summary units and reject
# incomplete or ambiguous reports. No benchmark engines are needed.

set -eu
: "${RELIC_SRCDIR:?RELIC_SRCDIR must name the source directory}"
parser=$RELIC_SRCDIR/bench/compare/timing.awk
fixtures=$RELIC_SRCDIR/tests/fixtures/bench
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM

assert_summary() {
	actual=$(awk -f "$parser" "$1")
	if [ "$actual" != "$2" ]; then
		echo "$1: expected '$2', got '$actual'" >&2
		exit 1
	fi
}

assert_summary "$fixtures/hyperfine-v2.json" \
    '0.0026196665000000003 0.002407583 0.00029993136205555435 0.00077 1212416.0'

cat > "$work/v1.json" <<'EOF'
{
  "results": [
    {
      "command": "true",
      "median": 2.0,
      "min": 1.0,
      "stddev": 0.5,
      "user": 1.5,
      "memory_usage_byte": [
        4096,
        8192
      ]
    }
  ]
}
EOF
assert_summary "$work/v1.json" '2.0 1.0 0.5 1.5 8192'
sed 's/"stddev": 0.5/"stddev": null/' "$work/v1.json" > "$work/one.json"
assert_summary "$work/one.json" '2.0 1.0 - 1.5 8192'

for schema in "$work/v1.json" "$fixtures/hyperfine-v2.json"; do
	for key in median min stddev user mean memory_usage_byte memory_peak_resident; do
		if ! grep -q "\"$key\":" "$schema"; then
			continue
		fi
		awk -v key="$key" '
			index($0, "\"" key "\":") {
				if ($0 ~ /[{[]/) skip = 1
				next
			}
			skip { if ($0 ~ /[}\]]/) skip = 0; next }
			{ print }
		' "$schema" > "$work/incomplete.json"
		if awk -f "$parser" "$work/incomplete.json"; then
			echo "accepted a report missing $key: $schema" >&2
			exit 1
		fi
	done
	cat "$schema" "$schema" > "$work/multiple.json"
	if awk -f "$parser" "$work/multiple.json"; then
		echo "accepted multiple benchmark results" >&2
		exit 1
	fi
done
sed 's/"median": 2.0/"median": null/' "$work/v1.json" > "$work/invalid.json"
if awk -f "$parser" "$work/invalid.json"; then
	echo "accepted a null median" >&2
	exit 1
fi
