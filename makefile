SHELL := /bin/bash

############################################
# Project layout
############################################

PROJECT_ROOT := $(abspath .)
ENV_FILE := $(PROJECT_ROOT)/scripts/.env
RUN_SCRIPT := $(PROJECT_ROOT)/scripts/run_udp_test.sh

RECEIVER_TARGET := kernel-bypass
RECEIVER_SRC := kernel-bypass.cpp

SENDER_TARGET := send_udp
SENDER_SRC := send_udp.cpp

############################################
# Toolchain
############################################

CXX := g++
CXXFLAGS := -O3 -g -std=c++17 -Wall -Wextra -pedantic
LDFLAGS :=

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    CXXFLAGS += -march=native
endif

############################################
# Load .env as single source of truth
############################################

ifeq ($(wildcard $(ENV_FILE)),)
    $(error Missing .env file: $(ENV_FILE))
endif

include $(ENV_FILE)

export UDP_PORT
export UDP_DURATION_SECONDS
export UDP_BUFFER_SIZE
export UDP_PAYLOAD_SIZE
export UDP_PPS

############################################
# Build
############################################

all: $(RECEIVER_TARGET) $(SENDER_TARGET)

$(RECEIVER_TARGET): $(RECEIVER_SRC)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

$(SENDER_TARGET): $(SENDER_SRC)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

############################################
# Run
############################################

run: all
	@echo "Running UDP benchmark using $(ENV_FILE)"
	@/bin/bash $(RUN_SCRIPT)

############################################
# Helpers
############################################

show-config:
	@echo "ENV_FILE              = $(ENV_FILE)"
	@echo "UDP_PORT              = $(UDP_PORT)"
	@echo "UDP_DURATION_SECONDS  = $(UDP_DURATION_SECONDS)"
	@echo "UDP_BUFFER_SIZE       = $(UDP_BUFFER_SIZE)"
	@echo "UDP_PAYLOAD_SIZE      = $(UDP_PAYLOAD_SIZE)"
	@echo "UDP_PPS               = $(UDP_PPS)"

clean:
	rm -f $(RECEIVER_TARGET) $(SENDER_TARGET)

rebuild: clean all

.PHONY: all run show-config clean rebuild