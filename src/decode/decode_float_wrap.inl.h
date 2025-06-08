#ifndef SSRJSON_DECODE_DECODE_FLOAT_WRAP_H
#define SSRJSON_DECODE_DECODE_FLOAT_WRAP_H

#include "decode_float_utils.h"

#define COMPILE_READ_UCS_LEVEL 1
#include "decode_float.inl.h"
#undef COMPILE_READ_UCS_LEVEL

#define COMPILE_READ_UCS_LEVEL 2
#include "decode_float.inl.h"
#undef COMPILE_READ_UCS_LEVEL

#define COMPILE_READ_UCS_LEVEL 4
#include "decode_float.inl.h"
#undef COMPILE_READ_UCS_LEVEL

#endif // SSRJSON_DECODE_DECODE_FLOAT_WRAP_H
