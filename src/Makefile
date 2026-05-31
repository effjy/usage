CC = gcc
CFLAGS = -Wall -Wextra -O2 `pkg-config --cflags gtk+-3.0` -lm
LIBS = `pkg-config --libs gtk+-3.0` -lm
TARGET = usage
SRC = main.c
OBJ = $(SRC:.c=.o)

PREFIX = /usr/local
BINDIR = $(PREFIX)/bin
APPDIR = /usr/share/applications
ICONDIR = /usr/share/icons/hicolor/256x256/apps

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJ)

install: all
	@echo "Installing binary..."
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	
	@echo "Installing desktop entry..."
	install -d $(DESTDIR)$(APPDIR)
	install -m 644 usage.desktop $(DESTDIR)$(APPDIR)/usage.desktop
	
	@echo "Installing program icon..."
	install -d $(DESTDIR)$(ICONDIR)
	install -m 644 usage.png $(DESTDIR)$(ICONDIR)/usage.png
	
	@echo "Updating system desktop database and icon caches..."
	-gtk-update-icon-cache -f -t /usr/share/icons/hicolor
	-update-desktop-database /usr/share/applications
	@echo "Installation complete."

uninstall:
	@echo "Uninstalling binary..."
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	
	@echo "Uninstalling desktop entry..."
	rm -f $(DESTDIR)$(APPDIR)/usage.desktop
	
	@echo "Uninstalling icon..."
	rm -f $(DESTDIR)$(ICONDIR)/usage.png
	
	@echo "Updating system desktop database and icon caches..."
	-gtk-update-icon-cache -f -t /usr/share/icons/hicolor
	-update-desktop-database /usr/share/applications
	@echo "Uninstallation complete."

.PHONY: all clean install uninstall
