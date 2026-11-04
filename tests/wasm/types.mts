import { evaluate, limits, RelicError, type Value } from "../../wasm/relic.mjs";

const result = await evaluate("p(a). p(X)?", { maxRows: limits.maxRows });
for (const query of result.queries) {
	for (const row of query.rows) {
		for (const cell of row) {
			if (cell.type === "integer") {
				const exact: bigint = cell.value + 1n;
			} else if (cell.type === "string") {
				const bytes: Uint8Array = cell.value;
			} else {
				const name: string = cell.value;
			}
		}
	}
}
const error = new RelicError("limit", "test");
const diagnostics = error.diagnostics;
// @ts-expect-error Integers cannot be JavaScript numbers.
const lossy: Value = { type: "integer", value: 1 };
// @ts-expect-error The option name is part of the public contract.
await evaluate("p(a).", { maxRow: 1 });
