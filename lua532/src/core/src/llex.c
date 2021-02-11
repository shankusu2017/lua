/*
** $Id: llex.c,v 2.95 2015/11/19 19:16:22 roberto Exp $
** Lexical Analyzer
** See Copyright Notice in lua.h
*/

#define llex_c
#define LUA_CORE

#include "lprefix.h"


#include <locale.h>
#include <string.h>

#include "lua.h"

#include "lctype.h"
#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "llex.h"
#include "lobject.h"
#include "lparser.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "lzio.h"

static l_noret lexerror (LexState *ls, const char *msg, int token);

#define currIsNewline(ls) (ls->current == '\n' || ls->current == '\r')


/* ORDER RESERVED 
 * 保留字(关键字)
*/
static const char *const luaX_tokens [] = {
  /* part.1:字符串型(即满足组成identifier的条件，又是关键字)关键字 */
  "and", "break",
  "do", "else", "elseif","end", "false", " for", "function",
  "goto", "if", "in", "local", "nil", "not", "or", "repeat",
  "return", "then", "true", "until", "while",

  /* part.2:符号型(不满足组成identifier的条件)关键字 */
    "//", "..", "...", "==", ">=", "<=", "~=",
    "<<", ">>",
    "::", "<eof>",
    "<number>", "<integer>", "<name>", "<string>"
};

/*
 ** 先将ls中的current存档到Mbuffer中
 ** 再从ls的ZIO中读一个字符到current上
*/
#define save_and_next(ls) (save(ls, ls->current), next(ls))


/* 
** 从ZIO本身的buf中读下一个字节，存到LexState的current中
** 若本身的buf空了，则调用reader读一片mem到本省的buf，且返回一个字节
*/
#define next(ls) (ls->current = zgetc(ls->z))

/*
** 将c存到LexState中的buffer中
** c一般是上一步next()中的LexState(ls)中的current
*/
static void save (LexState *ls, int c) {
  Mbuffer *b = ls->buff;
  if (luaZ_bufflen(b) + 1 > luaZ_sizebuffer(b)) { /* buff长度不够了，需要扩容 */
    size_t newsize;
    if (luaZ_sizebuffer(b) >= MAX_SIZE/2)
      lexerror(ls, "lexical element too long", 0);
    newsize = luaZ_sizebuffer(b) * 2;
    luaZ_resizebuffer(ls->L, b, newsize);
  }
  b->buffer[luaZ_bufflen(b)++] = cast(char, c); /* TODOTHINK:这里降级了哦！*/
}

/* 
 * 注册一些字符串常量的语法关键词 
 * 下面luaX_next函数中会用到这里注册的关键字的token
*/
void luaX_init (lua_State *L) {
  int i;
  TString *e = luaS_newliteral(L, LUA_ENV);  /* create env name */
  luaC_fix(L, obj2gco(e));  /* never collect this name */
  for (i=0; i<NUM_RESERVED; i++) {
    TString *ts = luaS_new(L, luaX_tokens[i]);
    luaC_fix(L, obj2gco(ts));  /* reserved words are never collected */
  
    /* 保留字在llex函数中返回token时会加上 FIRST_RESERVED */
    ts->extra = cast_byte(i+1);  /* reserved word */
  }
}

/* token对应的字符串char 
** token < 257,普通的ascii-char?
** token >= 257,那么必然为一个RESERVED
*/
const char *luaX_token2str (LexState *ls, int token) {
  if (token < FIRST_RESERVED) {  /* single-byte symbols? */
    lua_assert(token == cast_uchar(token)); /* 这里保证为整数 */
    return luaO_pushfstring(ls->L, "'%c'", token);
  } else {
    const char *s = luaX_tokens[token - FIRST_RESERVED];
    if (token < TK_EOS)  /* fixed format (symbols and reserved words)? */
      return luaO_pushfstring(ls->L, "'%s'", s);
    else  /* names, strings, and numerals */
      return s;
  }
}

/* 返回token对应的char *p
 * 可能位于Mbuffer中，也可能位于token *p[]常量中
*/
static const char *txtToken (LexState *ls, int token) {
  switch (token) {
    case TK_NAME:
    case TK_STRING:
    case TK_FLT:
    case TK_INT:
      save(ls, '\0'); /* 这里补一个\0,形成C风格的字符串 */
      return luaO_pushfstring(ls->L, "'%s'", luaZ_buffer(ls->buff));
    default:
      return luaX_token2str(ls, token);
  }
}


static l_noret lexerror (LexState *ls, const char *msg, int token) {
  msg = luaG_addinfo(ls->L, msg, ls->source, ls->linenumber);
  if (token)
    luaO_pushfstring(ls->L, "%s near %s", msg, txtToken(ls, token));
  luaD_throw(ls->L, LUA_ERRSYNTAX);
}

/* 报句法错误 */
l_noret luaX_syntaxerror (LexState *ls, const char *msg) {
  lexerror(ls, msg, ls->t.token);
}


/*
** creates a new string and anchors(锚点) it in scanner's table so that
** it will not be collected until the end of the compilation
** (by that time it should be anchored somewhere)
**
** 先构建对应的TString,再尝试往ls->h中存档
*/
TString *luaX_newstring (LexState *ls, const char *str, size_t l) {
  lua_State *L = ls->L;
  TValue *o;  /* entry for 'str' */
  
  TString *ts = luaS_newlstr(L, str, l);  /* create new string */
  setsvalue2s(L, L->top++, ts);  /* temporarily anchor it in stack */
  
  o = luaH_set(L, ls->h, L->top - 1);
  if (ttisnil(o)) {  /* not in use yet? */
    /* boolean value does not need GC barrier;
       table has no metatable, so it does not need to invalidate cache */
    setbvalue(o, 1);  /* t[string] = true */
    luaC_checkGC(L);
  } else {                        /* string already present */
    /* 通过keyfromval宏嘟嘟转转又回到了ts中的TValue* */
    ts = tsvalue(keyfromval(o));  /* re-use value previously stored */
  }
  L->top--;  /* remove string from stack */
  return ts;
}


/*
** increment line number and skips newline sequence (any of
** \n, \r, \n\r, or \r\n)
*/
static void inclinenumber (LexState *ls) {
  int old = ls->current;
  lua_assert(currIsNewline(ls));
  next(ls);  /* skip '\n' or '\r' */
  if (currIsNewline(ls) && ls->current != old)
    next(ls);  /* skip '\n\r' or '\r\n' */
  /* 这个可能性不大 */
  if (++ls->linenumber >= MAX_INT)
    lexerror(ls, "chunk has too many lines", 0);
}

/*
** 初始化LexState
*/
void luaX_setinput (lua_State *L, LexState *ls, ZIO *z, TString *source, int firstchar)
{
  ls->t.token   = 0;
  ls->decpoint  = '.';
  ls->L         = L;

  /* 这里赋一个firstchar对后面的逻辑判断相当有用，不然到处都要来个if not read firstchar的判断
  ** 所以这里干脆读一个char，省了后面一大堆的判断了
  */
  ls->current     = firstchar;
  
  ls->lookahead.token = TK_EOS;  /* no look-ahead token */
  ls->z           = z;

  /* 刚开始时不对应任何的FuncState          */ 
  ls->fs          = NULL;
  
  ls->linenumber  = 1;
  ls->lastline    = 1;
  ls->source      = source;
  
  /* "_ENV"
  ** 这个概念是不是在哪里看到过呢?
  */
  ls->envn        = luaS_newliteral(L, LUA_ENV);      /* get env name */
  
  luaZ_resizebuffer(ls->L, ls->buff, LUA_MINBUFFER);  /* initialize buffer */
}



/*
** =======================================================
** LEXICAL ANALYZER
** =======================================================
**
** 如果当前的字符current是期望的字符，则放弃本字符，继续读下一个字符，返回1
** 若不是，直接返回0
*/

static int check_next1 (LexState *ls, int c) {
  if (ls->current == c) {
    next(ls);
    return 1;
  } else {
    return 0;
  }
}


/*
** Check whether current char is in set 'set' (with two chars) and
** saves it
** 接下来的字符是否是2个字符中的一个eg: c='x' or 'X' (16进制的0x,0X格式头)
** 是期望的字符之一，则吃掉该字符，继续读下一个字符，返回1
** 不是，则直接返回0
*/
static int check_next2 (LexState *ls, const char *set) {
  lua_assert(set[2] == '\0');
  if (ls->current == set[0] || ls->current == set[1]) {
    save_and_next(ls);
    return 1;
  } else {
    return 0;
  }
}


/*
** change all characters 'from' in buffer to 'to'
**
** 将buffer中所有的from字符替换成to
*/
static void buffreplace (LexState *ls, char from, char to) {
  if (from != to) {
    size_t n = luaZ_bufflen(ls->buff);
    char *p = luaZ_buffer(ls->buff);
    while (n--)
      if (p[n] == from)
        p[n] = to;
  }
}


/*
** in case of format error, try to change decimal（10进制） point separator to
** the one defined in the current locale and check again
** 刷新下小数点的字符，再尝试将字符串转换为数字
** 失败：用'.'更新小数点分隔符，报错
** 成功：将转换成功的数值存储在o中
*/
static void trydecpoint (LexState *ls, TValue *o) {
  char old     = ls->decpoint;
  ls->decpoint = lua_getlocaledecpoint();
  buffreplace(ls, old, ls->decpoint);  /* try new decimal separator */
  
  
  if (luaO_str2num(luaZ_buffer(ls->buff), o) == 0) {  /* 转换失败了 */
    /* format error with correct decimal point: no more options */
    buffreplace(ls, ls->decpoint, '.');  /* undo change (for error message) */
    lexerror(ls, "malformed number", TK_FLT);
  }
}


/* LUA_NUMBER 
** 
** this function is quite liberal（相当自由） in what it accepts, as 'luaO_str2num'
** will reject ill-formed (错误格式)numerals.
*/
static int read_numeral (LexState *ls, SemInfo *seminfo) {
  TValue obj;
  const char *expo = "Ee";  /* 科学计数法 1914 = 1.914E3或1.914e3, 0.001914=1.914E-3 */
  int first = ls->current;
  lua_assert(lisdigit(ls->current));
  save_and_next(ls);

  /* 科学计数法中用到了Ee,这里Pp和16进制有啥关系呢？ */
  if (first == '0' && check_next2(ls, "xX"))  /* hexadecimal? */
    expo = "Pp";
  for (;;) {
    if (check_next2(ls, expo))  /* exponent part? */
      check_next2(ls, "-+");    /* optional exponent sign */
    if (lisxdigit(ls->current)) /* 0-9,A-F,a-f */
      save_and_next(ls);
    else if (ls->current == '.')
      save_and_next(ls);
    else
     break;
  }
  save(ls, '\0'); /* 添加结束符，以便"调用C库" */

  buffreplace(ls, '.', ls->decpoint);  /* follow locale for decimal point */
  if (luaO_str2num(luaZ_buffer(ls->buff), &obj) == 0)  /* format error? */
    trydecpoint(ls, &obj); /* try to update decimal point separator */

  if (ttisinteger(&obj)) {
    seminfo->i = ivalue(&obj);
    return TK_INT;
  } else {
    lua_assert(ttisfloat(&obj));
    seminfo->r = fltvalue(&obj);
    return TK_FLT;
  }
}


/*
** skip a sequence '[=*[' or ']=*]'; 
** if sequence is well formed, return its number of '='s; 
** otherwise, return a negative number (-1 iff there
** are no '='s after initial bracket)
**
** 读取[======*]中的N个'='字符，最后停留在=的下一个字符上
** 返回值:
**   [0,+N], 形成了[=*[或]=*]的匹配模式
**   -1:没有形成匹配模式，且[或]后面没有一个'=',eg：[abcdxxxxxxx
**   -N:没有形成匹配模式，且[或]后面有M个'=',          eg:[=*abcdxxxxxxxx
** 匹配模式:
**    [[这种两个'[',或]]这种两个']'
*/
static int skip_sep (LexState *ls) {
  int count = 0;
  int s   = ls->current;  /* 存下来以便下面判断是否形成了[[或]]对 */
  lua_assert(s == '[' || s == ']');
  save_and_next(ls);

  /* 读入中间可能的N个'=' */
  while (ls->current == '=') {
    save_and_next(ls);
    count++;
  }
  
  /* 形成[*[匹配则返回读入的'='字符个数，反之返回某个负数 */
  return (ls->current == s) ? count : (-count) - 1;  /* 这里-1,以便和0分开 */
}

/* 
 ** 读取一段[=*[long_string]=*]中的字符串 
 ** 被包裹的数据一律按照字符串字面值来处理，换行符进行了必要的处理外(忽略刚开始的换行符)
 **
*/
static void read_long_string (LexState *ls, SemInfo *seminfo, int sep) {
  int line = ls->linenumber;  /* initial line (for error message) */
  
  save_and_next(ls);  /* skip 2nd '[' */

  /* 忽略掉一开始的换行符
  ** 后面有什么字面字符都照收不误,包含换行 
  */
  if (currIsNewline(ls))  /* string starts with a newline? */
    inclinenumber(ls);    /* skip it */
  
  for (;;) {
    switch (ls->current) {
      case EOZ: {  /* error */
        const char *what = (seminfo ? "string" : "comment");
        const char *msg = luaO_pushfstring(ls->L,
                     "unfinished long %s (starting at line %d)", what, line);
        lexerror(ls, msg, TK_EOS);
        break;  /* to avoid warnings */
      }
      case ']': {
        if (skip_sep(ls) == sep) {  /* 看到了么，这里一直读一直读，直到读到匹配的字符串为准 */
          save_and_next(ls);  /* skip 2nd ']' */
          goto endloop;
        }
        break;
      }
      case '\n': case '\r': {
        save(ls, '\n');
        inclinenumber(ls);
        if (!seminfo)
          luaZ_resetbuffer(ls->buff);  /* avoid wasting space */
        break;
      }
      default: {
        if (seminfo)
          save_and_next(ls);
        else
          next(ls);
      }
    }
  } endloop:
  if (seminfo)
    seminfo->ts = luaX_newstring(ls, luaZ_buffer(ls->buff) + (2 + sep),
                                     luaZ_bufflen(ls->buff) - 2*(2 + sep));
}


static void esccheck (LexState *ls, int c, const char *msg) {
  if (!c) {
    if (ls->current != EOZ)
      save_and_next(ls);  /* add current to buffer for error message */
    lexerror(ls, msg, TK_STRING);
  }
}

/* 
** C语言参考手册 2.7 数值转义码 *
** 分两种情况：八进制（Octal）和十六进制(hex)
**
** 八进制：(Lua中没有8进制)
**        \[xdigit,[xdigit,[xdigit,]]],八进制的数量最多达到3个，且遇到第一个不是八进制的字符便停止
** 十六进制：
**        \x[xdigit*],十六进制的字符数量可以是任意的，，且遇到第一个不是十六进制的字符便停止
**
**
** Lua中的十进制
**      语法规则和C语言的八进制一样，只是改C语言的八进制为Lua的十进制，即Lua中是没有八进制的
**      Lua5.3.2参考手册 对应描述是3.1节
** Lua中的十六进制
**      \x[xdigit,xdigit],和C不同，Lua中必须是2个十六进制的xdigit
*/

/* 处理十进制(decimal)字符序列 */
static int readdecesc (LexState *ls)
{
  int i;
  int r = 0;  /* result accumulator */
  for (i = 0; i < 3 && lisdigit(ls->current); i++) {  /* read up to 3 digits */
    r = 10*r + ls->current - '0';
    save_and_next(ls);
  }
  esccheck(ls, r <= UCHAR_MAX, "decimal escape too large");

  /* 移除相应的buf.char,因为表示的值用r表示了
  ** eg: 输入字符串为：'\101hello'
  **     \101被转移成了'e'（ascii值等于101）,移除MBuffer中的'101',改为返回r,'\'字符由外围函数处理
  */
  luaZ_buffremove(ls->buff, i);  /* remove read digits from buffer */

  return r;
}

/* 
** 尝试读取一个十六进制的xdigit字符，并返回其数值 
*/
static int gethexa (LexState *ls) {
  /*
  ** 先将ls中的current存档到Mbuffer中
  ** 在从ls的ZIO中读一个字符到current上
  */
  save_and_next(ls);
  esccheck (ls, lisxdigit(ls->current), "hexadecimal digit expected");
  return luaO_hexavalue(ls->current); /* 'b'-> 11 */
}
/* 读一个十六进制序列
 ** 输入字符串：\xffabc
 ** 此时ls->current=x
 */
static int readhexaesc (LexState *ls) {
  int r = gethexa(ls);
  r = (r << 4) + gethexa(ls);
  /* 调用下面的预计后Mbuffer中的字符为\f(假设输入字符串是\xffabc),这和一般的\t是一样的，方便外壳函数统一处理 */
  luaZ_buffremove(ls->buff, 2);  /* remove saved chars from buffer */
  return r;
}

/*
** Lua中的UTF-8编码的unicode字符，格式是：\u{XXX},XXX:用16进制表示的字符编号
** 读取了XXX的值，Mbuffer中移除了（\u{xx}） 字符序列
*/
static unsigned long readutf8esc (LexState *ls) {
  unsigned long r;
  int i = 4;  /* chars to be removed: '\', 'u', '{', and first digit */
  save_and_next(ls);  /* skip 'u' */
  /* buff=\u, current={ */

  esccheck(ls, ls->current == '{', "missing '{'");
  r = gethexa(ls);  /* must have at least one digit */
  /* buff="\u{" current = f, (假设输入字符是"\u{f5}")*/

  while ((save_and_next(ls), lisxdigit(ls->current))) {
    i++;
    r = (r << 4) + luaO_hexavalue(ls->current);
    esccheck(ls, r <= 0x10FFFF, "UTF-8 value too large");
  }
  esccheck(ls, ls->current == '}', "missing '}'");
  next(ls);  /* skip '}' */
  luaZ_buffremove(ls->buff, i);  /* remove saved chars from buffer */
  return r;
}


static void utf8esc (LexState *ls) {
  char buff[UTF8BUFFSZ];
  int n = luaO_utf8esc(buff, readutf8esc(ls));
  for (; n > 0; n--)  /* add 'buff' to string */
    save(ls, buff[UTF8BUFFSZ - n]);
}

/*
** delimiter(定界符),字符串开头结尾的{",'}。
** eg:"我是字符串"，'我是字符串'
** 这里可以看到Lua的字符串必须是两个""匹配或两个''匹配。
*/
static void read_string (LexState *ls, int del, SemInfo *seminfo) {
  save_and_next(ls);  /* keep delimiter (for error messages) */
  while (ls->current != del) {  /* 读取两种格式字符串中的字符串,eg:"字符串"，'字符串' */
    switch (ls->current) {
      case EOZ: 
        lexerror(ls, "unfinished string", TK_EOS);
        break;  /* to avoid warnings */
      case '\n': case '\r':
        lexerror(ls, "unfinished string", TK_STRING);
        break;  /* to avoid warnings */
      case '\\': {  /* escape sequences */
        int c;  /* final character to be saved */
        save_and_next(ls);  /* keep '\\' for error messages */

        switch (ls->current) {
          case 'a': c = '\a'; goto read_save;
          case 'b': c = '\b'; goto read_save;
          case 'f': c = '\f'; goto read_save;
          case 'n': c = '\n'; goto read_save;
          case 'r': c = '\r'; goto read_save;
          case 't': c = '\t'; goto read_save;
          case 'v': c = '\v'; goto read_save;
          case '\\': case '\"': case '\'':
              c = ls->current; goto read_save;
          case 'x': c = readhexaesc(ls); goto read_save;

          case 'u': utf8esc(ls);  goto no_save;
          case '\n': case '\r':
            inclinenumber(ls); c = '\n'; goto only_save;
          case EOZ: goto no_save;  /* will raise an error next loop */
          case 'z': {  /* zap following span of spaces: 转义串 '\z' 会忽略其后的一系列空白符，包括换行 */
            luaZ_buffremove(ls->buff, 1);  /* remove '\\' */
            next(ls);  /* skip the 'z' */
            while (lisspace(ls->current)) {
              if (currIsNewline(ls))
               inclinenumber(ls);
              else
               next(ls);
            }
            goto no_save;
          }
          default: {
            esccheck(ls, lisdigit(ls->current), "invalid escape sequence");
            c = readdecesc(ls);  /* digital escape '\ddd' d:是个10进制的值，且最多3个 */
            goto only_save;
          }
        }

    
       read_save: /* 这里没有break */
         next(ls);
         /* go through */
       only_save:
         /* remove '\\' */
         luaZ_buffremove(ls->buff, 1);  
     
     /* 这里存下转义后的c */
         save(ls, c);
         /* go through */
       no_save:
         break;
      }
      default:
        save_and_next(ls);
    }
  }

  /* 存下结束的",'。再读入下一个字符 */
  save_and_next(ls);  /* skip delimiter(定界符) */
  
  /* 
  ** eg: "我是字符串",
  ** 形成的内容中，不包括开头和结尾的[{","}, {','}],所以下面要+1,-2
  */
  seminfo->ts = luaX_newstring(ls, luaZ_buffer(ls->buff) + 1,
                                   luaZ_bufflen(ls->buff) - 2);
}


static int llex (LexState *ls, SemInfo *seminfo) {
  luaZ_resetbuffer(ls->buff);
  for (;;) {
    switch (ls->current) {
      /* 看到了么：Lua直接忽略了换行和空格! */
      case '\n': case '\r': {  /* line breaks */
        inclinenumber(ls);
        break;
      }
      case ' ': case '\f': case '\t': case '\v': {  /* spaces */
        next(ls);
        break;
      }
      
      /* 
      **这是一个重点的分支，一定要搞懂
      **可以画逻辑运行图（条件分支图，状态图）
      */
      case '-': {  /* '-' or '--' (comment) */
        next(ls);
        if (ls->current != '-')
          return '-';
    
        /* else is a comment 
        ** 到这里就是 -- 开头的lua注释头格式了
        */
        next(ls);
    
        if (ls->current == '[') {   /* 以"--[": 开头, long comment? */
          int sep = skip_sep(ls); 
          luaZ_resetbuffer(ls->buff);  /* 'skip_sep' may dirty the buffer */
          if (sep >= 0) { /* 以 --[=*[ 开头,则必须以对应的]=*] 结尾 */
            read_long_string(ls, NULL, sep);  /* skip long comment */
            luaZ_resetbuffer(ls->buff);       /* previous call may dirty the buff. */
            break;
          } else {
            ;/* --[abcded[ 这种情况也算是一行普通的注释了 */
          }
        }
        /* else short comment */
        while (!currIsNewline(ls) && ls->current != EOZ)
          next(ls);  /* skip until end of line (or end of file) */
        break;
      }
      case '[': {  /* long string or simply '[' */
        int sep = skip_sep(ls);
        if (sep >= 0) { /* [=* 这种模式开头 */
          read_long_string(ls, seminfo, sep);
          return TK_STRING;
        } else if (sep != -1) {  /* '[=*...',少了第二个[ missing second bracket */
          lexerror(ls, "invalid long string delimiter", TK_STRING);
        } else { /* -1==sep:则是下面的模式 */
          return '['; /* 否则就是 [abcdxxxxx,这种模式了 */
        }
      }
      case '=': {
        next(ls);
        if (check_next1(ls, '='))
          return TK_EQ;
        else
          return '=';
      }
      case '<': {
        next(ls);
        if (check_next1(ls, '='))
          return TK_LE;
        else if (check_next1(ls, '<'))
          return TK_SHL;
        else
          return '<';
      }
      case '>': {
        next(ls);
        if (check_next1(ls, '=')) /* >=:形成了一个独立的token */
          return TK_GE;
        else if (check_next1(ls, '>'))
          return TK_SHR;
        else 
          return '>';
      }
      case '/': {
        next(ls);
        if (check_next1(ls, '/'))
          return TK_IDIV;
        else
          return '/';
      }
      case '~': {
        next(ls);
        if (check_next1(ls, '='))
          return TK_NE;
        else
          return '~';
      }
      case ':': {
        next(ls);
        if (check_next1(ls, ':'))
          return TK_DBCOLON;
        else
          return ':';
      }
      case '"': case '\'': {  /* short literal strings */
        read_string(ls, ls->current, seminfo);
        return TK_STRING;
      }
      case '.': {             /* '.', '..', '...', or number */
        save_and_next(ls);
        if (check_next1(ls, '.')) {   /* 这里是典型的贪心算法哦 */
          if (check_next1(ls, '.'))
            return TK_DOTS;   /* '...' */
          else
            return TK_CONCAT; /* '..' */
        } else if (!lisdigit(ls->current)) {
          return '.';
        } else {
          return read_numeral(ls, seminfo);
        }
      }
      case '0': case '1': case '2': case '3': case '4':
      case '5': case '6': case '7': case '8': case '9': {
        return read_numeral(ls, seminfo);
      }
      case EOZ: {
        return TK_EOS;
      }
      default: {
        if (lislalpha(ls->current)) {  /* identifier or reserved word?，这里可以看出Lua的标识符的规则哦 */
          TString *ts;  
          do {
            save_and_next(ls);
          } while (lislalnum(ls->current));
          ts = luaX_newstring(ls, luaZ_buffer(ls->buff),
                                  luaZ_bufflen(ls->buff));
          seminfo->ts = ts;
          /*
          ** 这里看出来了没，reserved这个字段就是专门留给lex系统用的，
          ** 不然其它系统标记这个域，下面的代码岂不是乱了套...
           */
          if (isreserved(ts)) {  /* reserved word? */
            return ts->extra - 1 + FIRST_RESERVED;
          } else {
            return TK_NAME;
          }
        } else {  /* single-char tokens (+ - / ...) */
          int c = ls->current;
          next(ls);
          return c;
        }
      }
    }
  }
}

void luaX_next (LexState *ls) {
  ls->lastline = ls->linenumber;
  /* 有前看符号就直接取前看符号的值 */
  if (ls->lookahead.token != TK_EOS) {  /* is there a look-ahead token? */
    ls->t = ls->lookahead;              /* use this one */
    ls->lookahead.token = TK_EOS;       /* and discharge it */
  } else {
    ls->t.token = llex(ls, &ls->t.seminfo);  /* read next token */
  }
}

int luaX_lookahead (LexState *ls) {
  lua_assert(ls->lookahead.token == TK_EOS);
  ls->lookahead.token = llex(ls, &ls->lookahead.seminfo);
  return ls->lookahead.token;
}

