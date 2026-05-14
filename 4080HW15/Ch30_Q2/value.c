#include <stdio.h>
#include <string.h>

#include "memory.h"
#include "object.h"
#include "value.h"

void initValueArray(ValueArray* array) {
    array->values   = NULL;
    array->capacity = 0;
    array->count    = 0;
}

void writeValueArray(ValueArray* array, Value value) {
    if (array->capacity < array->count + 1) {
        int oldCapacity = array->capacity;
        array->capacity = GROW_CAPACITY(oldCapacity);
        array->values   = GROW_ARRAY(Value, array->values,
                                     oldCapacity, array->capacity);
    }
    array->values[array->count] = value;
    array->count++;
}

void freeValueArray(ValueArray* array) {
    FREE_ARRAY(Value, array->values, array->capacity);
    initValueArray(array);
}

void printValue(Value value) {
    switch (value.type) {
        case VAL_BOOL:         printf(AS_BOOL(value) ? "true" : "false"); break;
        case VAL_NIL:          printf("nil");                              break;
        case VAL_NUMBER:       printf("%g", AS_NUMBER(value));             break;
        case VAL_OBJ:          printObject(value);                         break;
        case VAL_SHORT_STRING: printf("%.*s", value.as.shortStr.length,
                                              value.as.shortStr.chars);    break;
    }
}

bool valuesEqual(Value a, Value b) {
    if (a.type != b.type) {
        if (IS_SHORT_STRING(a) && IS_OBJ(b)) {
            ObjString* s = (ObjString*)AS_OBJ(b);
            return s->length == a.as.shortStr.length &&
                   memcmp(s->chars, a.as.shortStr.chars, s->length) == 0;
        }
        if (IS_OBJ(a) && IS_SHORT_STRING(b)) {
            ObjString* s = (ObjString*)AS_OBJ(a);
            return s->length == b.as.shortStr.length &&
                   memcmp(s->chars, b.as.shortStr.chars, s->length) == 0;
        }
        return false;
    }
    switch (a.type) {
        case VAL_BOOL:         return AS_BOOL(a) == AS_BOOL(b);
        case VAL_NIL:          return true;
        case VAL_NUMBER:       return AS_NUMBER(a) == AS_NUMBER(b);
        case VAL_OBJ:          return AS_OBJ(a) == AS_OBJ(b);
        case VAL_SHORT_STRING:
            return a.as.shortStr.length == b.as.shortStr.length &&
                   memcmp(a.as.shortStr.chars, b.as.shortStr.chars,
                          a.as.shortStr.length) == 0;
    }
    return false;
}

ObjString* copyStringMaybeShort(const char* chars, int length, Value* out) {
    if (length <= SHORT_STRING_MAX) {
        *out = makeShortString(chars, length);
        return NULL;
    }
    return copyString(chars, length);
}
