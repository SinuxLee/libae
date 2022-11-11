SOURCE = src/ae.c src/anet.c src/sockcompat.c
LIB_TARGET = libae.lib
LIBS = ws2_32.lib User32.lib libae.lib
LINK_FLAG = /DEBUG /subsystem:console

$(LIB_TARGET):
	@cl /EHsc /D_WIN32 /c $(SOURCE)
	@link -lib *.obj /out:$(LIB_TARGET)
	
timer:
	@cl /c example\timer.c
	@link $(LINK_FLAG) timer.obj $(LIBS) /OUT:timer.exe
	
echoserver: example\echoserver.c
	@cl /c example\echoserver.c
	@link $(LINK_FLAG) echoserver.obj $(LIBS) /OUT:echoserver.exe

echoclient: example\echoclient.c
	@cl /c example\echoclient.c
	@link $(LINK_FLAG) echoclient.obj $(LIBS) /OUT:echoclient.exe

clean:
	@del *.exe
	@del *.obj
	@del *.lib
	@del *.ilk
	@del *.pdb