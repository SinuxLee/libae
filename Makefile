SRC = src/ae.c src/anet.c
OBJ = ${SRC:.c=.o}
CFLAGS = -Wno-parentheses -Wno-switch-enum -Wno-unused-value

all: libae.a timer echo

libae.a: $(OBJ)
	$(AR) -rc $@ $(OBJ)

%.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@

timer: example/timer.o libae.a
	$(CC) $^ -o $@

echo: example/echo.o libae.a
	$(CC) $^ -o $@

clean:
	rm -f $(OBJ) libae.a example/*.o timer echo

.PHONY: all timer echo clean 
