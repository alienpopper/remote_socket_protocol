PROJECT_ROOT := .
BUILD_DIR := build
BIN_DIR := $(BUILD_DIR)/bin
OBJ_DIR := $(BUILD_DIR)/obj
PROTOBUF_GENERATED_DIR := $(BUILD_DIR)/gen

include common/Makefile
include os/Makefile
include client/cpp/Makefile

TARGET := $(BIN_DIR)/resource_manager
KEYPAIR_TEST_TARGET := $(BIN_DIR)/keypair_test
BASE_TYPES_TEST_TARGET := $(BIN_DIR)/base_types_test
CLIENT_TEST_TARGET := $(BIN_DIR)/client_test
ENDORSEMENT_TEST_TARGET := $(BIN_DIR)/endorsement_test
MESSAGE_QUEUE_TEST_TARGET := $(BIN_DIR)/message_queue_test
LIB_DIR := $(BUILD_DIR)/lib
RSPCLIENT_STATIC_TARGET := $(LIB_DIR)/librspclient.a
RSPCLIENT_SHARED_TARGET := $(LIB_DIR)/librspclient.so
PROTOBUF_GENERATED_SOURCE := $(PROTOBUF_GENERATED_DIR)/messages.pb.cc
PROTOBUF_GENERATED_HEADER := $(PROTOBUF_GENERATED_DIR)/messages.pb.h
PROTOBUF_GENERATED_OBJECT := $(OBJ_DIR)/build/gen/messages.pb.o

COMMON_SOURCES := \
	$(COMMON_BASE_TYPES_SOURCE) \
	$(COMMON_NODE_SOURCE) \
	$(COMMON_KEYPAIR_SOURCE) \
	$(COMMON_MESSAGE_QUEUE_SOURCE) \
	$(COMMON_ASCII_HANDSHAKE_SOURCE) \
	$(COMMON_ENCODING_SOURCE) \
	$(COMMON_PROTOBUF_ENCODING_SOURCE) \
	$(COMMON_ENDORSEMENT_SOURCE) \
	$(COMMON_TRANSPORT_SOURCE) \
	$(COMMON_TRANSPORT_TCP_SOURCE) \
	resource_manager/resource_manager.cpp \
	resource_manager/resource_manager_main.cpp

CLIENT_LIBRARY_SOURCES := \
	$(COMMON_BASE_TYPES_SOURCE) \
	$(COMMON_NODE_SOURCE) \
	$(COMMON_KEYPAIR_SOURCE) \
	$(COMMON_MESSAGE_QUEUE_SOURCE) \
	$(COMMON_ASCII_HANDSHAKE_SOURCE) \
	$(COMMON_ENCODING_SOURCE) \
	$(COMMON_PROTOBUF_ENCODING_SOURCE) \
	$(COMMON_ENDORSEMENT_SOURCE) \
	$(COMMON_TRANSPORT_SOURCE) \
	$(COMMON_TRANSPORT_TCP_SOURCE) \
	$(CLIENT_CPP_RSP_CLIENT_SOURCE)

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
KEYPAIR_TEST_OBJECTS := \
	$(OBJ_DIR)/common/base_types.o \
	$(OBJ_DIR)/common/node.o \
	$(OBJ_DIR)/common/keypair.o \
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
	$(OBJ_DIR)/common/message_queue.o \
	$(OBJ_DIR)/build/gen/messages.pb.o \
	$(OBJ_DIR)/test/message_queue_test.o
CLIENT_TEST_OBJECTS := \
	$(OBJ_DIR)/common/base_types.o \
	$(OBJ_DIR)/common/node.o \
	$(OBJ_DIR)/common/keypair.o \
	$(OBJ_DIR)/common/message_queue.o \
	$(OBJ_DIR)/common/ascii_handshake.o \
	$(OBJ_DIR)/common/encoding/encoding.o \
	$(OBJ_DIR)/common/encoding/protobuf/protobuf_encoding.o \
	$(PROTOBUF_GENERATED_OBJECT) \
	$(OBJ_DIR)/common/transport/transport.o \
	$(OBJ_DIR)/common/transport/transport_tcp.o \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_COMMON_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOCKET_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOURCE)) \
	$(OBJ_DIR)/resource_manager/resource_manager.o \
	$(OBJ_DIR)/client/cpp/rsp_client.o \
	$(OBJ_DIR)/test/client_test.o

CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -pedantic
CPPFLAGS ?= -I$(PROJECT_ROOT)
THREAD_FLAGS := -pthread
CXXFLAGS += $(THREAD_FLAGS)
LDFLAGS += $(THREAD_FLAGS)
SHARED_CXXFLAGS := $(CXXFLAGS) -fPIC

.PHONY: all clean directories test test-base-types test-client test-endorsement test-keypair test-message-queue

all: $(TARGET) $(RSPCLIENT_STATIC_TARGET) $(RSPCLIENT_SHARED_TARGET)

include third_party/Makefile

CPPFLAGS += -I$(BORINGSSL_INCLUDE_DIR) -I$(PROTOBUF_INCLUDE_DIR) -I$(PROTOBUF_GENERATED_DIR)

$(TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OBJECTS)
	$(CXX) $(CXXFLAGS) $(OBJECTS) $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(RSPCLIENT_STATIC_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(CLIENT_LIBRARY_OBJECTS)
	@mkdir -p $(LIB_DIR)
	ar rcs $@ $(CLIENT_LIBRARY_OBJECTS)

$(RSPCLIENT_SHARED_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(PROTOBUF_GENERATED_SOURCE)
	@mkdir -p $(LIB_DIR)
	$(CXX) $(SHARED_CXXFLAGS) $(CPPFLAGS) -DRSPCLIENT_BUILD_DLL -shared $(CLIENT_LIBRARY_SOURCES) $(PROTOBUF_GENERATED_SOURCE) $(OS_COMMON_SOURCE) $(OS_SOCKET_SOURCE) $(OS_SOURCE) $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(KEYPAIR_TEST_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(KEYPAIR_TEST_OBJECTS)
	$(CXX) $(CXXFLAGS) $(KEYPAIR_TEST_OBJECTS) $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(ENDORSEMENT_TEST_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(ENDORSEMENT_TEST_OBJECTS)
	$(CXX) $(CXXFLAGS) $(ENDORSEMENT_TEST_OBJECTS) $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(BASE_TYPES_TEST_TARGET): directories $(BASE_TYPES_TEST_OBJECTS)
	$(CXX) $(CXXFLAGS) $(BASE_TYPES_TEST_OBJECTS) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(MESSAGE_QUEUE_TEST_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(MESSAGE_QUEUE_TEST_OBJECTS)
	$(CXX) $(CXXFLAGS) $(MESSAGE_QUEUE_TEST_OBJECTS) $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(CLIENT_TEST_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(CLIENT_TEST_OBJECTS)
	$(CXX) $(CXXFLAGS) $(CLIENT_TEST_OBJECTS) $(BORINGSSL_CRYPTO_LIB) $(PROTOBUF_LITE_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

test-base-types: $(BASE_TYPES_TEST_TARGET)
	$(BASE_TYPES_TEST_TARGET)

test-message-queue: $(MESSAGE_QUEUE_TEST_TARGET)
	$(MESSAGE_QUEUE_TEST_TARGET)

test-client: $(CLIENT_TEST_TARGET)
	$(CLIENT_TEST_TARGET)

test-keypair: $(KEYPAIR_TEST_TARGET)
	$(KEYPAIR_TEST_TARGET)

test-endorsement: $(ENDORSEMENT_TEST_TARGET)
	$(ENDORSEMENT_TEST_TARGET)

test: test-base-types test-keypair test-endorsement test-message-queue test-client

$(PROTOBUF_GENERATED_SOURCE): messages.proto $(PROTOBUF_PROTOC)
	@mkdir -p $(PROTOBUF_GENERATED_DIR)
	$(PROTOBUF_PROTOC) --proto_path=$(PROJECT_ROOT) --cpp_out=$(PROTOBUF_GENERATED_DIR) $<

$(PROTOBUF_GENERATED_HEADER): $(PROTOBUF_GENERATED_SOURCE)

$(PROTOBUF_GENERATED_OBJECT): $(PROTOBUF_GENERATED_SOURCE) $(PROTOBUF_GENERATED_HEADER)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $(PROTOBUF_GENERATED_SOURCE) -o $@

$(OBJ_DIR)/common/keypair.o: $(BORINGSSL_INCLUDE_HEADER) $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/common/node.o: $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/common/ascii_handshake.o: $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/common/message_queue.o: $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/test/keypair_test.o: $(BORINGSSL_INCLUDE_HEADER) $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/common/endorsement/endorsement.o: $(BORINGSSL_INCLUDE_HEADER) $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/test/endorsement_test.o: $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/common/encoding/encoding.o: $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/common/encoding/protobuf/protobuf_encoding.o: $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/resource_manager/resource_manager.o: $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/client/cpp/rsp_client.o: $(BORINGSSL_INCLUDE_HEADER) $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/test/client_test.o: $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/test/message_queue_test.o: $(PROTOBUF_GENERATED_HEADER)

$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

directories:
	@mkdir -p $(BIN_DIR)
	@mkdir -p $(LIB_DIR)
	@mkdir -p $(PROTOBUF_GENERATED_DIR)

clean:
	rm -rf $(BUILD_DIR)