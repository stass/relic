#!/bin/sh
# Verification captures answers; timing serializes the same rows to stdout.
set -eu
: "${RELIC_SRCDIR:?RELIC_SRCDIR must name the source directory}"
cleanup=$(mktemp -d)
trap 'rm -rf "$cleanup"' EXIT
trap 'exit 1' HUP INT TERM
# Spaces and apostrophes must survive command construction for both phases.
work="$cleanup/engine's output"
mkdir -p "$work/bin" "$work/data/startup"
awk '
    /^(quote|joined|engine_command|measure|parse_timing)\(\)/ { copying = 1 }
    copying { print }
    copying && (/^}/ || /^\)$/) { copying = 0 }
' "$RELIC_SRCDIR/bench/compare/run.sh" > "$work/functions.sh"
. "$work/functions.sh"
die() { echo "$*" >&2; exit 2; }
programs_of() { printf '%s\n' "$RELIC_SRCDIR/bench/compare/startup"; }
program_file() { printf '%s\n' "$1"; }
dataset_of() { printf '%s\n' startup; }
cat > "$work/engine" <<'ENGINE'
#!/bin/sh
set -eu
out=
stdout=no
while [ "$#" -gt 0 ]; do
    case $1 in
    -D) out=$2; shift ;;
    --param)
        [ "$2" = 'answer_resource=""' ] && stdout=yes
        shift
        ;;
    esac
    shift
done
if [ "$out" = - ] || [ "$stdout" = yes ]; then
    printf '1\t2\n'
else
    printf '1\t2\n' > "$out/answer"
fi
ENGINE
chmod +x "$work/engine"
souffle=$work/engine
nmo=$work/engine
souffle_switches=-j1
nemo_switches='--report none -o'
cp "$work/engine" "$work/bin/souffle"
cp "$work/engine" "$work/bin/souffle-magic"
for engine in souffle-interp souffle souffle-magic nemo; do
    mkdir -p "$work/out/$engine"
    sh -c "$(engine_command "$engine" startup verify)" > "$work/verification-stdout"
    [ ! -s "$work/verification-stdout" ]
    printf '1\t2\n' > "$work/expected"
    cmp "$work/expected" "$work/out/$engine/answer"
    printf 'verification retained\n' > "$work/out/$engine/answer"
    sh -c "$(engine_command "$engine" startup timed)" > "$work/timed-stdout"
    if ! cmp -s "$work/expected" "$work/timed-stdout"; then
        echo "$engine timing did not serialize its answer to stdout" >&2
        exit 1
    fi
    [ "$(cat "$work/out/$engine/answer")" = 'verification retained' ]
done
if (engine_command nemo startup invalid-phase) > /dev/null 2>&1; then
    echo 'Accepted an invalid output phase' >&2
    exit 1
fi
# Check that measure selects timed output, not the verification command.
here=$RELIC_SRCDIR/bench/compare
runs=2
clingo_ignore=-i
timing_switches=
status=ok
hyperfine() {
    for command; do :; done
    sh -c "$command" > /dev/null
    cat "$RELIC_SRCDIR/tests/fixtures/bench/hyperfine-v2.json" > "$work/timing.json"
}
for engine in souffle-interp souffle souffle-magic nemo; do
    measure "$engine" startup
    [ "$status" = ok ]
    [ "$(cat "$work/out/$engine/answer")" = 'verification retained' ]
done
printf 'bench-output: captured verification, serialized stdout and timing phase passed\n'
