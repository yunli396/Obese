CXX := g++
CXXFLAGS := -O0 -g3 -std=c++17 -pthread -Wall -Wextra -fno-omit-frame-pointer

TARGET := obese
SERVER := obese-server

all: $(TARGET) $(SERVER)

$(TARGET): obese.cpp lha.cpp lha.h
	$(CXX) $(CXXFLAGS) -o $@ obese.cpp lha.cpp

$(SERVER): obese-server.cpp lha.cpp lha.h
	$(CXX) $(CXXFLAGS) -o $@ obese-server.cpp lha.cpp

# The point is to be bloated and slow, so we deliberately do NOT use -O2,
# do NOT strip, and keep all debug info. You have been warned.

install: $(TARGET) $(SERVER)
	install -m 0755 $(TARGET) /usr/local/bin/$(TARGET)
	install -m 0755 $(SERVER) /usr/local/bin/$(SERVER)
	@echo "obese installed. it will now install itself again on every boot."

clean:
	rm -f $(TARGET) $(SERVER)

.PHONY: all install clean
