/*
 * falconc — Falcon language compiler  (v4)
 *
 * New in v4:
 *   -arch x86-32-linux  — original 32-bit target (default, unchanged)
 *   -arch x86-64-linux  — new 64-bit target (System V AMD64 ABI)
 *                         • syscall instead of int 0x80
 *                         • args in rdi,rsi,rdx,rcx,r8,r9
 *                         • long is a native 64-bit register (rax)
 *                         • all stack slots 8 bytes
 *                         • SSE2 for float/double (xmm0)
 *
 * New in v3:
 *   float         — 32-bit IEEE-754 via x87 FPU (flds/fstps/fadd etc.)
 *   double        — 64-bit IEEE-754 via x87 FPU (fldl/fstpl/faddp etc.)
 *   let           — mutable variable declaration (sugar: let x: int = 5)
 *   const         — immutable compile-time constant (folded, no stack slot)
 *                   const PI: float = 3.14   const MAX: int = 100
 *
 * New in v2:
 *   long          — 64-bit signed integer
 *   Type checker  — every expression has a computed type; mismatches are
 *                   errors at compile time, not silent wrong-code at runtime
 *   str_concat(a,b) -> str   — heap-allocate and concatenate two strings
 *   str_format(fmt, ...) -> str  — %-style formatting (%d %s %%) up to 8 args
 *   Dynamic heap  — _flr_alloc uses brk/sbrk (32-bit) or mmap (64-bit)
 *
 * Unchanged from v1:
 *   Lexer, parser structure, all operators, hardware intrinsics,
 *   freestanding mode, import system, arrays, structs, bool, break/continue
 *
 * Build:
 *   gcc -O2 -o falconc falconc.c
 *
 * Linking (32-bit hosted):
 *   as --32 out.s -o out.o
 *   ld -m elf_i386 flr.o out.o -o prog
 *
 * Linking (64-bit hosted):
 *   as out.s -o out.o
 *   ld out.o -o prog
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

/* ═══════════════════════════════════════════════════════════════════════
   UTILS
   ═══════════════════════════════════════════════════════════════════════ */

static void die(const char *fmt,...){
    va_list ap;va_start(ap,fmt);
    fprintf(stderr,"falconc: ");vfprintf(stderr,fmt,ap);
    fprintf(stderr,"\n");va_end(ap);exit(1);
}
static void warn(const char *fmt,...){
    va_list ap;va_start(ap,fmt);
    fprintf(stderr,"falconc: warning: ");vfprintf(stderr,fmt,ap);
    fprintf(stderr,"\n");va_end(ap);
}

static char *xstrdup(const char *s){return s?strdup(s):strdup("");}
static char *xstrndup(const char *s,int n){
    char *p=malloc(n+1);memcpy(p,s,n);p[n]=0;return p;
}
static char *path_dir(const char *path){
    char *tmp=xstrdup(path);
    char *sl=strrchr(tmp,'/');
    if(!sl){free(tmp);return xstrdup(".");}
    *sl='\0';return tmp;
}
static char *path_join(const char *dir,const char *name){
    size_t n=strlen(dir)+1+strlen(name)+1;
    char *p=malloc(n);snprintf(p,n,"%s/%s",dir,name);return p;
}

/* forward declaration — defined later in CODEGEN section */
static int has_std;

/* ═══════════════════════════════════════════════════════════════════════
   IMPORT DEDUP + SEARCH PATHS
   ═══════════════════════════════════════════════════════════════════════ */

#define MAX_IMPORTED 512
static char *imported_files[MAX_IMPORTED];
static int   nimported=0;
static int already_imported(const char *p){
    for(int i=0;i<nimported;i++)if(strcmp(imported_files[i],p)==0)return 1;return 0;
}
static void mark_imported(const char *p){
    if(nimported<MAX_IMPORTED)imported_files[nimported++]=xstrdup(p);
}

#define MAX_SEARCH 64
static char *search_paths[MAX_SEARCH];
static int   nsearch=0;
static void add_search(const char *p){
    if(nsearch<MAX_SEARCH)search_paths[nsearch++]=xstrdup(p);
}

static char *resolve_import(const char *name,const char *from_file){
    const char *exts[]={".fl",".fal",".flc",".flsrc",".ofc",".ofcc","",NULL};
    char *from_dir=path_dir(from_file);
    for(int e=0;exts[e];e++){
        char nb[512];snprintf(nb,sizeof nb,"%s%s",name,exts[e]);
        char *p=path_join(from_dir,nb);
        FILE *f=fopen(p,"rb");if(f){fclose(f);free(from_dir);return p;}free(p);
    }
    free(from_dir);
    for(int si=0;si<nsearch;si++){
        for(int e=0;exts[e];e++){
            char nb[512];snprintf(nb,sizeof nb,"%s%s",name,exts[e]);
            char *p=path_join(search_paths[si],nb);
            FILE *f=fopen(p,"rb");if(f){fclose(f);return p;}free(p);
        }
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════
   LEXER
   ═══════════════════════════════════════════════════════════════════════ */

typedef enum{
    TT_INT_LIT,TT_LONG_LIT,TT_FLOAT_LIT,TT_DOUBLE_LIT,TT_STR_LIT,
    TT_VOID,TT_INT,TT_STR,TT_BOOL,TT_LONG,TT_FLOAT,TT_DOUBLE,
    TT_LET,TT_CONST,
    TT_FUNC,TT_RETURN,
    TT_IF,TT_ELIF,TT_ELSE,
    TT_WHILE,TT_FOR,
    TT_AND,TT_OR,TT_NOT,
    TT_TRUE,TT_FALSE,
    TT_BREAK,TT_CONTINUE,
    TT_STRUCT,TT_IMPORT,TT_TYPEDEF,TT_EXTERN,
    TT_LBRACE,TT_RBRACE,
    TT_LPAREN,TT_RPAREN,
    TT_LBRACKET,TT_RBRACKET,
    TT_COLON,TT_COMMA,TT_ARROW,TT_DOT,
    TT_ASSIGN,
    TT_PLUS_ASSIGN,TT_MINUS_ASSIGN,TT_STAR_ASSIGN,TT_SLASH_ASSIGN,TT_MOD_ASSIGN,
    TT_SHL_ASSIGN,TT_SHR_ASSIGN,
    TT_AND_ASSIGN,TT_OR_ASSIGN,TT_XOR_ASSIGN,
    TT_EQ,TT_NEQ,TT_LT,TT_GT,TT_LTE,TT_GTE,
    TT_PLUS,TT_MINUS,TT_STAR,TT_SLASH,TT_MOD,
    TT_AMPERSAND,TT_PIPE,TT_CARET,TT_TILDE,
    TT_SHL,TT_SHR,
    TT_IDENT,TT_NEWLINE,TT_EOF,
    TT_SEMI,TT_QUESTION,
    TT_CHAR,TT_UNSIGNED,
    TT_ENUM,TT_SWITCH,TT_CASE,TT_DEFAULT,TT_DO,TT_SIZEOF,TT_ASM,
    TT_HASH_DEFINE,TT_HASH_INCLUDE,TT_HASH_IFDEF,TT_HASH_IFNDEF,
    TT_HASH_ENDIF,TT_HASH_ELSE,TT_HASH_UNDEF
}TT;

typedef struct{TT type;char *val;int line;const char *file;}Token;

#define MAX_TOKS 524288
static Token  gtoks[MAX_TOKS];
static int    gntoks=0;
static int    gpos=0;

typedef struct{const char *w;TT t;}KW;
static const KW kws[]={
    {"void",TT_VOID},{"int",TT_INT},{"str",TT_STR},{"bool",TT_BOOL},{"long",TT_LONG},
    {"float",TT_FLOAT},{"double",TT_DOUBLE},{"let",TT_LET},{"const",TT_CONST},
    {"func",TT_FUNC},{"return",TT_RETURN},
    {"if",TT_IF},{"elif",TT_ELIF},{"else",TT_ELSE},
    {"while",TT_WHILE},{"for",TT_FOR},
    {"and",TT_AND},{"or",TT_OR},{"not",TT_NOT},
    {"true",TT_TRUE},{"false",TT_FALSE},
    {"break",TT_BREAK},{"continue",TT_CONTINUE},
    {"struct",TT_STRUCT},{"import",TT_IMPORT},{"typedef",TT_TYPEDEF},{"extern",TT_EXTERN},
    {"char",TT_CHAR},{"unsigned",TT_UNSIGNED},
    {"enum",TT_ENUM},{"switch",TT_SWITCH},{"case",TT_CASE},{"default",TT_DEFAULT},
    {"do",TT_DO},{"sizeof",TT_SIZEOF},{"asm",TT_ASM},
    {NULL,0}
};

static void emit_tok(TT t,const char *v,int line,const char *file){
    if(gntoks>=MAX_TOKS)die("too many tokens");
    gtoks[gntoks].type=t;gtoks[gntoks].val=xstrdup(v);
    gtoks[gntoks].line=line;gtoks[gntoks].file=file;
    gntoks++;
}

static void tokenize(const char *src,const char *filename){
    int i=0,n=(int)strlen(src),line=1,last_nl=1;
    const char *file=xstrdup(filename);
    while(i<n){
        unsigned char c=src[i];
        if(c==' '||c=='\t'||c=='\r'){i++;continue;}
        if(c=='#'){
            /* preprocessor directives */
            i++;
            while(i<n&&(src[i]==' '||src[i]=='\t'))i++;
            int j=i;while(j<n&&isalpha((unsigned char)src[j]))j++;
            char *dir=xstrndup(src+i,j-i);i=j;
            while(i<n&&(src[i]==' '||src[i]=='\t'))i++;
            if(strcmp(dir,"define")==0){
                int k=i;while(k<n&&(src[k]=='_'||isalnum((unsigned char)src[k])))k++;
                char *nm=xstrndup(src+i,k-i);i=k;
                while(i<n&&(src[i]==' '||src[i]=='\t'))i++;
                int v=i;while(v<n&&src[v]!='\n')v++;
                char *val=xstrndup(src+i,v-i);
                emit_tok(TT_HASH_DEFINE,"#define",line,file);
                emit_tok(TT_IDENT,nm,line,file);
                emit_tok(TT_STR_LIT,val,line,file);
                free(nm);free(val);i=v;
            } else if(strcmp(dir,"include")==0){
                if(i<n&&(src[i]=='<'||src[i]=='"')){
                    char delim=src[i]=='<'?'>':'"';i++;
                    int k=i;while(k<n&&src[k]!=delim&&src[k]!='\n')k++;
                    char *path=xstrndup(src+i,k-i);
                    emit_tok(TT_HASH_INCLUDE,"#include",line,file);
                    emit_tok(TT_STR_LIT,path,line,file);
                    free(path);i=k+(k<n?1:0);
                }
            } else if(strcmp(dir,"ifdef")==0){
                int k=i;while(k<n&&(src[k]=='_'||isalnum((unsigned char)src[k])))k++;
                char *nm=xstrndup(src+i,k-i);
                emit_tok(TT_HASH_IFDEF,"#ifdef",line,file);
                emit_tok(TT_IDENT,nm,line,file);
                free(nm);i=k;
            } else if(strcmp(dir,"ifndef")==0){
                int k=i;while(k<n&&(src[k]=='_'||isalnum((unsigned char)src[k])))k++;
                char *nm=xstrndup(src+i,k-i);
                emit_tok(TT_HASH_IFNDEF,"#ifndef",line,file);
                emit_tok(TT_IDENT,nm,line,file);
                free(nm);i=k;
            } else if(strcmp(dir,"endif")==0){
                emit_tok(TT_HASH_ENDIF,"#endif",line,file);
            } else if(strcmp(dir,"else")==0){
                emit_tok(TT_HASH_ELSE,"#else",line,file);
            } else if(strcmp(dir,"undef")==0){
                int k=i;while(k<n&&(src[k]=='_'||isalnum((unsigned char)src[k])))k++;
                char *nm=xstrndup(src+i,k-i);
                emit_tok(TT_HASH_UNDEF,"#undef",line,file);
                emit_tok(TT_IDENT,nm,line,file);
                free(nm);i=k;
            } else {
                while(i<n&&src[i]!='\n')i++;
            }
            free(dir);last_nl=1;continue;
        }
        if(c=='/'&&i+1<n&&src[i+1]=='/'){while(i<n&&src[i]!='\n')i++;continue;}
        if(c=='/'&&i+1<n&&src[i+1]=='*'){
            i+=2;
            while(i+1<n&&!(src[i]=='*'&&src[i+1]=='/')){if(src[i]=='\n')line++;i++;}
            if(i+1<n)i+=2;continue;
        }
        if(c=='\n'){
            if(!last_nl)emit_tok(TT_NEWLINE,"\n",line,file);
            last_nl=1;line++;i++;continue;
        }
        last_nl=0;
        /* string */
        if(c=='"'){
            i++;
            char buf[16384];int bi=0;
            while(i<n&&src[i]!='"'){
                if(src[i]=='\\'&&i+1<n){
                    i++;
                    switch(src[i]){
                        case 'n':buf[bi++]='\n';break;case 't':buf[bi++]='\t';break;
                        case 'r':buf[bi++]='\r';break;case '0':buf[bi++]='\0';break;
                        case '"':buf[bi++]='"';break;case '\\':buf[bi++]='\\';break;
                        default:buf[bi++]='\\';buf[bi++]=src[i];break;
                    }
                }else{if(src[i]=='\n')line++;buf[bi++]=src[i];}
                i++;
            }
            if(i>=n)die("%s:%d: unterminated string",file,line);
            i++;buf[bi]=0;
            emit_tok(TT_STR_LIT,buf,line,file);continue;
        }
        /* hex literal */
        if(c=='0'&&i+1<n&&(src[i+1]=='x'||src[i+1]=='X')){
            int j=i+2;while(j<n&&isxdigit((unsigned char)src[j]))j++;
            int is_long=(j<n&&(src[j]=='L'||src[j]=='l'));
            char *s=xstrndup(src+i,j-i);
            emit_tok(is_long?TT_LONG_LIT:TT_INT_LIT,s,line,file);
            free(s);i=j+(is_long?1:0);continue;
        }
        /* decimal / float / double */
        if(isdigit(c)||(c=='.'&&i+1<n&&isdigit((unsigned char)src[i+1]))){
            int j=i;
            while(j<n&&isdigit((unsigned char)src[j]))j++;
            int is_float=0,is_double=0;
            if(j<n&&src[j]=='.'){
                j++;
                while(j<n&&isdigit((unsigned char)src[j]))j++;
                /* exponent */
                if(j<n&&(src[j]=='e'||src[j]=='E')){
                    j++;
                    if(j<n&&(src[j]=='+'||src[j]=='-'))j++;
                    while(j<n&&isdigit((unsigned char)src[j]))j++;
                }
                /* suffix: f/F = float, d/D or none = double */
                if(j<n&&(src[j]=='f'||src[j]=='F')){is_float=1;}
                else{is_double=1;}
                char *s=xstrndup(src+i,j-i);
                emit_tok(is_float?TT_FLOAT_LIT:TT_DOUBLE_LIT,s,line,file);
                free(s);i=j+(is_float?1:0);continue;
            }
            int is_long=(j<n&&(src[j]=='L'||src[j]=='l'));
            char *s=xstrndup(src+i,j-i);
            emit_tok(is_long?TT_LONG_LIT:TT_INT_LIT,s,line,file);
            free(s);i=j+(is_long?1:0);continue;
        }
        /* ident / keyword */
        if(c=='_'||isalpha(c)){
            int j=i;while(j<n&&(src[j]=='_'||isalnum((unsigned char)src[j])))j++;
            char *w=xstrndup(src+i,j-i);
            TT t2=TT_IDENT;
            for(int k=0;kws[k].w;k++)if(strcmp(w,kws[k].w)==0){t2=kws[k].t;break;}
            emit_tok(t2,w,line,file);free(w);i=j;continue;
        }
        /* 3-char ops */
        if(i+2<n){
            if(src[i]=='<'&&src[i+1]=='<'&&src[i+2]=='='){emit_tok(TT_SHL_ASSIGN,"<<=",line,file);i+=3;continue;}
            if(src[i]=='>'&&src[i+1]=='>'&&src[i+2]=='='){emit_tok(TT_SHR_ASSIGN,">>=",line,file);i+=3;continue;}
        }
        /* 2-char ops */
        if(i+1<n){
            char nc=src[i+1];
            if(c=='-'&&nc=='>'){emit_tok(TT_ARROW,"->",line,file);i+=2;continue;}
            if(c=='>'&&nc=='='){emit_tok(TT_GTE,">=",line,file);i+=2;continue;}
            if(c=='<'&&nc=='='){emit_tok(TT_LTE,"<=",line,file);i+=2;continue;}
            if(c=='!'&&nc=='='){emit_tok(TT_NEQ,"!=",line,file);i+=2;continue;}
            if(c=='='&&nc=='='){emit_tok(TT_EQ,"==",line,file);i+=2;continue;}
            if(c=='+'&&nc=='='){emit_tok(TT_PLUS_ASSIGN,"+=",line,file);i+=2;continue;}
            if(c=='-'&&nc=='='){emit_tok(TT_MINUS_ASSIGN,"-=",line,file);i+=2;continue;}
            if(c=='*'&&nc=='='){emit_tok(TT_STAR_ASSIGN,"*=",line,file);i+=2;continue;}
            if(c=='/'&&nc=='='){emit_tok(TT_SLASH_ASSIGN,"/=",line,file);i+=2;continue;}
            if(c=='%'&&nc=='='){emit_tok(TT_MOD_ASSIGN,"%=",line,file);i+=2;continue;}
            if(c=='&'&&nc=='='){emit_tok(TT_AND_ASSIGN,"&=",line,file);i+=2;continue;}
            if(c=='|'&&nc=='='){emit_tok(TT_OR_ASSIGN,"|=",line,file);i+=2;continue;}
            if(c=='^'&&nc=='='){emit_tok(TT_XOR_ASSIGN,"^=",line,file);i+=2;continue;}
            if(c=='<'&&nc=='<'){emit_tok(TT_SHL,"<<",line,file);i+=2;continue;}
            if(c=='>'&&nc=='>'){emit_tok(TT_SHR,">>",line,file);i+=2;continue;}
        }
        /* single-char */
        switch(c){
            case '{':emit_tok(TT_LBRACE,"{",line,file);break;
            case '}':emit_tok(TT_RBRACE,"}",line,file);break;
            case '(':emit_tok(TT_LPAREN,"(",line,file);break;
            case ')':emit_tok(TT_RPAREN,")",line,file);break;
            case '[':emit_tok(TT_LBRACKET,"[",line,file);break;
            case ']':emit_tok(TT_RBRACKET,"]",line,file);break;
            case ':':emit_tok(TT_COLON,":",line,file);break;
            case ',':emit_tok(TT_COMMA,",",line,file);break;
            case '.':emit_tok(TT_DOT,".",line,file);break;
            case '=':emit_tok(TT_ASSIGN,"=",line,file);break;
            case '+':emit_tok(TT_PLUS,"+",line,file);break;
            case '-':emit_tok(TT_MINUS,"-",line,file);break;
            case '*':emit_tok(TT_STAR,"*",line,file);break;
            case '/':emit_tok(TT_SLASH,"/",line,file);break;
            case '%':emit_tok(TT_MOD,"%",line,file);break;
            case '<':emit_tok(TT_LT,"<",line,file);break;
            case '>':emit_tok(TT_GT,">",line,file);break;
            case '&':emit_tok(TT_AMPERSAND,"&",line,file);break;
            case '|':emit_tok(TT_PIPE,"|",line,file);break;
            case '^':emit_tok(TT_CARET,"^",line,file);break;
            case '~':emit_tok(TT_TILDE,"~",line,file);break;
            case ';':emit_tok(TT_SEMI,";",line,file);break;
            case '?':emit_tok(TT_QUESTION,"?",line,file);break;
            default:
                if(c>0x7f){i++;continue;} /* skip UTF-8 bytes */
                die("%s:%d: unexpected character '%c' (0x%02x)",file,line,isprint(c)?c:'?',c);
        }
        i++;
    }
}

/* ═══════════════════════════════════════════════════════════════════════
   IMPORT EXPANDER
   ═══════════════════════════════════════════════════════════════════════ */

static char *read_file(const char *path){
    FILE *f=fopen(path,"rb");if(!f)return NULL;
    fseek(f,0,SEEK_END);long sz=ftell(f);rewind(f);
    char *buf=malloc(sz+1);fread(buf,1,sz,f);buf[sz]=0;fclose(f);return buf;
}

static void expand_imports(int start,const char *from_file);

static void tokenize_file(const char *path){
    if(already_imported(path))return;
    mark_imported(path);
    char *src=read_file(path);
    if(!src)die("cannot open import '%s'",path);
    int tok_start=gntoks;
    tokenize(src,path);free(src);
    expand_imports(tok_start,path);
}

static void expand_imports(int start,const char *from_file){
    for(int i=start;i<gntoks-1;i++){
        if(gtoks[i].type==TT_IMPORT&&gtoks[i+1].type==TT_STR_LIT){
            const char *name=gtoks[i+1].val;
            char *resolved=resolve_import(name,from_file);
            if(!resolved){
                /* No real std.fl was found: use Falcon's virtual std runtime. */
                if(strcmp(name,"std")==0){has_std=1;continue;}
                die("%s:%d: cannot find import '%s'",gtoks[i].file,gtoks[i].line,name);
            }
            if(already_imported(resolved)){
                memmove(&gtoks[i],&gtoks[i+2],(gntoks-(i+2))*sizeof(Token));
                gntoks-=2;free(resolved);i--;continue;
            }
            /*
             * A real file named std.fl is just a normal source import.
             * This lets projects and libraries ship their own std.fl without
             * accidentally enabling Falcon's built-in runtime.
             */
            int splice_start=gntoks;
            tokenize_file(resolved);
            int splice_count=gntoks-splice_start;
            free(resolved);
            if(splice_count>0){
                Token *tmp=malloc(splice_count*sizeof(Token));
                memcpy(tmp,&gtoks[splice_start],splice_count*sizeof(Token));
                int tail=splice_start-(i+2);
                memmove(&gtoks[i+splice_count],&gtoks[i+2],tail*sizeof(Token));
                memcpy(&gtoks[i],tmp,splice_count*sizeof(Token));
                free(tmp);
                gntoks=i+splice_count+tail;
            }else{
                memmove(&gtoks[i],&gtoks[i+2],(gntoks-(i+2))*sizeof(Token));
                gntoks-=2;i--;
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════
   TOKEN STREAM
   ═══════════════════════════════════════════════════════════════════════ */

static Token *peek(void)    {return &gtoks[gpos];}
static Token *advance(void) {Token *t=&gtoks[gpos];if(gpos<gntoks-1)gpos++;return t;}
static int    check(TT t)   {return peek()->type==t;}
static void   skip_nl(void) {while(check(TT_NEWLINE))advance();}

static Token *expect(TT t,const char *what){
    skip_nl();
    if(!check(t))die("%s:%d: expected %s, got '%s'",peek()->file,peek()->line,what,peek()->val);
    return advance();
}
static int match(TT t){skip_nl();if(check(t)){advance();return 1;}return 0;}

/* ═══════════════════════════════════════════════════════════════════════
   TYPE SYSTEM
   ═══════════════════════════════════════════════════════════════════════ */

typedef enum{TY_VOID,TY_INT,TY_LONG,TY_FLOAT,TY_DOUBLE,TY_STR,TY_BOOL,TY_ARRAY,TY_STRUCT,TY_PTR}TypeKind;
typedef struct TypeRef TypeRef;
struct TypeRef{TypeKind kind;char *name;TypeRef *elem;};

static TypeRef *mktype(TypeKind k,const char *name,TypeRef *elem){
    TypeRef *t=calloc(1,sizeof*t);t->kind=k;
    t->name=name?xstrdup(name):NULL;t->elem=elem;return t;
}

static const char *type_name(TypeRef *t){
    if(!t)return "unknown";
    switch(t->kind){
        case TY_VOID:  return "void";
        case TY_INT:   return "int";
        case TY_LONG:  return "long";
        case TY_FLOAT: return "float";
        case TY_DOUBLE:return "double";
        case TY_STR:   return "str";
        case TY_BOOL:  return "bool";
        case TY_ARRAY: return "array";
        case TY_STRUCT:return t->name?t->name:"struct";
        case TY_PTR:   return "ptr";
    }
    return "unknown";
}

/* two types are compatible for assignment / comparison */
static int types_compat(TypeRef *a,TypeRef *b){
    if(!a||!b)return 1;
    if(a->kind==b->kind){
        if(a->kind==TY_STRUCT)
            return a->name&&b->name&&strcmp(a->name,b->name)==0;
        return 1;
    }
    /* int <-> long widening allowed */
    if((a->kind==TY_INT||a->kind==TY_LONG)&&(b->kind==TY_INT||b->kind==TY_LONG))return 1;
    /* float/double interchangeable; int/long can assign to float/double */
    if((a->kind==TY_FLOAT||a->kind==TY_DOUBLE)&&(b->kind==TY_FLOAT||b->kind==TY_DOUBLE))return 1;
    if((a->kind==TY_FLOAT||a->kind==TY_DOUBLE)&&(b->kind==TY_INT||b->kind==TY_LONG))return 1;
    /* bool <-> int allowed */
    if((a->kind==TY_INT||a->kind==TY_BOOL)&&(b->kind==TY_INT||b->kind==TY_BOOL))return 1;
    /* struct vars are heap pointers: allow int/ptr on rhs (e.g. _flr_alloc return) */
    if(a->kind==TY_STRUCT&&(b->kind==TY_INT||b->kind==TY_PTR))return 1;
    /* ptr <-> int interchangeable */
    if((a->kind==TY_PTR||a->kind==TY_INT)&&(b->kind==TY_PTR||b->kind==TY_INT))return 1;
    /* str <-> int/ptr: str is just a char pointer, allow for bare-metal */
    if((a->kind==TY_STR)&&(b->kind==TY_INT||b->kind==TY_PTR))return 1;
    if((b->kind==TY_STR)&&(a->kind==TY_INT||a->kind==TY_PTR))return 1;
    return 0;
}

static int type_is_numeric(TypeRef *t){
    return t&&(t->kind==TY_INT||t->kind==TY_LONG||t->kind==TY_BOOL||t->kind==TY_FLOAT||t->kind==TY_DOUBLE);
}

/* ═══════════════════════════════════════════════════════════════════════
   AST
   ═══════════════════════════════════════════════════════════════════════ */

typedef enum{
    N_PROGRAM,N_FUNC,N_STRUCT,N_IMPORT,
    N_VARDECL,N_LETDECL,N_CONSTDECL,N_ASSIGN,N_RETURN,
    N_IF,N_ELIF,N_WHILE,N_FOR,N_BREAK,N_CONTINUE,N_EXPRSTMT,
    N_INTLIT,N_LONGLIT,N_FLOATLIT,N_DOUBLELIT,N_STRLIT,N_BOOLLIT,N_IDENT,N_BINOP,N_UNOP,
    N_CALL,N_INDEX,N_FIELD,N_ARRAYLIT,
    N_TERNARY,N_SWITCH,N_CASE,N_DO,N_ASM,N_SIZEOF,
    N_TYPEDEF_DECL,N_EXTERN
}NK;

typedef struct Node Node;
typedef struct{Node **d;int n,cap;}NList;
static void nl_push(NList *l,Node *n){
    if(l->n>=l->cap){l->cap=l->cap?l->cap*2:8;l->d=realloc(l->d,l->cap*sizeof*l->d);}
    l->d[l->n++]=n;
}
typedef struct{char *name;TypeRef *type;}Param;
typedef struct{Param *d;int n,cap;}PList;
static void pl_push(PList *l,Param p){
    if(l->n>=l->cap){l->cap=l->cap?l->cap*2:4;l->d=realloc(l->d,l->cap*sizeof*l->d);}
    l->d[l->n++]=p;
}
typedef struct{char *name;TypeRef *type;}Field;
typedef struct{Field *d;int n,cap;}FList;
static void fl_push(FList *l,Field f){
    if(l->n>=l->cap){l->cap=l->cap?l->cap*2:4;l->d=realloc(l->d,l->cap*sizeof*l->d);}
    l->d[l->n++]=f;
}
typedef struct{Node *cond;NList body;}ElifClause;
typedef struct{ElifClause *d;int n,cap;}ElifList;
static void el_push(ElifList *l,ElifClause e){
    if(l->n>=l->cap){l->cap=l->cap?l->cap*2:4;l->d=realloc(l->d,l->cap*sizeof*l->d);}
    l->d[l->n++]=e;
}

struct Node{
    NK kind;int line;const char *file;
    TypeRef *typeref;       /* declared type (vardecl) */
    TypeRef *etype;         /* computed expression type (filled by type-checker) */
    long long ival;double dval;char *sval;int bval;char *op;
    int is_const;  /* for N_CONSTDECL: enforce immutability */
    Node *left,*right,*cond;
    char *fname;PList params;TypeRef *rettype;NList body;
    char *structname;FList fields;
    ElifList elifs;NList else_body;
    Node *for_init,*for_post;
    char *callee;NList args;
    NList elems;
    char *import_path;char *name;
};

static Node *mknode(NK k,int line){
    Node *n=calloc(1,sizeof*n);n->kind=k;n->line=line;n->file=peek()->file;return n;
}

/* ═══════════════════════════════════════════════════════════════════════
   PARSER
   ═══════════════════════════════════════════════════════════════════════ */

static Node *parse_expr(void);
static NList parse_block(void);
/* forward declarations for typedef registry (defined after struct registry) */
static void register_typedef(const char *alias,TypeRef *t);
static TypeRef *resolve_typedef(const char *name);

static TypeRef *parse_type(void){
    skip_nl();Token *t=peek();
    if(t->type==TT_STAR){advance();return mktype(TY_PTR,NULL,parse_type());}
    if(t->type==TT_LBRACKET){
        advance();TypeRef *elem=parse_type();expect(TT_RBRACKET,"]");
        return mktype(TY_ARRAY,NULL,elem);
    }
    advance();
    switch(t->type){
        case TT_INT:  return mktype(TY_INT,NULL,NULL);
        case TT_LONG:  return mktype(TY_LONG,NULL,NULL);
        case TT_FLOAT: return mktype(TY_FLOAT,NULL,NULL);
        case TT_DOUBLE:return mktype(TY_DOUBLE,NULL,NULL);
        case TT_STR:   return mktype(TY_STR,NULL,NULL);
        case TT_BOOL: return mktype(TY_BOOL,NULL,NULL);
        case TT_VOID: return mktype(TY_VOID,NULL,NULL);
        case TT_IDENT:{
            /* check typedef registry first, then fall back to struct name */
            TypeRef *aliased=resolve_typedef(t->val);
            if(aliased)return aliased;
            return mktype(TY_STRUCT,t->val,NULL);
        }
        default:die("%s:%d: expected type, got '%s'",t->file,t->line,t->val);
    }
    return NULL;
}

static int is_intrinsic(const char *name){
    static const char *ii[]={
        "__syscall","__inb","__outb","__cli","__sti","__hlt",
        "__rdtsc","__peek","__poke","__peekb","__pokeb",
        "__memset","__memcpy",NULL
    };
    for(int i=0;ii[i];i++)if(strcmp(name,ii[i])==0)return 1;
    return 0;
}

static Node *parse_primary(void){
    skip_nl();Token *t=peek();
    if(t->type==TT_LONG_LIT){
        advance();Node *n=mknode(N_LONGLIT,t->line);
        if(t->val[0]=='0'&&(t->val[1]=='x'||t->val[1]=='X'))
            n->ival=(long long)strtoll(t->val,NULL,16);
        else n->ival=atoll(t->val);
        return n;
    }
    if(t->type==TT_FLOAT_LIT){
        advance();Node *n=mknode(N_FLOATLIT,t->line);
        n->dval=strtod(t->val,NULL);return n;
    }
    if(t->type==TT_DOUBLE_LIT){
        advance();Node *n=mknode(N_DOUBLELIT,t->line);
        n->dval=strtod(t->val,NULL);return n;
    }
    if(t->type==TT_INT_LIT){
        advance();Node *n=mknode(N_INTLIT,t->line);
        if(t->val[0]=='0'&&(t->val[1]=='x'||t->val[1]=='X'))
            n->ival=(long long)strtoll(t->val,NULL,16);
        else n->ival=atoll(t->val);
        return n;
    }
    if(t->type==TT_STR_LIT){
        advance();Node *n=mknode(N_STRLIT,t->line);n->sval=xstrdup(t->val);return n;
    }
    if(t->type==TT_TRUE||t->type==TT_FALSE){
        advance();Node *n=mknode(N_BOOLLIT,t->line);n->bval=(t->type==TT_TRUE);return n;
    }
    if(t->type==TT_LBRACKET){
        advance();Node *n=mknode(N_ARRAYLIT,t->line);
        skip_nl();
        if(!check(TT_RBRACKET)){
            nl_push(&n->elems,parse_expr());skip_nl();
            while(match(TT_COMMA)){skip_nl();if(check(TT_RBRACKET))break;nl_push(&n->elems,parse_expr());skip_nl();}
        }
        expect(TT_RBRACKET,"]");return n;
    }
    if(t->type==TT_LPAREN){
        advance();skip_nl();Node *n=parse_expr();skip_nl();expect(TT_RPAREN,")");return n;
    }
    if(t->type==TT_IDENT){
        advance();skip_nl();
        if(check(TT_LPAREN)){
            advance();skip_nl();
            Node *n=mknode(N_CALL,t->line);n->callee=xstrdup(t->val);
            if(!check(TT_RPAREN)){
                nl_push(&n->args,parse_expr());skip_nl();
                while(match(TT_COMMA)){skip_nl();if(check(TT_RPAREN))break;nl_push(&n->args,parse_expr());skip_nl();}
            }
            expect(TT_RPAREN,")");return n;
        }
        Node *n=mknode(N_IDENT,t->line);n->name=xstrdup(t->val);return n;
    }
    die("%s:%d: unexpected token '%s' in expression",t->file,t->line,t->val);
    return NULL;
}

static Node *parse_postfix(void){
    Node *n=parse_primary();
    for(;;){
        skip_nl();
        if(check(TT_LBRACKET)){
            int line=peek()->line;advance();skip_nl();
            Node *idx=mknode(N_INDEX,line);idx->left=n;idx->right=parse_expr();
            skip_nl();expect(TT_RBRACKET,"]");n=idx;
        }else if(check(TT_DOT)){
            int line=peek()->line;advance();skip_nl();
            Token *f=expect(TT_IDENT,"field name");
            Node *fe=mknode(N_FIELD,line);fe->left=n;fe->sval=xstrdup(f->val);n=fe;
        }else break;
    }
    return n;
}

static Node *parse_unary(void){
    skip_nl();
    if(check(TT_MINUS)||check(TT_NOT)||check(TT_TILDE)){
        Token *t=advance();Node *n=mknode(N_UNOP,t->line);
        n->op=xstrdup(t->val);n->left=parse_unary();return n;
    }
    return parse_postfix();
}

static int prec(TT t){
    switch(t){
        case TT_OR:         return 1;
        case TT_AND:        return 2;
        case TT_PIPE:       return 3;
        case TT_CARET:      return 4;
        case TT_AMPERSAND:  return 5;
        case TT_EQ:case TT_NEQ:return 6;
        case TT_LT:case TT_GT:case TT_LTE:case TT_GTE:return 7;
        case TT_SHL:case TT_SHR:return 8;
        case TT_PLUS:case TT_MINUS:return 9;
        case TT_STAR:case TT_SLASH:case TT_MOD:return 10;
        default:return 0;
    }
}

static Node *parse_binop(int min_prec){
    Node *lhs=parse_unary();
    for(;;){
        skip_nl();int p=prec(peek()->type);if(p<min_prec)break;
        Token *op=advance();skip_nl();Node *rhs=parse_binop(p+1);
        Node *n=mknode(N_BINOP,op->line);n->op=xstrdup(op->val);n->left=lhs;n->right=rhs;lhs=n;
    }
    return lhs;
}

static Node *parse_expr(void){
    Node *cond=parse_binop(1);
    skip_nl();
    if(peek()->type==TT_QUESTION){
        Token *qt=advance();skip_nl();
        Node *then_=parse_expr();skip_nl();
        if(peek()->type!=TT_COLON)die("%s:%d: expected ':' in ternary",peek()->file,peek()->line);
        advance();skip_nl();
        Node *else_=parse_expr();
        Node *n=mknode(N_TERNARY,qt->line);
        n->cond=cond;n->left=then_;n->right=else_;
        return n;
    }
    return cond;
}
static int is_lvalue(Node *n){return n->kind==N_IDENT||n->kind==N_INDEX||n->kind==N_FIELD;}

static int is_compound_assign(TT t){
    return t==TT_ASSIGN||t==TT_PLUS_ASSIGN||t==TT_MINUS_ASSIGN||
           t==TT_STAR_ASSIGN||t==TT_SLASH_ASSIGN||t==TT_MOD_ASSIGN||
           t==TT_SHL_ASSIGN||t==TT_SHR_ASSIGN||
           t==TT_AND_ASSIGN||t==TT_OR_ASSIGN||t==TT_XOR_ASSIGN;
}

static int is_type_tok(TT t){
    return t==TT_VOID||t==TT_INT||t==TT_STR||t==TT_BOOL||t==TT_LONG||
           t==TT_FLOAT||t==TT_DOUBLE||t==TT_CHAR||t==TT_UNSIGNED||t==TT_STAR;
}

static Node *parse_stmt(void){
    /* eat leading semis/newlines */
    while(1){TT t=peek()->type;if(t==TT_NEWLINE||t==TT_SEMI)advance();else break;}
    Token *t=peek();

    /* preprocessor nodes */
    if(t->type==TT_HASH_DEFINE){
        advance();
        Token *nm=expect(TT_IDENT,"macro name");
        Token *val=expect(TT_STR_LIT,"macro value");
        Node *n=mknode(N_ASM,t->line); /* reuse N_ASM as container, sval=name, import_path=val */
        n->kind=N_TYPEDEF_DECL; /* repurpose: name=macro name, sval=value */
        n->name=xstrdup(nm->val); n->sval=xstrdup(val->val);
        return n;
    }
    if(t->type==TT_HASH_IFDEF||t->type==TT_HASH_IFNDEF){
        int is_ifdef=(t->type==TT_HASH_IFDEF);advance();
        Token *nm=expect(TT_IDENT,"macro name");
        Node *n=mknode(N_IF,t->line);
        /* cond is a fake ident node holding the macro name */
        Node *cond=mknode(N_IDENT,nm->line);cond->name=xstrdup(nm->val);
        if(!is_ifdef){Node *u=mknode(N_UNOP,nm->line);u->op=xstrdup("not");u->left=cond;n->cond=u;}
        else n->cond=cond;
        /* body: parse until #endif or #else */
        NList body={0};
        while(peek()->type!=TT_HASH_ENDIF&&peek()->type!=TT_HASH_ELSE&&peek()->type!=TT_EOF)
            nl_push(&body,parse_stmt());
        n->body=body;
        if(peek()->type==TT_HASH_ELSE){
            advance();
            NList ebody={0};
            while(peek()->type!=TT_HASH_ENDIF&&peek()->type!=TT_EOF)
                nl_push(&ebody,parse_stmt());
            n->else_body=ebody;
        }
        if(peek()->type==TT_HASH_ENDIF)advance();
        return n;
    }
    if(t->type==TT_HASH_ENDIF||t->type==TT_HASH_ELSE||t->type==TT_HASH_UNDEF||t->type==TT_HASH_INCLUDE){
        advance();if(peek()->type==TT_STR_LIT)advance(); /* consume path/name */
        return mknode(N_EXPRSTMT,t->line); /* no-op */
    }

    if(t->type==TT_IMPORT){
        advance();Token *path=expect(TT_STR_LIT,"import path");
        Node *n=mknode(N_IMPORT,t->line);n->import_path=xstrdup(path->val);return n;
    }
    if(t->type==TT_RETURN){
        advance();Node *n=mknode(N_RETURN,t->line);skip_nl();
        if(!check(TT_NEWLINE)&&!check(TT_RBRACE)&&!check(TT_EOF)&&!check(TT_SEMI))
            n->left=parse_expr();
        return n;
    }
    if(t->type==TT_BREAK)   {advance();return mknode(N_BREAK,t->line);}
    if(t->type==TT_CONTINUE){advance();return mknode(N_CONTINUE,t->line);}

    /* switch(expr){ case x: ... default: ... } */
    if(t->type==TT_SWITCH){
        advance();Node *n=mknode(N_SWITCH,t->line);
        expect(TT_LPAREN,"(");skip_nl();n->cond=parse_expr();skip_nl();expect(TT_RPAREN,")");
        skip_nl();expect(TT_LBRACE,"{");
        NList cases={0};
        while(peek()->type!=TT_RBRACE&&peek()->type!=TT_EOF){
            skip_nl();
            if(peek()->type==TT_CASE){
                Token *ct=advance();skip_nl();
                Node *c=mknode(N_CASE,ct->line);
                c->left=parse_expr();skip_nl();
                expect(TT_COLON,":");
                NList cbody={0};
                while(peek()->type!=TT_CASE&&peek()->type!=TT_DEFAULT&&
                      peek()->type!=TT_RBRACE&&peek()->type!=TT_EOF){
                    if(peek()->type==TT_NEWLINE||peek()->type==TT_SEMI){advance();continue;}
                    nl_push(&cbody,parse_stmt());
                }
                c->body=cbody;nl_push(&cases,c);
            } else if(peek()->type==TT_DEFAULT){
                Token *dt=advance();skip_nl();expect(TT_COLON,":");
                Node *c=mknode(N_CASE,dt->line);c->left=NULL;
                NList cbody={0};
                while(peek()->type!=TT_CASE&&peek()->type!=TT_DEFAULT&&
                      peek()->type!=TT_RBRACE&&peek()->type!=TT_EOF){
                    if(peek()->type==TT_NEWLINE||peek()->type==TT_SEMI){advance();continue;}
                    nl_push(&cbody,parse_stmt());
                }
                c->body=cbody;nl_push(&cases,c);
            } else {advance();}
        }
        expect(TT_RBRACE,"}");n->body=cases;return n;
    }

    /* do { } while(cond); */
    if(t->type==TT_DO){
        advance();Node *n=mknode(N_DO,t->line);
        skip_nl();expect(TT_LBRACE,"{");n->body=parse_block();expect(TT_RBRACE,"}");
        skip_nl();
        if(peek()->type==TT_WHILE)advance();
        else die("%s:%d: expected 'while' after do-block",peek()->file,peek()->line);
        expect(TT_LPAREN,"(");skip_nl();n->cond=parse_expr();skip_nl();expect(TT_RPAREN,")");
        return n;
    }

    /* asm("...") inline assembly */
    if(t->type==TT_ASM){
        advance();expect(TT_LPAREN,"(");
        Token *body=expect(TT_STR_LIT,"asm string");
        expect(TT_RPAREN,")");
        Node *n=mknode(N_ASM,t->line);n->sval=xstrdup(body->val);return n;
    }

    if(t->type==TT_IF){
        advance();Node *n=mknode(N_IF,t->line);
        expect(TT_LPAREN,"(");skip_nl();n->cond=parse_expr();skip_nl();expect(TT_RPAREN,")");
        skip_nl();expect(TT_LBRACE,"{");n->body=parse_block();expect(TT_RBRACE,"}");
        for(;;){
            skip_nl();if(!check(TT_ELIF))break;advance();
            ElifClause ec={0};
            expect(TT_LPAREN,"(");skip_nl();ec.cond=parse_expr();skip_nl();expect(TT_RPAREN,")");
            skip_nl();expect(TT_LBRACE,"{");ec.body=parse_block();expect(TT_RBRACE,"}");
            el_push(&n->elifs,ec);
        }
        skip_nl();
        if(check(TT_ELSE)){
            advance();skip_nl();expect(TT_LBRACE,"{");n->else_body=parse_block();expect(TT_RBRACE,"}");
        }
        return n;
    }
    if(t->type==TT_WHILE){
        advance();Node *n=mknode(N_WHILE,t->line);
        expect(TT_LPAREN,"(");skip_nl();n->cond=parse_expr();skip_nl();expect(TT_RPAREN,")");
        skip_nl();expect(TT_LBRACE,"{");n->body=parse_block();expect(TT_RBRACE,"}");return n;
    }
    if(t->type==TT_FOR){
        advance();Node *n=mknode(N_FOR,t->line);
        expect(TT_LPAREN,"(");skip_nl();
        /* ObjFalcon for supports both comma-separated (Falcon) and semicolon-separated (C) */
        n->for_init=parse_stmt();skip_nl();
        TT sep=peek()->type;
        if(sep==TT_COMMA)advance();else if(sep==TT_SEMI)advance();
        else die("%s:%d: expected ',' or ';' in for",peek()->file,peek()->line);
        skip_nl();n->cond=parse_expr();skip_nl();
        sep=peek()->type;
        if(sep==TT_COMMA)advance();else if(sep==TT_SEMI)advance();
        else die("%s:%d: expected ',' or ';' in for",peek()->file,peek()->line);
        skip_nl();n->for_post=parse_stmt();skip_nl();expect(TT_RPAREN,")");
        skip_nl();expect(TT_LBRACE,"{");n->body=parse_block();expect(TT_RBRACE,"}");return n;
    }
    if(t->type==TT_LET){
        advance();skip_nl();
        Token *nm=expect(TT_IDENT,"variable name");skip_nl();
        Node *n=mknode(N_LETDECL,nm->line);n->name=xstrdup(nm->val);
        if(match(TT_COLON)){skip_nl();n->typeref=parse_type();skip_nl();}
        if(match(TT_ASSIGN)){skip_nl();n->left=parse_expr();}
        return n;
    }
    if(t->type==TT_CONST){
        advance();skip_nl();
        /* const can be followed by a type keyword (C-style) or identifier */
        if(is_type_tok(peek()->type)){
            /* C-style: const int NAME = expr */
            TypeRef *tr=parse_type();skip_nl();
            Token *nm=expect(TT_IDENT,"constant name");skip_nl();
            Node *n=mknode(N_CONSTDECL,nm->line);n->name=xstrdup(nm->val);n->is_const=1;
            n->typeref=tr;
            expect(TT_ASSIGN,"=");skip_nl();n->left=parse_expr();
            return n;
        }
        Token *nm=expect(TT_IDENT,"constant name");skip_nl();
        Node *n=mknode(N_CONSTDECL,nm->line);n->name=xstrdup(nm->val);n->is_const=1;
        if(match(TT_COLON)){skip_nl();n->typeref=parse_type();skip_nl();}
        expect(TT_ASSIGN,"=");skip_nl();n->left=parse_expr();
        return n;
    }

    /* C-style type-leading declaration: int x = ...; OR int f(...){...} */
    if(is_type_tok(t->type)||t->type==TT_UNSIGNED||t->type==TT_CHAR){
        int saved=gpos;
        TypeRef *tr=parse_type();skip_nl();
        if(peek()->type==TT_IDENT){
            Token *nm=advance();skip_nl();
            if(peek()->type==TT_LPAREN){
                /* C-style function definition: type name(params){ } */
                advance();skip_nl();
                Node *n=mknode(N_FUNC,nm->line);n->fname=xstrdup(nm->val);n->rettype=tr;
                while(!check(TT_RPAREN)&&!check(TT_EOF)){
                    skip_nl();
                    if(peek()->type==TT_VOID&&gpos+1<gntoks&&gtoks[gpos+1].type==TT_RPAREN){advance();break;}
                    TypeRef *pty=parse_type();skip_nl();
                    Token *pnm=expect(TT_IDENT,"param name");
                    Param p;p.name=xstrdup(pnm->val);p.type=pty;
                    pl_push(&n->params,p);skip_nl();
                    if(!match(TT_COMMA))break;skip_nl();
                }
                expect(TT_RPAREN,")");skip_nl();
                expect(TT_LBRACE,"{");n->body=parse_block();expect(TT_RBRACE,"}");
                return n;
            }
            /* C-style var: type name = expr; */
            Node *n=mknode(N_VARDECL,nm->line);n->name=xstrdup(nm->val);n->typeref=tr;skip_nl();
            if(match(TT_ASSIGN)){skip_nl();n->left=parse_expr();}
            return n;
        }
        gpos=saved; /* backtrack */
    }

    /* Falcon-style: name: type = expr */
    if(t->type==TT_IDENT){
        int saved=gpos;advance();skip_nl();
        if(check(TT_COLON)){
            advance();skip_nl();
            Node *n=mknode(N_VARDECL,t->line);n->name=xstrdup(t->val);
            n->typeref=parse_type();skip_nl();
            if(match(TT_ASSIGN)){skip_nl();n->left=parse_expr();}
            return n;
        }
        gpos=saved;
    }
    Node *expr=parse_expr();skip_nl();
    TT at=peek()->type;
    if(is_compound_assign(at)){
        if(!is_lvalue(expr))die("%s:%d: left side not assignable",peek()->file,peek()->line);
        Token *op=advance();skip_nl();
        Node *n=mknode(N_ASSIGN,op->line);n->op=xstrdup(op->val);n->left=expr;n->right=parse_expr();return n;
    }
    Node *s=mknode(N_EXPRSTMT,expr->line);s->left=expr;return s;
}

static NList parse_block(void){
    NList list={0};
    for(;;){
        /* skip newlines and semicolons between statements */
        while(1){TT t=peek()->type;if(t==TT_NEWLINE||t==TT_SEMI)advance();else break;}
        TT t=peek()->type;if(t==TT_RBRACE||t==TT_EOF)break;
        nl_push(&list,parse_stmt());
        /* eat trailing semicolons/newlines */
        while(1){TT tt=peek()->type;if(tt==TT_SEMI||tt==TT_NEWLINE)advance();else break;}
    }
    return list;
}

static Node *parse_func(void){
    Token *ft=expect(TT_FUNC,"func");Node *n=mknode(N_FUNC,ft->line);
    skip_nl();Token *nm=expect(TT_IDENT,"function name");n->fname=xstrdup(nm->val);
    skip_nl();expect(TT_LPAREN,"(");skip_nl();
    if(!check(TT_RPAREN)){
        do{
            skip_nl();if(check(TT_RPAREN))break;
            Token *pn=expect(TT_IDENT,"param name");skip_nl();expect(TT_COLON,":");skip_nl();
            Param p;p.name=xstrdup(pn->val);p.type=parse_type();pl_push(&n->params,p);skip_nl();
        }while(match(TT_COMMA));
    }
    skip_nl();expect(TT_RPAREN,")");skip_nl();
    n->rettype=mktype(TY_VOID,NULL,NULL);
    if(match(TT_ARROW)){skip_nl();n->rettype=parse_type();skip_nl();}
    expect(TT_LBRACE,"{");n->body=parse_block();expect(TT_RBRACE,"}");return n;
}

static Node *parse_struct(void){
    Token *st=expect(TT_STRUCT,"struct");Node *n=mknode(N_STRUCT,st->line);
    skip_nl();Token *nm=expect(TT_IDENT,"struct name");n->structname=xstrdup(nm->val);
    skip_nl();expect(TT_LBRACE,"{");skip_nl();
    while(!check(TT_RBRACE)&&!check(TT_EOF)){
        skip_nl();if(check(TT_RBRACE))break;
        Token *fn2=expect(TT_IDENT,"field name");skip_nl();expect(TT_COLON,":");skip_nl();
        TypeRef *ftype=parse_type();
        Field f;f.name=xstrdup(fn2->val);f.type=ftype;fl_push(&n->fields,f);
        skip_nl();match(TT_COMMA);skip_nl();
    }
    expect(TT_RBRACE,"}");return n;
}

/* ── parse_typedef ──────────────────────────────────────────────────────
   Three C-style forms supported:
     typedef int           myint           -- primitive / any type alias
     typedef ExistingName  NewName         -- struct or type alias
     typedef struct { f:t ... } Name       -- anonymous inline struct + alias
     typedef struct Tag { f:t ... } Name   -- tagged inline struct + alias
   ─────────────────────────────────────────────────────────────────────── */
static Node *parse_typedef(void){
    Token *tt=expect(TT_TYPEDEF,"typedef");
    int ln=tt->line;
    skip_nl();

    /* Form 3: typedef struct { ... } Name */
    if(check(TT_STRUCT)){
        advance();skip_nl();

        /* optional tag: typedef struct Tag { } Alias  OR  typedef struct { } Alias */
        char *tag=NULL;
        if(check(TT_IDENT)){
            /* look one token ahead to decide if this is a tag or something else */
            /* We save position, consume the ident, check for '{', then restore or keep */
            int saved=gpos;
            Token *maybe_tag=advance();
            skip_nl();
            if(check(TT_LBRACE)){
                tag=xstrdup(maybe_tag->val); /* it was a tag */
            } else {
                gpos=saved; /* not a tag, put it back */
            }
        }

        expect(TT_LBRACE,"{");skip_nl();

        /* Parse field list */
        Node *sn=mknode(N_STRUCT,ln);
        while(!check(TT_RBRACE)&&!check(TT_EOF)){
            skip_nl();if(check(TT_RBRACE))break;
            Token *fn2=expect(TT_IDENT,"field name");skip_nl();
            expect(TT_COLON,":");skip_nl();
            TypeRef *ftype=parse_type();
            Field f;f.name=xstrdup(fn2->val);f.type=ftype;fl_push(&sn->fields,f);
            skip_nl();match(TT_COMMA);skip_nl();
        }
        expect(TT_RBRACE,"}");skip_nl();

        /* alias name after the closing brace */
        Token *aname=expect(TT_IDENT,"typedef name");
        char *alias=xstrdup(aname->val);

        /* Name the struct with the alias; store tag in sval for typecheck */
        sn->structname=xstrdup(alias);
        if(tag) sn->sval=xstrdup(tag);

        /* Register the typedef alias -> TY_STRUCT(alias) at parse time
           so that any subsequent parse_type() calls can resolve it.
           The StructInfo is registered in typecheck's first pass. */
        TypeRef *tr=mktype(TY_STRUCT,alias,NULL);
        register_typedef(alias,tr);
        if(tag){
            TypeRef *tr2=mktype(TY_STRUCT,alias,NULL);
            register_typedef(tag,tr2);
        }
        return sn;
    }

    /* Forms 1 & 2: typedef <type> <newname> */
    TypeRef *base=parse_type();
    skip_nl();
    Token *aname=expect(TT_IDENT,"typedef name");
    register_typedef(aname->val,base);

    /* Return a dummy N_IMPORT node (no code generated, just keeps AST clean) */
    Node *dummy=mknode(N_IMPORT,ln);
    dummy->import_path=NULL;
    dummy->name=xstrdup(aname->val);
    return dummy;
}

static Node *parse_program(void){
    Node *prog=mknode(N_PROGRAM,1);
    for(;;){
        skip_nl();TT t=peek()->type;if(t==TT_EOF)break;
        if(t==TT_FUNC)          nl_push(&prog->body,parse_func());
        else if(t==TT_STRUCT)   nl_push(&prog->body,parse_struct());
        else if(t==TT_TYPEDEF)  nl_push(&prog->body,parse_typedef());
        else if(t==TT_IMPORT){
            advance();Token *path=expect(TT_STR_LIT,"import path");
            Node *im=mknode(N_IMPORT,path->line);im->import_path=xstrdup(path->val);
            nl_push(&prog->body,im);
        }
        /* C-style top-level: type funcname(...){} or type varname = expr; */
        else if(is_type_tok(t)||t==TT_UNSIGNED||t==TT_CHAR||t==TT_CONST){
            Node *s=parse_stmt();nl_push(&prog->body,s);
        }
        /* preprocessor at top level */
        else if(t==TT_HASH_DEFINE||t==TT_HASH_IFDEF||t==TT_HASH_IFNDEF||
                t==TT_HASH_ENDIF||t==TT_HASH_ELSE||t==TT_HASH_UNDEF||t==TT_HASH_INCLUDE){
            Node *s=parse_stmt();nl_push(&prog->body,s);
        }
        else if(t==TT_EXTERN){
            advance();skip_nl();
            Node *n=mknode(N_EXTERN,peek()->line);
            /* extern func name(params) -> ret  OR  extern type name(params) */
            if(peek()->type==TT_FUNC){
                advance();skip_nl();
                Token *nm=expect(TT_IDENT,"function name");
                n->fname=xstrdup(nm->val);skip_nl();
                expect(TT_LPAREN,"(");skip_nl();
                while(!check(TT_RPAREN)&&!check(TT_EOF)){
                    if(peek()->type==TT_VOID&&gpos+1<gntoks&&gtoks[gpos+1].type==TT_RPAREN){advance();break;}
                    TypeRef *pty=parse_type();skip_nl();
                    /* param name optional in extern */
                    char *pname="";
                    if(peek()->type==TT_IDENT){Token *pn=advance();pname=xstrdup(pn->val);}
                    Param p;p.name=pname;p.type=pty;
                    pl_push(&n->params,p);skip_nl();
                    if(!match(TT_COMMA))break;skip_nl();
                }
                expect(TT_RPAREN,")");skip_nl();
                if(match(TT_ARROW)){skip_nl();n->rettype=parse_type();}
                else n->rettype=mktype(TY_VOID,NULL,NULL);
            } else {
                /* C-style: extern rettype name(params) */
                TypeRef *ret=parse_type();skip_nl();
                Token *nm=expect(TT_IDENT,"function name");
                n->fname=xstrdup(nm->val);n->rettype=ret;skip_nl();
                expect(TT_LPAREN,"(");skip_nl();
                while(!check(TT_RPAREN)&&!check(TT_EOF)){
                    if(peek()->type==TT_VOID&&gpos+1<gntoks&&gtoks[gpos+1].type==TT_RPAREN){advance();break;}
                    TypeRef *pty=parse_type();skip_nl();
                    char *pname="";
                    if(peek()->type==TT_IDENT){Token *pn=advance();pname=xstrdup(pn->val);}
                    Param p;p.name=pname;p.type=pty;
                    pl_push(&n->params,p);skip_nl();
                    if(!match(TT_COMMA))break;skip_nl();
                }
                expect(TT_RPAREN,")");skip_nl();
            }
            nl_push(&prog->body,n);
        }
        else if(t==TT_SEMI||t==TT_NEWLINE){advance();}
        else die("%s:%d: unexpected top-level token '%s'",peek()->file,peek()->line,peek()->val);
    }
    return prog;
}

/* ═══════════════════════════════════════════════════════════════════════
   TYPE CHECKER
   Walks the AST after parsing. Annotates every expression node with
   its etype. Reports type errors with file+line and bails via die().
   ═══════════════════════════════════════════════════════════════════════ */

/* function registry for return-type lookup */
typedef struct{char *name;TypeRef *rettype;PList params;}FuncSig;
static FuncSig fsigs[1024];
static int     nfsigs=0;

static FuncSig *find_func(const char *name){
    for(int i=0;i<nfsigs;i++)if(strcmp(fsigs[i].name,name)==0)return &fsigs[i];
    return NULL;
}

/* struct registry (shared with codegen) */
typedef struct{char *name;Field *fields;int nfields;}StructInfo;
static StructInfo structs[512];
static int nstructs=0;

static StructInfo *find_struct(const char *name){
    /* direct lookup first */
    for(int i=0;i<nstructs;i++)if(strcmp(structs[i].name,name)==0)return &structs[i];
    /* then try to resolve through the typedef registry */
    TypeRef *aliased=resolve_typedef(name);
    if(aliased&&aliased->kind==TY_STRUCT&&aliased->name&&strcmp(aliased->name,name)!=0)
        return find_struct(aliased->name);
    return NULL;
}

/* ── typedef registry ───────────────────────────────────────────────── */
typedef struct{char *alias;TypeRef *type;}TypeAlias;
static TypeAlias typedefs[1024];
static int ntypedefs=0;

static void register_typedef(const char *alias,TypeRef *t){
    for(int i=0;i<ntypedefs;i++)
        if(strcmp(typedefs[i].alias,alias)==0)
            die("typedef: '%s' already defined",alias);
    if(ntypedefs>=1024)die("too many typedefs");
    typedefs[ntypedefs].alias=xstrdup(alias);
    typedefs[ntypedefs].type=t;
    ntypedefs++;
}

/* Resolve alias chain up to 64 hops */
static TypeRef *resolve_typedef(const char *name){
    const char *cur=name;
    for(int depth=0;depth<64;depth++){
        int found=0;
        for(int i=0;i<ntypedefs;i++){
            if(strcmp(typedefs[i].alias,cur)==0){
                TypeRef *t=typedefs[i].type;
                if(t->kind==TY_STRUCT&&t->name){
                    int is_alias=0;
                    for(int j=0;j<ntypedefs;j++)
                        if(strcmp(typedefs[j].alias,t->name)==0){is_alias=1;break;}
                    if(is_alias){cur=t->name;found=1;break;}
                }
                return t;
            }
        }
        if(!found)break;
    }
    return NULL;
}

/* variable type stack (same logical layout as codegen vars) */
typedef struct{char *name;TypeRef *type;int is_const;}TVar;
static TVar  tvars[4096];
static int   ntvars=0;

static TypeRef *find_tvar(const char *name){
    for(int i=ntvars-1;i>=0;i--)if(strcmp(tvars[i].name,name)==0)return tvars[i].type;
    return NULL;
}
static void push_tvar(const char *name,TypeRef *t){
    if(ntvars>=4096)die("too many variables");
    tvars[ntvars].name=xstrdup(name);tvars[ntvars].type=t;tvars[ntvars].is_const=0;ntvars++;
}
static void push_tvar_const(const char *name,TypeRef *t){
    if(ntvars>=4096)die("too many variables");
    tvars[ntvars].name=xstrdup(name);tvars[ntvars].type=t;tvars[ntvars].is_const=1;ntvars++;
}

static TypeRef *tc_expr(Node *n,TypeRef *expected_ret);

/* return type of a builtin/runtime call; NULL means "not a known builtin" */
static TypeRef *builtin_rettype(const char *name){
    if(has_std&&strcmp(name,"print")==0)     return mktype(TY_VOID,NULL,NULL);
    if(strcmp(name,"len")==0)                return mktype(TY_INT,NULL,NULL);
    if(strcmp(name,"str_concat")==0)         return mktype(TY_STR,NULL,NULL);
    if(strcmp(name,"str_format")==0)         return mktype(TY_STR,NULL,NULL);
    if(strcmp(name,"_flr_str_len")==0||
       strcmp(name,"_flr_strlen")==0)        return mktype(TY_INT,NULL,NULL);
    if(strcmp(name,"_flr_str_eq")==0)        return mktype(TY_INT,NULL,NULL);
    if(strcmp(name,"_flr_int_to_str")==0)    return mktype(TY_STR,NULL,NULL);
    if(strcmp(name,"_flr_str_to_int")==0)    return mktype(TY_INT,NULL,NULL);
    if(strcmp(name,"_flr_abs")==0)           return mktype(TY_INT,NULL,NULL);
    if(strcmp(name,"_flr_min")==0||
       strcmp(name,"_flr_max")==0)           return mktype(TY_INT,NULL,NULL);
    if(strcmp(name,"_flr_alloc")==0)         return mktype(TY_INT,NULL,NULL);
    if(strcmp(name,"_flr_free")==0)          return mktype(TY_VOID,NULL,NULL);
    if(strcmp(name,"_flr_exit")==0)          return mktype(TY_VOID,NULL,NULL);
    if(strcmp(name,"_flr_assert")==0)        return mktype(TY_VOID,NULL,NULL);
    /* intrinsics */
    if(strcmp(name,"__syscall")==0)          return mktype(TY_INT,NULL,NULL);
    if(strcmp(name,"__rdtsc")==0)            return mktype(TY_INT,NULL,NULL);
    if(strcmp(name,"__peek")==0||
       strcmp(name,"__peekb")==0||
       strcmp(name,"__inb")==0)              return mktype(TY_INT,NULL,NULL);
    if(strcmp(name,"__poke")==0||
       strcmp(name,"__pokeb")==0||
       strcmp(name,"__outb")==0||
       strcmp(name,"__cli")==0||
       strcmp(name,"__sti")==0||
       strcmp(name,"__hlt")==0||
       strcmp(name,"__memset")==0||
       strcmp(name,"__memcpy")==0)           return mktype(TY_VOID,NULL,NULL);
    return NULL;
}

static TypeRef *tc_expr(Node *n,TypeRef *ret){
    if(!n){n->etype=mktype(TY_VOID,NULL,NULL);return n->etype;}
    switch(n->kind){
    case N_INTLIT:  n->etype=mktype(TY_INT,NULL,NULL); break;
    case N_LONGLIT: n->etype=mktype(TY_LONG,NULL,NULL);break;
    case N_FLOATLIT: n->etype=mktype(TY_FLOAT,NULL,NULL); break;
    case N_DOUBLELIT:n->etype=mktype(TY_DOUBLE,NULL,NULL); break;
    case N_STRLIT:  n->etype=mktype(TY_STR,NULL,NULL); break;
    case N_BOOLLIT: n->etype=mktype(TY_BOOL,NULL,NULL);break;
    case N_IDENT:{
        TypeRef *vt=find_tvar(n->name);
        if(!vt)die("%s:%d: undefined variable '%s'",n->file,n->line,n->name);
        n->etype=vt;break;
    }
    case N_UNOP:{
        TypeRef *et=tc_expr(n->left,ret);
        if(strcmp(n->op,"not")==0){
            n->etype=mktype(TY_BOOL,NULL,NULL);
        }else if(strcmp(n->op,"-")==0||strcmp(n->op,"~")==0){
            if(!type_is_numeric(et))
                die("%s:%d: unary '%s' requires numeric type, got %s",n->file,n->line,n->op,type_name(et));
            n->etype=et;
        }
        break;
    }
    case N_BINOP:{
        TypeRef *lt=tc_expr(n->left,ret);
        TypeRef *rt=tc_expr(n->right,ret);
        const char *op=n->op;
        /* comparison ops always return bool */
        if(strcmp(op,"==")==0||strcmp(op,"!=")==0||
           strcmp(op,"<")==0||strcmp(op,">")==0||
           strcmp(op,"<=")==0||strcmp(op,">=")==0){
            if(!types_compat(lt,rt))
                die("%s:%d: cannot compare %s with %s",n->file,n->line,type_name(lt),type_name(rt));
            n->etype=mktype(TY_BOOL,NULL,NULL);
        }else if(strcmp(op,"and")==0||strcmp(op,"or")==0){
            n->etype=mktype(TY_BOOL,NULL,NULL);
        }else if(strcmp(op,"+")==0&&(lt->kind==TY_STR||rt->kind==TY_STR)){
            /* str + str only allowed if both are str; otherwise error */
            if(lt->kind!=TY_STR||rt->kind!=TY_STR)
                die("%s:%d: cannot add str and %s — use str_concat()",n->file,n->line,
                    lt->kind!=TY_STR?type_name(lt):type_name(rt));
            /* rewrite as str_concat call in etype but keep node as binop */
            n->etype=mktype(TY_STR,NULL,NULL);
        }else{
            if(!type_is_numeric(lt)||!type_is_numeric(rt)){
                /* bitwise/arith — both must be numeric */
                if(!type_is_numeric(lt))
                    die("%s:%d: operator '%s' requires numeric left operand, got %s",
                        n->file,n->line,op,type_name(lt));
                if(!type_is_numeric(rt))
                    die("%s:%d: operator '%s' requires numeric right operand, got %s",
                        n->file,n->line,op,type_name(rt));
            }
            /* result is long if either side is long */
            if(lt->kind==TY_LONG||rt->kind==TY_LONG)
                n->etype=mktype(TY_LONG,NULL,NULL);
            /* float/double propagation */
            else if(lt->kind==TY_DOUBLE||rt->kind==TY_DOUBLE)
                n->etype=mktype(TY_DOUBLE,NULL,NULL);
            else if(lt->kind==TY_FLOAT||rt->kind==TY_FLOAT)
                n->etype=mktype(TY_FLOAT,NULL,NULL);
            else
                n->etype=mktype(TY_INT,NULL,NULL);
        }
        break;
    }
    case N_CALL:{
        /* check each argument */
        for(int i=0;i<n->args.n;i++)tc_expr(n->args.d[i],ret);
        /* virtual std special: print takes any single value */
        if(has_std&&strcmp(n->callee,"print")==0){
            if(n->args.n!=1)die("%s:%d: print takes 1 argument",n->file,n->line);
            n->etype=mktype(TY_VOID,NULL,NULL);break;
        }
        /* str_concat(a:str, b:str) -> str */
        if(strcmp(n->callee,"str_concat")==0){
            if(n->args.n!=2)die("%s:%d: str_concat takes 2 arguments",n->file,n->line);
            TypeRef *a=n->args.d[0]->etype,*b=n->args.d[1]->etype;
            if(a->kind!=TY_STR)die("%s:%d: str_concat arg1 must be str, got %s",n->file,n->line,type_name(a));
            if(b->kind!=TY_STR)die("%s:%d: str_concat arg2 must be str, got %s",n->file,n->line,type_name(b));
            n->etype=mktype(TY_STR,NULL,NULL);break;
        }
        /* str_format(fmt:str, ...) -> str — fmt must be str literal or str var */
        if(strcmp(n->callee,"str_format")==0){
            if(n->args.n<1)die("%s:%d: str_format needs at least a format string",n->file,n->line);
            TypeRef *fmt=n->args.d[0]->etype;
            if(fmt->kind!=TY_STR)die("%s:%d: str_format first arg must be str, got %s",n->file,n->line,type_name(fmt));
            if(n->args.n>9)die("%s:%d: str_format supports at most 8 format arguments",n->file,n->line);
            n->etype=mktype(TY_STR,NULL,NULL);break;
        }
        /* known builtins */
        TypeRef *brt=builtin_rettype(n->callee);
        if(brt){n->etype=brt;break;}
        /* user-defined functions */
        FuncSig *sig=find_func(n->callee);
        if(sig){
            /* check arg count */
            if(n->args.n!=sig->params.n)
                die("%s:%d: function '%s' expects %d args, got %d",
                    n->file,n->line,n->callee,sig->params.n,n->args.n);
            /* check arg types */
            for(int i=0;i<n->args.n;i++){
                TypeRef *at=n->args.d[i]->etype;
                TypeRef *pt=sig->params.d[i].type;
                if(!types_compat(at,pt))
                    die("%s:%d: arg %d of '%s': expected %s, got %s",
                        n->file,n->line,i+1,n->callee,type_name(pt),type_name(at));
            }
            n->etype=sig->rettype;break;
        }
        /* unknown — allow (runtime/external) with void type, warn */
        warn("%s:%d: unknown function '%s' — assuming void",n->file,n->line,n->callee);
        n->etype=mktype(TY_VOID,NULL,NULL);
        break;
    }
    case N_INDEX:{
        TypeRef *at=tc_expr(n->left,ret);
        TypeRef *it=tc_expr(n->right,ret);
        if(at->kind!=TY_ARRAY)
            die("%s:%d: index operator on non-array type %s",n->file,n->line,type_name(at));
        if(!type_is_numeric(it))
            die("%s:%d: array index must be numeric, got %s",n->file,n->line,type_name(it));
        n->etype=at->elem?at->elem:mktype(TY_INT,NULL,NULL);
        break;
    }
    case N_FIELD:{
        TypeRef *st=tc_expr(n->left,ret);
        if(st->kind!=TY_STRUCT&&st->kind!=TY_INT&&st->kind!=TY_PTR){
            /* tolerate int/ptr (raw alloc returns int) */
        }
        /* look up field type */
        if(st->kind==TY_STRUCT&&st->name){
            StructInfo *si=find_struct(st->name);
            if(si){
                for(int i=0;i<si->nfields;i++){
                    if(strcmp(si->fields[i].name,n->sval)==0){
                        n->etype=si->fields[i].type;goto field_done;
                    }
                }
                die("%s:%d: struct '%s' has no field '%s'",n->file,n->line,st->name,n->sval);
            }
        }
        /* raw pointer / unknown struct: assume int field */
        n->etype=mktype(TY_INT,NULL,NULL);
        field_done:;
        break;
    }
    case N_ARRAYLIT:{
        TypeRef *elem=NULL;
        for(int i=0;i<n->elems.n;i++){
            TypeRef *et=tc_expr(n->elems.d[i],ret);
            if(!elem)elem=et;
            else if(!types_compat(elem,et))
                die("%s:%d: array literal has mixed types (%s vs %s)",
                    n->file,n->line,type_name(elem),type_name(et));
        }
        if(!elem)elem=mktype(TY_INT,NULL,NULL);
        n->etype=mktype(TY_ARRAY,NULL,elem);
        break;
    }
    case N_TERNARY:
        tc_expr(n->cond,ret);
        tc_expr(n->left,ret);
        tc_expr(n->right,ret);
        n->etype=n->left->etype?n->left->etype:mktype(TY_INT,NULL,NULL);
        break;
    default:
        n->etype=mktype(TY_VOID,NULL,NULL);break;
    }
    return n->etype;
}

static void tc_stmts(NList *stmts,TypeRef *ret_type,int in_loop);

static void tc_stmt(Node *n,TypeRef *ret_type,int in_loop){
    if(!n)return;
    switch(n->kind){
    case N_IMPORT:break;
    case N_VARDECL:
    case N_LETDECL:{
        TypeRef *decl=n->typeref;
        if(n->left){
            TypeRef *et=tc_expr(n->left,ret_type);
            if(decl&&!types_compat(decl,et))
                die("%s:%d: cannot assign %s to variable '%s' of type %s",
                    n->file,n->line,type_name(et),n->name,type_name(decl));
        }
        push_tvar(n->name,decl?decl:(n->left?n->left->etype:mktype(TY_INT,NULL,NULL)));
        break;
    }
    case N_CONSTDECL:{
        TypeRef *decl=n->typeref;
        if(!n->left)die("%s:%d: const '%s' requires an initializer",n->file,n->line,n->name);
        TypeRef *et=tc_expr(n->left,ret_type);
        if(decl&&!types_compat(decl,et))
            die("%s:%d: cannot assign %s to const '%s' of type %s",
                n->file,n->line,type_name(et),n->name,type_name(decl));
        push_tvar_const(n->name,decl?decl:et);
        break;
    }
    case N_ASSIGN:{
        TypeRef *rhs=tc_expr(n->right,ret_type);
        TypeRef *lhs=tc_expr(n->left,ret_type);
        /* check const immutability */
        if(n->left->kind==N_IDENT){
            for(int ci=ntvars-1;ci>=0;ci--){
                if(strcmp(tvars[ci].name,n->left->name)==0){
                    if(tvars[ci].is_const)
                        die("%s:%d: cannot assign to const '%s'",n->file,n->line,n->left->name);
                    break;
                }
            }
        }
        if(strcmp(n->op,"=")==0){
            if(!types_compat(lhs,rhs))
                die("%s:%d: cannot assign %s to %s",n->file,n->line,type_name(rhs),type_name(lhs));
        }else{
            /* compound assign: both sides must be numeric (or str for +=) */
            if(!type_is_numeric(lhs)&&!(strcmp(n->op,"+=")==0&&lhs->kind==TY_STR))
                die("%s:%d: compound assignment requires numeric left operand, got %s",
                    n->file,n->line,type_name(lhs));
        }
        break;
    }
    case N_RETURN:{
        TypeRef *et=n->left?tc_expr(n->left,ret_type):mktype(TY_VOID,NULL,NULL);
        if(ret_type&&ret_type->kind!=TY_VOID&&!types_compat(ret_type,et))
            die("%s:%d: return type mismatch: function returns %s, got %s",
                n->file,n->line,type_name(ret_type),type_name(et));
        break;
    }
    case N_EXPRSTMT:tc_expr(n->left,ret_type);break;
    case N_IF:{
        TypeRef *ct=tc_expr(n->cond,ret_type);
        (void)ct;/* any type allowed in condition */
        tc_stmts(&n->body,ret_type,in_loop);
        for(int i=0;i<n->elifs.n;i++){
            tc_expr(n->elifs.d[i].cond,ret_type);
            tc_stmts(&n->elifs.d[i].body,ret_type,in_loop);
        }
        tc_stmts(&n->else_body,ret_type,in_loop);
        break;
    }
    case N_WHILE:{
        tc_expr(n->cond,ret_type);
        tc_stmts(&n->body,ret_type,1);
        break;
    }
    case N_FOR:{
        tc_stmt(n->for_init,ret_type,0);
        tc_expr(n->cond,ret_type);
        tc_stmt(n->for_post,ret_type,0);
        tc_stmts(&n->body,ret_type,1);
        break;
    }
    case N_BREAK:case N_CONTINUE:
        if(!in_loop)die("%s:%d: break/continue outside loop",n->file,n->line);
        break;
    case N_SWITCH:
        tc_expr(n->cond,ret_type);
        for(int i=0;i<n->body.n;i++){
            Node *c=n->body.d[i];
            if(c->left)tc_expr(c->left,ret_type);
            tc_stmts(&c->body,ret_type,1);
        }
        break;
    case N_DO:
        tc_stmts(&n->body,ret_type,1);
        tc_expr(n->cond,ret_type);
        break;
    case N_ASM:case N_TYPEDEF_DECL:break;
    case N_FUNC:
        tc_stmts(&n->body,n->rettype,0);break;
    default:break;
    }
}

static void tc_stmts(NList *stmts,TypeRef *ret_type,int in_loop){
    for(int i=0;i<stmts->n;i++)tc_stmt(stmts->d[i],ret_type,in_loop);
}

static void typecheck(Node *prog){
    /* first pass: register all structs and function signatures */
    for(int i=0;i<prog->body.n;i++){
        Node *n=prog->body.d[i];
        if(n->kind==N_STRUCT){
            /* avoid double-registering (typedef struct {} Name already called
               register_typedef at parse time; we still need the StructInfo) */
            if(!find_struct(n->structname)){
                StructInfo *si=&structs[nstructs++];
                si->name=xstrdup(n->structname);
                si->fields=n->fields.d;si->nfields=n->fields.n;
            }
            /* if this was 'typedef struct Tag { } Alias', also register Tag */
            if(n->sval&&!find_struct(n->sval)){
                StructInfo *si2=&structs[nstructs++];
                si2->name=xstrdup(n->sval);
                si2->fields=n->fields.d;si2->nfields=n->fields.n;
            }
        }
        if(n->kind==N_FUNC||n->kind==N_EXTERN){
            if(nfsigs>=1024)die("too many functions");
            fsigs[nfsigs].name=xstrdup(n->fname);
            fsigs[nfsigs].rettype=n->rettype;
            fsigs[nfsigs].params=n->params;
            nfsigs++;
        }
    }
    /* second pass: type-check each function body */
    for(int i=0;i<prog->body.n;i++){
        Node *n=prog->body.d[i];
        if(n->kind!=N_FUNC)continue;
        int saved_ntvars=ntvars;
        /* push params into scope */
        for(int j=0;j<n->params.n;j++)
            push_tvar(n->params.d[j].name,n->params.d[j].type);
        tc_stmts(&n->body,n->rettype,0);
        ntvars=saved_ntvars;/* pop function scope */
    }
}

/* ═══════════════════════════════════════════════════════════════════════
   CODE GENERATOR — x86-32 GAS AT&T, cdecl, int 0x80
   ═══════════════════════════════════════════════════════════════════════ */

static char  *out_buf=NULL;
static size_t out_len=0,out_cap=0;

static void out(const char *fmt,...){
    char tmp[4096];va_list ap;va_start(ap,fmt);int n=vsnprintf(tmp,sizeof tmp,fmt,ap);va_end(ap);
    if(out_len+n+1>out_cap){
        out_cap=out_cap?out_cap*2:65536;
        while(out_cap<out_len+n+1)out_cap*=2;
        out_buf=realloc(out_buf,out_cap);
    }
    memcpy(out_buf+out_len,tmp,n);out_len+=n;out_buf[out_len]=0;
}

static char *str_lits[8192];
static int   nstr_lits=0;
static int add_strlit(const char *s){
    for(int i=0;i<nstr_lits;i++)if(strcmp(str_lits[i],s)==0)return i;
    str_lits[nstr_lits]=xstrdup(s);return nstr_lits++;
}
typedef struct{double val;int is_double;}FLit;
static FLit flits[1024];
static int  nflits=0;
static int add_flit(double v,int is_double){
    for(int i=0;i<nflits;i++)if(flits[i].val==v&&flits[i].is_double==is_double)return i;
    flits[nflits].val=v;flits[nflits].is_double=is_double;return nflits++;
}

typedef struct{char *name;int offset;TypeRef *type;}Var;
static Var  vars[4096];
static int  nvars=0,frame_sz=0,lbl_cnt=0;
static char break_lbl[64],cont_lbl[64];
static int  has_std=0,freestanding=0;

/* ── architecture selection ──────────────────────────────────────── */
#define ARCH_X86_32 32
#define ARCH_X86_64 64
static int arch=ARCH_X86_32;   /* default: 32-bit */

/* x86-64 System V AMD64 ABI integer argument registers (in order) */
static const char *argregs64[6]={"rdi","rsi","rdx","rcx","r8","r9"};

static int   new_label(void){return lbl_cnt++;}
static Var  *find_var(const char *name){
    for(int i=nvars-1;i>=0;i--)if(strcmp(vars[i].name,name)==0)return &vars[i];return NULL;
}
static int alloc_var(const char *name,TypeRef *type){
    int sz;
    if(arch==ARCH_X86_64){
        /* on 64-bit every slot is 8 bytes (simplest, always aligned) */
        sz=8;
    } else {
        /* long/double occupy 8 bytes; float 4 bytes; everything else 4 bytes */
        sz=(type&&(type->kind==TY_LONG||type->kind==TY_DOUBLE))?8:4;
    }
    frame_sz+=sz;
    vars[nvars].name=xstrdup(name);
    vars[nvars].offset=-frame_sz;
    vars[nvars].type=type;
    nvars++;
    return -frame_sz;
}

static void gen_expr(Node *n);
static void gen_stmt(Node *n);
static void gen_func(Node *fn);
static void gen_func64(Node *fn);

/* ── long (64-bit) helpers emitted once into the output ──────────── */
static int long_helpers_emitted=0;
static void need_long_helpers(void){
    if(long_helpers_emitted)return;
    long_helpers_emitted=1;
    /* _flr_ladd(alo,ahi,blo,bhi) -> edx:eax */
    out("_flr_ladd:\n");
    out("    movl 4(%%esp),%%eax\n    movl 8(%%esp),%%edx\n");
    out("    addl 12(%%esp),%%eax\n    adcl 16(%%esp),%%edx\n    ret\n\n");
    /* _flr_lsub */
    out("_flr_lsub:\n");
    out("    movl 4(%%esp),%%eax\n    movl 8(%%esp),%%edx\n");
    out("    subl 12(%%esp),%%eax\n    sbbl 16(%%esp),%%edx\n    ret\n\n");
    /* _flr_lmul (lo word only via imul trick) */
    out("_flr_lmul:\n");
    out("    pushl %%ebp\n    movl %%esp,%%ebp\n");
    out("    movl 8(%%ebp),%%eax\n    movl 12(%%ebp),%%ecx\n"); /* alo, ahi */
    out("    movl 16(%%ebp),%%edx\n    movl 20(%%ebp),%%esi\n"); /* blo, bhi */
    /* result_hi = alo*bhi + ahi*blo + (alo*blo)>>32 */
    out("    pushl %%esi\n    pushl %%ecx\n    pushl %%edx\n    pushl %%eax\n");
    out("    imull %%edx,%%ecx\n"); /* ahi*blo -> ecx */
    out("    imull %%eax,%%esi\n"); /* alo*bhi -> esi */
    out("    mull %%edx\n");        /* alo*blo -> edx:eax */
    out("    addl %%ecx,%%edx\n    addl %%esi,%%edx\n");
    out("    addl $16,%%esp\n    leave\n    ret\n\n");
    /* _flr_lprint(lo,hi) — print signed 64-bit integer */
    out("_flr_lprint:\n");
    out("    pushl %%ebp\n    movl %%esp,%%ebp\n");
    out("    pushl %%esi\n    pushl %%edi\n    pushl %%ebx\n");
    out("    movl 8(%%ebp),%%eax\n    movl 12(%%ebp),%%edx\n"); /* lo, hi */
    /* negative? */
    out("    testl %%edx,%%edx\n    jge .Llpr_pos\n");
    out("    negl %%eax\n    adcl $0,%%edx\n    negl %%edx\n"); /* negate 64-bit */
    out("    movl $1,%%esi\n    jmp .Llpr_go\n");
    out(".Llpr_pos:\n    xorl %%esi,%%esi\n");
    out(".Llpr_go:\n");
    /* convert to decimal using 64-bit division by 10 */
    out("    leal .Lflr_ibuf+23,%%edi\n    movb $10,(%%edi)\n    decl %%edi\n");
    out(".Llpr_l:\n");
    out("    pushl %%edx\n    pushl %%eax\n"); /* save hi:lo */
    out("    movl $10,%%ecx\n");
    /* divide 64-bit by 10: use double division */
    out("    xorl %%edx,%%edx\n    movl 4(%%esp),%%eax\n    divl %%ecx\n"); /* hi/10 */
    out("    movl %%eax,%%ebx\n");             /* quotient hi */
    out("    movl (%%esp),%%eax\n    divl %%ecx\n"); /* lo/10, edx=rem */
    out("    popl %%ecx\n    popl %%ecx\n");   /* discard saved */
    out("    addb $48,%%dl\n    movb %%dl,(%%edi)\n    decl %%edi\n");
    out("    orl %%ebx,%%eax\n    je .Llpr_done\n    movl %%ebx,%%edx\n");
    out("    jmp .Llpr_l\n");
    out(".Llpr_done:\n");
    out("    testl %%esi,%%esi\n    je .Llpr_nomin\n");
    out("    movb $45,(%%edi)\n    decl %%edi\n");
    out(".Llpr_nomin:\n    incl %%edi\n");
    out("    leal .Lflr_ibuf+24,%%edx\n    subl %%edi,%%edx\n    movl %%edi,%%ecx\n");
    out("    movl $4,%%eax\n    movl $1,%%ebx\n    int $0x80\n");
    out("    movl $4,%%eax\n    movl $1,%%ebx\n    leal .Lflr_nl,%%ecx\n    movl $1,%%edx\n    int $0x80\n");
    out("    popl %%ebx\n    popl %%edi\n    popl %%esi\n    leave\n    ret\n\n");
}

/* push a long variable: pushes hi then lo (so lo is at lower address on stack) */
static void push_long_var(int off){
    /* off is the LOW word offset (as returned by alloc_var for long) */
    out("    pushl %d(%%ebp)\n",off+4); /* hi */
    out("    pushl %d(%%ebp)\n",off);   /* lo */
}

/* ── hardware intrinsics ──────────────────────────────────────────── */
static void gen_intrinsic(Node *n){
    const char *nm=n->callee;
    if(strcmp(nm,"__syscall")==0){
        int argc=n->args.n;
        if(argc<1)die("__syscall: need at least syscall number");
        out("    pushl %%esi\n    pushl %%edi\n    pushl %%ebx\n");
        for(int i=argc-1;i>=0;i--){gen_expr(n->args.d[i]);out("    pushl %%eax\n");}
        out("    popl %%eax\n");
        if(argc>1)out("    popl %%ebx\n");else out("    xorl %%ebx,%%ebx\n");
        if(argc>2)out("    popl %%ecx\n");else out("    xorl %%ecx,%%ecx\n");
        if(argc>3)out("    popl %%edx\n");else out("    xorl %%edx,%%edx\n");
        if(argc>4)out("    popl %%esi\n");else out("    xorl %%esi,%%esi\n");
        if(argc>5)out("    popl %%edi\n");else out("    xorl %%edi,%%edi\n");
        out("    int $0x80\n");
        out("    popl %%ebx\n    popl %%edi\n    popl %%esi\n");
        return;
    }
    if(strcmp(nm,"__inb")==0){
        if(n->args.n!=1)die("__inb takes 1 argument");
        gen_expr(n->args.d[0]);
        out("    movw %%ax,%%dx\n    xorl %%eax,%%eax\n    inb %%dx,%%al\n    movzbl %%al,%%eax\n");
        return;
    }
    if(strcmp(nm,"__outb")==0){
        if(n->args.n!=2)die("__outb takes 2 arguments");
        gen_expr(n->args.d[1]);out("    pushl %%eax\n");
        gen_expr(n->args.d[0]);out("    movw %%ax,%%dx\n");
        out("    popl %%eax\n    outb %%al,%%dx\n    xorl %%eax,%%eax\n");
        return;
    }
    if(strcmp(nm,"__cli")==0){out("    cli\n    xorl %%eax,%%eax\n");return;}
    if(strcmp(nm,"__sti")==0){out("    sti\n    xorl %%eax,%%eax\n");return;}
    if(strcmp(nm,"__hlt")==0){out("    hlt\n    xorl %%eax,%%eax\n");return;}
    if(strcmp(nm,"__rdtsc")==0){out("    rdtsc\n");return;}
    if(strcmp(nm,"__peek")==0){
        if(n->args.n!=1)die("__peek takes 1 argument");
        gen_expr(n->args.d[0]);out("    movl (%%eax),%%eax\n");return;
    }
    if(strcmp(nm,"__poke")==0){
        if(n->args.n!=2)die("__poke takes 2 arguments");
        gen_expr(n->args.d[1]);out("    pushl %%eax\n");
        gen_expr(n->args.d[0]);out("    popl %%ecx\n    movl %%ecx,(%%eax)\n    xorl %%eax,%%eax\n");return;
    }
    if(strcmp(nm,"__peekb")==0){
        if(n->args.n!=1)die("__peekb takes 1 argument");
        gen_expr(n->args.d[0]);out("    movzbl (%%eax),%%eax\n");return;
    }
    if(strcmp(nm,"__pokeb")==0){
        if(n->args.n!=2)die("__pokeb takes 2 arguments");
        gen_expr(n->args.d[1]);out("    pushl %%eax\n");
        gen_expr(n->args.d[0]);out("    popl %%ecx\n    movb %%cl,(%%eax)\n    xorl %%eax,%%eax\n");return;
    }
    if(strcmp(nm,"__memset")==0){
        if(n->args.n!=3)die("__memset takes 3 arguments");
        gen_expr(n->args.d[2]);out("    pushl %%eax\n");
        gen_expr(n->args.d[1]);out("    pushl %%eax\n");
        gen_expr(n->args.d[0]);out("    pushl %%eax\n");
        out("    call _flr_memset\n    addl $12,%%esp\n");return;
    }
    if(strcmp(nm,"__memcpy")==0){
        if(n->args.n!=3)die("__memcpy takes 3 arguments");
        gen_expr(n->args.d[2]);out("    pushl %%eax\n");
        gen_expr(n->args.d[1]);out("    pushl %%eax\n");
        gen_expr(n->args.d[0]);out("    pushl %%eax\n");
        out("    call _flr_memcpy\n    addl $12,%%esp\n");return;
    }
    die("unknown intrinsic '%s'",nm);
}

/* ── expression codegen ─────────────────────────────────────────── */
static void gen_expr(Node *n){
    if(!n){out("    xorl %%eax,%%eax\n");return;}
    switch(n->kind){
    case N_INTLIT:
        out("    movl $%lld,%%eax\n",n->ival);break;
    case N_LONGLIT:
        /* result in edx:eax */
        out("    movl $%lld,%%eax\n",(int)(n->ival&0xFFFFFFFFLL));
        out("    movl $%lld,%%edx\n",(int)(n->ival>>32));
        break;
    case N_BOOLLIT:
        out("    movl $%d,%%eax\n",n->bval?1:0);break;
    case N_FLOATLIT:{
        /* store float constant in rodata, load with flds, push as int bits via st(0) */
        int idx=add_flit(n->dval,0);
        out("    flds .Lfl%d\n",idx);
        out("    subl $4,%%esp\n    fstps (%%esp)\n    popl %%eax\n");break;
    }
    case N_DOUBLELIT:{
        int idx=add_flit(n->dval,1);
        out("    fldl .Lfl%d\n",idx);
        /* return in edx:eax (push 8 bytes, pop lo then hi) */
        out("    subl $8,%%esp\n    fstpl (%%esp)\n");
        out("    popl %%eax\n    popl %%edx\n");break;
    }
    case N_STRLIT:
        out("    leal .Lstr%d,%%eax\n",add_strlit(n->sval));break;
    case N_IDENT:{
        Var *v=find_var(n->name);
        if(!v)die("%s:%d: undefined variable '%s'",n->file,n->line,n->name);
        if(v->type&&v->type->kind==TY_FLOAT){
            out("    flds %d(%%ebp)\n",v->offset);
            out("    subl $4,%%esp\n    fstps (%%esp)\n    popl %%eax\n");
        } else if(v->type&&v->type->kind==TY_DOUBLE){
            out("    fldl %d(%%ebp)\n",v->offset);
            out("    subl $8,%%esp\n    fstpl (%%esp)\n");
            out("    popl %%eax\n    popl %%edx\n");
        } else {
            out("    movl %d(%%ebp),%%eax\n",v->offset);
            if(v->type&&v->type->kind==TY_LONG)
                out("    movl %d(%%ebp),%%edx\n",v->offset+4);
        }
        break;
    }
    case N_BINOP:{
        const char *op=n->op;
        int is_long=(n->etype&&n->etype->kind==TY_LONG);
        int llong=(n->left&&n->left->etype&&n->left->etype->kind==TY_LONG);
        int rlong=(n->right&&n->right->etype&&n->right->etype->kind==TY_LONG);

        /* str + str → str_concat */
        if(strcmp(op,"+")==0&&n->left->etype&&n->left->etype->kind==TY_STR){
            gen_expr(n->right);out("    pushl %%eax\n");
            gen_expr(n->left);out("    pushl %%eax\n");
            out("    call _flr_str_concat\n    addl $8,%%esp\n");
            break;
        }
        /* float/double arithmetic via x87 */
        int is_fp=(n->etype&&(n->etype->kind==TY_FLOAT||n->etype->kind==TY_DOUBLE));
        int lf=(n->left&&n->left->etype&&(n->left->etype->kind==TY_FLOAT||n->left->etype->kind==TY_DOUBLE));
        int rf=(n->right&&n->right->etype&&(n->right->etype->kind==TY_FLOAT||n->right->etype->kind==TY_DOUBLE));
        int use_dbl=(n->etype&&n->etype->kind==TY_DOUBLE)||
                    (n->left&&n->left->etype&&n->left->etype->kind==TY_DOUBLE)||
                    (n->right&&n->right->etype&&n->right->etype->kind==TY_DOUBLE);
        if(is_fp||lf||rf){
            /* push rhs as float/double onto x87 stack */
            gen_expr(n->right);
            if(rf&&use_dbl&&n->right->etype&&n->right->etype->kind==TY_DOUBLE){
                out("    pushl %%edx\n    pushl %%eax\n    fldl (%%esp)\n    addl $8,%%esp\n");
            } else if(rf){
                out("    pushl %%eax\n    flds (%%esp)\n    addl $4,%%esp\n");
                if(use_dbl)out("    fldl .Lfl_zero\n    faddp\n"); /* widen to double */
            } else {
                /* int/long operand: convert via fild */
                out("    pushl %%eax\n    filds (%%esp)\n    addl $4,%%esp\n");
            }
            /* push lhs */
            gen_expr(n->left);
            if(lf&&use_dbl&&n->left->etype&&n->left->etype->kind==TY_DOUBLE){
                out("    pushl %%edx\n    pushl %%eax\n    fldl (%%esp)\n    addl $8,%%esp\n");
            } else if(lf){
                out("    pushl %%eax\n    flds (%%esp)\n    addl $4,%%esp\n");
                if(use_dbl)out("    fldl .Lfl_zero\n    faddp\n");
            } else {
                out("    pushl %%eax\n    filds (%%esp)\n    addl $4,%%esp\n");
            }
            /* st(0)=lhs, st(1)=rhs; operate */
            int is_cmp=(!strcmp(op,"==")||!strcmp(op,"!=")||!strcmp(op,"<")||!strcmp(op,">")||!strcmp(op,"<=")||!strcmp(op,">="));
            if(!strcmp(op,"+")) out("    faddp\n");
            else if(!strcmp(op,"-")) out("    fsubrp\n");
            else if(!strcmp(op,"*")) out("    fmulp\n");
            else if(!strcmp(op,"/")) out("    fdivrp\n");
            else if(is_cmp){
                int lc=new_label();
                out("    fucomip %%st(1),%%st\n    fstp %%st(0)\n");
                if(!strcmp(op,"==")){out("    sete %%al\n");}
                else if(!strcmp(op,"!=")){out("    setne %%al\n");}
                else if(!strcmp(op,"<")) {out("    setb %%al\n");}
                else if(!strcmp(op,">")) {out("    seta %%al\n");}
                else if(!strcmp(op,"<=")){out("    setbe %%al\n");}
                else if(!strcmp(op,">=")){out("    setae %%al\n");}
                out("    movzbl %%al,%%eax\n");
                break;
            }
            else die("unsupported float operator '%s'",op);
            /* store result back to eax (float) or edx:eax (double) */
            if(use_dbl){
                out("    subl $8,%%esp\n    fstpl (%%esp)\n");
                out("    popl %%eax\n    popl %%edx\n");
            } else {
                out("    subl $4,%%esp\n    fstps (%%esp)\n    popl %%eax\n");
            }
            break;
        }

        if(is_long||llong||rlong){
            need_long_helpers();
            /* push rhs */
            gen_expr(n->right);
            if(rlong){out("    pushl %%edx\n    pushl %%eax\n");}
            else     {out("    cdq\n    pushl %%edx\n    pushl %%eax\n");}
            /* push lhs */
            gen_expr(n->left);
            if(llong){out("    pushl %%edx\n    pushl %%eax\n");}
            else     {out("    cdq\n    pushl %%edx\n    pushl %%eax\n");}

            if(strcmp(op,"+")==0){
                out("    call _flr_ladd\n    addl $16,%%esp\n");
            }else if(strcmp(op,"-")==0){
                out("    call _flr_lsub\n    addl $16,%%esp\n");
            }else if(strcmp(op,"*")==0){
                out("    call _flr_lmul\n    addl $16,%%esp\n");
            }else{
                /* comparison: compare hi then lo */
                out("    popl %%eax\n    popl %%edx\n"); /* lhs lo,hi */
                out("    popl %%ecx\n    popl %%esi\n"); /* rhs lo,hi */
                out("    cmpl %%esi,%%edx\n"); /* compare hi */
                int l=new_label();
                if(strcmp(op,"==")==0){
                    out("    jne .Llcmp%d\n",l);
                    out("    cmpl %%ecx,%%eax\n");
                    out(".Llcmp%d:\n    sete %%al\n    movzbl %%al,%%eax\n",l);
                }else if(strcmp(op,"!=")==0){
                    out("    jne .Llcmp%d\n",l);
                    out("    cmpl %%ecx,%%eax\n");
                    out(".Llcmp%d:\n    setne %%al\n    movzbl %%al,%%eax\n",l);
                }else if(strcmp(op,"<")==0){
                    out("    jne .Llcmp%d\n",l);out("    cmpl %%ecx,%%eax\n");
                    out(".Llcmp%d:\n    setl %%al\n    movzbl %%al,%%eax\n",l);
                }else if(strcmp(op,">")==0){
                    out("    jne .Llcmp%d\n",l);out("    cmpl %%ecx,%%eax\n");
                    out(".Llcmp%d:\n    setg %%al\n    movzbl %%al,%%eax\n",l);
                }else if(strcmp(op,"<=")==0){
                    out("    jne .Llcmp%d\n",l);out("    cmpl %%ecx,%%eax\n");
                    out(".Llcmp%d:\n    setle %%al\n    movzbl %%al,%%eax\n",l);
                }else if(strcmp(op,">=")==0){
                    out("    jne .Llcmp%d\n",l);out("    cmpl %%ecx,%%eax\n");
                    out(".Llcmp%d:\n    setge %%al\n    movzbl %%al,%%eax\n",l);
                }else{
                    die("unsupported long operator '%s'",op);
                }
            }
            break;
        }

        /* normal 32-bit */
        gen_expr(n->right);out("    pushl %%eax\n");
        gen_expr(n->left); out("    popl %%ecx\n");
        if     (strcmp(op,"+")==0) out("    addl %%ecx,%%eax\n");
        else if(strcmp(op,"-")==0) out("    subl %%ecx,%%eax\n");
        else if(strcmp(op,"*")==0) out("    imull %%ecx,%%eax\n");
        else if(strcmp(op,"/")==0){out("    cdq\n");out("    idivl %%ecx\n");}
        else if(strcmp(op,"%")==0){out("    cdq\n");out("    idivl %%ecx\n");out("    movl %%edx,%%eax\n");}
        else if(strcmp(op,"&")==0) out("    andl %%ecx,%%eax\n");
        else if(strcmp(op,"|")==0) out("    orl  %%ecx,%%eax\n");
        else if(strcmp(op,"^")==0) out("    xorl %%ecx,%%eax\n");
        else if(strcmp(op,"<<")==0)out("    shll %%cl,%%eax\n");
        else if(strcmp(op,">>")==0)out("    sarl %%cl,%%eax\n");
        else if(strcmp(op,"==")==0){out("    cmpl %%ecx,%%eax\n");out("    sete %%al\n");out("    movzbl %%al,%%eax\n");}
        else if(strcmp(op,"!=")==0){out("    cmpl %%ecx,%%eax\n");out("    setne %%al\n");out("    movzbl %%al,%%eax\n");}
        else if(strcmp(op,"<")==0) {out("    cmpl %%ecx,%%eax\n");out("    setl %%al\n");out("    movzbl %%al,%%eax\n");}
        else if(strcmp(op,">")==0) {out("    cmpl %%ecx,%%eax\n");out("    setg %%al\n");out("    movzbl %%al,%%eax\n");}
        else if(strcmp(op,"<=")==0){out("    cmpl %%ecx,%%eax\n");out("    setle %%al\n");out("    movzbl %%al,%%eax\n");}
        else if(strcmp(op,">=")==0){out("    cmpl %%ecx,%%eax\n");out("    setge %%al\n");out("    movzbl %%al,%%eax\n");}
        else if(strcmp(op,"and")==0){
            out("    testl %%eax,%%eax\n");out("    setne %%al\n");
            out("    testl %%ecx,%%ecx\n");out("    setne %%cl\n");
            out("    andb %%cl,%%al\n");out("    movzbl %%al,%%eax\n");
        }
        else if(strcmp(op,"or")==0){
            out("    orl %%ecx,%%eax\n");out("    setne %%al\n");out("    movzbl %%al,%%eax\n");
        }
        else die("unknown binop '%s'",op);
        break;
    }
    case N_UNOP:
        gen_expr(n->left);
        if     (strcmp(n->op,"-"  )==0)out("    negl %%eax\n");
        else if(strcmp(n->op,"~"  )==0)out("    notl %%eax\n");
        else if(strcmp(n->op,"not")==0){out("    testl %%eax,%%eax\n");out("    sete %%al\n");out("    movzbl %%al,%%eax\n");}
        break;
    case N_CALL:{
        if(is_intrinsic(n->callee)){gen_intrinsic(n);break;}
        if(has_std){
            /* print: dispatch on etype */
            if(strcmp(n->callee,"print")==0&&n->args.n==1){
                Node *arg=n->args.d[0];
                int is_long_arg=(arg->etype&&arg->etype->kind==TY_LONG);
                if(is_long_arg){
                    need_long_helpers();
                    gen_expr(arg);
                    out("    pushl %%edx\n    pushl %%eax\n");
                    out("    call _flr_lprint\n    addl $8,%%esp\n");
                    break;
                }
                int is_str2=(arg->etype&&arg->etype->kind==TY_STR);
                int is_fa=(arg->etype&&arg->etype->kind==TY_FLOAT);
                int is_da=(arg->etype&&arg->etype->kind==TY_DOUBLE);
                if(!is_fa&&!is_da&&arg->kind==N_IDENT){
                    Var *v=find_var(arg->name);
                    if(v&&v->type&&v->type->kind==TY_FLOAT)is_fa=1;
                    if(v&&v->type&&v->type->kind==TY_DOUBLE)is_da=1;
                }
                gen_expr(arg);
                if(is_fa){out("    pushl %%eax\n    call _flr_print_float\n    addl $4,%%esp\n");break;}
                if(is_da){out("    pushl %%edx\n    pushl %%eax\n    call _flr_print_double\n    addl $8,%%esp\n");break;}
                out("    pushl %%eax\n");
                out("    call %s\n    addl $4,%%esp\n",
                    is_str2?"_flr_print_str":"_flr_print_int");
                break;
            }
            if(strcmp(n->callee,"len")==0&&n->args.n==1){
                gen_expr(n->args.d[0]);out("    movl (%%eax),%%eax\n");break;
            }
            /* str_concat(a, b) -> str */
            if(strcmp(n->callee,"str_concat")==0&&n->args.n==2){
                gen_expr(n->args.d[1]);out("    pushl %%eax\n");
                gen_expr(n->args.d[0]);out("    pushl %%eax\n");
                out("    call _flr_str_concat\n    addl $8,%%esp\n");
                break;
            }
            /* str_format(fmt, ...) -> str */
            if(strcmp(n->callee,"str_format")==0&&n->args.n>=1){
                /* push args right-to-left then fmt last */
                for(int i=n->args.n-1;i>=1;i--){
                    gen_expr(n->args.d[i]);out("    pushl %%eax\n");
                }
                out("    pushl $%d\n",n->args.n-1); /* nargs */
                gen_expr(n->args.d[0]);out("    pushl %%eax\n");
                out("    call _flr_str_format\n    addl $%d,%%esp\n",(n->args.n+1)*4);
                break;
            }
        }
        for(int i=n->args.n-1;i>=0;i--){gen_expr(n->args.d[i]);out("    pushl %%eax\n");}
        out("    call %s\n",n->callee);
        if(n->args.n>0)out("    addl $%d,%%esp\n",n->args.n*4);
        break;
    }
    case N_INDEX:
        gen_expr(n->right);out("    pushl %%eax\n");
        gen_expr(n->left); out("    popl %%ecx\n");
        out("    leal 8(%%eax,%%ecx,4),%%eax\n");
        out("    movl (%%eax),%%eax\n");break;
    case N_FIELD:{
        gen_expr(n->left);
        int found_off=-1;
        for(int si=0;si<nstructs&&found_off<0;si++)
            for(int fi=0;fi<structs[si].nfields;fi++)
                if(strcmp(structs[si].fields[fi].name,n->sval)==0){found_off=fi*4;break;}
        if(found_off<0)die("%s:%d: unknown field '%s'",n->file,n->line,n->sval);
        out("    movl %d(%%eax),%%eax\n",found_off);break;
    }
    case N_ARRAYLIT:{
        int cnt=n->elems.n,alloc_sz=8+cnt*4;
        /* use sbrk(alloc_sz) via sys_brk trick: just call _flr_alloc */
        out("    pushl $%d\n",alloc_sz);
        out("    call _flr_alloc\n    addl $4,%%esp\n");
        out("    pushl %%eax\n");
        out("    movl $%d,(%%eax)\n    movl $%d,4(%%eax)\n",cnt,cnt);
        for(int i=0;i<cnt;i++){
            out("    movl (%%esp),%%edi\n");
            gen_expr(n->elems.d[i]);
            out("    movl %%eax,%d(%%edi)\n",8+i*4);
        }
        out("    popl %%eax\n");break;
    }
    case N_TERNARY:{
        int lp=lbl_cnt++;
        gen_expr(n->cond);
        out("    cmpl $0,%%eax\n    je .Ltern_else%d\n",lp);
        gen_expr(n->left);
        out("    jmp .Ltern_end%d\n",lp);
        out(".Ltern_else%d:\n",lp);
        gen_expr(n->right);
        out(".Ltern_end%d:\n",lp);
        break;
    }
    default:die("gen_expr: unhandled kind %d",n->kind);
    }
}

static void gen_store(Node *lv){
    switch(lv->kind){
    case N_IDENT:{
        Var *v=find_var(lv->name);
        if(!v)die("%s:%d: undefined variable '%s'",lv->file,lv->line,lv->name);
        if(v->type&&v->type->kind==TY_FLOAT){
            out("    pushl %%eax\n    flds (%%esp)\n    addl $4,%%esp\n");
            out("    fstps %d(%%ebp)\n",v->offset);
        } else if(v->type&&v->type->kind==TY_DOUBLE){
            out("    pushl %%edx\n    pushl %%eax\n    fldl (%%esp)\n    addl $8,%%esp\n");
            out("    fstpl %d(%%ebp)\n",v->offset);
        } else {
            out("    movl %%eax,%d(%%ebp)\n",v->offset);
            if(v->type&&v->type->kind==TY_LONG)
                out("    movl %%edx,%d(%%ebp)\n",v->offset+4);
        }
        break;
    }
    case N_INDEX:
        out("    pushl %%eax\n");
        gen_expr(lv->right);out("    pushl %%eax\n");
        gen_expr(lv->left); out("    popl %%ecx\n");
        out("    leal 8(%%eax,%%ecx,4),%%edx\n");
        out("    popl %%eax\n    movl %%eax,(%%edx)\n");break;
    case N_FIELD:{
        out("    pushl %%eax\n");gen_expr(lv->left);
        int found_off=-1;
        for(int si=0;si<nstructs&&found_off<0;si++)
            for(int fi=0;fi<structs[si].nfields;fi++)
                if(strcmp(structs[si].fields[fi].name,lv->sval)==0){found_off=fi*4;break;}
        if(found_off<0)die("unknown field '%s'",lv->sval);
        out("    popl %%ecx\n    movl %%ecx,%d(%%eax)\n",found_off);break;
    }
    default:die("gen_store: not an lvalue");
    }
}

static void gen_stmt(Node *n){
    if(!n)return;
    switch(n->kind){
    case N_IMPORT:if(strcmp(n->import_path,"std")==0)has_std=1;break;
    case N_VARDECL:
    case N_LETDECL:
    case N_CONSTDECL:{
        TypeRef *vtype=n->typeref;
        /* infer type from initializer if not declared */
        if(!vtype&&n->left&&n->left->etype)vtype=n->left->etype;
        int off=alloc_var(n->name,vtype);
        if(n->left){
            gen_expr(n->left);
            if(vtype&&vtype->kind==TY_FLOAT){
                /* eax holds float bits; store with flds/fstps */
                out("    pushl %%eax\n    flds (%%esp)\n    addl $4,%%esp\n");
                out("    fstps %d(%%ebp)\n",off);
            } else if(vtype&&vtype->kind==TY_DOUBLE){
                out("    pushl %%edx\n    pushl %%eax\n    fldl (%%esp)\n    addl $8,%%esp\n");
                out("    fstpl %d(%%ebp)\n",off);
            } else {
                out("    movl %%eax,%d(%%ebp)\n",off);
                if(vtype&&vtype->kind==TY_LONG)
                    out("    movl %%edx,%d(%%ebp)\n",off+4);
            }
        }else{
            out("    movl $0,%d(%%ebp)\n",off);
            if(vtype&&(vtype->kind==TY_LONG||vtype->kind==TY_DOUBLE))
                out("    movl $0,%d(%%ebp)\n",off+4);
        }
        break;
    }
    case N_ASSIGN:{
        gen_expr(n->right);
        if(strcmp(n->op,"=")==0){gen_store(n->left);}
        else{
            out("    pushl %%eax\n");gen_expr(n->left);out("    popl %%ecx\n");
            if     (strcmp(n->op,"+=")==0)out("    addl %%ecx,%%eax\n");
            else if(strcmp(n->op,"-=")==0)out("    subl %%ecx,%%eax\n");
            else if(strcmp(n->op,"*=")==0)out("    imull %%ecx,%%eax\n");
            else if(strcmp(n->op,"/=")==0){out("    pushl %%ecx\n    cdq\n    idivl (%%esp)\n    addl $4,%%esp\n");}
            else if(strcmp(n->op,"%=")==0){out("    pushl %%ecx\n    cdq\n    idivl (%%esp)\n    addl $4,%%esp\n    movl %%edx,%%eax\n");}
            else if(strcmp(n->op,"&=")==0)out("    andl %%ecx,%%eax\n");
            else if(strcmp(n->op,"|=")==0)out("    orl  %%ecx,%%eax\n");
            else if(strcmp(n->op,"^=")==0)out("    xorl %%ecx,%%eax\n");
            else if(strcmp(n->op,"<<=")==0)out("    shll %%cl,%%eax\n");
            else if(strcmp(n->op,">>=")==0)out("    sarl %%cl,%%eax\n");
            gen_store(n->left);
        }
        break;
    }
    case N_RETURN:
        if(n->left)gen_expr(n->left);else out("    xorl %%eax,%%eax\n");
        out("    popl %%ebx\n    popl %%edi\n    popl %%esi\n");
        out("    leave\n    ret\n");break;
    case N_EXPRSTMT:{
        Node *e=n->left;
        if(has_std&&e->kind==N_CALL&&strcmp(e->callee,"print")==0){
            if(e->args.n!=1)die("print takes 1 argument");
            Node *arg=e->args.d[0];
            int is_long_arg=(arg->etype&&arg->etype->kind==TY_LONG);
            if(is_long_arg){
                need_long_helpers();
                gen_expr(arg);
                out("    pushl %%edx\n    pushl %%eax\n");
                out("    call _flr_lprint\n    addl $8,%%esp\n");
                break;
            }
            int is_str=(arg->etype&&arg->etype->kind==TY_STR);
            if(!is_str&&arg->kind==N_IDENT){Var *v=find_var(arg->name);if(v&&v->type&&v->type->kind==TY_STR)is_str=1;}
            int is_float_arg=(arg->etype&&arg->etype->kind==TY_FLOAT);
            int is_double_arg=(arg->etype&&arg->etype->kind==TY_DOUBLE);
            if(!is_float_arg&&!is_double_arg&&arg->kind==N_IDENT){
                Var *v=find_var(arg->name);
                if(v&&v->type&&v->type->kind==TY_FLOAT)is_float_arg=1;
                if(v&&v->type&&v->type->kind==TY_DOUBLE)is_double_arg=1;
            }
            if(is_float_arg||is_double_arg){
                gen_expr(arg);
                if(is_double_arg){
                    out("    pushl %%edx\n    pushl %%eax\n");
                    out("    call _flr_print_double\n    addl $8,%%esp\n");
                } else {
                    out("    pushl %%eax\n");
                    out("    call _flr_print_float\n    addl $4,%%esp\n");
                }
                break;
            }
            gen_expr(arg);out("    pushl %%eax\n");
            out("    call %s\n    addl $4,%%esp\n",is_str?"_flr_print_str":"_flr_print_int");
            break;
        }
        gen_expr(e);break;
    }
    case N_IF:{
        int lend=new_label(),lnext=new_label();
        gen_expr(n->cond);out("    testl %%eax,%%eax\n    je .Lif%d\n",lnext);
        for(int i=0;i<n->body.n;i++)gen_stmt(n->body.d[i]);
        out("    jmp .Lif%d\n.Lif%d:\n",lend,lnext);
        for(int ei=0;ei<n->elifs.n;ei++){
            ElifClause *ec=&n->elifs.d[ei];int ln2=new_label();
            gen_expr(ec->cond);out("    testl %%eax,%%eax\n    je .Lif%d\n",ln2);
            for(int i=0;i<ec->body.n;i++)gen_stmt(ec->body.d[i]);
            out("    jmp .Lif%d\n.Lif%d:\n",lend,ln2);
        }
        for(int i=0;i<n->else_body.n;i++)gen_stmt(n->else_body.d[i]);
        out(".Lif%d:\n",lend);break;
    }
    case N_WHILE:{
        int ls=new_label(),le=new_label();
        char ob[64],oc[64];strcpy(ob,break_lbl);strcpy(oc,cont_lbl);
        snprintf(break_lbl,64,".Lwh%d",le);snprintf(cont_lbl,64,".Lwh%d",ls);
        out(".Lwh%d:\n",ls);gen_expr(n->cond);out("    testl %%eax,%%eax\n    je .Lwh%d\n",le);
        for(int i=0;i<n->body.n;i++)gen_stmt(n->body.d[i]);
        out("    jmp .Lwh%d\n.Lwh%d:\n",ls,le);
        strcpy(break_lbl,ob);strcpy(cont_lbl,oc);break;
    }
    case N_FOR:{
        int ls=new_label(),le=new_label(),lp=new_label();
        char ob[64],oc[64];strcpy(ob,break_lbl);strcpy(oc,cont_lbl);
        snprintf(break_lbl,64,".Lfor%d",le);snprintf(cont_lbl,64,".Lfor%d",lp);
        gen_stmt(n->for_init);
        out(".Lfor%d:\n",ls);gen_expr(n->cond);out("    testl %%eax,%%eax\n    je .Lfor%d\n",le);
        for(int i=0;i<n->body.n;i++)gen_stmt(n->body.d[i]);
        out(".Lfor%d:\n",lp);gen_stmt(n->for_post);
        out("    jmp .Lfor%d\n.Lfor%d:\n",ls,le);
        strcpy(break_lbl,ob);strcpy(cont_lbl,oc);break;
    }
    case N_BREAK:
        if(!break_lbl[0])die("%s:%d: break outside loop",n->file,n->line);
        out("    jmp %s\n",break_lbl);break;
    case N_CONTINUE:
        if(!cont_lbl[0])die("%s:%d: continue outside loop",n->file,n->line);
        out("    jmp %s\n",cont_lbl);break;
    case N_ASM:
        /* inline asm: emit raw string as assembly */
        out("    %s\n",n->sval);break;
    case N_TYPEDEF_DECL:
        /* #define macro — no code emission, ignored at codegen */
        break;
    case N_DO:{
        int lp=lbl_cnt++;
        char old_brk[32],old_cont[32];
        strcpy(old_brk,break_lbl);strcpy(old_cont,cont_lbl);
        snprintf(break_lbl,32,".Ldobrk%d",lp);
        snprintf(cont_lbl,32,".Ldocnt%d",lp);
        out(".Ldoloop%d:\n",lp);
        for(int i=0;i<n->body.n;i++)gen_stmt(n->body.d[i]);
        out("%s:\n",cont_lbl);
        gen_expr(n->cond);
        out("    cmpl $0,%%eax\n    jne .Ldoloop%d\n",lp);
        out("%s:\n",break_lbl);
        strcpy(break_lbl,old_brk);strcpy(cont_lbl,old_cont);
        break;
    }
    case N_SWITCH:{
        int lp=lbl_cnt++;
        char old_brk[32];strcpy(old_brk,break_lbl);
        snprintf(break_lbl,32,".Lswbrk%d",lp);
        gen_expr(n->cond);
        out("    pushl %%eax\n"); /* save switch value */
        /* emit jump table */
        for(int i=0;i<n->body.n;i++){
            Node *c=n->body.d[i];
            if(c->left){
                gen_expr(c->left);
                out("    cmpl %%eax,(%%esp)\n    je .Lcase%d_%d\n",lp,i);
            }
        }
        /* find default */
        for(int i=0;i<n->body.n;i++){
            Node *c=n->body.d[i];
            if(!c->left){out("    jmp .Lcase%d_%d\n",lp,i);break;}
        }
        out("    jmp %s\n",break_lbl);
        for(int i=0;i<n->body.n;i++){
            Node *c=n->body.d[i];
            out(".Lcase%d_%d:\n",lp,i);
            for(int j=0;j<c->body.n;j++)gen_stmt(c->body.d[j]);
        }
        out("    addl $4,%%esp\n"); /* pop switch value */
        out("%s:\n",break_lbl);
        strcpy(break_lbl,old_brk);
        break;
    }
    case N_FUNC:
        /* nested function def (C-style at top level parsed into body) — generate it */
        gen_func(n);break;
    default:die("gen_stmt: unhandled kind %d",n->kind);
    }
}

static void gen_func(Node *fn){
    nvars=0;frame_sz=0;/* lbl_cnt is global, not reset per function */
    memset(break_lbl,0,sizeof break_lbl);memset(cont_lbl,0,sizeof cont_lbl);
    for(int i=0;i<fn->params.n;i++){
        Var *v=&vars[nvars++];
        v->name=xstrdup(fn->params.d[i].name);
        v->offset=8+i*4;
        v->type=fn->params.d[i].type;
    }
    size_t body_start=out_len;
    for(int i=0;i<fn->body.n;i++)gen_stmt(fn->body.d[i]);
    size_t body_len=out_len-body_start;
    char *body_asm=malloc(body_len+1);
    memcpy(body_asm,out_buf+body_start,body_len);body_asm[body_len]=0;
    out_len=body_start;out_buf[out_len]=0;

    int fsz=(frame_sz+15)&~15;
    out(".globl %s\n%s:\n",fn->fname,fn->fname);
    out("    pushl %%ebp\n    movl %%esp,%%ebp\n");
    if(fsz>0)out("    subl $%d,%%esp\n",fsz);
    out("    pushl %%esi\n    pushl %%edi\n    pushl %%ebx\n");

    if(out_len+body_len+1>out_cap){out_cap=out_cap*2+body_len+64;out_buf=realloc(out_buf,out_cap);}
    memcpy(out_buf+out_len,body_asm,body_len);out_len+=body_len;out_buf[out_len]=0;free(body_asm);

    if(strcmp(fn->fname,"main")==0){
        if(!freestanding){
            out("    popl %%ebx\n    popl %%edi\n    popl %%esi\n");
            out("    xorl %%ebx,%%ebx\n    movl $1,%%eax\n    int $0x80\n");
        }else{
            out("    popl %%ebx\n    popl %%edi\n    popl %%esi\n");
            out("    cli\n.Lhlt_loop:\n    hlt\n    jmp .Lhlt_loop\n");
        }
    }else{
        out("    popl %%ebx\n    popl %%edi\n    popl %%esi\n");
        out("    leave\n    ret\n");
    }
    out("\n");
}

/* ── runtime (inlined when hosted + has_std) ────────────────────── */
static void emit_runtime(void){
    if(!has_std||freestanding)return;
    out("# ── Falcon Runtime (inline) ───────────────────────────\n");
    /* ELF entry point */
    out(".globl _start\n_start:\n");
    out("    xorl %%ebp,%%ebp\n");
    out("    call main\n");
    out("    movl %%eax,%%ebx\n");
    out("    movl $1,%%eax\n");
    out("    int $0x80\n\n");

    /* memset / memcpy */
    out("_flr_memset:\n    pushl %%ebp\n    movl %%esp,%%ebp\n    pushl %%edi\n");
    out("    movl 8(%%ebp),%%edi\n    movl 12(%%ebp),%%eax\n    movl 16(%%ebp),%%ecx\n");
    out("    rep stosb\n    popl %%edi\n    leave\n    ret\n\n");
    out("_flr_memcpy:\n    pushl %%ebp\n    movl %%esp,%%ebp\n    pushl %%esi\n    pushl %%edi\n");
    out("    movl 8(%%ebp),%%edi\n    movl 12(%%ebp),%%esi\n    movl 16(%%ebp),%%ecx\n");
    out("    rep movsb\n    popl %%edi\n    popl %%esi\n    leave\n    ret\n\n");

    /* print_str */
    out("_flr_print_str:\n    pushl %%ebp\n    movl %%esp,%%ebp\n    pushl %%esi\n");
    out("    movl 8(%%ebp),%%esi\n    movl %%esi,%%ecx\n");
    out(".Lfps_l:\n    cmpb $0,(%%ecx)\n    je .Lfps_d\n    incl %%ecx\n    jmp .Lfps_l\n");
    out(".Lfps_d:\n    subl %%esi,%%ecx\n    movl %%ecx,%%edx\n    movl %%esi,%%ecx\n");
    out("    movl $4,%%eax\n    movl $1,%%ebx\n    int $0x80\n");
    out("    movl $4,%%eax\n    movl $1,%%ebx\n    leal .Lflr_nl,%%ecx\n    movl $1,%%edx\n    int $0x80\n");
    out("    popl %%esi\n    leave\n    ret\n\n");

    /* print_int */
    out("_flr_print_int:\n    pushl %%ebp\n    movl %%esp,%%ebp\n    pushl %%esi\n    pushl %%edi\n");
    out("    movl 8(%%ebp),%%eax\n");
    out("    leal .Lflr_ibuf+11,%%edi\n    movb $10,(%%edi)\n    decl %%edi\n");
    out("    testl %%eax,%%eax\n    jge .Lfpi_pos\n    negl %%eax\n    movl $1,%%esi\n    jmp .Lfpi_l\n");
    out(".Lfpi_pos:\n    xorl %%esi,%%esi\n");
    out(".Lfpi_l:\n    movl $10,%%ecx\n    xorl %%edx,%%edx\n    divl %%ecx\n");
    out("    addb $48,%%dl\n    movb %%dl,(%%edi)\n    decl %%edi\n");
    out("    testl %%eax,%%eax\n    jne .Lfpi_l\n");
    out("    testl %%esi,%%esi\n    je .Lfpi_nom\n    movb $45,(%%edi)\n    decl %%edi\n");
    out(".Lfpi_nom:\n    incl %%edi\n");
    out("    leal .Lflr_ibuf+12,%%edx\n    subl %%edi,%%edx\n    movl %%edi,%%ecx\n");
    out("    movl $4,%%eax\n    movl $1,%%ebx\n    int $0x80\n");
    out("    popl %%edi\n    popl %%esi\n    leave\n    ret\n\n");

    /* exit */
    out("_flr_exit:\n    movl 4(%%esp),%%ebx\n    movl $1,%%eax\n    int $0x80\n\n");

    /* _flr_print_float(bits:int) — print a 32-bit float */
    out("_flr_print_float:\n");
    out("    pushl %%ebp\n    movl %%esp,%%ebp\n");
    out("    pushl %%esi\n    pushl %%edi\n    pushl %%ebx\n");
    /* load float from stack arg, convert to double for printing */
    out("    flds 8(%%ebp)\n");
    out("    subl $8,%%esp\n    fstpl (%%esp)\n");
    out("    pushl 4(%%esp)\n    pushl 4(%%esp)\n"); /* push hi,lo of double */
    out("    call _flr_print_double\n    addl $8,%%esp\n");
    out("    addl $8,%%esp\n");
    out("    popl %%ebx\n    popl %%edi\n    popl %%esi\n    leave\n    ret\n\n");

    /* _flr_print_double(lo:int, hi:int) — print a 64-bit double */
    /* Uses integer arithmetic to avoid printf; prints up to 6 decimal places */
    out("_flr_print_double:\n");
    out("    pushl %%ebp\n    movl %%esp,%%ebp\n");
    out("    pushl %%esi\n    pushl %%edi\n    pushl %%ebx\n");
    /* load double from args */
    out("    fldl 8(%%ebp)\n");
    /* check sign */
    out("    fxam\n    fnstsw %%ax\n");
    out("    testw $0x0200,%%ax\n    je .Lfpd_pos\n");
    out("    fchs\n");
    /* print minus */
    out("    movl $4,%%eax\n    movl $1,%%ebx\n    leal .Lflr_minus,%%ecx\n    movl $1,%%edx\n    int $0x80\n");
    out(".Lfpd_pos:\n");
    /* get integer part via fist */
    out("    fld %%st(0)\n");
    out("    subl $4,%%esp\n    fistpl (%%esp)\n    popl %%eax\n");
    out("    pushl %%eax\n    call _flr_print_int\n    addl $4,%%esp\n");
    /* print decimal point */
    out("    movl $4,%%eax\n    movl $1,%%ebx\n    leal .Lflr_dot,%%ecx\n    movl $1,%%edx\n    int $0x80\n");
    /* subtract integer part from float to get fraction */
    out("    fld %%st(0)\n");
    out("    subl $4,%%esp\n    fistpl (%%esp)\n    filds (%%esp)\n    addl $4,%%esp\n");
    out("    fsubrp\n");
    /* multiply fraction by 1000000, round to int, print 6 digits */
    out("    fldl .Lflr_1e6\n    fmulp\n");
    out("    subl $4,%%esp\n    fistpl (%%esp)\n    popl %%eax\n");
    out("    testl %%eax,%%eax\n    jge .Lfpd_fpos\n    negl %%eax\n");
    out(".Lfpd_fpos:\n");
    /* print 6-digit zero-padded fraction */
    out("    leal .Lflr_ibuf+12,%%edi\n    movb $10,(%%edi)\n");
    out("    movl $6,%%ecx\n");
    out("    decl %%edi\n");
    out(".Lfpd_fl:\n    movl $10,%%esi\n    xorl %%edx,%%edx\n    divl %%esi\n");
    out("    addb $48,%%dl\n    movb %%dl,(%%edi)\n    decl %%edi\n");
    out("    loop .Lfpd_fl\n");
    out("    incl %%edi\n");
    out("    movl $6,%%edx\n    movl %%edi,%%ecx\n");
    out("    movl $4,%%eax\n    movl $1,%%ebx\n    int $0x80\n");
    /* newline */
    out("    movl $4,%%eax\n    movl $1,%%ebx\n    leal .Lflr_nl,%%ecx\n    movl $1,%%edx\n    int $0x80\n");
    out("    popl %%ebx\n    popl %%edi\n    popl %%esi\n    leave\n    ret\n\n");


    /* strlen / str_len */
    out("_flr_strlen:\n    movl 4(%%esp),%%ecx\n    movl %%ecx,%%eax\n");
    out(".Lflrsl:\n    cmpb $0,(%%ecx)\n    je .Lflrsld\n    incl %%ecx\n    jmp .Lflrsl\n");
    out(".Lflrsld:\n    subl 4(%%esp),%%ecx\n    movl %%ecx,%%eax\n    ret\n\n");
    out("_flr_str_len:\n    jmp _flr_strlen\n\n");

    /* str_eq */
    out("_flr_str_eq:\n    pushl %%ebp\n    movl %%esp,%%ebp\n    pushl %%esi\n    pushl %%edi\n");
    out("    movl 8(%%ebp),%%esi\n    movl 12(%%ebp),%%edi\n");
    out(".Lfseq:\n    movzbl (%%esi),%%eax\n    movzbl (%%edi),%%ecx\n");
    out("    cmpl %%ecx,%%eax\n    jne .Lfseq_no\n    testl %%eax,%%eax\n    je .Lfseq_yes\n");
    out("    incl %%esi\n    incl %%edi\n    jmp .Lfseq\n");
    out(".Lfseq_yes:\n    movl $1,%%eax\n    jmp .Lfseq_ret\n");
    out(".Lfseq_no:\n    xorl %%eax,%%eax\n");
    out(".Lfseq_ret:\n    popl %%edi\n    popl %%esi\n    leave\n    ret\n\n");

    /* int_to_str */
    out("_flr_int_to_str:\n    pushl %%ebp\n    movl %%esp,%%ebp\n    pushl %%esi\n    pushl %%edi\n");
    out("    movl 8(%%ebp),%%eax\n    leal .Lflr_ibuf+11,%%edi\n    movb $0,(%%edi)\n    decl %%edi\n");
    out("    testl %%eax,%%eax\n    jge .Lfits_p\n    negl %%eax\n    movl $1,%%esi\n    jmp .Lfits_l\n");
    out(".Lfits_p:\n    xorl %%esi,%%esi\n");
    out(".Lfits_l:\n    movl $10,%%ecx\n    xorl %%edx,%%edx\n    divl %%ecx\n");
    out("    addb $48,%%dl\n    movb %%dl,(%%edi)\n    decl %%edi\n");
    out("    testl %%eax,%%eax\n    jne .Lfits_l\n");
    out("    testl %%esi,%%esi\n    je .Lfits_n\n    movb $45,(%%edi)\n    decl %%edi\n");
    out(".Lfits_n:\n    incl %%edi\n    movl %%edi,%%eax\n");
    out("    popl %%edi\n    popl %%esi\n    leave\n    ret\n\n");

    /* str_to_int */
    out("_flr_str_to_int:\n    pushl %%ebp\n    movl %%esp,%%ebp\n    pushl %%esi\n");
    out("    movl 8(%%ebp),%%esi\n    xorl %%eax,%%eax\n    xorl %%ecx,%%ecx\n");
    out("    cmpb $45,(%%esi)\n    jne .Lfsti_l\n    movl $1,%%ecx\n    incl %%esi\n");
    out(".Lfsti_l:\n    movzbl (%%esi),%%edx\n    cmpl $48,%%edx\n    jl .Lfsti_d\n");
    out("    cmpl $57,%%edx\n    jg .Lfsti_d\n    imull $10,%%eax\n    subl $48,%%edx\n    addl %%edx,%%eax\n");
    out("    incl %%esi\n    jmp .Lfsti_l\n");
    out(".Lfsti_d:\n    testl %%ecx,%%ecx\n    je .Lfsti_r\n    negl %%eax\n");
    out(".Lfsti_r:\n    popl %%esi\n    leave\n    ret\n\n");

    /* abs / min / max */
    out("_flr_abs:\n    movl 4(%%esp),%%eax\n    testl %%eax,%%eax\n    jge .Labs_ok\n    negl %%eax\n.Labs_ok:\n    ret\n\n");
    out("_flr_min:\n    movl 4(%%esp),%%eax\n    movl 8(%%esp),%%ecx\n    cmpl %%ecx,%%eax\n    jle .Lmin_ok\n    movl %%ecx,%%eax\n.Lmin_ok:\n    ret\n\n");
    out("_flr_max:\n    movl 4(%%esp),%%eax\n    movl 8(%%esp),%%ecx\n    cmpl %%ecx,%%eax\n    jge .Lmax_ok\n    movl %%ecx,%%eax\n.Lmax_ok:\n    ret\n\n");

    /* assert */
    out("_flr_assert:\n    pushl %%ebp\n    movl %%esp,%%ebp\n");
    out("    movl 8(%%ebp),%%eax\n    testl %%eax,%%eax\n    jne .Lassert_ok\n");
    out("    movl $4,%%eax\n    movl $2,%%ebx\n    leal .Lflr_assert_msg,%%ecx\n    movl $19,%%edx\n    int $0x80\n");
    out("    movl 12(%%ebp),%%esi\n    movl %%esi,%%ecx\n");
    out(".Lasssl:\n    cmpb $0,(%%ecx)\n    je .Lasssd\n    incl %%ecx\n    jmp .Lasssl\n");
    out(".Lasssd:\n    subl %%esi,%%ecx\n    movl %%ecx,%%edx\n    movl %%esi,%%ecx\n");
    out("    movl $4,%%eax\n    movl $2,%%ebx\n    int $0x80\n");
    out("    movl $4,%%eax\n    movl $2,%%ebx\n    leal .Lflr_nl,%%ecx\n    movl $1,%%edx\n    int $0x80\n");
    out("    movl $1,%%ebx\n    movl $1,%%eax\n    int $0x80\n");
    out(".Lassert_ok:\n    leave\n    ret\n\n");

    /* ── dynamic heap via sbrk (sys_brk = 45) ────────────────────
       _flr_alloc(size) -> ptr
       Calls brk(0) to get current break, aligns size to 4,
       then calls brk(break+size) to grow. Simple, no hard cap.     */
    out("_flr_alloc:\n");
    out("    pushl %%ebp\n    movl %%esp,%%ebp\n    pushl %%ebx\n");
    /* get current break */
    out("    movl $45,%%eax\n    xorl %%ebx,%%ebx\n    int $0x80\n");
    out("    movl %%eax,%%ecx\n"); /* ecx = current break = base of new block */
    /* align requested size to 4 */
    out("    movl 8(%%ebp),%%edx\n    addl $3,%%edx\n    andl $-4,%%edx\n");
    /* new break = old + size */
    out("    movl $45,%%eax\n    leal (%%ecx,%%edx),%%ebx\n    int $0x80\n");
    out("    movl %%ecx,%%eax\n"); /* return base of allocated block */
    out("    popl %%ebx\n    leave\n    ret\n\n");
    out("_flr_free:\n    ret\n\n");

    /* ── str_concat(a:str, b:str) -> str ─────────────────────────
       Allocates len(a)+len(b)+1 bytes, copies both strings in.     */
    out("_flr_str_concat:\n");
    out("    pushl %%ebp\n    movl %%esp,%%ebp\n");
    out("    pushl %%esi\n    pushl %%edi\n    pushl %%ebx\n");
    out("    movl 8(%%ebp),%%esi\n");   /* a */
    out("    movl 12(%%ebp),%%edi\n");  /* b */
    /* len(a) */
    out("    pushl %%esi\n    call _flr_strlen\n    addl $4,%%esp\n    movl %%eax,%%ebx\n");
    /* len(b) */
    out("    pushl %%edi\n    call _flr_strlen\n    addl $4,%%esp\n");
    out("    addl %%ebx,%%eax\n    incl %%eax\n"); /* total = lena+lenb+1 */
    /* alloc */
    out("    pushl %%eax\n    call _flr_alloc\n    addl $4,%%esp\n");
    out("    pushl %%eax\n"); /* save result ptr */
    /* copy a into result */
    out("    movl %%eax,%%edi\n");
    out(".Lsca_l:\n    movzbl (%%esi),%%ecx\n    testl %%ecx,%%ecx\n    je .Lsca_d\n");
    out("    movb %%cl,(%%edi)\n    incl %%esi\n    incl %%edi\n    jmp .Lsca_l\n");
    out(".Lsca_d:\n");
    /* copy b */
    out("    movl 12(%%ebp),%%esi\n");
    out(".Lscb_l:\n    movzbl (%%esi),%%ecx\n    movb %%cl,(%%edi)\n    testl %%ecx,%%ecx\n    je .Lscb_d\n");
    out("    incl %%esi\n    incl %%edi\n    jmp .Lscb_l\n");
    out(".Lscb_d:\n");
    out("    popl %%eax\n"); /* return result */
    out("    popl %%ebx\n    popl %%edi\n    popl %%esi\n    leave\n    ret\n\n");

    /* ── str_format(fmt:str, nargs:int, arg0..argN) -> str ───────
       Supports %d (int), %s (str), %% (literal %).
       Output buffer: alloc 512 bytes (simple, sufficient for typical use).  */
    out("_flr_str_format:\n");
    out("    pushl %%ebp\n    movl %%esp,%%ebp\n");
    out("    pushl %%esi\n    pushl %%edi\n    pushl %%ebx\n");
    /* alloc 512 byte output buffer */
    out("    pushl $512\n    call _flr_alloc\n    addl $4,%%esp\n");
    out("    movl %%eax,%%edi\n"); /* edi = output ptr */
    out("    pushl %%edi\n");      /* save result */
    out("    movl 8(%%ebp),%%esi\n");    /* fmt */
    out("    movl 12(%%ebp),%%ecx\n");   /* nargs */
    out("    leal 16(%%ebp),%%ebx\n");   /* &arg0 */
    out(".Lsfmt_l:\n");
    out("    movzbl (%%esi),%%eax\n    testl %%eax,%%eax\n    je .Lsfmt_end\n");
    out("    cmpl $37,%%eax\n    jne .Lsfmt_copy\n"); /* '%' */
    out("    incl %%esi\n    movzbl (%%esi),%%eax\n");
    out("    cmpl $37,%%eax\n    je .Lsfmt_copy\n");  /* %% -> % */
    out("    cmpl $100,%%eax\n    je .Lsfmt_d\n");    /* %d */
    out("    cmpl $115,%%eax\n    je .Lsfmt_s\n");    /* %s */
    /* unknown spec: just copy literal */
    out("    jmp .Lsfmt_copy\n");
    /* %d */
    out(".Lsfmt_d:\n");
    out("    incl %%esi\n");
    out("    testl %%ecx,%%ecx\n    je .Lsfmt_l\n");
    out("    pushl %%ecx\n    pushl %%esi\n    pushl %%edi\n    pushl %%ebx\n");
    out("    movl (%%ebx),%%eax\n    addl $4,%%ebx\n    decl %%ecx\n");
    out("    pushl %%eax\n    call _flr_int_to_str\n    addl $4,%%esp\n");
    /* copy int string into output */
    out("    movl %%eax,%%esi\n");
    out(".Lsfmt_dc:\n    movzbl (%%esi),%%eax\n    testl %%eax,%%eax\n    je .Lsfmt_dr\n");
    out("    movb %%al,(%%edi)\n    incl %%esi\n    incl %%edi\n    jmp .Lsfmt_dc\n");
    out(".Lsfmt_dr:\n");
    out("    popl %%ebx\n    popl %%edi\n    popl %%esi\n    popl %%ecx\n");
    out("    jmp .Lsfmt_l\n");
    /* %s */
    out(".Lsfmt_s:\n");
    out("    incl %%esi\n");
    out("    testl %%ecx,%%ecx\n    je .Lsfmt_l\n");
    out("    pushl %%ecx\n    pushl %%esi\n    pushl %%edi\n    pushl %%ebx\n");
    out("    movl (%%ebx),%%esi\n    addl $4,%%ebx\n    decl %%ecx\n");
    out(".Lsfmt_sc:\n    movzbl (%%esi),%%eax\n    testl %%eax,%%eax\n    je .Lsfmt_sr\n");
    out("    movb %%al,(%%edi)\n    incl %%esi\n    incl %%edi\n    jmp .Lsfmt_sc\n");
    out(".Lsfmt_sr:\n");
    out("    popl %%ebx\n    popl %%edi\n    popl %%esi\n    popl %%ecx\n");
    out("    jmp .Lsfmt_l\n");
    /* plain copy */
    out(".Lsfmt_copy:\n    movb %%al,(%%edi)\n    incl %%esi\n    incl %%edi\n    jmp .Lsfmt_l\n");
    out(".Lsfmt_end:\n    movb $0,(%%edi)\n");
    out("    popl %%eax\n"); /* return result ptr */
    out("    popl %%ebx\n    popl %%edi\n    popl %%esi\n    leave\n    ret\n\n");
}

/* ═══════════════════════════════════════════════════════════════════════
   CODE GENERATOR — x86-64 GAS AT&T, System V AMD64 ABI, syscall
   Result of integer/pointer/bool/long expressions: rax
   Result of float expressions:  xmm0 (32-bit lane)
   Result of double expressions: xmm0 (64-bit lane)
   ═══════════════════════════════════════════════════════════════════════ */

static void gen_expr64(Node *n);
static void gen_stmt64(Node *n);

static void gen_store64(Node *lv){
    switch(lv->kind){
    case N_IDENT:{
        Var *v=find_var(lv->name);
        if(!v)die("%s:%d: undefined variable '%s'",lv->file,lv->line,lv->name);
        if(v->type&&(v->type->kind==TY_FLOAT)){
            out("    movss %%xmm0,%d(%%rbp)\n",v->offset);
        } else if(v->type&&(v->type->kind==TY_DOUBLE)){
            out("    movsd %%xmm0,%d(%%rbp)\n",v->offset);
        } else {
            out("    movq %%rax,%d(%%rbp)\n",v->offset);
        }
        break;
    }
    case N_INDEX:
        out("    pushq %%rax\n");
        gen_expr64(lv->right); out("    pushq %%rax\n");
        gen_expr64(lv->left);  out("    popq %%rcx\n");
        /* array layout: [count:8][count:8][elem0][elem1]...  elements at base+16 */
        out("    leaq 16(%%rax,%%rcx,8),%%rdx\n");
        out("    popq %%rax\n    movq %%rax,(%%rdx)\n");
        break;
    case N_FIELD:{
        out("    pushq %%rax\n"); gen_expr64(lv->left);
        int found_off=-1;
        for(int si=0;si<nstructs&&found_off<0;si++)
            for(int fi=0;fi<structs[si].nfields;fi++)
                if(strcmp(structs[si].fields[fi].name,lv->sval)==0){found_off=fi*8;break;}
        if(found_off<0)die("unknown field '%s'",lv->sval);
        out("    popq %%rcx\n    movq %%rcx,%d(%%rax)\n",found_off);
        break;
    }
    default:die("gen_store64: not an lvalue");
    }
}

static void gen_intrinsic64(Node *n){
    const char *nm=n->callee;
    if(strcmp(nm,"__syscall")==0){
        int argc=n->args.n;
        if(argc<1)die("__syscall: need at least syscall number");
        /* evaluate args right-to-left, push, then load registers */
        for(int i=argc-1;i>=0;i--){gen_expr64(n->args.d[i]);out("    pushq %%rax\n");}
        out("    popq %%rax\n"); /* syscall number */
        const char *sc[6]={"rdi","rsi","rdx","r10","r8","r9"};
        for(int i=1;i<argc&&i<=6;i++) out("    popq %%%s\n",sc[i-1]);
        out("    syscall\n");
        return;
    }
    if(strcmp(nm,"__inb")==0){
        gen_expr64(n->args.d[0]);
        out("    movw %%ax,%%dx\n    xorl %%eax,%%eax\n    inb %%dx,%%al\n    movzbl %%al,%%eax\n");
        return;
    }
    if(strcmp(nm,"__outb")==0){
        gen_expr64(n->args.d[1]);out("    pushq %%rax\n");
        gen_expr64(n->args.d[0]);out("    movw %%ax,%%dx\n");
        out("    popq %%rax\n    outb %%al,%%dx\n    xorl %%eax,%%eax\n");
        return;
    }
    if(strcmp(nm,"__cli")==0){out("    cli\n    xorl %%eax,%%eax\n");return;}
    if(strcmp(nm,"__sti")==0){out("    sti\n    xorl %%eax,%%eax\n");return;}
    if(strcmp(nm,"__hlt")==0){out("    hlt\n    xorl %%eax,%%eax\n");return;}
    if(strcmp(nm,"__rdtsc")==0){out("    rdtsc\n    shlq $32,%%rdx\n    orq %%rdx,%%rax\n");return;}
    if(strcmp(nm,"__peek")==0){
        gen_expr64(n->args.d[0]);out("    movq (%%rax),%%rax\n");return;
    }
    if(strcmp(nm,"__poke")==0){
        gen_expr64(n->args.d[1]);out("    pushq %%rax\n");
        gen_expr64(n->args.d[0]);out("    popq %%rcx\n    movq %%rcx,(%%rax)\n    xorl %%eax,%%eax\n");return;
    }
    if(strcmp(nm,"__peekb")==0){
        gen_expr64(n->args.d[0]);out("    movzbl (%%rax),%%eax\n");return;
    }
    if(strcmp(nm,"__pokeb")==0){
        gen_expr64(n->args.d[1]);out("    pushq %%rax\n");
        gen_expr64(n->args.d[0]);out("    popq %%rcx\n    movb %%cl,(%%rax)\n    xorl %%eax,%%eax\n");return;
    }
    if(strcmp(nm,"__memset")==0){
        /* rdi=ptr, rsi=val, rdx=n */
        gen_expr64(n->args.d[2]);out("    pushq %%rax\n");
        gen_expr64(n->args.d[1]);out("    pushq %%rax\n");
        gen_expr64(n->args.d[0]);
        out("    movq %%rax,%%rdi\n");
        out("    popq %%rsi\n    popq %%rdx\n");
        out("    call _flr_memset\n");
        return;
    }
    if(strcmp(nm,"__memcpy")==0){
        gen_expr64(n->args.d[2]);out("    pushq %%rax\n");
        gen_expr64(n->args.d[1]);out("    pushq %%rax\n");
        gen_expr64(n->args.d[0]);
        out("    movq %%rax,%%rdi\n");
        out("    popq %%rsi\n    popq %%rdx\n");
        out("    call _flr_memcpy\n");
        return;
    }
    die("unknown intrinsic '%s'",nm);
}

static void gen_expr64(Node *n){
    if(!n){out("    xorl %%eax,%%eax\n");return;}
    switch(n->kind){
    case N_INTLIT:
        out("    movq $%lld,%%rax\n",n->ival); break;
    case N_LONGLIT:
        out("    movq $%lld,%%rax\n",n->ival); break;
    case N_BOOLLIT:
        out("    movq $%d,%%rax\n",n->bval?1:0); break;
    case N_FLOATLIT:{
        int idx=add_flit(n->dval,0);
        out("    movss .Lfl%d(%%rip),%%xmm0\n",idx); break;
    }
    case N_DOUBLELIT:{
        int idx=add_flit(n->dval,1);
        out("    movsd .Lfl%d(%%rip),%%xmm0\n",idx); break;
    }
    case N_STRLIT:
        out("    leaq .Lstr%d(%%rip),%%rax\n",add_strlit(n->sval)); break;
    case N_IDENT:{
        Var *v=find_var(n->name);
        if(!v)die("%s:%d: undefined variable '%s'",n->file,n->line,n->name);
        if(v->type&&v->type->kind==TY_FLOAT){
            out("    movss %d(%%rbp),%%xmm0\n",v->offset);
        } else if(v->type&&v->type->kind==TY_DOUBLE){
            out("    movsd %d(%%rbp),%%xmm0\n",v->offset);
        } else {
            out("    movq %d(%%rbp),%%rax\n",v->offset);
        }
        break;
    }
    case N_BINOP:{
        const char *op=n->op;
        int lf=(n->left&&n->left->etype&&(n->left->etype->kind==TY_FLOAT||n->left->etype->kind==TY_DOUBLE));
        int rf=(n->right&&n->right->etype&&(n->right->etype->kind==TY_FLOAT||n->right->etype->kind==TY_DOUBLE));
        int use_dbl=(n->etype&&n->etype->kind==TY_DOUBLE)||
                    (n->left&&n->left->etype&&n->left->etype->kind==TY_DOUBLE)||
                    (n->right&&n->right->etype&&n->right->etype->kind==TY_DOUBLE);

        /* str + str → str_concat */
        if(strcmp(op,"+")==0&&n->left->etype&&n->left->etype->kind==TY_STR){
            gen_expr64(n->right); out("    pushq %%rax\n");
            gen_expr64(n->left);
            out("    movq %%rax,%%rdi\n    popq %%rsi\n");
            out("    call _flr_str_concat\n");
            break;
        }

        /* float/double via SSE2 */
        if(lf||rf){
            int is_cmp=(!strcmp(op,"==")||!strcmp(op,"!=")||!strcmp(op,"<")||!strcmp(op,">")||!strcmp(op,"<=")||!strcmp(op,">="));
            /* evaluate rhs into xmm1, lhs into xmm0 */
            gen_expr64(n->right);
            if(use_dbl){
                if(rf&&n->right->etype&&n->right->etype->kind==TY_DOUBLE)
                    out("    movsd %%xmm0,%%xmm1\n");
                else if(rf)
                    out("    cvtss2sd %%xmm0,%%xmm1\n");
                else
                    out("    cvtsi2sdq %%rax,%%xmm1\n");
            } else {
                if(rf&&n->right->etype&&n->right->etype->kind==TY_FLOAT)
                    out("    movss %%xmm0,%%xmm1\n");
                else if(rf)
                    out("    cvtsd2ss %%xmm0,%%xmm1\n");
                else
                    out("    cvtsi2ssl %%eax,%%xmm1\n");
            }
            gen_expr64(n->left);
            if(use_dbl){
                if(lf&&n->left->etype&&n->left->etype->kind==TY_DOUBLE)
                    {}/* already in xmm0 */
                else if(lf)
                    out("    cvtss2sd %%xmm0,%%xmm0\n");
                else
                    out("    cvtsi2sdq %%rax,%%xmm0\n");
            } else {
                if(lf&&n->left->etype&&n->left->etype->kind==TY_FLOAT)
                    {}/* already in xmm0 */
                else if(lf)
                    out("    cvtsd2ss %%xmm0,%%xmm0\n");
                else
                    out("    cvtsi2ssl %%eax,%%xmm0\n");
            }
            if(is_cmp){
                if(use_dbl) out("    ucomisd %%xmm1,%%xmm0\n");
                else        out("    ucomiss %%xmm1,%%xmm0\n");
                if(!strcmp(op,"=="))     out("    sete %%al\n");
                else if(!strcmp(op,"!="))out("    setne %%al\n");
                else if(!strcmp(op,"<")) out("    setb %%al\n");
                else if(!strcmp(op,">")) out("    seta %%al\n");
                else if(!strcmp(op,"<="))out("    setbe %%al\n");
                else if(!strcmp(op,">="))out("    setae %%al\n");
                out("    movzbq %%al,%%rax\n");
            } else {
                if(use_dbl){
                    if(!strcmp(op,"+"))     out("    addsd %%xmm1,%%xmm0\n");
                    else if(!strcmp(op,"-"))out("    subsd %%xmm1,%%xmm0\n");  /* xmm0 = xmm0 - xmm1 */
                    else if(!strcmp(op,"*"))out("    mulsd %%xmm1,%%xmm0\n");
                    else if(!strcmp(op,"/"))out("    divsd %%xmm1,%%xmm0\n");
                    else die("unsupported double op '%s'",op);
                } else {
                    if(!strcmp(op,"+"))     out("    addss %%xmm1,%%xmm0\n");
                    else if(!strcmp(op,"-"))out("    subss %%xmm1,%%xmm0\n");
                    else if(!strcmp(op,"*"))out("    mulss %%xmm1,%%xmm0\n");
                    else if(!strcmp(op,"/"))out("    divss %%xmm1,%%xmm0\n");
                    else die("unsupported float op '%s'",op);
                }
            }
            break;
        }

        /* integer / long / bool — all native 64-bit */
        gen_expr64(n->right); out("    pushq %%rax\n");
        gen_expr64(n->left);  out("    popq %%rcx\n");
        if     (!strcmp(op,"+"))  out("    addq %%rcx,%%rax\n");
        else if(!strcmp(op,"-"))  out("    subq %%rcx,%%rax\n");
        else if(!strcmp(op,"*"))  out("    imulq %%rcx,%%rax\n");
        else if(!strcmp(op,"/"))  {out("    cqto\n");out("    idivq %%rcx\n");}
        else if(!strcmp(op,"%"))  {out("    cqto\n");out("    idivq %%rcx\n");out("    movq %%rdx,%%rax\n");}
        else if(!strcmp(op,"&"))  out("    andq %%rcx,%%rax\n");
        else if(!strcmp(op,"|"))  out("    orq  %%rcx,%%rax\n");
        else if(!strcmp(op,"^"))  out("    xorq %%rcx,%%rax\n");
        else if(!strcmp(op,"<<")) out("    shlq %%cl,%%rax\n");
        else if(!strcmp(op,">>")) out("    sarq %%cl,%%rax\n");
        else if(!strcmp(op,"==")) {out("    cmpq %%rcx,%%rax\n");out("    sete %%al\n");out("    movzbq %%al,%%rax\n");}
        else if(!strcmp(op,"!=")) {out("    cmpq %%rcx,%%rax\n");out("    setne %%al\n");out("    movzbq %%al,%%rax\n");}
        else if(!strcmp(op,"<"))  {out("    cmpq %%rcx,%%rax\n");out("    setl %%al\n");out("    movzbq %%al,%%rax\n");}
        else if(!strcmp(op,">"))  {out("    cmpq %%rcx,%%rax\n");out("    setg %%al\n");out("    movzbq %%al,%%rax\n");}
        else if(!strcmp(op,"<=")) {out("    cmpq %%rcx,%%rax\n");out("    setle %%al\n");out("    movzbq %%al,%%rax\n");}
        else if(!strcmp(op,">=")) {out("    cmpq %%rcx,%%rax\n");out("    setge %%al\n");out("    movzbq %%al,%%rax\n");}
        else if(!strcmp(op,"and")){
            out("    testq %%rax,%%rax\n");out("    setne %%al\n");
            out("    testq %%rcx,%%rcx\n");out("    setne %%cl\n");
            out("    andb %%cl,%%al\n");out("    movzbq %%al,%%rax\n");
        }
        else if(!strcmp(op,"or")){
            out("    orq %%rcx,%%rax\n");out("    setne %%al\n");out("    movzbq %%al,%%rax\n");
        }
        else die("unknown binop '%s'",op);
        break;
    }
    case N_UNOP:
        gen_expr64(n->left);
        if     (!strcmp(n->op,"-"))  out("    negq %%rax\n");
        else if(!strcmp(n->op,"~"))  out("    notq %%rax\n");
        else if(!strcmp(n->op,"not")){out("    testq %%rax,%%rax\n");out("    sete %%al\n");out("    movzbq %%al,%%rax\n");}
        break;
    case N_CALL:{
        if(is_intrinsic(n->callee)){gen_intrinsic64(n);break;}

        /* evaluate all args, push onto stack in reverse order,
           then load first 6 into registers (integer ABI) */
        int nargs=n->args.n;

        /* handle print specially */
        if(has_std&&strcmp(n->callee,"print")==0&&nargs==1){
            Node *arg=n->args.d[0];
            int is_str=(arg->etype&&arg->etype->kind==TY_STR);
            int is_flt=(arg->etype&&arg->etype->kind==TY_FLOAT);
            int is_dbl=(arg->etype&&arg->etype->kind==TY_DOUBLE);
            if(!is_flt&&!is_dbl&&arg->kind==N_IDENT){
                Var *v=find_var(arg->name);
                if(v&&v->type&&v->type->kind==TY_FLOAT)is_flt=1;
                if(v&&v->type&&v->type->kind==TY_DOUBLE)is_dbl=1;
            }
            gen_expr64(arg);
            if(is_flt){out("    call _flr_print_float\n");}
            else if(is_dbl){out("    call _flr_print_double\n");}
            else if(is_str){out("    movq %%rax,%%rdi\n    call _flr_print_str\n");}
            else           {out("    movq %%rax,%%rdi\n    call _flr_print_int\n");}
            break;
        }

        /* str_concat(a,b) */
        if(has_std&&strcmp(n->callee,"str_concat")==0&&nargs==2){
            gen_expr64(n->args.d[1]); out("    pushq %%rax\n");
            gen_expr64(n->args.d[0]);
            out("    movq %%rax,%%rdi\n    popq %%rsi\n");
            out("    call _flr_str_concat\n");
            break;
        }

        /* str_format(fmt, ...) */
        if(has_std&&strcmp(n->callee,"str_format")==0&&nargs>=1){
            /* push extra args right-to-left, then nargs-1 count, then fmt */
            for(int i=nargs-1;i>=1;i--){gen_expr64(n->args.d[i]);out("    pushq %%rax\n");}
            out("    pushq $%d\n",nargs-1);
            gen_expr64(n->args.d[0]); out("    pushq %%rax\n");
            /* load first 2 into rdi/rsi */
            out("    popq %%rdi\n    popq %%rsi\n");
            /* remaining on stack — _flr_str_format reads from rsp+16 */
            out("    call _flr_str_format\n");
            if(nargs>1) out("    addq $%d,%%rsp\n",(nargs-1)*8);
            break;
        }

        /* general call: push args r-to-l, load first 6 into regs */
        for(int i=nargs-1;i>=0;i--){
            gen_expr64(n->args.d[i]);
            /* float/double: save xmm0 to stack slot */
            int is_fp=(n->args.d[i]->etype&&
                      (n->args.d[i]->etype->kind==TY_FLOAT||
                       n->args.d[i]->etype->kind==TY_DOUBLE));
            if(is_fp) out("    subq $8,%%rsp\n    movsd %%xmm0,(%%rsp)\n");
            else       out("    pushq %%rax\n");
        }
        /* load up to 6 integer args into registers */
        int ireg=0;
        for(int i=0;i<nargs&&ireg<6;i++){
            int is_fp=(n->args.d[i]->etype&&
                      (n->args.d[i]->etype->kind==TY_FLOAT||
                       n->args.d[i]->etype->kind==TY_DOUBLE));
            if(!is_fp){
                out("    popq %%%s\n",argregs64[ireg++]);
            }
            /* fp args stay on stack for now (simple approach) */
        }
        out("    xorl %%eax,%%eax\n"); /* al=0: no xmm args (conservative) */
        out("    call %s\n",n->callee);
        /* clean up any remaining stack args */
        int rem=nargs-ireg;
        if(rem>0) out("    addq $%d,%%rsp\n",rem*8);
        break;
    }
    case N_INDEX:
        gen_expr64(n->right); out("    pushq %%rax\n");
        gen_expr64(n->left);  out("    popq %%rcx\n");
        out("    leaq 16(%%rax,%%rcx,8),%%rax\n");
        out("    movq (%%rax),%%rax\n"); break;
    case N_FIELD:{
        gen_expr64(n->left);
        int found_off=-1;
        for(int si=0;si<nstructs&&found_off<0;si++)
            for(int fi=0;fi<structs[si].nfields;fi++)
                if(strcmp(structs[si].fields[fi].name,n->sval)==0){found_off=fi*8;break;}
        if(found_off<0)die("%s:%d: unknown field '%s'",n->file,n->line,n->sval);
        out("    movq %d(%%rax),%%rax\n",found_off); break;
    }
    case N_ARRAYLIT:{
        int cnt=n->elems.n;
        long long alloc_sz=16+(long long)cnt*8;
        out("    movq $%lld,%%rdi\n",alloc_sz);
        out("    call _flr_alloc\n");
        out("    pushq %%rax\n");
        out("    movq $%d,(%%rax)\n    movq $%d,8(%%rax)\n",cnt,cnt);
        for(int i=0;i<cnt;i++){
            out("    movq (%%rsp),%%rdi\n");
            gen_expr64(n->elems.d[i]);
            out("    movq %%rax,%d(%%rdi)\n",(int)(16+i*8));
        }
        out("    popq %%rax\n"); break;
    }
    case N_TERNARY:{
        int lp=lbl_cnt++;
        gen_expr64(n->cond);
        out("    cmpq $0,%%rax\n    je .Ltern_else%d\n",lp);
        gen_expr64(n->left);
        out("    jmp .Ltern_end%d\n",lp);
        out(".Ltern_else%d:\n",lp);
        gen_expr64(n->right);
        out(".Ltern_end%d:\n",lp);
        break;
    }
    default:die("gen_expr64: unhandled kind %d",n->kind);
    }
}

static void gen_stmt64(Node *n){
    if(!n)return;
    switch(n->kind){
    case N_IMPORT:if(strcmp(n->import_path,"std")==0)has_std=1;break;
    case N_VARDECL:
    case N_LETDECL:
    case N_CONSTDECL:{
        TypeRef *vtype=n->typeref;
        if(!vtype&&n->left&&n->left->etype)vtype=n->left->etype;
        int off=alloc_var(n->name,vtype);
        if(n->left){
            gen_expr64(n->left);
            if(vtype&&vtype->kind==TY_FLOAT)
                out("    movss %%xmm0,%d(%%rbp)\n",off);
            else if(vtype&&vtype->kind==TY_DOUBLE)
                out("    movsd %%xmm0,%d(%%rbp)\n",off);
            else
                out("    movq %%rax,%d(%%rbp)\n",off);
        } else {
            out("    movq $0,%d(%%rbp)\n",off);
        }
        break;
    }
    case N_ASSIGN:{
        gen_expr64(n->right);
        if(strcmp(n->op,"=")==0){gen_store64(n->left);}
        else{
            /* compound: load lhs into rcx/xmm1, operate, store */
            int is_fp=(n->left->etype&&(n->left->etype->kind==TY_FLOAT||n->left->etype->kind==TY_DOUBLE));
            if(is_fp){
                /* save rhs xmm0 -> xmm1, load lhs -> xmm0, operate */
                int dbl=(n->left->etype->kind==TY_DOUBLE);
                if(dbl) out("    movsd %%xmm0,%%xmm1\n"); else out("    movss %%xmm0,%%xmm1\n");
                gen_expr64(n->left);
                if(!strcmp(n->op,"+=")){ if(dbl)out("    addsd %%xmm1,%%xmm0\n");else out("    addss %%xmm1,%%xmm0\n");}
                else if(!strcmp(n->op,"-=")){ if(dbl)out("    subsd %%xmm1,%%xmm0\n");else out("    subss %%xmm1,%%xmm0\n");}
                else if(!strcmp(n->op,"*=")){ if(dbl)out("    mulsd %%xmm1,%%xmm0\n");else out("    mulss %%xmm1,%%xmm0\n");}
                else if(!strcmp(n->op,"/=")){ if(dbl)out("    divsd %%xmm1,%%xmm0\n");else out("    divss %%xmm1,%%xmm0\n");}
                gen_store64(n->left);
            } else {
                out("    pushq %%rax\n"); gen_expr64(n->left); out("    popq %%rcx\n");
                if     (!strcmp(n->op,"+="))  out("    addq %%rcx,%%rax\n");
                else if(!strcmp(n->op,"-="))  out("    subq %%rcx,%%rax\n");
                else if(!strcmp(n->op,"*="))  out("    imulq %%rcx,%%rax\n");
                else if(!strcmp(n->op,"/="))  {out("    pushq %%rcx\n    cqto\n    idivq (%%rsp)\n    addq $8,%%rsp\n");}
                else if(!strcmp(n->op,"%="))  {out("    pushq %%rcx\n    cqto\n    idivq (%%rsp)\n    addq $8,%%rsp\n    movq %%rdx,%%rax\n");}
                else if(!strcmp(n->op,"&="))  out("    andq %%rcx,%%rax\n");
                else if(!strcmp(n->op,"|="))  out("    orq  %%rcx,%%rax\n");
                else if(!strcmp(n->op,"^="))  out("    xorq %%rcx,%%rax\n");
                else if(!strcmp(n->op,"<<=")) out("    shlq %%cl,%%rax\n");
                else if(!strcmp(n->op,">>=")) out("    sarq %%cl,%%rax\n");
                gen_store64(n->left);
            }
        }
        break;
    }
    case N_RETURN:
        if(n->left) gen_expr64(n->left);
        else out("    xorl %%eax,%%eax\n");
        out("    addq $%d,%%rsp\n",((frame_sz+15)&~15)); /* dealloc locals */
        out("    popq %%r15\n    popq %%r14\n    popq %%r13\n");
        out("    popq %%rbx\n");
        out("    leave\n    ret\n");
        break;
    case N_EXPRSTMT:{
        Node *e=n->left;
        /* special-case print at statement level (avoids duplicate dispatch) */
        if(has_std&&e->kind==N_CALL&&strcmp(e->callee,"print")==0&&e->args.n==1){
            Node *arg=e->args.d[0];
            int is_str=(arg->etype&&arg->etype->kind==TY_STR);
            int is_flt=(arg->etype&&arg->etype->kind==TY_FLOAT);
            int is_dbl=(arg->etype&&arg->etype->kind==TY_DOUBLE);
            if(!is_flt&&!is_dbl&&arg->kind==N_IDENT){
                Var *v=find_var(arg->name);
                if(v&&v->type&&v->type->kind==TY_FLOAT)is_flt=1;
                if(v&&v->type&&v->type->kind==TY_DOUBLE)is_dbl=1;
            }
            gen_expr64(arg);
            if(is_flt)      out("    call _flr_print_float\n");
            else if(is_dbl) out("    call _flr_print_double\n");
            else if(is_str) out("    movq %%rax,%%rdi\n    call _flr_print_str\n");
            else            out("    movq %%rax,%%rdi\n    call _flr_print_int\n");
            break;
        }
        gen_expr64(e); break;
    }
    case N_IF:{
        int lend=new_label(),lnext=new_label();
        gen_expr64(n->cond);out("    testq %%rax,%%rax\n    je .Lif%d\n",lnext);
        for(int i=0;i<n->body.n;i++)gen_stmt64(n->body.d[i]);
        out("    jmp .Lif%d\n.Lif%d:\n",lend,lnext);
        for(int ei=0;ei<n->elifs.n;ei++){
            ElifClause *ec=&n->elifs.d[ei];int ln2=new_label();
            gen_expr64(ec->cond);out("    testq %%rax,%%rax\n    je .Lif%d\n",ln2);
            for(int i=0;i<ec->body.n;i++)gen_stmt64(ec->body.d[i]);
            out("    jmp .Lif%d\n.Lif%d:\n",lend,ln2);
        }
        for(int i=0;i<n->else_body.n;i++)gen_stmt64(n->else_body.d[i]);
        out(".Lif%d:\n",lend); break;
    }
    case N_WHILE:{
        int ls=new_label(),le=new_label();
        char ob[64],oc[64];strcpy(ob,break_lbl);strcpy(oc,cont_lbl);
        snprintf(break_lbl,64,".Lwh%d",le);snprintf(cont_lbl,64,".Lwh%d",ls);
        out(".Lwh%d:\n",ls);gen_expr64(n->cond);out("    testq %%rax,%%rax\n    je .Lwh%d\n",le);
        for(int i=0;i<n->body.n;i++)gen_stmt64(n->body.d[i]);
        out("    jmp .Lwh%d\n.Lwh%d:\n",ls,le);
        strcpy(break_lbl,ob);strcpy(cont_lbl,oc); break;
    }
    case N_FOR:{
        int ls=new_label(),le=new_label(),lp=new_label();
        char ob[64],oc[64];strcpy(ob,break_lbl);strcpy(oc,cont_lbl);
        snprintf(break_lbl,64,".Lfor%d",le);snprintf(cont_lbl,64,".Lfor%d",lp);
        gen_stmt64(n->for_init);
        out(".Lfor%d:\n",ls);gen_expr64(n->cond);out("    testq %%rax,%%rax\n    je .Lfor%d\n",le);
        for(int i=0;i<n->body.n;i++)gen_stmt64(n->body.d[i]);
        out(".Lfor%d:\n",lp);gen_stmt64(n->for_post);
        out("    jmp .Lfor%d\n.Lfor%d:\n",ls,le);
        strcpy(break_lbl,ob);strcpy(cont_lbl,oc); break;
    }
    case N_BREAK:
        if(!break_lbl[0])die("%s:%d: break outside loop",n->file,n->line);
        out("    jmp %s\n",break_lbl); break;
    case N_CONTINUE:
        if(!cont_lbl[0])die("%s:%d: continue outside loop",n->file,n->line);
        out("    jmp %s\n",cont_lbl); break;
    case N_ASM:
        out("    %s\n",n->sval);break;
    case N_TYPEDEF_DECL:break;
    case N_DO:{
        int lp=lbl_cnt++;
        char ob[64],oc[64];strcpy(ob,break_lbl);strcpy(oc,cont_lbl);
        snprintf(break_lbl,64,".Ldobrk%d",lp);snprintf(cont_lbl,64,".Ldocnt%d",lp);
        out(".Ldoloop%d:\n",lp);
        for(int i=0;i<n->body.n;i++)gen_stmt64(n->body.d[i]);
        out("%s:\n",cont_lbl);gen_expr64(n->cond);
        out("    testq %%rax,%%rax\n    jne .Ldoloop%d\n",lp);
        out("%s:\n",break_lbl);
        strcpy(break_lbl,ob);strcpy(cont_lbl,oc);break;
    }
    case N_SWITCH:{
        int lp=lbl_cnt++;
        char ob[64];strcpy(ob,break_lbl);snprintf(break_lbl,64,".Lswbrk%d",lp);
        gen_expr64(n->cond);out("    pushq %%rax\n");
        for(int i=0;i<n->body.n;i++){
            Node *c=n->body.d[i];
            if(c->left){gen_expr64(c->left);out("    cmpq %%rax,(%%rsp)\n    je .Lcase%d_%d\n",lp,i);}
        }
        for(int i=0;i<n->body.n;i++){Node *c=n->body.d[i];if(!c->left){out("    jmp .Lcase%d_%d\n",lp,i);break;}}
        out("    jmp %s\n",break_lbl);
        for(int i=0;i<n->body.n;i++){
            Node *c=n->body.d[i];out(".Lcase%d_%d:\n",lp,i);
            for(int j=0;j<c->body.n;j++)gen_stmt64(c->body.d[j]);
        }
        out("    addq $8,%%rsp\n%s:\n",break_lbl);
        strcpy(break_lbl,ob);break;
    }
    case N_FUNC:gen_func64(n);break;
    default:die("gen_stmt64: unhandled kind %d",n->kind);
    }
}

static void gen_func64(Node *fn){
    nvars=0;frame_sz=0;
    memset(break_lbl,0,sizeof break_lbl);memset(cont_lbl,0,sizeof cont_lbl);

    /* parameters: first 6 ints via registers, rest on stack above rbp+16 */
    /* we store all params as locals for simplicity */
    /* allocate slots for params */
    for(int i=0;i<fn->params.n;i++){
        int off=alloc_var(fn->params.d[i].name,fn->params.d[i].type);
        (void)off;
    }

    /* generate body into temp buffer */
    size_t body_start=out_len;
    /* emit param loads INSIDE the body buffer (after prologue sets up frame) */
    /* we'll emit them first, then the actual stmts */
    for(int i=0;i<fn->params.n;i++){
        Var *v=&vars[i]; /* params are allocated first */
        if(i<6){
            int is_fp=(v->type&&(v->type->kind==TY_FLOAT||v->type->kind==TY_DOUBLE));
            if(is_fp){
                /* xmm args: xmm0..xmm7 */
                if(v->type->kind==TY_DOUBLE)
                    out("    movsd %%xmm%d,%d(%%rbp)\n",i,v->offset);
                else
                    out("    movss %%xmm%d,%d(%%rbp)\n",i,v->offset);
            } else {
                out("    movq %%%s,%d(%%rbp)\n",argregs64[i],v->offset);
            }
        } else {
            /* extra params are at rbp+16, rbp+24, ... */
            int stack_off=16+(i-6)*8;
            out("    movq %d(%%rbp),%%rax\n",stack_off);
            out("    movq %%rax,%d(%%rbp)\n",v->offset);
        }
    }
    for(int i=0;i<fn->body.n;i++)gen_stmt64(fn->body.d[i]);

    size_t body_len=out_len-body_start;
    char *body_asm=malloc(body_len+1);
    memcpy(body_asm,out_buf+body_start,body_len);body_asm[body_len]=0;
    out_len=body_start;out_buf[out_len]=0;

    int fsz=(frame_sz+15)&~15;

    out(".globl %s\n%s:\n",fn->fname,fn->fname);
    out("    pushq %%rbp\n    movq %%rsp,%%rbp\n");
    if(fsz>0) out("    subq $%d,%%rsp\n",fsz);
    /* save callee-saved registers */
    out("    pushq %%rbx\n");
    out("    pushq %%r13\n    pushq %%r14\n    pushq %%r15\n");

    /* paste body */
    if(out_len+body_len+1>out_cap){out_cap=out_cap*2+body_len+64;out_buf=realloc(out_buf,out_cap);}
    memcpy(out_buf+out_len,body_asm,body_len);out_len+=body_len;out_buf[out_len]=0;free(body_asm);

    if(strcmp(fn->fname,"main")==0){
        if(!freestanding){
            out("    popq %%r15\n    popq %%r14\n    popq %%r13\n    popq %%rbx\n");
            out("    xorl %%edi,%%edi\n    movl $60,%%eax\n    syscall\n");
        } else {
            out("    popq %%r15\n    popq %%r14\n    popq %%r13\n    popq %%rbx\n");
            out("    cli\n.Lhlt_loop:\n    hlt\n    jmp .Lhlt_loop\n");
        }
    } else {
        out("    popq %%r15\n    popq %%r14\n    popq %%r13\n    popq %%rbx\n");
        out("    leave\n    ret\n");
    }
    out("\n");
}

/* ── 64-bit runtime ──────────────────────────────────────────────── */
static void emit_runtime64(void){
    if(!has_std||freestanding)return;
    out("# ── Falcon Runtime 64-bit (inline) ───────────────────────────\n");
    /* ELF entry point */
    out(".globl _start\n_start:\n");
    out("    xorl %%ebp,%%ebp\n");
    out("    call main\n");
    out("    movl %%eax,%%edi\n");
    out("    movl $60,%%eax\n");
    out("    syscall\n\n");

    /* memset(ptr:rdi, val:rsi, n:rdx) */
    out("_flr_memset:\n");
    out("    movq %%rdi,%%rax\n    movq %%rdx,%%rcx\n    movb %%sil,%%al\n");
    out("    rep stosb\n    ret\n\n");

    /* memcpy(dst:rdi, src:rsi, n:rdx) */
    out("_flr_memcpy:\n");
    out("    movq %%rdx,%%rcx\n    rep movsb\n    ret\n\n");

    /* print_str(s:rdi) */
    out("_flr_print_str:\n");
    out("    pushq %%rbx\n");
    out("    movq %%rdi,%%rbx\n");
    /* strlen: walk rsi until NUL */
    out("    movq %%rdi,%%rsi\n");
    out(".Lfps64_l:\n    cmpb $0,(%%rsi)\n    je .Lfps64_d\n    incq %%rsi\n    jmp .Lfps64_l\n");
    out(".Lfps64_d:\n    subq %%rbx,%%rsi\n    movq %%rsi,%%rdx\n"); /* rdx = len */
    out("    movq $1,%%rax\n    movq $1,%%rdi\n    movq %%rbx,%%rsi\n    syscall\n");
    /* newline */
    out("    movq $1,%%rax\n    movq $1,%%rdi\n    leaq .Lflr_nl(%%rip),%%rsi\n    movq $1,%%rdx\n    syscall\n");
    out("    popq %%rbx\n    ret\n\n");

    /* print_int(n:rdi) */
    out("_flr_print_int:\n");
    out("    pushq %%rbx\n    pushq %%r12\n    pushq %%r13\n");
    out("    movq %%rdi,%%rax\n");
    out("    leaq .Lflr_ibuf+22(%%rip),%%r12\n    movb $10,(%%r12)\n    decq %%r12\n");
    out("    testq %%rax,%%rax\n    jge .Lfpi64_pos\n");
    out("    negq %%rax\n    movq $1,%%r13\n    jmp .Lfpi64_l\n");
    out(".Lfpi64_pos:\n    xorl %%r13d,%%r13d\n");
    out(".Lfpi64_l:\n    movq $10,%%rcx\n    xorl %%edx,%%edx\n    divq %%rcx\n");
    out("    addb $48,%%dl\n    movb %%dl,(%%r12)\n    decq %%r12\n");
    out("    testq %%rax,%%rax\n    jne .Lfpi64_l\n");
    out("    testq %%r13,%%r13\n    je .Lfpi64_nom\n");
    out("    movb $45,(%%r12)\n    decq %%r12\n");
    out(".Lfpi64_nom:\n    incq %%r12\n");
    out("    leaq .Lflr_ibuf+23(%%rip),%%rdx\n    subq %%r12,%%rdx\n");
    out("    movq $1,%%rax\n    movq $1,%%rdi\n    movq %%r12,%%rsi\n    syscall\n");
    /* newline */
    out("    movq $1,%%rax\n    movq $1,%%rdi\n    leaq .Lflr_nl(%%rip),%%rsi\n    movq $1,%%rdx\n    syscall\n");
    out("    popq %%r13\n    popq %%r12\n    popq %%rbx\n    ret\n\n");

    /* exit(code:rdi) */
    out("_flr_exit:\n    movl $60,%%eax\n    syscall\n\n");

    /* print_float(val in xmm0) — widen to double and print */
    out("_flr_print_float:\n");
    out("    cvtss2sd %%xmm0,%%xmm0\n");
    out("    call _flr_print_double\n    ret\n\n");

    /* print_double(val in xmm0) — integer digits + 6 decimal places */
    out("_flr_print_double:\n");
    out("    pushq %%rbx\n    pushq %%r12\n    pushq %%r13\n    pushq %%r14\n");
    /* check sign via movq xmm0->rax, test high bit */
    out("    movq %%xmm0,%%rax\n");
    out("    testq %%rax,%%rax\n    jns .Lfpd64_pos\n");
    /* negative: flip sign bit, print '-' */
    out("    movabsq $0x8000000000000000,%%rcx\n    xorq %%rcx,%%rax\n    movq %%rax,%%xmm0\n");
    out("    movq $1,%%rax\n    movq $1,%%rdi\n    leaq .Lflr_minus(%%rip),%%rsi\n    movq $1,%%rdx\n    syscall\n");
    out(".Lfpd64_pos:\n");
    /* extract integer part */
    out("    cvttsd2si %%xmm0,%%rdi\n    call _flr_print_int_nonl\n");
    /* decimal point */
    out("    movq $1,%%rax\n    movq $1,%%rdi\n    leaq .Lflr_dot(%%rip),%%rsi\n    movq $1,%%rdx\n    syscall\n");
    /* fraction: (val - floor(val)) * 1e6, print 6 digits */
    out("    cvttsd2si %%xmm0,%%rax\n    cvtsi2sdq %%rax,%%xmm1\n");
    out("    subsd %%xmm1,%%xmm0\n");
    out("    movsd .Lflr_1e6(%%rip),%%xmm1\n    mulsd %%xmm1,%%xmm0\n");
    out("    cvttsd2si %%xmm0,%%rax\n    testq %%rax,%%rax\n    jge .Lfpd64_fpos\n    negq %%rax\n");
    out(".Lfpd64_fpos:\n");
    /* print 6 zero-padded decimal digits */
    out("    leaq .Lflr_ibuf+12(%%rip),%%r12\n    movb $0,(%%r12)\n    decq %%r12\n");
    out("    movl $6,%%ecx\n");
    out(".Lfpd64_fl:\n    movq $10,%%r13\n    xorl %%edx,%%edx\n    divq %%r13\n");
    out("    addb $48,%%dl\n    movb %%dl,(%%r12)\n    decq %%r12\n    loop .Lfpd64_fl\n");
    out("    incq %%r12\n");
    out("    movq $1,%%rax\n    movq $1,%%rdi\n    movq %%r12,%%rsi\n    movq $6,%%rdx\n    syscall\n");
    /* newline */
    out("    movq $1,%%rax\n    movq $1,%%rdi\n    leaq .Lflr_nl(%%rip),%%rsi\n    movq $1,%%rdx\n    syscall\n");
    out("    popq %%r14\n    popq %%r13\n    popq %%r12\n    popq %%rbx\n    ret\n\n");

    /* print_int_nonl — print int in rdi without newline (helper for double) */
    out("_flr_print_int_nonl:\n");
    out("    pushq %%rbx\n    pushq %%r12\n    pushq %%r13\n");
    out("    movq %%rdi,%%rax\n");
    out("    leaq .Lflr_ibuf+22(%%rip),%%r12\n    movb $0,(%%r12)\n    decq %%r12\n");
    out("    testq %%rax,%%rax\n    jge .Lpni64_pos\n");
    out("    negq %%rax\n    movq $1,%%r13\n    jmp .Lpni64_l\n");
    out(".Lpni64_pos:\n    xorl %%r13d,%%r13d\n");
    out(".Lpni64_l:\n    movq $10,%%rcx\n    xorl %%edx,%%edx\n    divq %%rcx\n");
    out("    addb $48,%%dl\n    movb %%dl,(%%r12)\n    decq %%r12\n");
    out("    testq %%rax,%%rax\n    jne .Lpni64_l\n");
    out("    testq %%r13,%%r13\n    je .Lpni64_nom\n");
    out("    movb $45,(%%r12)\n    decq %%r12\n");
    out(".Lpni64_nom:\n    incq %%r12\n");
    out("    leaq .Lflr_ibuf+23(%%rip),%%rdx\n    subq %%r12,%%rdx\n");
    out("    movq $1,%%rax\n    movq $1,%%rdi\n    movq %%r12,%%rsi\n    syscall\n");
    out("    popq %%r13\n    popq %%r12\n    popq %%rbx\n    ret\n\n");

    /* strlen */
    out("_flr_strlen:\n");
    out("    movq %%rdi,%%rax\n");
    out(".Lflrsl64:\n    cmpb $0,(%%rax)\n    je .Lflrsld64\n    incq %%rax\n    jmp .Lflrsl64\n");
    out(".Lflrsld64:\n    subq %%rdi,%%rax\n    ret\n\n");
    out("_flr_str_len:\n    jmp _flr_strlen\n\n");

    /* str_eq(a:rdi, b:rsi) */
    out("_flr_str_eq:\n");
    out(".Lfseq64:\n    movzbl (%%rdi),%%eax\n    movzbl (%%rsi),%%ecx\n");
    out("    cmpl %%ecx,%%eax\n    jne .Lfseq64_no\n    testl %%eax,%%eax\n    je .Lfseq64_yes\n");
    out("    incq %%rdi\n    incq %%rsi\n    jmp .Lfseq64\n");
    out(".Lfseq64_yes:\n    movl $1,%%eax\n    ret\n");
    out(".Lfseq64_no:\n    xorl %%eax,%%eax\n    ret\n\n");

    /* int_to_str(n:rdi) -> rax (static buffer) */
    out("_flr_int_to_str:\n");
    out("    pushq %%rbx\n    pushq %%r12\n");
    out("    movq %%rdi,%%rax\n");
    out("    leaq .Lflr_ibuf+11(%%rip),%%r12\n    movb $0,(%%r12)\n    decq %%r12\n");
    out("    testq %%rax,%%rax\n    jge .Lfits64_p\n    negq %%rax\n    movq $1,%%rbx\n    jmp .Lfits64_l\n");
    out(".Lfits64_p:\n    xorl %%ebx,%%ebx\n");
    out(".Lfits64_l:\n    movq $10,%%rcx\n    xorl %%edx,%%edx\n    divq %%rcx\n");
    out("    addb $48,%%dl\n    movb %%dl,(%%r12)\n    decq %%r12\n");
    out("    testq %%rax,%%rax\n    jne .Lfits64_l\n");
    out("    testq %%rbx,%%rbx\n    je .Lfits64_n\n    movb $45,(%%r12)\n    decq %%r12\n");
    out(".Lfits64_n:\n    incq %%r12\n    movq %%r12,%%rax\n");
    out("    popq %%r12\n    popq %%rbx\n    ret\n\n");

    /* str_to_int(s:rdi) -> rax */
    out("_flr_str_to_int:\n");
    out("    xorl %%eax,%%eax\n    xorl %%ecx,%%ecx\n");
    out("    cmpb $45,(%%rdi)\n    jne .Lfsti64_l\n    movl $1,%%ecx\n    incq %%rdi\n");
    out(".Lfsti64_l:\n    movzbl (%%rdi),%%edx\n    cmpl $48,%%edx\n    jl .Lfsti64_d\n");
    out("    cmpl $57,%%edx\n    jg .Lfsti64_d\n    imulq $10,%%rax\n    subl $48,%%edx\n    addl %%edx,%%eax\n");
    out("    incq %%rdi\n    jmp .Lfsti64_l\n");
    out(".Lfsti64_d:\n    testl %%ecx,%%ecx\n    je .Lfsti64_r\n    negq %%rax\n");
    out(".Lfsti64_r:\n    ret\n\n");

    /* abs/min/max */
    out("_flr_abs:\n    movq %%rdi,%%rax\n    testq %%rax,%%rax\n    jge .Labs64_ok\n    negq %%rax\n.Labs64_ok:\n    ret\n\n");
    out("_flr_min:\n    movq %%rdi,%%rax\n    cmpq %%rsi,%%rax\n    jle .Lmin64_ok\n    movq %%rsi,%%rax\n.Lmin64_ok:\n    ret\n\n");
    out("_flr_max:\n    movq %%rdi,%%rax\n    cmpq %%rsi,%%rax\n    jge .Lmax64_ok\n    movq %%rsi,%%rax\n.Lmax64_ok:\n    ret\n\n");

    /* assert(cond:rdi, msg:rsi) */
    out("_flr_assert:\n");
    out("    testq %%rdi,%%rdi\n    jne .Lassert64_ok\n");
    out("    movq $1,%%rax\n    movq $2,%%rdi\n    leaq .Lflr_assert_msg(%%rip),%%rsi\n    movq $19,%%rdx\n    syscall\n");
    out("    pushq %%rsi\n    call _flr_strlen\n    popq %%rsi\n");
    out("    movq %%rax,%%rdx\n    movq $1,%%rax\n    movq $2,%%rdi\n    syscall\n");
    out("    movq $1,%%rax\n    movq $2,%%rdi\n    leaq .Lflr_nl(%%rip),%%rsi\n    movq $1,%%rdx\n    syscall\n");
    out("    movq $60,%%rax\n    movq $1,%%rdi\n    syscall\n");
    out(".Lassert64_ok:\n    ret\n\n");

    /* alloc(size:rdi) -> rax  — uses mmap(NULL,size,PROT_RW,MAP_ANON|MAP_PRIV,-1,0) */
    out("_flr_alloc:\n");
    out("    pushq %%rbx\n");
    out("    movq %%rdi,%%rsi\n");            /* length */
    out("    addq $7,%%rsi\n    andq $-8,%%rsi\n"); /* align to 8 */
    out("    xorl %%edi,%%edi\n");             /* addr = NULL */
    out("    movl $3,%%edx\n");               /* PROT_READ|PROT_WRITE */
    out("    movl $0x22,%%r10d\n");            /* MAP_PRIVATE|MAP_ANONYMOUS */
    out("    movq $-1,%%r8\n");               /* fd = -1 */
    out("    xorl %%r9d,%%r9d\n");             /* offset = 0 */
    out("    movl $9,%%eax\n");               /* sys_mmap */
    out("    syscall\n");
    out("    popq %%rbx\n    ret\n\n");
    out("_flr_free:\n    ret\n\n");           /* no-op bump */

    /* str_concat(a:rdi, b:rsi) -> rax */
    out("_flr_str_concat:\n");
    out("    pushq %%rbx\n    pushq %%r12\n    pushq %%r13\n");
    out("    movq %%rdi,%%r12\n    movq %%rsi,%%r13\n");
    out("    call _flr_strlen\n    movq %%rax,%%rbx\n"); /* len(a) */
    out("    movq %%r13,%%rdi\n    call _flr_strlen\n");  /* len(b) */
    out("    addq %%rbx,%%rax\n    incq %%rax\n");        /* total */
    out("    movq %%rax,%%rdi\n    call _flr_alloc\n");
    out("    pushq %%rax\n");
    /* copy a */
    out("    movq %%rax,%%rdi\n    movq %%r12,%%rsi\n");
    out(".Lsca64_l:\n    movzbl (%%rsi),%%ecx\n    testl %%ecx,%%ecx\n    je .Lsca64_d\n");
    out("    movb %%cl,(%%rdi)\n    incq %%rsi\n    incq %%rdi\n    jmp .Lsca64_l\n");
    out(".Lsca64_d:\n");
    /* copy b */
    out("    movq %%r13,%%rsi\n");
    out(".Lscb64_l:\n    movzbl (%%rsi),%%ecx\n    movb %%cl,(%%rdi)\n    testl %%ecx,%%ecx\n    je .Lscb64_d\n");
    out("    incq %%rsi\n    incq %%rdi\n    jmp .Lscb64_l\n");
    out(".Lscb64_d:\n");
    out("    popq %%rax\n");
    out("    popq %%r13\n    popq %%r12\n    popq %%rbx\n    ret\n\n");

    /* str_format(fmt:rdi, nargs:rsi, arg0...) -> rax */
    out("_flr_str_format:\n");
    out("    pushq %%rbx\n    pushq %%r12\n    pushq %%r13\n    pushq %%r14\n    pushq %%r15\n");
    out("    movq %%rdi,%%r12\n");   /* fmt */
    out("    movq %%rsi,%%r13\n");   /* nargs */
    out("    leaq 16(%%rsp),%%r14\n"); /* &arg0 on stack (after pushes) — caller put them above ret addr */
    /* alloc 512 output bytes */
    out("    movq $512,%%rdi\n    call _flr_alloc\n");
    out("    movq %%rax,%%r15\n    pushq %%rax\n"); /* save output ptr */
    out("    movq %%rax,%%rbx\n");  /* rbx = write ptr */
    out(".Lsfmt64_l:\n");
    out("    movzbl (%%r12),%%eax\n    testl %%eax,%%eax\n    je .Lsfmt64_end\n");
    out("    cmpl $37,%%eax\n    jne .Lsfmt64_copy\n");
    out("    incq %%r12\n    movzbl (%%r12),%%eax\n");
    out("    cmpl $37,%%eax\n    je .Lsfmt64_copy\n");
    out("    cmpl $100,%%eax\n    je .Lsfmt64_d\n");
    out("    cmpl $115,%%eax\n    je .Lsfmt64_s\n");
    out("    jmp .Lsfmt64_copy\n");
    /* %d */
    out(".Lsfmt64_d:\n    incq %%r12\n");
    out("    testq %%r13,%%r13\n    je .Lsfmt64_l\n");
    out("    pushq %%r12\n    pushq %%r13\n    pushq %%r14\n    pushq %%rbx\n");
    out("    movq (%%r14),%%rdi\n    addq $8,%%r14\n    decq %%r13\n");
    out("    call _flr_int_to_str\n    movq %%rax,%%rsi\n");
    out(".Lsfmt64_dc:\n    movzbl (%%rsi),%%eax\n    testl %%eax,%%eax\n    je .Lsfmt64_dr\n");
    out("    movb %%al,(%%rbx)\n    incq %%rsi\n    incq %%rbx\n    jmp .Lsfmt64_dc\n");
    out(".Lsfmt64_dr:\n    popq %%rbx\n    popq %%r14\n    popq %%r13\n    popq %%r12\n");
    out("    jmp .Lsfmt64_l\n");
    /* %s */
    out(".Lsfmt64_s:\n    incq %%r12\n");
    out("    testq %%r13,%%r13\n    je .Lsfmt64_l\n");
    out("    pushq %%r12\n    pushq %%r13\n    pushq %%r14\n    pushq %%rbx\n");
    out("    movq (%%r14),%%rsi\n    addq $8,%%r14\n    decq %%r13\n");
    out(".Lsfmt64_sc:\n    movzbl (%%rsi),%%eax\n    testl %%eax,%%eax\n    je .Lsfmt64_sr\n");
    out("    movb %%al,(%%rbx)\n    incq %%rsi\n    incq %%rbx\n    jmp .Lsfmt64_sc\n");
    out(".Lsfmt64_sr:\n    popq %%rbx\n    popq %%r14\n    popq %%r13\n    popq %%r12\n");
    out("    jmp .Lsfmt64_l\n");
    /* plain copy */
    out(".Lsfmt64_copy:\n    movb %%al,(%%rbx)\n    incq %%r12\n    incq %%rbx\n    jmp .Lsfmt64_l\n");
    out(".Lsfmt64_end:\n    movb $0,(%%rbx)\n");
    out("    popq %%rax\n"); /* return result ptr */
    out("    popq %%r15\n    popq %%r14\n    popq %%r13\n    popq %%r12\n    popq %%rbx\n    ret\n\n");
}

static void emit_strlit(const char *s){
    out("    .ascii \"");
    for(const char *p=s;*p;p++){
        if     (*p=='\n')out("\\n");
        else if(*p=='\t')out("\\t");
        else if(*p=='\\')out("\\\\");
        else if(*p=='"' )out("\\\"");
        else if(*p=='\0')out("\\0");
        else             out("%c",*p);
    }
    out("\\0\"\n");
}

static void emit_data(void){
    out(".section .data\n");
    if(has_std&&!freestanding){
        out(".Lflr_ibuf:\n    .space 32\n");
        out(".Lflr_nl:\n    .byte 10\n");
        out(".Lflr_dot:\n    .ascii \".\"\n");
        out(".Lflr_minus:\n    .ascii \"-\"\n");
        /* 1000000.0 as IEEE-754 double (little-endian 64-bit) */
        out(".Lflr_1e6:\n    .long 0\n    .long 1093567616\n");
        out(".Lflr_assert_msg:\n    .ascii \"assertion failed: \"\n");
    }
    out(".section .rodata\n");
    for(int i=0;i<nstr_lits;i++){out(".Lstr%d:\n",i);emit_strlit(str_lits[i]);}
    for(int i=0;i<nflits;i++){
        out(".Lfl%d:\n",i);
        if(flits[i].is_double){
            union{double d;unsigned u[2];}cv;cv.d=flits[i].val;
            out("    .long %u\n    .long %u\n",cv.u[0],cv.u[1]);
        } else {
            union{float f;unsigned u;}cv;cv.f=(float)flits[i].val;
            out("    .long %u\n",cv.u);
        }
    }
    out(".Lfl_zero:\n    .long 0\n    .long 0\n");
}

static void codegen(Node *prog){
    /* structs and has_std already registered by typecheck; re-scan for has_std */
    for(int i=0;i<prog->body.n;i++){
        Node *n=prog->body.d[i];
        if(n->kind==N_IMPORT&&n->import_path&&strcmp(n->import_path,"std")==0)has_std=1;
    }
    /* emit .extern declarations for all extern funcs */
    for(int i=0;i<prog->body.n;i++){
        Node *n=prog->body.d[i];
        if(n->kind==N_EXTERN) out(".extern %s\n",n->fname);
    }
    out(".section .text\n\n");
    if(arch==ARCH_X86_64){
        for(int i=0;i<prog->body.n;i++){
            Node *n=prog->body.d[i];if(n->kind==N_FUNC)gen_func64(n);
        }
        emit_runtime64();
    } else {
        for(int i=0;i<prog->body.n;i++){
            Node *n=prog->body.d[i];if(n->kind==N_FUNC)gen_func(n);
        }
        emit_runtime();
    }
    emit_data();
}

/* ═══════════════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════════════ */

static int valid_ext(const char *p){
    const char *e[]={".fl",".fal",".flc",".flsrc",".ofc",".ofcc",NULL};
    const char *d=strrchr(p,'.');if(!d)return 0;
    for(int i=0;e[i];i++)if(strcmp(d,e[i])==0)return 1;return 0;
}

int main(int argc,char **argv){
    const char *infile=NULL,*outfile=NULL;
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"--freestanding")==0)     freestanding=1;
        else if(strcmp(argv[i],"-o")==0&&i+1<argc)  outfile=argv[++i];
        else if(strncmp(argv[i],"-I",2)==0)          add_search(argv[i]+2);
        else if(strcmp(argv[i],"-arch")==0&&i+1<argc){
            i++;
            if(strcmp(argv[i],"x86-64-linux")==0)       arch=ARCH_X86_64;
            else if(strcmp(argv[i],"x86-32-linux")==0)  arch=ARCH_X86_32;
            else die("unknown arch '%s' (valid: x86-32-linux, x86-64-linux)",argv[i]);
        }
        else if(!infile)                             infile=argv[i];
    }
    if(!infile){
        fprintf(stderr,
            "falconc v4 — Falcon language compiler\n"
            "usage: ofcc <input.fl> [-o out.s] [-arch <target>] [--freestanding] [-I<path>...]\n\n"
            "targets:\n"
            "  -arch x86-32-linux   32-bit Linux, cdecl, int 0x80  (default)\n"
            "  -arch x86-64-linux   64-bit Linux, System V AMD64, syscall\n\n"
            "new in v4: -arch x86-64-linux\n"
            "new in v3: float, double, let, const\n"
            "new in v2: long, type checker, str_concat, str_format\n\n"
            "32-bit linking:\n"
            "  as --32 out.s -o out.o\n"
            "  ld -m elf_i386 flr.o out.o -o prog\n\n"
            "64-bit linking:\n"
            "  as out.s -o out.o\n"
            "  ld out.o -o prog\n"
        );
        return 1;
    }
    if(!valid_ext(infile))die("unrecognised extension (expected .fl .ofc .ofcc .fal .flc .flsrc)");

    char *indir=path_dir(infile);add_search(indir);free(indir);

    char *src=read_file(infile);
    if(!src)die("cannot open '%s'",infile);
    mark_imported(infile);
    tokenize(src,infile);
    free(src);
    expand_imports(0,infile);
    emit_tok(TT_EOF,"",0,infile);

    Node *prog=parse_program();
    typecheck(prog);   /* ← new: type-check before codegen */
    codegen(prog);

    if(outfile){
        FILE *f=fopen(outfile,"w");
        if(!f)die("cannot write '%s'",outfile);
        fwrite(out_buf,1,out_len,f);fclose(f);
        fprintf(stderr,"falconc: wrote %zu bytes to %s\n",out_len,outfile);
    }else{
        fwrite(out_buf,1,out_len,stdout);
    }
    return 0;
}
