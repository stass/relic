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
		    ".js": "text/javascript", ".css": "text/css", ".wasm": "application/wasm" };
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
	const submit = async program => {
		await client.evaluate("document.getElementById('program').value = " + JSON.stringify(program) +
		    "; document.getElementById('playground').requestSubmit()");
	};
	const finished = () => wait_until(() => client.evaluate(
	    "document.getElementById('run').disabled === false"), "playground completion");
	for (let i = 0; i < examples.length; i++) {
		await client.evaluate(`document.getElementById('example').value = '${i}';` +
		    "document.getElementById('example').dispatchEvent(new Event('change'));" +
		    "document.getElementById('playground').requestSubmit()");
		await finished();
		const text = await client.evaluate("document.querySelector('#results pre')?.textContent");
		assert.deepEqual(text?.split("\n").sort(), examples[i].answer.slice().sort(), examples[i].name);
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

	await submit(busy_program);
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
