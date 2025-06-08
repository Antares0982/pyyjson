#define COMPILE_CONTEXT_DECODE

#define XXH_INLINE_ALL
#include "decode_bytes.h"
#include "decode_bytes_root_wrap.h"
#include "decode_shared.h"
#include "decode_str_root_wrap.h"
#include "simd/cvt.h"
#include "simd/mask_table.h"
#include "simd/memcpy.h"
#include "simd/simd_impl.h"
#include "ssrjson.h"
#include "str/ascii.h"
#include "str/ucs.h"
#include "tls.h"

static_assert((SSRJSON_STRING_BUFFER_SIZE % 64) == 0, "(SSRJSON_STRING_BUFFER_SIZE % 64) == 0");


#if SSRJSON_ENABLE_TRACE
Py_ssize_t max_str_len = 0;
int __count_trace[SSRJSON_OP_BITCOUNT_MAX] = {0};
int __hash_trace[SSRJSON_KEY_CACHE_SIZE] = {0};
size_t __hash_hit_counter = 0;
size_t __hash_add_key_call_count = 0;

#    define SSRJSON_TRACE_STR_LEN(_len) max_str_len = max_str_len > _len ? max_str_len : _len
#    define SSRJSON_TRACE_HASH(_hash) \
        __hash_add_key_call_count++;  \
        __hash_trace[_hash & (SSRJSON_KEY_CACHE_SIZE - 1)]++
#    define SSRJSON_TRACE_CACHE_HIT() __hash_hit_counter++
#    define SSRJSON_TRACE_HASH_CONFLICT(_hash) printf("hash conflict: %lld, index=%lld\n", (long long int)_hash, (long long int)(_hash & (SSRJSON_KEY_CACHE_SIZE - 1)))
#else // SSRJSON_ENABLE_TRACE
#    define SSRJSON_TRACE_STR_LEN(_len) (void)(0)
#    define SSRJSON_TRACE_HASH(_hash) (void)(0)
#    define SSRJSON_TRACE_CACHE_HIT() (void)(0)
#    define SSRJSON_TRACE_HASH_CONFLICT(_hash) (void)(0)
#endif // SSRJSON_ENABLE_TRACE

force_inline PyObject *make_string(const u8 *unicode_str, Py_ssize_t len, int type_flag, bool is_key) {
    SSRJSON_TRACE_STR_LEN(len);
    PyObject *obj;
    decode_keyhash_t hash;
    size_t real_len;
    Py_ssize_t offset;
    Py_UCS4 max_char;
    int kind;
    bool ascii;

    switch (type_flag) {
        case SSRJSON_STRING_TYPE_ASCII: {
            ascii = true;
            kind = 1;
            max_char = 0x7f;
            real_len = len;
            offset = sizeof(PyASCIIObject);
            break;
        }
        case SSRJSON_STRING_TYPE_LATIN1: {
            ascii = false;
            kind = 1;
            max_char = 0xff;
            real_len = len;
            offset = sizeof(PyCompactUnicodeObject);
            break;
        }
        case SSRJSON_STRING_TYPE_UCS2: {
            ascii = false;
            kind = 2;
            max_char = 0xffff;
            real_len = len * 2;
            offset = sizeof(PyCompactUnicodeObject);
            break;
        }
        case SSRJSON_STRING_TYPE_UCS4: {
            ascii = false;
            kind = 4;
            max_char = 0x10ffff;
            real_len = len * 4;
            offset = sizeof(PyCompactUnicodeObject);
            break;
        }
        default:
            SSRJSON_UNREACHABLE();
    }

    bool should_cache = (is_key && real_len && likely(real_len <= 64));

    if (should_cache) {
        hash = XXH3_64bits(unicode_str, real_len);
        obj = get_key_cache(unicode_str, hash, real_len, kind, ascii);
        if (obj) {
            Py_INCREF(obj);
            return obj;
        }
    }

    obj = PyUnicode_New(len, max_char);
    if (obj == NULL) return NULL;
    ssrjson_memcpy(SSRJSON_CAST(u8 *, obj) + offset, unicode_str, real_len);
    if (should_cache) {
        add_key_cache(hash, obj);
    }
success:
    if (is_key) {
        PyASCIIObject *ascii_obj = SSRJSON_CAST(PyASCIIObject *, obj);
        if (len) {
            assert(ascii_obj->hash == -1);
            make_hash(ascii_obj, unicode_str, real_len);
        } else {
            // empty unicode has zero hash
            assert(ascii_obj->hash != -1);
        }
    }
    return obj;
}

#if SSRJSON_ENABLE_TRACE
#    define SSRJSON_TRACE_OP(x)                                 \
        do {                                                    \
            for (int i = 0; i < SSRJSON_OP_BITCOUNT_MAX; i++) { \
                if (x & (1 << i)) {                             \
                    __count_trace[i]++;                         \
                    break;                                      \
                }                                               \
            }                                                   \
            __op_counter++;                                     \
        } while (0)
#else
#    define SSRJSON_TRACE_OP(x) (void)0
#endif


bool _decode_obj_stack_resize(DecodeObjStackInfo *restrict decode_obj_stack_info);

force_inline bool push_obj(DecodeObjStackInfo *restrict decode_obj_stack_info, PyObject *obj) {
    static_assert(((Py_ssize_t)SSRJSON_DECODE_OBJ_BUFFER_INIT_SIZE << 1) > 0, "(SSRJSON_DECODE_OBJ_BUFFER_INIT_SIZE << 1) > 0");
    if (unlikely(decode_obj_stack_info->cur_write_result_addr >= decode_obj_stack_info->result_stack_end)) {
        bool c = _decode_obj_stack_resize(decode_obj_stack_info);
        RETURN_ON_UNLIKELY_ERR(!c);
    }
    *decode_obj_stack_info->cur_write_result_addr++ = obj;
    return true;
}

force_inline bool decode_arr(DecodeObjStackInfo *restrict decode_obj_stack_info, Py_ssize_t arr_len) {
    assert(arr_len >= 0);
    PyObject *list = PyList_New(arr_len);
    RETURN_ON_UNLIKELY_ERR(!list);
    PyObject **list_val_start = decode_obj_stack_info->cur_write_result_addr - arr_len;
    assert(list_val_start >= decode_obj_stack_info->result_stack);
    for (Py_ssize_t j = 0; j < arr_len; j++) {
        PyObject *val = list_val_start[j];
        assert(val);
        PyList_SET_ITEM(list, j, val); // this never fails
    }
    decode_obj_stack_info->cur_write_result_addr -= arr_len;
    return push_obj(decode_obj_stack_info, list);
}

force_inline bool decode_obj(DecodeObjStackInfo *restrict decode_obj_stack_info, Py_ssize_t dict_len) {
    PyObject *dict = _PyDict_NewPresized(dict_len);
    RETURN_ON_UNLIKELY_ERR(!dict);
    PyObject **dict_val_start = decode_obj_stack_info->cur_write_result_addr - dict_len * 2;
    PyObject **dict_val_view = dict_val_start;
    for (size_t j = 0; j < dict_len; j++) {
        PyObject *key = *dict_val_view++;
        assert(PyUnicode_Check(key));
        PyObject *val = *dict_val_view++;
        assert(((PyASCIIObject *)key)->hash != -1);
        int retcode = _PyDict_SetItem_KnownHash(dict, key, val, ((PyASCIIObject *)key)->hash); // this may fail
        if (likely(0 == retcode)) {
            Py_DecRef_NoCheck(key);
            Py_DecRef_NoCheck(val);
        } else {
            // we already decrefed some objects, have to manually handle all refcnt here
            Py_DECREF(dict);
            // also need to clean up the rest k-v pairs
            for (size_t k = j * 2; k < dict_len * 2; k++) {
                Py_DECREF(dict_val_start[k]);
            }
            // move cur_write_result_addr to the first key addr, avoid double decref
            decode_obj_stack_info->cur_write_result_addr = dict_val_start;
            return false;
        }
    }
    decode_obj_stack_info->cur_write_result_addr -= dict_len * 2;
    return push_obj(decode_obj_stack_info, dict);
}

force_inline bool decode_null(DecodeObjStackInfo *restrict decode_obj_stack_info) {
    SSRJSON_TRACE_OP(SSRJSON_OP_CONSTANTS);
    Py_Immortal_IncRef(Py_None);
    return push_obj(decode_obj_stack_info, Py_None);
}

force_inline bool decode_false(DecodeObjStackInfo *restrict decode_obj_stack_info) {
    SSRJSON_TRACE_OP(SSRJSON_OP_CONSTANTS);
    Py_Immortal_IncRef(Py_False);
    return push_obj(decode_obj_stack_info, Py_False);
}

force_inline bool decode_true(DecodeObjStackInfo *restrict decode_obj_stack_info) {
    SSRJSON_TRACE_OP(SSRJSON_OP_CONSTANTS);
    Py_Immortal_IncRef(Py_True);
    return push_obj(decode_obj_stack_info, Py_True);
}

force_inline bool decode_nan(DecodeObjStackInfo *restrict decode_obj_stack_info, bool is_signed) {
    SSRJSON_TRACE_OP(SSRJSON_OP_NAN_INF);
    PyObject *o = PyFloat_FromDouble(is_signed ? -fabs(Py_NAN) : fabs(Py_NAN));
    RETURN_ON_UNLIKELY_ERR(!o);
    return push_obj(decode_obj_stack_info, o);
}

static int invalid_arg_checked = 0;

PyObject *SIMD_NAME_MODIFIER(ssrjson_Decode)(PyObject *self, PyObject *args, PyObject *kwargs) {
    PyObject *obj;
    PyObject *ret;
    //
    PyObject *cls = NULL, *object_hook = NULL, *parse_float = NULL, *parse_int = NULL, *parse_constant = NULL, *object_pairs_hook = NULL;
    static const char *kwlist[] = {"s", "cls", "object_hook", "parse_float", "parse_int", "parse_constant", "object_pairs_hook", NULL};
    //
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|OOOOOO", (char **)kwlist, &obj, &cls, &object_hook, &parse_float, &parse_int, &parse_constant, &object_pairs_hook)) {
        return NULL;
    }

    if (!invalid_arg_checked && (cls || object_hook || parse_float || parse_int || parse_constant || object_pairs_hook)) {
        fprintf(stderr, "Warning: some options are not supported in this version of ssrjson\n");
        invalid_arg_checked = 1;
    }

    if (PyUnicode_Check(obj)) {
        PyASCIIObject *ascii_head = SSRJSON_CAST(PyASCIIObject *, obj);
        PyUnicodeObject *in_unicode = SSRJSON_CAST(PyUnicodeObject *, obj);
        int kind = ascii_head->state.ascii ? 0 : ascii_head->state.kind;
        switch (kind) {
            case SSRJSON_STRING_TYPE_ASCII: {
                ret = decode_ascii(in_unicode);
                break;
            }
            case SSRJSON_STRING_TYPE_LATIN1: {
                ret = decode_ucs1(in_unicode);
                break;
            }
            case SSRJSON_STRING_TYPE_UCS2: {
                ret = decode_ucs2(in_unicode);
                break;
            }
            case SSRJSON_STRING_TYPE_UCS4: {
                ret = decode_ucs4(in_unicode);
                break;
            }
            default: {
                ret = NULL;
                SSRJSON_UNREACHABLE();
            }
        }
        goto done;
    }

    if (PyBytes_Check(obj)) {
        char *buffer;
        Py_ssize_t length;
        if (unlikely(0 != PyBytes_AsStringAndSize(obj, &buffer, &length))) {
            ret = NULL;
            goto done;
        }
        ret = ssrjson_decode_bytes(buffer, length);
        goto done;
    }

    if (PyByteArray_Check(obj)) {
        char *buffer = PyByteArray_AS_STRING(obj);
        Py_ssize_t length = PyByteArray_GET_SIZE(obj);
        ret = ssrjson_decode_bytes(buffer, length);
        goto done;
    }

fail:;
    ret = NULL;
    PyErr_SetString(PyExc_TypeError, "Invalid argument");

done:;
    if (unlikely(!ret && !PyErr_Occurred())) {
        PyErr_SetString(JSONDecodeError, "Failed to decode JSON: unknown error");
    }
    return ret;
}
