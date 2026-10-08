# Read one pretty-printed hyperfine JSON result, in schema 1 or schema 2.
# Schema 1 stores statistics beside arrays of samples; schema 2 groups
# statistics by metric under summary, with metric keys indented eight spaces.
# Samples do not set summary values.

function number(value) {
	return value ~ /^[0-9]+([.][0-9]+)?([eE][+-]?[0-9]+)?$/
}

/"schema_version":/ { schema = $2 + 0 }
/"command":/ { commands++ }

schema == 2 {
	if ($0 ~ /"summary":/)
		in_summary = 1
	if (!in_summary)
		next
	if ($0 ~ /^        "[^"]+": \{/ ) {
		metric = $1
		gsub(/[":]/, "", metric)
	}
	value = $2
	gsub(/,/, "", value)
	if (metric == "time_wall_clock") {
		if ($1 == "\"median\":") median = value
		if ($1 == "\"min\":") fastest = value
		if ($1 == "\"stddev\":") spread = value
	}
	if (metric == "time_user" && $1 == "\"mean\":")
		user = value
	if (metric == "memory_peak_resident" && $1 == "\"max\":")
		peak = value
	next
}

/"median":/ { median = $2 }
/"min":/ { fastest = $2 }
/"stddev":/ { spread = $2 }
/"user":/ { user = $2 }
/"memory_usage_byte":/ { in_memory = 1; next }
in_memory && /]/ { in_memory = 0 }
in_memory {
	value = $1
	gsub(/,/, "", value)
	if (!number(value))
		invalid = 1
	if (peak == "" || value + 0 > peak + 0)
		peak = value
}

END {
	gsub(/,/, "", median)
	gsub(/,/, "", fastest)
	gsub(/,/, "", spread)
	gsub(/,/, "", user)
	if (commands != 1 || invalid || (schema != 0 && schema != 2) ||
	    !number(median) || !number(fastest) || !number(user) ||
	    !number(peak) || (spread != "null" && !number(spread)))
		exit 1
	if (spread == "null")
		spread = "-"
	print median, fastest, spread, user, peak
}
