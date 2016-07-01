#include "stdafx.h"

#pragma comment(lib, "ws2_32.lib")

enum MSG_TYPE : uint8_t
{
	C_FILE_INFO_REQUEST = 0x10,           // CLI->SRV
	C_FILE_INFO_RESPONSE = 0x20,          // SRV->CLI
	C_FILE_BLOCK_RANGE_REQUEST = 0x30,    // CLI->SRV
	C_FILE_BLOCK_ARRAY_REQUEST = 0x40,    // CLI->SRV
	C_FILE_BLOCK_RESPONSE = 0x50,         // SRV->CLI
	C_ERROR = 0xEE,                       // SRV->CLI
};

enum ERROR_TYPE : uint8_t
{
	C_ERR_FILE_NOT_FOUND = 0x20,          // SRV->CLI
	C_ERR_INVALID_FILE_HANDLER = 0x21,    // SRV->CLI
	C_ERR_INVALID_BLOCK_REQUESTED = 0x22, // SRV->CLI
	C_ERR_INVALID_OPERATION = 0x23,       // SRV->CLI
};

void SetConsoleToUTF8();
int CheckArgumens(int argc, char* argv[]);
int WinSockInit();
int RecvFromWithTimeout(long sec, long usec);

const uint16_t DATAGRAM_MAX_SIZE = 4096;
const uint16_t MAX_BLOCK_SIZE = 4096 - 7;

char* buf = new char[DATAGRAM_MAX_SIZE];

SOCKET clientSocket;
struct sockaddr_in clientAddr;
struct sockaddr* serverAddr;
int serverAddrLen;
struct sockaddr* recvedFrom;
int recvedFromLen;

uint32_t blocksCount;
uint16_t fileIdentifier;
std::set<uint32_t>remainingBlocks;

std::ofstream file;

int main(int argc, char* argv[])
{
	clock_t begin = clock();

	int error;
	SetConsoleToUTF8();
	error = CheckArgumens(argc, argv);
	if (error != 0)
	{
		wprintf(L"Ошибка аргументов командной строки.\n");
		wprintf(L"Использование: .\\client.exe 192.168.10.20 3570 file-to-get.dat\n");
		return 10;
	}

	error = WinSockInit();
	if (error != 0)
	{
		wprintf(L"Ошибка при инициализации WinSock\n");
		return 20;
	}

	// https://msdn.microsoft.com/en-us/library/windows/desktop/bb530741(v=vs.85).aspx
	clientSocket = INVALID_SOCKET;
	clientSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (clientSocket == INVALID_SOCKET) {
		wprintf(L"Ошибка при вызове socket(): %d\n", WSAGetLastError());
		WSACleanup();
		return 30;
	}

	ZeroMemory(&clientAddr, sizeof(clientAddr));
	clientAddr.sin_family = AF_INET;
	clientAddr.sin_port = 0;
	// TODO: "0.0.0.0" вынести в отдельную переменную
	error = inet_pton(AF_INET, "0.0.0.0", &clientAddr.sin_addr.s_addr);
	if (error < 0)
	{
		wprintf(L"Ошибка inet_pton()\n");
		WSACleanup();
		return 40;
	}

	error = bind(clientSocket, reinterpret_cast<SOCKADDR*>(&clientAddr), sizeof(clientAddr));
	if (error != 0)
	{
		wprintf(L"Ошибка bind()\n");
		WSACleanup();
		return 50;
	}
	
	sockaddr_in bindInfo;
	int bindInfoLen;
	getsockname(clientSocket, reinterpret_cast<SOCKADDR*>(&bindInfo), &bindInfoLen);
	wprintf(L"Клиент запущен по адресу 0.0.0.0:%d\n", bindInfo.sin_port);
	
	struct addrinfo hints;
	struct addrinfo* result = nullptr;
	ZeroMemory(&hints, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = IPPROTO_UDP;

	// Resolve the server address and port
	INT infoResult = getaddrinfo(argv[1], argv[2], &hints, &result);
	if (infoResult != 0) {
		wprintf(L"getaddrinfo failed: %d\n", infoResult);
		WSACleanup();
		return 18;
	}

	serverAddr = static_cast<sockaddr*>(result->ai_addr);
	serverAddrLen = result->ai_addrlen;
	char servIpAddr[INET6_ADDRSTRLEN];
	const char* ntopError = inet_ntop(AF_INET,
			&reinterpret_cast<struct sockaddr_in *>(result->ai_addr)->sin_addr,
			servIpAddr,
			sizeof(servIpAddr));

	if (ntopError == nullptr)
	{
		wprintf(L"Ошибка inet_ntop()\n");
		return 24;
	}
	wprintf(L"Адрес сервера: %hs:%hs\n", servIpAddr, argv[2]);
	
	while(true)
	{
		buf[0] = C_FILE_INFO_REQUEST;
		strcpy(&buf[1], argv[3]);
		sendto(clientSocket, buf, strlen(buf) + 1, 0, serverAddr, serverAddrLen);
		int bytes = RecvFromWithTimeout(2, 500000);
		if (bytes > 0)
		{
			if (buf[0] != C_ERROR)
			{
				fileIdentifier = *reinterpret_cast<uint16_t*>(&buf[1]);
				blocksCount = *reinterpret_cast<uint32_t*>(&buf[3]);
				wprintf(L"Метаданные получены. ID файла: %d, кол-во блоков: %d.\n", fileIdentifier, blocksCount);
				break;
			}
			wprintf(L"Сервер сообщил об ошибке\n");
			return 44;
		}
		wprintf(L"Ошибка или таймаут...\n");
	}

	file.open(argv[3], std::ios::binary | std::ios::trunc);

	for (uint32_t i=0; i<blocksCount; i++)
	{
		remainingBlocks.insert(i);
	}

	std::set<uint32_t>::iterator it;
	while (!remainingBlocks.empty())
	{
		//wprintf(L"Проход по оставшимся блокам...\n");
		buf[0] = C_FILE_BLOCK_ARRAY_REQUEST;
		*reinterpret_cast<uint16_t*>(&buf[1]) = fileIdentifier;
		uint16_t maxBlocksRequested = 50;
		it = remainingBlocks.begin();
		uint16_t blocksRequested = 0;
		int index = 0;
		while (blocksRequested < maxBlocksRequested && it != remainingBlocks.end())
		{
			*reinterpret_cast<uint32_t*>(&buf[3 + index * sizeof(uint32_t)]) = *it++;
			blocksRequested++;
			index++;
		}

		sendto(clientSocket, buf, blocksRequested * 4 + 3, 0, serverAddr, serverAddrLen);

		for (uint16_t i = 0; i<blocksRequested; i++)
		{
			int bytes = RecvFromWithTimeout(0, 100000);
			if (bytes > 0)
			{
				uint32_t receivedBlock = *reinterpret_cast<uint32_t*>(&buf[3]);
				file.seekp(receivedBlock*MAX_BLOCK_SIZE);
				file.write(&buf[7], bytes - 7);
				remainingBlocks.erase(*reinterpret_cast<uint32_t*>(&buf[3]));
			}
			else
			{
				break;
			}
		}
	}

	//wprintf(L"Файл получен: %d\n", *reinterpret_cast<uint32_t*>(&buf[3]));

	file.seekp(0, std::ios::end);
	auto size = file.tellp();
	file.seekp(0, std::ios::beg);

	file.close();

	uint32_t sizeMbytes = size / 1024 / 1024;

	clock_t end = clock();
	double timeElapsed = static_cast<double>(end - begin) / CLOCKS_PER_SEC;
	double speed = sizeMbytes / timeElapsed;

	wprintf(L"Передача окончена. Размер файла: %d МБ. Скорость передачи: %.1f МБайт/c", sizeMbytes, speed);

	wprintf(L"\n\nНажмите любую клавишу для продолжения...");

	getwchar();

    return 0;
}



void SetConsoleToUTF8()
{
	_setmode(_fileno(stdout), _O_U16TEXT);
	_setmode(_fileno(stdin), _O_U16TEXT);
	_setmode(_fileno(stderr), _O_U16TEXT);
}

int CheckArgumens(int argc, char* argv[])
{
	// TODO: добавить дополнительные проверки на валидность ip, порта и т.д.
	if (argc != 4)
	{
		return 20;
	}
	return 0;
}

int WinSockInit()
{
	// Существуют версии WinSock: 1.0, 1.1, 2.0, 2.1, 2.2
	// Мы запрашиваем версию 2.2
	WORD wVersionRequested = MAKEWORD(2, 2);
	WSAData wsaData;
	int16_t err;

	// Перед началом работы с сокетами необходимо вызвать
	// инициализирующую функцию и передать ей 
	// нужную нам версиию. При успешном вызове функции
	// должен вернуться код 0
	err = WSAStartup(wVersionRequested, &wsaData);
	if (err != 0)
	{
		return err;
	}

	// Библиотека инициализирована, но ее еще желательно проверить
	// на соответствие версии (должна быть 2.2).
	if (LOBYTE(wsaData.wVersion) != 2 || HIBYTE(wsaData.wVersion) != 2)
	{
		WSACleanup();
		return 10;
	}

	return 0;
}

int RecvFromWithTimeout(long sec, long usec)
{
	fd_set fds;
	int n;
	struct timeval tv;

	// Set up the file descriptor set.
	FD_ZERO(&fds);
	FD_SET(clientSocket, &fds);

	// Set up the struct timeval for the timeout.
	tv.tv_sec = sec;
	tv.tv_usec = usec;

	// Wait until timeout or data received.
	n = select(clientSocket, &fds, nullptr, nullptr, &tv);
	if (n == 0)
	{
		//wprintf(L"Время ожидания ответа истекло.\n");
		return 0;
	}
	else if (n == -1)
	{
		wprintf(L"Ошибка select()\n");
		return -10;
	}
	//wprintf(L"Trying recvfrom..\n");
	int bytes = recvfrom(clientSocket, buf, DATAGRAM_MAX_SIZE, 0, serverAddr, &serverAddrLen);
	//wprintf(L"Bytes received: %d.\n", bytes);
	return bytes;
}