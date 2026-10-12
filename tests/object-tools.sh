#!/bin/sh
#
# Prepare one built object for the structural audits. LLVM bitcode has no
# native sections; lower it in private scratch space with the configured
# compiler before classifying storage or target-specific symbol names.
#
# audit_object object scratch-directory sets audit_native, audit_format and
# audit_listing. Every tool failure is returned to the caller. The scratch
# directory belongs to the caller and may be reused between objects.

: "${RELIC_CC:=cc}"
: "${RELIC_CFLAGS:=}"
: "${RELIC_NM:=nm}"

audit_nm_available() (
	set -f
	# shellcheck disable=SC2086
	set -- $RELIC_NM
	command -v "$1" >/dev/null 2>&1
)

audit_magic() {
	od -An -tx1 -N4 "$1" > "$2/magic" || return 1
	tr -d ' \n' < "$2/magic"
}

audit_object() {
	audit_input=$1
	audit_work=$2
	audit_native=$audit_input
	audit_listing="$audit_work/symbols"
	audit_bitcode=no
	audit_header=$(audit_magic "$audit_input" "$audit_work") || return 1
	case "$audit_header" in
	4243c0de|dec0170b)
		audit_bitcode=yes
		# Compiler commands and flags follow configure's word-splitting
		# contract. Paths are quoted; flags preserve the target and ABI.
		# The final flags prevent another LTO or optimization pass.
		# shellcheck disable=SC2086
		if ! $RELIC_CC $RELIC_CFLAGS -O0 -fno-lto -c -x ir \
		    "$audit_input" -o "$audit_work/native.o" \
		    > "$audit_work/compiler.log" 2>&1; then
			cat "$audit_work/compiler.log" >&2
			return 1
		fi
		audit_native="$audit_work/native.o"
		audit_header=$(audit_magic "$audit_native" "$audit_work") || return 1
		;;
	esac

	# The file header determines the format even when its only symbols
	# are common or undefined and nm prints no section-qualified names.
	case "$audit_header" in
	7f454c46)
		audit_format=elf
		$RELIC_NM --format=sysv "$audit_native" > "$audit_listing" || return 1
		;;
	feedface|cefaedfe|feedfacf|cffaedfe|cafebabe|bebafeca|cafebabf|bfbafeca)
		audit_format=macho
		$RELIC_NM -m "$audit_native" > "$audit_listing" || return 1
		;;
	*)
		echo "$audit_input: unsupported or damaged object format" >&2
		return 1
		;;
	esac

	if [ "$audit_bitcode" = no ]; then
		return 0
	fi

	# A conversion may not make a suspect definition disappear. Compare
	# data candidates with all native definitions, since a constant's type
	# can change when it is assigned a native read-only section.
	$RELIC_NM -P "$audit_input" > "$audit_work/before" || return 1
	$RELIC_NM -P "$audit_native" > "$audit_work/after" || return 1
	awk -v format="$audit_format" '
	FILENAME == ARGV[1] {
		if ($2 !~ /^[BbCcDdGgSsVu]$/)
			next
		name = $1
		if (format == "macho") {
			if (substr(name, 1, 1) != "_")
				next
			name = substr(name, 2)
		}
		if (name ~ /^\./ || name ~ /^(__|_[A-Z])/)
			next
		candidate[$1] = 1
		next
	}
	$2 !~ /^[Uwv?]$/ { delete candidate[$1] }
	END {
		for (name in candidate) {
			print "native conversion lost data symbol: " name
			failed = 1
		}
		exit failed
	}' "$audit_work/before" "$audit_work/after" >&2
}
