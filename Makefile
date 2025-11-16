# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2019 Xing Loong <xing.xl.loong@gmail.com>

CC = $(CROSS_COMPILE)gcc
LD = $(CROSS_COMPILE)gcc
STRIP = $(CROSS_COMPILE)strip

CFLAGS = -g -O1 -Wall -Werror -std=gnu99 -MD -MP \
	-D_GNU_SOURCE -Iinclude
LDFLAGS = -lpthread

.PHONY: all clean
all: mbedtee-supp

OBJS = main.o reefs.o rpmb.o

-include $(OBJS:.o=.d)

mbedtee-supp: $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^
	$(STRIP) $@

%.o: %.c
	$(CC) $(CFLAGS) -o $@ -c $<

clean:
	@rm -f $(OBJS) $(OBJS:.o=.d) mbedtee-supp
