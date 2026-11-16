// App lab: TLS in front of a lab relay (docker-compose.app-lab.yml tls-a / tls-b), so its https:// origin can go on
// account cards (multi-relay MR004 takes no http:// origin). Terminates TLS with the lab's certificate (signed by the
// lab CA pp_local_test.sh makes; the probes trust it through PP_BROWSER_CA_BUNDLE) and passes each request through
// to the relay (its local stack's listener) as it came. The relay gone: 502.
//
// Lab-only recording (multi-relay MR007, replayed_calls_refused_at_other_relay): with RECORD_DIR set, while the file
// RECORD_DIR/on exists each request is appended to RECORD_DIR/requests.jsonl as {method, path, body} — the harness
// replays the bodies at the other relay. Nothing is recorded otherwise.
//
//   node proxy.js <listen port> <upstream host> <upstream port>   (certificate and key in /tls)
"use strict";
const fs = require("fs");
const http = require("http");
const https = require("https");

const [listenPort, upstreamHost, upstreamPort] = process.argv.slice(2);
const tls = { key: fs.readFileSync("/tls/server.key"), cert: fs.readFileSync("/tls/server.crt") };
const recordDir = process.env.RECORD_DIR || "";

function record(req) {
  if (!recordDir || !fs.existsSync(`${recordDir}/on`)) {
    return;
  }
  const chunks = [];
  req.on("data", (chunk) => chunks.push(chunk));
  req.on("end", () => {
    const line = JSON.stringify({ method: req.method, path: req.url, body: Buffer.concat(chunks).toString("utf8") });
    fs.appendFileSync(`${recordDir}/requests.jsonl`, `${line}\n`);
  });
}

https
  .createServer(tls, (req, res) => {
    record(req);
    const upstream = http.request(
      { host: upstreamHost, port: Number(upstreamPort), method: req.method, path: req.url, headers: req.headers },
      (answer) => {
        res.writeHead(answer.statusCode, answer.headers);
        answer.pipe(res);
      },
    );
    upstream.on("error", () => {
      if (!res.headersSent) {
        res.writeHead(502, { "content-type": "application/json" });
      }
      res.end('{"error":"relay unreachable"}');
    });
    req.pipe(upstream);
  })
  .listen(Number(listenPort), "0.0.0.0");
