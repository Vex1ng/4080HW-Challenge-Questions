#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Node {
    char *data;
    struct Node *prev;
    struct Node *next;
};

typedef struct Node Node;

void insert(Node **head, Node *after, char *value) {
    Node *new = malloc(sizeof(Node));
    new->data = value;
    new->next = NULL;
    new->prev = NULL;

    if (after == NULL) {
        new->next = *head;
        if (*head != NULL) {
            (*head)->prev = new;
        }
        *head = new;
    } else {
        new->next = after->next;
        after->next = new;
        if (new->next != NULL) {
            new->next->prev = new;
        }
        new->prev = after;
    }
}

Node *find(Node *head, char *value) {
    Node *curr = head;
    while (curr != NULL) {
        if (strcmp(curr->data, value) == 0) {
            return curr;
        }
        curr = curr->next;
    }
    return NULL;
}

void delete(Node **head, Node *target) {
    if (target == *head) {
        *head = target->next;
        if (*head) (*head)->prev = NULL;
    } else {
        target->prev->next = target->next;
        if (target->next) {
            target->next->prev = target->prev;
        }
    }
    free(target);
}

void dump(Node *list) {
    Node *p = list;
    printf("[");
    int count = 0;
    while (p != NULL) {
        printf(" %s", p->data);
        p = p->next;
        count++;
        if (count > 100) break;
    }
    printf(" ]\n");
}

int main() {
    Node *list = NULL;

    char *s1 = "four";
    char *s2 = "one";
    char *s3 = "two";
    char *s4 = "three";

    insert(&list, NULL, s1);
    insert(&list, NULL, s2);
    insert(&list, find(list, "one"), s3);
    insert(&list, find(list, "two"), s4);

    printf("After inserts:\n");
    dump(list);

    printf("-- delete three --\n");
    delete(&list, find(list, "three"));
    dump(list);

    printf("-- delete one --\n");
    delete(&list, find(list, "one"));
    dump(list);

    return 0;
}