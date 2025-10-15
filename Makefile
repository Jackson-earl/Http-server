CC= clang
CFLAGS= -Wall -Wextra -Werror -pedantic

TARGET= httpserver
SOURCES= httpserver.c asgn2_helper_funcs.a

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) -o $(TARGET) httpserver.c asgn2_helper_funcs.a

clean:
	rm -f httpserver *.o


