#include <errno.h>
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>

int portnr;
unsigned long long size_bytes;

int error_mapper(DWORD winerr)
{
	switch(winerr){
	case ERROR_ACCESS_DENIED:
	case ERROR_WRITE_PROTECT:
		return EACCES;

	case ERROR_WRITE_FAULT:
	case ERROR_READ_FAULT:
	case ERROR_GEN_FAILURE:
		return EIO;

	case ERROR_SEEK:
	case ERROR_NEGATIVE_SEEK:
		return ERANGE;

	case ERROR_BAD_UNIT:
	case ERROR_NOT_READY:
	case ERROR_CRC:
	case ERROR_SECTOR_NOT_FOUND:
	case ERROR_DEV_NOT_EXIST:
	case ERROR_DISK_CHANGE:
	case ERROR_BUSY:
	case ERROR_CAN_NOT_COMPLETE:
	case ERROR_UNRECOGNIZED_VOLUME:
	case ERROR_DISK_RECALIBRATE_FAILED:
	case ERROR_DISK_OPERATION_FAILED:
	case ERROR_DISK_RESET_FAILED:
		return EIO;
	}

	return EINVAL; /* what else? */
}

LARGE_INTEGER add_li(LARGE_INTEGER i1, LARGE_INTEGER i2)
{
	LARGE_INTEGER dummy;

	dummy = i1;

	dummy.LowPart += i2.LowPart;
	if (dummy.LowPart <= i1.LowPart && i2.LowPart > 0)
	{
		dummy.HighPart++;
	}

	dummy.HighPart += i2.HighPart;

	return dummy;
}

int READ(SOCKET sh, UCHAR *whereto, int howmuch)
{
	int pnt = 0;

#ifdef _DEBUG
	printf("read: %d bytes requested\n", howmuch);
#endif

	while(howmuch > 0)
	{
		int nread = recv(sh, (char *)&whereto[pnt], howmuch, 0);
		if (nread == 0)
			break;
		if (nread == SOCKET_ERROR)
		{
			fprintf(stderr, "Connection dropped. Error: %d\n", WSAGetLastError());
			break;
		}

		pnt += nread;
		howmuch -= nread;
	}

	return pnt;
}

int WRITE(SOCKET sh, UCHAR *wherefrom, int howmuch)
{
	int pnt = 0;

	while(howmuch > 0)
	{
		int nwritten = send(sh, (char *)&wherefrom[pnt], howmuch, 0);
		if (nwritten == 0)
			break;
		if (nwritten == SOCKET_ERROR)
		{
			fprintf(stderr, "Connection dropped. Error: %d\n", WSAGetLastError());
			break;
		}

		pnt += nwritten;
		howmuch -= nwritten;
	}

	return pnt;
}

BOOL getu32(SOCKET sh, ULONG *val)
{
	UCHAR buffer[4];

	if (READ(sh, buffer, 4) != 4)
		return FALSE;

	*val = (buffer[0] << 24) + (buffer[1] << 16) + (buffer[2] << 8) + (buffer[3]);

	return TRUE;
}

BOOL putu32(SOCKET sh, ULONG value)
{
	UCHAR buffer[4];

	buffer[0] = (value >> 24) & 255;
	buffer[1] = (value >> 16) & 255;
	buffer[2] = (value >>  8) & 255;
	buffer[3] = (value      ) & 255;

	if (WRITE(sh, buffer, 4) != 4)
		return FALSE;
	else
		return TRUE;
}

DWORD WINAPI draad(LPVOID data)
{
	SOCKET sockh = (SOCKET)data;
	LARGE_INTEGER offset, fsize;

	// find length of file or starting offset of partition
	offset.QuadPart = 0LL;
	fsize.QuadPart = size_bytes;

	char* mem_data = (char*)malloc(size_bytes);
	
	if (!mem_data)
	{
		fprintf(stderr, "Error malloc %lld bytes\n", size_bytes);
		goto error;
	}

	/* negotiate */
	printf("Negotiating...\n");
	if (WRITE(sockh, (unsigned char *)"NBDMAGIC", 8) != 8)
	{
		fprintf(stderr, "Failed to send magic string\n");
		goto error;
	}

	// some other magic value
	unsigned char magic[8];
	magic[0] = 0x00;
	magic[1] = 0x00;
	magic[2] = 0x42;
	magic[3] = 0x02;
	magic[4] = 0x81;
	magic[5] = 0x86;
	magic[6] = 0x12;
	magic[7] = 0x53;
	if (WRITE(sockh, magic, 8) != 8)
	{
		fprintf(stderr, "Failed to send 2nd magic string\n");
		goto error;
	}

	// send size of file
	unsigned char exportsize[8];
	exportsize[7] = (fsize.LowPart       ) & 255;	// low word
	exportsize[6] = (fsize.LowPart  >>  8) & 255;
	exportsize[5] = (fsize.LowPart  >> 16) & 255;
	exportsize[4] = (fsize.LowPart  >> 24) & 255;
	exportsize[3] = (fsize.HighPart      ) & 255;	// high word
	exportsize[2] = (fsize.HighPart >>  8) & 255;
	exportsize[1] = (fsize.HighPart >> 16) & 255;
	exportsize[0] = (fsize.HighPart >> 24) & 255;
	if (WRITE(sockh, exportsize, 8) != 8)
	{
		fprintf(stderr, "Failed to send filesize\n");
		goto error;
	}
	
	// send a couple of zeros */
	unsigned char buffer[128];
	memset(buffer, 0x00, 128);
	if (WRITE(sockh, buffer, 128) != 128)
	{
		fprintf(stderr, "Failed to send a couple of 0x00s\n");
		goto error;
	}

	printf("Started!\n");

	/* main loop */
	for(;;)
	{
		UCHAR handle[9];
		ULONG magic, len, type;
		LARGE_INTEGER from;
		LARGE_INTEGER cur_offset;
		int err = 0;

		if (getu32(sockh, &magic) == FALSE ||	// 0x12560953
			getu32(sockh, &type)  == FALSE ||	// 0=read,1=write
			READ(sockh, handle, 8) != 8    ||	// handle
			getu32(sockh, (DWORD *)&from.HighPart) == FALSE ||	// high word of offset
			getu32(sockh, &from.LowPart)  == FALSE ||	// ...low word of offset
			getu32(sockh, &len)   == FALSE)		// length
		{
			fprintf(stderr, "Failed to read from socket\n");
			break;
		}

#ifdef _DEBUG
		handle[8] = 0x00;
		printf("Magic:    %lx\n", magic);
		printf("Offset:   %ld,%ld (%lx%lx)\n", from.HighPart, from.LowPart, from.HighPart, from.LowPart);
		printf("Len:      %ld\n", len);
		printf("Handle:   %s\n", handle);
		printf("Req.type: %ld (%s)\n\n", type, type?"write":"read");
#endif

		// verify protocol
		if (magic != 0x25609513)
		{
			fprintf(stderr, "Unexpected protocol version! (got: %lx, expected: 0x25609513)\n", magic);
			break;
		}

		// calculate current offset
		cur_offset = add_li(offset, from);

		// error while seeking?
		if (cur_offset.QuadPart < 0 || cur_offset.QuadPart > (signed long long)size_bytes)
		{
#ifdef _DEBUG
			printf("Sending errno=%d\n", err);
#endif
			// send errorstate
			if (putu32(sockh, 0x67446698) == FALSE ||
				putu32(sockh, err) == FALSE ||
				WRITE(sockh, handle, 8) != 8)
			{
				fprintf(stderr, "Failed to send errorstate through socket\n");
				break;
			}
		}
		else if (type == 1)	// write
		{
			if (cur_offset.QuadPart + len >= (signed long long int)size_bytes)
			{
				err = error_mapper(5);
				break;
			}
			while(len > 0)
			{
				UCHAR buffer[32768];
				// read from socket
				DWORD nb = recv(sockh, (char *)buffer, min(len, 32768), 0);
				if (nb == 0)
					break;

				// write to file;
				memcpy(mem_data+cur_offset.QuadPart, buffer, nb);
				cur_offset.QuadPart += nb;

				len -= nb;
			}
			if (len)	// connection was closed
			{
				fprintf(stderr, "Connection was dropped while receiving data\n");
				break;
			}

			// send 'ack'
			if (putu32(sockh, 0x67446698) == FALSE ||
				putu32(sockh, err) == FALSE ||
				WRITE(sockh, handle, 8) != 8)
			{
				fprintf(stderr, "Failed to send through socket\n");
				break;
			}
		}
		else if (type == 0)
		{
			// send 'ack'
			if (putu32(sockh, 0x67446698) == FALSE ||
				putu32(sockh, 0) == FALSE ||
				WRITE(sockh, handle, 8) != 8)
			{
				fprintf(stderr, "Failed to send through socket\n");
				break;
			}

			while(len > 0)
			{
				UCHAR buffer[32768];
				int nb = min(len, 32768);

				// read nb to buffer;
				memcpy(buffer, mem_data+cur_offset.QuadPart, nb);
				cur_offset.QuadPart += nb;

				// send through socket
				if (WRITE(sockh, buffer, nb) != nb) // connection was closed
				{
					fprintf(stderr, "Connection dropped while sending block\n");
					break;
				}

				len -= nb;
			}
			if (len)	// connection was closed
				break;
		}
		else
		{
			printf("Unexpected commandtype: %ld\n", type);
			break;
		}
	}

	// close file
error:
	closesocket(sockh);

	ExitThread(0);

	return 0;
}
	
int main(int argc, char *argv[])
{
	SOCKET newconnh;
	WSADATA WSAData;

	printf("memnbd (in-memory NBD server) based on nbdsrvr v0.2, (C) 2003 by folkert@vanheusden.com\n");
	printf("Designed to \"connect\" Windows box's memory to Linux hosts. (Coded by _Vi)\n");

	if (argc != 3 && argc != 4)
	{
		fprintf(stderr, "Usage: %s size_in_bytes portnr\n", argv[0]);
		return 1;
	}
	size_bytes = atoll(argv[1]);
	portnr = atoi(argv[2]);

	// initialize WinSock library
	(void)WSAStartup(0x101, &WSAData); 
 	
	// create listener socket
	newconnh= socket(AF_INET, SOCK_STREAM, 0);
	if (newconnh == INVALID_SOCKET)
		return -1;

	// bind
	struct sockaddr_in      ServerAddr;
	int     ServerAddrLen;
	ServerAddrLen = sizeof(ServerAddr);
	memset((char *)&ServerAddr, '\0', ServerAddrLen);
	ServerAddr.sin_family = AF_INET;
	ServerAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	ServerAddr.sin_port = htons(portnr);
	if (bind(newconnh, (struct sockaddr *)&ServerAddr, ServerAddrLen) == -1)
		return -1;

	// listen
	if (listen(newconnh, 5) == -1)
		return -1;

	for(;;)
	{
		SOCKET clienth;
		struct sockaddr_in      clientaddr;
		int     clientaddrlen;

		clientaddrlen = sizeof(clientaddr);

		/* accept a connection */
		clienth = accept(newconnh, (struct sockaddr *)&clientaddr, &clientaddrlen);

		if (clienth != INVALID_SOCKET)
		{
			printf("Connection made with %s\n", inet_ntoa(clientaddr.sin_addr));

			DWORD tid;
			HANDLE th = CreateThread(NULL, 0, draad, (void *)clienth, 0, &tid);
			(void)th;
		}
	}

	return 0;
}
