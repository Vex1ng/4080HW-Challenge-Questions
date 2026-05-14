#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "compiler.h"
#include "debug.h"
#include "memory.h"
#include "object.h"
#include "vm.h"

VM vm;

static void resetStack() {
    vm.stackTop      = vm.stack;
    vm.frameCount    = 0;
    vm.openUpvalues  = NULL;
}

static void runtimeError(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputs("\n", stderr);

    for (int i = vm.frameCount - 1; i >= 0; i--) {
        CallFrame*   frame       = &vm.frames[i];
        ObjFunction* function    = frame->function;
        size_t       instruction = frame->ip - function->chunk.code - 1;
        fprintf(stderr, "[line %d] in ", function->chunk.lines[instruction]);
        if (function->name == NULL) {
            fprintf(stderr, "script\n");
        } else {
            fprintf(stderr, "%s()\n", function->name->chars);
        }
    }
    resetStack();
}

static Value minNative(int argCount, Value* args) {
    if (!IS_NUMBER(args[0]) || !IS_NUMBER(args[1])) {
        runtimeError("min() expects two numbers.");
        return NIL_VAL;
    }
    double a = AS_NUMBER(args[0]);
    double b = AS_NUMBER(args[1]);
    return NUMBER_VAL(a < b ? a : b);
}

static Value maxNative(int argCount, Value* args) {
    if (!IS_NUMBER(args[0]) || !IS_NUMBER(args[1])) {
        runtimeError("max() expects two numbers.");
        return NIL_VAL;
    }
    double a = AS_NUMBER(args[0]);
    double b = AS_NUMBER(args[1]);
    return NUMBER_VAL(a > b ? a : b);
}

static Value deleteFieldNative(int argCount, Value* args) {
    if (argCount != 2) return NIL_VAL;
    if (!IS_INSTANCE(args[0])) return NIL_VAL;
    if (!IS_STRING(args[1])) return NIL_VAL;

    ObjInstance* instance = AS_INSTANCE(args[0]);
    tableDelete(&instance->fields, OBJ_VAL(AS_STRING(args[1])));
    return NIL_VAL;
}

static void defineNative(const char* name, NativeFn function, int arity) {
    push(OBJ_VAL(copyString(name, (int)strlen(name))));
    push(OBJ_VAL(newNative(function, arity)));
    tableSet(&vm.globals, vm.stack[0], vm.stack[1]);
    pop();
    pop();
}

void initVM() {
    resetStack();
    vm.objects        = NULL;
    vm.bytesAllocated = 0;
    vm.nextGC         = 1024 * 1024;
    vm.grayCount      = 0;
    vm.grayCapacity   = 0;
    vm.grayStack      = NULL;
    vm.initString     = NULL;
    initTable(&vm.globals);
    initTable(&vm.strings);
    vm.initString = copyString("init", 4);

    defineNative("min", minNative, 2);
    defineNative("max", maxNative, 2);
    defineNative("deleteField", deleteFieldNative, 2);
}

void freeVM() {
    freeTable(&vm.globals);
    freeTable(&vm.strings);
    vm.initString = NULL;
    freeObjects();
}

void push(Value value) {
    *vm.stackTop = value;
    vm.stackTop++;
}

Value pop() {
    vm.stackTop--;
    return *vm.stackTop;
}

static Value peek(int distance) {
    return vm.stackTop[-1 - distance];
}

static bool call(ObjClosure* closure, int argCount) {
    if (argCount != closure->function->arity) {
        runtimeError("Expected %d arguments but got %d.",
                     closure->function->arity, argCount);
        return false;
    }
    if (vm.frameCount == FRAMES_MAX) {
        runtimeError("Stack overflow.");
        return false;
    }
    CallFrame* frame = &vm.frames[vm.frameCount++];
    frame->closure   = closure;
    frame->function  = closure->function;
    frame->ip        = closure->function->chunk.code;
    frame->slots     = vm.stackTop - argCount - 1;
    return true;
}

static bool bindMethod(ObjClass* klass, ObjString* name) {
    Value method;
    if (!tableGet(&klass->methods, OBJ_VAL(name), &method)) {
        runtimeError("Undefined property '%s'.", name->chars);
        return false;
    }
    ObjBoundMethod* bound = newBoundMethod(peek(0), AS_CLOSURE(method));
    pop();
    push(OBJ_VAL(bound));
    return true;
}

static bool callValue(Value callee, int argCount) {
    if (IS_OBJ(callee)) {
        switch (OBJ_TYPE(callee)) {
            case OBJ_CLASS: {
                ObjClass* klass = AS_CLASS(callee);
                vm.stackTop[-argCount - 1] = OBJ_VAL(newInstance(klass));
                Value initializer;
                if (tableGet(&klass->methods, OBJ_VAL(vm.initString), &initializer)) {
                    return call(AS_CLOSURE(initializer), argCount);
                } else if (argCount != 0) {
                    runtimeError("Expected 0 arguments but got %d.", argCount);
                    return false;
                }
                return true;
            }
            case OBJ_BOUND_METHOD: {
                ObjBoundMethod* bound = AS_BOUND_METHOD(callee);
                vm.stackTop[-argCount - 1] = bound->receiver;
                return call(bound->method, argCount);
            }
            case OBJ_CLOSURE:
                return call(AS_CLOSURE(callee), argCount);
            case OBJ_FUNCTION: {
                ObjFunction* fn = AS_FUNCTION(callee);
                if (argCount != fn->arity) {
                    runtimeError("Expected %d arguments but got %d.", fn->arity, argCount);
                    return false;
                }
                if (vm.frameCount == FRAMES_MAX) {
                    runtimeError("Stack overflow.");
                    return false;
                }
                CallFrame* frame = &vm.frames[vm.frameCount++];
                frame->closure   = NULL;
                frame->function  = fn;
                frame->ip        = fn->chunk.code;
                frame->slots     = vm.stackTop - argCount - 1;
                return true;
            }
            case OBJ_NATIVE: {
                ObjNative* native = AS_NATIVE(callee);
                if (native->arity != -1 && argCount != native->arity) {
                    runtimeError("Expected %d arguments but got %d.",
                                 native->arity, argCount);
                    return false;
                }
                Value result = native->function(argCount, vm.stackTop - argCount);
                vm.stackTop -= argCount + 1;
                push(result);
                return true;
            }
            default: break;
        }
    }
    runtimeError("Can only call functions and classes.");
    return false;
}

static ObjUpvalue* captureUpvalue(Value* local) {
    ObjUpvalue* prevUpvalue = NULL;
    ObjUpvalue* upvalue     = vm.openUpvalues;

    while (upvalue != NULL && upvalue->location > local) {
        prevUpvalue = upvalue;
        upvalue     = upvalue->next;
    }

    if (upvalue != NULL && upvalue->location == local) {
        return upvalue;
    }

    ObjUpvalue* createdUpvalue = newUpvalue(local);
    createdUpvalue->next = upvalue;

    if (prevUpvalue == NULL) {
        vm.openUpvalues = createdUpvalue;
    } else {
        prevUpvalue->next = createdUpvalue;
    }

    return createdUpvalue;
}

static void closeUpvalues(Value* last) {
    while (vm.openUpvalues != NULL && vm.openUpvalues->location >= last) {
        ObjUpvalue* upvalue = vm.openUpvalues;
        upvalue->closed     = *upvalue->location;
        upvalue->location   = &upvalue->closed;
        vm.openUpvalues     = upvalue->next;
    }
}

static bool isFalsey(Value value) {
    return IS_NIL(value) || (IS_BOOL(value) && !AS_BOOL(value));
}

static void concatenate() {
    ObjString* b = AS_STRING(pop());
    ObjString* a = AS_STRING(pop());

    int   length = a->length + b->length;
    char* chars  = ALLOCATE(char, length + 1);
    memcpy(chars,             a->chars, a->length);
    memcpy(chars + a->length, b->chars, b->length);
    chars[length] = '\0';

    ObjString* result = takeString(chars, length);
    push(OBJ_VAL(result));
}

#define SAVE_IP() (frame->ip = ip)
#define LOAD_IP() (ip = frame->ip)

static bool invoke(ObjString* name, int argCount) {
    Value receiver = peek(argCount);
    if (!IS_INSTANCE(receiver)) {
        runtimeError("Only instances have methods.");
        return false;
    }
    ObjInstance* instance = AS_INSTANCE(receiver);
    Value value;
    if (tableGet(&instance->fields, OBJ_VAL(name), &value)) {
        vm.stackTop[-argCount - 1] = value;
        return callValue(value, argCount);
    }
    if (!tableGet(&instance->klass->methods, OBJ_VAL(name), &value)) {
        runtimeError("Undefined property '%s'.", name->chars);
        return false;
    }
    return call(AS_CLOSURE(value), argCount);
}

static InterpretResult run() {
    CallFrame* frame = &vm.frames[vm.frameCount - 1];
    register uint8_t* ip = frame->ip;

#define READ_BYTE()     (*ip++)
#define READ_CONSTANT() (frame->function->chunk.constants.values[READ_BYTE()])
#define READ_STRING()   AS_STRING(READ_CONSTANT())
#define READ_SHORT()    (ip += 2, (uint16_t)((ip[-2] << 8) | ip[-1]))

#define BINARY_OP(valueType, op) \
    do { \
        if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) { \
            SAVE_IP(); \
            runtimeError("Operands must be numbers."); \
            return INTERPRET_RUNTIME_ERROR; \
        } \
        double b = AS_NUMBER(pop()); \
        double a = AS_NUMBER(pop()); \
        push(valueType(a op b)); \
    } while (false)

    for (;;) {
#ifdef DEBUG_TRACE_EXECUTION
        printf(" ");
        for (Value* slot = vm.stack; slot < vm.stackTop; slot++) {
            printf("[ ");
            printValue(*slot);
            printf(" ]");
        }
        printf("\n");
        SAVE_IP();
        disassembleInstruction(&frame->function->chunk,
                               (int)(frame->ip - frame->function->chunk.code));
#endif

        uint8_t instruction;
        switch (instruction = READ_BYTE()) {
            case OP_CONSTANT: {
                Value constant = READ_CONSTANT();
                push(constant);
                break;
            }
            case OP_NIL:   push(NIL_VAL);         break;
            case OP_TRUE:  push(BOOL_VAL(true));  break;
            case OP_FALSE: push(BOOL_VAL(false)); break;
            case OP_POP:   pop();                 break;

            case OP_GET_LOCAL: {
                uint16_t slot = READ_SHORT();
                push(frame->slots[slot]);
                break;
            }
            case OP_SET_LOCAL: {
                uint16_t slot = READ_SHORT();
                frame->slots[slot] = peek(0);
                break;
            }

            case OP_GET_UPVALUE: {
                uint8_t slot = READ_BYTE();
                push(*frame->closure->upvalues[slot]->location);
                break;
            }
            case OP_SET_UPVALUE: {
                uint8_t slot = READ_BYTE();
                *frame->closure->upvalues[slot]->location = peek(0);
                break;
            }

            case OP_GET_GLOBAL: {
                ObjString* name = READ_STRING();
                Value value;
                if (!tableGet(&vm.globals, OBJ_VAL(name), &value)) {
                    SAVE_IP();
                    runtimeError("Undefined variable '%s'.", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(value);
                break;
            }
            case OP_DEFINE_GLOBAL: {
                ObjString* name = READ_STRING();
                tableSet(&vm.globals, OBJ_VAL(name), peek(0));
                pop();
                break;
            }
            case OP_SET_GLOBAL: {
                ObjString* name = READ_STRING();
                if (tableSet(&vm.globals, OBJ_VAL(name), peek(0))) {
                    tableDelete(&vm.globals, OBJ_VAL(name));
                    SAVE_IP();
                    runtimeError("Undefined variable '%s'.", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }

            case OP_NEGATE:
                if (!IS_NUMBER(peek(0))) {
                    SAVE_IP();
                    runtimeError("Operand must be a number.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(NUMBER_VAL(-AS_NUMBER(pop())));
                break;

            case OP_ADD:
                if (IS_STRING(peek(0)) && IS_STRING(peek(1))) {
                    concatenate();
                } else if (IS_NUMBER(peek(0)) && IS_NUMBER(peek(1))) {
                    double b = AS_NUMBER(pop());
                    double a = AS_NUMBER(pop());
                    push(NUMBER_VAL(a + b));
                } else {
                    SAVE_IP();
                    runtimeError("Operands must be two numbers or two strings.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;

            case OP_SUBTRACT: BINARY_OP(NUMBER_VAL, -); break;
            case OP_MULTIPLY: BINARY_OP(NUMBER_VAL, *); break;
            case OP_DIVIDE:   BINARY_OP(NUMBER_VAL, /); break;

            case OP_EQUAL: {
                Value b = pop();
                Value a = pop();
                push(BOOL_VAL(valuesEqual(a, b)));
                break;
            }
            case OP_GREATER: BINARY_OP(BOOL_VAL, >); break;
            case OP_LESS:    BINARY_OP(BOOL_VAL, <); break;

            case OP_NOT:
                push(BOOL_VAL(isFalsey(pop())));
                break;

            case OP_TERNARY: {
                Value elseBranch = pop();
                Value thenBranch = pop();
                Value condition  = pop();
                push(isFalsey(condition) ? elseBranch : thenBranch);
                break;
            }

            case OP_PRINT: {
                printValue(pop());
                printf("\n");
                break;
            }

            case OP_JUMP: {
                uint16_t offset = READ_SHORT();
                ip += offset;
                break;
            }
            case OP_JUMP_IF_FALSE: {
                uint16_t offset = READ_SHORT();
                if (isFalsey(peek(0))) ip += offset;
                break;
            }
            case OP_LOOP: {
                uint16_t offset = READ_SHORT();
                ip -= offset;
                break;
            }

            case OP_CALL: {
                int argCount = READ_BYTE();
                SAVE_IP();
                if (!callValue(peek(argCount), argCount)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                frame = &vm.frames[vm.frameCount - 1];
                LOAD_IP();
                break;
            }

            case OP_CLOSURE: {
                ObjFunction* function = AS_FUNCTION(READ_CONSTANT());
                ObjClosure*  closure  = newClosure(function);
                push(OBJ_VAL(closure));
                for (int i = 0; i < closure->upvalueCount; i++) {
                    uint8_t isLocal = READ_BYTE();
                    uint8_t index   = READ_BYTE();
                    if (isLocal) {
                        closure->upvalues[i] = captureUpvalue(frame->slots + index);
                    } else {
                        closure->upvalues[i] = frame->closure->upvalues[index];
                    }
                }
                break;
            }

            case OP_CLOSE_UPVALUE:
                closeUpvalues(vm.stackTop - 1);
                pop();
                break;

            case OP_CLASS:
                push(OBJ_VAL(newClass(READ_STRING())));
                break;

            case OP_GET_PROPERTY: {
                if (!IS_INSTANCE(peek(0))) {
                    SAVE_IP();
                    runtimeError("Only instances have properties.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjInstance* instance = AS_INSTANCE(peek(0));
                ObjString*   name     = READ_STRING();
                Value value;
                if (tableGet(&instance->fields, OBJ_VAL(name), &value)) {
                    pop();
                    push(value);
                    break;
                }
                SAVE_IP();
                if (!bindMethod(instance->klass, name)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }

            case OP_SET_PROPERTY: {
                if (!IS_INSTANCE(peek(1))) {
                    SAVE_IP();
                    runtimeError("Only instances have fields.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjInstance* instance = AS_INSTANCE(peek(1));
                ObjString*   name     = READ_STRING();
                tableSet(&instance->fields, OBJ_VAL(name), peek(0));
                Value value = pop();
                pop();
                push(value);
                break;
            }

            case OP_INHERIT: {
                Value superVal = peek(1);
                if (!IS_CLASS(superVal)) {
                    SAVE_IP();
                    runtimeError("Superclass must be a class.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjClass* superclass = AS_CLASS(superVal);
                ObjClass* subclass   = AS_CLASS(peek(0));
                for (int i = 0; i < subclass->methods.capacity; i++) {
                    Entry* entry = &subclass->methods.entries[i];
                    if (IS_NIL(entry->key)) continue;
                    Value existing;
                    if (tableGet(&superclass->methods, entry->key, &existing)) {
                        tableSet(&superclass->inners, entry->key, entry->value);
                    } else {
                        tableSet(&superclass->methods, entry->key, entry->value);
                    }
                }
                tableAddAll(&superclass->methods, &subclass->methods);
                pop();
                break;
            }

            case OP_INNER: {
                ObjString* name = frame->closure->function->name;
                if (name == NULL) break;
                Value receiver = frame->slots[0];
                if (!IS_INSTANCE(receiver)) break;
                ObjInstance* instance = AS_INSTANCE(receiver);
                Value innerMethod;
                if (tableGet(&instance->klass->inners, OBJ_VAL(name), &innerMethod)) {
                    if (!call(AS_CLOSURE(innerMethod), 0)) {
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    frame = &vm.frames[vm.frameCount - 1];
                    LOAD_IP();
                }
                break;
            }

            case OP_METHOD: {
                ObjString* name  = READ_STRING();
                ObjClass*  klass = AS_CLASS(peek(1));
                Value method     = peek(0);
                tableSet(&klass->methods, OBJ_VAL(name), method);
                pop();
                break;
            }

            case OP_INVOKE: {
                ObjString* method   = READ_STRING();
                int        argCount = READ_BYTE();
                SAVE_IP();
                if (!invoke(method, argCount)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                frame = &vm.frames[vm.frameCount - 1];
                LOAD_IP();
                break;
            }

            case OP_RETURN: {
                Value result = pop();
                closeUpvalues(frame->slots);
                vm.frameCount--;
                if (vm.frameCount == 0) {
                    pop();
                    return INTERPRET_OK;
                }
                vm.stackTop = frame->slots;
                push(result);
                frame = &vm.frames[vm.frameCount - 1];
                LOAD_IP();
                break;
            }
        }
    }

#undef READ_BYTE
#undef READ_CONSTANT
#undef READ_STRING
#undef READ_SHORT
#undef SAVE_IP
#undef LOAD_IP
#undef BINARY_OP
}

InterpretResult interpret(const char* source) {
    ObjFunction* function = compile(source);
    if (function == NULL) return INTERPRET_COMPILE_ERROR;

    push(OBJ_VAL(function));
    ObjClosure* closure = newClosure(function);
    pop();
    push(OBJ_VAL(closure));
    call(closure, 0);

    return run();
}