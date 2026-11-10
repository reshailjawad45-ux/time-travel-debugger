// ======================= TIME-TRAVEL DEBUGGER - CLIENT TEMPLATE =======================

// The client never talks to the server directly. It only talks to the load balancer.
// The load balancer wakes the server up if it is down, so the client does not care.
// The first "start" may just take a bit longer.
//
// The network is used ONLY by "start". After the .tdbg file is downloaded,
// next / prv / jmp read that file on the local disk. They never contact the load balancer.
//
// How the pieces fit together:
//   main loop -> readCommand -> runCommand
//   start -> cmdStart()  -> requestDebugFile() (Part A) -> openTdbg() (Part B) -> shows step 1
//   next, prv, jmp -> moveBy() (Part D) -> readSnapshotAt() (Part B) -> printSnapshot() (Part E)

#include <iostream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <vector>
#include <sys/socket.h>
#include "helper.h"
#include<netinet/in.h>
#include<arpa/inet.h>
#include <sys/time.h>
#include <cerrno>
using namespace std;

// ---- Constants ----
const char *PROGRAM_NAME = "ttdb";
const char *PROMPT = "ttdb> ";
const char *TDBG_FILE = "ttdb_last.tdbg"; // where the downloaded .tdbg is saved
const int MAX_JUMP = 1000000000;          // biggest n accepted by jmp
string LB_HOST = "127.0.0.1";
int LB_PORT = 5000;

// These two must be the same as in Phase 1


// ---- Structs (same as Phase 1, so the file can be read back exactly as it was written) ----

// ---- Structs only the client needs ----
struct TdbgReader
{
    FILE * file=nullptr; ///changed the index her   ecuz iknow how to read with the file
    TTDBHeader header;
    vector<int64_t> index;
};
struct DebugSession
{
    int active_flag;     // false until a "start" has worked
    TdbgReader reader;   // the .tdbg file stays open for the whole session
    int64_t currentStep; 
};

// =============================================================================================
// PART A: TALKING TO THE LOAD BALANCER
// =============================================================================================




int connectToHost(const char *host, int port)
{
    // Create a TCP socket and connect it to host:port.
    // Return the socket, or -1 if the connection fails.
    int sock=socket(AF_INET,SOCK_STREAM,0);
    if(sock <0){
        return -1;
    }
    timeval timeout{30,0};
    setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));

    sockaddr_in serverAddress{};
    serverAddress.sin_family=AF_INET;
    serverAddress.sin_port=htons(port);
    if((inet_pton(AF_INET,host,&serverAddress.sin_addr)!=1)|| connect(sock,(sockaddr *)&serverAddress,sizeof(serverAddress))<0){
        close(sock);
        return -1;
    }
    return sock;

}
bool uploadSource(int sock, const char *sourcePath)
{
    // have to do checksum and then send
    // When the file is finished, call shutdown(sock, SHUT_WR). This is how the server knows the string is complete.
    // Return false if the file cannot be opened or any send fails.
    FILE *src_f=fopen(sourcePath,"rb");
    if(!src_f){
        return false;
    }

    CrcBuilder checksumbuilder;
    char status=STATUS_OK;
    bool success=sendAll(sock,&status,1);
    checksumbuilder.add(&status,1);
    static char buffer[IO_BUFFER_SIZE];
    size_t bytesread;

    while(success && (bytesread=fread(buffer,1,IO_BUFFER_SIZE,src_f))>0){
        checksumbuilder.add(buffer,bytesread);
        success=sendAll(sock,buffer,bytesread);
    }
    fclose(src_f);
    if(!success){
        return false;
    }
    if(!checksumbuilder.sendTrailer(sock)){
        return false;
    }
    shutdown(sock,SHUT_WR);
    return true;
}
bool receiveResponse(int sock, const char *outTdbgPath, string &outError)
{
    // Reply on the wire: [status][payload][CRC-32]. The CRC covers the status byte too.
    // Feed every received byte (status included) to a TrailerChecker.
    // Status 1: collect the payload into outError. Status 0: write the payload to the file.
    // At the end call verify(). If it fails, delete the file, set outError to
    // "checksum mismatch, response corrupted" and return false.

    TrailerChecker tc;
    static char rec_buff[IO_BUFFER_SIZE];
    static char out_buff[IO_BUFFER_SIZE+4];

    FILE * fout=nullptr;
    int status=-1;
    bool rec_failed=false;
    string err_msg;

    while(true){
        ssize_t bytes_rec =recv(sock,rec_buff,IO_BUFFER_SIZE,0);
        if(bytes_rec<0 && errno ==EINTR){
            continue;
        }
        if(bytes_rec<0){
            rec_failed=true;
            break;
        }
        if(bytes_rec==0){
            break;
        }

        size_t body_bytes=tc.feed(rec_buff,bytes_rec,out_buff);

        size_t dataoffset=0;
        if(body_bytes>0 && status==-1){
            status =(unsigned char)out_buff[0];
            dataoffset=1;
            if(status==0){
                fout=fopen(outTdbgPath,"wb");
                if(!fout){
                    outError="cannot create file\n";
                    return false;
                }
            }
        }
        if(body_bytes>dataoffset){
            if(status==0){
                fwrite(out_buff+dataoffset,1,body_bytes-dataoffset,fout);
            }else{
                err_msg.append(out_buff+dataoffset,body_bytes-dataoffset);
            }
        }

    }
    if(fout){
        fclose(fout);

    }
    if(rec_failed || status==-1){
        remove(outTdbgPath);
        outError="no valid reply from server";
        return false;
    }

    if(!tc.verify() || (status !=0 && status !=1)){
        remove(outTdbgPath);
        outError="checksum mismatch,response corrupted";
        return false;
    }
    if(status==1){
        outError=err_msg;
        return false;
    }

    return true;
}

bool requestDebugFile(const char * h,int port_no,const char * src,const char * out_p,string & err){
    FILE * src_f=fopen(src,"rb");
    if(!src_f){
        err=string("cannot open\n")+src;
        return false;
    }
    fclose(src_f);
    int sock=connectToHost(h,port_no);
    if(sock<0 ){
        err="cannot connect to load balancer";
        return false;
    }
    if(!uploadSource(sock,src)){
        close(sock);
        err="upload failed";
        return false;
    }

    bool success=receiveResponse(sock,out_p,err);
    close(sock);
    return success;
}

// =============================================================================================
// PART B: READING THE .tdbg FILE
// File layout (written by Phase 1):
//   header | snapshot 0 | snapshot 1 | ... | index (one 8 byte offset per snapshot)
// The index lets us jump straight to any step without reading the steps before it.
// =============================================================================================


bool openTdbg(const char *path, TdbgReader &r)
{
    FILE * f=fopen(path,"rb");

    if(!f){
        return false;

    }
    TTDBHeader h;
    if(fread(h.magic,1,4,f)!=4|| 
       fread(&h.version,sizeof(int32_t),1,f)!=1||
       fread(&h.stepCount,sizeof(int32_t),1,f)!=1||
       fread(&h.indexOffset,sizeof(int64_t),1,f)!=1){
        fclose(f);
        return false;

    }
    fseek(f,0,SEEK_END);
    int64_t file_s=ftell(f);

    if(memcmp(h.magic,"TTDB",4)!=0 || h.version!=1 || h.stepCount<=0 ||
       h.indexOffset<20 ||
       h.indexOffset+(int64_t)h.stepCount*8 != file_s){
        fclose(f);
        return false;
    }

    r.index.resize(h.stepCount);
    fseek(f,h.indexOffset,SEEK_SET);
    if(fread(r.index.data(),sizeof(int64_t),h.stepCount,f)!=(size_t)h.stepCount){
        fclose(f);
        return false;
    }
    r.file=f;
    r.header=h;
    return true;

}
void closeTdbg(TdbgReader &r)
{
    if(r.file){
        fclose(r.file);
        r.file=nullptr;
        r.index.clear();
    }
}
bool read_str(FILE * f,string &output){
    int32_t l;
    if(fread(&l,sizeof(int32_t),1,f)!=1){
        return false;
    }
    if(l<0|| l> 1024){
        return false;
    }
    output.resize(l);
    if(l>0 && fread(&output[0],1,l,f)!=(size_t)l){
        return false;
    }
    return true;
}

bool read_var(FILE * f,Variable &var){
    return read_str(f,var.name) && fread(&var.value,sizeof(int32_t),1,f)==1;
}
bool read_frame(FILE * f,Frame &fr){
    if(!read_str(f,fr.func_name)){
        return false;
    }
    if(fread(&fr.argc,sizeof(int32_t),1,f)!=1){
        return false;
    }
    if(fr.argc<0 || fr.argc>MAX_VARS_PER_FRAME){
        return false;
    }
    for(int i=0;i<fr.argc;i++){
        if(!read_var(f,fr.argv[i])){
            return false;
        }
    }

    if(fread(&fr.returnLine,sizeof(int32_t),1,f)!=1){
        return false;
    }
    if(fread(&fr.localCount,sizeof(int32_t),1,f)!=1){
        return false;
    }
    if(fr.localCount<0 || fr.localCount>MAX_VARS_PER_FRAME){
        return false;
    }
    for(int i=0;i<fr.localCount;i++){
        if(!read_var(f,fr.locals[i])){
            return false;
        }
    }

    return true;
}
Snapshot *readSnapshotAt(TdbgReader &r, int32_t step)
{
    if(step<0 || step >=r.header.stepCount){
        return nullptr;
    }

    FILE * f=r.file;
    if(fseek(f,r.index[step],SEEK_SET)!=0){
        return nullptr;
    }

    Snapshot* snap=new Snapshot;
    if(fread(&snap->stackDepth,sizeof(int32_t),1,f)!=1|| snap->stackDepth<0 || snap->stackDepth> MAX_STACK_DEPTH){
        delete snap;
        return nullptr;
    }

    for(int i=0;i<snap->stackDepth;i++){
        if(!read_frame(f,snap->callStack[i])){
            delete snap;
            return nullptr;
        }
    }
    return snap;


}

// =============================================================================================
// PART C: THE SESSION IN MEMORY
// There is one DebugSession variable, created in main() and passed to every command by reference.
// =============================================================================================

void initSession(DebugSession &s)
{
    s.active_flag=0;
    s.reader.file=nullptr;
    s.currentStep=0;
   
}
void closeSession(DebugSession &s)
{
    if(s.active_flag){
        closeTdbg(s.reader);
    }
    s.active_flag=0;
    // If active, call closeTdbg(s.reader). Then set active = false.
    // Called when the user types quit and when the input ends.
}

// =============================================================================================
// PART D: THE COMMANDS
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s); // defined in Part E

bool showCurrent(DebugSession &s)
{
    // Read the snapshot at s.currentStep, call printSnapshot, delete the snapshot.
    // If readSnapshotAt returns nullptr, print "damaged .tdbg file" and return false.
    Snapshot* snap=readSnapshotAt(s.reader,(int32_t)s.currentStep);
    if(snap==nullptr){
        cout <<"damaged .tdbg file\n";
        return false;
    }
    printSnapshot((int32_t)s.currentStep+1,s.reader.header.stepCount,*snap);
    delete snap;
    return true;
}
bool moveBy(DebugSession &s, int32_t delta)
{
    if(s.active_flag==0){
        cout<<"No session active.\n";
        return false;
    }
    int64_t tar=s.currentStep+delta;
    int64_t end_step=s.reader.header.stepCount-1;

    if(tar<0 || tar>end_step){
        cout <<"You cannot move there,steps( 1 -"<<s.reader.header.stepCount<<" )  current Step:"<<s.currentStep+1<<endl;
        return false;
    }

    int64_t curr=s.currentStep;
    s.currentStep=tar;
    if(!showCurrent(s)){
        s.currentStep=curr;
        return false;
    }

    return true;
}
bool cmdStart(DebugSession &s, const string &sourcePath)
{

    // 1. requestDebugFile with host and port, saving to TDBG_FILE.
    // 2. If it fails, print the error text and return false. Keep the old session as it was.
    // 3. Open the new file with openTdbg into a NEW local TdbgReader.
    //    If that fails, print "server sent a damaged .tdbg file", keep the old session, return false.
    // 4. Only now close the old reader (if s.active), then put the new reader into s,
    //    set s.currentStep = 0 and s.active = true.
    // 5. Show step 1 with showCurrent(s).

    string tmp=string(TDBG_FILE)+".tmp";
    string err;
    if(!requestDebugFile(LB_HOST.c_str(),LB_PORT,sourcePath.c_str(),tmp.c_str(),err)){
        cout <<"ERROR: "<<err<<endl;
        return false;

    }
    TdbgReader new_r;
    if(!openTdbg(tmp.c_str(),new_r)){
        remove(tmp.c_str());
        cout <<"ERROR: server sent a damaged .tdbg file\n";
        return false;
    }

    if(s.active_flag==1){
        closeTdbg(s.reader);
    }
    rename(tmp.c_str(),TDBG_FILE);
    s.reader=new_r;
    s.currentStep=0;
    s.active_flag=1;
    return showCurrent(s);

}
bool cmdNext(DebugSession &s) {
    return moveBy(s,1);
}
bool cmdPrv(DebugSession &s) {
    return moveBy(s,-1);
}
bool cmdJmpNext(DebugSession &s, int32_t n) {
    return moveBy(s,n);
}
bool cmdJmpPrv(DebugSession &s, int32_t n) { 
    return moveBy(s,-n);
}

// =============================================================================================
// PART E: PRINTING
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s)
{
    cout <<"Step: ["<<step<<":"<<totalSteps<<"]\n";
    if(s.stackDepth==0){
        cout <<"Programe Ends!\n";
        return;
    }
    for(int i=0;i<s.stackDepth;i++){
        const Frame &curr=s.callStack[i];
        cout <<" "<<curr.func_name<<" : ";
        for(int j=0;j<curr.argc;j++){
            cout <<" "<<curr.argv[j].name <<"="<<curr.argv[j].value;

        }
        
        for(int j=0;j<curr.localCount;j++){
            cout <<" "<<curr.locals[j].name <<"="<<curr.locals[j].value;

        }
        cout <<endl;
    }
}
void printHelp()
{
    cout <<"Commands:\n";
    cout <<" - start <file>\n";
    cout <<" - next\n";
    cout <<" - prv\n";
    cout <<" - jmp next <n>\n";
    cout <<" - jmp prv <n>\n";
    cout <<" - help\n";
    cout <<" - quit || exit\n";

    // Print the commands
}

// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================

bool check_postive_num(const string &word,int32_t & num){
    if(word.empty()){
        return false;
    }
    for(int i=0;i<word.size();i++){
        if(word[i]<'0' || word[i]>'9'){
            return false;
        }
    }
    if(word.size() > 10){ 
        return false;
    }
    long long n = stoll(word);
    if(n < 1 || n > MAX_JUMP){
        return  false;
    }
    num = (int32_t)n;
    return true;
}
bool runCommand(DebugSession &s, const string &line, bool &quitRequested)
{
    // 1. Split the line into words.
    // 2. If the first word is "ttdb", drop it.
    // 3. Match the first word:
    //      start      -> needs a path after it, then cmdStart
    //      next       -> no more words, then cmdNext
    //      prv        -> no more words, then cmdPrv
    //      jmp        -> second word "next" or "prv", third word a positive number ,
    //                    then cmdJmpNext or cmdJmpPrv
    //      help       -> printHelp
    //      quit, exit -> set quitRequested = true
    // 4. For anything else, or a wrong number of words, print "unknown command" and the help text, and return false.

    vector<string> words;
    string word="";
    for(size_t i=0;i<line.size();i++){
        if(line[i]==' ' || line[i]=='\t'){
            if(word!=""){
                words.push_back(word);
                word="";
            }
        }else{
            word=word+line[i];
        }
    }
    if(word!=""){
        words.push_back(word);
    }

    if(words.size()!=0 && words[0]==PROGRAM_NAME){
        words.erase(words.begin());
    }
    if(words.empty()){
        return true;
    }
    if(words.size()==2 && words[0]=="start"){
        return cmdStart(s,words[1]);
    }
    if(words.size()==1 && words[0]=="next"){
        return cmdNext(s);
    }
    if(words.size()==1 && words[0]=="prv"){
        return cmdPrv(s);
    }
    if(words.size()==3 && words[0]=="jmp" && words[1]=="next"){
        int32_t n;
        if(check_postive_num(words[2],n)){
            return cmdJmpNext(s,n);
        }
        
    }
    if(words.size()==3 && words[0]=="jmp" && words[1]=="prv"){
        int32_t n;
        if(check_postive_num(words[2],n)){
            return cmdJmpPrv(s,n);
        }
        
    }
    if(words.size()==1 &&words[0]=="help"){
        printHelp();
        return true;
    }
    if(words.size()==1 &&(words[0]=="quit" || words[0]=="exit")){
        quitRequested=true;
        return true;
    }
    cout <<"Unknown Command!\n";
    printHelp();
    return false;


}




int main(int argc, char *argv[])
{
    // you get string LB_HOST and int LB_PORT = 0 in arguments of this main function. 

    // DebugSession session; initSession(session);
    // Print a short welcome and the help text.
    //
    // Infinite loop:
    //   if quitRequested, break
    //
    // After the loop: closeSession(session) and return 0.
    // Never exit the loop because of an error. Only quit or the end of the input leaves it.
    if(argc==3){
        LB_HOST=argv[1];
        LB_PORT=atoi(argv[2]);
    }

    DebugSession s;
    initSession(s);
    cout <<"\n===TIME TRAVERLER DEBUGGER===\n";
    printHelp();

    bool quitRequested=false;
    string line;
    while(quitRequested==false){
        cout <<PROMPT<<flush;
        if(!getline(cin,line)){
            break;

        }
        runCommand(s,line,quitRequested);
    }
    closeSession(s);
    return  0;

}
