//getopt
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
//для работы с socket
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
//разное
#include <string>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <assert.h>
#include <thread>
//для работы epoll
#include <sys/epoll.h>
#include <functional>
//regex
#include <regex>
//организуем цикл
//при добавлении нового клиента добавляем его событие в очередь на обработку.
//запускаем сразу 4(или больше) потока на обработку очереди событий(изъять событие(сокет) из очереди потокобезопасно и обработать его)
//в теле потоков использовать блокировки на добавление/изъятие события в очереди событий. Все потоки параллельно ломятся взять событие из очереди и обработать его.
/*запрос: 
только заголовка:
curl -I -0 -X GET "http://127.0.0.1:1234/index.html"
заголовка и содержимого:
curl -0 -X GET "http://127.0.0.1:1234/index.html"
*/

#define MAX_EVENTS 1024	//максимальное число событий дескриптора за раз
#define BUFSIZE 4096
#define NTHREADS 4
std::mutex mtx;
std::condition_variable cv_start_threads;

int set_nonblock(int fd)
{
	int flags;
	#if defined(O_NONBLOCK)
		if (-1 == (flags = fcntl(fd, F_GETFL, 0)))
			flags = 0;
		return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	#else
		flags = 1;
		return ioctl(fd, FIONBIO, &flags);
	#endif

}
class ThreadSafeQueue{
private:
	std::queue<int>Queue;
public:
	void push(int fd);
	int pop(int thN);
	bool empty();
};
void ThreadSafeQueue::push(int fd){
	std::unique_lock<std::mutex>lk(mtx);
	std::cout << "fd#" << fd << ". Push to the Queue\n";
	Queue.push(fd);
}
int ThreadSafeQueue::pop(int thN){
	std::unique_lock<std::mutex>lk(mtx);
	int next;
	if (!Queue.empty()){					//если очередь сокетов не пустая, то извлекаем сокет
		next = Queue.front();
		std::cout << "fd#" << next << ". Pop from the Queue\n";
		Queue.pop();
	}
	else{ 
		next = -1;							//если очередь пустая, то ждём условной переменной
	//	printf("thread#%d. Waiting...\n", thN);
		cv_start_threads.wait(lk);
	//	printf("thread#%d. Working.\n", thN);
	}
	return next;
}
bool ThreadSafeQueue::empty(){
	return Queue.empty();
}

static ThreadSafeQueue SocketQueue;
void RequestHandle(int thN){
	while(true){
		int next_fd = SocketQueue.pop(thN);
		if(next_fd == -1)
			continue;
		char buf[BUFSIZE];
		ssize_t RecvResult = recv(next_fd, buf, BUFSIZE, MSG_NOSIGNAL);
		if (RecvResult == 0 && errno != EAGAIN)
		{
			shutdown(next_fd, SHUT_RDWR);
			close(next_fd);
			std::cout << "socket#" << next_fd << " shutdown.\n";
		}
		else if (RecvResult == -1){
			printf("recv=-1. errno=%d\n", errno);
		} else if(RecvResult > 0){
			std::cout << "thread#" << thN << ". Data transfer...\n";
			std::cout << "RecvResult=" << RecvResult << "\n";
			std::string request(buf, RecvResult);
		    std::smatch re_result;										//список совпадений запроса регулярного выражения.
		    std::string answer;											//строка ответа на запрос
		    //ищем параметры запроса в строке запроса
			std::regex re("(GET) /(.+[.]html)");
			std::regex_search(request, re_result, re);
			std::fstream file;
			if(re_result.size() != 3 || re_result[1] != "GET" || !(file.open(re_result[2], std::ios::in), file.is_open())){
				answer = "HTTP/1.0 404 NOT FOUND\r\n"
							 "Content-length: 0\r\n"
							 "Content-Type: text/html\r\n\r\n";
			} else{
				file.seekg(0, std::ios_base::end);
				answer = "HTTP/1.0 200 OK\r\nContent-length: " + std::to_string(file.tellg()) + "\r\n"
						 + "Connection: close\r\n"
						 + "Content-Type: text/html\r\n\r\n";
				char buf;
				file.seekg(0, std::ios_base::beg);
				while((buf = file.get()) != EOF){
					answer += buf;
				}
			}	
			send(next_fd, answer.c_str(), answer.size(), MSG_NOSIGNAL);
			std::cout << "thread#" << thN << ". Transfer finished.\n";
		}
	}
}



int main(int argc, char** argv)
{
	//парсим входные аргументы
	int opt;
	std::string ip;
	std::string directory;
	std::string port;
//	std::cout << argc << "\n";
	while ((opt = getopt(argc, argv, "h:p:d:")) != -1){
		assert((opt == 'h' || opt == 'p' || opt == 'd') && "getopt return error");
		switch (opt){
			case 'h':
				ip = optarg;
				std::cout << "ip=" << ip << "\n";
				break;
			case 'p':
				port = optarg;
				std::cout << "port=" << port << "\n";
				break;
			case 'd':
				directory = std::string(".") + optarg;
				std::cout << "directory=" << directory << "\n";
				break;
			default:
				printf("option error: %c\n", opt);
				exit(EXIT_FAILURE);
		}
	}
	if(-1 == daemon(1,0)){
		std::cout << "daemon error";
	}
	//меняем рабочую директорию
	if (-1 == chdir(directory.c_str())){
		std::cout << "chdir_error";
	}

	
	//создаем мастер-сокет
	int MasterSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
//	std::set<int> SlaveSokets;
	sockaddr_in SockAddr;
	SockAddr.sin_family = AF_INET;
	SockAddr.sin_port = htons(atoi(port.c_str()));
	SockAddr.sin_addr.s_addr = inet_addr(ip.c_str());
	
	//биндим мастерсокет и запускаем слушать.
	bind(MasterSocket, (sockaddr*)(&SockAddr), sizeof(SockAddr));
	set_nonblock(MasterSocket);		/*делаем сокет неблокирующим, чтобы подвесить accept под select,
	потому что иначе не сможем нормально делать accept*/
	listen(MasterSocket, SOMAXCONN);

	//организуем epoll
	int EPoll = epoll_create1(0);	//создаем дескриптор в ядре ОС
	epoll_event Event;				//создаем структуру для хранения событий дескриптора
	Event.data.fd = MasterSocket;	//присваиваем сокет
	Event.events = EPOLLIN;			//установка режима регистрации событий - доступность на чтение
	
	//организуем условную переменную

/*
	pthread_t workers[NTHREADS];
	pthread_attr_t attr;			//структура установки атрибутов при создании потока.
	pthread_attr_init(&attr)		//инициализация структуры pthread_attr_t
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);	//установка типа потока в струтуру
	for(int i = 0; i < NTHREADS; ++i)
		pthread_create(worker + i, &attr, RequestHandle, NULL);
*/
	std::vector<std::thread> threads;
	for(int i = 0; i < NTHREADS; ++i)
		threads.emplace_back(std::bind(RequestHandle, i));

	epoll_ctl(EPoll, EPOLL_CTL_ADD, MasterSocket, &Event);  //EPOLL_CTL_ADD - константа для добавления дескриптора в Epoll в ядре ОС.
	
	while (true)
	{
		epoll_event Events[MAX_EVENTS];		//массив структур для хранения A числа событий
		int N = epoll_wait(EPoll, Events, MAX_EVENTS, -1);		//N - возвращает число событий
		if (N == -1)
			continue;
		for (int i = 0; i < N; i++)
		{
			std::cout << "Event in fd#" << Events[i].data.fd << "\n";
			if(Events[i].data.fd == MasterSocket){		//если событие происходит в Мастерсокете, то добавляем SlaveSocket в Epoll
				int SlaveSocket = accept(MasterSocket, 0, 0);
				std::cout << "fd#" << SlaveSocket << ". accept\n";
				set_nonblock(SlaveSocket);		//
				epoll_event Event;				//создаем структуру для хранения событий дескриптора
				Event.data.fd = SlaveSocket;	//присваиваем сокет
				Event.events = EPOLLIN;			//установка режима регистрации событий - доступность на чтение
				epoll_ctl(EPoll, EPOLL_CTL_ADD, SlaveSocket, &Event);  //EPOLL_CTL_ADD - константа для добавления дескриптора в Epoll в ядре ОС.
			} else{								//если событие в SlaveSocket, то добавляем его в очередь сокетов
				SocketQueue.push(Events[i].data.fd);
			}
		}
		std::cout << "events_pushed\n";
		while(!SocketQueue.empty()){
			std::unique_lock<std::mutex> lk(mtx);
			cv_start_threads.notify_all();
	//		std::cout << "notify_all\n";
		}
	}
	for(int i = 0; i < NTHREADS; ++i)
		threads[i].join();
	return 0;
}