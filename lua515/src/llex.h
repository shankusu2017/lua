/*
** $Id: llex.h,v 1.58.1.1 2007/12/27 13:02:25 roberto Exp $
** Lexical Analyzer
** See Copyright Notice in lua.h
*/

#ifndef llex_h
#define llex_h

#include "lobject.h"
#include "lzio.h"


#define FIRST_RESERVED	257

/* maximum length of a reserved word */
#define TOKEN_LEN	(sizeof("function")/sizeof(char))


/*
* WARNING: if you change the order of this enumeration,
* grep "ORDER RESERVED"
*/
enum RESERVED {
  /* terminal symbols denoted(表示、标识) by reserved words */
  TK_AND = FIRST_RESERVED, TK_BREAK,
  TK_DO, TK_ELSE, TK_ELSEIF, TK_END, TK_FALSE, TK_FOR, TK_FUNCTION,
  TK_IF, TK_IN, TK_LOCAL, TK_NIL, TK_NOT, TK_OR, TK_REPEAT,
  TK_RETURN, TK_THEN, TK_TRUE, TK_UNTIL, TK_WHILE,
  /* other terminal symbols */
  TK_CONCAT, 	/* .. */
  TK_DOTS,		/* ... */
  TK_EQ, TK_GE, TK_LE, TK_NE, TK_NUMBER,
  TK_NAME,		/* 标识符 */
  TK_STRING,	/* 字符串 */
  TK_EOS
};

/* number of reserved words */
#define NUM_RESERVED	(cast(int, TK_WHILE-FIRST_RESERVED+1))


/* array with token `names' */
LUAI_DATA const char *const luaX_tokens [];


typedef union {
  lua_Number r;	/* 若为数字则存储实际的数字 */
  TString *ts;	/* 若为字符串则指向字符串的addr，实际字符串存在global_state中 */
} SemInfo;  /* semantics(语义) information */


typedef struct Token {
  int token;		/* token类型 */
  SemInfo seminfo;	/* token值 */
} Token;

/* 整个编译逻辑的状态机而不单单是lex */
typedef struct LexState {
  TString *source;		  /* current source name ：块名*/

  struct lua_State *L;

  struct FuncState *fs;  	/* `FuncState' is private to the parser */

  int current;  			/* current character (charint), 下一个要被分析的字符，类似vm中的pc */
  int linenumber;  			/* input line counter */

  /* 相关函数  luaX_next */
  Token t;  				/* current token 当前被分析ing的token */
  Token lookahead;  		/* look ahead token 提前准备好的token，被取走后就失效了(除非重新准备此lookhead'token) */

  int lastline;  			/* line of last token `consumed' */

  /* buff可以看成是我的结构体的内部域，用于解析某个token时的临时存储区 */
  Mbuffer *buff;  			/* buffer for tokens */
  
  char decpoint;  			/* locale decimal point，不同国家小数点不一样(eg:中国是.,法国是,) */

  /* z有点外面的系统指针的意思（相对buff）*/
  ZIO *z;  					/* input stream */
} LexState;


LUAI_FUNC void luaX_init (lua_State *L);
LUAI_FUNC void luaX_setinput (lua_State *L, LexState *ls, ZIO *z,
                              TString *source);
LUAI_FUNC TString *luaX_newstring (LexState *ls, const char *str, size_t l);
LUAI_FUNC void luaX_next (LexState *ls);
LUAI_FUNC void luaX_lookahead (LexState *ls);
LUAI_FUNC void luaX_lexerror (LexState *ls, const char *msg, int token);
LUAI_FUNC void luaX_syntaxerror (LexState *ls, const char *s);
LUAI_FUNC const char *luaX_token2str (LexState *ls, int token);


#endif
