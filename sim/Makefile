# OpenProfalux — PC simulation mode
# Compile with gcc for local debugging without ESP32 hardware

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g -DSIM_MODE -I../firmware/main -I.
LDFLAGS =

SRCS = sim_main.c \
       mock_cc1101.c \
       mock_nvs.c \
       ../firmware/main/keeloq.c \
       ../firmware/main/profalux_sim.c

OBJS = $(SRCS:.c=.o)

all: sim_openprofalux test_keeloq

sim_openprofalux: sim_main.o mock_cc1101.o mock_nvs.o ../firmware/main/keeloq.o ../firmware/main/profalux_sim.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_keeloq: test_keeloq.o ../firmware/main/keeloq.o ../firmware/main/pfx_keys.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) sim_openprofalux test_keeloq *.o ../firmware/main/*.o

.PHONY: all clean test
