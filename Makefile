CC = gcc

SRC_DIR = src
BIN_DIR = bin

TARGET_NAME = voip_phone
TARGET = $(BIN_DIR)/$(TARGET_NAME)

SRC = $(SRC_DIR)/voip_phone.c

CFLAGS := $(shell pkg-config --cflags gtk4 speexdsp) -pthread

LIBS := $(shell pkg-config --libs gtk4 speexdsp) -lportaudio -lm

all: $(TARGET)

$(TARGET): $(SRC)
	@mkdir -p $(BIN_DIR) # binディレクトリがなければ作成
	@echo "Compiling $< -> $@"
	$(CC) $< -o $@ $(CFLAGS) $(LIBS)
	@echo "Build finished: $@"

clean:
	@echo "Cleaning up..."
	rm -rf $(BIN_DIR)

.PHONY: all clean
