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
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <stdexcept>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "helper.h"
using namespace std;

// ---- Constants ----
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t SOCKET_TIMEOUT_SEC = 5;                     
 // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

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

void writeHeader(FILE *f, const TTDBHeader &h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);
    fwrite(&h.stepCount,sizeof(int32_t),1,f);
    fwrite(&h.indexOffset,sizeof(int64_t),1,f);

    // placeholder for other two data members
}

// resolve.bin - bookkeeping




// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream &in, string &out)
{
    sstring newline;
    while(getline(in, newline)){
        while(!newline.empty() &&
              (newline.back()=='\r' || newline.back()==' ' || newline.back()=='\t')){
            newline.pop_back();
        }
        size_t s = newline.find_first_not_of(" \t");
        if(s == string::npos){
            continue;            
        }
        out = newline.substr(s);    
        return true;
    }
    return false;

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
            if(funcCount>=MAX_FUNCS){
                cout <<"Number of function exceeded";
                fclose(fout);
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
            if(patchCount>=MAX_PATCHES){
                cout <<"Number of pathches exceeded";
                fclose(fout);
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
            fclose(fout);
            return -1;
        }
        fseek(fout,patches[i].byteOffsetOfOffsetField,SEEK_SET);
        fwrite(&tar_offset,sizeof(int64_t),1,fout);
    }
    if(mainOffset==-1){
        cout <<"no main function";
        fclose(fout);
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
    if(para_ct >=MAX_VARS_PER_FRAME){
        break;
    }

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
    snap->stackDepth=callStack.depth();

    callStack.snapshot_into(snap->callStack,MAX_STACK_DEPTH);

    return snap;
    // build the snapshot based on the callStack given
}

Variable *search_variable(Frame &curr,string var_name){
    for(int i=0;i<curr.argc;i++){
        if(curr.argv[i].name ==var_name){           //check for variable in local and arv of frame
            return &curr.argv[i];

        }
    }

    for(int i=0;i<curr.localCount;i++){
        if(curr.locals[i].name==var_name){
            return &curr.locals[i];
        }
    }
    return nullptr;
}


Variable *find_or_create_var(Frame & f,string var_name){
    Variable* var=search_variable(f,var_name);
    if(var !=nullptr){
        return var;
    }                                //check the varin local and srv aif not found creatte neew local

    if(f.localCount>=MAX_VARS_PER_FRAME){
        return nullptr;
    }

    f.locals[f.localCount].name=var_name;
    f.locals[f.localCount].value=0;
    f.localCount++;
    return &f.locals[f.localCount -1];
}
int32_t value_or_variable(Frame& curr,string & param){
    bool check_num=true;

    int st=0;
    if(param.length()>0 && param[0]=='-'){  //checking if the number if the text is -neg
        st=1;
    }

    for(int i=st;i<param.length();i++){
        if(param[i]< '0' || param[i]> '9'){
            check_num=false;
            break;
        }
    }

    if(check_num && param.length() > st){
        return stoi(param);    //return value
    }

    Variable* var=search_variable(curr,param);
    if(var!=nullptr){
        return var->value;
    }

    return 0;
}
void executeProgram(const char *resolveBinPath, int64_t mainOffset, Timeline &timeline)
{
    FILE * fin=fopen(resolveBinPath,"rb");
    if(!fin){
        cout <<"File not found\n";
        return;
    }
    Stack<Frame> callStack;  //initailix=ze call satck

    fseek(fin,mainOffset,SEEK_SET);
    string call_arguments[MAX_STACK_DEPTH+1][MAX_VARS_PER_FRAME];

    Frame main_;       //crreate main frame
    main_.func_name="main";
    main_.argc=0;
    main_.localCount=0;
    main_.returnLine=-1;

    callStack.push(main_);  //push the frame into stack callStack

    string curr_line;
    int64_t offset;
    while(true){

        offset=readResolveRecord(fin,curr_line);

        if(offset==-1){
            fclose(fin);
            return;
        }
        Token tokens[MAX_TOKENS];
        int token_ct=tokenizeLine(curr_line,tokens,MAX_TOKENS);

        if(token_ct==0){
            continue;
        }

        if((tokens[0].text=="set"||tokens[0].text=="add"||tokens[0].text=="sub"||tokens[0].text=="mul"||tokens[0].text=="div") && token_ct <3){
            cout <<"Missin value.\n";
            fclose(fin);
            return;
        }

        if(tokens[0].text=="func"){
            continue;

        }
        else if(tokens[0].text=="set"){
            string var=tokens[1].text;
            Frame &curr=callStack.peek();
            int32_t val=value_or_variable(curr,tokens[2].text);

            Variable * new_var=find_or_create_var(curr,var);
            if(new_var==nullptr){
                cout <<"Number of variable Exceeded!.\n";
                fclose(fin);
                return;
            }

            new_var->value=val;
        }
        else if(tokens[0].text=="add"){
            Frame &curr=callStack.peek();
            string var=tokens[1].text;
            int32_t val= value_or_variable(curr,tokens[2].text);
            Variable* v=search_variable(curr,var);
            if(v==nullptr){
                cout<<"Varible not defined.\n";
                fclose(fin);
                return;
            }
            v->value=v->value+ val;
        }
        else if(tokens[0].text=="sub"){
            Frame &curr=callStack.peek();
            string var=tokens[1].text;
            int32_t val= value_or_variable(curr,tokens[2].text);
            Variable* v=search_variable(curr,var);
            if(v==nullptr){
                cout<<"Varible not defined.\n";
                fclose(fin);
                return;
            }
            v->value=v->value - val;
        }
        else if(tokens[0].text=="mul"){
            Frame &curr=callStack.peek();
            string var=tokens[1].text;
            int32_t val= value_or_variable(curr,tokens[2].text);
            Variable* v=search_variable(curr,var);
            if(v==nullptr){
                cout<<"Varible not defined.\n";
                fclose(fin);
                return;
            }
            v->value=v->value * val;
        }else if(tokens[0].text=="div"){
            Frame &curr=callStack.peek();
            string var=tokens[1].text;
            int32_t val= value_or_variable(curr,tokens[2].text);
            Variable* v=search_variable(curr,var);
            if(val==0){
                cout <<"division by zero not possible!!.\n";
                fclose(fin);
                return;
            }
            if(v==nullptr){
                cout<<"Varible not defined.\n";
                fclose(fin);
                return;
            }
            v->value=v->value / val;
        }
        else if(tokens[0].text=="call"){
            if(callStack.depth() >=MAX_STACK_DEPTH){
                cout <<"Call stack function exceeded.!!\n";
                fclose(fin);
                return;
            }
            Frame & caller_frame=callStack.peek();

            int32_t return_line=ftell(fin);

            fseek(fin,offset,SEEK_SET);

            string new_func;
            readResolveRecord(fin,new_func);
            Token ftokens[MAX_TOKENS];
            int fct=tokenizeLine(new_func,ftokens,MAX_TOKENS);

            int param_ct=fct-2;
            int arg_ct=token_ct-2;

            if(param_ct!=arg_ct){
                cout <<"Wrong number of Parameter passed.\n";
                fclose(fin);
                return;
            }

            Frame called_func;
            called_func.func_name=ftokens[1].text;
            called_func.argc=param_ct;
            called_func.returnLine=return_line;
            called_func.localCount=0;

            int dep=callStack.depth() +1;

            for(int i=0;i<param_ct;i++){
                called_func.argv[i].name=ftokens[2 +i].text;
                called_func.argv[i].value=value_or_variable(caller_frame,tokens[i+2].text);
                call_arguments[dep][i]=tokens[2+i].text;
            }
            callStack.push(called_func);

        }
        else if(tokens[0].text=="func_end"){
            int new_depth=callStack.depth();

            Frame func_comp=callStack.pop();

            if(callStack.isEmpty()){
                Snapshot * s=buildSnapshot(callStack);
                timeline.record(s);
                fclose(fin);
                return;
            }

            Frame & curr=callStack.peek();
            for(int i=0;i<func_comp.argc;i++){
                Variable * var=search_variable(curr,call_arguments[new_depth][i]);
                if(var!=nullptr){
                    var->value=func_comp.argv[i].value;

                }
            }
            fseek(fin,func_comp.returnLine,SEEK_SET);
        }
        else{

            cout <<"Insttruction incorrect\n";
            fclose(fin);
            return;
        }

        timeline.record(buildSnapshot(callStack));

    }


    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
}

void writeStringinBinary(FILE* f,const string &str){
    int32_t len=str.length();
    fwrite(&len,sizeof(int32_t),1,f);
    fwrite(str.c_str(),1,len,f);
}

void write_var_in_bin(FILE *f,Variable var){
    writeStringinBinary(f,var.name);
    fwrite(&var.value,sizeof(int32_t),1,f);
}


void writeSnapshot_in_bin(FILE * f,const Snapshot & snap){
    fwrite(&snap.stackDepth,sizeof(int32_t),1,f);
    for(int i=0;i<snap.stackDepth;i++){

        const Frame& curr=snap.callStack[i];
        writeStringinBinary(f,curr.func_name);
        fwrite(&curr.argc,sizeof(int32_t),1,f);
        for(int j=0;j<curr.argc;j++){
            write_var_in_bin(f,curr.argv[j]);

        }

        fwrite(&curr.returnLine,sizeof(int32_t),1,f);

        fwrite(&curr.localCount,sizeof(int32_t),1,f);
        for(int j=0;j<curr.localCount;j++){
            write_var_in_bin(f,curr.locals[j]);
        }
    }
}
// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline &timeline, const char *tdbgPath)
{
    FILE *fout=fopen(tdbgPath,"wb");

    if(!fout){
        cout <<"File not created.\n";
        return;
    }

    TTDBHeader head;
    head.magic[0]='T';
    head.magic[1]='T';
    head.magic[2]='D';
    head.magic[3]='B';

    head.version=1;
    head.stepCount=timeline.getStepCount();
    head.indexOffset=0;

    writeHeader(fout,head);

    int32_t step_ct=timeline.getStepCount();

    int64_t *index;
    if(step_ct >0){
        index=new int64_t[step_ct];
    }else{
        index=new int64_t[1];

    }

    int32_t idx=0;

    TimelineNode *n=timeline.begin();
    while(n!=nullptr){
        index[idx]=ftell(fout);
        idx++;

        writeSnapshot_in_bin(fout,*(n->data));

        n=n->next;

    }
    head.indexOffset=ftell(fout);
    fwrite(index,sizeof(int64_t),step_ct,fout);

    fseek(fout,0,SEEK_SET);
    writeHeader(fout,head);

    delete[] index;

    fclose(fout);

    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
}


////PHASE2///////////////

bool receiveSource(int sock, string &errorMessage)
{
    FILE *src_f = fopen("source.bin", "wb");

    if (src_f == nullptr){
        errorMessage = "Cannot create source.bin";
        return false;
    }

    TrailerChecker checker;

    char rec_buff[IO_BUFFER_SIZE];
    char out_buff[IO_BUFFER_SIZE + 4];
    bool status_rec = false;
    bool success = true;
    uint64_t totalBytes = 0;

    while (true){
        ssize_t bytes_rec = recv(sock, rec_buff,IO_BUFFER_SIZE, 0);
        if (bytes_rec < 0 && errno == EINTR){
            continue;
        }

        if (bytes_rec < 0){
            errorMessage = "Error receiving source file";
            success = false;
            break;
        }

        if (bytes_rec == 0){
            break;
        }

        size_t body_bytes = checker.feed( rec_buff, bytes_rec,out_buff);

        if (body_bytes == 0){
            continue;
        }

        size_t start = 0;
        if (!status_rec){
            status_rec = true;
            if (out_buff[0] != 0){
                errorMessage = "Invalid upload status";
                success = false;
                break;
            }
            start = 1;
        }
        size_t fileBytes = body_bytes - start;

        if (fileBytes > MAX_SOURCE_BYTES - totalBytes){
            errorMessage = "Source file is too large";
            success = false;
            break;
        }
        if (fwrite(out_buff + start, 1,fileBytes, src_f) != fileBytes){
            errorMessage = "Error writing source.bin";
            success = false;
            break;
        }

        totalBytes += fileBytes;
    }

    if (fclose(src_f) != 0){
        errorMessage = "Error saving source.bin";
        success = false;
    }
    if (success && (!status_rec|| !checker.verify())){
        errorMessage = "Upload checksum mismatch";
        success = false;
    }

    if (success && totalBytes == 0){
        errorMessage = "Source file is empty";
        success = false;
    }

    if (!success){
        remove("source.bin");
    }

    return success;
}




bool sendServerError(int sock, const string &errorMessage)
{
    CrcBuilder crc;
    char status = 1;
    crc.add(&status, 1);
    crc.add(errorMessage.c_str(), errorMessage.size());

    if(!sendAll(sock, &status, 1)){
        return false;
    }

    if(!sendAll(sock, errorMessage.c_str(), errorMessage.size())){
        return false;
    }

    return crc.sendTrailer(sock);
}



bool sendDebugFile(int sock, const char *tdbgPath)
{
    FILE *debug_f = fopen(tdbgPath, "rb");

    if(debug_f == nullptr){
        return false;
    }

    CrcBuilder crc;
    char status = 0;

    bool success = sendAll(sock, &status, 1);

    if(success){
        crc.add(&status, 1);
    }

    char send_buff[IO_BUFFER_SIZE];
    size_t bytes_read;

    while(success && (bytes_read = fread(send_buff, 1, IO_BUFFER_SIZE,debug_f)) > 0){
        crc.add(send_buff, bytes_read);

        if(!sendAll(sock, send_buff, bytes_read)){
            success = false;
            break;
        }
    }
    if(ferror(debug_f)){
        success = false;
    }
    if(fclose(debug_f) != 0){
        success = false;
    }
    if(!success){
        return false;
    }

    return crc.sendTrailer(sock);
}



bool handleClient(int sock)
{
    string errorMessage;
    if(!receiveSource(sock, errorMessage)){
        return sendServerError(sock, errorMessage);
    }
    if(!validateProgram("source.bin")){
        return sendServerError(sock, "Invalid source program");
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");

    if(mainOffset == -1){
        return sendServerError(sock, "Error resolving source program");
    }

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    if(timeline.getStepCount() == 0){
        return sendServerError(sock, "Program execution failed");
    }
    TimelineNode *curr = timeline.begin();

    while(curr->next != nullptr){
        curr = curr->next;
    }

    if(curr->data->stackDepth != 0){
        return sendServerError(sock, "Program execution incomplete");
    }

    if(remove("session.tdbg") != 0 && errno != ENOENT){
        return sendServerError(sock, "Cannot remove old debug file");
    }

    writeTdbg(timeline, "session.tdbg");

    FILE *debug_f = fopen("session.tdbg", "rb");

    if(debug_f == nullptr){
        return sendServerError(sock, "Cannot create debug file");
    }

    fclose(debug_f);
    return sendDebugFile(sock, "session.tdbg");
}

// main section
int main(int argc, char *argv[])
{
    if(argc != 2){
        cout << "Usage: ./server <port>" << endl;
        return 1;
    }

    char *end = nullptr;
    errno = 0;
    long port_num = strtol(argv[1], &end, 10);

    if(errno != 0 || end == argv[1] || *end != '\0' ||
       port_num < 1 || port_num > 65535){
        cout << "Invalid port number" << endl;
        return 1;
    }

    int listenSock = createListenSocket((int)port_num);

    if(listenSock < 0){
        cout << "Cannot create listening socket" << endl;
        return 1;
    }

    cout << "Server listening on port " << port_num << endl;

    while(true){
        int clientSock = accept(listenSock, nullptr, nullptr);

        if(clientSock < 0){
            if(errno == EINTR){
                continue;
            }
            perror("accept");
            break;
        }

        timeval timeout{};
        timeout.tv_sec = SOCKET_TIMEOUT_SEC;

        if(setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO,&timeout, sizeof(timeout)) < 0 ||setsockopt(clientSock, SOL_SOCKET, SO_SNDTIMEO,&timeout, sizeof(timeout)) < 0){
            perror("setsockopt");
            close(clientSock);
            continue;
        }

        try{
            if(!handleClient(clientSock)){
                cout << "Error handling client" << endl;
            }
        }
        catch(const exception &e){
            cout << "Server error: " << e.what() << endl;
        }

        close(clientSock);

        remove("source.bin");
        remove("resolve.bin");
        remove("session.tdbg");
    }

    close(listenSock);
    return 1;
}