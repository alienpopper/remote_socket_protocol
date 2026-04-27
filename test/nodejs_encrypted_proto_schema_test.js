'use strict';

const assert = require('assert');
const messages = require('../client/nodejs/messages');
const {JsonEncoding} = require('../client/nodejs_full/encodings/json');

function b64(bytes) {
    return Buffer.from(bytes, 'utf8').toString('base64');
}

function field(typeName, fieldName) {
    const definition = messages.schema.messages[typeName];
    assert(definition, `missing message definition for ${typeName}`);
    const found = definition.fields.find((entry) => entry.name === fieldName);
    assert(found, `missing field ${typeName}.${fieldName}`);
    return found;
}

function main() {
    assert.strictEqual(field('FieldEncryptionFixture', 'clear_text').encrypted, false);
    assert.strictEqual(field('FieldEncryptionFixture', 'secret_text').encrypted, true);
    assert.strictEqual(field('FieldEncryptionFixture', 'secret_bytes').encrypted, true);
    assert.ok(field('RSPMessage', 'aes_key_negotiation_request'));
    assert.ok(field('RSPMessage', 'aes_key_negotiation_reply'));

    const message = messages.createRSPMessage({
        source: {value: b64('source-node-12345')},
        nonce: {value: b64('message-nonce-123')},
        ping_request: {
            nonce: {value: b64('ping-nonce-123456')},
            sequence: 7,
        },
        encrypted_fields: [
            {
                path: {segments: ['service_message', 'secret_text']},
                iv: b64('123456789012'),
                ciphertext: b64('ciphertext-a'),
                tag: b64('0123456789ABCDEF'),
                algorithm: 1,
            },
        ],
    });

    const hashA = messages.hashRSPMessage(message).toString('hex');
    const modified = JSON.parse(JSON.stringify(message));
    modified.encrypted_fields[0].ciphertext = b64('ciphertext-b');
    const hashB = messages.hashRSPMessage(modified).toString('hex');
    assert.notStrictEqual(hashA, hashB, 'encrypted field data should affect canonical hash');

    const jsonEncoding = new JsonEncoding();
    const wirePayload = jsonEncoding.encode(message);
    const roundTrip = jsonEncoding.decode(wirePayload);
    assert.deepStrictEqual(roundTrip.encrypted_fields[0].path.segments, ['service_message', 'secret_text']);
    assert.strictEqual(roundTrip.encrypted_fields[0].algorithm, 1);
    assert.strictEqual(roundTrip.encrypted_fields[0].ciphertext, message.encrypted_fields[0].ciphertext);

    const keyNegotiationMessage = messages.createRSPMessage({
        source: {value: b64('source-node-12345')},
        destination: {value: b64('destination-node')},
        aes_key_negotiation_request: {
            key_id: {value: b64('1234567890ABCDEF')},
            ephemeral_public_key: {
                algorithm: messages.SIGNATURE_ALGORITHMS.P256,
                public_key: b64('ephemeral-public-key'),
            },
            requested_lifetime_ms: 5000,
            algorithm: messages.KEY_NEGOTIATION_ALGORITHM
                .KEY_NEGOTIATION_ALGORITHM_P256_SHA256_AES256,
        },
    });
    const keyHashA = messages.hashRSPMessage(keyNegotiationMessage).toString('hex');
    const modifiedKeyMessage = JSON.parse(JSON.stringify(keyNegotiationMessage));
    modifiedKeyMessage.aes_key_negotiation_request.key_id.value = b64('ABCDE1234567890F');
    const keyHashB = messages.hashRSPMessage(modifiedKeyMessage).toString('hex');
    assert.notStrictEqual(keyHashA, keyHashB, 'AES key negotiation fields should affect canonical hash');

    console.log('nodejs_encrypted_proto_schema_test passed');
}

main();
