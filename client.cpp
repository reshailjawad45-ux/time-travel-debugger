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
using namespace std;

// ---- Constants ----
const char *PROGRAM_NAME = "ttdb";
const char *PROMPT = "ttdb> ";
const int IO_BUFFER_SIZE = 64 * 1024;     // fixed buffer for streaming files to and from the socket
const char *TDBG_FILE = "ttdb_last.tdbg"; // where the downloaded .tdbg is saved
const int MAX_JUMP = 1000000000;          // biggest n accepted by jmp
string LB_HOST = "127.0.0.0";
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

bool recvAll(int sock, char *buf, size_t n)
{
    // recv() can return fewer bytes than asked for, so keep calling it until exactly n bytes are read.
    // Return false if the connection closes or fails before n bytes arrive.
}
bool sendAll(int sock, const char *buf, size_t n)
{
    // send() can send fewer bytes than asked for, so keep calling it until all n bytes are sent.
    // Use the flag MSG_NOSIGNAL. Return false if the connection fails.
}

int connectToHost(const char *host, int port)
{
    // Create a TCP socket and connect it to host:port.
    // Return the socket, or -1 if the connection fails.
}
bool uploadSource(int sock, const char *sourcePath)
{
    // have to do checksum and then send
    // When the file is finished, call shutdown(sock, SHUT_WR). This is how the server knows the string is complete.
    // Return false if the file cannot be opened or any send fails.
}
bool receiveResponse(int sock, const char *outTdbgPath, string &outError)
{
    // Reply on the wire: [status][payload][CRC-32]. The CRC covers the status byte too.
    // Feed every received byte (status included) to a TrailerChecker.
    // Status 1: collect the payload into outError. Status 0: write the payload to the file.
    // At the end call verify(). If it fails, delete the file, set outError to
    // "checksum mismatch, response corrupted" and return false.
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
        cout<<"FAIL 2: header read\n";
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
   
}
void closeSession(DebugSession &s)
{
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
}
bool moveBy(DebugSession &s, int32_t delta)
{
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
}
bool cmdNext(DebugSession &s) {}
bool cmdPrv(DebugSession &s) {}
bool cmdJmpNext(DebugSession &s, int32_t n) {}
bool cmdJmpPrv(DebugSession &s, int32_t n) { }

// =============================================================================================
// PART E: PRINTING
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s)
{
}
void printHelp()
{
    // Print the commands
}

// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================

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

        TdbgReader r;
    if (!openTdbg("session.tdbg", r)) { cout << "damaged\n"; return 1; }

    int32_t steps[2] = {1, 4};
    for (int k = 0; k < 2; k++)
    {
        Snapshot *s = readSnapshotAt(r, steps[k]);
        if (!s) { cout << "nullptr\n"; continue; }
        cout << "step index " << steps[k] << ", depth " << s->stackDepth << endl;
        for (int i = 0; i < s->stackDepth; i++)
        {
            Frame &f = s->callStack[i];
            cout << "  " << f.func_name << ": ";
            for (int a = 0; a < f.argc; a++) cout << f.argv[a].name << "=" << f.argv[a].value << " ";
            for (int l = 0; l < f.localCount; l++) cout << f.locals[l].name << "=" << f.locals[l].value << " ";
            cout << endl;
        }
        delete s;
    }
    cout << (readSnapshotAt(r, 99) == nullptr ? "step 99: nullptr (correct)" : "step 99: WRONG") << endl;
    closeTdbg(r);
}
