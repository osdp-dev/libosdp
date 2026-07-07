#
#  Copyright (c) 2024-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

ROOT_DIR ?= ../..
BUILD_DIR ?= $(ROOT_DIR)/build

all:
	g++ -std=c++0x -I$(ROOT_DIR)/include cp_app.cpp -o cp_sample -L$(BUILD_DIR)/lib -losdp
	g++ -std=c++0x -I$(ROOT_DIR)/include pd_app.cpp -o pd_sample -L$(BUILD_DIR)/lib -losdp
