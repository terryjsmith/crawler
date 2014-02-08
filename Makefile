
CC := g++
CFLAGS=-Wall -g -Iinclude -I/usr/include/mysql -pthread
LDFLAGS=-lmysqlclient -lhiredis -lcrypto -lssl -pthread -lcares

# Link command:
crawler: src/main.o src/url.o src/httprequest.o src/worker.o src/domain.o src/parser.o
	$(CC) $^ -o bin/$@ $(LDFLAGS)

urls: src/url.o test/urltest.o
	$(CC) $^ -o bin/$@ $(LDFLAGS)

robots: src/url.o src/httprequest.o test/robotstest.o
	$(CC) $^ -o bin/$@ $(LDFLAGS)

parser: src/url.o src/httprequest.o src/domain.o src/parser.o test/parser.o
	$(CC) $^ -o bin/$@ $(LDFLAGS)

# Individual files
%.o: %.cpp
	$(CC) $(CFLAGS) -c $< -o $@

# Build both
all: crawler
tests: urls robots parser

clean:
	rm src/*.o test/*.o
