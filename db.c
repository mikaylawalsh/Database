#include "./db.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXLEN 256

// The root node of the binary tree, unlike all
// other nodes in the tree, this one is never
// freed (it's allocated in the data region).
node_t head = {"", "", 0, 0};

/*
lock: locks the lk using the specified lock type (either read or write)
parameters: lt, a lock type, and lk, the lock
returns: nothing
*/
#define lock(lt, lk) (((lt) == l_read)? pthread_rwlock_rdlock(lk): pthread_rwlock_wrlock(lk))

/*
node_constructor: 
parameters: the argument name, value, and the right and left child
returns: a pointer to the new node 
*/
node_t *node_constructor(char *arg_name, char *arg_value, node_t *arg_left,
                         node_t *arg_right) {
    size_t name_len = strlen(arg_name);
    size_t val_len = strlen(arg_value);

    if (name_len > MAXLEN || val_len > MAXLEN) return 0;

    node_t *new_node = (node_t *)malloc(sizeof(node_t));

    if (new_node == 0) return 0;

    if ((new_node->name = (char *)malloc(name_len + 1)) == 0) {
        free(new_node);
        return 0;
    }

    if ((new_node->value = (char *)malloc(val_len + 1)) == 0) {
        free(new_node->name);
        free(new_node);
        return 0;
    }

    if ((snprintf(new_node->name, MAXLEN, "%s", arg_name)) < 0) {
        free(new_node->value);
        free(new_node->name);
        free(new_node);
        return 0;
    } else if ((snprintf(new_node->value, MAXLEN, "%s", arg_value)) < 0) {
        free(new_node->value);
        free(new_node->name);
        free(new_node);
        return 0;
    }

    new_node->lchild = arg_left;
    new_node->rchild = arg_right;
    pthread_rwlock_init(&new_node->lock, NULL);
    return new_node;
}

/*
node_destructor: frees the node struct 
parameters: the node to destroy
returns: nothing
*/
void node_destructor(node_t *node) {
    if (node->name != 0) free(node->name);
    if (node->value != 0) free(node->value);
    free(node);
}

/*
db_query: looks for the node in the tree with the specified name if it is there
parameters: the name of the argument to search for, the string to put the result in,
and the length of the result array
returns: nothing
*/
void db_query(char *name, char *result, int len) {
    pthread_rwlock_rdlock(&head.lock);
    node_t *target;
    target = search(name, &head, 0, l_read);

    if (target == 0) {
        snprintf(result, len, "not found");
        return;
    } else {
        snprintf(result, len, "%s", target->value);
        pthread_rwlock_unlock(&target->lock);

        return;
    }
}

/*
db_add: adds a new node to the tree with the provided name and value 
parameters: the name and value to add to the tree
returns: an int
*/
int db_add(char *name, char *value) {
    node_t *parent;
    node_t *target;
    node_t *newnode;

    pthread_rwlock_wrlock(&head.lock);
    if ((target = search(name, &head, &parent, l_write)) != 0) {
        pthread_rwlock_unlock(&target->lock);
        pthread_rwlock_unlock(&parent->lock);
        return (0);
    }

    newnode = node_constructor(name, value, 0, 0);
    if (strcmp(name, parent->name) < 0)
        parent->lchild = newnode;
    else
        parent->rchild = newnode;
    pthread_rwlock_unlock(&parent->lock);
    return (1);
}

/*
db_remove: removes the node with the given name from the tree if it is there 
parameters: the name of the node to remove
returns: an int 
*/
int db_remove(char *name) {
    node_t *parent;
    node_t *dnode;
    node_t *next;

    pthread_rwlock_wrlock(&head.lock);
    // first, find the node to be removed
    if ((dnode = search(name, &head, &parent, l_write)) == 0) {
        // it's not there
        pthread_rwlock_unlock(&parent->lock);
        //pthread_rwlock_unlock(&dnode->lock);
        return (0);
    }
    //on return from search, if result is none zero, parent and dnode are locked

    // We found it, if the node has no
    // right child, then we can merely replace its parent's pointer to
    // it with the node's left child.

    if (dnode->rchild == 0) {
        if (strcmp(dnode->name, parent->name) < 0)
            parent->lchild = dnode->lchild;
        else
            parent->rchild = dnode->lchild;
        pthread_rwlock_unlock(&dnode->lock);
        pthread_rwlock_unlock(&parent->lock);
        // done with dnode
        node_destructor(dnode);
    } else if (dnode->lchild == 0) {
        // ditto if the node had no left child
        if (strcmp(dnode->name, parent->name) < 0)
            parent->lchild = dnode->rchild;
        else
            parent->rchild = dnode->rchild;
        pthread_rwlock_unlock(&dnode->lock);
        pthread_rwlock_unlock(&parent->lock);
        // done with dnode
        node_destructor(dnode);
    } else {
        // Find the lexicographically smallest node in the right subtree and
        // replace the node to be deleted with that node. This new node thus is
        // lexicographically smaller than all nodes in its right subtree, and
        // greater than all nodes in its left subtree

        next = dnode->rchild;
        node_t **pnext = &dnode->rchild;
        pthread_rwlock_wrlock(&next->lock);
        pthread_rwlock_unlock(&parent->lock);

        while (next->lchild != 0) {
            // work our way down the lchild chain, finding the smallest node
            // in the subtree.
            node_t *nextl = next->lchild;
            pthread_rwlock_wrlock(&nextl->lock);
            pnext = &next->lchild;
            pthread_rwlock_unlock(&next->lock);
            next = nextl;
        }

        dnode->name = realloc(dnode->name, strlen(next->name) + 1);
        dnode->value = realloc(dnode->value, strlen(next->value) + 1);

        snprintf(dnode->name, MAXLEN, "%s", next->name);
        snprintf(dnode->value, MAXLEN, "%s", next->value);
        *pnext = next->rchild;

        pthread_rwlock_unlock(&dnode->lock);
        pthread_rwlock_unlock(&next->lock);

        node_destructor(next);
    }
    return (1);
}

/*
search: searches for a node with the given name in the tree and if it is found it is returned 
and its parent is put in the parent argument 
parameters: the name of the node to find, the parent of that node, the pointer to the parent, 
and the lock type to use
returns: a pointer to the node if it is found
*/
node_t *search(char *name, node_t *parent, node_t **parentpp, enum locktype lt) {
    // Search the tree, starting at parent, for a node containing
    // name (the "target node").  Return a pointer to the node,
    // if found, otherwise return 0.  If parentpp is not 0, then it points
    // to a location at which the address of the parent of the target node
    // is stored.  If the target node is not found, the location pointed to
    // by parentpp is set to what would be the the address of the parent of
    // the target node, if it were there.

    node_t *next;
    node_t *result;

    if (strcmp(name, parent->name) < 0) {
        next = parent->lchild;
    } else {
        next = parent->rchild;
    }
    if (next == NULL) {
        result = NULL;
    } else {
        lock(lt, &next->lock);
        if (strcmp(name, next->name) == 0) {
            result = next;
        } else {
            pthread_rwlock_unlock(&parent->lock);
            return search(name, next, parentpp, lt);
        }
    }

    if (parentpp != NULL) {
        *parentpp = parent;
    } else {
        pthread_rwlock_unlock(&parent->lock);
    }
    return result;
}

/*
print_spaces: prints the spaces for the tree 
parameters: the level of the tree and the file to print to
returns: nothing
*/
static inline void print_spaces(int lvl, FILE *out) {
    for (int i = 0; i < lvl; i++) {
        fprintf(out, " ");
    }
}

/*
db_print_recurs: steps through the tree recursively and prints as it goes
parameters: the current node, the level we are at, and the file to print to
returns: nothing
*/
void db_print_recurs(node_t *node, int lvl, FILE *out) {
    // print spaces to differentiate levels
    print_spaces(lvl, out);
    // print out the current node
    if (node == NULL) {
        fprintf(out, "(null)\n");
        return;
    }
    pthread_rwlock_rdlock(&node->lock);
    if (node == &head) {
        fprintf(out, "(root)\n");
    } else {
        fprintf(out, "%s %s\n", node->name, node->value);
    }

    db_print_recurs(node->lchild, lvl + 1, out);
    db_print_recurs(node->rchild, lvl + 1, out);
    pthread_rwlock_unlock(&node->lock);
}

/*
db_print: prints the tree 
parameters: the file to print to 
returns: int indicting an error or no error
*/
int db_print(char *filename) {
    FILE *out;
    if (filename == NULL) {
        db_print_recurs(&head, 0, stdout);
        return 0;
    }

    // skip over leading whitespace
    while (isspace(*filename)) {
        filename++;
    }

    if (*filename == '\0') {
        db_print_recurs(&head, 0, stdout);
        return 0;
    }

    if ((out = fopen(filename, "w+")) == NULL) {
        return -1;
    }

    db_print_recurs(&head, 0, out);
    fclose(out);

    return 0;
}

/*
db_cleanup_recurs: steps through the tree recursively, deleting nodes as it goes
parameters: the current node
returns: nothing
*/
void db_cleanup_recurs(node_t *node) {
    if (node == NULL) {
        return;
    }

    db_cleanup_recurs(node->lchild);
    db_cleanup_recurs(node->rchild);

    node_destructor(node);
}

/*
db_cleanup: deletes the entire tree
parameters: nothing
returns: nothing
*/
void db_cleanup() {
    db_cleanup_recurs(head.lchild);
    db_cleanup_recurs(head.rchild);
}

/*
interpret_command: interprets the command in the command string, handles it be performing
the command, and puts a response in the response string
parameters: the string that contains a command, the string to put a response, and 
the length of the response 
returns: nothing
*/
void interpret_command(char *command, char *response, int len) {
    char value[MAXLEN];
    char ibuf[MAXLEN];
    char name[MAXLEN];
    int sscanf_ret;

    if (strlen(command) <= 1) {
        snprintf(response, len, "ill-formed command");
        return;
    }

    // which command is it?
    switch (command[0]) {
        case 'q':
            // Query
            sscanf_ret = sscanf(&command[1], "%255s", name);
            if (sscanf_ret < 1) {
                snprintf(response, len, "ill-formed command");
                return;
            }
            db_query(name, response, len);
            if (strlen(response) == 0) {
                snprintf(response, len, "not found");
            }

            return;

        case 'a':
            // Add to the database
            sscanf_ret = sscanf(&command[1], "%255s %255s", name, value);
            if (sscanf_ret < 2) {
                snprintf(response, len, "ill-formed command");
                return;
            }
            if (db_add(name, value)) {
                snprintf(response, len, "added");
            } else {
                snprintf(response, len, "already in database");
            }

            return;

        case 'd':
            // Delete from the database
            sscanf_ret = sscanf(&command[1], "%255s", name);
            if (sscanf_ret < 1) {
                snprintf(response, len, "ill-formed command");
                return;
            }
            if (db_remove(name)) {
                snprintf(response, len, "removed");
            } else {
                snprintf(response, len, "not in database");
            }

            return;

        case 'f':
            // process the commands in a file (silently)
            sscanf_ret = sscanf(&command[1], "%255s", name);
            if (sscanf_ret < 1) {
                snprintf(response, len, "ill-formed command");
                return;
            }

            FILE *finput = fopen(name, "r");
            if (!finput) {
                snprintf(response, len, "bad file name");
                return;
            }
            while (fgets(ibuf, sizeof(ibuf), finput) != 0) {
                pthread_testcancel();  // fgets is not a cancellation point
                interpret_command(ibuf, response, len);
            }
            fclose(finput);
            snprintf(response, len, "file processed");
            return;

        default:
            snprintf(response, len, "ill-formed command");
            return;
    }
}
