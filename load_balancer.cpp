// ======================= TIME-TRAVEL DEBUGGER - LOAD BALANCER TEMPLATE =======================

// What load balancer does:
//   1. Many clients can connect at the same time. Each one sends one string (the source code). 
//   2. Clients wait in a queue (first come, first served).
//   3. For each client, in order: make sure the server is running (start it with system() if not),
//      pass the string to the server, pass the server's reply back to the client.
//
// It does not read or understand the messages. It only moves bytes.
// The only messages it writes itself are errors ("server busy", "server failed to start").
//
// Two threads:
//   acceptor thread (acceptLoop) -> takes new clients and puts them in the queue. It never waits for the server.
//   main thread (dispatcherLoop) -> takes ONE client out of the queue and serves it completely, then the next.
//
// How the server is started:
//   system("./server 5001 &");
//   The "&" is required. Without it, system() waits until the server exits and the load balancer freezes.
//
//
// What the server program must do (the load balancer depends on this):
//   - it takes its port as the first argument, for example: ./server 5001
//   - it reads the string until the connection's write side is closed (recv returns 0)
//   - it sends its reply as [status: 1 byte][payload], then closes the connection 


#include <iostream>
#include <string>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <stdexcept>
#include <sys/time.h>
#include "helper.h"
using namespace std;


// ---- Constants ----
const char *SERVER_PATH = "./server";      // the program we start when the server is down
const int MAX_PENDING_CLIENTS = 8;         // clients allowed to wait in our queue. More than this get "server busy".
const int STARTUP_MAX_ATTEMPTS = 50;       // after starting the server, how many times we try to connect
const int STARTUP_POLL_INTERVAL_MS = 100;  // wait between those tries (50 x 100 ms = 5 seconds in total)
const int CLIENT_TIMEOUT_SEC = 30;  
int lbPort = 5000;
string serverHost = "127.0.0.1";
int serverPort = 5001;


// ---- Queue: here it holds client sockets (int). ----
// Implement it yourself, do not use std::queue.
template <typename T>
class Queue
{
    struct Node
    {
        T data;
        Node *next;
    };
    Node *front, *rear;
    int count;


public:
    // Implement these functions:
    Queue()
    {
        count=0;
        front=rear=nullptr;

    }
    ~Queue()
    {
        Node * temp=front;
        while(temp!=nullptr){
            front=front->next;
            delete temp;
            temp=front;

        }

    }
    void enqueue(const T &val)
    {
        Node * n=new Node{val,nullptr};
        if(front==nullptr){
            front=rear=n;
            count++;
            return;
        }
        rear->next=n;
        rear=n;
        count++;

    }
    T dequeue()
    {
        if(isEmpty()){
            throw underflow_error("Queue is Empty!\n");
        }
        T val=front->data;
        if(front==rear){
            delete front;
            count--;
            front=nullptr;
            rear=nullptr;
            return val;
        }
        Node * temp=front;
        front=front->next;
        delete temp;
        count--;
        return val;

    }
    bool isEmpty()
    {
        return count==0;
        
    }
    int size()
    {
        return count;

    }
};

struct ClientInfo
{
    int sock;           // the connected socket
    string address;     // the client's IP as text, e.g. "127.0.0.1"
    int port;           // the client's port
    time_t arrivedAt;   // when accept() returned
};

// The queue is used by two threads, so every use of it must be inside queueMutex.
Queue<ClientInfo> pendingClients;
mutex queueMutex;
sig_atomic_t flag = 1;


// ---- Basic socket helpers ----

bool sendError(int sock, const string &message)
{
    // Reply format: [status: 1 byte][payload until close][checksum: 4 bytes].
    // For an error: send one byte with value 1, then the message text. 
    CrcBuilder checksumBuilder;
    char status = STATUS_ERR;
    checksumBuilder.add(&status, 1);
    checksumBuilder.add(message.data(), message.size());

    if(!sendAll(sock, &status, 1)){
        return false;
    }
    if(!sendAll(sock, message.data(), message.size())){
        return false;
    }
    return checksumBuilder.sendTrailer(sock);
}
// int createListenSocket(int port)
// { 
//     // Create a TCP socket with SOCK_CLOEXEC(flag).
//     // set SO_REUSEADDR so we can restart the load balancer right away.
//     // bind() to the port on any address, then listen(LISTEN_BACKLOG).
//     // If bind or listen fails, print the error and exit, because without this socket we cannot work.
//     int sock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
//     if(sock < 0){
//         perror("socket");
//         exit(1);
//     }
//     int reuseAddress = 1;
//     setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuseAddress, sizeof(reuseAddress));

//     sockaddr_in serverAddress{};
//     serverAddress.sin_family = AF_INET;
//     serverAddress.sin_addr.s_addr = htonl(INADDR_ANY);
//     serverAddress.sin_port = htons(port);

//     if(bind(sock, (sockaddr*)&serverAddress, sizeof(serverAddress)) < 0){
//         perror("bind");
//         exit(1);
//     }
//     if(listen(sock, LISTEN_BACKLOG) < 0){
//         perror("listen");
//         exit(1);
//     }
//     return sock;

// }
int connectToServer(const char *host, int port)
{
    // Create a TCP socket (SOCK_CLOEXEC) and connect it to host:port.
    // Return the socket, or -1 if the connection fails 
    int sock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if(sock< 0){
        return -1;
    }
    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(port);

    if(inet_pton(AF_INET, host, &serverAddress.sin_addr) != 1 ||
       connect(sock, (sockaddr *)&serverAddress, sizeof(serverAddress)) < 0){
        close(sock);
        return -1;
    }
    return sock;
}

// =============================================================================================
// PART 1: WAKING UP THE SERVER IF IT IS DOWN
// =============================================================================================


bool startServer(int port)
{
    // Build the command text: SERVER_PATH + " " + port as text + "&"
    //   Example: "./server 5001 &"
    // Call system() with it.
    // Return true if system() returned 0, otherwise false.
    string command = string(SERVER_PATH) + " " + to_string(port) + " &";
    return system(command.c_str()) == 0;
}
int connectToServerWithWakeup()
{
    // 1. Try connectToServer(). If it works, return that socket. The server is alive.
    // 2. Otherwise call startServer(). If it fails, return -1.
    // 3. Try connectToServer() up to STARTUP_MAX_ATTEMPTS times 
    // 4. Return the first working socket, or -1 if the server never came up.
    int sock = connectToServer(serverHost.c_str(), serverPort);
    if(sock >= 0){
        return sock;
    }
    if(!startServer(serverPort)){
        return -1;
    }
    for(int i = 0; i < STARTUP_MAX_ATTEMPTS; i++){
        sock = connectToServer(serverHost.c_str(), serverPort);
        if(sock  >= 0){
            return sock ;
        }
        usleep(STARTUP_POLL_INTERVAL_MS * 1000);
    }
    return -1;
}


// =============================================================================================
// PART 2: PASSING THE STRING AND THE REPLY (one client, called by the main thread only)
// =============================================================================================


void relayOneClient(ClientInfo &c)
{
    // 1. serverSock = connectToServerWithWakeup().
    //    If it is -1: sendError "backend server failed to start", close clientSock, return.
    // 3. Client -> server: repeat recv() from clientSock into a buffer of IO_BUFFER_SIZE and sendAll() it to serverSock.
    //    Stop when recv() returns 0 (the client finished sending) or fails.
    // 4. Call shutdown(serverSock, SHUT_WR), so the server knows the string is complete.
    // 5. Server -> client: repeat recv() from serverSock and sendAll() to clientSock.
    //    Stop when recv() returns 0 (the server sent its whole reply and closed) or fails.
    // 6. Close both sockets.

    int s_sock = connectToServerWithWakeup();

    

    if(s_sock < 0){
    cout << "Backend connection failed" << endl;

    timeval timeout{5, 0};
    setsockopt(c.sock, SOL_SOCKET, SO_RCVTIMEO,
               &timeout, sizeof(timeout));

    char rec_buff[IO_BUFFER_SIZE];

    while(true){
        ssize_t bytes_rec = recv(c.sock, rec_buff,
                                 IO_BUFFER_SIZE, 0);

        if(bytes_rec > 0){
            continue;
        }

        if(bytes_rec < 0 && errno == EINTR){
            continue;
        }

        break;
    }

    bool success = sendError(c.sock, "backend server failed to start");

    if(success){
        cout << "Error message sent" << endl;
    }else{
        cout << "Error message could not be sent" << endl;
    }

    return;
    }




    timeval timeout{5, 0};
    setsockopt(c.sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    timeout.tv_sec = 30;
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    static char receiveBuffer[IO_BUFFER_SIZE];
    static char outputBuffer[IO_BUFFER_SIZE + 4];

    TrailerChecker uploadChecker;   
    CrcBuilder uploadChecksum;        
    bool clientReceiveOk = true;
    bool serverSendOk = true;

    while(true){
        ssize_t bytes_rec = recv(c.sock, receiveBuffer, IO_BUFFER_SIZE, 0);
        if(bytes_rec < 0 && errno == EINTR){
            continue;
        }
        if(bytes_rec < 0){
            clientReceiveOk = false;
            break;
        }
        if(bytes_rec == 0){
            break;   
        }

        size_t body_bytes = uploadChecker.feed(receiveBuffer, bytes_rec, outputBuffer);
        if(body_bytes > 0){
            uploadChecksum.add(outputBuffer, body_bytes);
            if(!sendAll(s_sock, outputBuffer, body_bytes)){
                serverSendOk = false;
                break;
            }
        }
    }

    if(!serverSendOk){
        sendError(c.sock, "backend server failed to start");
        close(s_sock);
        return;
    }
    if(!clientReceiveOk || !uploadChecker.verify()){
        sendError(c.sock, "upload checksum mismatch, request corrupted");
        close(s_sock);
        return;
    }
    if(!uploadChecksum.sendTrailer(s_sock)){
        sendError(c.sock, "backend server failed to start");
        close(s_sock);
        return;
    }
    shutdown(s_sock, SHUT_WR);

    TrailerChecker replyChecker;
    CrcBuilder replyChecksum;
    bool serverReceiveOk = true;
    bool clientSendOk = true;
    size_t totalBytesForwarded = 0;

    while(true){
        ssize_t bytes_rec = recv(s_sock, receiveBuffer, IO_BUFFER_SIZE, 0);
        if(bytes_rec < 0 && errno == EINTR){
            continue;
        }
        if(bytes_rec < 0){
            serverReceiveOk = false;
            break;
        }
        if(bytes_rec == 0){
            break;  
        }

        size_t body_bytes = replyChecker.feed(receiveBuffer, bytes_rec, outputBuffer);
        if(body_bytes > 0){
            replyChecksum.add(outputBuffer, body_bytes);
            totalBytesForwarded += body_bytes;
            if(!sendAll(c.sock, outputBuffer, body_bytes)){
                clientSendOk = false;
                break;
            }
        }
    }

    close(s_sock);

    if(!clientSendOk){
        return;   
    }
    if(totalBytesForwarded == 0){
        sendError(c.sock, "backend server failed to respond");
        return;
    }
    if(serverReceiveOk && replyChecker.verify()){
        replyChecksum.sendTrailer(c.sock);
    }



}


// =============================================================================================
// PART 3: THE TWO THREADS
// =============================================================================================


void acceptLoop(int listenSock)
{
    // Repeat: accept() a new client. 
    //  If pendingClients.size() is already MAX_PENDING_CLIENTS: unlock, sendError "server busy, try again later",
    // else
    // Lock queueMutex.
    //   close the client, and go on.
    //   Otherwise enqueue the client socket.
    // Never wait for the server here, so new clients are always accepted quickly.
    // If accept() fails, print the error and keep going (do not end the thread).
    while(flag){
        sockaddr_in clientAddress{};
        socklen_t addressLength = sizeof(clientAddress);
        int clientSocket = accept4(listenSock, (sockaddr *)&clientAddress, &addressLength, SOCK_CLOEXEC);
        if(clientSocket < 0){
            if(errno != EINTR && flag){
                perror("accept");
            }
            continue;
        }

        ClientInfo clientInfo;
        clientInfo.sock = clientSocket;
        char ipAddress[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddress.sin_addr, ipAddress, sizeof(ipAddress));
        clientInfo.address = ipAddress;
        clientInfo.port = ntohs(clientAddress.sin_port);
        clientInfo.arrivedAt = time(nullptr);

        bool queueFull = false;
        {
            lock_guard<mutex> queueLock(queueMutex);
            if(pendingClients.size() >= MAX_PENDING_CLIENTS){
                queueFull = true;
            }else{
                pendingClients.enqueue(clientInfo);
            }
        }

        if(queueFull){
            sendError(clientSocket, "server busy, try again later");
            shutdown(clientSocket, SHUT_WR);
            char unusedData[1024];
            while(recv(clientSocket, unusedData, sizeof(unusedData), MSG_DONTWAIT) > 0){
            }
            close(clientSocket);
        }
    }
}
void dispatcherLoop()
{
    // This runs in the main thread and never ends.
    // Repeat:
    //   Lock queueMutex, take ONE client out of the queue if there is one, unlock.
    //   If we got a client: call relayOneClient(). The next client is taken out of the queue only after this one is finished.
    while(flag){
        bool hasClient = false;
        ClientInfo clientInfo;
        {
            lock_guard<mutex> queueLock(queueMutex);
            if(!pendingClients.isEmpty()){
                clientInfo = pendingClients.dequeue();
                hasClient = true;
            }
        }

        if(!hasClient){
            usleep(10000);   
            continue;
        }
        if(time(nullptr) - clientInfo.arrivedAt > CLIENT_TIMEOUT_SEC){
            close(clientInfo.sock);
            continue;
        }
        relayOneClient(clientInfo);
        close(clientInfo.sock);
    }

    
}

void handler(int sg) {
    (void)sg;
    flag = 0;
}
int main(int argc, char *argv[])
{

    signal(SIGINT, handler); // whenever ctrl+c will be pressed to exit the program the flag wll be set so insead of using infinite loop you can loop on flag to safely close things and exit
    // 2. listenSock = createListenSocket(LB_PORT).
    // 3. Start a thread that runs acceptLoop(listenSock) and detach it.
    // 4. Call dispatcherLoop().
    if(argc==3){
        serverHost=argv[1];
        serverPort=atoi(argv[2]);
    }
    int listenSock=createListenSocket(lbPort);
    thread(acceptLoop,listenSock).detach();
    dispatcherLoop();
    close(listenSock);
    lock_guard<mutex> lock(queueMutex);
    while(!pendingClients.isEmpty()){
        close(pendingClients.dequeue().sock);
    }
    return 0;
}