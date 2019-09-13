SRC = src/ae.c src/anet.c
OBJ = ${SRC:.c=.o}
CFLAGS = -Wno-parentheses -Wno-switch-enum -Wno-unused-value

libae.a: $(OBJ)
	$(AR) -rc $@ $(OBJ)

%.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@

timer: example/timer.o libae.a
	$(CC) $^ -o $@

echoclient: example/echoclient.o libae.a
	$(CC) $^ -o $@

echoserver: example/echoserver.o libae.a
	$(CC) $^ -o $@

clean:
	rm -f $(OBJ) libae.a example/timer.o timer example/echoserver.o echoserver example/echoclient.o echoclient

.PHONY: clean
