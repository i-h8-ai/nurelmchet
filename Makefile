CC      = gcc

# Feature flags — override on the command line, e.g.: make TEXT_SELECT=0 USE_SDL2=1
TEXT_SELECT ?= 1
USE_SDL2    ?= 0

ifeq ($(USE_SDL2),1)
# SDL2 build — for Raspberry Pi OS where SDL3 is not packaged
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 SDL2_ttf SDL2_image 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs   sdl2 SDL2_ttf SDL2_image 2>/dev/null \
                      || echo "-lSDL2 -lSDL2_ttf -lSDL2_image")
SDL_DEFINE := -DUSE_SDL2
else
# SDL3 build (default)
SDL3IMG_CFLAGS := $(shell pkg-config --cflags SDL3_image 2>/dev/null)
SDL3IMG_LIBS   := $(shell pkg-config --libs   SDL3_image 2>/dev/null || echo "-lSDL3_image")
SDL_CFLAGS := $(shell pkg-config --cflags sdl3 sdl3-ttf) $(SDL3IMG_CFLAGS)
SDL_LIBS   := $(shell pkg-config --libs   sdl3 sdl3-ttf) $(SDL3IMG_LIBS)
SDL_DEFINE :=
endif

CFLAGS  = -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE \
          $(if $(filter 1,$(TEXT_SELECT)),-DENABLE_TEXT_SELECT) \
          $(SDL_DEFINE) \
          $(SDL_CFLAGS) \
          -I/usr/include/cjson

LDFLAGS = $(SDL_LIBS) \
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
