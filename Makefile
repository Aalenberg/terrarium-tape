TARGET = terrarium_tape
CPP_SOURCES = main.cpp

LIBDAISY_DIR ?= third_party/libDaisy
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core

include $(SYSTEM_FILES_DIR)/Makefile