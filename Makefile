PROJECT_ROOT := .
BUILD_DIR := build
BIN_DIR := $(BUILD_DIR)/bin
OBJ_DIR := $(BUILD_DIR)/obj

include common/Makefile
include os/Makefile
include client/cpp/Makefile

TARGET := $(BIN_DIR)/resource_manager
KEYPAIR_TEST_TARGET := $(BIN_DIR)/keypair_test
BASE_TYPES_TEST_TARGET := $(BIN_DIR)/base_types_test
CLIENT_TEST_TARGET := $(BIN_DIR)/client_test
LIB_DIR := $(BUILD_DIR)/lib
RSPCLIENT_STATIC_TARGET := $(LIB_DIR)/librspclient.a
RSPCLIENT_SHARED_TARGET := $(LIB_DIR)/librspclient.so

COMMON_SOURCES := \
	$(COMMON_BASE_TYPES_SOURCE) \
	$(COMMON_NODE_SOURCE) \
	$(COMMON_KEYPAIR_SOURCE) \
	$(COMMON_TRANSPORT_SOURCE) \
	$(COMMON_TRANSPORT_TCP_SOURCE) \
	resource_manager/resource_manager.cpp \
	resource_manager/resource_manager_main.cpp

CLIENT_LIBRARY_SOURCES := \
	$(COMMON_BASE_TYPES_SOURCE) \
	$(COMMON_NODE_SOURCE) \
	$(COMMON_KEYPAIR_SOURCE) \
	$(COMMON_TRANSPORT_SOURCE) \
	$(COMMON_TRANSPORT_TCP_SOURCE) \
	$(CLIENT_CPP_RSP_CLIENT_SOURCE)

ifeq ($(OS),Windows_NT)
	TARGET := $(TARGET).exe
endif

SOURCES := $(COMMON_SOURCES) $(OS_COMMON_SOURCE) $(OS_SOCKET_SOURCE) $(OS_SOURCE)
OBJECTS := $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(SOURCES))
CLIENT_LIBRARY_OBJECTS := \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(CLIENT_LIBRARY_SOURCES)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_COMMON_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOCKET_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOURCE))
KEYPAIR_TEST_OBJECTS := \
	$(OBJ_DIR)/common/base_types.o \
	$(OBJ_DIR)/common/node.o \
	$(OBJ_DIR)/common/keypair.o \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_COMMON_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOURCE)) \
	$(OBJ_DIR)/test/keypair_test.o
BASE_TYPES_TEST_OBJECTS := \
	$(OBJ_DIR)/common/base_types.o \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOURCE)) \
	$(OBJ_DIR)/test/base_types_test.o
CLIENT_TEST_OBJECTS := \
	$(OBJ_DIR)/common/base_types.o \
	$(OBJ_DIR)/common/node.o \
	$(OBJ_DIR)/common/keypair.o \
	$(OBJ_DIR)/common/transport/transport.o \
	$(OBJ_DIR)/common/transport/transport_tcp.o \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_COMMON_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOCKET_SOURCE)) \
	$(patsubst %.cpp,$(OBJ_DIR)/%.o,$(OS_SOURCE)) \
	$(OBJ_DIR)/client/cpp/rsp_client.o \
	$(OBJ_DIR)/test/client_test.o

CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -pedantic
CPPFLAGS ?= -I$(PROJECT_ROOT)
THREAD_FLAGS := -pthread
CXXFLAGS += $(THREAD_FLAGS)
LDFLAGS += $(THREAD_FLAGS)
SHARED_CXXFLAGS := $(CXXFLAGS) -fPIC

.PHONY: all clean directories test test-base-types test-client test-keypair

all: $(TARGET) $(RSPCLIENT_STATIC_TARGET) $(RSPCLIENT_SHARED_TARGET)

include third_party/Makefile

CPPFLAGS += -I$(BORINGSSL_INCLUDE_DIR)

$(TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(OBJECTS)
	$(CXX) $(CXXFLAGS) $(OBJECTS) $(BORINGSSL_CRYPTO_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(RSPCLIENT_STATIC_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(CLIENT_LIBRARY_OBJECTS)
	@mkdir -p $(LIB_DIR)
	ar rcs $@ $(CLIENT_LIBRARY_OBJECTS)

$(RSPCLIENT_SHARED_TARGET): directories $(BORINGSSL_CRYPTO_LIB)
	@mkdir -p $(LIB_DIR)
	$(CXX) $(SHARED_CXXFLAGS) $(CPPFLAGS) -DRSPCLIENT_BUILD_DLL -shared $(CLIENT_LIBRARY_SOURCES) $(OS_COMMON_SOURCE) $(OS_SOCKET_SOURCE) $(OS_SOURCE) $(BORINGSSL_CRYPTO_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(KEYPAIR_TEST_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(KEYPAIR_TEST_OBJECTS)
	$(CXX) $(CXXFLAGS) $(KEYPAIR_TEST_OBJECTS) $(BORINGSSL_CRYPTO_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(BASE_TYPES_TEST_TARGET): directories $(BASE_TYPES_TEST_OBJECTS)
	$(CXX) $(CXXFLAGS) $(BASE_TYPES_TEST_OBJECTS) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

$(CLIENT_TEST_TARGET): directories $(BORINGSSL_CRYPTO_LIB) $(CLIENT_TEST_OBJECTS)
	$(CXX) $(CXXFLAGS) $(CLIENT_TEST_OBJECTS) $(BORINGSSL_CRYPTO_LIB) $(OS_SYSTEM_LIBS) $(LDFLAGS) -o $@

test-base-types: $(BASE_TYPES_TEST_TARGET)
	$(BASE_TYPES_TEST_TARGET)

test-client: $(CLIENT_TEST_TARGET)
	$(CLIENT_TEST_TARGET)

test-keypair: $(KEYPAIR_TEST_TARGET)
	$(KEYPAIR_TEST_TARGET)

test: test-base-types test-keypair test-client

$(OBJ_DIR)/common/keypair.o: $(BORINGSSL_INCLUDE_HEADER)

$(OBJ_DIR)/test/keypair_test.o: $(BORINGSSL_INCLUDE_HEADER)

$(OBJ_DIR)/client/cpp/rsp_client.o: $(BORINGSSL_INCLUDE_HEADER)

$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

directories:
	@mkdir -p $(BIN_DIR)
	@mkdir -p $(LIB_DIR)

clean:
	rm -rf $(BUILD_DIR)