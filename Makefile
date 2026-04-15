PROJECT_ROOT := .
BUILD_DIR := build
BIN_DIR := $(BUILD_DIR)/bin
OBJ_DIR := $(BUILD_DIR)/obj
PROTOBUF_GENERATED_DIR := $(BUILD_DIR)/gen

include common/Makefile
include os/Makefile
include client/cpp/Makefile
include client/cpp_full/Makefile

TARGET := $(BIN_DIR)/resource_manager
KEYPAIR_TEST_TARGET := $(BIN_DIR)/keypair_test
BASE_TYPES_TEST_TARGET := $(BIN_DIR)/base_types_test
CLIENT_TEST_TARGET := $(BIN_DIR)/client_test
ENDORSEMENT_TEST_TARGET := $(BIN_DIR)/endorsement_test
MESSAGE_QUEUE_TEST_TARGET := $(BIN_DIR)/message_queue_test
MQ_ASCII_HANDSHAKE_TEST_TARGET := $(BIN_DIR)/mq_ascii_handshake_test
MQ_SIGNING_TEST_TARGET := $(BIN_DIR)/mq_signing_test
MQ_AUTHN_TEST_TARGET := $(BIN_DIR)/mq_authn_test
MQ_AUTHZ_TEST_TARGET := $(BIN_DIR)/mq_authz_test
MESSAGE_HASH_TEST_TARGET := $(BIN_DIR)/message_hash_test
RUNTIME_PROTO_TEST_TARGET := $(BIN_DIR)/runtime_proto_test
TRANSCODING_TEST_TARGET := $(BIN_DIR)/transcoding_test
RSP_ENDORSEMENT_TOOL_TARGET := $(BIN_DIR)/rsp_endorsement
RSP_CLI_TOOL_TARGET := $(BIN_DIR)/rsp
NODE_TEST_TARGET := $(BIN_DIR)/node_test
RESOURCE_SERVICE_TEST_TARGET := $(BIN_DIR)/resource_service_test
RESOURCE_SERVICE_JSON_TEST_TARGET := $(BIN_DIR)/resource_service_json_test
NODEJS_PING_FIXTURE_TARGET := $(BIN_DIR)/nodejs_ping_fixture
RESOURCE_SERVICE_TARGET := $(BIN_DIR)/resource_service
BSD_SOCKETS_TARGET := $(BIN_DIR)/rsp_bsd_sockets
ENDORSEMENT_SERVICE_TEST_TARGET := $(BIN_DIR)/endorsement_service_test
ENDORSEMENT_SERVICE_TARGET := $(BIN_DIR)/endorsement_service
TRANSPORT_MEMORY_TEST_TARGET := $(BIN_DIR)/transport_memory_test
RSP_SSHD_TARGET := $(BIN_DIR)/rsp_sshd
RSP_SSH_TARGET  := $(BIN_DIR)/rsp_ssh
LIB_DIR := $(BUILD_DIR)/lib
RSPCLIENT_STATIC_TARGET := $(LIB_DIR)/librspclient.a
RSPCLIENT_SHARED_TARGET := $(LIB_DIR)/librspclient.so
RSPFULLCLIENT_STATIC_TARGET := $(LIB_DIR)/librspclient_full.a
PROTOBUF_GENERATED_SOURCE := $(PROTOBUF_GENERATED_DIR)/messages.pb.cc
PROTOBUF_GENERATED_HEADER := $(PROTOBUF_GENERATED_DIR)/messages.pb.h
BSD_SOCKETS_GENERATED_SOURCE := $(PROTOBUF_GENERATED_DIR)/resource_service/bsd_sockets/bsd_sockets.pb.cc
BSD_SOCKETS_GENERATED_HEADER := $(PROTOBUF_GENERATED_DIR)/resource_service/bsd_sockets/bsd_sockets.pb.h
BSD_SOCKETS_GENERATED_OBJECT := $(OBJ_DIR)/build/gen/resource_service/bsd_sockets/bsd_sockets.pb.o
SSHD_GENERATED_SOURCE := $(PROTOBUF_GENERATED_DIR)/resource_service/sshd/sshd.pb.cc
SSHD_GENERATED_HEADER := $(PROTOBUF_GENERATED_DIR)/resource_service/sshd/sshd.pb.h
SSHD_GENERATED_OBJECT := $(OBJ_DIR)/build/gen/resource_service/sshd/sshd.pb.o
PROTOBUF_GENERATED_OBJECT := $(OBJ_DIR)/build/gen/messages.pb.o $(BSD_SOCKETS_GENERATED_OBJECT) $(SSHD_GENERATED_OBJECT)
EMBED_DESCRIPTOR_SCRIPT := scripts/embed_descriptor.py
BSD_SOCKETS_DESC := $(PROTOBUF_GENERATED_DIR)/resource_service/bsd_sockets/bsd_sockets.desc
BSD_SOCKETS_DESC_HEADER := $(PROTOBUF_GENERATED_DIR)/resource_service/bsd_sockets/bsd_sockets_desc.hpp
SSHD_DESC := $(PROTOBUF_GENERATED_DIR)/resource_service/sshd/sshd.desc
SSHD_DESC_HEADER := $(PROTOBUF_GENERATED_DIR)/resource_service/sshd/sshd_desc.hpp

TOP_BIN_DIR := bin
ESP_RM_TARGET := $(TOP_BIN_DIR)/esp_rm
ESP_ES_TARGET := $(TOP_BIN_DIR)/esp_es

GENERATE_MESSAGES_SCRIPT := scripts/generate_messages.py
NODEJS_MESSAGES_JS := client/nodejs/messages.js
PYTHON_MESSAGES_PY := client/python/messages.py

COMMON_SOURCES := \
	$(COMMON_BASE_TYPES_SOURCE) \
	$(COMMON_NODE_SOURCE) \
	$(COMMON_KEYPAIR_SOURCE) \
	$(COMMON_MESSAGE_QUEUE_SOURCE) \
	$(COMMON_MESSAGE_QUEUE_ASCII_HANDSHAKE_SOURCE) \
	$(COMMON_MESSAGE_QUEUE_AUTHN_SOURCE) \
	$(COMMON_MESSAGE_QUEUE_AUTHZ_SOURCE) \
	$(COMMON_MESSAGE_QUEUE_SIGNING_SOURCE) \
	$(COMMON_ENCODING_SOURCE) \
	$(COMMON_PROTOBUF_ENCODING_SOURCE) \
	$(COMMON_JSON_ENCODING_SOURCE) \
	$(COMMON_ENDORSEMENT_SOURCE) \
	common/endorsement/endorsement_text.cpp \
	$(COMMON_TRANSPORT_SOURCE) \
	$(COMMON_TRANSPORT_TCP_SOURCE) \
	$(COMMON_TRANSPORT_MEMORY_SOURCE) \
	resource_manager/resource_manager.cpp \
	resource_manager/resource_manager_main.cpp

CLIENT_LIBRARY_SOURCES := \
	$(COMMON_BASE_TYPES_SOURCE) \
	$(COMMON_NODE_SOURCE) \
	$(COMMON_KEYPAIR_SOURCE) \
	$(COMMON_MESSAGE_QUEUE_SOURCE) \
	$(COMMON_MESSAGE_QUEUE_ASCII_HANDSHAKE_SOURCE) \
	$(COMMON_MESSAGE_QUEUE_AUTHN_SOURCE) \
	$(COMMON_MESSAGE_QUEUE_SIGNING_SOURCE) \
	$(COMMON_ENCODING_SOURCE) \
	$(COMMON_PROTOBUF_ENCODING_SOURCE) \
	$(COMMON_JSON_ENCODING_SOURCE) \
	$(COMMON_ENDORSEMENT_SOURCE) \
	$(COMMON_TRANSPORT_SOURCE) \
	$(COMMON_TRANSPORT_TCP_SOURCE) \
	$(COMMON_TRANSPORT_MEMORY_SOURCE) \
	$(CLIENT_CPP_RSP_CLIENT_MESSAGE_SOURCE) \
	$(CLIENT_CPP_RSP_CLIENT_SOURCE)

FULL_CLIENT_LIBRARY_SOURCES := \
	$(COMMON_BASE_TYPES_SOURCE) \
	$(COMMON_NODE_SOURCE) \
	$(COMMON_KEYPAIR_SOURCE) \
	$(COMMON_MESSAGE_QUEUE_SOURCE) \
	$(COMMON_MESSAGE_QUEUE_ASCII_HANDSHAKE_SOURCE) \
	$(COMMON_MESSAGE_QUEUE_AUTHN_SOURCE) \
	$(COMMON_MESSAGE_QUEUE_SIGNING_SOURCE) \
	$(COMMON_ENCODING_SOURCE) \
	$(COMMON_PROTOBUF_ENCODING_SOURCE) \
	$(COMMON_JSON_ENCODING_SOURCE) \
	$(COMMON_TRANSPORT_SOURCE) \
	$(COMMON_TRANSPORT_TCP_SOURCE) \
	$(COMMON_TRANSPORT_MEMORY_SOURCE) \
	$(CLIENT_CPP_FULL_RSP_CLIENT_SOURCE)

RESOURCE_SERVICE_SOURCES := \
	$(FULL_CLIENT_LIBRARY_SOURCES) \
	resource_service/resource_service.cpp \
	resource_service/bsd_sockets/resource_service_bsd_sockets.cpp \
	resource_service/resource_service_main.cpp

BSD_SOCKETS_SOURCES := \
	$(FULL_CLIENT_LIBRARY_SOURCES) \
	resource_service/resource_service.cpp \
	resource_service/bsd_sockets/resource_service_bsd_sockets.cpp \
	resource_service/bsd_sockets/resource_service_bsd_sockets_main.cpp

ENDORSEMENT_SERVICE_SOURCES := \
	$(COMMON_ENDORSEMENT_SOURCE) \
	$(FULL_CLIENT_LIBRARY_SOURCES) \
	endorsement_service/endorsement_service.cpp \
	endorsement_service/endorsement_service_main.cpp

ifeq ($(OS),Windows_NT)
	TARGET := $(TARGET).exe
endif

SOURCES := $(COMMON_SOURCES) $(OS_COMMON_SOURCE) $(OS_SOCKET_SOURCE) $(OS_SOURCE)
OBJECTS := $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(SOURCES)) $(PROTOBUF_GENERATED_OBJECT)
CLIENT_LIBRARY_OBJECTS := \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(CLIENT_LIBRARY_SOURCES)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_COMMON_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOCKET_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOURCE)) \
	$(PROTOBUF_GENERATED_OBJECT)
FULL_CLIENT_LIBRARY_OBJECTS := \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(FULL_CLIENT_LIBRARY_SOURCES)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_COMMON_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOCKET_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOURCE)) \
	$(PROTOBUF_GENERATED_OBJECT)
RESOURCE_SERVICE_OBJECTS := \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(RESOURCE_SERVICE_SOURCES)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_COMMON_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOCKET_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOURCE)) \
	$(PROTOBUF_GENERATED_OBJECT)
BSD_SOCKETS_OBJECTS := \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(BSD_SOCKETS_SOURCES)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_COMMON_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOCKET_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOURCE)) \
	$(PROTOBUF_GENERATED_OBJECT)
ENDORSEMENT_SERVICE_OBJECTS := \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(ENDORSEMENT_SERVICE_SOURCES)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_COMMON_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOCKET_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOURCE)) \
	$(PROTOBUF_GENERATED_OBJECT)
KEYPAIR_TEST_OBJECTS := \
	$(OBJ_DIR)/common/base_types.o \
	$(OBJ_DIR)/common/node.o \
	$(OBJ_DIR)/common/keypair.o \
	$(OBJ_DIR)/common/message_queue/mq_signing.o \
	$(PROTOBUF_GENERATED_OBJECT) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_COMMON_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOURCE)) \
	$(OBJ_DIR)/test/keypair_test.o
ENDORSEMENT_TEST_OBJECTS := \
	$(OBJ_DIR)/common/base_types.o \
	$(OBJ_DIR)/common/keypair.o \
	$(OBJ_DIR)/common/endorsement/endorsement.o \
	$(PROTOBUF_GENERATED_OBJECT) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_COMMON_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOURCE)) \
	$(OBJ_DIR)/test/endorsement_test.o
BASE_TYPES_TEST_OBJECTS := \
	$(OBJ_DIR)/common/base_types.o \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOURCE)) \
	$(OBJ_DIR)/test/base_types_test.o
MESSAGE_QUEUE_TEST_OBJECTS := \
	$(OBJ_DIR)/common/message_queue/mq.o \
	$(PROTOBUF_GENERATED_OBJECT) \
	$(OBJ_DIR)/test/message_queue_test.o
MQ_ASCII_HANDSHAKE_TEST_OBJECTS := \
	$(OBJ_DIR)/common/base_types.o \
	$(OBJ_DIR)/common/keypair.o \
	$(OBJ_DIR)/common/message_queue/mq.o \
	$(OBJ_DIR)/common/message_queue/mq_ascii_handshake.o \
	$(OBJ_DIR)/common/message_queue/mq_signing.o \
	$(OBJ_DIR)/common/encoding/encoding.o \
	$(OBJ_DIR)/common/encoding/protobuf/protobuf_encoding.o \
	$(OBJ_DIR)/common/encoding/json/json_encoding.o \
	$(OBJ_DIR)/common/transport/transport.o \
	$(OBJ_DIR)/common/transport/transport_memory.o \
	$(PROTOBUF_GENERATED_OBJECT) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_COMMON_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOCKET_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOURCE)) \
	$(OBJ_DIR)/test/mq_ascii_handshake_test.o

MQ_SIGNING_TEST_OBJECTS := \
	$(OBJ_DIR)/common/base_types.o \
	$(OBJ_DIR)/common/node.o \
	$(OBJ_DIR)/common/keypair.o \
	$(OBJ_DIR)/common/message_queue/mq.o \
	$(OBJ_DIR)/common/message_queue/mq_signing.o \
	$(PROTOBUF_GENERATED_OBJECT) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_COMMON_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOURCE)) \
	$(OBJ_DIR)/test/mq_signing_test.o

MESSAGE_HASH_TEST_OBJECTS := \
	$(OBJ_DIR)/common/base_types.o \
	$(OBJ_DIR)/common/node.o \
	$(OBJ_DIR)/common/keypair.o \
	$(OBJ_DIR)/common/message_queue/mq.o \
	$(OBJ_DIR)/common/message_queue/mq_signing.o \
	$(PROTOBUF_GENERATED_OBJECT) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_COMMON_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOURCE)) \
	$(OBJ_DIR)/test/message_hash_test.o

RUNTIME_PROTO_TEST_OBJECTS := \
	$(OBJ_DIR)/test/runtime_proto_test.o

TRANSCODING_TEST_OBJECTS := \
	$(OBJ_DIR)/test/transcoding_test.o


MQ_AUTHN_TEST_OBJECTS := \
	$(OBJ_DIR)/common/base_types.o \
	$(OBJ_DIR)/common/node.o \
	$(OBJ_DIR)/common/keypair.o \
	$(OBJ_DIR)/common/message_queue/mq.o \
	$(OBJ_DIR)/common/message_queue/mq_authn.o \
	$(OBJ_DIR)/common/message_queue/mq_signing.o \
	$(OBJ_DIR)/common/encoding/encoding.o \
	$(OBJ_DIR)/common/encoding/protobuf/protobuf_encoding.o \
	$(OBJ_DIR)/common/encoding/json/json_encoding.o \
	$(OBJ_DIR)/common/transport/transport.o \
	$(OBJ_DIR)/common/transport/transport_memory.o \
	$(PROTOBUF_GENERATED_OBJECT) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_COMMON_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOCKET_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOURCE)) \
	$(OBJ_DIR)/test/mq_authn_test.o

MQ_AUTHZ_TEST_OBJECTS := \
	$(OBJ_DIR)/common/base_types.o \
	$(OBJ_DIR)/common/keypair.o \
	$(OBJ_DIR)/common/message_queue/mq.o \
	$(OBJ_DIR)/common/message_queue/mq_authz.o \
	$(OBJ_DIR)/common/message_queue/mq_signing.o \
	$(OBJ_DIR)/common/endorsement/endorsement.o \
	$(PROTOBUF_GENERATED_OBJECT) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_COMMON_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOURCE)) \
	$(OBJ_DIR)/test/mq_authz_test.o
NODE_TEST_OBJECTS := \
	$(OBJ_DIR)/common/base_types.o \
	$(OBJ_DIR)/common/node.o \
	$(OBJ_DIR)/common/keypair.o \
	$(OBJ_DIR)/common/message_queue/mq.o \
	$(OBJ_DIR)/common/message_queue/mq_signing.o \
	$(PROTOBUF_GENERATED_OBJECT) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_COMMON_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOURCE)) \
	$(OBJ_DIR)/test/node_test.o
CLIENT_TEST_OBJECTS := \
	$(OBJ_DIR)/common/base_types.o \
	$(OBJ_DIR)/common/node.o \
	$(OBJ_DIR)/common/keypair.o \
	$(OBJ_DIR)/common/message_queue/mq.o \
	$(OBJ_DIR)/common/message_queue/mq_ascii_handshake.o \
	$(OBJ_DIR)/common/message_queue/mq_authn.o \
	$(OBJ_DIR)/common/message_queue/mq_authz.o \
	$(OBJ_DIR)/common/message_queue/mq_signing.o \
	$(OBJ_DIR)/common/encoding/encoding.o \
	$(OBJ_DIR)/common/encoding/protobuf/protobuf_encoding.o \
	$(OBJ_DIR)/common/encoding/json/json_encoding.o \
	$(OBJ_DIR)/common/endorsement/endorsement.o \
	$(PROTOBUF_GENERATED_OBJECT) \
	$(OBJ_DIR)/common/transport/transport.o \
	$(OBJ_DIR)/common/transport/transport_tcp.o \
	$(OBJ_DIR)/common/transport/transport_memory.o \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_COMMON_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOCKET_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOURCE)) \
	$(OBJ_DIR)/resource_manager/resource_manager.o \
	$(OBJ_DIR)/client/cpp/rsp_client_message.o \
	$(OBJ_DIR)/client/cpp/rsp_client.o \
	$(OBJ_DIR)/client/cpp_full/rsp_client.o \
	$(OBJ_DIR)/test/client_test.o

RESOURCE_SERVICE_TEST_OBJECTS := \
	$(OBJ_DIR)/common/base_types.o \
	$(OBJ_DIR)/common/node.o \
	$(OBJ_DIR)/common/keypair.o \
	$(OBJ_DIR)/common/message_queue/mq.o \
	$(OBJ_DIR)/common/message_queue/mq_ascii_handshake.o \
	$(OBJ_DIR)/common/message_queue/mq_authn.o \
	$(OBJ_DIR)/common/message_queue/mq_authz.o \
	$(OBJ_DIR)/common/message_queue/mq_signing.o \
	$(OBJ_DIR)/common/encoding/encoding.o \
	$(OBJ_DIR)/common/encoding/protobuf/protobuf_encoding.o \
	$(OBJ_DIR)/common/encoding/json/json_encoding.o \
	$(OBJ_DIR)/common/endorsement/endorsement.o \
	$(PROTOBUF_GENERATED_OBJECT) \
	$(OBJ_DIR)/common/transport/transport.o \
	$(OBJ_DIR)/common/transport/transport_tcp.o \
	$(OBJ_DIR)/common/transport/transport_memory.o \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_COMMON_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOCKET_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOURCE)) \
	$(OBJ_DIR)/resource_manager/resource_manager.o \
	$(OBJ_DIR)/client/cpp/rsp_client_message.o \
	$(OBJ_DIR)/client/cpp/rsp_client.o \
	$(OBJ_DIR)/client/cpp_full/rsp_client.o \
	$(OBJ_DIR)/resource_service/resource_service.o \
	$(OBJ_DIR)/resource_service/bsd_sockets/resource_service_bsd_sockets.o \
	$(OBJ_DIR)/test/resource_service_test.o

RESOURCE_SERVICE_JSON_TEST_OBJECTS := \
	$(OBJ_DIR)/common/base_types.o \
	$(OBJ_DIR)/common/node.o \
	$(OBJ_DIR)/common/keypair.o \
	$(OBJ_DIR)/common/message_queue/mq.o \
	$(OBJ_DIR)/common/message_queue/mq_ascii_handshake.o \
	$(OBJ_DIR)/common/message_queue/mq_authn.o \
	$(OBJ_DIR)/common/message_queue/mq_authz.o \
	$(OBJ_DIR)/common/message_queue/mq_signing.o \
	$(OBJ_DIR)/common/encoding/encoding.o \
	$(OBJ_DIR)/common/encoding/protobuf/protobuf_encoding.o \
	$(OBJ_DIR)/common/encoding/json/json_encoding.o \
	$(OBJ_DIR)/common/endorsement/endorsement.o \
	$(PROTOBUF_GENERATED_OBJECT) \
	$(OBJ_DIR)/common/transport/transport.o \
	$(OBJ_DIR)/common/transport/transport_tcp.o \
	$(OBJ_DIR)/common/transport/transport_memory.o \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_COMMON_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOCKET_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOURCE)) \
	$(OBJ_DIR)/resource_manager/resource_manager.o \
	$(OBJ_DIR)/client/cpp_full/rsp_client.o \
	$(OBJ_DIR)/resource_service/resource_service.o \
	$(OBJ_DIR)/resource_service/bsd_sockets/resource_service_bsd_sockets.o \
	$(OBJ_DIR)/test/resource_service_json_test.o

ENDORSEMENT_SERVICE_TEST_OBJECTS := \
	$(OBJ_DIR)/common/base_types.o \
	$(OBJ_DIR)/common/node.o \
	$(OBJ_DIR)/common/keypair.o \
	$(OBJ_DIR)/common/message_queue/mq.o \
	$(OBJ_DIR)/common/message_queue/mq_ascii_handshake.o \
	$(OBJ_DIR)/common/message_queue/mq_authn.o \
	$(OBJ_DIR)/common/message_queue/mq_authz.o \
	$(OBJ_DIR)/common/message_queue/mq_signing.o \
	$(OBJ_DIR)/common/encoding/encoding.o \
	$(OBJ_DIR)/common/encoding/protobuf/protobuf_encoding.o \
	$(OBJ_DIR)/common/encoding/json/json_encoding.o \
	$(OBJ_DIR)/common/endorsement/endorsement.o \
	$(PROTOBUF_GENERATED_OBJECT) \
	$(OBJ_DIR)/common/transport/transport.o \
	$(OBJ_DIR)/common/transport/transport_tcp.o \
	$(OBJ_DIR)/common/transport/transport_memory.o \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_COMMON_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOCKET_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOURCE)) \
	$(OBJ_DIR)/resource_manager/resource_manager.o \
	$(OBJ_DIR)/client/cpp/rsp_client_message.o \
	$(OBJ_DIR)/client/cpp/rsp_client.o \
	$(OBJ_DIR)/client/cpp_full/rsp_client.o \
	$(OBJ_DIR)/endorsement_service/endorsement_service.o \
	$(OBJ_DIR)/test/endorsement_service_test.o

NODEJS_PING_FIXTURE_OBJECTS := \
	$(OBJ_DIR)/common/base_types.o \
	$(OBJ_DIR)/common/node.o \
	$(OBJ_DIR)/common/keypair.o \
	$(OBJ_DIR)/common/message_queue/mq.o \
	$(OBJ_DIR)/common/message_queue/mq_ascii_handshake.o \
	$(OBJ_DIR)/common/message_queue/mq_authn.o \
	$(OBJ_DIR)/common/message_queue/mq_authz.o \
	$(OBJ_DIR)/common/message_queue/mq_signing.o \
	$(OBJ_DIR)/common/encoding/encoding.o \
	$(OBJ_DIR)/common/encoding/protobuf/protobuf_encoding.o \
	$(OBJ_DIR)/common/encoding/json/json_encoding.o \
	$(OBJ_DIR)/common/endorsement/endorsement.o \
	$(PROTOBUF_GENERATED_OBJECT) \
	$(OBJ_DIR)/common/transport/transport.o \
	$(OBJ_DIR)/common/transport/transport_tcp.o \
	$(OBJ_DIR)/common/transport/transport_memory.o \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_COMMON_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOCKET_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOURCE)) \
	$(OBJ_DIR)/resource_manager/resource_manager.o \
	$(OBJ_DIR)/client/cpp_full/rsp_client.o \
	$(OBJ_DIR)/resource_service/resource_service.o \
	$(OBJ_DIR)/resource_service/bsd_sockets/resource_service_bsd_sockets.o \
	$(OBJ_DIR)/endorsement_service/endorsement_service.o \
	$(OBJ_DIR)/test/nodejs_ping_fixture.o

TRANSPORT_MEMORY_TEST_OBJECTS := \
	$(OBJ_DIR)/common/base_types.o \
	$(OBJ_DIR)/common/node.o \
	$(OBJ_DIR)/common/keypair.o \
	$(OBJ_DIR)/common/message_queue/mq.o \
	$(OBJ_DIR)/common/message_queue/mq_ascii_handshake.o \
	$(OBJ_DIR)/common/message_queue/mq_authn.o \
	$(OBJ_DIR)/common/message_queue/mq_authz.o \
	$(OBJ_DIR)/common/message_queue/mq_signing.o \
	$(OBJ_DIR)/common/encoding/encoding.o \
	$(OBJ_DIR)/common/encoding/protobuf/protobuf_encoding.o \
	$(OBJ_DIR)/common/encoding/json/json_encoding.o \
	$(OBJ_DIR)/common/endorsement/endorsement.o \
	$(PROTOBUF_GENERATED_OBJECT) \
	$(OBJ_DIR)/common/transport/transport.o \
	$(OBJ_DIR)/common/transport/transport_tcp.o \
	$(OBJ_DIR)/common/transport/transport_memory.o \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_COMMON_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOCKET_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOURCE)) \
	$(OBJ_DIR)/resource_manager/resource_manager.o \
	$(OBJ_DIR)/client/cpp/rsp_client_message.o \
	$(OBJ_DIR)/client/cpp/rsp_client.o \
	$(OBJ_DIR)/client/cpp_full/rsp_client.o \
	$(OBJ_DIR)/resource_service/resource_service.o \
	$(OBJ_DIR)/resource_service/bsd_sockets/resource_service_bsd_sockets.o \
	$(OBJ_DIR)/test/transport_memory_test.o

RSP_ENDORSEMENT_TOOL_OBJECTS := \
	$(OBJ_DIR)/common/base_types.o \
	$(OBJ_DIR)/common/keypair.o \
	$(OBJ_DIR)/common/endorsement/endorsement.o \
	$(PROTOBUF_GENERATED_OBJECT) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_COMMON_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOURCE)) \
	$(OBJ_DIR)/tools/rsp_endorsement/rsp_endorsement_main.o

RSP_CLI_TOOL_OBJECTS := \
	$(CLIENT_LIBRARY_OBJECTS) \
	$(OBJ_DIR)/tools/rsp_cli/rsp_cli_main.o

CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -pedantic
CPPFLAGS ?= -I$(PROJECT_ROOT)
THREAD_FLAGS := -pthread
CXXFLAGS += $(THREAD_FLAGS)
LDFLAGS += $(THREAD_FLAGS)
SHARED_CXXFLAGS := $(CXXFLAGS) -fPIC

.PHONY: all clean directories test test-base-types test-client test-endorsement test-keypair test-message-queue test-mq-ascii-handshake test-mq-signing test-mq-authn test-mq-authz test-node test-resource-service test-endorsement-service test-transport-memory test-nodejs-client test-nodejs-client-reconnect test-nodejs-express test-nodejs-express-stress test-python-http-server test-openssh-stress generate-messages rsp-sshd rsp-ssh

$(NODEJS_MESSAGES_JS) $(PYTHON_MESSAGES_PY): messages.proto resource_service/bsd_sockets/bsd_sockets.proto resource_service/sshd/sshd.proto $(GENERATE_MESSAGES_SCRIPT)
	python3 $(GENERATE_MESSAGES_SCRIPT) --proto messages.proto resource_service/bsd_sockets/bsd_sockets.proto resource_service/sshd/sshd.proto --nodejs $(NODEJS_MESSAGES_JS) --python $(PYTHON_MESSAGES_PY)

generate-messages: $(NODEJS_MESSAGES_JS) $(PYTHON_MESSAGES_PY)

all: generate-messages $(TARGET) $(ESP_RM_TARGET) $(ESP_ES_TARGET) $(RSPCLIENT_STATIC_TARGET) $(RSPCLIENT_SHARED_TARGET) $(RSPFULLCLIENT_STATIC_TARGET) $(RESOURCE_SERVICE_TARGET) $(BSD_SOCKETS_TARGET) $(ENDORSEMENT_SERVICE_TARGET) $(RSP_ENDORSEMENT_TOOL_TARGET) $(RSP_CLI_TOOL_TARGET) $(RSP_SSHD_TARGET) $(RSP_SSH_TARGET)

include third_party/Makefile

CPPFLAGS += -I$(BORINGSSL_INCLUDE_DIR) -I$(PROTOBUF_INCLUDE_DIR) -I$(ABSEIL_INCLUDE_DIR) -I$(PROTOBUF_GENERATED_DIR)

$(TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OBJECTS)
	$(CXX) $(CXXFLAGS) $(OBJECTS) $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(ESP_RM_TARGET): $(TARGET)
	@mkdir -p $(TOP_BIN_DIR)
	cp $(TARGET) $(ESP_RM_TARGET)

$(RSPCLIENT_STATIC_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(CLIENT_LIBRARY_OBJECTS)
	@mkdir -p $(LIB_DIR)
	ar rcs $@ $(CLIENT_LIBRARY_OBJECTS)

$(RSPFULLCLIENT_STATIC_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(FULL_CLIENT_LIBRARY_OBJECTS)
	@mkdir -p $(LIB_DIR)
	ar rcs $@ $(FULL_CLIENT_LIBRARY_OBJECTS)

$(RSPCLIENT_SHARED_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(PROTOBUF_GENERATED_SOURCE)
	@mkdir -p $(LIB_DIR)
	$(CXX) $(SHARED_CXXFLAGS) $(CPPFLAGS) -DRSPCLIENT_BUILD_DLL -shared $(CLIENT_LIBRARY_SOURCES) $(PROTOBUF_GENERATED_SOURCE) $(OS_COMMON_SOURCE) $(OS_SOCKET_SOURCE) $(OS_SOURCE) $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(RESOURCE_SERVICE_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(RESOURCE_SERVICE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(RESOURCE_SERVICE_OBJECTS) $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(BSD_SOCKETS_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(BSD_SOCKETS_OBJECTS)
	$(CXX) $(CXXFLAGS) $(BSD_SOCKETS_OBJECTS) $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(ENDORSEMENT_SERVICE_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(ENDORSEMENT_SERVICE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(ENDORSEMENT_SERVICE_OBJECTS) $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(ESP_ES_TARGET): $(ENDORSEMENT_SERVICE_TARGET)
	@mkdir -p $(TOP_BIN_DIR)
	cp $(ENDORSEMENT_SERVICE_TARGET) $(ESP_ES_TARGET)

$(KEYPAIR_TEST_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(KEYPAIR_TEST_OBJECTS)
	$(CXX) $(CXXFLAGS) $(KEYPAIR_TEST_OBJECTS) $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(ENDORSEMENT_TEST_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(ENDORSEMENT_TEST_OBJECTS)
	$(CXX) $(CXXFLAGS) $(ENDORSEMENT_TEST_OBJECTS) $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(BASE_TYPES_TEST_TARGET): directories $(BASE_TYPES_TEST_OBJECTS)
	$(CXX) $(CXXFLAGS) $(BASE_TYPES_TEST_OBJECTS) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(MESSAGE_QUEUE_TEST_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(MESSAGE_QUEUE_TEST_OBJECTS)
	$(CXX) $(CXXFLAGS) $(MESSAGE_QUEUE_TEST_OBJECTS) $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(MQ_ASCII_HANDSHAKE_TEST_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(MQ_ASCII_HANDSHAKE_TEST_OBJECTS)
	$(CXX) $(CXXFLAGS) $(MQ_ASCII_HANDSHAKE_TEST_OBJECTS) $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(MQ_SIGNING_TEST_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(MQ_SIGNING_TEST_OBJECTS)
	$(CXX) $(CXXFLAGS) $(MQ_SIGNING_TEST_OBJECTS) $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(MESSAGE_HASH_TEST_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(MESSAGE_HASH_TEST_OBJECTS)
	$(CXX) $(CXXFLAGS) $(MESSAGE_HASH_TEST_OBJECTS) $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(RUNTIME_PROTO_TEST_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(RUNTIME_PROTO_TEST_OBJECTS)
	$(CXX) $(CXXFLAGS) $(RUNTIME_PROTO_TEST_OBJECTS) $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(TRANSCODING_TEST_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(TRANSCODING_TEST_OBJECTS)
	$(CXX) $(CXXFLAGS) $(TRANSCODING_TEST_OBJECTS) $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(MQ_AUTHN_TEST_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(MQ_AUTHN_TEST_OBJECTS)
	$(CXX) $(CXXFLAGS) $(MQ_AUTHN_TEST_OBJECTS) $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(MQ_AUTHZ_TEST_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(MQ_AUTHZ_TEST_OBJECTS)
	$(CXX) $(CXXFLAGS) $(MQ_AUTHZ_TEST_OBJECTS) $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(NODE_TEST_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(NODE_TEST_OBJECTS)
	$(CXX) $(CXXFLAGS) $(NODE_TEST_OBJECTS) $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(CLIENT_TEST_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(CLIENT_TEST_OBJECTS)
	$(CXX) $(CXXFLAGS) $(CLIENT_TEST_OBJECTS) $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(RESOURCE_SERVICE_TEST_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(RESOURCE_SERVICE_TEST_OBJECTS)
	$(CXX) $(CXXFLAGS) $(RESOURCE_SERVICE_TEST_OBJECTS) $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(RESOURCE_SERVICE_JSON_TEST_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(RESOURCE_SERVICE_JSON_TEST_OBJECTS)
	$(CXX) $(CXXFLAGS) $(RESOURCE_SERVICE_JSON_TEST_OBJECTS) $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(ENDORSEMENT_SERVICE_TEST_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(ENDORSEMENT_SERVICE_TEST_OBJECTS)
	$(CXX) $(CXXFLAGS) $(ENDORSEMENT_SERVICE_TEST_OBJECTS) $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(NODEJS_PING_FIXTURE_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(NODEJS_PING_FIXTURE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(NODEJS_PING_FIXTURE_OBJECTS) $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(TRANSPORT_MEMORY_TEST_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(TRANSPORT_MEMORY_TEST_OBJECTS)
	$(CXX) $(CXXFLAGS) $(TRANSPORT_MEMORY_TEST_OBJECTS) $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(RSP_ENDORSEMENT_TOOL_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(RSP_ENDORSEMENT_TOOL_OBJECTS)
	$(CXX) $(CXXFLAGS) $(RSP_ENDORSEMENT_TOOL_OBJECTS) $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(RSP_CLI_TOOL_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(RSP_CLI_TOOL_OBJECTS)
	$(CXX) $(CXXFLAGS) $(RSP_CLI_TOOL_OBJECTS) $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

RSP_SSHD_OBJECTS := \
	$(FULL_CLIENT_LIBRARY_OBJECTS) \
	$(OBJ_DIR)/resource_service/resource_service.o \
	$(OBJ_DIR)/resource_service/bsd_sockets/resource_service_bsd_sockets.o \
	$(OBJ_DIR)/resource_service/sshd/resource_service_sshd.o

$(RSP_SSHD_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(RSP_SSHD_OBJECTS)
	$(CXX) $(CXXFLAGS) $(RSP_SSHD_OBJECTS) $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

rsp-sshd: $(RSP_SSHD_TARGET)

RSP_SSH_OBJECTS := \
	$(CLIENT_LIBRARY_OBJECTS) \
	$(OBJ_DIR)/integration/openssh/modification/rsp_ssh.o

$(RSP_SSH_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(RSP_SSH_OBJECTS)
	$(CXX) $(CXXFLAGS) $(RSP_SSH_OBJECTS) $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

rsp-ssh: $(RSP_SSH_TARGET)

test-base-types: $(BASE_TYPES_TEST_TARGET)
	$(BASE_TYPES_TEST_TARGET)

test-message-queue: $(MESSAGE_QUEUE_TEST_TARGET)
	$(MESSAGE_QUEUE_TEST_TARGET)

test-mq-ascii-handshake: $(MQ_ASCII_HANDSHAKE_TEST_TARGET)
	$(MQ_ASCII_HANDSHAKE_TEST_TARGET)

test-mq-signing: $(MQ_SIGNING_TEST_TARGET)
	$(MQ_SIGNING_TEST_TARGET)

test-message-hash: $(MESSAGE_HASH_TEST_TARGET)
	$(MESSAGE_HASH_TEST_TARGET)

test-runtime-proto: $(RUNTIME_PROTO_TEST_TARGET)
	$(RUNTIME_PROTO_TEST_TARGET)

test-transcoding: $(TRANSCODING_TEST_TARGET)
	$(TRANSCODING_TEST_TARGET)

test-mq-authn: $(MQ_AUTHN_TEST_TARGET)
	$(MQ_AUTHN_TEST_TARGET)

test-mq-authz: $(MQ_AUTHZ_TEST_TARGET)
	$(MQ_AUTHZ_TEST_TARGET)

test-node: $(NODE_TEST_TARGET)
	$(NODE_TEST_TARGET)

test-client: $(CLIENT_TEST_TARGET)
	$(CLIENT_TEST_TARGET)

test-resource-service: $(RESOURCE_SERVICE_TEST_TARGET)
	$(RESOURCE_SERVICE_TEST_TARGET)

test-resource-service-json: $(RESOURCE_SERVICE_JSON_TEST_TARGET)
	$(RESOURCE_SERVICE_JSON_TEST_TARGET)

test-endorsement-service: $(ENDORSEMENT_SERVICE_TEST_TARGET)
	$(ENDORSEMENT_SERVICE_TEST_TARGET)

test-keypair: $(KEYPAIR_TEST_TARGET)
	$(KEYPAIR_TEST_TARGET)

test-endorsement: $(ENDORSEMENT_TEST_TARGET)
	$(ENDORSEMENT_TEST_TARGET)

test-transport-memory: $(TRANSPORT_MEMORY_TEST_TARGET)
	$(TRANSPORT_MEMORY_TEST_TARGET)

test-nodejs-client: $(NODEJS_PING_FIXTURE_TARGET) $(NODEJS_MESSAGES_JS)
	node test/nodejs_ping_integration.js $(NODEJS_PING_FIXTURE_TARGET)

test-nodejs-client-reconnect: $(NODEJS_PING_FIXTURE_TARGET) $(NODEJS_MESSAGES_JS)
	node test/nodejs_client_reconnect_integration.js $(NODEJS_PING_FIXTURE_TARGET)

test-nodejs-express: $(NODEJS_PING_FIXTURE_TARGET) $(NODEJS_MESSAGES_JS)
	node test/nodejs_express_integration.js $(NODEJS_PING_FIXTURE_TARGET)

test-nodejs-express-stress: $(NODEJS_PING_FIXTURE_TARGET) $(NODEJS_MESSAGES_JS)
	node test/nodejs_express_stress_integration.js $(NODEJS_PING_FIXTURE_TARGET)

test-openssh-stress: $(RSP_SSHD_TARGET) $(RSP_SSH_TARGET) $(TARGET) $(ENDORSEMENT_SERVICE_TARGET)
	bash test/openssh_stress_integration.sh $(BIN_DIR)

test-python-http-server: $(NODEJS_PING_FIXTURE_TARGET) $(NODEJS_MESSAGES_JS) $(PYTHON_MESSAGES_PY)
	node test/python_http_server_integration.js $(NODEJS_PING_FIXTURE_TARGET)

test: test-base-types test-keypair test-endorsement test-message-hash test-message-queue test-mq-ascii-handshake test-mq-signing test-node test-client test-resource-service test-endorsement-service test-transport-memory

$(PROTOBUF_GENERATED_SOURCE): messages.proto resource_service/bsd_sockets/bsd_sockets.proto resource_service/sshd/sshd.proto $(PROTOBUF_PROTOC)
	@mkdir -p $(PROTOBUF_GENERATED_DIR)
	$(PROTOBUF_PROTOC) --proto_path=$(PROJECT_ROOT) --proto_path=$(PROTOBUF_INCLUDE_DIR) --cpp_out=$(PROTOBUF_GENERATED_DIR) messages.proto resource_service/bsd_sockets/bsd_sockets.proto resource_service/sshd/sshd.proto
	@mkdir -p $(dir $(BSD_SOCKETS_DESC))
	$(PROTOBUF_PROTOC) --proto_path=$(PROJECT_ROOT) --proto_path=$(PROTOBUF_INCLUDE_DIR) --descriptor_set_out=$(BSD_SOCKETS_DESC) --include_imports resource_service/bsd_sockets/bsd_sockets.proto
	$(PROTOBUF_PROTOC) --proto_path=$(PROJECT_ROOT) --proto_path=$(PROTOBUF_INCLUDE_DIR) --descriptor_set_out=$(SSHD_DESC) --include_imports resource_service/sshd/sshd.proto
	python3 $(EMBED_DESCRIPTOR_SCRIPT) $(BSD_SOCKETS_DESC) $(BSD_SOCKETS_DESC_HEADER) --name kBsdSocketsDescriptor
	python3 $(EMBED_DESCRIPTOR_SCRIPT) $(SSHD_DESC) $(SSHD_DESC_HEADER) --name kSshdDescriptor

$(PROTOBUF_GENERATED_HEADER): $(PROTOBUF_GENERATED_SOURCE)

$(BSD_SOCKETS_GENERATED_SOURCE): $(PROTOBUF_GENERATED_SOURCE)

$(BSD_SOCKETS_GENERATED_HEADER): $(BSD_SOCKETS_GENERATED_SOURCE)

$(BSD_SOCKETS_DESC): $(PROTOBUF_GENERATED_SOURCE)

$(BSD_SOCKETS_DESC_HEADER): $(BSD_SOCKETS_DESC)

$(SSHD_GENERATED_SOURCE): $(PROTOBUF_GENERATED_SOURCE)

$(SSHD_GENERATED_HEADER): $(SSHD_GENERATED_SOURCE)

$(SSHD_DESC): $(PROTOBUF_GENERATED_SOURCE)

$(SSHD_DESC_HEADER): $(SSHD_DESC)

$(OBJ_DIR)/build/gen/messages.pb.o: $(PROTOBUF_GENERATED_SOURCE) $(PROTOBUF_GENERATED_HEADER)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $(PROTOBUF_GENERATED_SOURCE) -o $(OBJ_DIR)/build/gen/messages.pb.o

$(BSD_SOCKETS_GENERATED_OBJECT): $(BSD_SOCKETS_GENERATED_SOURCE) $(BSD_SOCKETS_GENERATED_HEADER)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $(BSD_SOCKETS_GENERATED_SOURCE) -o $@

$(SSHD_GENERATED_OBJECT): $(SSHD_GENERATED_SOURCE) $(SSHD_GENERATED_HEADER)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $(SSHD_GENERATED_SOURCE) -o $@

$(OBJ_DIR)/common/keypair.o: $(BORINGSSL_INCLUDE_HEADER) $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/common/node.o: $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/common/message_queue/mq.o: $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/common/message_queue/mq_ascii_handshake.o: $(BORINGSSL_INCLUDE_HEADER) $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/common/message_queue/mq_signing.o: $(BORINGSSL_INCLUDE_HEADER) $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/common/message_queue/mq_authn.o: $(BORINGSSL_INCLUDE_HEADER) $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/common/message_queue/mq_authz.o: $(BORINGSSL_INCLUDE_HEADER) $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/test/keypair_test.o: $(BORINGSSL_INCLUDE_HEADER) $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/common/endorsement/endorsement.o: $(BORINGSSL_INCLUDE_HEADER) $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/test/endorsement_test.o: $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/common/encoding/encoding.o: common/message_queue/mq.hpp $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/common/encoding/protobuf/protobuf_encoding.o: common/message_queue/mq.hpp $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/common/encoding/json/json_encoding.o: common/message_queue/mq.hpp $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/resource_manager/resource_manager.o: common/message_queue/mq.hpp $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/resource_service/resource_service.o: $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/resource_service/bsd_sockets/resource_service_bsd_sockets.o: $(BSD_SOCKETS_GENERATED_HEADER) $(BSD_SOCKETS_DESC_HEADER) $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/resource_service/sshd/resource_service_sshd.o: $(SSHD_GENERATED_HEADER) $(SSHD_DESC_HEADER) $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/client/cpp_full/rsp_client.o: common/message_queue/mq.hpp $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/client/cpp/rsp_client_message.o: common/message_queue/mq.hpp $(BORINGSSL_INCLUDE_HEADER) $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/client/cpp/rsp_client.o: common/message_queue/mq.hpp $(BORINGSSL_INCLUDE_HEADER) $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/test/client_test.o: common/message_queue/mq.hpp $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/test/resource_service_test.o: common/message_queue/mq.hpp $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/test/resource_service_json_test.o: common/message_queue/mq.hpp $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/test/message_queue_test.o: $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/test/message_hash_test.o: $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/test/mq_ascii_handshake_test.o: $(BORINGSSL_INCLUDE_HEADER) $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/test/mq_signing_test.o: $(BORINGSSL_INCLUDE_HEADER) $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/test/mq_authn_test.o: $(BORINGSSL_INCLUDE_HEADER) $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/test/mq_authz_test.o: $(BORINGSSL_INCLUDE_HEADER) $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/tools/rsp_endorsement/rsp_endorsement_main.o: $(BORINGSSL_INCLUDE_HEADER) $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/tools/rsp_cli/rsp_cli_main.o: $(BORINGSSL_INCLUDE_HEADER) $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/test/runtime_proto_test.o: $(PROTOBUF_INCLUDE_HEADER)

$(OBJ_DIR)/test/transcoding_test.o: $(PROTOBUF_INCLUDE_HEADER)

$(OBJ_DIR)/test/node_test.o: $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/test/endorsement_service_test.o: common/message_queue/mq.hpp $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/test/nodejs_ping_fixture.o: common/message_queue/mq.hpp $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/test/transport_memory_test.o: common/message_queue/mq.hpp $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/endorsement_service/endorsement_service.o: common/message_queue/mq.hpp $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

directories:
	@mkdir -p $(BIN_DIR)
	@mkdir -p $(LIB_DIR)
	@mkdir -p $(TOP_BIN_DIR)
	@mkdir -p $(PROTOBUF_GENERATED_DIR)

clean:
	rm -rf $(BUILD_DIR)