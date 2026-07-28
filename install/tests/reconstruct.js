/* Run the built page's own decoder in a sandbox and write out the bytes its
   shellcode string occupies in memory, which is what the stub will find.
   Usage: node reconstruct.js <index.html> <image.b64> <out.bin> */

const fs = require("fs");
const vm = require("vm");

const [, , pagePath, b64Path, outPath] = process.argv;
const html = fs.readFileSync(pagePath, "utf8");

const scripts = [...html.matchAll(/<script[^>]*>([\s\S]*?)<\/script>/g)].map((m) => m[1]);
if (scripts.length < 2) throw new Error("page has no inlined chain and page script");

/* One node per id, kept, so the page's own readiness decision can be read back out. */
const nodes = {};
const node = (id) => (nodes[id] = nodes[id] || { innerHTML: "", className: "", disabled: false });
const sandbox = {
    document: { getElementById: node },
    window: { location: { search: "" }, setTimeout: () => {} },
    ActiveXObject: function () { return {}; },
    CollectGarbage: () => {},
    String, Array, Math, Number,
};
sandbox.window.String = String;
vm.createContext(sandbox);
for (const s of scripts) vm.runInContext(s, sandbox);

const decoded = sandbox.b64ToString(fs.readFileSync(b64Path, "utf8"));
const shellcode = sandbox.stubString() + decoded.text;

const buf = Buffer.alloc(shellcode.length * 2);
for (let i = 0; i < shellcode.length; i++) buf.writeUInt16LE(shellcode.charCodeAt(i), i * 2);
fs.writeFileSync(outPath, buf);

console.log(JSON.stringify({
    reportedBytes: decoded.bytes,
    expectedBytes: sandbox.IMAGE_BYTES,
    stubBytes: sandbox.STUB.length,
    units: shellcode.length,
    blocked: sandbox.blocked,
    msgHtml: nodes.msg ? nodes.msg.innerHTML : "",
    goClass: nodes.go ? nodes.go.className : null,
    maxTries: sandbox.MAX_TRIES,
}));
