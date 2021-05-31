/*
** $Id: llex.c,v 2.20.1.2 2009/11/23 14:58:22 roberto Exp $
** Lexical Analyzer
** See Copyright Notice in lua.h
*/


#include <ctype.h>
#include <locale.h>
#include <string.h>

#define llex_c
#define LUA_CORE

#include "lua.h"

#include "ldo.h"
#include "llex.h"
#include "lobject.h"
#include "lparser.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "lzio.h"



#define next(ls) (ls->current = zgetc(ls->z))




#define currIsNewline(ls)	(ls->current == '\n' || ls->current == '\r')


/* ORDER RESERVED */
const char *const luaX_tokens [] = {
    "and", "break", "do", "else", "elseif",
    "end", "false", "for", "function", "if",
    "in", "local", "nil", "not", "or", "repeat",
    "return", "then", "true", "until", "while",
    "..", "...", "==", ">=", "<=", "~=",
    "<number>", "<name>", "<string>", "<eof>",
    NULL
};


#define save_and_next(ls) (save(ls, ls->current), next(ls))

/* 将c存到ls->buff中 */
static void save (LexState *ls, int c) {
  Mbuffer *b = ls->buff;
  if (b->n + 1 > b->buffsize) {
    size_t newsize;
    if (b->buffsize >= MAX_SIZET/2)
      luaX_lexerror(ls, "lexical element too long", 0);
    newsize = b->buffsize * 2;
    luaZ_resizebuffer(ls->L, b, newsize);
  }
  b->buffer[b->n++] = cast(char, c);
}

/* 构建出关键字 */
void luaX_init (lua_State *L) {
  int i;
  for (i=0; i<NUM_RESERVED; i++) {
    TString *ts = luaS_new(L, luaX_tokens[i]);
    luaS_fix(ts);  /* reserved words are never collected */
    lua_assert(strlen(luaX_tokens[i])+1 <= TOKEN_LEN);
    ts->tsv.reserved = cast_byte(i+1);  /* reserved word */
  }
}


#define MAXSRC          80


const char *luaX_token2str (LexState *ls, int token) {
  if (token < FIRST_RESERVED) {
    lua_assert(token == cast(unsigned char, token));
    return (iscntrl(token)) ? luaO_pushfstring(ls->L, "char(%d)", token) :
                              luaO_pushfstring(ls->L, "%c", token);
  }
  else
    return luaX_tokens[token-FIRST_RESERVED];
}


static const char *txtToken (LexState *ls, int token) {
  switch (token) {
    case TK_NAME:
    case TK_STRING:
    case TK_NUMBER:
      save(ls, '\0');
      return luaZ_buffer(ls->buff);
    default:
      return luaX_token2str(ls, token);
  }
}


void luaX_lexerror (LexState *ls, const char *msg, int token) {
  char buff[MAXSRC];
  luaO_chunkid(buff, getstr(ls->source), MAXSRC);
  msg = luaO_pushfstring(ls->L, "%s:%d: %s", buff, ls->linenumber, msg);
  if (token)
    luaO_pushfstring(ls->L, "%s near " LUA_QS, msg, txtToken(ls, token));
  luaD_throw(ls->L, LUA_ERRSYNTAX);
}


void luaX_syntaxerror (LexState *ls, const char *msg) {
  luaX_lexerror(ls, msg, ls->t.token);
}


TString *luaX_newstring (LexState *ls, const char *str, size_t l) {
  lua_State *L = ls->L;
  TString *ts = luaS_newlstr(L, str, l);
  TValue *o = luaH_setstr(L, ls->fs->h, ts);  /* entry for `str' */
  if (ttisnil(o)) {
  	/* 将 str作为key用起来，这样就不会将其gc了,bool作为values简单，也可以用lua_number这种不用gc的都行 */
    setbvalue(o, 1);  /* make sure `str' will not be collected */
    luaC_checkGC(L);
  }
  return ts;
}

/* 读到单个换行符\n，\r，再多读一个字符，看是否是双字符的换行\n\r,\r\n */
static void inclinenumber (LexState *ls) {
  int old = ls->current;
  lua_assert(currIsNewline(ls));	/* 当前是换行符 */

  /* 往后再读一个字符，看看不是不是\n\r和\r\n这种双字符的换行符（仅算一次换行） */
  next(ls);  /* skip `\n' or `\r' */
  if (currIsNewline(ls) && ls->current != old)
    next(ls);  /* skip `\n\r' or `\r\n' */
  
  if (++ls->linenumber >= MAX_INT)
    luaX_syntaxerror(ls, "chunk has too many lines");
}

/* 初始化LexState，初始化ls->buf,从ZIO读取第一个字符 */
void luaX_setinput (lua_State *L, LexState *ls, ZIO *z, TString *source) {
  ls->decpoint = '.';
  ls->L = L;
  ls->lookahead.token = TK_EOS;  /* no look-ahead token */
  ls->z = z;
  ls->fs = NULL;	/* 尚未开始编译函数，这里置NULL */
  ls->linenumber = 1;	/* 当前在第一行，亲 */
  ls->lastline = 1;
  ls->source = source;
  /* 申请属于ls->buff的私有buff */
  luaZ_resizebuffer(ls->L, ls->buff, LUA_MINBUFFER);  /* initialize buffer */
  
  next(ls);  /* read first char */
}



/*
** =======================================================
** LEXICAL ANALYZER
** =======================================================
*/


/* ls->current和set字符集中的某个字符匹配吗 */
static int check_next (LexState *ls, const char *set) {
  if (!strchr(set, ls->current))	
    return 0;
  save_and_next(ls);
  return 1;
}

/* 将ls->buff中的from字符替换层to字符 */
static void buffreplace (LexState *ls, char from, char to) {
  size_t n = luaZ_bufflen(ls->buff);
  char *p = luaZ_buffer(ls->buff);
  while (n--)
    if (p[n] == from) p[n] = to;
}

/* 数字字符串转换为数字时失败，尝试更换成本地区的数字字符小数点后再次尝试转换 */
static void trydecpoint (LexState *ls, SemInfo *seminfo) {
  /* format error: try to update decimal point separator */
  struct lconv *cv = localeconv();
  char old = ls->decpoint;
  ls->decpoint = (cv ? cv->decimal_point[0] : '.');
  buffreplace(ls, old, ls->decpoint);  /* try updated decimal separator */
  if (!luaO_str2d(luaZ_buffer(ls->buff), &seminfo->r)) {
    /* format error with correct decimal point: no more options */
    buffreplace(ls, ls->decpoint, '.');  /* undo change (for error message) */
    luaX_lexerror(ls, "malformed number", TK_NUMBER);
  }
}


/* LUA_NUMBER 
** .123这种  123或者科学计数法形式(1.99714E+13)的数字
*/
static void read_numeral (LexState *ls, SemInfo *seminfo) {
  lua_assert(isdigit(ls->current));	/* 属于 [0,9] 集合？*/
  /* 读取第一部分 1.99714 */
  do {
    save_and_next(ls);
  } while (isdigit(ls->current) || ls->current == '.');

  /* 读取第二部分 E+ */
  if (check_next(ls, "Ee"))  /* `E'? */
    check_next(ls, "+-");  /* optional exponent sign */
  /* 读取第三部分 13 */
  while (isalnum(ls->current) || ls->current == '_')	/* 这里的_不太明白其含义 */
    save_and_next(ls);
  /* 主动补\0,关闭字符串 */
  save(ls, '\0');
  /* 不同国家不同的小数点 */
  buffreplace(ls, '.', ls->decpoint);  /* follow locale for decimal point */
  if (!luaO_str2d(luaZ_buffer(ls->buff), &seminfo->r))  /* format error? */
    trydecpoint(ls, seminfo); /* try to update decimal point separator */
}

/* 尝试读取多行字符串的开头或结尾 --[{=}[ 或者 ]{=}] */
static int skip_sep (LexState *ls) {
  int count = 0;
  int s = ls->current;
  lua_assert(s == '[' || s == ']');
  save_and_next(ls);
  while (ls->current == '=') {
    save_and_next(ls);
    count++;
  }
  return (ls->current == s) ? count : (-count) - 1;
}

/* 读取一个长字符串和"结尾控制符"(长注释或String的token)(同时更新linenumber) */
static void read_long_string (LexState *ls, SemInfo *seminfo, int sep) {
  int cont = 0;
  (void)(cont);  /* avoid warnings when `cont' is not used */
  save_and_next(ls);  		/* skip 2nd `[' */
  
  if (currIsNewline(ls))  	/* string starts with a newline? */
    inclinenumber(ls);  /* skip it */
  
  for (;;) {
    switch (ls->current) {
      case EOZ:
        luaX_lexerror(ls, (seminfo) ? "unfinished long string" :
                                   "unfinished long comment", TK_EOS);
        break;  /* to avoid warnings */
#if defined(LUA_COMPAT_LSTR)
      case '[': {
        if (skip_sep(ls) == sep) {
          save_and_next(ls);  /* skip 2nd `[' */
          cont++;
#if LUA_COMPAT_LSTR == 1
          if (sep == 0)
            luaX_lexerror(ls, "nesting of [[...]] is deprecated", '[');
#endif
        }
        break;
      }
#endif
      case ']': {
        if (skip_sep(ls) == sep) {
          save_and_next(ls);  /* skip 2nd `]' */
#if defined(LUA_COMPAT_LSTR) && LUA_COMPAT_LSTR == 2
          cont--;
          if (sep == 0 && cont >= 0) break;
#endif
          goto endloop;
        }
        break;
      }
      case '\n':
      case '\r': {
        save(ls, '\n');	
        inclinenumber(ls);	/* 注释的业务中也不能忘了linenumber */
        if (!seminfo) luaZ_resetbuffer(ls->buff);  /* avoid wasting space */
        break;
      }
      default: {
        if (seminfo) save_and_next(ls);
        else next(ls);
      }
    }
  } endloop:

  /* seminfo为null是注释逻辑，注释不用管里面的具体String，否则是一个正常的String的token，需将String保存 */
  if (seminfo) {
  	/* 2+sep:[{=}[ */
    seminfo->ts = luaX_newstring(ls, luaZ_buffer(ls->buff) + (2 + sep),	
                                     luaZ_bufflen(ls->buff) - 2*(2 + sep));
  }
}

/* 读一个"字符串"或'字符串'格式的字符串 */
static void read_string (LexState *ls, int del, SemInfo *seminfo) {
  save_and_next(ls);
  while (ls->current != del) {
    switch (ls->current) {
      case EOZ:
        luaX_lexerror(ls, "unfinished string", TK_EOS);
        continue;  /* to avoid warnings */
      case '\n':
      case '\r':
        luaX_lexerror(ls, "unfinished string", TK_STRING);
        continue;  /* to avoid warnings */
      case '\\': {	/* 可能的转移序列 */
        int c;
        next(ls);  /* do not save the `\' */
        switch (ls->current) {
          case 'a': c = '\a'; break;
          case 'b': c = '\b'; break;
          case 'f': c = '\f'; break;
          case 'n': c = '\n'; break;
          case 'r': c = '\r'; break;
          case 't': c = '\t'; break;
          case 'v': c = '\v'; break;

		  /* 本身就是一个换行符 */
          case '\n':  /* go through */
          case '\r': save(ls, '\n'); inclinenumber(ls); continue;
		  
          case EOZ: continue;  /* will raise an error next loop */
          default: {
            if (!isdigit(ls->current))
              save_and_next(ls);  /* handles \\, \", \', and \? */
            else {  /* \xxx */
              int i = 0;
              c = 0;
              do {
                c = 10*c + (ls->current-'0');
                next(ls);
              } while (++i<3 && isdigit(ls->current));
              if (c > UCHAR_MAX)
                luaX_lexerror(ls, "escape sequence too large", TK_STRING);
              save(ls, c);
            }
            continue;
          }
        }
        save(ls, c);
        next(ls);
        continue;
      }
      default:
        save_and_next(ls);
    }
  }
  save_and_next(ls);  /* skip delimiter(分隔符) */
  seminfo->ts = luaX_newstring(ls, luaZ_buffer(ls->buff) + 1,
                                   luaZ_bufflen(ls->buff) - 2);
}

/* 从读取下一个字符token 
** 从ZIO读取下一个token到seminfo,并返回TokenType(中途可能用到lx->buff)
*/
static int llex (LexState *ls, SemInfo *seminfo) {
  luaZ_resetbuffer(ls->buff);
  for (;;) {
    switch (ls->current) {
      case '\n':
      case '\r': {
        inclinenumber(ls);
        continue;
      }
      case '-': {
        next(ls);
		/* 单独的 - */
        if (ls->current != '-') return '-';
        /* else is a comment */
        next(ls);	/* 单行OR多行注释? */
        if (ls->current == '[') {
          int sep = skip_sep(ls);
          luaZ_resetbuffer(ls->buff);  /* `skip_sep' may dirty the buffer */
          if (sep >= 0) {
            read_long_string(ls, NULL, sep);  /* long comment */
            luaZ_resetbuffer(ls->buff);
            continue;
          }
        }
        /* else short comment */
        while (!currIsNewline(ls) && ls->current != EOZ)
          next(ls);
        continue;
      }
      case '[': {	/* 长字符串: [{=}[ String ]{=}] */
        int sep = skip_sep(ls);
        if (sep >= 0) {	/* 多行字符串开头 [{=}[ */
          read_long_string(ls, seminfo, sep);
          return TK_STRING;
        }
        else if (sep == -1) return '[';	/* [,others这样的开头 */
        else luaX_lexerror(ls, "invalid long string delimiter", TK_STRING);		/* [={=},others这样的开头 */
      }
      case '=': {
        next(ls);
        if (ls->current != '=') return '=';
        else { next(ls); return TK_EQ; }
      }
      case '<': {
        next(ls);
        if (ls->current != '=') return '<';
        else { next(ls); return TK_LE; }
      }
      case '>': {
        next(ls);
        if (ls->current != '=') return '>';
        else { next(ls); return TK_GE; }
      }
      case '~': {
        next(ls);
        if (ls->current != '=') return '~';
        else { next(ls); return TK_NE; }
      }
	  /* 短字符串 */
	  case '"':
      case '\'': {
        read_string(ls, ls->current, seminfo);
        return TK_STRING;
      }
	  /* 看这个符号的解析，是一个深度优先的解析示例 */
      case '.': {
        save_and_next(ls);
        if (check_next(ls, ".")) {
          if (check_next(ls, "."))
            return TK_DOTS;   /* ... */
          else return TK_CONCAT;   /* .. */
        }
        else if (!isdigit(ls->current)) return '.';
        else {
          read_numeral(ls, seminfo);
          return TK_NUMBER;
        }
      }
      case EOZ: {
        return TK_EOS;
      }
      default: {
        if (isspace(ls->current)) {
          lua_assert(!currIsNewline(ls));	/* 换行符在前面就被解析掉了，这里不能再是换行符了,否则就重复了 */
          next(ls);
          continue;
        }
        else if (isdigit(ls->current)) {
          read_numeral(ls, seminfo);
          return TK_NUMBER;
        }
        else if (isalpha(ls->current) || ls->current == '_') {	/* 标识符 */
          /* identifier or reserved word */
          TString *ts;
          do {
            save_and_next(ls);
          } while (isalnum(ls->current) || ls->current == '_');	/* 这里和上面的有一点差别 */
		  
          ts = luaX_newstring(ls, luaZ_buffer(ls->buff),
                                  luaZ_bufflen(ls->buff));
		  /* 关键或保留字符串 */
          if (ts->tsv.reserved > 0)  /* reserved word? */
            return ts->tsv.reserved - 1 + FIRST_RESERVED;
          else {
            seminfo->ts = ts;
            return TK_NAME;
          }
        }
        else {
          int c = ls->current;
          next(ls);
          return c;  /* single-char tokens (+ - / ...) */
        }
      }
    }
  }
}

/* 读下一个token */
void luaX_next (LexState *ls) {
  ls->lastline = ls->linenumber;

  /* 已有一个准备好的lookhead'token,则取出来用 */
  if (ls->lookahead.token != TK_EOS) {  /* is there a look-ahead token? */
    ls->t = ls->lookahead;  /* use this one */
    ls->lookahead.token = TK_EOS;  /* and discharge it */
  }
  else /* 读取一个token */
    ls->t.token = llex(ls, &ls->t.seminfo);  /* read next token */
}

/* 当前lookhead'token已过期，重新准备lookhead'token */
void luaX_lookahead (LexState *ls) {
  lua_assert(ls->lookahead.token == TK_EOS);
  ls->lookahead.token = llex(ls, &ls->lookahead.seminfo);
}

