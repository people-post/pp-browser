// App lab: TLS in front of a lab relay (docker-compose.app-lab.yml tls-a / tls-b), so its https:// origin can go on
// account cards (multi-relay MR004 takes no http:// origin). Terminates TLS with the lab's certificate (signed by the
// lab CA pp_local_test.sh makes; the probes trust it through PP_BROWSER_CA_BUNDLE) and passes each request through
// to the relay's gateway as it came. The relay gone: 502.
//
//   node proxy.js <listen port> <upstream host> <upstream port>   (certificate and key in /tls)
"use strict";
const fs = require("fs");
const http = require("http");
const https = require("https");

const [listenPort, upstreamHost, upstreamPort] = process.argv.slice(2);
const tls = { key: fs.readFileSync("/tls/server.key"), cert: fs.readFileSync("/tls/server.crt") };

https
  .createServer(tls, (req, res) => {
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
