#!/bin/sh
#
# A requested ThinLTO build fails at configure when a tool cannot carry
# bitcode through the library. Stand-ins delegate native work to the real
# tools and refuse only the capability under test.

set -eu

: "${RELIC_SRCDIR:?RELIC_SRCDIR must name the source directory}"
: "${RELIC_CC:?RELIC_CC must name the configured C compiler}"
: "${RELIC_CXX:?RELIC_CXX must name the configured C++ compiler}"
: "${RELIC_AR:?RELIC_AR must name the configured archiver}"
: "${RELIC_RANLIB:?RELIC_RANLIB must name the configured archive indexer}"
: "${RELIC_NM:?RELIC_NM must name the configured symbol reader}"
: "${RELIC_CFLAGS=}" "${RELIC_CXXFLAGS=}"
: "${RELIC_CPPFLAGS=}" "${RELIC_LDFLAGS=}" "${RELIC_LIBS=}"

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM

cat > "$work/tool" <<'EOF'
#!/bin/sh
set -eu
case "$RELIC_TEST_MODE" in
ignore)
	for argument do
		shift
		case "$argument" in
		-flto=thin) ;;
		*) set -- "$@" "$argument" ;;
		esac
	done
	;;
link)
	thin=no
	compile=no
	for argument do
		case "$argument" in
		-flto=thin) thin=yes ;;
		-fno-lto) thin=no ;;
		-c|-E|-S) compile=yes ;;
		esac
	done
	if [ "$thin" = yes ] && [ "$compile" = no ]; then
		exit 1
	fi
	;;
*)
	for argument do
		if [ ! -f "$argument" ]; then
			continue
		fi
		input=$argument
		if [ "$RELIC_TEST_MODE" = index ]; then
			if ! $RELIC_AR t "$argument" > "$RELIC_TEST_WORK/members" 2>/dev/null; then
				continue
			fi
			# Some archivers include their symbol index in an unqualified
			# print. Inspect named members so that metadata cannot hide IR.
			while IFS= read -r member; do
				$RELIC_AR p "$argument" "$member" > "$RELIC_TEST_WORK/member"
				magic=$(od -An -tx1 -N4 "$RELIC_TEST_WORK/member" | tr -d ' \n')
				case "$magic" in
				4243c0de|dec0170b) exit 1 ;;
				esac
			done < "$RELIC_TEST_WORK/members"
			continue
		fi
		magic=$(od -An -tx1 -N4 "$input" | tr -d ' \n')
		case "$magic" in
		4243c0de|dec0170b) exit 1 ;;
		esac
	done
	;;
esac
exec $RELIC_REAL_TOOL "$@"
EOF
chmod +x "$work/tool"

check_configure() {
	name=$1
	expected=$2
	diagnostic=$3
	shift 3
	mkdir "$work/$name"
	status=0
	# Preliminary configure probes stay native even if the builder supplied
	# LTO flags directly. Enabling the option appends -flto=thin afterward.
	(cd "$work/$name" && "$RELIC_SRCDIR/configure" \
	    CC="$RELIC_CC" CXX="$RELIC_CXX" AR="$RELIC_AR" \
	    RANLIB="$RELIC_RANLIB" NM="$RELIC_NM" \
	    CFLAGS="$RELIC_CFLAGS -fno-lto" CXXFLAGS="$RELIC_CXXFLAGS -fno-lto" \
	    CPPFLAGS="$RELIC_CPPFLAGS" LDFLAGS="$RELIC_LDFLAGS" \
	    LIBS="$RELIC_LIBS" --enable-thinlto "$@") \
	    > "$work/$name.log" 2>&1 || status=$?
	if [ "$expected" = failure ]; then
		if [ "$status" -eq 0 ] || ! grep -q -- "$diagnostic" "$work/$name.log"; then
			cat "$work/$name.log" >&2
			echo "$name: configure must refuse $diagnostic" >&2
			exit 1
		fi
	elif [ "$status" -ne 0 ]; then
		cat "$work/$name.log" >&2
		echo "$name: configure must succeed" >&2
		exit 1
	fi
}

check_configure invalid failure 'accepts yes or no' --enable-thinlto=invalid
check_configure disabled success '' --disable-thinlto
if ! grep -q '^RELIC_LTO_CFLAGS = *$' "$work/disabled/Makefile"; then
	echo "--disable-thinlto must leave RELIC_LTO_CFLAGS empty" >&2
	exit 1
fi

RELIC_TEST_WORK=$work
export RELIC_TEST_WORK
for mode in ignore archive index symbols link cxx; do
	RELIC_TEST_MODE=$mode
	export RELIC_TEST_MODE
	case "$mode" in
	ignore|link)
		RELIC_REAL_TOOL=$RELIC_CC
		setting="CC=$work/tool"
		if [ "$mode" = ignore ]; then
			diagnostic='compiler did not emit LLVM bitcode'
		else
			diagnostic='linker cannot consume ThinLTO archives'
		fi
		;;
	archive)
		RELIC_REAL_TOOL=$RELIC_AR
		setting="AR=$work/tool"
		diagnostic='AR cannot archive LLVM bitcode'
		;;
	index)
		RELIC_REAL_TOOL=$RELIC_RANLIB
		setting="RANLIB=$work/tool"
		diagnostic='RANLIB cannot index LLVM bitcode'
		;;
	symbols)
		RELIC_REAL_TOOL=$RELIC_NM
		setting="NM=$work/tool"
		diagnostic='NM cannot read LLVM bitcode'
		;;
	cxx) RELIC_REAL_TOOL=$RELIC_CXX; setting="CXX=$work/tool" ;;
	esac
	export RELIC_REAL_TOOL
	if [ "$mode" = cxx ]; then
		check_configure "$mode" success '' "$setting"
		if ! grep -q 'C++ header test *no' "$work/$mode.log"; then
			cat "$work/$mode.log" >&2
			echo "an incompatible C++ compiler must omit its header test" >&2
			exit 1
		fi
	else
		check_configure "$mode" failure "$diagnostic" "$setting"
	fi
done
