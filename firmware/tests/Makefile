CC      = gcc
CFLAGS  = -Wall -Wextra -Wpedantic -std=c11 -g \
           -I../node/main \
           -DUNIT_TEST

TARGET  = test_radcom
SRC     = test_main.c

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: run clean
