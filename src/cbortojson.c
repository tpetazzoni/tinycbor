/****************************************************************************
**
** Copyright (C) 2015 Intel Corporation
**
** Permission is hereby granted, free of charge, to any person obtaining a copy
** of this software and associated documentation files (the "Software"), to deal
** in the Software without restriction, including without limitation the rights
** to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
** copies of the Software, and to permit persons to whom the Software is
** furnished to do so, subject to the following conditions:
**
** The above copyright notice and this permission notice shall be included in
** all copies or substantial portions of the Software.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
** AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
** LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
** OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
** THE SOFTWARE.
**
****************************************************************************/

#define _BSD_SOURCE 1
#define _GNU_SOURCE 1
#include "cbor.h"
#include "cborjson.h"
#include "compilersupport_p.h"

#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CborError value_to_json(FILE *out, CborValue *it, int flags, CborType type);

static CborError generic_dump_base64(char **result, CborValue *it, const char alphabet[65])
{
    size_t n = 0;
    uint8_t *buffer, *out, *in;
    CborError err = cbor_value_calculate_string_length(it, &n);
    if (err)
        return err;

    // a Base64 output (untruncated) has 4 bytes for every 3 in the input
    size_t len = (n + 5) / 3 * 4;
    out = buffer = (uint8_t *)malloc(len + 1);
    *result = (char *)buffer;

    // we read our byte string at the tail end of the buffer
    // so we can do an in-place conversion while iterating forwards
    in = buffer + len - n;

    // let cbor_value_copy_byte_string know we have an extra byte for the terminating NUL
    ++n;
    err = cbor_value_copy_byte_string(it, in, &n, it);
    assert(err == CborNoError);

    uint_least32_t val;
    for ( ; n >= 3; n -= 3, in += 3) {
        // read 3 bytes x 8 bits = 24 bits
        val = (in[0] << 16) | (in[1] << 8) | in[2];

        // write 4 chars x 6 bits = 24 bits
        *out++ = alphabet[(val >> 18) & 0x3f];
        *out++ = alphabet[(val >> 12) & 0x3f];
        *out++ = alphabet[(val >> 6) & 0x3f];
        *out++ = alphabet[val & 0x3f];
    }

    // maybe 1 or 2 bytes left
    if (n) {
        val = in[0] << 16;

        // the 65th character in the alphabet is our filler: either '=' or '\0'
        out[4] = '\0';
        out[3] = alphabet[64];
        if (n == 2) {
            // read 2 bytes x 8 bits = 16 bits
            val |= (in[1] << 8);

            // write the third char in 3 chars x 6 bits = 18 bits
            out[2] = alphabet[(val >> 6) & 0x3f];
        } else {
            out[2] = alphabet[64];  // filler
        }
        out[1] = alphabet[(val >> 12) & 0x3f];
        out[0] = alphabet[(val >> 18) & 0x3f];
    } else {
        out[0] = '\0';
    }

    return CborNoError;
}

static CborError dump_bytestring_base64url(char **result, CborValue *it)
{
    static const char alphabet[] = "ABCDEFGH" "IJKLMNOP" "QRSTUVWX" "YZabcdef"
                                   "ghijklmn" "opqrstuv" "wxyz0123" "456789-_";
    return generic_dump_base64(result, it, alphabet);
}

static CborError stringify_map_key(char **key, CborValue *it, int flags, CborType type)
{
    (void)flags;
    switch (type) {
    case CborArrayType:
    case CborMapType:
        // can't convert these
        return CborErrorJsonObjectKeyIsAggregate;

    case CborIntegerType:
        if (cbor_value_is_unsigned_integer(it)) {
            uint64_t val;
            cbor_value_get_uint64(it, &val);
            asprintf(key, "%" PRIu64, val);
        } else {
            int64_t val;
            cbor_value_get_int64(it, &val);     // can't fail
            if (val < 0)
                asprintf(key, "%" PRIi64, val);
            else
                asprintf(key, "-%" PRIu64, (uint64_t)(-val - 1));
        }
        break;

    case CborByteStringType:
        return dump_bytestring_base64url(key, it);

    case CborTextStringType:
        unreachable();
        return CborErrorInternalError;

    case CborTagType: {
        CborTag tag;
        cbor_value_get_tag(it, &tag);       // can't fail
        return CborErrorUnsupportedType;
    }

    case CborSimpleType: {
        uint8_t type;
        cbor_value_get_simple_type(it, &type);  // can't fail
        asprintf(key, "simple(%" PRIu8 ")", type) ? CborErrorOutOfMemory : CborNoError;
        break;
    }

    case CborNullType:
        *key = strdup("null");
        break;

    case CborUndefinedType:
        *key = strdup("undefined");
        break;

    case CborBooleanType: {
        bool val;
        cbor_value_get_boolean(it, &val);       // can't fail
        *key = strdup(val ? "true" : "false");
        break;
    }

    case CborDoubleType: {
        double val;
        if (false) {
            float f;
    case CborFloatType:
            cbor_value_get_float(it, &f);
            val = f;
        } else {
            cbor_value_get_double(it, &val);
        }

        if (isnan(val) || isinf(val))
            *key = strdup("null");
        else
            asprintf(key, "%.19g", val);
        break;
    }

    case CborHalfFloatType:
        return CborErrorUnsupportedType;

    case CborInvalidType:
        return CborErrorUnknownType;
    }

    if (*key == NULL)
        return CborErrorOutOfMemory;
    return cbor_value_advance_fixed(it);
}

static CborError array_to_json(FILE *out, CborValue *it, int flags)
{
    const char *comma = "";
    while (!cbor_value_at_end(it)) {
        if (fprintf(out, "%s", comma) < 0)
            return CborErrorIO;
        comma = ",";

        CborError err = value_to_json(out, it, flags, cbor_value_get_type(it));
        if (err)
            return err;
    }
    return CborNoError;
}

static CborError map_to_json(FILE *out, CborValue *it, int flags)
{
    const char *comma = "";
    CborError err;
    while (!cbor_value_at_end(it)) {
        char *key;
        if (fprintf(out, "%s", comma) < 0)
            return CborErrorIO;
        comma = ",";

        CborType keyType = cbor_value_get_type(it);
        if (likely(keyType == CborTextStringType)) {
            size_t n = 0;
            err = cbor_value_dup_text_string(it, &key, &n, it);
        } else if (flags & CborConvertStringifyMapKeys) {
            err = stringify_map_key(&key, it, flags, keyType);
        } else {
            return CborErrorJsonObjectKeyNotString;
        }
        if (err)
            return err;

        // first, print the key
        if (fprintf(out, "\"%s\":", key) < 0)
            return CborErrorIO;

        // then, print the value
        err = value_to_json(out, it, flags, cbor_value_get_type(it));

        free(key);
        if (err)
            return err;
    }
    return CborNoError;
}

static CborError value_to_json(FILE *out, CborValue *it, int flags, CborType type)
{
    CborError err;
    switch (type) {
    case CborArrayType:
    case CborMapType: {
        // recursive type
        CborValue recursed;
        err = cbor_value_enter_container(it, &recursed);
        if (err) {
            it->ptr = recursed.ptr;
            return err;       // parse error
        }
        if (fputc(type == CborArrayType ? '[' : '{', out) < 0)
            return CborErrorIO;

        err = (type == CborArrayType) ?
                  array_to_json(out, &recursed, flags) :
                  map_to_json(out, &recursed, flags);
        if (err) {
            it->ptr = recursed.ptr;
            return err;       // parse error
        }

        if (fputc(type == CborArrayType ? ']' : '}', out) < 0)
            return CborErrorIO;
        err = cbor_value_leave_container(it, &recursed);
        if (err)
            return err;       // parse error

        return CborNoError;
    }

    case CborIntegerType: {
        double num;     // JS numbers are IEEE double precision
        uint64_t val;
        cbor_value_get_raw_integer(it, &val);    // can't fail
        num = val;

        if (cbor_value_is_negative_integer(it)) {
            num = -num - 1;                     // convert to negative
        }
        if (fprintf(out, "%.0f", num) < 0)  // this number has no fraction, so no decimal points please
            return CborErrorIO;
        break;
    }

    case CborByteStringType:
    case CborTextStringType: {
        char *str;
        if (type == CborByteStringType) {
            err = dump_bytestring_base64url(&str, it);
        } else {
            size_t n = 0;
            err = cbor_value_dup_text_string(it, &str, &n, it);
        }
        if (err)
            return err;
        err = (fprintf(out, "\"%s\"", str) < 0) ? CborErrorIO : CborNoError;
        free(str);
        return err;
    }

    case CborTagType: {
        CborTag tag;
        cbor_value_get_tag(it, &tag);       // can't fail
        return CborErrorUnsupportedType;
    }

    case CborSimpleType: {
        uint8_t simple_type;
        cbor_value_get_simple_type(it, &simple_type);  // can't fail
        if (fprintf(out, "\"simple(%" PRIu8 ")\"", simple_type) < 0)
            return CborErrorIO;
        break;
    }

    case CborNullType:
        if (fprintf(out, "null") < 0)
            return CborErrorIO;
        break;

    case CborUndefinedType:
        if (fprintf(out, "\"undefined\"") < 0)
            return CborErrorIO;
        break;

    case CborBooleanType: {
        bool val;
        cbor_value_get_boolean(it, &val);       // can't fail
        if (fprintf(out, val ? "true" : "false") < 0)
            return CborErrorIO;
        break;
    }

    case CborDoubleType: {
        double val;
        if (false) {
            float f;
    case CborFloatType:
            cbor_value_get_float(it, &f);
            val = f;
        } else {
            cbor_value_get_double(it, &val);
        }

        if (isinf(val) || isnan(val)) {
            if (fprintf(out, "null") < 0)
                return CborErrorIO;
        } else {
            uint64_t ival = (uint64_t)fabs(val);
            int r;
            if ((double)ival == fabs(val)) {
                // print as integer so we get the full precision
                r = fprintf(out, "%s%" PRIu64, val < 0 ? "-" : "", ival);
            } else {
                // this number is definitely not a 64-bit integer
                r = fprintf(out, "%." DBL_DECIMAL_DIG_STR "g", val);
            }
            if (r < 0)
                return CborErrorIO;
        }
        break;
    }

    case CborHalfFloatType:
        return CborErrorUnsupportedType;

    case CborInvalidType:
        return CborErrorUnknownType;
    }

    return cbor_value_advance_fixed(it);
}

CborError cbor_value_to_json_advance(FILE *out, CborValue *value, int flags)
{
    return value_to_json(out, value, flags, cbor_value_get_type(value));
}
