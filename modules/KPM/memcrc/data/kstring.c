#include <linux/kernel.h>
#include <linux/string.h>
#include "kstring.h"

/**
 * kstring_strtok_r - 线程安全的字符串分割函数
 */
char *kstring_strtok_r(char *str, const char *delim, char **saveptr)
{
    char *token;

    if (str == NULL) {
        str = *saveptr;
    }

    str += strspn(str, delim);
    if (*str == '\0') {
        *saveptr = str;
        return NULL;
    }

    token = str;
    str = strpbrk(token, delim);
    if (str == NULL) {
        *saveptr = strchr(token, '\0');
    } else {
        *str = '\0';
        *saveptr = str + 1;
    }

    return token;
}

/**
 * kstring_strtoul - 将字符串转换为无符号长整型
 */
int kstring_strtoul(const char *s, unsigned int base, unsigned long *res)
{
    unsigned long result = 0;
    const char *cp = s;

    if (!s || !res) {
        return -EINVAL;
    }

    while (*cp && kstring_isspace(*cp)) {
        cp++;
    }

    if (*cp == '\0') {
        return -EINVAL;
    }

    /* 目前只支持base 10 */
    if (base != 10) {
        return -EINVAL;
    }

    while (*cp) {
        if (kstring_isdigit(*cp)) {
            unsigned long old_result = result;
            result = result * 10 + (*cp - '0');

            if (result < old_result) {
                return -ERANGE;
            }
            cp++;
        } else if (kstring_isspace(*cp)) {
            cp++;
            while (*cp) {
                if (!kstring_isspace(*cp)) {
                    return -EINVAL;
                }
                cp++;
            }
            break;
        } else {
            return -EINVAL;
        }
    }

    *res = result;
    return 0;
}

/**
 * kstring_strtol - 将字符串转换为长整型
 */
int kstring_strtol(const char *s, unsigned int base, long *res)
{
    long result = 0;
    const char *cp = s;
    int sign = 1;

    if (!s || !res) {
        return -EINVAL;
    }

    while (*cp && kstring_isspace(*cp)) {
        cp++;
    }

    if (*cp == '\0') {
        return -EINVAL;
    }

    if (*cp == '-') {
        sign = -1;
        cp++;
    } else if (*cp == '+') {
        cp++;
    }

    if (*cp == '\0') {
        return -EINVAL;
    }

    if (base != 10) {
        return -EINVAL;
    }

    while (*cp) {
        if (kstring_isdigit(*cp)) {
            long old_result = result;
            result = result * 10 + (*cp - '0');

            if (sign == 1 && result < old_result) {
                return -ERANGE;
            } else if (sign == -1 && result > old_result) {
                return -ERANGE;
            }
            cp++;
        } else if (kstring_isspace(*cp)) {
            cp++;
            while (*cp) {
                if (!kstring_isspace(*cp)) {
                    return -EINVAL;
                }
                cp++;
            }
            break;
        } else {
            return -EINVAL;
        }
    }

    *res = result * sign;
    return 0;
}

/**
 * kstring_simple_strtol - 简单的字符串转长整型
 */
long kstring_simple_strtol(const char *cp, char **endp, unsigned int base)
{
    long result = 0;
    int sign = 1;

    if (!cp) {
        if (endp) *endp = (char *)cp;
        return 0;
    }

    while (*cp && kstring_isspace(*cp)) {
        cp++;
    }

    if (*cp == '-') {
        sign = -1;
        cp++;
    } else if (*cp == '+') {
        cp++;
    }

    if (base != 10) {
        if (endp) *endp = (char *)cp;
        return 0;
    }

    while (kstring_isdigit(*cp)) {
        result = result * 10 + (*cp - '0');
        cp++;
    }

    if (endp) {
        *endp = (char *)cp;
    }

    return result * sign;
}

/**
 * kstring_simple_strtoul - 简单的字符串转无符号长整型
 */
unsigned long kstring_simple_strtoul(const char *cp, char **endp, unsigned int base)
{
    unsigned long result = 0;

    if (!cp) {
        if (endp) *endp = (char *)cp;
        return 0;
    }

    while (*cp && kstring_isspace(*cp)) {
        cp++;
    }

    if (*cp == '+') {
        cp++;
    }

    if (base != 10) {
        if (endp) *endp = (char *)cp;
        return 0;
    }

    while (kstring_isdigit(*cp)) {
        result = result * 10 + (*cp - '0');
        cp++;
    }

    if (endp) {
        *endp = (char *)cp;
    }

    return result;
}

/**
 * kstring_trim - 去除字符串首尾的空白字符
 */
char *kstring_trim(char *str)
{
    char *end;

    if (!str) {
        return NULL;
    }

    while (*str && kstring_isspace(*str)) {
        str++;
    }

    if (*str == '\0') {
        return str;
    }

    end = str + strlen(str) - 1;
    while (end > str && kstring_isspace(*end)) {
        *end-- = '\0';
    }

    return str;
}

/**
 * kstring_skip_spaces - 跳过字符串开头的空白字符
 */
const char *kstring_skip_spaces(const char *str)
{
    if (!str) {
        return NULL;
    }

    while (*str && kstring_isspace(*str)) {
        str++;
    }

    return str;
}

/**
 * kstring_parse_int - 解析整数
 */
int kstring_parse_int(const char *str, int *value)
{
    long lval;
    int ret;

    if (!str || !value) {
        return -EINVAL;
    }

    ret = kstring_strtol(str, 10, &lval);
    if (ret != 0) {
        return ret;
    }

    if (lval > INT_MAX || lval < INT_MIN) {
        return -ERANGE;
    }

    *value = (int)lval;
    return 0;
}

/**
 * kstring_parse_uint - 解析无符号整数
 */
int kstring_parse_uint(const char *str, unsigned int *value)
{
    unsigned long ulval;
    int ret;

    if (!str || !value) {
        return -EINVAL;
    }

    ret = kstring_strtoul(str, 10, &ulval);
    if (ret != 0) {
        return ret;
    }

    if (ulval > UINT_MAX) {
        return -ERANGE;
    }

    *value = (unsigned int)ulval;
    return 0;
}

void *kstring_memcpy(void *dest, const void *src, size_t n)
{
    char *d = (char *)dest;
    const char *s = (const char *)src;

    void *ret = dest;

    /* 如果源和目标都是8字节对齐，使用8字节复制 */
    if ((((unsigned long)d | (unsigned long)s) & 7) == 0) {
        while (n >= 8) {
            *(unsigned long *)d = *(unsigned long *)s;
            d += 8;
            s += 8;
            n -= 8;
        }
    }
        /* 如果源和目标都是4字节对齐，使用4字节复制 */
    else if ((((unsigned long)d | (unsigned long)s) & 3) == 0) {
        while (n >= 4) {
            *(unsigned int *)d = *(unsigned int *)s;
            d += 4;
            s += 4;
            n -= 4;
        }
    }
        /* 如果源和目标都是2字节对齐，使用2字节复制 */
    else if ((((unsigned long)d | (unsigned long)s) & 1) == 0) {
        while (n >= 2) {
            *(unsigned short *)d = *(unsigned short *)s;
            d += 2;
            s += 2;
            n -= 2;
        }
    }

    while (n > 0) {
        *d++ = *s++;
        n--;
    }

    return ret;
}

/*
void *kstring_memmove(void *dest, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;

    if (d < s) {
        */
/* 从前向后复制 *//*

        while (n--) {
            *d++ = *s++;
        }
    } else if (d > s) {
        */
/* 从后向前复制 *//*

        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }

    return dest;
}

void *kstring_memset(void *s, int c, size_t n)
{
    unsigned char *p = (unsigned char *)s;
    unsigned char value = (unsigned char)c;

    while (n--) {
        *p++ = value;
    }

    return s;
}


int kstring_memcmp(const void *s1, const void *s2, size_t n)
{
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;

    while (n--) {
        if (*p1 != *p2) {
            return *p1 - *p2;
        }
        p1++;
        p2++;
    }

    return 0;
}*/
