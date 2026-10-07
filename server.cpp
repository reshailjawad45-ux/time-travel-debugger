// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)


#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <unistd.h>
#include <sys/socket.h>
#include <cstdint>
#include <cstdio>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node *next;
    };
    Node *top;
    int32_t count;

public:
    // Implement these functions:
    Stack()
    { 
        top=nullptr;
        count=0;
    }
    void push(const T &val)
    {
        if(count==MAX_STACK_DEPTH){
            throw overflow_error("The stack limt is reached.\n");
        }
        Node * n=new Node;
        n->data=val;

        n->next=top;
        top=n;
        count++;

        // pushes the value on the stack if max limit is not reached yet.
    }
    T pop()
    {
        if(count==0){
            throw underflow_error("The stack is Empty.\n");
        }
        Node *n=top->next;
        T val=top->data;
        delete top;
        top=n;
        count--;
        return val;
        // pop the top value on the stack
    }
    T &peek()
    {
        if(count==0){
            throw underflow_error("The stack is Empty.\n");
        }
        return top->data;

        // returns the top value on the stack
    }
    bool isEmpty()
    {
        return count==0;
    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        int ct_out=0;
        Node * temp=top;
        while(temp!=nullptr && ct_out <maxLen){
            out[ct_out]=temp->data;
            ct_out++;
            temp=temp->next;
        }
        return ct_out;
        // copies every frame, top to bottom in the array given as a parameter
        // this is what buildSnapshot() call, returns count written
    }
};


// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot *data;
    TimelineNode *next;
    TimelineNode *prev;
};
class Timeline
{
    TimelineNode *head, *tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline()
    {
        head=tail=nullptr;
        stepCount=0;
    }
    void record(Snapshot *s)
    {
        TimelineNode * node=new TimelineNode;
        node->data=s;
        node->next=nullptr;
        node->prev=nullptr;

        if(head==nullptr){
            head=tail=node;
            stepCount++;
            return;
        }
        tail->next=node;
        node->prev=tail;
        tail=tail->next;
        stepCount++;

        // add record in the timeline
    }
    TimelineNode *begin()
    {
        return head;
    }
    int32_t getStepCount()
    {
        return stepCount;
    }
};

// Core structs
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
void writeHeader(FILE *f, const TTDBHeader &h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);

    // placeholder for other two data members
}

// resolve.bin - bookkeeping
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



// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream &in, string &out)
{
    string newline;
    while(getline(in,newline)){
        bool flag=true;
        //used for checking if line is blank or not

        for(int i=0;i<newline.length();i++){
            if(newline[i]!=' ' && newline[i]!='\t'){
                flag=false;
                break;
            }
        }
        if(!flag){
            out =newline;
            return true;
        }
    }

    return false;
    // reads the next nonblank line
}
string firstWord(const string &line)
{
    string word1="";
   for(int i=0;i<line.length();i++){

    if(line[i]==' ' || line[i]=='\t'){
        break;
    }
    word1+=line[i];
   }

   return  word1;
    // returns first word from the input string
}
string secondWord(const string &line)
{
    string word2="";
    int st_idx=0;
    //skipin irst word
    while(st_idx<line.length() &&line[st_idx] !=' ' && line[st_idx]!='\t'){
        st_idx++;
    }
    //skiping spaces afteer first word if there are
    while(st_idx <line.length() && (line[st_idx]==' ' || line[st_idx]=='\t')){
        st_idx++;
    }

    //readin sec word

    for(int i=st_idx;i<line.length();i++){
        if(line[i]==' ' || line[i]=='\t'){
        break;
        }
        word2+=line[i];
    }

    return word2;
}
bool validateProgram(const char *sourcePath)
{
    ifstream fin(sourcePath);
    if(!fin){
        cout <<"File not found\n";
        return false;
    }
    Stack<string> check;

    string line;
    while(readSourceLine(fin,line)){
        string first_word=firstWord(line);
        if(first_word=="func"){
            if(!check.isEmpty() && check.peek()=="func"){
                cout <<"Error: Nested Function detected.\n";
                return false;
            }
            check.push(first_word);
        }else if(first_word=="func_end"){
            if(check.isEmpty()){
                cout <<"Error: func end without func.\n";
                return false;
            }
            check.pop();
        }
    }
    if(!check.isEmpty()){
            cout <<"Error: Func not closed.";
            return false;
    }
    return true;
    // for each func defined there should be exactly one func_end and no nested funcs allowed - 
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE *f, int64_t offsetField, const string &text)
{
    int64_t record_pos=ftell(f);

    int32_t str_size=text.length();

    fwrite(&offsetField,sizeof(int64_t),1,f);
    fwrite(&str_size,sizeof(int32_t),1,f);
    fwrite(text.c_str(),1,str_size,f);

    return record_pos;

    // writes one [offset(8B)][size(4B)][string] record at the current file position
    // returns this record's own starting byte position
}
int64_t readResolveRecord(FILE *f, string &outText)
{
    int64_t offset;
    int32_t str_size;

    if(fread(&offset,sizeof(int64_t),1,f)!=1){
        return -1;
    }
    if(fread(&str_size,sizeof(int32_t),1,f)!=1){
        return -1;
    }
    outText.resize(str_size);
    fread(&outText[0],sizeof(char),str_size,f);

    return offset;
    // reads one record at the current position and advances past it, returns the offset field - the raw line text comes back untouched in outText.
}
int64_t resolveProgram(const char *sourcePath, const char *resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;

    ifstream fin(sourcePath);
    FILE * fout=fopen(resolveBinPath,"wb");

    if(!fin){
        cout <<"Sorce file not opening\n";
        return -1;
    }

    if(!fout){
        cout <<"File not created";
        return -1;
    }
    string line;

    int64_t offset=0;
    int64_t mainOffset=-1;
    while(readSourceLine(fin,line)){
        int64_t record_pos=writeResolveRecord(fout,offset,line);

        string first_word=firstWord(line);
        if(first_word=="func"){
            if(MAX_FUNCS<funcCount){
                cout <<"Number of function exceeded";
                return-1;
            }
            string secondword=secondWord(line);

            funcArray[funcCount].funcName=secondword;
            funcArray[funcCount].byteOffsetInResolveBin=offset;
            funcCount++;

            if(secondword=="main"){
                mainOffset=offset;
            }
        }
        else if(first_word=="call"){
            if(MAX_PATCHES<patchCount){
                cout <<"Number of pathches exceeded";
                return-1;
            }
            string secondword=secondWord(line);

            patches[patchCount].targetFuncName=secondword;
            patches[patchCount].byteOffsetOfOffsetField=record_pos;
            patchCount++;
        }

        offset=offset+8+4+line.length();
    }

    
    for(int i=0;i<patchCount;i++){
        int64_t tar_offset=-1;
        for(int j=0;j<funcCount;j++){
            if(patches[i].targetFuncName==funcArray[j].funcName){
                tar_offset=funcArray[j].byteOffsetInResolveBin;
                break;
            }
        }
        if(tar_offset==-1){
            cout <<"Function not found\n";
            return -1;
        }
        fseek(fout,patches[i].byteOffsetOfOffsetField,SEEK_SET);
        fwrite(&tar_offset,sizeof(int64_t),1,fout);
    }
    if(mainOffset==-1){
        cout <<"no main function";
        return -1;
    }



    fin.close();
    fclose(fout);

    return mainOffset;

    // Every source line becomes one record holding the raw line, as-is.
    // resolve() only PEEKS at the leading word(s) -- enough to spot FUNC
    // (remember its position) and CALL (remember which function it needs
    // and where its offset field sits).
    // Once the whole file is written, every CALL's offset field is patched
    // with its target's position. Patching happens after the full write
    // Returns the byte offset of main's FUNC header record.
    // if there is no main return the error 
}

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    string text;
};
int32_t tokenizeLine(const string &line, Token tokens[], int32_t maxTokens)
{
    int32_t token_ct=0;

    string word1="";
    string word2="";

    string parameters[MAX_VARS_PER_FRAME];
    int para_ct=0;
    int start=0;

    //reading first word
    for(int i=start;i<line.length();i++){
        if(line[i]==' ' || line[i]=='\t'){
        break;
        }
        word1+=line[i];
        start++;
   }

   if(word1==""){
    return token_ct;
   }

   tokens[token_ct].type= KEYWORD; //add keyword
   tokens[token_ct].text=word1;
   token_ct++;

    //remove spaces
   while(start<line.length() && (line[start]==' ' || line[start]=='\t')){
    start++;
   }

   //read second word
   for(int i=start;i<line.length();i++){
        if(line[i]==' ' || line[i]=='\t'){
        break;
        }
        word2+=line[i];
        start++;
   }

   if(word2==""){
    return token_ct;
   }

   tokens[token_ct].type=IDENTIFIER;     //add identifier
   tokens[token_ct].text=word2;
   token_ct++;

   //reove spaces
   while(start<line.length() && (line[start]==' ' || line[start]=='\t')){
    start++;
   }
    
   //loop to read al parameter and add in param
   while(start<line.length()){

    string word="";
    for(int i=start;i<line.length();i++){
        
        if(line[i]==' ' || line[i]=='\t'){
            break;
        }
        word+=line[i];
        start++;
    }
    parameters[para_ct]=word;
    para_ct++;
    while(start<line.length() && (line[start]==' ' || line[start]=='\t')){
    start++;
    }
   }
   if(para_ct==0){
    return token_ct;
   }

   //add the parameters in tokens
   for(int i=0;i<para_ct;i++){
    if(token_ct>=maxTokens){
        return token_ct;
    }
    tokens[token_ct].type=PARAM;
    tokens[token_ct].text=parameters[i];
    token_ct++;
   }

   return token_ct;

    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
}
Snapshot *buildSnapshot(Stack<Frame> &callStack)
{
    Snapshot * snap=new Snapshot;
    snap.stackDepth=callStack.depth();

    callStack.snapshot_into(snap->callStack,MAX_STACK_DEPTH);

    return snap;
    // build the snapshot based on the callStack given
}
void executeProgram(const char *resolveBinPath, int64_t mainOffset, Timeline &timeline)
{
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline &timeline, const char *tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
}
// main section
int32_t main()
{

    if (!validateProgram("source.bin"))
    {
        // send an error response instead of a .tdbg file
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");

    if(mainOffset ==-1){
        return 1;
    }
    

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");

    return 0;
}