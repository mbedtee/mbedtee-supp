#
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2019 KapaXL (kapa.xl@outlook.com)
#

CC = $(CROSS_COMPILE)gcc
LD = $(CROSS_COMPILE)gcc
STRIP = $(CROSS_COMPILE)strip

CFLAGS = -g -O2 -Wall -Werror -D_GNU_SOURCE \
	-Iinclude -I$(MBEDTEE_INC)
LDFLAGS = -lpthread

.PHONE: all
all: mbedtee_supp

OBJS = main.o reefs.o

mbedtee_supp: $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^
	$(STRIP) $@

%.o: %.c
	$(CC) $(CFLAGS) -o $@ -c $<

clean:
	@rm -f $(OBJS) mbedtee_supp
