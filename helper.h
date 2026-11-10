#pragma once
#include <iostream>
#include <string>
#include <cstdint>
#include<cstring>
#include<cstddef>
#include <cerrno>
#include <sys/socket.h>
using namespace std;


const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int IO_BUFFER_SIZE = 64 * 1024;     // fixed buffer for streaming files to and from the socket

struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
// Helper.h should contain the structs, functions, constants, etc. that are used more than one time in the project. For example, the structs that are used in both server and client, the constants that are used in all three files. 

struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};

inline bool recvAll(int sock, char *buf, size_t n)
{
    // recv() can return fewer bytes than asked for, so keep calling it until exactly n bytes are read.
    // Return false if the connection closes or fails before n bytes arrive.
    size_t bytes_rec=0;
    while(bytes_rec<n){
        ssize_t res=recv(sock,buf+bytes_rec,n-bytes_rec,0);
        if(res<0 && errno ==EINTR){
            continue;
        }
        if(res<=0){
            return false;
        }
        bytes_rec+=res;
    }
    return true;
}
inline bool sendAll(int sock, const char *buf, size_t n)
{
    // send() can send fewer bytes than asked for, so keep calling it until all n bytes are sent.
    // Use the flag MSG_NOSIGNAL. Return false if the connection fails.
    size_t bytes_sent=0;
    while(bytes_sent< n){
        ssize_t res=send(sock,buf+bytes_sent,n-bytes_sent,MSG_NOSIGNAL);
        if(res<0 && errno==EINTR){
            continue;
        }
        if(res<=0){
            return false;
        }
        bytes_sent+=res;

    }
    return true;
}

inline uint32_t crc32_update(uint32_t checksum,const unsigned char * p,size_t n){
    for(size_t i=0;i<n;i++){
        checksum ^=p[i];

        for(int j=0;j<8;j++){
            if(checksum & 1){
                checksum=(checksum>>1) ^ 0xEDB88320u;
            }else{
                checksum=checksum>>1;
            }

        }
    }
    return checksum;

}


inline void putTrailer(char trailer[4],uint32_t checksum){
    trailer[0]=(char)(checksum>>24);
    trailer[1]=(char)(checksum>>16);
    trailer[2]=(char)(checksum>>8);
    trailer[3]=(char)checksum;
}

inline uint32_t getTrailer(const char trailer[4]){
    return ((uint32_t)(unsigned char)trailer[0] <<24) | ((uint32_t)(unsigned char)trailer[1] <<16 )| 
    ((uint32_t)(unsigned char)trailer[2] <<8) | ((uint32_t)(unsigned char)trailer[3]);
}

class CrcBuilder{
    uint32_t checksum= 0xFFFFFFFFu;

    public:
    void add(const char * data,size_t n){
        checksum=crc32_update(checksum,(const unsigned char *)data,n);
    }

    uint32_t finish(){
        return checksum ^ 0xFFFFFFFFu;
    }

    bool sendTrailer(int sock){
        char trailer[4];
        putTrailer(trailer,finish());
        return sendAll(sock,trailer,4);
    }
};

class TrailerChecker{
    char lastFourBytes[4];
    size_t bytes_held=0;
    uint32_t checksum= 0xFFFFFFFFu;

    public:
    size_t feed(const char* rec_data,size_t rec_s,char* output){
        size_t n=bytes_held + rec_s;
        if(n<=4){
            memcpy(lastFourBytes+bytes_held,rec_data,rec_s);
            bytes_held=n;
            return 0;
        }
        memcpy(output,lastFourBytes,bytes_held);
        memcpy(output+bytes_held,rec_data,rec_s);
        size_t body_bytes=n-4;
        checksum=crc32_update(checksum,(const unsigned char*)output,body_bytes);
        memcpy(lastFourBytes,output+body_bytes,4);
        bytes_held=4;
        return body_bytes;


    }
    bool  verify(){
        return bytes_held==4 && getTrailer(lastFourBytes)==(checksum ^ 0xFFFFFFFFu);
    }

    uint32_t trailerValue(){
        return getTrailer(lastFourBytes);
    }

    uint32_t compute_crc(){
        return checksum^ 0xFFFFFFFFu;
    }
};