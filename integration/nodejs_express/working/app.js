const http = require("http");
const express = require("express");

const { RSPClient } = require("../../../client/nodejs/rsp_client");
const { createServer: createRSPServer } = require("../../../client/nodejs/rsp_net");

const ENDORSEMENT_SUCCESS = 0;
const ETYPE_ACCESS = "f1e2d3c4-5b6a-7980-1a2b-3c4d5e6f7a8b";
const EVALUE_ACCESS_NETWORK = "f1e2d3c4-5b6a-7980-1a2b-3c4d5e6f7a8b";
const ETYPE_ROLE = "0963c0ab-215f-42c1-b042-747bf21e330e";
const EVALUE_ROLE_RESOURCE_SERVICE = "a7f8c9d6-3b2e-4f1a-8c9d-5e6f7a8b9c0d";

function buildApp() {
  const app = express();

  app.get("/", (_req, res) => {
    res.status(200).type("html").send(`<!doctype html>
<html>
  <head>
    <meta charset="utf-8" />
    <title>RSP Express</title>
  </head>
  <body>
    <h1>Remote Socket Protocol</h1>
    <p>Node.js Express is serving this page through RS/RM transport.</p>
  </body>
</html>`);
  });

  app.get("/healthz", (_req, res) => {
    res.status(200).json({ ok: true, transport: process.env.RSP_TRANSPORT ? "rsp" : "tcp" });
  });

  return app;
}

function adaptSocketForHttp(socket) {
  if (typeof socket.setTimeout !== "function") {
    socket.setTimeout = () => socket;
  }
  if (typeof socket.setNoDelay !== "function") {
    socket.setNoDelay = () => socket;
  }
  if (typeof socket.setKeepAlive !== "function") {
    socket.setKeepAlive = () => socket;
  }
  if (typeof socket.destroySoon !== "function") {
    socket.destroySoon = () => socket.end();
  }
  if (typeof socket.address !== "function") {
    socket.address = () => ({ address: "rsp", family: "RSP", port: 0 });
  }
  if (socket.remoteAddress === undefined) {
    socket.remoteAddress = "rsp";
  }
  if (socket.remotePort === undefined) {
    socket.remotePort = 0;
  }
  if (socket.localAddress === undefined) {
    socket.localAddress = "rsp";
  }
  if (socket.localPort === undefined) {
    socket.localPort = 0;
  }
}

async function startOverRSP(app) {
  const transportSpec = process.env.RSP_TRANSPORT;
  const rsNodeId = process.env.RSP_RESOURCE_SERVICE_NODE_ID;
  const endorsementNodeId = process.env.RSP_ENDORSEMENT_NODE_ID;
  const hostPort = process.env.RSP_HOST_PORT || "127.0.0.1:8080";

  if (!transportSpec || !rsNodeId) {
    throw new Error("RSP_TRANSPORT and RSP_RESOURCE_SERVICE_NODE_ID are required in RSP mode");
  }

  const client = new RSPClient();
  await client.connect(transportSpec);

  if (endorsementNodeId) {
    const reachable = await client.ping(endorsementNodeId, 3000);
    if (!reachable) {
      throw new Error("endorsement service is unreachable");
    }

    let accessReply = null;
    for (let attempt = 0; attempt < 3 && (!accessReply || accessReply.status !== ENDORSEMENT_SUCCESS); attempt += 1) {
      accessReply = await client.beginEndorsementRequest(
        endorsementNodeId,
        ETYPE_ACCESS,
        EVALUE_ACCESS_NETWORK
      );
    }
    if (!accessReply || accessReply.status !== ENDORSEMENT_SUCCESS) {
      throw new Error(`failed to acquire access endorsement (status=${accessReply ? accessReply.status : "null"})`);
    }

    let roleReply = null;
    for (let attempt = 0; attempt < 3 && (!roleReply || roleReply.status !== ENDORSEMENT_SUCCESS); attempt += 1) {
      roleReply = await client.beginEndorsementRequest(
        endorsementNodeId,
        ETYPE_ROLE,
        EVALUE_ROLE_RESOURCE_SERVICE
      );
    }
    if (!roleReply || roleReply.status !== ENDORSEMENT_SUCCESS) {
      throw new Error(`failed to acquire role endorsement (status=${roleReply ? roleReply.status : "null"})`);
    }
  }

  const httpServer = http.createServer(app);
  const rspServer = await createRSPServer(client, rsNodeId, hostPort, { asyncAccept: true, childrenAsyncData: true });

  rspServer.on("connection", (socket) => {
    console.error("[express-rsp] incoming RSP connection");
    adaptSocketForHttp(socket);
    socket.on("data", (chunk) => {
      console.error(`[express-rsp] socket data bytes=${chunk.length}`);
    });
    socket.on("end", () => {
      console.error("[express-rsp] socket end");
    });
    socket.on("close", () => {
      console.error("[express-rsp] socket close");
    });
    socket.on("error", (error) => {
      console.error(`[express-rsp] socket error: ${error.message}`);
    });
    httpServer.emit("connection", socket);
    if (typeof socket.resume === "function") {
      socket.resume();
    }
  });

  httpServer.on("request", (req) => {
    console.error(`[express-rsp] http request ${req.method} ${req.url}`);
  });

  const shutdown = async () => {
    await rspServer.close().catch(() => {});
    await client.close().catch(() => {});
    process.exit(0);
  };

  process.on("SIGINT", shutdown);
  process.on("SIGTERM", shutdown);

  console.log(`Express over RSP listening on ${hostPort}`);
}

function startOverTCP(app) {
  const port = Number.parseInt(process.env.PORT || "3000", 10);
  const server = app.listen(port, () => {
    console.log(`Express server listening on port ${port}`);
  });

  const shutdown = () => {
    server.close(() => process.exit(0));
  };

  process.on("SIGINT", shutdown);
  process.on("SIGTERM", shutdown);
}

async function main() {
  const app = buildApp();
  if (process.env.RSP_TRANSPORT) {
    await startOverRSP(app);
    return;
  }
  startOverTCP(app);
}

main().catch((error) => {
  console.error(`Failed to start app: ${error.message}`);
  process.exit(1);
});
