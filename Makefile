memnbd.exe: *.cpp Makefile
	i586-mingw32msvc-g++ -g3 -O1 -Wall nbdsrvr.cpp -o memnbd.exe -lws2_32

memnbd.exe.so: *.cpp Makefile
	wineg++ -lws2_32 nbdsrvr.cpp -o memnbd
