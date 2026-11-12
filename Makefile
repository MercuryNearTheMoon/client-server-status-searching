CC = cc
BUILD_DIR ?= build
ENABLE_DEBUG ?= 0

CPPFLAGS += -Iutil -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -DENABLE_DEBUG=$(ENABLE_DEBUG)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -pthread
LDLIBS += -pthread

UTIL_SOURCES := util/protocol.c util/io.c util/logger.c
UTIL_OBJECTS := $(UTIL_SOURCES:%.c=$(BUILD_DIR)/obj/%.o)
CLIENT_OBJECT := $(BUILD_DIR)/obj/client/main.o
SERVER_OBJECT := $(BUILD_DIR)/obj/server/main.o
OBJECTS := $(UTIL_OBJECTS) $(CLIENT_OBJECT) $(SERVER_OBJECT)
LIBRARY := $(BUILD_DIR)/lib/libcommon.so
CLIENT := $(BUILD_DIR)/client
SERVER := $(BUILD_DIR)/server

.PHONY: all objects clean test server
all: $(CLIENT) $(SERVER)

server: $(SERVER)

test:
	CC="$(CC)" sh scripts/ci-local.sh "$(if $(UNIT),$(UNIT),all)"

# Useful while source files are still skeletons without main().
objects: $(OBJECTS)

$(BUILD_DIR)/obj/util/%.o: util/%.c
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -MMD -MP -c $< -o $@

$(BUILD_DIR)/obj/%.o: %.c
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(LIBRARY): $(UTIL_OBJECTS)
	mkdir -p $(@D)
	$(CC) $(LDFLAGS) -shared -Wl,-soname,libcommon.so -o $@ $^ $(LDLIBS)

$(CLIENT): $(CLIENT_OBJECT) $(LIBRARY)
	$(CC) $(LDFLAGS) -o $@ $(CLIENT_OBJECT) -L$(BUILD_DIR)/lib -Wl,-rpath,'$$ORIGIN/lib' -lcommon $(LDLIBS)

$(SERVER): $(SERVER_OBJECT) $(LIBRARY)
	$(CC) $(LDFLAGS) -o $@ $(SERVER_OBJECT) -L$(BUILD_DIR)/lib -Wl,-rpath,'$$ORIGIN/lib' -lcommon $(LDLIBS)

clean:
	$(RM) $(OBJECTS) $(OBJECTS:.o=.d) $(LIBRARY) $(CLIENT) $(SERVER)

-include $(OBJECTS:.o=.d)
