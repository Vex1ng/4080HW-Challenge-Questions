#ifndef clox_value_h
#define clox_value_h

#include <string.h>
#include "common.h"

typedef struct Obj Obj;
typedef struct ObjString ObjString;

#define SHORT_STRING_MAX 7

typedef enum {
    VAL_BOOL,
    VAL_NIL,
    VAL_NUMBER,
    VAL_OBJ,
    VAL_SHORT_STRING,
} ValueType;

typedef struct {
    ValueType type;
    union {
        bool   boolean;
        double number;
        Obj*   obj;
        struct {
            uint8_t length;
            char    chars[SHORT_STRING_MAX];
        } shortStr;
    } as;
} Value;

#define IS_BOOL(value)         ((value).type == VAL_BOOL)
#define IS_NIL(value)          ((value).type == VAL_NIL)
#define IS_NUMBER(value)       ((value).type == VAL_NUMBER)
#define IS_OBJ(value)          ((value).type == VAL_OBJ)
#define IS_SHORT_STRING(value) ((value).type == VAL_SHORT_STRING)

#define AS_BOOL(value)    ((value).as.boolean)
#define AS_NUMBER(value)  ((value).as.number)
#define AS_OBJ(value)     ((value).as.obj)

#define BOOL_VAL(value)   ((Value){VAL_BOOL,   {.boolean = value}})
#define NIL_VAL           ((Value){VAL_NIL,    {.number  = 0}})
#define NUMBER_VAL(value) ((Value){VAL_NUMBER, {.number  = value}})
#define OBJ_VAL(object)   ((Value){VAL_OBJ,   {.obj     = (Obj*)(object)}})

static inline Value makeShortString(const char* chars, int length) {
    Value v;
    v.type               = VAL_SHORT_STRING;
    v.as.shortStr.length = (uint8_t)length;
    memcpy(v.as.shortStr.chars, chars, length);
    if (length < SHORT_STRING_MAX) v.as.shortStr.chars[length] = '\0';
    return v;
}

#define SHORT_STRING_VAL(chars, len) makeShortString(chars, len)

typedef struct {
    int    capacity;
    int    count;
    Value* values;
} ValueArray;

bool valuesEqual(Value a, Value b);
void initValueArray(ValueArray* array);
void writeValueArray(ValueArray* array, Value value);
void freeValueArray(ValueArray* array);
void printValue(Value value);

#endif
