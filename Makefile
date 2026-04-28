CC      = gcc

# SDL3_image handled separately so a missing package doesn't break the SDL3 flags
SDL3IMG_CFLAGS := $(shell pkg-config --cflags SDL3_image 2>/dev/null)
SDL3IMG_LIBS   := $(shell pkg-config --libs   SDL3_image 2>/dev/null || echo "-lSDL3_image")

CFLAGS  = -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE \
          $(shell pkg-config --cflags sdl3 sdl3-ttf) \
          $(SDL3IMG_CFLAGS) \
          -I/usr/include/cjson

LDFLAGS = $(shell pkg-config --libs sdl3 sdl3-ttf) \
          $(SDL3IMG_LIBS) \
          -lcjson -lcurl -Wl,--no-as-needed -lssl -lcrypto -lpthread -lm

SRCS = src/main.c src/chat.c src/ws.c src/gui.c src/console.c \
       src/config.c src/imgcache.c
OBJS = $(SRCS:.c=.o)
BIN  = nurealmschat

HDRS = src/chat.h src/config.h src/ws.h src/gui.h src/imgcache.h src/console.h

.PHONY: all clean

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

src/%.o: src/%.c $(HDRS)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(BIN)
