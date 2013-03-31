/* LazySorted objects */

#include <Python.h>
#include <time.h>
#include "params.h"

/* Definitions and functions for the binary search tree of pivot points.
 * The BST implementation is a Treap, selected because of its general speed,
 * especially when inserting and removing elements, which happens a lot in this
 * application. */

typedef struct PivotNode {
    Py_ssize_t idx;             /* The index it represents */
    int flags;                  /* Descriptors of the data between pivots */
    int priority;               /* Priority in the Treap */
    struct PivotNode *left;
    struct PivotNode *right;
    struct PivotNode *parent;
} PivotNode;

/* SORTED_RIGHT means the pivot is to the right of a sorted region.
 * SORTED_LEFT means the pivot is the left of a sorted region */
#define SORTED_RIGHT 1
#define SORTED_LEFT 2
#define UNSORTED 0
#define SORTED_BOTH 3

/* The LazySorted object */
typedef struct {
    PyObject_HEAD
    PyListObject        *xs;            /* Partially sorted list */
    PivotNode           *root;          /* Root of the pivot BST */
} LSObject;

static PyTypeObject LS_Type;

#define LSObject_Check(v)      (Py_TYPE(v) == &LS_Type)

/* Returns the next (bigger) pivot, or NULL if it's the last pivot */
PivotNode *
next_pivot(PivotNode *current)
{
    PivotNode *curr = current;
    if (curr->right != NULL) {
        curr = curr->right;
        while (curr->left != NULL) {
            curr = curr->left;
        }
    }
    else {
        while (curr->parent != NULL && curr->parent->idx < curr->idx) {
            curr = curr->parent;
        }

        if (curr->parent == NULL) {
            return NULL;
        }
        else {
            curr = curr->parent;
        }
    }

    assert(curr->idx > current->idx);
    return curr;
}

/* A recursive function getting the consistency of a node.
 * Does not assume that the node is the root of the tree, and does NOT examine
 * the parentage of node. This is important, because it is often called on
 * nodes whose future parents don't know them yet, like in merge_trees(.) */
static void
assert_node(PivotNode *node)
{
    if (node->left != NULL) {
        assert(node->left->idx < node->idx);
        assert(node->left->priority <= node->priority);
        assert(node->left->parent == node);
        assert_node(node->left);
    }
    if (node->right != NULL) {
        assert(node->right->idx > node->idx);
        assert(node->right->priority <= node->priority);
        assert(node->right->parent == node);
        assert_node(node->right);
    }
}

/* A series of assert statements that the tree structure is consistent */
static void
assert_tree(PivotNode *root)
{
    assert(root != NULL);
    assert(root->parent == NULL);
    assert_node(root);
}

/* A series of assert statements that the tree's flags are consistent */
static void
assert_tree_flags(PivotNode *root)
{
    PivotNode *prev = NULL;
    PivotNode *curr = root;
    while (curr->left != NULL)
        curr = curr->left;
    while (curr != NULL) {
        if (curr->flags & SORTED_LEFT)
            assert(next_pivot(curr)->flags & SORTED_RIGHT);
        if (curr->flags & SORTED_RIGHT)
            assert(prev->flags & SORTED_LEFT);

        prev = curr;
        curr = next_pivot(curr);
    }
}

/* Inserts an index, returning a pointer to the node, or NULL on error.
 * *root is the root of the tree, while start is the node to insert from.
 */
static PivotNode *
insert_pivot(Py_ssize_t k, int flags, PivotNode **root, PivotNode *start)
{
    /* Build the node */
    PivotNode *node = (PivotNode *)PyMem_Malloc(sizeof(PivotNode));
    if (node == NULL)
        return (PivotNode *)PyErr_NoMemory();
    node->idx = k;
    node->flags = flags;
    node->priority = rand(); /* XXX Security, thread-safety */
    node->left = NULL;
    node->right = NULL;

    /* Special case the empty tree */
    if (*root == NULL) {
        node->parent = NULL;
        *root = node;
        return node;
    }

    /* Put the node in it's sorted order */
    PivotNode *current = start;
    while (1) {
        if (current->idx < k) {
            if (current->right == NULL) {
                current->right = node;
                node->parent = current;
                break;
            }
            current = current->right;
        }
        else if (current->idx > k) {
            if (current->left == NULL) {
                current->left = node;
                node->parent = current;
                break;
            }
            current = current->left;
        }
        else {
            /* The pivot BST should always have unique pivots */
            PyErr_SetString(PyExc_SystemError, "All pivots must be unique");
            return NULL;
        }
    }

    /* Reestablish the treap invariant if necessary by tree rotations */
    PivotNode *child, *parent, *grandparent;
    while (node->priority > node->parent->priority) {
        /*          (parent)    (node)
         *            /              \
         *           /                \
         *          /                  \
         *     (node)        ->       (parent)
         *        \                     /
         *         \                   /
         *       (child)            (child)
         */
        if (node->idx < node->parent->idx) {
            child = node->right;
            parent = node->parent;
            grandparent = parent->parent;

            node->parent = grandparent;
            node->right = parent;
            parent->parent = node;
            parent->left = child;
            if (child != NULL)
                child->parent = parent;
        }
        /* (parent)                (node)
         *      \                   /
         *       \                 /
         *        \               /
         *        (node)  ->  (parent)
         *        /              \
         *       /                \
         *    (child)           (child)
         */
        else {
            child = node->left;
            parent = node->parent;
            grandparent = parent->parent;

            node->parent = grandparent;
            node->left = parent;
            parent->parent = node;
            parent->right = child;
            if (child != NULL)
                child->parent = parent;
        }

        /* Adjust node->parent's child pointer to point to node */
        if (node->parent != NULL) {
            if (k < node->parent->idx) {
                node->parent->left = node;
            }
            else {
                node->parent->right = node;
            }
        }
        else {  /* The node has propogated up to the root */
            *root = node;
            break;
        }
    }

    assert_tree(*root);
    assert_tree_flags(*root);
    return node;
}

/* Takes two trees and merges them into one while preserving the treap
 * invariant. left must have a smaller index than right. */
static PivotNode *
merge_trees(PivotNode *left, PivotNode *right)
{
    assert(left != NULL || right != NULL);

    if (left == NULL)
        return right;
    if (right == NULL)
        return left;

    assert(left->parent == right->parent);
    assert(left->idx < right->idx);
    assert_node(left);
    assert_node(right);

    if (left->priority > right->priority) {
        right->parent = left;
        left->right = merge_trees(left->right, right);

        assert_node(left);
        return left;
    }
    else {
        left->parent = right;
        right->left = merge_trees(left, right->left);

        assert_node(right);
        return right;
    }
}

static void
delete_node(PivotNode *node, PivotNode **root)
{
    assert_tree(*root);

    if (node->left == NULL) {
        /* node has at most one child in node->right, so we just have the 
         * grandparent adopt it, if node is not the root. If node is the root,
         * we promote the child to root. */
        if (node->parent != NULL) {
            if (node->parent->left == node) {
                node->parent->left = node->right;
            }
            else {
                node->parent->right = node->right;
            }
        }
        else {  /* Node is the root */
            *root = node->right;
        }

        if (node->right != NULL) {
            node->right->parent = node->parent;
        }

        PyMem_Free(node);
    }
    else {
        if (node->right == NULL) {
            /* node has a single child in node->left, so have grandparent
             * adopt it as above */
            if (node->parent != NULL) {
                if (node->parent->left == node) {
                    node->parent->left = node->left;
                }
                else {
                    node->parent->right = node->left;
                }
            }
            else {  /* Node is the root */
                *root = node->left;
            }

            /* node->left is not NULL because of the outer if-else statement */
            node->left->parent = node->parent;

            PyMem_Free(node);
        }
        else {
            /* The hard case: node has two children. We merge the two children
             * into one treap, and then replace node by this treap */
            PivotNode *children = merge_trees(node->left, node->right);

            if (node->parent != NULL) {
                if (node->parent->left == node) {
                    node->parent->left = children;
                }
                else {
                    node->parent->right = children;
                }
            }
            else {  /* Node is the root */
                *root = children;
            }

            /* children is not NULL since node has two children */
            children->parent = node->parent;

            PyMem_Free(node);
        }
    }

    assert_tree(*root);
}

/* If a sorted pivot is between two sorted section, removes the sorted pivot */
static void
depivot(PivotNode *left, PivotNode *right, PivotNode **root)
{
    assert_tree(*root);
    assert_tree_flags(*root);
    assert(left->flags & SORTED_LEFT);
    assert(right->flags & SORTED_RIGHT);

    if (left->flags & SORTED_RIGHT) {
        delete_node(left, root);
    }

    if (right->flags & SORTED_LEFT) {
        delete_node(right, root);
    }

    assert_tree(*root);
    assert_tree_flags(*root);
}

/* If the value at middle is equal to the value at left, left is removed.
 * If the value at middle is equal to the value at right, right is removed.
 * Returns 0 on success, or -1 on failure */
static int
uniq_pivots(PivotNode *left, PivotNode *middle, PivotNode *right, LSObject *ls)
{
    assert_tree(ls->root);
    assert_tree_flags(ls->root);
    assert(left->idx < middle->idx && middle->idx < right->idx);
    int cmp;

    if (left->idx >= 0) {
        if ((cmp = PyObject_RichCompareBool(ls->xs->ob_item[left->idx],
                                            ls->xs->ob_item[middle->idx],
                                            Py_EQ)) < 0) {
            return -1;
        }
        else if (cmp) {
            middle->flags = left->flags;
            delete_node(left, &ls->root);
        }
    }

    if (right->idx < Py_SIZE(ls->xs)) {
        if ((cmp = PyObject_RichCompareBool(ls->xs->ob_item[middle->idx],
                                            ls->xs->ob_item[right->idx],
                                            Py_EQ)) < 0) {
            return -1;
        }
        else if (cmp) {
            middle->flags = right->flags;
            delete_node(right, &ls->root);
        }
    }

    assert_tree(ls->root);
    assert_tree_flags(ls->root);
    return 0;
}

/* Finds PivotNodes left and right that bound the index */
/* Never returns k in right_node, only the left, if applicable */
static void
bound_idx(Py_ssize_t k, PivotNode *root, PivotNode **left, PivotNode **right)
{
    assert_tree(root);
    assert_tree_flags(root);

    *left = NULL;
    *right = NULL;
    PivotNode *current = root;
    while (current != NULL) {
        if (current->idx < k) {
            *left = current;
            current = current->right;
        }
        else if (current->idx > k) {
            *right = current;
            current = current->left;
        }
        else {
            *left = current;
            break;
        }
    }

    assert(*left != NULL && ((*left)->idx == k || *right != NULL));
    assert((*left)->idx == k || *right == next_pivot(*left));
}

static void
free_tree(PivotNode *root)
{
    assert_node(root);  /* might not be the actual root because of recursion */

    if (root->left != NULL)
        free_tree(root->left);
    if (root->right != NULL)
        free_tree(root->right);

    PyMem_Free(root);
}

static PyObject *
newLSObject(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    LSObject *self;
    self = (LSObject *)type->tp_alloc(type, 0);
    if (self == NULL)
        return NULL;

    self->xs = (PyListObject *)PyList_Type.tp_new(&PyList_Type, args, kwds);
    if (self->xs == NULL)
        return NULL;

    self->root = NULL;
    if (insert_pivot(-1, UNSORTED, &self->root, self->root) == NULL)
        return NULL;

    return (PyObject *)self;
}

/* Private helper functions for partial sorting */

/* These macros are basically taken from list.c
 * Returns 1 if x < y, 0 if x >= y, and -1 on error */
#define ISLT(X, Y) PyObject_RichCompareBool(X, Y, Py_LT)

#define IFLT(X, Y) if ((ltflag = ISLT(X, Y)) < 0) goto fail;  \
            if(ltflag)

/* N.B: No semicolon at the end, so that you can include one yourself */
#define SWAP(i, j) tmp = ob_item[i];  \
                   ob_item[i] = ob_item[j];  \
                   ob_item[j] = tmp

/* Picks a pivot point among the indices left <= i < right */
static Py_ssize_t
pick_pivot(PyObject **ob_item, Py_ssize_t left, Py_ssize_t right)
{
    return left + rand() % (right - left);
}

/* Partitions the data between left and right into
 * [less than region | greater or equal to region]
 * and returns the pivot index, or -1 on error */
static Py_ssize_t
partition(PyObject **ob_item, Py_ssize_t left, Py_ssize_t right)
{
    PyObject *tmp;  /* Used by SWAP macro */
    PyObject *pivot;
    int ltflag;

    Py_ssize_t piv_idx = pick_pivot(ob_item, left, right);
    pivot = ob_item[piv_idx];

    SWAP(left, piv_idx);
    Py_ssize_t last_less = left;

    /* Invariant: last_less and everything to its left is less than
     * pivot or the pivot itself */

    Py_ssize_t i;
    for (i = left + 1; i < right; i++) {
        IFLT(ob_item[i], pivot) {
            last_less++;
            SWAP(i, last_less);
        }
    }

    SWAP(left, last_less);
    return last_less;

fail:  /* From IFLT macro */
    return -1;
}

/* Runs insertion sort on the items left <= i < right */
static void
insertion_sort(PyObject **ob_item, Py_ssize_t left, Py_ssize_t right)
{
    PyObject *tmp;
    Py_ssize_t i, j;

    for (i = left; i < right; i++) {
        tmp = ob_item[i];
        for (j = i; j > 0 && ISLT(tmp, ob_item[j - 1]); j--)
            ob_item[j] = ob_item[j - 1];
        ob_item[j] = tmp;
    }
}

/* Runs quicksort on the items left <= i < right, returning 1 on success
 * or zero on error. Does not affect stored pivots at all. */
static int
quick_sort(PyObject **ob_item, Py_ssize_t left, Py_ssize_t right)
{
    if (right - left <= SORT_THRESH) {
        insertion_sort(ob_item, left, right);
        return 0;
    }

    Py_ssize_t piv_idx = partition(ob_item, left, right);
    if (piv_idx < 0)
        return -1;

    if (quick_sort(ob_item, left, piv_idx) < 0)
        return -1;

    if (quick_sort(ob_item, piv_idx + 1, right) < 0)
        return -1;

    return 0;
}

/* Sorts the list ls sufficiently such that ls->xs->ob_item[k] is actually the
 * kth value in sorted order. Returns 0 on success and -1 on error. */
static int
sort_point(LSObject *ls, Py_ssize_t k)
{
    /* Find the best possible bounds */
    PivotNode *left, *right, *middle;
    bound_idx(k, ls->root, &left, &right);

    /* bound_idx never returns k in right, but right might be NULL if
     * left->idx == k, so check left->idx first. */
    if (left->idx == k || right->flags & SORTED_RIGHT) {
        return 0;
    }

    /* Run quickselect */
    Py_ssize_t piv_idx;

    while (left->idx + 1 + SORT_THRESH <= right->idx) {
        piv_idx = partition(ls->xs->ob_item,
                                left->idx + 1,
                                right->idx);
        if (piv_idx < k) {
            if (left->right == NULL) {
                middle = insert_pivot(piv_idx, UNSORTED, &ls->root, left);
            }
            else {
                middle = insert_pivot(piv_idx, UNSORTED, &ls->root, right);
            }
            if (middle == NULL)
                return -1;

            if (uniq_pivots(left, middle, right, ls) < 0) return -1;
            left = middle;
        }
        else if (piv_idx > k) {
            if (left->right == NULL) {
                middle = insert_pivot(piv_idx, UNSORTED, &ls->root, left);
            }
            else {
                middle = insert_pivot(piv_idx, UNSORTED, &ls->root, right);
            }
            if (middle == NULL)
                return -1;

            if (uniq_pivots(left, middle, right, ls) < 0) return -1;
            right = middle;
        }
        else {
            if (left->right == NULL) {
                middle = insert_pivot(piv_idx, UNSORTED, &ls->root, left);
            }
            else {
                middle = insert_pivot(piv_idx, UNSORTED, &ls->root, right);
            }

            if (uniq_pivots(left, middle, right, ls) < 0) return -1;
            return 0;
        }
    }

    insertion_sort(ls->xs->ob_item, left->idx + 1, right->idx);
    left->flags |= SORTED_LEFT;
    right->flags |= SORTED_RIGHT;
    depivot(left, right, &ls->root);

    return 0;
}

/* Sorts the list ls sufficiently such that everything between indices start
 * and stop is in sorted order. Returns 0 on success and -1 on error. */
static int
sort_range(LSObject *ls, Py_ssize_t start, Py_ssize_t stop)
{
    /* The xs list is always partially sorted, with pivots partioning up the
     * space, like in this picture:
     *
     * | ~~~~~ | ~~~ | ~~~~~ | ~~ | ~~~~~~~ |
     *
     * '|' indicates a pivot and '~~' indicates unsorted data.
     *
     * So we iterate through the regions bounding our data, and sort them.
     */

    assert(0 <= start && start < stop && stop <= Py_SIZE(ls->xs));

    if (sort_point(ls, start) < 0)
        return -1;
    if (sort_point(ls, stop) < 0)
        return -1;

    PivotNode *current, *next;
    bound_idx(start, ls->root, &current, &next);
    if (current->idx == start)
        next = next_pivot(current);

    while (current->idx < stop) {
        if (current->flags & SORTED_LEFT) {
            assert(next->flags & SORTED_RIGHT);
        }
        else {
            /* Since we are sorting the entire region, we don't need to keep
             * track of pivots, and so we can use vanilla quicksort */
            quick_sort(ls->xs->ob_item, current->idx + 1, next->idx);
            current->flags |= SORTED_LEFT;
            next->flags |= SORTED_RIGHT;
        }

        if (current->flags & SORTED_RIGHT) {
            delete_node(current, &ls->root);
        }

        current = next;
        next = next_pivot(current);
    }

    assert(current->flags & SORTED_RIGHT);
    if (current->flags & SORTED_LEFT) {
        delete_node(current, &ls->root);
    }

    return 0;
}

/* Returns the first index of item in the list, or -2 on error, or -1 if item
 * is not present. Places item in that first idx, but makes no guarantees
 * any duplicate versions of item will immediately follow. Eg, it's possible
 * calling find_item on some list with item = 1 will result in the following
 * list:
 * [0, 0, 0, 1, 2, 2, 1, 2, 1, 1, 2]
 */
static Py_ssize_t
find_item(LSObject *ls, PyObject *item)
{
    PivotNode *left = NULL;
    PivotNode *right = NULL;
    PivotNode *middle;
    PivotNode *current = ls->root;
    int ltflag;
    Py_ssize_t xs_len = Py_SIZE(ls->xs);
    Py_ssize_t left_idx, right_idx;

    while (current != NULL) {
        if (current->idx == -1) {
            left = current;
            current = current->right;
        }
        else if (current->idx == xs_len) {
            right = current;
            current = current->left;
        }
        else {
            IFLT(ls->xs->ob_item[current->idx], item) {
                left = current;
                current = current->right;
            }
            else {
                right = current;
                current = current->left;
            }
        }
    }

    if (left->flags & SORTED_LEFT) {
        assert(right->flags & SORTED_RIGHT);
        left_idx = left->idx + 1;
        right_idx = right->idx == xs_len ? xs_len : right->idx + 1;
    }
    else {
        Py_ssize_t piv_idx;
        while (left->idx + 1 + SORT_THRESH <= right->idx) {
            piv_idx = partition(ls->xs->ob_item, left->idx + 1,
                                    right->idx);
            IFLT(ls->xs->ob_item[piv_idx], item) {
                if (left->right == NULL) {
                    middle = insert_pivot(piv_idx, UNSORTED, &ls->root, left);
                }
                else {
                    middle = insert_pivot(piv_idx, UNSORTED, &ls->root, right);
                }
                if (middle == NULL)
                    return -2;

                if (uniq_pivots(left, middle, right, ls) < 0) return -1;
                left = middle;
            }
            else {
                if (left->right == NULL) {
                    middle = insert_pivot(piv_idx, UNSORTED, &ls->root, left);
                }
                else {
                    middle = insert_pivot(piv_idx, UNSORTED, &ls->root, right);
                }
                if (middle == NULL)
                    return -2;

                if (uniq_pivots(left, middle, right, ls) < 0) return -1;
                right = middle;
            }
        }

        left_idx = left->idx + 1;
        right_idx = right->idx == xs_len ? xs_len : right->idx + 1;

        insertion_sort(ls->xs->ob_item, left->idx + 1, right->idx);
        left->flags |= SORTED_LEFT;
        right->flags |= SORTED_RIGHT;
        depivot(left, right, &ls->root);
    }

    /* TODO: Do binary search now */
    Py_ssize_t k;
    int cmp = 0;
    for (k = left_idx; cmp == 0 && k < right_idx; k++) {
        cmp = PyObject_RichCompareBool(item, ls->xs->ob_item[k], Py_EQ);
    }

    if (cmp < 0) {
        return -2;
    }
    else if (cmp == 0) {
        return -1;
    }
    else {
        return k - 1;  /* -1 since incremented in for loop */
    }

fail:
    return -2;
}

/* LazySorted methods */

static PyObject *indexerr = NULL;

static PyObject *
ls_subscript(LSObject* self, PyObject* item)
{
    Py_ssize_t xs_len = Py_SIZE(self->xs);

    if (PyIndex_Check(item)) {
        Py_ssize_t k;
        k = PyNumber_AsSsize_t(item, PyExc_IndexError);
        if (k == -1 && PyErr_Occurred())
            return NULL;
        if (k < 0)
            k += xs_len;

        if (k < 0 || k >= xs_len) {
            if (indexerr == NULL) {
                indexerr = PyString_FromString("list index out of range");
                if (indexerr == NULL)
                    return NULL;
            }
            PyErr_SetObject(PyExc_IndexError, indexerr);
            return NULL;
        }

        if (sort_point(self, k) < 0)
            return NULL;

        Py_INCREF(self->xs->ob_item[k]);
        return self->xs->ob_item[k];
    }
    else if (PySlice_Check(item)) {
        Py_ssize_t start, stop, step, slicelength;

        if (PySlice_GetIndicesEx((PySliceObject*)item, Py_SIZE(self->xs),
                         &start, &stop, &step, &slicelength) < 0) {
            return NULL;
        }

        if (slicelength <= 0) {
            return PyList_New(0);
        }
        else if (-CONTIG_THRESH <= step && step <= CONTIG_THRESH) {
            Py_ssize_t left = start < stop ? start : stop;
            Py_ssize_t right = start < stop ? stop : start;

            if (step < 0) {
                left++;
                right++;
            }

            if (sort_range(self, left, right) < 0)
                return NULL;

            PyListObject *result = (PyListObject *)PyList_New(slicelength);
            if (result == NULL)
                return NULL;

            Py_ssize_t k, j;
            for (k = start, j = 0; j < slicelength; k += step, j++) {
                Py_INCREF(self->xs->ob_item[k]);
                result->ob_item[j] = self->xs->ob_item[k];
            }

            return (PyObject *)result;
        }
        else {
            PyListObject *result = (PyListObject *)PyList_New(slicelength);
            if (result == NULL)
                return NULL;

            Py_ssize_t k, j;
            for (k = start, j = 0; j < slicelength; k += step, j++) {
                if (sort_point(self, k) < 0)
                    return NULL;
                Py_INCREF(self->xs->ob_item[k]);
                result->ob_item[j] = self->xs->ob_item[k];
            }

            return (PyObject *)result;
        }
    }
    else {
        PyErr_Format(PyExc_TypeError,
                     "list indices must be integers, not %.200s",
                     item->ob_type->tp_name);
        return NULL;
    }
}

/* Returns (possibly unsorted) data in a specified contiguous range */
static PyObject *
between(LSObject *self, PyObject *args)
{
    Py_ssize_t left;
    Py_ssize_t right;

    if (!PyArg_ParseTuple(args, "nn:list", &left, &right))
        return NULL;

    Py_ssize_t xlen = Py_SIZE(self->xs);
    if (left < 0) {
        left += xlen;
    }
    else if (left > xlen) {
        left = xlen;
    }

    if (right < 0) {
        right += xlen;
    }
    else if (right > xlen) {
        right = xlen;
    }

    if (left >= right || right <= 0) {
        return PyList_New(0);
    }

    if (left != 0 && sort_point(self, left) < 0)
        return NULL;
    if (right != xlen && sort_point(self, right) < 0)
        return NULL;

    PyListObject *result = (PyListObject *)PyList_New(right - left);
    if (result == NULL)
        return NULL;

    Py_ssize_t k;
    for (k = left; k < right; k++) {
        Py_INCREF(self->xs->ob_item[k]);
        result->ob_item[k - left] = self->xs->ob_item[k];
    }

    return (PyObject *)result;
}

static PyObject *
ls_index(LSObject *self, PyObject *args)
{
    PyObject *item;
    if (!PyArg_ParseTuple(args, "O:list", &item))
        return NULL;

    Py_ssize_t index = find_item(self, item);
    if (index == -2) {
        return NULL;
    }
    else if (index == -1) {
        PyObject *err_format, *err_string, *format_tuple;
        err_format = PyString_FromString("%r is not in list");
        if (err_format == NULL)
            return NULL;
        format_tuple = PyTuple_Pack(1, item);
        if (format_tuple == NULL)
            return NULL;
        err_string = PyString_Format(err_format, format_tuple);
        Py_DECREF(format_tuple);
        if (err_string == NULL)
            return NULL;
        PyErr_SetObject(PyExc_ValueError, err_string);
        Py_DECREF(err_string);
        return NULL;
    }
    else {
        return PyInt_FromSsize_t(index);
    }
}

static PyObject *
ls_count(LSObject *self, PyObject *args)
{
    PyObject *item;
    if (!PyArg_ParseTuple(args, "O:list", &item))
        return NULL;

    Py_ssize_t k = find_item(self, item);
    if (k == -2) {
        return NULL;
    }

    if (k == -1) {
        return PyInt_FromSsize_t(0);
    }
    else {
        /* Figure out where items may be */
        PivotNode *left, *right;
        bound_idx(k, self->root, &left, &right);
        if (right == NULL) {
            right = next_pivot(left);
        }

        int xs_len = Py_SIZE(self->xs);
        int cmp;
        for (cmp = 1; right->idx < xs_len && cmp; right = next_pivot(right)) {
            cmp = PyObject_RichCompareBool(item,
                                           self->xs->ob_item[right->idx],
                                           Py_EQ);
            if (cmp < 0) {
                return NULL;
            }
        }

        /* TODO: do some additional sorting here to take advantage of the
         * compares. Or refactor the code substantially or something. */
        Py_ssize_t count = 1;
        for (k++; k < right->idx; k++) {
            cmp = PyObject_RichCompareBool(item, self->xs->ob_item[k], Py_EQ);
            if (cmp < 0) {
                return NULL;
            }
            else if (cmp) {
                count++;
            }
        }
        return PyInt_FromSsize_t(count);
    }
}

static int
ls_contains(LSObject *self, PyObject *item)
{
    Py_ssize_t idx = find_item(self, item);
    if (idx == -2) {
        return -1;
    }
    else if (idx == -1) {
        return 0;
    }
    else {
        return 1;
    }
}

static PyObject *
ls_pivots(LSObject *self)
{
    PyObject *result = PyList_New(0);
    if (result == NULL) 
        return NULL;

    PyObject *unsorted = PyString_FromString("UNSORTED");
    if (unsorted == NULL)
        return NULL;
    PyObject *sortedright = PyString_FromString("SORTED_RIGHT");
    if (sortedright == NULL)
        return NULL;
    PyObject *sortedleft = PyString_FromString("SORTED_LEFT");
    if (sortedleft == NULL)
        return NULL;
    PyObject *sortedboth = PyString_FromString("SORTED_BOTH");
    if (sortedboth == NULL)
        return NULL;

    PyObject *flags[4] = {unsorted, sortedright, sortedleft, sortedboth};

    PivotNode *curr = self->root;
    while (curr->left != NULL)
        curr = curr->left;

    Py_ssize_t i;
    PyObject *index;
    PyObject *tuple;
    for (i = 0; curr != NULL; i++, curr = next_pivot(curr)) {
        index = PyInt_FromSsize_t(curr->idx);
        if (index == NULL) {
            Py_DECREF(result);
            return NULL;
        }
        tuple = PyTuple_Pack(2, index, flags[curr->flags]);
        if (tuple == NULL) {
            Py_DECREF(index);
            Py_DECREF(result);
            return NULL;
        }
        PyList_Append(result, tuple);
    }

    return (PyObject*)result;
}

static Py_ssize_t
ls_length(LSObject *self)
{
    return Py_SIZE(self->xs);
}

static void
LS_dealloc(LSObject *self)
{
    Py_DECREF(self->xs);
    free_tree(self->root);
    PyObject_Del(self);
}

/* TODO: This documentation sucks */
static PyMethodDef LS_methods[] = {
    {"__getitem__", (PyCFunction)ls_subscript, METH_O|METH_COEXIST,
        PyDoc_STR(
"__getitem__ allows you to access items from the LazySorted instance. It is\n"
"equivalent to the subscript notation, ie, LS.__getitem__(x) is the same\n"
"thing as LS[x], where x is integer or slice object.\n"
"When __getitem__ is called, it sorts the internal list only enough so that\n"
"it can get your query, and then returns it.\n"
"\n"
"Examples:\n"
"    >>> xs = range(100)\n"
"    >>> random.shuffle(xs)\n"
"    >>> ls = LazySorted(xs)\n"
"    >>> ls[26]\n"
"    26\n"
"    >>> ls[5:10]\n"
"    [5, 6, 7, 8, 9]\n"
"    >>> ls[::20]\n"
"    [0, 20, 40, 60, 80]"
)},
    {"between", (PyCFunction)between, METH_VARARGS,
        PyDoc_STR(
"between allows you to access all points that are between particular\n"
"indices. The order of the points it returns, however, is undefined. This is\n"
"useful if you want to throw away outliers in some data set, for example.\n"
"\n"
"Examples:\n\n"
"    >>> xs = range(100)\n"
"    >>> random.shuffle(xs)\n"
"    >>> ls = LazySorted(xs)\n"
"    >>> set(ls.between(5, 95)) == set(range(5, 95))\n"
"    True"
)},
    {"index", (PyCFunction)ls_index, METH_VARARGS,
        PyDoc_STR(
"Returns the index of item in the list, or raises a ValueError if it isn't\n"
"present"
)},
    {"count", (PyCFunction)ls_count, METH_VARARGS,
        PyDoc_STR(
"Returns the number of times the item appears in the list"
)},
    {"_pivots", (PyCFunction)ls_pivots, METH_NOARGS,
        PyDoc_STR(
"Returns the list of pivot indices, for debugging"
)},
    {NULL,              NULL}           /* sentinel */
};

static PySequenceMethods ls_as_sequence = {
    (lenfunc)ls_length,                         /* sq_length */
    0,                                          /* sq_concat */
    0,                                          /* sq_repeat */
    0,                                          /* sq_item */
    0,                                          /* sq_slice */
    0,                                          /* sq_ass_item */
    0,                                          /* sq_ass_slice */
    (objobjproc)ls_contains,                    /* sq_contains */
    0,                                          /* sq_inplace_concat */
    0,                                          /* sq_inplace_repeat */
};

static PyMappingMethods ls_as_mapping = {
    (lenfunc)ls_length,
    (binaryfunc)ls_subscript,
    NULL,
};

static int
LS_init(LSObject *self, PyObject *args, PyObject *kw)
{
    PyObject *arg = NULL;
    static char *kwlist[] = {"sequence", 0};

    if (!PyArg_ParseTupleAndKeywords(args, kw, "|O:list", kwlist, &arg))
        return -1;

    if (PyList_Type.tp_init((PyObject *)self->xs, args, kw))
        return -1;

    if (insert_pivot(Py_SIZE(self->xs), UNSORTED, &self->root, self->root) ==
            NULL)
        return -1;

    return 0;
}

static PyTypeObject LS_Type = {
    /* The ob_type field must be initialized in the module init function
     * to be portable to Windows without using C++. */
    PyVarObject_HEAD_INIT(NULL, 0)
    "lazysorted.LazySorted",/*tp_name*/
    sizeof(LSObject),       /*tp_basicsize*/
    0,                      /*tp_itemsize*/
    /* methods */
    (destructor)LS_dealloc, /*tp_dealloc*/
    0,                      /*tp_print*/
    0,                      /*tp_getattr*/
    0,                      /*tp_setattr*/
    0,                      /*tp_compare*/
    0,                      /*tp_repr*/
    0,                      /*tp_as_number*/
    &ls_as_sequence,        /*tp_as_sequence*/
    &ls_as_mapping,         /*tp_as_mapping*/
    0,                      /*tp_hash*/
    0,                      /*tp_call*/
    0,                      /*tp_str*/
    0,                      /*tp_getattro*/
    0,                      /*tp_setattro*/
    0,                      /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT,     /*tp_flags*/
    0,                      /*tp_doc*/
    0,                      /*tp_traverse*/
    0,                      /*tp_clear*/
    0,                      /*tp_richcompare*/
    0,                      /*tp_weaklistoffset*/
    0,                      /*tp_iter*/
    0,                      /*tp_iternext*/
    LS_methods,             /*tp_methods*/
    0,                      /*tp_members*/
    0,                      /*tp_getset*/
    0,                      /*tp_base*/
    0,                      /*tp_dict*/
    0,                      /*tp_descr_get*/
    0,                      /*tp_descr_set*/
    0,                      /*tp_dictoffset*/
    (initproc)LS_init,      /*tp_init*/
    0,                      /*tp_alloc*/
    newLSObject,            /*tp_new*/
    0,                      /*tp_free*/
    0,                      /*tp_is_gc*/
};

/* List of functions defined in the module */
static PyMethodDef ls_methods[] = {
    {NULL,              NULL}           /* sentinel */
};

PyDoc_STRVAR(module_doc,
"TODO: Add module docs..."
);

/* Initialization function for the module */
PyMODINIT_FUNC
initlazysorted(void)
{
    srand(time(NULL)); /* XXX Worry about security, thread-safety etc */

    PyObject *m;

    /* Finalize the type object including setting type of the new type
     * object; doing it here is required for portability, too. */

    if (PyType_Ready(&LS_Type) < 0)
        return;

    /* Create the module and add the functions */
    m = Py_InitModule3("lazysorted", ls_methods, module_doc);
    if (m == NULL)
        return;

    PyModule_AddObject(m, "LazySorted", (PyObject *)&LS_Type);
}
