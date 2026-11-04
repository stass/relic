/* Copyright (c) 2025-2026 Stanislav Sedov. SPDX-License-Identifier: BSD-2-Clause */

export function format_value(cell) {
	if (cell.type === "integer")
		return cell.value.toString();
	if (cell.type === "symbol")
		return cell.value;
	const escapes = new Map([[0, "\\0"], [9, "\\t"], [10, "\\n"],
	    [13, "\\r"], [34, '\\"'], [92, "\\\\"]]);
	const parts = ['"'];
	for (const byte of cell.value) {
		if (escapes.has(byte))
			parts.push(escapes.get(byte));
		else if (byte >= 32 && byte < 127)
			parts.push(String.fromCharCode(byte));
		else
			parts.push("\\x" + byte.toString(16).padStart(2, "0"));
	}
	parts.push('"');
	return parts.join("");
}

export function format_rows(rows) {
	if (rows.length === 0)
		return "No rows.";
	return rows.map(row => row.length === 0 ? "true" :
	    row.map(format_value).join("\t")).join("\n");
}
