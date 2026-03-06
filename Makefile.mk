SOURCE = src/ae.c src/anet.c src/sockcompat.c
LIB_TARGET = libae.lib
LIBS = ws2_32.lib libae.lib
LINK_FLAG = /DEBUG /subsystem:console
CFLAGS = /EHsc /D_WIN32 /D_MSC_VER /I. /Isrc /W3 /nologo

# ae.c includes ae_iocp.c on Windows, so ae.obj depends on both
ae.obj: src/ae.c src/ae_iocp.c
	cl $(CFLAGS) /c src/ae.c /Foae.obj
anet.obj: src/anet.c
	cl $(CFLAGS) /c src/anet.c /Foanet.obj
sockcompat.obj: src/sockcompat.c
	cl $(CFLAGS) /c src/sockcompat.c /Fosockcompat.obj

$(LIB_TARGET): ae.obj anet.obj sockcompat.obj
	link -lib ae.obj anet.obj sockcompat.obj /out:$(LIB_TARGET) /nologo

timer: $(LIB_TARGET) example\timer.c
	cl $(CFLAGS) /c example\timer.c
	link $(LINK_FLAG) timer.obj $(LIBS) /OUT:timer.exe /nologo

echoserver: $(LIB_TARGET) example\echoserver.c
	cl $(CFLAGS) /c example\echoserver.c
	link $(LINK_FLAG) echoserver.obj $(LIBS) /OUT:echoserver.exe /nologo

echoclient: $(LIB_TARGET) example\echoclient.c
	cl $(CFLAGS) /c example\echoclient.c
	link $(LINK_FLAG) echoclient.obj $(LIBS) /OUT:echoclient.exe /nologo

clean:
	-del /Q *.exe *.obj *.lib *.ilk *.pdb 2>nul