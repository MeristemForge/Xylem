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

#include "xylem/xylem-json.h"

#include <stdlib.h>
#include <string.h>

#include "yyjson/yyjson.h"

#define JSON_MAX_CHILDREN 64

typedef struct _root_s _root_t;

/**
 * Internal handle.
 *
 * For parsed JSON (read-only): imut_doc is set, mut_doc is NULL.
 * For constructed JSON (mutable): mut_doc is set, imut_doc is NULL.
 * Sub-handles borrow from the root and own nothing.
 */
struct xylem_json_s {
    yyjson_doc*     imut_doc;
    yyjson_val*     imut_val;
    yyjson_mut_doc* mut_doc;
    yyjson_mut_val* mut_val;
    _root_t*        root;
};

/**
 * Root allocation: the handle itself followed by a pool of sub-handles.
 * All borrowed children share the root's lifetime.
 */
struct _root_s {
    struct xylem_json_s handle;
    struct xylem_json_s children[JSON_MAX_CHILDREN];
    size_t              count;
};

static struct xylem_json_s* _json_borrow_imut(_root_t* root,
                                              yyjson_val* val) {
    if (!root || !val || root->count >= JSON_MAX_CHILDREN) {
        return NULL;
    }
    struct xylem_json_s* child = &root->children[root->count++];
    memset(child, 0, sizeof(*child));
    child->imut_val = val;
    child->root     = root;
    return child;
}

static struct xylem_json_s* _json_borrow_mut(_root_t* root,
                                             yyjson_mut_doc* doc,
                                             yyjson_mut_val* val) {
    if (!root || !val || root->count >= JSON_MAX_CHILDREN) {
        return NULL;
    }
    struct xylem_json_s* child = &root->children[root->count++];
    memset(child, 0, sizeof(*child));
    child->mut_doc = doc;
    child->mut_val = val;
    child->root    = root;
    return child;
}

static _root_t* _json_root(const xylem_json_t* j) {
    if (!j) {
        return NULL;
    }
    return j->root;
}

static yyjson_val* _json_get_imut(const xylem_json_t* j, const char* key) {
    if (!j || !j->imut_val || !key || !yyjson_is_obj(j->imut_val)) {
        return NULL;
    }
    return yyjson_obj_get(j->imut_val, key);
}

static yyjson_mut_val* _json_get_mut(const xylem_json_t* j, const char* key) {
    if (!j || !j->mut_val || !key || !yyjson_mut_is_obj(j->mut_val)) {
        return NULL;
    }
    return yyjson_mut_obj_get(j->mut_val, key);
}

/* Deep-copy a value into a mutable doc, handling both imut and mut sources. */
static yyjson_mut_val* _json_copy_val(yyjson_mut_doc* doc,
                                      const xylem_json_t* src) {
    if (!doc || !src) {
        return NULL;
    }
    if (src->mut_val) {
        return yyjson_mut_val_mut_copy(doc, src->mut_val);
    }
    if (src->imut_val) {
        return yyjson_val_mut_copy(doc, src->imut_val);
    }
    return NULL;
}

static xylem_json_t* _json_new_root(yyjson_mut_val* val,
                                    yyjson_mut_doc* doc) {
    _root_t* r = (_root_t*)calloc(1, sizeof(_root_t));
    if (!r) {
        return NULL;
    }
    r->handle.mut_doc = doc;
    r->handle.mut_val = val;
    r->handle.root    = r;
    return &r->handle;
}

xylem_json_t* xylem_json_parse(const char* str) {
    if (!str) {
        return NULL;
    }
    size_t len = strlen(str);
    yyjson_doc* doc = yyjson_read(str, len, 0);
    if (!doc) {
        return NULL;
    }
    _root_t* r = (_root_t*)calloc(1, sizeof(_root_t));
    if (!r) {
        yyjson_doc_free(doc);
        return NULL;
    }
    r->handle.imut_doc = doc;
    r->handle.imut_val = yyjson_doc_get_root(doc);
    r->handle.root     = r;
    return &r->handle;
}

void xylem_json_destroy(xylem_json_t* j) {
    if (!j || !j->root) {
        return;
    }
    _root_t* r = j->root;
    /* Only the root handle may free the document. */
    if (j != &r->handle) {
        return;
    }
    if (r->handle.imut_doc) {
        yyjson_doc_free(r->handle.imut_doc);
    }
    if (r->handle.mut_doc) {
        yyjson_mut_doc_free(r->handle.mut_doc);
    }
    free(r);
}

char* xylem_json_print(const xylem_json_t* j) {
    if (!j) {
        return NULL;
    }
    if (j->imut_val) {
        return yyjson_val_write(j->imut_val, YYJSON_WRITE_NOFLAG, NULL);
    }
    if (j->mut_val) {
        return yyjson_mut_val_write(j->mut_val, YYJSON_WRITE_NOFLAG, NULL);
    }
    return NULL;
}

char* xylem_json_print_pretty(const xylem_json_t* j) {
    if (!j) {
        return NULL;
    }
    if (j->imut_val) {
        return yyjson_val_write(j->imut_val, YYJSON_WRITE_PRETTY, NULL);
    }
    if (j->mut_val) {
        return yyjson_mut_val_write(j->mut_val, YYJSON_WRITE_PRETTY, NULL);
    }
    return NULL;
}

xylem_json_type_t xylem_json_type(const xylem_json_t* j) {
    if (!j) {
        return XYLEM_JSON_TYPE_NONE;
    }
    yyjson_type t;
    if (j->imut_val) {
        t = yyjson_get_type(j->imut_val);
    } else if (j->mut_val) {
        t = yyjson_mut_get_type(j->mut_val);
    } else {
        return XYLEM_JSON_TYPE_NONE;
    }
    switch (t) {
    case YYJSON_TYPE_NULL: return XYLEM_JSON_TYPE_NULL;
    case YYJSON_TYPE_BOOL: return XYLEM_JSON_TYPE_BOOL;
    case YYJSON_TYPE_NUM:  return XYLEM_JSON_TYPE_NUM;
    case YYJSON_TYPE_STR:  return XYLEM_JSON_TYPE_STR;
    case YYJSON_TYPE_ARR:  return XYLEM_JSON_TYPE_ARR;
    case YYJSON_TYPE_OBJ:  return XYLEM_JSON_TYPE_OBJ;
    default:               return XYLEM_JSON_TYPE_NONE;
    }
}

const char* xylem_json_str(const xylem_json_t* j, const char* key) {
    yyjson_val* v = _json_get_imut(j, key);
    if (v) {
        return yyjson_get_str(v);
    }
    yyjson_mut_val* mv = _json_get_mut(j, key);
    if (mv) {
        return yyjson_mut_get_str(mv);
    }
    return NULL;
}

int32_t xylem_json_i32(const xylem_json_t* j, const char* key) {
    yyjson_val* v = _json_get_imut(j, key);
    if (v) {
        return (int32_t)yyjson_get_sint(v);
    }
    yyjson_mut_val* mv = _json_get_mut(j, key);
    if (mv) {
        return (int32_t)yyjson_mut_get_sint(mv);
    }
    return 0;
}

int64_t xylem_json_i64(const xylem_json_t* j, const char* key) {
    yyjson_val* v = _json_get_imut(j, key);
    if (v) {
        return yyjson_get_sint(v);
    }
    yyjson_mut_val* mv = _json_get_mut(j, key);
    if (mv) {
        return yyjson_mut_get_sint(mv);
    }
    return 0;
}

double xylem_json_f64(const xylem_json_t* j, const char* key) {
    yyjson_val* v = _json_get_imut(j, key);
    if (v) {
        return yyjson_get_num(v);
    }
    yyjson_mut_val* mv = _json_get_mut(j, key);
    if (mv) {
        return yyjson_mut_get_num(mv);
    }
    return 0.0;
}

bool xylem_json_bool(const xylem_json_t* j, const char* key) {
    yyjson_val* v = _json_get_imut(j, key);
    if (v) {
        return yyjson_get_bool(v);
    }
    yyjson_mut_val* mv = _json_get_mut(j, key);
    if (mv) {
        return yyjson_mut_get_bool(mv);
    }
    return false;
}

xylem_json_t* xylem_json_obj(const xylem_json_t* j, const char* key) {
    _root_t* r = _json_root(j);
    yyjson_val* v = _json_get_imut(j, key);
    if (v && yyjson_is_obj(v)) {
        return _json_borrow_imut(r, v);
    }
    yyjson_mut_val* mv = _json_get_mut(j, key);
    if (mv && yyjson_mut_is_obj(mv)) {
        return _json_borrow_mut(r, j->mut_doc, mv);
    }
    return NULL;
}

xylem_json_t* xylem_json_arr(const xylem_json_t* j, const char* key) {
    _root_t* r = _json_root(j);
    yyjson_val* v = _json_get_imut(j, key);
    if (v && yyjson_is_arr(v)) {
        return _json_borrow_imut(r, v);
    }
    yyjson_mut_val* mv = _json_get_mut(j, key);
    if (mv && yyjson_mut_is_arr(mv)) {
        return _json_borrow_mut(r, j->mut_doc, mv);
    }
    return NULL;
}

size_t xylem_json_arr_len(const xylem_json_t* j) {
    if (!j) {
        return 0;
    }
    if (j->imut_val) {
        return yyjson_arr_size(j->imut_val);
    }
    if (j->mut_val) {
        return yyjson_mut_arr_size(j->mut_val);
    }
    return 0;
}

xylem_json_t* xylem_json_arr_get(const xylem_json_t* j, size_t index) {
    if (!j) {
        return NULL;
    }
    _root_t* r = _json_root(j);
    if (j->imut_val) {
        yyjson_val* v = yyjson_arr_get(j->imut_val, index);
        return _json_borrow_imut(r, v);
    }
    if (j->mut_val) {
        yyjson_mut_val* v = yyjson_mut_arr_get(j->mut_val, index);
        return _json_borrow_mut(r, j->mut_doc, v);
    }
    return NULL;
}

xylem_json_t* xylem_json_new_obj(void) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        return NULL;
    }
    yyjson_mut_val* val = yyjson_mut_obj(doc);
    if (!val) {
        yyjson_mut_doc_free(doc);
        return NULL;
    }
    yyjson_mut_doc_set_root(doc, val);
    xylem_json_t* j = _json_new_root(val, doc);
    if (!j) {
        yyjson_mut_doc_free(doc);
    }
    return j;
}

xylem_json_t* xylem_json_new_arr(void) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        return NULL;
    }
    yyjson_mut_val* val = yyjson_mut_arr(doc);
    if (!val) {
        yyjson_mut_doc_free(doc);
        return NULL;
    }
    yyjson_mut_doc_set_root(doc, val);
    xylem_json_t* j = _json_new_root(val, doc);
    if (!j) {
        yyjson_mut_doc_free(doc);
    }
    return j;
}

int xylem_json_set_str(xylem_json_t* j, const char* key, const char* val) {
    if (!j || !j->mut_doc || !j->mut_val || !key || !val) {
        return -1;
    }
    bool ok = yyjson_mut_obj_add_strcpy(j->mut_doc, j->mut_val, key, val);
    return ok ? 0 : -1;
}

int xylem_json_set_i32(xylem_json_t* j, const char* key, int32_t val) {
    if (!j || !j->mut_doc || !j->mut_val || !key) {
        return -1;
    }
    bool ok = yyjson_mut_obj_add_sint(j->mut_doc, j->mut_val, key,
                                      (int64_t)val);
    return ok ? 0 : -1;
}

int xylem_json_set_i64(xylem_json_t* j, const char* key, int64_t val) {
    if (!j || !j->mut_doc || !j->mut_val || !key) {
        return -1;
    }
    bool ok = yyjson_mut_obj_add_sint(j->mut_doc, j->mut_val, key, val);
    return ok ? 0 : -1;
}

int xylem_json_set_f64(xylem_json_t* j, const char* key, double val) {
    if (!j || !j->mut_doc || !j->mut_val || !key) {
        return -1;
    }
    bool ok = yyjson_mut_obj_add_real(j->mut_doc, j->mut_val, key, val);
    return ok ? 0 : -1;
}

int xylem_json_set_bool(xylem_json_t* j, const char* key, bool val) {
    if (!j || !j->mut_doc || !j->mut_val || !key) {
        return -1;
    }
    bool ok = yyjson_mut_obj_add_bool(j->mut_doc, j->mut_val, key, val);
    return ok ? 0 : -1;
}

int xylem_json_set_null(xylem_json_t* j, const char* key) {
    if (!j || !j->mut_doc || !j->mut_val || !key) {
        return -1;
    }
    bool ok = yyjson_mut_obj_add_null(j->mut_doc, j->mut_val, key);
    return ok ? 0 : -1;
}

int xylem_json_set_obj(xylem_json_t* j, const char* key,
                       xylem_json_t* child) {
    if (!j || !j->mut_doc || !j->mut_val || !key || !child) {
        return -1;
    }
    yyjson_mut_val* cv = _json_copy_val(j->mut_doc, child);
    if (!cv) {
        return -1;
    }
    bool ok = yyjson_mut_obj_add_val(j->mut_doc, j->mut_val, key, cv);
    if (ok) {
        xylem_json_destroy(child);
    }
    return ok ? 0 : -1;
}

int xylem_json_set_arr(xylem_json_t* j, const char* key,
                       xylem_json_t* child) {
    if (!j || !j->mut_doc || !j->mut_val || !key || !child) {
        return -1;
    }
    yyjson_mut_val* cv = _json_copy_val(j->mut_doc, child);
    if (!cv) {
        return -1;
    }
    bool ok = yyjson_mut_obj_add_val(j->mut_doc, j->mut_val, key, cv);
    if (ok) {
        xylem_json_destroy(child);
    }
    return ok ? 0 : -1;
}

int xylem_json_arr_push(xylem_json_t* j, xylem_json_t* item) {
    if (!j || !j->mut_doc || !j->mut_val || !item) {
        return -1;
    }
    yyjson_mut_val* val = _json_copy_val(j->mut_doc, item);
    if (!val) {
        return -1;
    }
    bool ok = yyjson_mut_arr_append(j->mut_val, val);
    if (ok) {
        xylem_json_destroy(item);
    }
    return ok ? 0 : -1;
}

int xylem_json_arr_push_str(xylem_json_t* j, const char* val) {
    if (!j || !j->mut_doc || !j->mut_val || !val) {
        return -1;
    }
    bool ok = yyjson_mut_arr_add_strcpy(j->mut_doc, j->mut_val, val);
    return ok ? 0 : -1;
}

int xylem_json_arr_push_i64(xylem_json_t* j, int64_t val) {
    if (!j || !j->mut_doc || !j->mut_val) {
        return -1;
    }
    bool ok = yyjson_mut_arr_add_sint(j->mut_doc, j->mut_val, val);
    return ok ? 0 : -1;
}

int xylem_json_arr_push_f64(xylem_json_t* j, double val) {
    if (!j || !j->mut_doc || !j->mut_val) {
        return -1;
    }
    bool ok = yyjson_mut_arr_add_real(j->mut_doc, j->mut_val, val);
    return ok ? 0 : -1;
}

int xylem_json_arr_push_bool(xylem_json_t* j, bool val) {
    if (!j || !j->mut_doc || !j->mut_val) {
        return -1;
    }
    bool ok = yyjson_mut_arr_add_bool(j->mut_doc, j->mut_val, val);
    return ok ? 0 : -1;
}
