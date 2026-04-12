# OpenSSH over RSP Integration

This integration runs standard OpenSSH (`sshd`/`ssh`) over the RSP socket layer.

## What this integration does

**Server side (`rsp_sshd.cpp`):**
1. Connects a Node.js RSP client to RM (`rsp_transport`).
2. Optionally requests endorsements from ES.
3. Opens an RSP listen socket on RS (`resource_service_node_id`, `host_port`).
4. For each incoming RSP connection, spawns `sshd -i` (inetd mode) and bridges
   the RSP socket to sshd's stdin/stdout.
5. Runs as a systemd service (`rsp-sshd.service`).

**Client side (`rsp_ssh.js`, coming soon):**
- Used as an `ssh -o ProxyCommand=...` helper.
- Connects through RSP and hands off the socket to the ssh client.

## Directory contents

- `integration/openssh/rsp_sshd.cpp`
  - C++ server forwarder source: listens on RSP, spawns `sshd -i` per connection.
- `example/rsp_sshd.conf.json`
  - Example config file (copy to `/etc/rsp-sshd/rsp_sshd.conf.json`).
- `example/rsp-sshd.service`
  - systemd unit file for running `rsp_sshd` as a system service.

## Building

From repository root:

```bash
make rsp-sshd
# Binary is at build/bin/rsp_sshd
```



From repository root:

```bash
bash integration/openssh/modification/fetch_and_apply.sh
```

## Server config file fields

| Field | Required | Default | Description |
|---|---|---|---|
| `rsp_transport` | yes | — | RM address, e.g. `tcp:host:7000` |
| `resource_service_node_id` | yes | — | NodeID of the RSP resource service |
| `endorsement_node_id` | no | — | NodeID of ES (skips endorsement if omitted) |
| `host_port` | no | `127.0.0.1:22` | Virtual host:port advertised on RS |
| `sshd_path` | no | `/usr/sbin/sshd` | Path to sshd binary |
| `sshd_config` | no | system default | Path to sshd_config |
| `sshd_debug` | no | `false` | Pass `-d` to sshd (verbose, single connection) |

## Deploying the systemd service

```bash
# Install config
sudo mkdir -p /etc/rsp-sshd
sudo cp example/rsp_sshd.conf.json /etc/rsp-sshd/rsp_sshd.conf.json
# Edit /etc/rsp-sshd/rsp_sshd.conf.json with your node IDs

# Install service (adjust ExecStart paths in rsp-sshd.service first)
sudo cp example/rsp-sshd.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now rsp-sshd
```

## Validation command

```bash
make test-openssh
```
