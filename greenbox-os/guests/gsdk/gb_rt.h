/*
 * gb_rt.h - the entire C library a guest gets.
 *
 * Declarations for gb_rt.c. There is no string.h here: a guest is linked
 * -nostdlib, so this is the whole list.
 */
#pragma once

#include <stddef.h>

void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
char  *strcpy(char *dst, const char *src);
