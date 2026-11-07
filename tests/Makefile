HGS_CPP := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../src/main/cpp)
BUILD_DIR ?= /tmp/hgs-service-unit
CXX ?= g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Werror -pthread -I$(HGS_CPP) -I$(HGS_CPP)/protocols
CORE := $(HGS_CPP)/core/window_registry.cpp $(HGS_CPP)/core/event_bus.cpp $(HGS_CPP)/core/input_router.cpp $(HGS_CPP)/runtime/service_runtime.cpp $(HGS_CPP)/api/hgs_endpoint.cpp
.PHONY: test
test:
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(HGS_CPP)/../../../tests/unit/service_runtime_test.cpp $(CORE) -o $(BUILD_DIR)/service_runtime_test
	$(BUILD_DIR)/service_runtime_test
	$(CXX) $(CXXFLAGS) $(HGS_CPP)/../../../tests/unit/socket_listener_test.cpp $(HGS_CPP)/platform/unix/socket_listener.cpp $(HGS_CPP)/api/hgs_endpoint.cpp -l:libwayland-server.so.0 -o $(BUILD_DIR)/socket_listener_test
	$(BUILD_DIR)/socket_listener_test
