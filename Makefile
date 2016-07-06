PKG_CONFIG ?= pkg-config

libav_pkgs := libavcodec libavfilter libavformat libavutil
libav_cflags  = $(shell $(PKG_CONFIG) --cflags $(libav_pkgs))
libav_ldflags = $(shell $(PKG_CONFIG) --libs   $(libav_pkgs))

cflags = -std=c99 -O0 -g -Wall -Wextra -Wpedantic -Wshadow \
		-Werror=implicit-function-declaration -Werror=vla \
		$(libav_cflags) $(CFLAGS)
ldflags = $(libav_ldflags) $(LDFLAGS)

cropdetect3: cropdetect3.c
	$(CC) $(strip $(cflags)) -o $@ $^ $(strip $(ldflags))

clean:
	$(RM) cropdetect3
