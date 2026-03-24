/** Copyright (c) 2026-2036, Jin.Wu <wujin.developer@gmail.com>
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 */

#include <share.h>
#include "platform/platform-io.h"

#include <sys/types.h>
#include <sys/stat.h>

FILE* platform_io_fopen(const char* restrict file, const char* restrict mode) {
    return _fsopen(file, mode, _SH_DENYNO);
}

int platform_io_vsprintf(char* str, size_t size, const char* restrict format, va_list ap) {
    return vsprintf_s(str, size, format, ap);
}

int platform_io_stat(const char* path, platform_io_stat_t* out) {
    struct __stat64 st;
    if (_stat64(path, &st) != 0) {
        return -1;
    }
    out->size   = st.st_size;
    out->mtime  = st.st_mtime;
    out->is_dir = (st.st_mode & _S_IFDIR) != 0;
    return 0;
}


