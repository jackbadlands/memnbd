memnbd.exe: *.cpp
	i586-mingw32msvc-g++ -g3 -O1 -Wall nbdsrvr.cpp -o memnbd.exe -lws2_32
