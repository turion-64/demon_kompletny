CC = gcc
CFLAGS = -Wall -Wextra -O2

TARGET = DEMON
SRC = demon.c

PREFIX = /usr/local/bin

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	@cp $(TARGET) $(PREFIX)/$(TARGET)
	@chmod 755 $(PREFIX)/$(TARGET)
	@echo "install: $(PREFIX)/$(TARGET)."

uninstall:
	@rm -f $(PREFIX)/$(TARGET)
	@echo "uninstall: $(PREFIX)/$(TARGET)."

.PHONY: all clean install uninstall
