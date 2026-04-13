# OpenSSH over RSP Integration

This integration runs standard OpenSSH (`sshd`/`ssh`) over the RSP socket layer,
providing a fully authenticated, routed SSH transport without any direct network
path between client and server.

## Architecture

```
SSH client
  └─ ssh -o ProxyCommand='rsp_ssh conf.json' user@host
       └─ rsp_ssh  ──TCP──►  Resource Manager (RM)  ◄──TCP──  rsp_sshd
                                                                  └─ fork sshd -i (per connection)
```

- **RM** (`resource_manager`) is the central router. Both `rsp_ssh` and `rsp_sshd` connect to it.
- **ES** (`endorsement_service`) is optional but recommended: enforces that only endorsed clients may connect.
- **`rsp_sshd`** is a `ResourceService` subclass. It registers with the RM and, for each incoming `TCP_CONNECT`, forks `sshd -i` (inetd mode) and bridges the RSP socket to sshd's stdin/stdout.
- **`rsp_ssh`** is an SSH `ProxyCommand`. It connects to the RM, acquires endorsements, then opens an RSP socket to `rsp_sshd` and bridges `stdin ↔ socket ↔ stdout`.

## Directory contents

- `rsp_sshd.cpp` — C++ ResourceService that spawns `sshd -i` per connection.
- `rsp_ssh.cpp` — C++ ProxyCommand client; bridges stdin/stdout over RSP.
- `example/rsp_sshd.conf.json` — Server config template.
- `example/rsp_ssh.conf.json` — Client config template.
- `example/rsp-sshd.service` — systemd unit file for `rsp_sshd`.

## Building

From the repository root:

```bash
make rsp-sshd   # produces build/bin/rsp_sshd
make rsp-ssh    # produces build/bin/rsp_ssh
```

The RM and ES are built as part of the normal build:

```bash
make            # builds everything including resource_manager and endorsement_service
```

## Deployment

### 1. Run the Resource Manager

On a host reachable by both client and server:

```bash
resource_manager 0.0.0.0:7000
```

### 2. Run the Endorsement Service (optional but recommended)

```bash
endorsement_service tcp:<rm-host>:7000
# Logs its Node ID — record it for client/server configs
```

### 3. Configure and run `rsp_sshd` on the SSH server host

Create `/etc/rsp-sshd/rsp_sshd.conf.json`:

```json
{
  "rsp_transport": "tcp:<rm-host>:7000",
  "sshd_path": "/usr/sbin/sshd",
  "sshd_config": "/etc/ssh/sshd_config",
  "sshd_debug": false
}
```

> **Note:** `sshd -i` requires readable host keys. If the system host keys are
> root-only, generate user-accessible keys:
> ```bash
> mkdir -p /etc/rsp-sshd/host-keys
> ssh-keygen -t ed25519 -f /etc/rsp-sshd/host-keys/ssh_host_ed25519_key -N ""
> ssh-keygen -t rsa    -f /etc/rsp-sshd/host-keys/ssh_host_rsa_key     -N ""
> ```
> Then set `sshd_config` to a file that uses those keys (`HostKey /etc/rsp-sshd/host-keys/...`).

Start `rsp_sshd` — it logs its **Node ID** on startup:

```
[rsp-sshd] Connected to RSP transport: tcp:<rm-host>:7000
[rsp-sshd] Node ID: <uuid>
[rsp-sshd] Registered as ResourceService — ready to accept SSH connections
```

Record the Node ID.

### 4. Configure `rsp_ssh` on the client

Create `~/.rsp-ssh/rsp_ssh.conf.json`:

```json
{
  "rsp_transport": "tcp:<rm-host>:7000",
  "resource_service_node_id": "<rsp_sshd-node-id>",
  "endorsement_node_id": "<es-node-id>",
  "host_port": "127.0.0.1:22",
  "connect_timeout_ms": 5000,
  "connect_retries": 3
}
```

### 5. Connect

```bash
ssh -o "ProxyCommand=rsp_ssh ~/.rsp-ssh/rsp_ssh.conf.json" user@host
```

Or add to `~/.ssh/config`:

```
Host mylab
    ProxyCommand rsp_ssh ~/.rsp-ssh/rsp_ssh.conf.json
    User alice
```

Then simply: `ssh mylab`

## Running as a systemd service

```bash
sudo cp build/bin/rsp_sshd /usr/local/bin/
sudo mkdir -p /etc/rsp-sshd
sudo cp integration/openssh/modification/example/rsp_sshd.conf.json /etc/rsp-sshd/
# Edit /etc/rsp-sshd/rsp_sshd.conf.json

sudo cp integration/openssh/modification/example/rsp-sshd.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now rsp-sshd
sudo journalctl -u rsp-sshd -f   # view logs
```

## Config reference

### `rsp_sshd.conf.json`

| Field | Required | Default | Description |
|---|---|---|---|
| `rsp_transport` | yes | — | RM address, e.g. `tcp:host:7000` |
| `sshd_path` | no | `/usr/sbin/sshd` | Path to sshd binary |
| `sshd_config` | no | system default | Path to sshd_config file |
| `sshd_debug` | no | `false` | Pass `-d` to sshd (verbose; single connection only) |

### `rsp_ssh.conf.json`

| Field | Required | Default | Description |
|---|---|---|---|
| `rsp_transport` | yes | — | RM address, e.g. `tcp:host:7000` |
| `resource_service_node_id` | yes | — | Node ID logged by `rsp_sshd` on startup |
| `endorsement_node_id` | no | — | ES Node ID (endorsement skipped if omitted) |
| `host_port` | no | `127.0.0.1:22` | Host:port passed to `rsp_sshd` for the connection |
| `connect_timeout_ms` | no | `5000` | Per-attempt connection timeout |
| `connect_retries` | no | `0` | Number of retry attempts |
