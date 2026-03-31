RUN_ARGS := 9000 10 2048


CXX := g++
CXXFLAGS := -O3 -g -std=c++17 -Wall -Wextra -pedantic
LDFLAGS :=

TARGET := kernel-bypass
SRC := kernel-bypass.cpp


UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    CXXFLAGS += -march=native
endif

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

run: $(TARGET)
	./$(TARGET) $(RUN_ARGS)

clean:
	rm -f $(TARGET)

rebuild: clean all

.PHONY: all run clean rebuild