// ============================================================
//  lib/string.c — Bare-metal string + memory primitives
// ============================================================
#include "string.h"

void *memset(void *d, int c, size_t n) {
    uint8_t *p = d; while (n--) *p++ = (uint8_t)c; return d; }

void *memcpy(void *restrict d, const void *restrict s, size_t n) {
    uint8_t *dd = d; const uint8_t *ss = s;
    while (n--) *dd++ = *ss++; return d; }

void *memmove(void *d, const void *s, size_t n) {
    uint8_t *dd = d; const uint8_t *ss = s;
    if (dd < ss) { while (n--) *dd++ = *ss++; }
    else if (dd > ss) { dd += n; ss += n; while (n--) *--dd = *--ss; }
    return d; }

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *pa = a, *pb = b;
    while (n--) { if (*pa != *pb) return *pa - *pb; pa++; pb++; }
    return 0; }

size_t strlen(const char *s) { size_t n=0; while(*s++) n++; return n; }

int strcmp(const char *a, const char *b) {
    while(*a && *a==*b){a++;b++;} return (unsigned char)*a-(unsigned char)*b; }

int strncmp(const char *a, const char *b, size_t n) {
    while(n-- && *a && *a==*b){a++;b++;}
    if(n==(size_t)-1) return 0;
    return (unsigned char)*a-(unsigned char)*b; }

char *strcpy(char *restrict d, const char *restrict s) {
    char *r=d; while((*d++=*s++)); return r; }

char *strncpy(char *restrict d, const char *restrict s, size_t n) {
    char *r=d; while(n&&(*d++=*s++))n--; while(n--)*d++='\0'; return r; }

char *strcat(char *restrict d, const char *restrict s) {
    char *r=d; while(*d) d++; while((*d++=*s++)); return r; }

char *strchr(const char *s, int c) {
    while(*s){if(*s==(char)c)return(char*)s;s++;}
    return c=='\0'?(char*)s:0; }

char *strrchr(const char *s, int c) {
    const char *last=0;
    while(*s){if(*s==(char)c)last=s;s++;}
    if(c=='\0')return(char*)s;
    return(char*)last; }

static const char digits[]="0123456789abcdef";

char *kuitoa(uint64_t v, char *buf, int base) {
    if(base<2||base>16){buf[0]='?';buf[1]='\0';return buf;}
    char tmp[65]; int i=0;
    if(!v){tmp[i++]='0';}
    else{while(v){tmp[i++]=digits[v%(uint64_t)base];v/=(uint64_t)base;}}
    int j=0; while(i--) buf[j++]=tmp[i]; buf[j]='\0'; return buf; }

char *kitoa(int64_t v, char *buf, int base) {
    if(v<0&&base==10){buf[0]='-';kuitoa((uint64_t)(-v),buf+1,base);}
    else kuitoa((uint64_t)v,buf,base);
    return buf; }
