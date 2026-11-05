/* Copyright (c) 2025-2026 Stanislav Sedov. SPDX-License-Identifier: BSD-2-Clause */

const SVG = "http://www.w3.org/2000/svg";
const status = document.getElementById("chart-status");
const serial = document.getElementById("serial-chart");
const parallel = document.getElementById("parallel-chart");
const serial_table = document.getElementById("serial-timings");
const parallel_table = document.getElementById("parallel-timings");

function svg_node(name, attributes, text) {
	const node = document.createElementNS(SVG, name);
	for (const [key, value] of Object.entries(attributes))
		node.setAttribute(key, value);
	if (text !== undefined)
		node.textContent = text;
	return node;
}

function time(seconds) {
	if (seconds === null)
		return "Unavailable";
	const unit = seconds >= 1 ? "s" : seconds >= .001 ? "ms" : "us";
	const value = seconds * (unit === "s" ? 1 : unit === "ms" ? 1000 : 1000000);
	return Number(value.toPrecision(3)) + " " + unit;
}

function ratio(value) {
	return value.toFixed(value > .8 && value < 1.25 ? 2 : 1) + "\u00d7";
}

/* A chart and its table consume these same records; unavailable cells have
 * no numeric ratio and never acquire an invented time or a zero-length bar. */
function comparisons(data, names, thread_count) {
	return names.map(workload => {
		const engines = data.profiles[thread_count][workload];
		const rivals = Object.entries(engines).filter(([name, cell]) =>
		    name !== "relic" && cell.status === "ok");
		rivals.sort((a, b) => a[1].seconds - b[1].seconds);
		const best = rivals[0];
		const measured = engines.relic.seconds;
		const reference = best ? best[1].seconds : null;
		const reference_name = best ? data.engines[best[0]] : "No completed rival";
		return {
			workload, measured, reference, reference_name,
			ratio: reference === null ? null : reference / measured,
			unavailable: Object.entries(engines).filter(([, cell]) => cell.status !== "ok")
			    .map(([name, cell]) => data.engines[name] + ": " + cell.status),
			description: workload + ": relic " + time(measured) +
			    "; " + reference_name + " " + time(reference)
		};
	});
}

function draw_chart(svg, rows) {
	const width = svg.clientWidth;
	if (width <= 0)
		return;
	const narrow = width < 600;
	const row_height = narrow ? 48 : 32;
	const top = 20, bottom = 46;
	const left = narrow ? 24 : 155, right = width - 66;
	const values = rows.filter(row => row.ratio !== null).map(row => row.ratio);
	const low = Math.floor(Math.log2(Math.min(.5, ...values)));
	const high = Math.ceil(Math.log2(Math.max(2, ...values)));
	const x = value => left + (Math.log2(value) - low) / (high - low) * (right - left);
	const plot_bottom = top + rows.length * row_height;
	svg.setAttribute("viewBox", `0 0 ${width} ${plot_bottom + bottom}`);
	svg.replaceChildren();
	for (let power = low; power <= high; power++) {
		const value = 2 ** power;
		if (narrow && power !== low && power !== high && power !== 0 && power % 3 !== 0)
			continue;
		svg.append(svg_node("line", { x1: x(value), x2: x(value), y1: 0, y2: plot_bottom,
		    class: value === 1 ? "chart-parity" : "chart-grid" }));
		svg.append(svg_node("text", { x: x(value), y: plot_bottom + 25,
		    "text-anchor": "middle", class: "chart-tick" }, value + "\u00d7"));
	}
	for (let index = 0; index < rows.length; index++) {
		const row = rows[index];
		const y = top + index * row_height + (narrow ? 22 : 0);
		const group = svg_node("g", { "data-workload": row.workload,
		    "data-ratio": row.ratio === null ? "unavailable" : row.ratio });
		group.append(svg_node("title", {}, row.description));
		group.append(svg_node("text", { x: narrow ? 0 : left - 14,
		    y: narrow ? y - 14 : y + 4, "text-anchor": narrow ? "start" : "end",
		    class: "chart-workload" }, row.workload));
		if (row.ratio === null) {
			group.append(svg_node("text", { x: left, y: y + 4,
			    class: "chart-tick" }, "No completed rival"));
		} else {
			const color = row.ratio >= 1 ? "chart-faster" : "chart-slower";
			group.append(svg_node("line", { x1: x(1), x2: x(row.ratio),
			    y1: y, y2: y, class: "chart-stem " + color }));
			group.append(svg_node("circle", { cx: x(row.ratio), cy: y, r: 5, class: color }));
			group.append(svg_node("text", { x: right + 12, y: y + 4,
			    class: "chart-ratio" }, ratio(row.ratio)));
		}
		svg.append(group);
	}
}

function append_row(body, values) {
	const tr = document.createElement("tr");
	for (const value of values) {
		const td = document.createElement("td");
		td.textContent = value;
		tr.append(td);
	}
	body.append(tr);
}

function render_table(body, rows) {
	body.replaceChildren();
	for (const row of rows)
		append_row(body, [row.workload, time(row.measured), row.reference_name,
		    time(row.reference), row.ratio === null ? "Unavailable" : ratio(row.ratio),
		    row.unavailable.join("; ") || "None"]);
}

async function start() {
	const response = await fetch(new URL("./benchmark-data.json", import.meta.url));
	if (!response.ok)
		throw new Error("Benchmark data could not be loaded.");
	const data = await response.json();
	const parallel_threads = String(data.parallel_threads);
	const names = Object.keys(data.profiles[parallel_threads]);
	const views = [
		{ svg: serial, body: serial_table, rows: comparisons(data, names, "1") },
		{ svg: parallel, body: parallel_table, rows: comparisons(data, names, parallel_threads) }
	];
	for (const view of views) {
		render_table(view.body, view.rows);
		draw_chart(view.svg, view.rows);
	}
	status.hidden = true;
	let widths = [];
	const observer = new ResizeObserver(() => {
		const next = views.map(view => view.svg.clientWidth);
		if (next.some((width, index) => width !== widths[index])) {
			widths = next;
			for (const view of views)
				draw_chart(view.svg, view.rows);
		}
	});
	for (const view of views)
		observer.observe(view.svg.parentElement);
}

start().catch(() => {
	status.hidden = false;
	status.textContent = "Chart data unavailable. See the complete benchmark report below.";
});
