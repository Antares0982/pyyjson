#include "decode/decode_shared.h"

bool _decode_obj_stack_resize(DecodeObjStackInfo *restrict decode_obj_stack_info) {
    // resize
    if (likely(SSRJSON_DECODE_OBJ_BUFFER_INIT_SIZE == decode_obj_stack_info->result_stack_end - decode_obj_stack_info->result_stack)) {
        void *new_buffer = malloc(sizeof(PyObject *) * (SSRJSON_DECODE_OBJ_BUFFER_INIT_SIZE << 1));
        if (unlikely(!new_buffer)) {
            PyErr_NoMemory();
            return false;
        }
        memcpy(new_buffer, decode_obj_stack_info->result_stack, sizeof(PyObject *) * SSRJSON_DECODE_OBJ_BUFFER_INIT_SIZE);
        decode_obj_stack_info->result_stack = (PyObject **)new_buffer;
        decode_obj_stack_info->cur_write_result_addr = decode_obj_stack_info->result_stack + SSRJSON_DECODE_OBJ_BUFFER_INIT_SIZE;
        decode_obj_stack_info->result_stack_end = decode_obj_stack_info->result_stack + (SSRJSON_DECODE_OBJ_BUFFER_INIT_SIZE << 1);
    } else {
        Py_ssize_t old_capacity = decode_obj_stack_info->result_stack_end - decode_obj_stack_info->result_stack;
        if (unlikely((PY_SSIZE_T_MAX >> 1) < old_capacity)) {
            PyErr_NoMemory();
            return false;
        }
        Py_ssize_t new_capacity = old_capacity << 1;
        void *new_buffer = realloc(decode_obj_stack_info->result_stack, sizeof(PyObject *) * new_capacity);
        if (unlikely(!new_buffer)) {
            PyErr_NoMemory();
            return false;
        }
        decode_obj_stack_info->result_stack = (PyObject **)new_buffer;
        decode_obj_stack_info->cur_write_result_addr = decode_obj_stack_info->result_stack + old_capacity;
        decode_obj_stack_info->result_stack_end = decode_obj_stack_info->result_stack + new_capacity;
    }
    return true;
}

#if !defined(Py_GIL_DISABLED)
ssrjson_align(64) u8 _DecodeTempBuffer[SSRJSON_STRING_BUFFER_SIZE];
decode_cache_t AssociativeKeyCache[SSRJSON_KEY_CACHE_SIZE];
ssrjson_align(64) u8 _DecodeBytesSrcBuffer[SSRJSON_STRING_BUFFER_SIZE];
DecodeCtnWithSize _DecodeCtnBuffer[SSRJSON_DECODE_MAX_RECURSION];
PyObject *_DecodeObjBuffer[SSRJSON_DECODE_OBJ_BUFFER_INIT_SIZE];
#endif
