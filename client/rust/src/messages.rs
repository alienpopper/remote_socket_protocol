use std::collections::HashMap;

use base64::engine::general_purpose::STANDARD as BASE64;
use base64::Engine;
use once_cell::sync::Lazy;
use serde::Deserialize;
use serde_json::{Map, Value};
use sha2::{Digest, Sha256};
use thiserror::Error;

include!(concat!(env!("OUT_DIR"), "/messages_generated.rs"));

#[derive(Debug, Clone, Deserialize)]
pub struct Schema {
    pub enums: HashMap<String, HashMap<String, i32>>,
    pub messages: HashMap<String, MessageSchema>,
}

#[derive(Debug, Clone, Deserialize)]
pub struct MessageSchema {
    pub fields: Vec<FieldSchema>,
    pub oneofs: Vec<OneofSchema>,
}

#[derive(Debug, Clone, Deserialize)]
pub struct FieldSchema {
    pub name: String,
    pub number: u32,
    pub kind: String,
    #[serde(rename = "type")]
    pub type_name: String,
    pub repeated: bool,
    pub has_presence: bool,
    pub oneof: Option<String>,
    pub encrypted: bool,
}

#[derive(Debug, Clone, Deserialize)]
pub struct OneofSchema {
    pub name: String,
    pub fields: Vec<String>,
}

pub static SCHEMA: Lazy<Schema> = Lazy::new(|| {
    serde_json::from_str(SCHEMA_JSON).expect("generated Rust schema JSON must remain valid")
});

#[derive(Debug, Error)]
pub enum MessageError {
    #[error("unknown message type: {0}")]
    UnknownMessageType(String),
    #[error("unknown enum type: {0}")]
    UnknownEnumType(String),
    #[error("unknown enum value '{value}' for enum '{enum_name}'")]
    UnknownEnumValue { enum_name: String, value: String },
    #[error("expected object for message type {0}")]
    ExpectedObject(String),
    #[error("expected array for repeated field {0}")]
    ExpectedArray(String),
    #[error("expected scalar type {expected} for field {field}")]
    InvalidScalarType { field: String, expected: &'static str },
    #[error("invalid integer for field {field}")]
    InvalidInteger { field: String },
    #[error("invalid base64 bytes for field {field}")]
    InvalidBase64 { field: String },
}

#[derive(Default)]
struct MessageHasher {
    digest: Sha256,
}

impl MessageHasher {
    fn feed(&mut self, bytes: &[u8]) {
        self.digest.update(bytes);
    }

    fn feed_u8(&mut self, value: u8) {
        self.feed(&[value]);
    }

    fn feed_u32(&mut self, value: u32) {
        self.feed(&value.to_be_bytes());
    }

    fn feed_i32(&mut self, value: i32) {
        self.feed(&value.to_be_bytes());
    }

    fn feed_u64(&mut self, value: u64) {
        self.feed(&value.to_be_bytes());
    }

    fn feed_bool(&mut self, value: bool) {
        self.feed_u8(if value { 1 } else { 0 });
    }

    fn feed_bytes(&mut self, bytes: &[u8]) {
        self.feed_u32(bytes.len() as u32);
        self.feed(bytes);
    }

    fn tag(&mut self, field_number: u32) {
        self.feed_u32(field_number);
    }

    fn finalize(self) -> [u8; 32] {
        self.digest.finalize().into()
    }
}

fn field_present(value: Option<&Map<String, Value>>, name: &str) -> bool {
    match value.and_then(|obj| obj.get(name)) {
        Some(Value::Null) | None => false,
        Some(_) => true,
    }
}

fn as_i64(value: &Value, field_name: &str) -> Result<i64, MessageError> {
    if let Some(number) = value.as_i64() {
        return Ok(number);
    }
    if let Some(number) = value.as_u64() {
        return Ok(number as i64);
    }
    if let Some(text) = value.as_str() {
        return text
            .parse::<i64>()
            .map_err(|_| MessageError::InvalidInteger { field: field_name.to_string() });
    }
    Err(MessageError::InvalidInteger { field: field_name.to_string() })
}

fn as_bool(value: &Value, field_name: &str) -> Result<bool, MessageError> {
    value
        .as_bool()
        .ok_or_else(|| MessageError::InvalidScalarType {
            field: field_name.to_string(),
            expected: "bool",
        })
}

fn as_string<'a>(value: &'a Value, field_name: &str) -> Result<&'a str, MessageError> {
    value.as_str().ok_or_else(|| MessageError::InvalidScalarType {
        field: field_name.to_string(),
        expected: "string",
    })
}

fn default_scalar_value(field_type: &str) -> Value {
    match field_type {
        "bool" => Value::Bool(false),
        "string" | "bytes" => Value::String(String::new()),
        _ => Value::Number(0.into()),
    }
}

fn hash_scalar(field: &FieldSchema, value: &Value, hasher: &mut MessageHasher) -> Result<(), MessageError> {
    let effective = if value.is_null() {
        default_scalar_value(&field.type_name)
    } else {
        value.clone()
    };

    match field.type_name.as_str() {
        "bool" => hasher.feed_bool(as_bool(&effective, &field.name)?),
        "string" => hasher.feed_bytes(as_string(&effective, &field.name)?.as_bytes()),
        "bytes" => {
            let text = as_string(&effective, &field.name)?;
            let decoded = BASE64
                .decode(text)
                .map_err(|_| MessageError::InvalidBase64 { field: field.name.clone() })?;
            hasher.feed_bytes(&decoded);
        }
        "uint32" | "fixed32" => hasher.feed_u32(as_i64(&effective, &field.name)? as u32),
        "int32" | "sint32" | "sfixed32" => hasher.feed_i32(as_i64(&effective, &field.name)? as i32),
        "uint64" | "fixed64" | "int64" | "sint64" | "sfixed64" => {
            hasher.feed_u64(as_i64(&effective, &field.name)? as u64);
        }
        _ => {
            return Err(MessageError::InvalidScalarType {
                field: field.name.clone(),
                expected: "supported scalar",
            })
        }
    }

    Ok(())
}

fn hash_enum(field: &FieldSchema, value: &Value, hasher: &mut MessageHasher) -> Result<(), MessageError> {
    let enum_values = SCHEMA
        .enums
        .get(&field.type_name)
        .ok_or_else(|| MessageError::UnknownEnumType(field.type_name.clone()))?;

    let numeric = if value.is_null() {
        0i32
    } else if let Some(number) = value.as_i64() {
        number as i32
    } else if let Some(number) = value.as_u64() {
        number as i32
    } else if let Some(name) = value.as_str() {
        *enum_values.get(name).ok_or_else(|| MessageError::UnknownEnumValue {
            enum_name: field.type_name.clone(),
            value: name.to_string(),
        })?
    } else {
        return Err(MessageError::InvalidInteger { field: field.name.clone() });
    };

    hasher.feed_u32(numeric as u32);
    Ok(())
}

fn hash_field_value(field: &FieldSchema, value: &Value, hasher: &mut MessageHasher) -> Result<(), MessageError> {
    match field.kind.as_str() {
        "scalar" => hash_scalar(field, value, hasher),
        "enum" => hash_enum(field, value, hasher),
        "message" => hash_message_object(&field.type_name, value, hasher),
        _ => Err(MessageError::InvalidScalarType {
            field: field.name.clone(),
            expected: "supported field kind",
        }),
    }
}

fn hash_message_object(type_name: &str, value: &Value, hasher: &mut MessageHasher) -> Result<(), MessageError> {
    let definition = SCHEMA
        .messages
        .get(type_name)
        .ok_or_else(|| MessageError::UnknownMessageType(type_name.to_string()))?;

    let object = if value.is_null() {
        None
    } else {
        value
            .as_object()
            .map(Some)
            .ok_or_else(|| MessageError::ExpectedObject(type_name.to_string()))?
    };

    for field in &definition.fields {
        if type_name == "RSPMessage" && field.name == "signature" {
            continue;
        }

        if field.repeated {
            hasher.tag(field.number);
            let list = match object.and_then(|obj| obj.get(&field.name)) {
                None | Some(Value::Null) => Vec::new(),
                Some(Value::Array(items)) => items.clone(),
                Some(_) => {
                    return Err(MessageError::ExpectedArray(format!("{type_name}.{}", field.name)));
                }
            };
            hasher.feed_u32(list.len() as u32);
            for item in &list {
                hash_field_value(field, item, hasher)?;
            }
            continue;
        }

        if field.has_presence && !field_present(object, &field.name) {
            continue;
        }

        hasher.tag(field.number);
        let field_value = object
            .and_then(|obj| obj.get(&field.name))
            .unwrap_or(&Value::Null);
        hash_field_value(field, field_value, hasher)?;
    }

    Ok(())
}

pub fn hash_message(type_name: &str, value: &Value) -> Result<[u8; 32], MessageError> {
    let mut hasher = MessageHasher::default();
    hash_message_object(type_name, value, &mut hasher)?;
    Ok(hasher.finalize())
}

pub fn hash_rsp_message(value: &Value) -> Result<[u8; 32], MessageError> {
    hash_message("RSPMessage", value)
}

pub fn hash_endorsement(value: &Value) -> Result<[u8; 32], MessageError> {
    hash_message("Endorsement", value)
}

#[cfg(test)]
mod tests {
    use serde_json::json;

    use super::{hash_rsp_message, SCHEMA};

    #[test]
    fn schema_contains_stream_fields() {
        let connect = SCHEMA
            .messages
            .get("ConnectTCPRequest")
            .expect("ConnectTCPRequest missing");
        assert!(connect.fields.iter().any(|f| f.name == "stream_id"));
        assert!(!connect.fields.iter().any(|f| f.name == "socket_number"));

        let reply = SCHEMA.messages.get("StreamReply").expect("StreamReply missing");
        assert!(reply.fields.iter().any(|f| f.name == "stream_id"));
        assert!(reply.fields.iter().any(|f| f.name == "new_stream_id"));
        assert!(!reply.fields.iter().any(|f| f.name == "socket_id"));
    }

    #[test]
    fn hash_changes_when_payload_changes() {
        let message_a = json!({
            "source": {"value": "c291cmNlLW5vZGUtMTIzNDU="},
            "nonce": {"value": "bWVzc2FnZS1ub25jZS0xMjM="},
            "ping_request": {
                "nonce": {"value": "cGluZy1ub25jZS0xMjM0NTY="},
                "sequence": 7
            }
        });
        let mut message_b = message_a.clone();
        message_b["ping_request"]["sequence"] = json!(8);

        let hash_a = hash_rsp_message(&message_a).expect("hash_a");
        let hash_b = hash_rsp_message(&message_b).expect("hash_b");
        assert_ne!(hash_a, hash_b);
    }
}
