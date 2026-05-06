use std::collections::{HashMap, VecDeque};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use base64::engine::general_purpose::STANDARD as BASE64;
use base64::Engine;
use p256::ecdsa::signature::{Signer, Verifier};
use p256::ecdsa::{Signature, SigningKey, VerifyingKey};
use p256::pkcs8::{DecodePublicKey, EncodePublicKey, LineEnding};
use rand::rngs::OsRng;
use rand::RngCore;
use serde_json::{json, Map, Value};
use sha2::{Digest, Sha256};
use thiserror::Error;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::tcp::{OwnedReadHalf, OwnedWriteHalf};
use tokio::net::TcpStream;
use tokio::sync::{oneshot, Mutex};
use tokio::task::JoinHandle;
use tokio::time::timeout;

use crate::messages::{self, enum_values};

const JSON_FRAME_MAGIC: u32 = 0x5253_504A;
const HANDSHAKE_TERMINATOR: &[u8] = b"\r\n\r\n";
const SERVICE_MESSAGE_TYPE_PREFIX: &str = "type.rsp/rsp.proto.";
const P256_ALGORITHM: i32 = 100;
const DEFAULT_TIMEOUT: Duration = Duration::from_secs(5);

const STREAM_SUCCESS: i32 = enum_values::stream_status::SUCCESS;
const STREAM_CONNECT_REFUSED: i32 = enum_values::stream_status::CONNECT_REFUSED;
const STREAM_CONNECT_TIMEOUT: i32 = enum_values::stream_status::CONNECT_TIMEOUT;
const STREAM_CLOSED: i32 = enum_values::stream_status::STREAM_CLOSED;
const STREAM_DATA: i32 = enum_values::stream_status::STREAM_DATA;
const STREAM_ERROR: i32 = enum_values::stream_status::STREAM_ERROR;
const STREAM_NEW_CONNECTION: i32 = enum_values::stream_status::NEW_CONNECTION;
const STREAM_ASYNC: i32 = enum_values::stream_status::ASYNC_STREAM;
const STREAM_INVALID_FLAGS: i32 = enum_values::stream_status::INVALID_FLAGS;
const STREAM_IN_USE: i32 = enum_values::stream_status::STREAM_IN_USE;

const ENDORSEMENT_UNKNOWN_IDENTITY: i32 = enum_values::ensdorsment_status::ENDORSEMENT_UNKNOWN_IDENTITY;

type PendingBoolTx = oneshot::Sender<Result<bool, String>>;
type PendingValueTx = oneshot::Sender<Result<Value, String>>;

#[derive(Debug, Error)]
pub enum RspClientError {
    #[error("transport parse error: {0}")]
    Transport(String),
    #[error("io error: {0}")]
    Io(#[from] std::io::Error),
    #[error("json error: {0}")]
    Json(#[from] serde_json::Error),
    #[error("message error: {0}")]
    Message(#[from] messages::MessageError),
    #[error("crypto error: {0}")]
    Crypto(String),
    #[error("protocol error: {0}")]
    Protocol(String),
    #[error("timeout waiting for {0}")]
    Timeout(&'static str),
    #[error("disconnected: {0}")]
    Disconnected(String),
    #[error("route not found for stream {0}")]
    RouteNotFound(String),
    #[error("invalid GUID/NodeID: {0}")]
    InvalidGuid(String),
}

#[derive(Debug, Clone)]
pub struct StreamReply {
    pub stream_id: Option<String>,
    pub new_stream_id: Option<String>,
    pub status_code: i32,
    pub status: Option<messages::StreamStatus>,
    pub message: Option<String>,
    pub new_stream_remote_address: Option<String>,
    pub stream_error_code: Option<i32>,
    pub data: Option<Vec<u8>>,
    pub raw: Value,
}

impl StreamReply {
    fn from_value(value: Value) -> Result<Self, RspClientError> {
        let object = value
            .as_object()
            .ok_or_else(|| RspClientError::Protocol("stream reply is not an object".to_string()))?;
        let status_code = object
            .get("error")
            .and_then(Value::as_i64)
            .unwrap_or(STREAM_SUCCESS as i64) as i32;
        let data = match object.get("data").and_then(Value::as_str) {
            Some(encoded) => Some(
                BASE64
                    .decode(encoded)
                    .map_err(|_| RspClientError::Protocol("stream reply data is not valid base64".to_string()))?,
            ),
            None => None,
        };

        Ok(Self {
            stream_id: stream_id_value(&value),
            new_stream_id: new_stream_id_value(&value),
            status_code,
            status: messages::StreamStatus::from_i32(status_code),
            message: object.get("message").and_then(Value::as_str).map(ToOwned::to_owned),
            new_stream_remote_address: object
                .get("new_stream_remote_address")
                .and_then(Value::as_str)
                .map(ToOwned::to_owned),
            stream_error_code: object.get("stream_error_code").and_then(Value::as_i64).map(|x| x as i32),
            data,
            raw: value,
        })
    }
}

#[derive(Debug, Clone)]
pub struct ConnectTcpOptions {
    pub timeout_ms: u32,
    pub retries: u32,
    pub retry_ms: u32,
    pub async_data: bool,
    pub share_socket: bool,
    pub use_socket: bool,
}

impl Default for ConnectTcpOptions {
    fn default() -> Self {
        Self {
            timeout_ms: 0,
            retries: 0,
            retry_ms: 0,
            async_data: false,
            share_socket: false,
            use_socket: false,
        }
    }
}

#[derive(Debug, Clone)]
pub struct ListenTcpOptions {
    pub timeout_ms: u32,
    pub async_accept: bool,
    pub share_listening_socket: bool,
    pub share_child_sockets: bool,
    pub children_use_socket: bool,
    pub children_async_data: bool,
}

impl Default for ListenTcpOptions {
    fn default() -> Self {
        Self {
            timeout_ms: 0,
            async_accept: false,
            share_listening_socket: false,
            share_child_sockets: false,
            children_use_socket: false,
            children_async_data: false,
        }
    }
}

#[derive(Debug, Clone)]
pub struct AcceptTcpOptions {
    pub new_stream_id: Option<String>,
    pub timeout_ms: u32,
    pub share_child_socket: bool,
    pub child_use_socket: bool,
    pub child_async_data: bool,
}

impl Default for AcceptTcpOptions {
    fn default() -> Self {
        Self {
            new_stream_id: None,
            timeout_ms: 0,
            share_child_socket: false,
            child_use_socket: false,
            child_async_data: false,
        }
    }
}

struct PendingPing {
    sequence: u32,
    tx: PendingBoolTx,
}

struct AwaitedStreamReply {
    id: u64,
    tx: PendingValueTx,
}

#[derive(Default)]
struct ClientState {
    stopping: bool,
    peer_node_id: Option<String>,
    peer_public_key_pem: Option<String>,
    ping_sequence: u32,
    pending_pings: HashMap<String, PendingPing>,
    pending_connects: HashMap<String, PendingValueTx>,
    pending_listens: HashMap<String, PendingValueTx>,
    awaited_stream_replies: HashMap<String, VecDeque<AwaitedStreamReply>>,
    next_waiter_id: u64,
    stream_routes: HashMap<String, String>,
    stream_reply_queues: HashMap<String, VecDeque<Value>>,
    pending_stream_replies: VecDeque<Value>,
    pending_endorsements: HashMap<String, PendingValueTx>,
    pending_resource_list: Option<PendingValueTx>,
    pending_resource_advertisements: VecDeque<Value>,
    pending_resource_query_replies: VecDeque<Value>,
    pending_name_reply: Option<PendingValueTx>,
}

pub struct RspClient {
    node_id: String,
    signing_key: SigningKey,
    public_key_pem: String,
    writer: std::sync::Arc<Mutex<Option<OwnedWriteHalf>>>,
    state: std::sync::Arc<Mutex<ClientState>>,
    receive_task: Option<JoinHandle<()>>,
}

impl RspClient {
    pub fn new() -> Result<Self, RspClientError> {
        let signing_key = SigningKey::random(&mut OsRng);
        let verifying_key = VerifyingKey::from(&signing_key);
        let public_key_pem = verifying_key
            .to_public_key_pem(LineEnding::LF)
            .map_err(|err| RspClientError::Crypto(format!("failed to encode public key PEM: {err}")))?;
        let node_id = node_id_from_public_key_pem(&public_key_pem)?;

        Ok(Self {
            node_id,
            signing_key,
            public_key_pem,
            writer: std::sync::Arc::new(Mutex::new(None)),
            state: std::sync::Arc::new(Mutex::new(ClientState {
                ping_sequence: 1,
                ..ClientState::default()
            })),
            receive_task: None,
        })
    }

    pub fn node_id(&self) -> &str {
        &self.node_id
    }

    pub async fn peer_node_id(&self) -> Option<String> {
        self.state.lock().await.peer_node_id.clone()
    }

    pub async fn connect(&mut self, transport_spec: &str) -> Result<(), RspClientError> {
        let (host, port) = parse_transport_spec(transport_spec)?;
        let stream = TcpStream::connect((host.as_str(), port)).await?;
        stream.set_nodelay(true)?;
        let (mut reader, mut writer) = stream.into_split();

        read_until(&mut reader, HANDSHAKE_TERMINATOR).await?;
        writer.write_all(b"encoding:json\r\n\r\n").await?;
        writer.flush().await?;
        let result = read_until(&mut reader, HANDSHAKE_TERMINATOR).await?;
        if !result.starts_with(b"1success: encoding:json") {
            return Err(RspClientError::Protocol(format!(
                "ASCII handshake failed: {}",
                String::from_utf8_lossy(&result).trim()
            )));
        }

        self.perform_initial_identity_exchange(&mut reader, &mut writer).await?;

        {
            let mut state = self.state.lock().await;
            state.stopping = false;
        }
        {
            let mut writer_lock = self.writer.lock().await;
            *writer_lock = Some(writer);
        }

        let state = std::sync::Arc::clone(&self.state);
        self.receive_task = Some(tokio::spawn(async move {
            if let Err(err) = receive_loop(reader, state.clone()).await {
                let stopping = state.lock().await.stopping;
                if !stopping {
                    reject_all_pending(&state, &format!("receive loop terminated: {err}")).await;
                }
            }
        }));

        Ok(())
    }

    pub async fn close(&mut self) -> Result<(), RspClientError> {
        {
            let mut state = self.state.lock().await;
            state.stopping = true;
            state.peer_node_id = None;
            state.peer_public_key_pem = None;
        }

        {
            let mut writer = self.writer.lock().await;
            if let Some(mut active_writer) = writer.take() {
                let _ = active_writer.shutdown().await;
            }
        }

        reject_all_pending(&self.state, "client closed").await;

        if let Some(task) = self.receive_task.take() {
            task.abort();
            let _ = task.await;
        }

        {
            let mut state = self.state.lock().await;
            state.stopping = false;
        }

        Ok(())
    }

    pub async fn ping(&self, node_id: &str, timeout_duration: Duration) -> Result<bool, RspClientError> {
        let ping_nonce = random_b64_16();
        let sequence = {
            let mut state = self.state.lock().await;
            let sequence = state.ping_sequence;
            state.ping_sequence = state.ping_sequence.wrapping_add(1);
            sequence
        };

        let request = json!({
            "destination": {"value": encode_node_id_for_field(node_id)?},
            "nonce": {"value": random_b64_16()},
            "ping_request": {
                "nonce": {"value": ping_nonce.clone()},
                "sequence": sequence,
                "time_sent": {"milliseconds_since_epoch": now_ms() as u64}
            }
        });

        let (tx, rx) = oneshot::channel();
        {
            let mut state = self.state.lock().await;
            state.pending_pings.insert(ping_nonce.clone(), PendingPing { sequence, tx });
        }

        if let Err(err) = self.send_signed_message(request).await {
            let mut state = self.state.lock().await;
            state.pending_pings.remove(&ping_nonce);
            return Err(err);
        }

        match timeout(timeout_duration, rx).await {
            Ok(Ok(Ok(value))) => Ok(value),
            Ok(Ok(Err(msg))) => Err(RspClientError::Disconnected(msg)),
            Ok(Err(_)) => Err(RspClientError::Disconnected("ping waiter dropped".to_string())),
            Err(_) => {
                let mut state = self.state.lock().await;
                state.pending_pings.remove(&ping_nonce);
                Ok(false)
            }
        }
    }

    pub async fn connect_tcp_ex(
        &self,
        node_id: &str,
        host_port: &str,
        options: ConnectTcpOptions,
    ) -> Result<StreamReply, RspClientError> {
        let stream_id = random_b64_16();
        let mut tcp = Map::new();
        tcp.insert("host_port".to_string(), Value::String(host_port.to_string()));
        tcp.insert("stream_id".to_string(), json!({"value": stream_id.clone()}));
        if options.use_socket {
            tcp.insert("use_socket".to_string(), Value::Bool(true));
        }
        if options.timeout_ms > 0 {
            tcp.insert("timeout_ms".to_string(), Value::Number(options.timeout_ms.into()));
        }
        if options.retries > 0 {
            tcp.insert("retries".to_string(), Value::Number(options.retries.into()));
        }
        if options.retry_ms > 0 {
            tcp.insert("retry_ms".to_string(), Value::Number(options.retry_ms.into()));
        }
        if options.async_data {
            tcp.insert("async_data".to_string(), Value::Bool(true));
        }
        if options.share_socket {
            tcp.insert("share_socket".to_string(), Value::Bool(true));
        }

        let request = json!({
            "destination": {"value": encode_node_id_for_field(node_id)?},
            "service_message": pack_service_message("ConnectTCPRequest", tcp),
        });

        let (tx, rx) = oneshot::channel();
        {
            let mut state = self.state.lock().await;
            state.pending_connects.insert(stream_id.clone(), tx);
        }

        if let Err(err) = self.send_signed_message(request).await {
            let mut state = self.state.lock().await;
            state.pending_connects.remove(&stream_id);
            return Err(err);
        }

        let wait_duration = if options.timeout_ms > 0 {
            Duration::from_millis(options.timeout_ms as u64 + 1000)
        } else {
            DEFAULT_TIMEOUT
        };

        let reply_value = match timeout(wait_duration, rx).await {
            Ok(Ok(Ok(value))) => value,
            Ok(Ok(Err(msg))) => return Err(RspClientError::Disconnected(msg)),
            Ok(Err(_)) => return Err(RspClientError::Disconnected("connect waiter dropped".to_string())),
            Err(_) => {
                let mut state = self.state.lock().await;
                state.pending_connects.remove(&stream_id);
                return Err(RspClientError::Timeout("connect_tcp_ex"));
            }
        };

        let confirmed_id = stream_id_value(&reply_value).unwrap_or(stream_id);
        let mut reply_with_aliases = reply_value.clone();
        set_id_field(&mut reply_with_aliases, "stream_id", &confirmed_id);
        set_id_field(&mut reply_with_aliases, "socket_id", &confirmed_id);

        if stream_status_code(&reply_with_aliases) == STREAM_SUCCESS {
            let mut state = self.state.lock().await;
            state.stream_routes.insert(confirmed_id, node_id.to_string());
        }

        StreamReply::from_value(reply_with_aliases)
    }

    pub async fn connect_tcp(
        &self,
        node_id: &str,
        host_port: &str,
        options: ConnectTcpOptions,
    ) -> Result<Option<String>, RspClientError> {
        let reply = self.connect_tcp_ex(node_id, host_port, options).await?;
        if reply.status_code != STREAM_SUCCESS {
            return Ok(None);
        }
        Ok(reply.stream_id)
    }

    pub async fn listen_tcp_ex(
        &self,
        node_id: &str,
        host_port: &str,
        options: ListenTcpOptions,
    ) -> Result<StreamReply, RspClientError> {
        let stream_id = random_b64_16();
        let mut tcp = Map::new();
        tcp.insert("host_port".to_string(), Value::String(host_port.to_string()));
        tcp.insert("stream_id".to_string(), json!({"value": stream_id.clone()}));
        if options.timeout_ms > 0 {
            tcp.insert("timeout_ms".to_string(), Value::Number(options.timeout_ms.into()));
        }
        if options.async_accept {
            tcp.insert("async_accept".to_string(), Value::Bool(true));
        }
        if options.share_listening_socket {
            tcp.insert("share_listening_socket".to_string(), Value::Bool(true));
        }
        if options.share_child_sockets {
            tcp.insert("share_child_sockets".to_string(), Value::Bool(true));
        }
        if options.children_use_socket {
            tcp.insert("children_use_socket".to_string(), Value::Bool(true));
        }
        if options.children_async_data {
            tcp.insert("children_async_data".to_string(), Value::Bool(true));
        }

        let request = json!({
            "destination": {"value": encode_node_id_for_field(node_id)?},
            "service_message": pack_service_message("ListenTCPRequest", tcp),
        });

        let (tx, rx) = oneshot::channel();
        {
            let mut state = self.state.lock().await;
            state.pending_listens.insert(stream_id.clone(), tx);
        }

        if let Err(err) = self.send_signed_message(request).await {
            let mut state = self.state.lock().await;
            state.pending_listens.remove(&stream_id);
            return Err(err);
        }

        let wait_duration = if options.timeout_ms > 0 {
            Duration::from_millis(options.timeout_ms as u64 + 1000)
        } else {
            DEFAULT_TIMEOUT
        };

        let reply_value = match timeout(wait_duration, rx).await {
            Ok(Ok(Ok(value))) => value,
            Ok(Ok(Err(msg))) => return Err(RspClientError::Disconnected(msg)),
            Ok(Err(_)) => return Err(RspClientError::Disconnected("listen waiter dropped".to_string())),
            Err(_) => {
                let mut state = self.state.lock().await;
                state.pending_listens.remove(&stream_id);
                return Err(RspClientError::Timeout("listen_tcp_ex"));
            }
        };

        let confirmed_id = stream_id_value(&reply_value).unwrap_or(stream_id);
        let mut reply_with_aliases = reply_value.clone();
        set_id_field(&mut reply_with_aliases, "stream_id", &confirmed_id);
        set_id_field(&mut reply_with_aliases, "socket_id", &confirmed_id);

        if stream_status_code(&reply_with_aliases) == STREAM_SUCCESS {
            let mut state = self.state.lock().await;
            state.stream_routes.insert(confirmed_id, node_id.to_string());
        }

        StreamReply::from_value(reply_with_aliases)
    }

    pub async fn listen_tcp(
        &self,
        node_id: &str,
        host_port: &str,
        options: ListenTcpOptions,
    ) -> Result<Option<String>, RspClientError> {
        let reply = self.listen_tcp_ex(node_id, host_port, options).await?;
        if reply.status_code != STREAM_SUCCESS {
            return Ok(None);
        }
        Ok(reply.stream_id)
    }

    pub async fn accept_tcp_ex(
        &self,
        listen_stream_id: &str,
        options: AcceptTcpOptions,
    ) -> Result<StreamReply, RspClientError> {
        let node_id = {
            let state = self.state.lock().await;
            state
                .stream_routes
                .get(listen_stream_id)
                .cloned()
                .ok_or_else(|| RspClientError::RouteNotFound(listen_stream_id.to_string()))?
        };

        let mut fields = Map::new();
        fields.insert(
            "listen_stream_id".to_string(),
            json!({"value": listen_stream_id}),
        );
        if let Some(new_stream_id) = options.new_stream_id {
            fields.insert(
                "new_stream_id".to_string(),
                json!({"value": normalize_guid(&new_stream_id)?}),
            );
        }
        if options.timeout_ms > 0 {
            fields.insert("timeout_ms".to_string(), Value::Number(options.timeout_ms.into()));
        }
        if options.share_child_socket {
            fields.insert("share_child_socket".to_string(), Value::Bool(true));
        }
        if options.child_use_socket {
            fields.insert("child_use_socket".to_string(), Value::Bool(true));
        }
        if options.child_async_data {
            fields.insert("child_async_data".to_string(), Value::Bool(true));
        }

        let request = json!({
            "destination": {"value": encode_node_id_for_field(&node_id)?},
            "service_message": pack_service_message("AcceptTCP", fields),
        });

        self.send_signed_message(request).await?;

        let wait_duration = if options.timeout_ms > 0 {
            Duration::from_millis(options.timeout_ms as u64 + 1000)
        } else {
            DEFAULT_TIMEOUT
        };
        let mut reply = self.wait_for_stream_reply(listen_stream_id, wait_duration).await?;

        if matches!(reply.status_code, STREAM_SUCCESS | STREAM_NEW_CONNECTION) {
            if let Some(new_stream_id) = reply.new_stream_id.clone() {
                let mut state = self.state.lock().await;
                state.stream_routes.insert(new_stream_id.clone(), node_id);
                set_id_field(&mut reply.raw, "new_stream_id", &new_stream_id);
                set_id_field(&mut reply.raw, "new_socket_id", &new_stream_id);
            }
        }

        Ok(reply)
    }

    pub async fn accept_tcp(
        &self,
        listen_stream_id: &str,
        options: AcceptTcpOptions,
    ) -> Result<Option<String>, RspClientError> {
        let reply = self.accept_tcp_ex(listen_stream_id, options).await?;
        if !matches!(reply.status_code, STREAM_SUCCESS | STREAM_NEW_CONNECTION) {
            return Ok(None);
        }
        Ok(reply.new_stream_id)
    }

    pub async fn stream_send(&self, stream_id: &str, data: &[u8]) -> Result<bool, RspClientError> {
        let node_id = {
            let state = self.state.lock().await;
            state
                .stream_routes
                .get(stream_id)
                .cloned()
                .ok_or_else(|| RspClientError::RouteNotFound(stream_id.to_string()))?
        };

        let request = json!({
            "destination": {"value": encode_node_id_for_field(&node_id)?},
            "service_message": pack_service_message("StreamSend", map_from_pairs([
                ("stream_id", json!({"value": stream_id})),
                ("data", Value::String(BASE64.encode(data))),
            ])),
        });
        self.send_signed_message(request).await?;

        let deadline = tokio::time::Instant::now() + DEFAULT_TIMEOUT;
        while tokio::time::Instant::now() < deadline {
            let remaining = deadline.saturating_duration_since(tokio::time::Instant::now());
            let reply = self.wait_for_stream_reply(stream_id, remaining).await?;
            match reply.status_code {
                STREAM_SUCCESS => return Ok(true),
                STREAM_DATA | STREAM_NEW_CONNECTION | STREAM_ASYNC => continue,
                STREAM_CLOSED | STREAM_ERROR | STREAM_INVALID_FLAGS => return Ok(false),
                _ => return Ok(false),
            }
        }
        Ok(false)
    }

    pub async fn stream_recv_ex(
        &self,
        stream_id: &str,
        max_bytes: u32,
        wait_ms: u32,
    ) -> Result<StreamReply, RspClientError> {
        let node_id = {
            let state = self.state.lock().await;
            state
                .stream_routes
                .get(stream_id)
                .cloned()
                .ok_or_else(|| RspClientError::RouteNotFound(stream_id.to_string()))?
        };

        let mut fields = Map::new();
        fields.insert("stream_id".to_string(), json!({"value": stream_id}));
        fields.insert("max_bytes".to_string(), Value::Number(max_bytes.into()));
        if wait_ms > 0 {
            fields.insert("wait_ms".to_string(), Value::Number(wait_ms.into()));
        }

        let request = json!({
            "destination": {"value": encode_node_id_for_field(&node_id)?},
            "service_message": pack_service_message("StreamRecv", fields),
        });
        self.send_signed_message(request).await?;

        let wait_duration = if wait_ms > 0 {
            Duration::from_millis(wait_ms as u64 + 1000)
        } else {
            DEFAULT_TIMEOUT
        };
        self.wait_for_stream_reply(stream_id, wait_duration).await
    }

    pub async fn stream_recv(
        &self,
        stream_id: &str,
        max_bytes: u32,
        wait_ms: u32,
    ) -> Result<Option<Vec<u8>>, RspClientError> {
        let reply = self.stream_recv_ex(stream_id, max_bytes, wait_ms).await?;
        if !matches!(reply.status_code, STREAM_DATA | STREAM_SUCCESS) {
            return Ok(None);
        }
        Ok(Some(reply.data.unwrap_or_default()))
    }

    pub async fn stream_close(&self, stream_id: &str) -> Result<bool, RspClientError> {
        let node_id = {
            let state = self.state.lock().await;
            match state.stream_routes.get(stream_id).cloned() {
                Some(node_id) => node_id,
                None => return Ok(true),
            }
        };

        let request = json!({
            "destination": {"value": encode_node_id_for_field(&node_id)?},
            "service_message": pack_service_message("StreamClose", map_from_pairs([
                ("stream_id", json!({"value": stream_id}))
            ])),
        });

        if self.send_signed_message(request).await.is_err() {
            let mut state = self.state.lock().await;
            state.stream_routes.remove(stream_id);
            return Ok(true);
        }

        let deadline = tokio::time::Instant::now() + DEFAULT_TIMEOUT;
        while tokio::time::Instant::now() < deadline {
            let remaining = deadline.saturating_duration_since(tokio::time::Instant::now());
            let reply = match self.wait_for_stream_reply(stream_id, remaining).await {
                Ok(reply) => reply,
                Err(_) => break,
            };
            match reply.status_code {
                STREAM_SUCCESS | STREAM_CLOSED => {
                    let mut state = self.state.lock().await;
                    state.stream_routes.remove(stream_id);
                    return Ok(true);
                }
                STREAM_DATA | STREAM_NEW_CONNECTION | STREAM_ASYNC => continue,
                _ => return Ok(false),
            }
        }

        let mut state = self.state.lock().await;
        state.stream_routes.remove(stream_id);
        Ok(false)
    }

    pub async fn try_dequeue_stream_reply(&self) -> Result<Option<StreamReply>, RspClientError> {
        let value = {
            let mut state = self.state.lock().await;
            state.pending_stream_replies.pop_front()
        };
        match value {
            Some(value) => Ok(Some(StreamReply::from_value(value)?)),
            None => Ok(None),
        }
    }

    pub async fn pending_stream_reply_count(&self) -> usize {
        self.state.lock().await.pending_stream_replies.len()
    }

    pub async fn register_stream_route(&self, stream_id: &str, node_id: &str) {
        let mut state = self.state.lock().await;
        state
            .stream_routes
            .insert(stream_id.to_string(), node_id.to_string());
    }

    pub async fn query_resources(
        &self,
        node_id: &str,
        query: &str,
        max_records: u32,
    ) -> Result<(), RspClientError> {
        let mut fields = Map::new();
        if !query.is_empty() {
            fields.insert("query".to_string(), Value::String(query.to_string()));
        }
        if max_records > 0 {
            fields.insert("max_records".to_string(), Value::Number(max_records.into()));
        }
        let request = json!({
            "destination": {"value": encode_node_id_for_field(node_id)?},
            "resource_query": Value::Object(fields),
        });
        self.send_signed_message(request).await
    }

    pub async fn resource_list(
        &self,
        node_id: &str,
        query: &str,
        max_records: u32,
        timeout_duration: Duration,
    ) -> Result<Option<Value>, RspClientError> {
        let (tx, rx) = oneshot::channel();
        {
            let mut state = self.state.lock().await;
            state.pending_resource_list = Some(tx);
        }

        if let Err(err) = self.query_resources(node_id, query, max_records).await {
            let mut state = self.state.lock().await;
            state.pending_resource_list = None;
            return Err(err);
        }

        match timeout(timeout_duration, rx).await {
            Ok(Ok(Ok(value))) => Ok(Some(value)),
            Ok(Ok(Err(msg))) => Err(RspClientError::Disconnected(msg)),
            Ok(Err(_)) => Err(RspClientError::Disconnected("resource list waiter dropped".to_string())),
            Err(_) => {
                let mut state = self.state.lock().await;
                state.pending_resource_list = None;
                Ok(None)
            }
        }
    }

    pub async fn try_dequeue_resource_advertisement(&self) -> Option<Value> {
        self.state.lock().await.pending_resource_advertisements.pop_front()
    }

    pub async fn try_dequeue_resource_query_reply(&self) -> Option<Value> {
        self.state.lock().await.pending_resource_query_replies.pop_front()
    }

    pub async fn begin_endorsement_request(
        &self,
        node_id: &str,
        endorsement_type: &str,
        endorsement_value: &[u8],
    ) -> Result<Option<Value>, RspClientError> {
        let pending_key = encode_node_id_for_field(node_id)?;
        {
            let state = self.state.lock().await;
            if state.pending_endorsements.contains_key(&pending_key) {
                return Ok(None);
            }
        }

        let mut requested = json!({
            "subject": {"value": encode_node_id_for_signer(&self.node_id)?},
            "endorsement_service": {"value": encode_node_id_for_signer(node_id)?},
            "endorsement_type": {"value": normalize_guid(endorsement_type)?},
            "endorsement_value": BASE64.encode(endorsement_value),
            "valid_until": {"milliseconds_since_epoch": now_ms() + 86_400_000}
        });
        let signature = self.sign_endorsement(&requested)?;
        if let Some(obj) = requested.as_object_mut() {
            obj.insert("signature".to_string(), Value::String(signature));
        }

        let mut repaired_unknown_identity = false;
        loop {
            let done = self
                .send_endorsement_request(node_id, &pending_key, requested.clone())
                .await?;
            let Some(done_value) = done else {
                return Ok(None);
            };

            let status = done_value.get("status").and_then(Value::as_i64).unwrap_or(0) as i32;
            if !repaired_unknown_identity && status == ENDORSEMENT_UNKNOWN_IDENTITY {
                repaired_unknown_identity = true;
                let _ = self.send_identity_to(node_id).await;
                continue;
            }

            return Ok(Some(done_value));
        }
    }

    pub async fn name_create(
        &self,
        node_id: &str,
        name: &str,
        owner: &str,
        type_uuid: &str,
        value_uuid: &str,
        timeout_duration: Duration,
    ) -> Result<Option<Value>, RspClientError> {
        self.send_name_request(
            node_id,
            "NameCreateRequest",
            json!({
                "record": {
                    "name": name,
                    "owner": {"value": encode_node_id_for_field(owner)?},
                    "type": {"value": encode_node_id_for_field(type_uuid)?},
                    "value": {"value": encode_node_id_for_field(value_uuid)?},
                }
            }),
            timeout_duration,
        )
        .await
    }

    pub async fn name_read(
        &self,
        node_id: &str,
        name: &str,
        owner: Option<&str>,
        type_uuid: Option<&str>,
        timeout_duration: Duration,
    ) -> Result<Option<Value>, RspClientError> {
        let mut fields = Map::new();
        fields.insert("name".to_string(), Value::String(name.to_string()));
        if let Some(owner) = owner {
            fields.insert("owner".to_string(), json!({"value": encode_node_id_for_field(owner)?}));
        }
        if let Some(type_uuid) = type_uuid {
            fields.insert("type".to_string(), json!({"value": encode_node_id_for_field(type_uuid)?}));
        }
        self.send_name_request(
            node_id,
            "NameReadRequest",
            Value::Object(fields),
            timeout_duration,
        )
        .await
    }

    pub async fn name_update(
        &self,
        node_id: &str,
        name: &str,
        owner: &str,
        type_uuid: &str,
        new_value: &str,
        timeout_duration: Duration,
    ) -> Result<Option<Value>, RspClientError> {
        self.send_name_request(
            node_id,
            "NameUpdateRequest",
            json!({
                "name": name,
                "owner": {"value": encode_node_id_for_field(owner)?},
                "type": {"value": encode_node_id_for_field(type_uuid)?},
                "new_value": {"value": encode_node_id_for_field(new_value)?},
            }),
            timeout_duration,
        )
        .await
    }

    pub async fn name_delete(
        &self,
        node_id: &str,
        name: &str,
        owner: &str,
        type_uuid: &str,
        timeout_duration: Duration,
    ) -> Result<Option<Value>, RspClientError> {
        self.send_name_request(
            node_id,
            "NameDeleteRequest",
            json!({
                "name": name,
                "owner": {"value": encode_node_id_for_field(owner)?},
                "type": {"value": encode_node_id_for_field(type_uuid)?},
            }),
            timeout_duration,
        )
        .await
    }

    pub async fn name_query(
        &self,
        node_id: &str,
        name_prefix: Option<&str>,
        owner: Option<&str>,
        type_uuid: Option<&str>,
        max_records: u32,
        timeout_duration: Duration,
    ) -> Result<Option<Value>, RspClientError> {
        let mut fields = Map::new();
        if let Some(name_prefix) = name_prefix {
            fields.insert("name_prefix".to_string(), Value::String(name_prefix.to_string()));
        }
        if let Some(owner) = owner {
            fields.insert("owner".to_string(), json!({"value": encode_node_id_for_field(owner)?}));
        }
        if let Some(type_uuid) = type_uuid {
            fields.insert("type".to_string(), json!({"value": encode_node_id_for_field(type_uuid)?}));
        }
        if max_records > 0 {
            fields.insert("max_records".to_string(), Value::Number(max_records.into()));
        }
        self.send_name_request(
            node_id,
            "NameQueryRequest",
            Value::Object(fields),
            timeout_duration,
        )
        .await
    }

    async fn send_name_request(
        &self,
        node_id: &str,
        type_name: &str,
        fields: Value,
        timeout_duration: Duration,
    ) -> Result<Option<Value>, RspClientError> {
        let fields = fields
            .as_object()
            .cloned()
            .ok_or_else(|| RspClientError::Protocol("name request fields must be object".to_string()))?;
        let request = json!({
            "destination": {"value": encode_node_id_for_field(node_id)?},
            "service_message": pack_service_message(type_name, fields),
        });

        let (tx, rx) = oneshot::channel();
        {
            let mut state = self.state.lock().await;
            state.pending_name_reply = Some(tx);
        }

        if let Err(err) = self.send_signed_message(request).await {
            let mut state = self.state.lock().await;
            state.pending_name_reply = None;
            return Err(err);
        }

        match timeout(timeout_duration, rx).await {
            Ok(Ok(Ok(value))) => Ok(Some(value)),
            Ok(Ok(Err(msg))) => Err(RspClientError::Disconnected(msg)),
            Ok(Err(_)) => Err(RspClientError::Disconnected("name waiter dropped".to_string())),
            Err(_) => {
                let mut state = self.state.lock().await;
                state.pending_name_reply = None;
                Ok(None)
            }
        }
    }

    async fn send_endorsement_request(
        &self,
        node_id: &str,
        pending_key: &str,
        requested: Value,
    ) -> Result<Option<Value>, RspClientError> {
        let request = json!({
            "destination": {"value": encode_node_id_for_field(node_id)?},
            "service_message": pack_service_message("BeginEndorsementRequest", map_from_pairs([
                ("requested_values", requested)
            ])),
        });

        let (tx, rx) = oneshot::channel();
        {
            let mut state = self.state.lock().await;
            state.pending_endorsements.insert(pending_key.to_string(), tx);
        }

        if let Err(err) = self.send_signed_message(request).await {
            let mut state = self.state.lock().await;
            state.pending_endorsements.remove(pending_key);
            return Err(err);
        }

        match timeout(DEFAULT_TIMEOUT, rx).await {
            Ok(Ok(Ok(value))) => Ok(Some(value)),
            Ok(Ok(Err(msg))) => Err(RspClientError::Disconnected(msg)),
            Ok(Err(_)) => Err(RspClientError::Disconnected("endorsement waiter dropped".to_string())),
            Err(_) => {
                let mut state = self.state.lock().await;
                state.pending_endorsements.remove(pending_key);
                Ok(None)
            }
        }
    }

    async fn send_identity_to(&self, node_id: &str) -> Result<(), RspClientError> {
        let identity_message = json!({
            "destination": {"value": encode_node_id_for_field(node_id)?},
            "identities": [{
                "public_key": {
                    "algorithm": P256_ALGORITHM,
                    "public_key": BASE64.encode(self.public_key_pem.as_bytes()),
                }
            }]
        });
        self.send_signed_message(identity_message).await
    }

    async fn wait_for_stream_reply(
        &self,
        stream_id: &str,
        timeout_duration: Duration,
    ) -> Result<StreamReply, RspClientError> {
        if let Some(reply) = {
            let mut state = self.state.lock().await;
            let queue = state.stream_reply_queues.get_mut(stream_id);
            queue.and_then(|queue| queue.pop_front())
        } {
            return StreamReply::from_value(reply);
        }

        let (waiter_id, rx) = {
            let (tx, rx) = oneshot::channel();
            let mut state = self.state.lock().await;
            let waiter_id = state.next_waiter_id;
            state.next_waiter_id = state.next_waiter_id.wrapping_add(1);
            state
                .awaited_stream_replies
                .entry(stream_id.to_string())
                .or_default()
                .push_back(AwaitedStreamReply { id: waiter_id, tx });
            (waiter_id, rx)
        };

        let value = match timeout(timeout_duration, rx).await {
            Ok(Ok(Ok(value))) => value,
            Ok(Ok(Err(msg))) => return Err(RspClientError::Disconnected(msg)),
            Ok(Err(_)) => return Err(RspClientError::Disconnected("stream waiter dropped".to_string())),
            Err(_) => {
                remove_stream_waiter(&self.state, stream_id, waiter_id).await;
                return Err(RspClientError::Timeout("stream reply"));
            }
        };
        StreamReply::from_value(value)
    }

    async fn send_signed_message(&self, message: Value) -> Result<(), RspClientError> {
        let mut signed = message;
        let signature = self.sign_rsp_message(&signed)?;
        let object = signed
            .as_object_mut()
            .ok_or_else(|| RspClientError::Protocol("signed message must be an object".to_string()))?;
        object.insert("signature".to_string(), signature);
        self.send_raw_message(&signed).await
    }

    async fn send_raw_message(&self, message: &Value) -> Result<(), RspClientError> {
        let mut writer_guard = self.writer.lock().await;
        let writer = writer_guard
            .as_mut()
            .ok_or_else(|| RspClientError::Disconnected("client is not connected".to_string()))?;
        send_raw_message_inner(writer, message).await
    }

    async fn perform_initial_identity_exchange(
        &self,
        reader: &mut OwnedReadHalf,
        writer: &mut OwnedWriteHalf,
    ) -> Result<(), RspClientError> {
        let local_challenge_nonce = random_b64_16();
        send_raw_message_inner(
            writer,
            &json!({"challenge_request": {"nonce": {"value": local_challenge_nonce.clone()}}}),
        )
        .await?;

        let mut peer_challenge_received = false;
        let mut peer_identity_received = false;

        while !peer_challenge_received || !peer_identity_received {
            let message = receive_raw_message(reader).await?;

            if has_field(&message, "challenge_request") {
                let nonce = message
                    .get("challenge_request")
                    .and_then(Value::as_object)
                    .and_then(|obj| obj.get("nonce"))
                    .and_then(Value::as_object)
                    .and_then(|obj| obj.get("value"))
                    .and_then(Value::as_str)
                    .ok_or_else(|| RspClientError::Protocol("invalid challenge request during auth".to_string()))?;

                let mut identity_message = json!({
                    "identity": {
                        "nonce": {"value": nonce},
                        "public_key": {
                            "algorithm": P256_ALGORITHM,
                            "public_key": BASE64.encode(self.public_key_pem.as_bytes()),
                        }
                    }
                });
                let signature = self.sign_rsp_message(&identity_message)?;
                identity_message
                    .as_object_mut()
                    .expect("identity message object")
                    .insert("signature".to_string(), signature);
                send_raw_message_inner(writer, &identity_message).await?;
                peer_challenge_received = true;
                continue;
            }

            if has_field(&message, "identity") {
                let peer_public_key_b64 = message
                    .get("identity")
                    .and_then(Value::as_object)
                    .and_then(|obj| obj.get("public_key"))
                    .and_then(Value::as_object)
                    .and_then(|obj| obj.get("public_key"))
                    .and_then(Value::as_str)
                    .ok_or_else(|| RspClientError::Protocol("identity missing public key".to_string()))?;
                let peer_public_key_pem_bytes = BASE64
                    .decode(peer_public_key_b64)
                    .map_err(|_| RspClientError::Protocol("identity public key is not base64".to_string()))?;
                let peer_public_key_pem = String::from_utf8(peer_public_key_pem_bytes)
                    .map_err(|_| RspClientError::Protocol("identity public key is not UTF-8 PEM".to_string()))?;
                let peer_nonce = message
                    .get("identity")
                    .and_then(Value::as_object)
                    .and_then(|obj| obj.get("nonce"))
                    .and_then(Value::as_object)
                    .and_then(|obj| obj.get("value"))
                    .and_then(Value::as_str)
                    .unwrap_or_default();

                if peer_nonce != local_challenge_nonce {
                    return Err(RspClientError::Protocol(
                        "peer identity nonce did not match challenge nonce".to_string(),
                    ));
                }
                if !verify_rsp_message_signature(&peer_public_key_pem, &message)? {
                    return Err(RspClientError::Protocol(
                        "peer identity signature verification failed".to_string(),
                    ));
                }

                let peer_node_id = node_id_from_public_key_pem(&peer_public_key_pem)?;
                let mut state = self.state.lock().await;
                state.peer_node_id = Some(peer_node_id);
                state.peer_public_key_pem = Some(peer_public_key_pem);
                peer_identity_received = true;
                continue;
            }

            return Err(RspClientError::Protocol(
                "unexpected message during initial identity exchange".to_string(),
            ));
        }

        Ok(())
    }

    fn sign_rsp_message(&self, message: &Value) -> Result<Value, RspClientError> {
        let digest = messages::hash_rsp_message(message)?;
        let signature: Signature = self.signing_key.sign(&digest);
        let signature_der = signature.to_der();
        Ok(json!({
            "signer": {"value": encode_node_id_for_signer(&self.node_id)?},
            "algorithm": P256_ALGORITHM,
            "signature": BASE64.encode(signature_der.as_bytes()),
        }))
    }

    fn sign_endorsement(&self, endorsement: &Value) -> Result<String, RspClientError> {
        let digest = messages::hash_endorsement(endorsement)?;
        let signature: Signature = self.signing_key.sign(&digest);
        let signature_der = signature.to_der();
        Ok(BASE64.encode(signature_der.as_bytes()))
    }
}

async fn send_raw_message_inner(writer: &mut OwnedWriteHalf, message: &Value) -> Result<(), RspClientError> {
    let payload = serde_json::to_vec(message)?;
    let mut frame = Vec::with_capacity(8 + payload.len());
    frame.extend_from_slice(&JSON_FRAME_MAGIC.to_be_bytes());
    frame.extend_from_slice(&(payload.len() as u32).to_be_bytes());
    frame.extend_from_slice(&payload);
    writer.write_all(&frame).await?;
    writer.flush().await?;
    Ok(())
}

async fn receive_raw_message(reader: &mut OwnedReadHalf) -> Result<Value, RspClientError> {
    let mut header = [0u8; 8];
    reader.read_exact(&mut header).await?;
    let magic = u32::from_be_bytes([header[0], header[1], header[2], header[3]]);
    if magic != JSON_FRAME_MAGIC {
        return Err(RspClientError::Protocol(format!(
            "unexpected JSON frame magic: 0x{magic:08x}"
        )));
    }
    let payload_len = u32::from_be_bytes([header[4], header[5], header[6], header[7]]) as usize;
    let mut payload = vec![0u8; payload_len];
    reader.read_exact(&mut payload).await?;
    Ok(serde_json::from_slice(&payload)?)
}

async fn read_until(reader: &mut OwnedReadHalf, marker: &[u8]) -> Result<Vec<u8>, RspClientError> {
    let mut buffer = Vec::new();
    let mut byte = [0u8; 1];
    loop {
        reader.read_exact(&mut byte).await?;
        buffer.push(byte[0]);
        if buffer.ends_with(marker) {
            return Ok(buffer);
        }
    }
}

async fn receive_loop(
    mut reader: OwnedReadHalf,
    state: std::sync::Arc<Mutex<ClientState>>,
) -> Result<(), RspClientError> {
    loop {
        let message = receive_raw_message(&mut reader).await?;
        dispatch_message(&state, message).await?;
    }
}

async fn dispatch_message(state: &std::sync::Arc<Mutex<ClientState>>, msg: Value) -> Result<(), RspClientError> {
    if has_field(&msg, "ping_reply") {
        handle_ping_reply(state, &msg).await;
        return Ok(());
    }

    if has_field(&msg, "service_message") {
        let type_name = service_message_type_name(&msg);
        if matches!(
            type_name.as_deref(),
            Some("rsp.proto.StreamReply") | Some("rsp.proto.SocketReply")
        ) {
            let stream_reply = unpack_service_message(&msg);
            handle_stream_reply(state, &msg, stream_reply).await?;
            return Ok(());
        }
        if matches!(type_name.as_deref(), Some("rsp.proto.EndorsementDone")) {
            let endorsement_done = unpack_service_message(&msg);
            handle_endorsement_done(state, &msg, endorsement_done).await?;
            return Ok(());
        }
        if let Some(type_name) = type_name {
            if type_name.starts_with("rsp.proto.Name") && type_name.ends_with("Reply") {
                handle_name_reply(state, unpack_service_message(&msg)).await;
                return Ok(());
            }
        }
        return Ok(());
    }

    if has_field(&msg, "resource_advertisement") {
        let mut st = state.lock().await;
        if let Some(advertisement) = msg.get("resource_advertisement") {
            st.pending_resource_advertisements
                .push_back(advertisement.clone());
        }
        return Ok(());
    }

    if has_field(&msg, "resource_query_reply") {
        let mut st = state.lock().await;
        if let Some(reply) = msg.get("resource_query_reply").cloned() {
            if let Some(tx) = st.pending_resource_list.take() {
                let _ = tx.send(Ok(reply));
            } else {
                st.pending_resource_query_replies.push_back(reply);
            }
        }
        return Ok(());
    }

    Ok(())
}

async fn handle_ping_reply(state: &std::sync::Arc<Mutex<ClientState>>, msg: &Value) {
    let nonce = msg
        .get("ping_reply")
        .and_then(Value::as_object)
        .and_then(|reply| reply.get("nonce"))
        .and_then(Value::as_object)
        .and_then(|nonce| nonce.get("value"))
        .and_then(Value::as_str);
    let sequence = msg
        .get("ping_reply")
        .and_then(Value::as_object)
        .and_then(|reply| reply.get("sequence"))
        .and_then(Value::as_u64)
        .map(|value| value as u32);

    let Some(nonce) = nonce else { return };
    let Some(sequence) = sequence else { return };

    let mut st = state.lock().await;
    if let Some(pending) = st.pending_pings.remove(nonce) {
        if pending.sequence == sequence {
            let _ = pending.tx.send(Ok(true));
        }
    }
}

async fn handle_stream_reply(
    state: &std::sync::Arc<Mutex<ClientState>>,
    msg: &Value,
    stream_reply: Value,
) -> Result<(), RspClientError> {
    let stream_id = stream_id_value(&stream_reply);
    let status = stream_status_code(&stream_reply);

    let mut st = state.lock().await;

    if let Some(stream_id) = stream_id.clone() {
        if let Some(tx) = st.pending_connects.remove(&stream_id) {
            if matches!(
                status,
                STREAM_SUCCESS
                    | STREAM_CONNECT_REFUSED
                    | STREAM_CONNECT_TIMEOUT
                    | STREAM_ERROR
                    | STREAM_IN_USE
                    | STREAM_INVALID_FLAGS
            ) {
                let _ = tx.send(Ok(stream_reply.clone()));
                return Ok(());
            }
            st.pending_connects.insert(stream_id.clone(), tx);
        }

        if let Some(tx) = st.pending_listens.remove(&stream_id) {
            if matches!(status, STREAM_SUCCESS | STREAM_ERROR | STREAM_IN_USE | STREAM_INVALID_FLAGS) {
                let _ = tx.send(Ok(stream_reply.clone()));
                return Ok(());
            }
            st.pending_listens.insert(stream_id.clone(), tx);
        }

        if let Some(new_stream_id) = new_stream_id_value(&stream_reply) {
            let source_node_id = decode_source_node_id(msg)
                .or_else(|| st.stream_routes.get(&stream_id).cloned());
            if let Some(source_node_id) = source_node_id {
                st.stream_routes.insert(new_stream_id, source_node_id);
            }
        }

        if let Some(waiters) = st.awaited_stream_replies.get_mut(&stream_id) {
            if let Some(waiter) = waiters.pop_front() {
                let _ = waiter.tx.send(Ok(stream_reply.clone()));
                if waiters.is_empty() {
                    st.awaited_stream_replies.remove(&stream_id);
                }
                return Ok(());
            }
        }

        st.stream_reply_queues
            .entry(stream_id)
            .or_default()
            .push_back(stream_reply.clone());
    }

    st.pending_stream_replies.push_back(stream_reply);
    Ok(())
}

async fn handle_endorsement_done(
    state: &std::sync::Arc<Mutex<ClientState>>,
    msg: &Value,
    endorsement_done: Value,
) -> Result<(), RspClientError> {
    let Some(source_node_id) = decode_source_node_id(msg) else {
        return Ok(());
    };
    let pending_key = encode_node_id_for_field(&source_node_id)?;
    let mut st = state.lock().await;
    if let Some(tx) = st.pending_endorsements.remove(&pending_key) {
        let _ = tx.send(Ok(endorsement_done));
    }
    Ok(())
}

async fn handle_name_reply(state: &std::sync::Arc<Mutex<ClientState>>, name_reply: Value) {
    let mut st = state.lock().await;
    if let Some(tx) = st.pending_name_reply.take() {
        let _ = tx.send(Ok(name_reply));
    }
}

async fn remove_stream_waiter(
    state: &std::sync::Arc<Mutex<ClientState>>,
    stream_id: &str,
    waiter_id: u64,
) {
    let mut st = state.lock().await;
    if let Some(waiters) = st.awaited_stream_replies.get_mut(stream_id) {
        if let Some(index) = waiters.iter().position(|waiter| waiter.id == waiter_id) {
            waiters.remove(index);
        }
        if waiters.is_empty() {
            st.awaited_stream_replies.remove(stream_id);
        }
    }
}

async fn reject_all_pending(state: &std::sync::Arc<Mutex<ClientState>>, reason: &str) {
    let mut st = state.lock().await;
    let reason = reason.to_string();

    for (_, pending) in st.pending_pings.drain() {
        let _ = pending.tx.send(Err(reason.clone()));
    }
    for (_, tx) in st.pending_connects.drain() {
        let _ = tx.send(Err(reason.clone()));
    }
    for (_, tx) in st.pending_listens.drain() {
        let _ = tx.send(Err(reason.clone()));
    }
    for (_, waiters) in st.awaited_stream_replies.drain() {
        for waiter in waiters {
            let _ = waiter.tx.send(Err(reason.clone()));
        }
    }
    for (_, tx) in st.pending_endorsements.drain() {
        let _ = tx.send(Err(reason.clone()));
    }
    if let Some(tx) = st.pending_resource_list.take() {
        let _ = tx.send(Err(reason.clone()));
    }
    if let Some(tx) = st.pending_name_reply.take() {
        let _ = tx.send(Err(reason));
    }
}

fn has_field(value: &Value, field: &str) -> bool {
    value
        .as_object()
        .and_then(|obj| obj.get(field))
        .map(|field| !field.is_null())
        .unwrap_or(false)
}

fn map_from_pairs<const N: usize>(pairs: [(&str, Value); N]) -> Map<String, Value> {
    let mut map = Map::new();
    for (key, value) in pairs {
        map.insert(key.to_string(), value);
    }
    map
}

fn pack_service_message(type_name: &str, mut fields: Map<String, Value>) -> Value {
    fields.insert(
        "@type".to_string(),
        Value::String(format!("{SERVICE_MESSAGE_TYPE_PREFIX}{type_name}")),
    );
    Value::Object(fields)
}

fn service_message_type_name(msg: &Value) -> Option<String> {
    let type_url = msg
        .get("service_message")
        .and_then(Value::as_object)
        .and_then(|service| service.get("@type"))
        .and_then(Value::as_str)?;
    let slash = type_url.rfind('/').unwrap_or(0);
    if slash > 0 {
        Some(type_url[slash + 1..].to_string())
    } else {
        Some(type_url.to_string())
    }
}

fn unpack_service_message(msg: &Value) -> Value {
    let mut copy = msg
        .get("service_message")
        .and_then(Value::as_object)
        .cloned()
        .unwrap_or_default();
    copy.remove("@type");
    Value::Object(copy)
}

fn stream_status_code(stream_reply: &Value) -> i32 {
    stream_reply
        .as_object()
        .and_then(|obj| obj.get("error"))
        .and_then(Value::as_i64)
        .unwrap_or(STREAM_SUCCESS as i64) as i32
}

fn stream_id_value(payload: &Value) -> Option<String> {
    payload
        .as_object()
        .and_then(|obj| obj.get("stream_id"))
        .and_then(Value::as_object)
        .and_then(|obj| obj.get("value"))
        .and_then(Value::as_str)
        .map(ToOwned::to_owned)
        .or_else(|| {
            payload
                .as_object()
                .and_then(|obj| obj.get("socket_id"))
                .and_then(Value::as_object)
                .and_then(|obj| obj.get("value"))
                .and_then(Value::as_str)
                .map(ToOwned::to_owned)
        })
}

fn new_stream_id_value(payload: &Value) -> Option<String> {
    payload
        .as_object()
        .and_then(|obj| obj.get("new_stream_id"))
        .and_then(Value::as_object)
        .and_then(|obj| obj.get("value"))
        .and_then(Value::as_str)
        .map(ToOwned::to_owned)
        .or_else(|| {
            payload
                .as_object()
                .and_then(|obj| obj.get("new_socket_id"))
                .and_then(Value::as_object)
                .and_then(|obj| obj.get("value"))
                .and_then(Value::as_str)
                .map(ToOwned::to_owned)
        })
}

fn set_id_field(payload: &mut Value, field_name: &str, value: &str) {
    if let Some(object) = payload.as_object_mut() {
        let entry = object
            .entry(field_name.to_string())
            .or_insert_with(|| Value::Object(Map::new()));
        if !entry.is_object() {
            *entry = Value::Object(Map::new());
        }
        if let Some(entry_object) = entry.as_object_mut() {
            entry_object.insert("value".to_string(), Value::String(value.to_string()));
        }
    }
}

fn decode_source_node_id(message: &Value) -> Option<String> {
    let source_b64 = message
        .as_object()
        .and_then(|obj| obj.get("source"))
        .and_then(Value::as_object)
        .and_then(|source| source.get("value"))
        .and_then(Value::as_str)?;
    decode_node_id_field(source_b64).ok()
}

fn parse_transport_spec(transport_spec: &str) -> Result<(String, u16), RspClientError> {
    let (transport, params) = transport_spec
        .split_once(':')
        .ok_or_else(|| RspClientError::Transport("transport spec must have form <transport>:<params>".to_string()))?;
    if transport != "tcp" {
        return Err(RspClientError::Transport(
            "the Rust client currently supports only tcp transport".to_string(),
        ));
    }
    let split = params
        .rfind(':')
        .ok_or_else(|| RspClientError::Transport("tcp transport must have host:port parameters".to_string()))?;
    let host = params[..split].to_string();
    let port = params[split + 1..]
        .parse::<u16>()
        .map_err(|_| RspClientError::Transport("tcp port must be a valid u16 integer".to_string()))?;
    Ok((host, port))
}

fn normalize_guid(value: &str) -> Result<String, RspClientError> {
    let compact = value
        .chars()
        .filter(|ch| *ch != '{' && *ch != '}' && *ch != '-')
        .collect::<String>()
        .to_lowercase();
    if compact.len() != 32 || !compact.chars().all(|ch| ch.is_ascii_hexdigit()) {
        return Err(RspClientError::InvalidGuid(value.to_string()));
    }
    Ok(compact)
}

fn parse_guid(value: &str) -> Result<(u64, u64), RspClientError> {
    let normalized = normalize_guid(value)?;
    let high = u64::from_str_radix(&normalized[..16], 16)
        .map_err(|_| RspClientError::InvalidGuid(value.to_string()))?;
    let low = u64::from_str_radix(&normalized[16..], 16)
        .map_err(|_| RspClientError::InvalidGuid(value.to_string()))?;
    Ok((high, low))
}

fn guid_from_bytes(bytes: &[u8]) -> Result<String, RspClientError> {
    if bytes.len() != 16 {
        return Err(RspClientError::InvalidGuid("GUID bytes must be exactly 16 bytes".to_string()));
    }
    let hex = hex::encode(bytes);
    Ok(format!(
        "{}-{}-{}-{}-{}",
        &hex[0..8],
        &hex[8..12],
        &hex[12..16],
        &hex[16..20],
        &hex[20..32]
    ))
}

fn encode_node_id_for_field(node_id: &str) -> Result<String, RspClientError> {
    let (high, low) = parse_guid(node_id)?;
    let mut bytes = [0u8; 16];
    if cfg!(target_endian = "little") {
        bytes[..8].copy_from_slice(&high.to_le_bytes());
        bytes[8..].copy_from_slice(&low.to_le_bytes());
    } else {
        bytes[..8].copy_from_slice(&high.to_be_bytes());
        bytes[8..].copy_from_slice(&low.to_be_bytes());
    }
    Ok(BASE64.encode(bytes))
}

fn decode_node_id_field(b64_value: &str) -> Result<String, RspClientError> {
    let bytes = BASE64
        .decode(b64_value)
        .map_err(|_| RspClientError::InvalidGuid("invalid base64 for node id field".to_string()))?;
    if bytes.len() != 16 {
        return Err(RspClientError::InvalidGuid("node id field must decode to 16 bytes".to_string()));
    }
    let mut high_bytes = [0u8; 8];
    let mut low_bytes = [0u8; 8];
    high_bytes.copy_from_slice(&bytes[..8]);
    low_bytes.copy_from_slice(&bytes[8..]);

    let high = if cfg!(target_endian = "little") {
        u64::from_le_bytes(high_bytes)
    } else {
        u64::from_be_bytes(high_bytes)
    };
    let low = if cfg!(target_endian = "little") {
        u64::from_le_bytes(low_bytes)
    } else {
        u64::from_be_bytes(low_bytes)
    };

    let mut canonical = [0u8; 16];
    canonical[..8].copy_from_slice(&high.to_be_bytes());
    canonical[8..].copy_from_slice(&low.to_be_bytes());
    guid_from_bytes(&canonical)
}

fn encode_node_id_for_signer(node_id: &str) -> Result<String, RspClientError> {
    let (high, low) = parse_guid(node_id)?;
    let mut bytes = [0u8; 16];
    bytes[..8].copy_from_slice(&high.to_be_bytes());
    bytes[8..].copy_from_slice(&low.to_be_bytes());
    Ok(BASE64.encode(bytes))
}

fn decode_signer_node_id(b64_value: &str) -> Result<String, RspClientError> {
    let bytes = BASE64
        .decode(b64_value)
        .map_err(|_| RspClientError::InvalidGuid("invalid base64 for signer node id".to_string()))?;
    guid_from_bytes(&bytes)
}

fn random_b64_16() -> String {
    let mut bytes = [0u8; 16];
    OsRng.fill_bytes(&mut bytes);
    BASE64.encode(bytes)
}

fn node_id_from_public_key_pem(public_key_pem: &str) -> Result<String, RspClientError> {
    let verifying_key = VerifyingKey::from_public_key_pem(public_key_pem)
        .map_err(|err| RspClientError::Crypto(format!("failed to parse public key PEM: {err}")))?;
    let der = verifying_key
        .to_public_key_der()
        .map_err(|err| RspClientError::Crypto(format!("failed to encode public key DER: {err}")))?;
    let digest = Sha256::digest(der.as_bytes());
    guid_from_bytes(&digest[..16])
}

fn verify_rsp_message_signature(public_key_pem: &str, message: &Value) -> Result<bool, RspClientError> {
    let signature_block = message
        .get("signature")
        .and_then(Value::as_object)
        .ok_or_else(|| RspClientError::Protocol("missing signature block".to_string()))?;
    let algorithm = signature_block
        .get("algorithm")
        .and_then(Value::as_i64)
        .unwrap_or_default() as i32;
    if algorithm != P256_ALGORITHM {
        return Ok(false);
    }

    let signer_b64 = signature_block
        .get("signer")
        .and_then(Value::as_object)
        .and_then(|signer| signer.get("value"))
        .and_then(Value::as_str)
        .ok_or_else(|| RspClientError::Protocol("signature signer missing".to_string()))?;
    let signer_node_id = decode_signer_node_id(signer_b64)?;
    let expected_node_id = node_id_from_public_key_pem(public_key_pem)?;
    if signer_node_id != expected_node_id {
        return Ok(false);
    }

    let signature_b64 = signature_block
        .get("signature")
        .and_then(Value::as_str)
        .ok_or_else(|| RspClientError::Protocol("signature payload missing".to_string()))?;
    let signature_bytes = BASE64
        .decode(signature_b64)
        .map_err(|_| RspClientError::Protocol("signature payload is not base64".to_string()))?;
    let signature = Signature::from_der(&signature_bytes)
        .map_err(|err| RspClientError::Crypto(format!("failed to parse DER signature: {err}")))?;
    let verifying_key = VerifyingKey::from_public_key_pem(public_key_pem)
        .map_err(|err| RspClientError::Crypto(format!("failed to parse public key PEM: {err}")))?;

    let digest = messages::hash_rsp_message(message)?;
    Ok(verifying_key.verify(&digest, &signature).is_ok())
}

fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64
}

#[cfg(test)]
mod tests {
    use super::{decode_node_id_field, encode_node_id_for_field, normalize_guid, parse_transport_spec};

    #[test]
    fn normalize_guid_accepts_dashed_and_compact_forms() {
        let dashed = "01234567-89ab-cdef-0123-456789abcdef";
        let compact = "0123456789abcdef0123456789abcdef";
        assert_eq!(normalize_guid(dashed).expect("dashed"), compact);
        assert_eq!(normalize_guid(compact).expect("compact"), compact);
    }

    #[test]
    fn node_id_field_round_trip() {
        let node_id = "01234567-89ab-cdef-0123-456789abcdef";
        let encoded = encode_node_id_for_field(node_id).expect("encode");
        let decoded = decode_node_id_field(&encoded).expect("decode");
        assert_eq!(decoded, node_id);
    }

    #[test]
    fn parse_transport_spec_supports_ipv4() {
        let (host, port) = parse_transport_spec("tcp:127.0.0.1:12345").expect("parse");
        assert_eq!(host, "127.0.0.1");
        assert_eq!(port, 12345);
    }
}
