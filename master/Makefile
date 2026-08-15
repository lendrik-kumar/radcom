CC      = gcc
CFLAGS  = -Wall -O2 -g
LIBS    = -lsqlite3 -lgpiod
OBJS    = master.o radio.o packet.o routing.o queue.o poller.o
TARGET  = radcom-master

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

install: all
	install -m 755 $(TARGET) /usr/local/bin/
	install -m 644 radcom-master.service /etc/systemd/system/
	mkdir -p /var/lib/radcom
	systemctl daemon-reload
	systemctl enable --now radcom-master
