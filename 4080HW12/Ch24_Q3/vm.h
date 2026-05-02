#ifndef clox_vm_h
#define clox_vm_h

#include "chunk.h"
#include "object.h"
#include "table.h"
#include "value.h"

#define FRAMES_MAX 64
#define STACK_MAX  (FRAMES_MAX * 256)

typedef enum {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR,
    INTERPRET_RUNTIME_ERROR,
} InterpretResult;

typedef struct {
    ObjFunction* function;
    uint8_t*     ip;
    Value*       slots;
} CallFrame;

#define NATIVE_ERROR_MAX 256

typedef struct {
    CallFrame frames[FRAMES_MAX];
    int       frameCount;
    Value     stack[STACK_MAX];
    Value*    stackTop;
    Table     globals;
    Table     strings;
    Obj*      objects;
    bool      hasNativeError;
    char      nativeErrorMsg[NATIVE_ERROR_MAX];
} VM;

extern VM vm;

void            initVM();
void            nativeRuntimeError(const char* format, ...);
void            freeVM();
void            push(Value value);
Value           pop();
InterpretResult interpret(const char* source);

#endif