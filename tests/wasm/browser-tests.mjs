#!/usr/bin/env node
/* Copyright (c) 2025-2026 Stanislav Sedov. SPDX-License-Identifier: BSD-2-Clause */

import assert from "node:assert/strict";
import { createServer } from "node:http";
import { readFile, mkdtemp, rm } from "node:fs/promises";
import { spawn } from "node:child_process";
import { tmpdir } from "node:os";
import { resolve, join, sep, extname } from "node:path";
import { fileURLToPath } from "node:url";
import { examples } from "../../website/assets/examples.mjs";
import { busy_program } from "./api-tests.mjs";

const root = fileURLToPath(new URL("../../", import.meta.url));
const browser_path = process.argv[2] ?? process.env.CHROME_BIN;
assert(browser_path, "Pass the Chrome/Chromium/Brave executable or set CHROME_BIN.");

const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
async function wait_until(read, description, timeout = 15000) {
	const deadline = Date.now() + timeout;
	while (Date.now() < deadline) {
		const value = await read();
		if (value)
			return value;
		await delay(50);
	}
	throw new Error("Timed out: " + description);
}

class DevTools {
	constructor(socket) {
		this.socket = socket;
		this.next_id = 0;
		this.pending = new Map();
		socket.onmessage = event => {
			const message = JSON.parse(event.data);
			const entry = this.pending.get(message.id);
			if (!entry)
				return;
			this.pending.delete(message.id);
			clearTimeout(entry.timer);
			if (message.error)
				entry.reject(new Error(JSON.stringify(message.error)));
			else
				entry.resolve(message.result);
		};
		socket.onclose = () => {
			for (const entry of this.pending.values()) {
				clearTimeout(entry.timer);
				entry.reject(new Error("Browser connection closed."));
			}
			this.pending.clear();
		};
	}

	call(method, params = {}) {
		return new Promise((resolve, reject) => {
			const id = ++this.next_id;
			const timer = setTimeout(() => {
				this.pending.delete(id);
				reject(new Error("DevTools deadline: " + method));
			}, 30000);
			this.pending.set(id, { resolve, reject, timer });
			this.socket.send(JSON.stringify({ id, method, params }));
		});
	}

	async evaluate(expression) {
		const result = await this.call("Runtime.evaluate", {
			expression, returnByValue: true, awaitPromise: true
		});
		if (result.exceptionDetails)
			throw new Error(JSON.stringify(result.exceptionDetails));
		return result.result.value;
	}
}

// Serve only test/site files on loopback; the repository's other files,
// including credentials or .git, are not browser-test server resources.
const server = createServer(async (request, response) => {
	try {
		const pathname = decodeURIComponent(new URL(request.url, "http://localhost").pathname);
		const path = resolve(root, "." + pathname);
		assert(["website", "tests/wasm"].some(directory =>
		    path.startsWith(join(root, directory) + sep)));
		const content = await readFile(path);
		const types = { ".html": "text/html", ".mjs": "text/javascript",
		    ".js": "text/javascript", ".css": "text/css", ".wasm": "application/wasm",
		    ".json": "application/json" };
		response.writeHead(200, { "Content-Type": types[extname(path)] ?? "application/octet-stream" });
		response.end(content);
	} catch {
		response.writeHead(404);
		response.end();
	}
});
await new Promise(resolve => server.listen(0, "127.0.0.1", resolve));
const origin = "http://127.0.0.1:" + server.address().port;
const profile = await mkdtemp(join(tmpdir(), "relic-browser-"));
const browser = spawn(browser_path, ["--headless", "--disable-gpu",
	"--no-first-run", "--no-default-browser-check", "--disable-background-networking",
	"--disable-component-update", "--disable-sync", "--disable-extensions",
	"--remote-debugging-port=0", "--user-data-dir=" + profile,
	...(process.platform === "linux" ? ["--no-sandbox"] : []), "about:blank"],
	{ stdio: ["ignore", "ignore", "pipe"] });
const browser_closed = new Promise(resolve => browser.once("close", resolve));
let browser_log = "", client;
browser.stderr.on("data", bytes => { browser_log = (browser_log + bytes).slice(-65536); });
browser.on("error", error => { browser_log += error.message; });

try {
	const port = await wait_until(async () => {
		try { return (await readFile(join(profile, "DevToolsActivePort"), "utf8")).split("\n")[0]; }
		catch { return null; }
	}, "browser startup");
	const tabs = await (await fetch("http://127.0.0.1:" + port + "/json/list")).json();
	const socket = new WebSocket(tabs.find(tab => tab.type === "page").webSocketDebuggerUrl);
	await new Promise((resolve, reject) => {
		const timer = setTimeout(() => {
			socket.close();
			reject(new Error("Timed out opening the browser debugging connection."));
		}, 5000);
		socket.onopen = () => { clearTimeout(timer); resolve(); };
		socket.onerror = () => { clearTimeout(timer); reject(new Error("Browser connection failed.")); };
	});
	client = new DevTools(socket);
	await client.call("Page.navigate", { url: origin + "/tests/wasm/browser.html" });
	await wait_until(() => client.evaluate("typeof window.test_result !== 'undefined'"), "API test loading");
	const api = await client.evaluate("window.test_result");
	assert(api.passed, api.error);
	console.log("PASS: " + api.count + " real-browser API scenarios");

	await client.call("Page.navigate", { url: origin + "/website/playground.html" });
	await wait_until(() => client.evaluate("document.getElementById('run')?.disabled === false"), "playground startup");
	const layout = () => client.evaluate(`(() => {
		const editor = document.querySelector('.playground-editor').getBoundingClientRect();
		const output = document.querySelector('.playground-output').getBoundingClientRect();
		const row = document.querySelector('.playground-output-row').getBoundingClientRect();
		const run = document.getElementById('run').getBoundingClientRect();
		const limits = document.getElementById('run-limits').getBoundingClientRect();
		return {
			editor: { left: editor.left, right: editor.right, bottom: editor.bottom, height: editor.height },
			output: { top: output.top, left: output.left, right: output.right, bottom: output.bottom, height: output.height },
			row_right: row.right, run_left: run.left, run_top: run.top,
			limits_top: limits.top,
			fits: document.documentElement.scrollWidth <= innerWidth
		};
	})()`);
	await client.call("Emulation.setDeviceMetricsOverride", {
		width: 1280, height: 900, deviceScaleFactor: 1, mobile: false
	});
	const desktop = await layout();
	assert(desktop.output.top >= desktop.editor.bottom &&
	    desktop.output.top - desktop.editor.bottom <= 24, "output sits immediately below code");
	assert(Math.abs(desktop.output.left - desktop.editor.left) < 1 &&
	    Math.abs(desktop.row_right - desktop.editor.right) < 1, "output and controls align with code edges");
	assert(desktop.run_left >= desktop.output.right &&
	    Math.abs(desktop.run_top - desktop.output.top) < 1, "Run sits to the right of output");
	assert(desktop.output.height < desktop.editor.height, "output window is compact");
	assert(desktop.fits, "desktop content fits viewport");
	assert.equal(await client.evaluate("document.getElementById('result-placeholder').hidden"), false);
	await client.call("Emulation.setFocusEmulationEnabled", { enabled: true });
	await client.call("Emulation.setEmulatedMedia", {
		features: [{ name: "forced-colors", value: "active" }]
	});
	const focus = await client.evaluate(`(() => {
		const program = document.getElementById('program');
		program.focus();
		const style = getComputedStyle(program);
		return { outline: style.outlineStyle, width: style.outlineWidth,
			focused: program.matches(':focus'), visible: program.matches(':focus-visible') };
	})()`);
	assert(focus.outline !== "none" && parseFloat(focus.width) >= 2,
	    "keyboard focus remains visible in forced-colors mode: " + JSON.stringify(focus));
	await client.call("Emulation.setEmulatedMedia", { features: [] });
	await client.evaluate("document.getElementById('program').blur()");
	await client.call("Emulation.setFocusEmulationEnabled", { enabled: false });
	await client.call("Emulation.setDeviceMetricsOverride", {
		width: 390, height: 844, deviceScaleFactor: 1, mobile: false
	});
	const mobile = await layout();
	assert(mobile.output.top >= mobile.editor.bottom, "mobile output follows editor");
	assert(mobile.run_left >= mobile.output.right, "mobile Run remains beside output");
	assert(mobile.output.bottom <= mobile.limits_top, "mobile output precedes explanatory text");
	assert(mobile.fits, "mobile content fits viewport");
	const submit = async program => {
		await client.evaluate("document.getElementById('program').value = " + JSON.stringify(program) +
		    "; document.getElementById('playground').requestSubmit()");
	};
	const finished = () => wait_until(() => client.evaluate(
	    "document.getElementById('run').disabled === false"), "playground completion");
	const expect_answers = async sample => {
		const texts = await client.evaluate("[...document.querySelectorAll('#results pre')].map(node => node.textContent)");
		const expected = sample.answers.map(rows => rows.length ? rows.slice().sort() : ["No rows."]);
		assert.deepEqual(texts.map(text => text.split("\n").sort()), expected, sample.name);
	};
	await submit('p("' + "a".repeat(3000) + '"). p(X)?');
	await finished();
	assert((await layout()).fits, "long result lines do not widen the mobile page");
	assert.equal(await client.evaluate("document.getElementById('result-placeholder').hidden"), true);
	await client.call("Emulation.setDeviceMetricsOverride", {
		width: 1280, height: 900, deviceScaleFactor: 1, mobile: false
	});
	for (let i = 0; i < examples.length; i++) {
		await client.evaluate(`document.getElementById('example').value = '${i}';` +
		    "document.getElementById('example').dispatchEvent(new Event('change'));" +
		    "document.getElementById('playground').requestSubmit()");
		await finished();
		await expect_answers(examples[i]);
	}
	for (const id of ["subway-routes", "unused-functions", "type-inference", "unknown-example"]) {
		const sample = examples.find(example => example.id === id) ?? examples[0];
		await client.call("Page.navigate", { url: origin + "/website/playground.html?example=" + id });
		await wait_until(() => client.evaluate("document.getElementById('run')?.disabled === false"), "linked example loading");
		assert.equal(await client.evaluate("document.getElementById('program').value"), sample.program);
		assert.equal(await client.evaluate("document.getElementById('status').textContent"), "Ready.",
		    "links select a bundled example without executing it");
		await submit(sample.program);
		await finished();
		await expect_answers(sample);
	}
	await submit(String.raw`v("\0\xff\"\\"). v(X)?`);
	await finished();
	assert.equal(await client.evaluate("document.querySelector('#results pre').textContent"),
	    String.raw`"\0\xff\"\\"`, "byte escaping in actual DOM");
	await submit("v(9223372036854775807). v(X)?");
	await finished();
	assert.equal(await client.evaluate("document.querySelector('#results pre').textContent"),
	    "9223372036854775807", "exact integer rendering");
	await submit('p("<img src=x onerror=alert(1)>"). p(X)?');
	await finished();
	assert.equal(await client.evaluate("document.querySelectorAll('#results img').length"), 0);
	assert((await client.evaluate("document.getElementById('results').textContent")).includes("<img"));
	await submit("p(.");
	await finished();
	assert((await client.evaluate("document.getElementById('diagnostics').textContent")).includes("1:"));
	assert.equal(await client.evaluate("document.getElementById('results-section').hidden"), true);
	assert.equal(await client.evaluate("document.getElementById('result-placeholder').hidden"), true);
	await submit("p(a).");
	await finished();
	assert.match(await client.evaluate("document.getElementById('result-placeholder').textContent"), /query ending in/);
	await client.call("Emulation.setDeviceMetricsOverride", {
		width: 390, height: 844, deviceScaleFactor: 1, mobile: false
	});
	await submit("p(" + "W".repeat(43) + ")?");
	await finished();
	assert((await layout()).fits, "long diagnostics fit the mobile page");
	await client.call("Emulation.setDeviceMetricsOverride", {
		width: 1280, height: 900, deviceScaleFactor: 1, mobile: false
	});

	await submit(busy_program);
	assert.equal(await client.evaluate("document.getElementById('status').textContent"), "Running...");
	await client.evaluate("setTimeout(() => { document.body.dataset.responsive = 'yes'; }, 10)");
	await wait_until(() => client.evaluate("document.body.dataset.responsive === 'yes'"), "responsive UI during evaluation");
	await client.evaluate("document.getElementById('stop').click()");
	await finished();
	assert.match(await client.evaluate("document.getElementById('status').textContent"), /cancelled/i);
	await submit(busy_program);
	await finished();
	assert.match(await client.evaluate("document.getElementById('status').textContent"), /time limit/);
	await submit("n(0). n(Y) :- n(X), X < 1100, Y = X + 1. n(X)?");
	await finished();
	assert.match(await client.evaluate("document.getElementById('status').textContent"), /row limit/);
	assert.equal(await client.evaluate("document.getElementById('results-section').hidden"), true);
	await submit("p(ok). p(X)?");
	await finished();
	assert.equal(await client.evaluate("document.querySelector('#results pre').textContent"), "ok");
	console.log("PASS: playground examples, exact rendering, diagnostics, responsiveness, Stop, timeout, limits and recovery");
	console.log("PASS: compact output below code, Run placement, mobile containment and output states");

	for (const page of ["index", "quickstart", "tutorial", "c-api", "performance"]) {
		await client.call("Page.navigate", { url: origin + "/website/" + page + ".html" });
		await wait_until(() => client.evaluate("document.readyState === 'complete' && " +
		    "location.pathname.endsWith('/" + page + ".html')"), page + " loading");
		if (page === "tutorial") {
			const snippets = await client.evaluate("[...document.querySelectorAll('[data-playground-example]')].map(node => ({ id: node.dataset.playgroundExample, program: node.textContent }))");
			assert.equal(snippets.length, 3, "tutorial contains the three worked application examples");
			for (const snippet of snippets) {
				const sample = examples.find(example => example.id === snippet.id);
				assert(sample, "tutorial example has a matching playground record: " + snippet.id);
				assert.equal(snippet.program, sample.program, "tutorial/playground program agreement: " + snippet.id);
			}
		}
		for (const width of [1280, 390]) {
			await client.call("Emulation.setDeviceMetricsOverride", {
				width, height: 900, deviceScaleFactor: 1, mobile: false
			});
			assert.equal(await client.evaluate("document.documentElement.scrollWidth <= innerWidth"),
			    true, page + ": content fits " + width + "px viewport");
		}
	}
	await wait_until(() => client.evaluate("document.getElementById('chart-status')?.hidden === true"),
	    "performance chart data");
	for (const id of ["serial-chart", "parallel-chart"])
		assert.equal(await client.evaluate("document.querySelectorAll('#" + id + " g[data-workload]').length"), 31);
	for (const id of ["serial-timings", "parallel-timings"])
		assert.equal(await client.evaluate("document.querySelectorAll('#" + id + " tr').length"), 31);
	assert.equal(await client.evaluate(`(() => {
		const chart = document.getElementById('parallel-chart');
		const parity = Number(chart.querySelector('.chart-parity').getAttribute('x1'));
		const faster = chart.querySelector('[data-workload="chain-closure"]');
		const slower = chart.querySelector('[data-workload="tc-cyclic-bf"]');
		return Number(faster.dataset.ratio) > 2.8 && Number(faster.dataset.ratio) < 2.9 &&
		    Number(faster.querySelector('circle').getAttribute('cx')) > parity &&
		    Number(slower.querySelector('circle').getAttribute('cx')) < parity;
	})()`), true, "ratio direction reflects both faster and slower workloads");
	const serial_ratio = await client.evaluate("Number(document.querySelector('#serial-chart [data-workload=chain-closure]').dataset.ratio)");
	assert(serial_ratio > 2 && serial_ratio < 2.1, "one-thread chart uses its own measurements");
	assert((await client.evaluate("document.getElementById('serial-timings').textContent")).includes("timeout"),
	    "unavailable measurements remain statuses in the timing table");
	await delay(100);
	assert.equal(await client.evaluate("document.documentElement.scrollWidth <= innerWidth"), true);
	assert.equal(await client.evaluate(`(() => {
		const chart = document.getElementById('serial-chart');
		return Math.abs(chart.viewBox.baseVal.width - chart.clientWidth) < 1;
	})()`), true, "mobile SVG coordinates preserve readable text size");
	await client.call("Network.enable");
	await client.call("Network.setBlockedURLs", { urls: ["*benchmark-data.json"] });
	await client.call("Page.reload");
	await wait_until(() => client.evaluate("document.getElementById('chart-status')?.textContent.includes('unavailable')"),
	    "missing chart asset fallback");
	assert.equal(await client.evaluate("document.getElementById('chart-status').hidden"), false);
	assert.equal(await client.evaluate("document.getElementById('methods').textContent"),
	    "Relic optimization techniques");
	await client.call("Network.setBlockedURLs", { urls: [] });
	console.log("PASS: all-page responsive layout, fixed 1/16-thread charts, ratio directions, timing table and missing-data fallback");
	await client.call("Browser.close");
} catch (error) {
	console.error(browser_log);
	throw error;
} finally {
	client?.socket.close();
	const force_shutdown = setTimeout(() => browser.kill("SIGKILL"), 2000);
	browser.kill();
	await browser_closed;
	clearTimeout(force_shutdown);
	await new Promise(resolve => server.close(resolve));
	await rm(profile, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 });
}
