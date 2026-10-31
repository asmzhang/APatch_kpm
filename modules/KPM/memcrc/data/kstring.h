#ifndef MEMORY_CRC_KSTRING_H
#define MEMORY_CRC_KSTRING_H

#ifndef EINVAL
#define EINVAL 22
#endif

#ifndef ERANGE
#define ERANGE 34
#endif

#ifndef isspace
#define kstring_isspace(c) ((c) == ' ' || (c) == '\t' || (c) == '\n' || \
                            (c) == '\r' || (c) == '\f' || (c) == '\v')
#else
#define kstring_isspace(c) isspace(c)
#endif

#define kstring_isdigit(c) ((c) >= '0' && (c) <= '9')

#define	UINT_MAX	0xffffffffU	/* max value for an unsigned int */
#define	INT_MAX		0x7fffffff	/* max value for an int */
#define	INT_MIN		(-0x7fffffff-1)	/* min value for an int */


char *kstring_strtok_r(char *str, const char *delim, char **saveptr);

int kstring_strtoul(const char *s, unsigned int base, unsigned long *res);

int kstring_strtol(const char *s, unsigned int base, long *res);

long kstring_simple_strtol(const char *cp, char **endp, unsigned int base);

unsigned long kstring_simple_strtoul(const char *cp, char **endp, unsigned int base);

char *kstring_trim(char *str);

const char *kstring_skip_spaces(const char *str);

int kstring_parse_int(const char *str, int *value);

int kstring_parse_uint(const char *str, unsigned int *value);

void *kstring_memcpy(void *dest, const void *src, size_t n);

/*void *kstring_memmove(void *dest, const void *src, size_t n);

void *kstring_memset(void *s, int c, size_t n);

int kstring_memcmp(const void *s1, const void *s2, size_t n);*/


#endif
