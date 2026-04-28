CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c11 \
          $(shell pkg-config --cflags sdl3 sdl3-ttf) \
          -I/usr/include/cjson
LDFLAGS = $(shell pkg-config --libs sdl3 sdl3-ttf) \
          -lcjson -lcurl -Wl,--no-as-needed -lssl -lcrypto -lpthread -lm

SRCS = src/main.c src/chat.c src/ws.c src/gui.c src/console.c
OBJS = $(SRCS:.c=.o)
BIN  = nurealmschat

.PHONY: all clean

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(BIN)
