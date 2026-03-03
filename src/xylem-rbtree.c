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

#include "xylem.h"

#define RB_RED 0
#define RB_BLACK 1

static inline void
_rbtree_rotate_left(xylem_rbtree_node_t* node, xylem_rbtree_t* tree) {
    xylem_rbtree_node_t* right = node->right;

    if ((node->right = right->left)) {
        right->left->parent = node;
    }
    right->left = node;
    if ((right->parent = node->parent)) {
        if (node == node->parent->left) {
            node->parent->left = right;
        } else {
            node->parent->right = right;
        }
    } else {
        tree->root = right;
    }
    node->parent = right;
}

static inline void
_rbtree_rotate_right(xylem_rbtree_node_t* node, xylem_rbtree_t* tree) {
    xylem_rbtree_node_t* left = node->left;

    if ((node->left = left->right)) {
        left->right->parent = node;
    }
    left->right = node;
    if ((left->parent = node->parent)) {
        if (node == node->parent->right) {
            node->parent->right = left;
        } else {
            node->parent->left = left;
        }
    } else {
        tree->root = left;
    }
    node->parent = left;
}

static inline void _rbtree_link_node(
    xylem_rbtree_node_t*  node,
    xylem_rbtree_node_t*  parent,
    xylem_rbtree_node_t** rb_link) {
    node->parent = parent;
    node->color = RB_RED;
    node->left = node->right = NULL;
    *rb_link = node;
}

static void
_rbtree_insert_color(xylem_rbtree_t* tree, xylem_rbtree_node_t* node) {
    xylem_rbtree_node_t *parent, *gparent;

    while ((parent = node->parent) && parent->color == RB_RED) {
        gparent = parent->parent;

        /* Case A: Parent is left child of grandparent */
        if (parent == gparent->left) {
            xylem_rbtree_node_t* uncle = gparent->right;

            /**
             * Subcase A1: Uncle is RED or NULL
             * Action: Recolor and move conflict upward.
             *
             * Before recoloring:
             *        (B) gparent
             *        /          \
             *    (R) parent   (R) uncle
             *      /
             *   (R) node   <- newly inserted
             *
             * After recoloring:
             *        (R) gparent   <- may cause new violation with its parent
             *        /          \
             *    (B) parent   (B) uncle
             *      /
             *   (R) node
             */
            if (uncle && uncle->color == RB_RED) {
                uncle->color = RB_BLACK;
                parent->color = RB_BLACK;
                gparent->color = RB_RED;
                node = gparent;
                continue;
            }

            /**
             * Subcase A2: Uncle is BLACK or NULL
             * Two possibilities based on node's position.
             *
             * If node is RIGHT child of parent -> "triangle" case:
             *        (B) gparent
             *        /
             *    (R) parent
             *          \
             *        (R) node
             *
             * Step 1: Left-rotate at parent -> becomes "line" case:
             *        (B) gparent
             *        /
             *    (R) node
             *      /
             * (R) parent
             */
            if (parent->right == node) {
                xylem_rbtree_node_t* tmp;

                _rbtree_rotate_left(parent, tree);
                tmp = parent;
                parent = node;
                node = tmp;
            }

            /**
             * Now in "line" case (node is LEFT child of parent):
             *        (B) gparent
             *        /
             *    (R) parent
             *      /
             * (R) node
             *
             * Step 2: Recolor + right-rotate at gparent:
             * - Set parent to BLACK
             * - Set gparent to RED
             * - Right-rotate gparent
             *
             * Result:
             *        (B) parent
             *        /        \
             *   (R) node   (R) gparent
             *                     \
             *                   (B) uncle (unchanged)
             *
             * No more red-red conflict. Black-height preserved.
             */
            parent->color = RB_BLACK;
            gparent->color = RB_RED;
            _rbtree_rotate_right(gparent, tree);
        } else {
            /* Case B: Parent is right child of grandparent (symmetric) */
            xylem_rbtree_node_t* uncle = gparent->left;

            /**
             * Subcase B1: Uncle is RED
             * Same logic as A1, mirrored.
             */
            if (uncle && uncle->color == RB_RED) {
                uncle->color = RB_BLACK;
                parent->color = RB_BLACK;
                gparent->color = RB_RED;
                node = gparent;
                continue;
            }

            /**
             * Subcase B2: Uncle is BLACK
             * If node is LEFT child -> "triangle", do right rotation first.
             */
            if (parent->left == node) {
                xylem_rbtree_node_t* tmp;
                _rbtree_rotate_right(parent, tree);
                tmp = parent;
                parent = node;
                node = tmp;
            }

            /**
             * "Line" case: node is RIGHT child of parent.
             * Recolor + left-rotate at gparent.
             */
            parent->color = RB_BLACK;
            gparent->color = RB_RED;
            _rbtree_rotate_left(gparent, tree);
        }
    }
    /* root must be BLACK */
    tree->root->color = RB_BLACK;
}

static void _rbtree_erase_color(
    xylem_rbtree_node_t* node,
    xylem_rbtree_node_t* parent,
    xylem_rbtree_t*      tree) {
    while ((!node || node->color == RB_BLACK) && node != tree->root) {
        if (parent->left == node) {
            xylem_rbtree_node_t* other = parent->right;

            if (other->color == RB_RED) {
                other->color = RB_BLACK;
                parent->color = RB_RED;
                _rbtree_rotate_left(parent, tree);
                other = parent->right;
            }
            if ((!other->left || other->left->color == RB_BLACK) &&
                (!other->right || other->right->color == RB_BLACK)) {
                other->color = RB_RED;
                node = parent;
                parent = node->parent;
            } else {
                if (!other->right || other->right->color == RB_BLACK) {
                    other->left->color = RB_BLACK;
                    other->color = RB_RED;
                    _rbtree_rotate_right(other, tree);
                    other = parent->right;
                }
                other->color = parent->color;
                parent->color = RB_BLACK;
                other->right->color = RB_BLACK;
                _rbtree_rotate_left(parent, tree);
                node = tree->root;
                break;
            }
        } else {
            xylem_rbtree_node_t* other = parent->left;

            if (other->color == RB_RED) {
                other->color = RB_BLACK;
                parent->color = RB_RED;
                _rbtree_rotate_right(parent, tree);
                other = parent->left;
            }

            if ((!other->left || other->left->color == RB_BLACK) &&
                (!other->right || other->right->color == RB_BLACK)) {
                other->color = RB_RED;
                node = parent;
                parent = node->parent;
            } else {
                if (!other->left || other->left->color == RB_BLACK) {
                    other->right->color = RB_BLACK;
                    other->color = RB_RED;
                    _rbtree_rotate_left(other, tree);
                    other = parent->left;
                }
                other->color = parent->color;
                parent->color = RB_BLACK;
                other->left->color = RB_BLACK;
                _rbtree_rotate_right(parent, tree);
                node = tree->root;
                break;
            }
        }
    }
    if (node) {
        node->color = RB_BLACK;
    }
}

void xylem_rbtree_init(
    xylem_rbtree_t*          tree,
    xylem_rbtree_cmp_nn_fn_t cmp_nn,
    xylem_rbtree_cmp_kn_fn_t cmp_kn) {
    tree->root = NULL;
    tree->cmp_nn = cmp_nn;
    tree->cmp_kn = cmp_kn;
}

void
xylem_rbtree_insert(xylem_rbtree_t* tree, xylem_rbtree_node_t* node) {
    xylem_rbtree_node_t** p = &(tree->root);
    xylem_rbtree_node_t*  parent = NULL;

    while (*p) {
        parent = *p;
        int r = tree->cmp_nn(
            (const xylem_rbtree_node_t*)node,
            (const xylem_rbtree_node_t*)parent);
        if (r < 0) {
            p = &(*p)->left;
        } else if (r > 0) {
            p = &(*p)->right;
        } else {
            return;
        }
    }
    _rbtree_link_node(node, parent, p);
    _rbtree_insert_color(tree, node);
}

void xylem_rbtree_erase(xylem_rbtree_t* tree, xylem_rbtree_node_t* node) {
    xylem_rbtree_node_t *child, *parent;
    int                  color;

    if (!node->left) {
        child = node->right;
    } else if (!node->right) {
        child = node->left;
    } else {
        xylem_rbtree_node_t *old = node, *left;
        node = node->right;
        while ((left = node->left)) {
            node = left;
        }
        if (old->parent) {
            if (old->parent->left == old) {
                old->parent->left = node;
            } else {
                old->parent->right = node;
            }
        } else {
            tree->root = node;
        }
        child = node->right;
        parent = node->parent;
        color = node->color;
        if (parent == old) {
            parent = node;
        } else {
            if (child) {
                child->parent = parent;
            }
            parent->left = child;
            node->right = old->right;
            old->right->parent = node;
        }
        node->parent = old->parent;
        node->color = old->color;
        node->left = old->left;
        old->left->parent = node;

        goto color;
    }
    parent = node->parent;
    color = node->color;
    if (child) {
        child->parent = parent;
    }
    if (parent) {
        if (parent->left == node) {
            parent->left = child;
        } else {
            parent->right = child;
        }
    } else {
        tree->root = child;
    }
color:
    if (color == RB_BLACK) {
        _rbtree_erase_color(child, parent, tree);
    }
}

bool xylem_rbtree_empty(xylem_rbtree_t* tree) {
    return (tree->root == NULL);
}

xylem_rbtree_node_t* xylem_rbtree_find(xylem_rbtree_t* tree, const void* key) {
    xylem_rbtree_node_t* n = tree->root;
    while (n) {
        int r = tree->cmp_kn(key, (const xylem_rbtree_node_t*)n);
        if (r < 0) {
            n = n->left;
        } else if (r > 0) {
            n = n->right;
        } else {
            return n;
        }
    }
    return NULL;
}

xylem_rbtree_node_t* xylem_rbtree_next(xylem_rbtree_node_t* node) {
    xylem_rbtree_node_t* parent;

    if (node->right) {
        node = node->right;
        while (node->left) {
            node = node->left;
        }
        return node;
    }
    /* No right-hand children.  Everything down and left is
       smaller than us, so any 'next' node must be in the general
       direction of our parent. Go up the tree; any time the
       ancestor is a right-hand child of its parent, keep going
       up. First time it's a left-hand child of its parent, said
       parent is our 'next' node. */
    while ((parent = node->parent) && node == parent->right) {
        node = parent;
    }
    return parent;
}

xylem_rbtree_node_t* xylem_rbtree_prev(xylem_rbtree_node_t* node) {
    xylem_rbtree_node_t* parent;

    if (node->left) {
        node = node->left;
        while (node->right) {
            node = node->right;
        }
        return node;
    }
    /* No left-hand children. Go up till we find an ancestor which
       is a right-hand child of its parent */
    while ((parent = node->parent) && node == parent->left) {
        node = parent;
    }
    return parent;
}

xylem_rbtree_node_t* xylem_rbtree_first(xylem_rbtree_t* tree) {
    xylem_rbtree_node_t* n;
    n = tree->root;
    if (!n) {
        return NULL;
    }
    while (n->left) {
        n = n->left;
    }
    return n;
}

xylem_rbtree_node_t* xylem_rbtree_last(xylem_rbtree_t* tree) {
    xylem_rbtree_node_t* n;
    n = tree->root;
    if (!n) {
        return NULL;
    }
    while (n->right) {
        n = n->right;
    }
    return n;
}