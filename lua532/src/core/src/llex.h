/*
** $Id: llex.h,v 1.78 2014/10/29 15:38:24 roberto Exp $
** Lexical Analyzer
** See Copyright Notice in lua.h
*/

#ifndef llex_h
#define llex_h

#include "lobject.h"
#include "lzio.h"


#define FIRST_RESERVED	257


#if !defined(LUA_ENV)
#define LUA_ENV		"_ENV"
#endif


/*
* WARNING: if you change the order of this enumeration,
* grep "ORDER RESERVED"
*
* FIRST_RESERVED==256+1
* TK_AND是TOKEN_AND的简写
*/
enum RESERVED {
  /* terminal symbols denoted(表示) by reserved words */
  TK_AND = FIRST_RESERVED, TK_BREAK,
  TK_DO, TK_ELSE, TK_ELSEIF, TK_END, TK_FALSE, TK_FOR, TK_FUNCTION,
  TK_GOTO, TK_IF, TK_IN, TK_LOCAL, TK_NIL, TK_NOT, TK_OR, TK_REPEAT,
  TK_RETURN, TK_THEN, TK_TRUE, TK_UNTIL, TK_WHILE,
  
  /* other terminal symbols */
  TK_IDIV,			/* //(整数除法) */
  TK_CONCAT,		/* .. */
  TK_DOTS,			/* ... */
  TK_EQ, TK_GE, TK_LE, TK_NE,
  TK_SHL, TK_SHR,
  TK_DBCOLON,		/* :: 形成goto标签？  ::gotoLabel:: */
  TK_EOS,				/* end of file还是看成暂未读取下一个token? */

	/* 这些时，token表示对应的类型，具体的内容直接读 SemInfo中的信息即可 */
  TK_FLT, TK_INT, TK_NAME, TK_STRING
};

/* 
 * number of reserved words
 * 其它的应该是保留的符号
*/
#define NUM_RESERVED	(cast(int, TK_WHILE-FIRST_RESERVED+1))

//语义方面的信息，分别表示float,int,string三种字面常量
typedef union {
  lua_Number  r;
  lua_Integer i;
  TString   *ts;
} SemInfo;  /*  information */

/* 主要的结构体 */
typedef struct Token 
{
  /* 注释可能有误，以实际情况为准
  ** [FIRST_RESERVED,+N]的是关键字或操作符对应的token,eg:local, >=
  ** [1,FIRST_RESERVED),单个字符对应的token,eg: >
   */
  int token;	
  
  SemInfo seminfo;
} Token;


/* 
** state of the lexer plus state of the parser(解析器) when shared by all functions 
** 数据流
** LexState不仅用于保存当前的词法分析状态信息，
** 而且也保存了整个编译系统的全局状态
**
** step.1 从ZIO中读一个char
**    ZIO->n--, int tmp = *ZIO->p++
** step.2 将上一步读到的char 赋值给 LexState.current
**     LexState.current = tmp
** step.3 判断读入的tmp是否符合要求
**      符合要求：
**					LexState.buff[idx] = LexState.current
**      不符合要求: 丢弃LexState.current,
**
*/
typedef struct LexState {
  /* 当前正被分析的char */
  int current;  	  /* current character (charint) */
  /* 上述current对应的行号 */
  int linenumber;  	/* input line counter */

  /* 上一个有效token应的行号 */
  int lastline;  	  /* line of last token 'consumed' */
  
  /* current function (parser) 
  ** 当前正在被编译的函数
  */
  struct FuncState *fs;  /* fs->f跳到对应Proto */
  struct lua_State *L;
  
  ZIO *z;  			    /* input stream */
  Mbuffer *buff;  	/* buffer for tokens(用于当前正在形成的token的buff) */

  /* 当前被分析的token（语法分析器中被使用,词法分析器中仅写入，不分析）*/
  Token t;          /* current token */
  Token lookahead;  /* look ahead token：前看符号 */

  /* 缓存词法分析，语法分析中生成的字符串
  ** 
  ** 这个表的结构是这样的 
  ** h["varName1"] = 0
  ** h["varName2"] = 1
  ** h["varName2"] = 2
  ** key:表示字符串常量，右边的0,1,2表示index(Proto->k数组中对应的索引)
  **
  ** 当要查找一个字符串常量时，先到此表(h)中找,找到了且type(val)=int且符合Proto相关数组索引范围的限制
  ** 且相等...则直接返回val(这个val就表示Proto.k中的下标)
  ** 不满足条件则将字符串常量对应的TString压入Proto->k中，并返回对应的索引
  */
  Table *h;           /* to avoid collection/reuse strings */
	
  /* 
  ** dynamic structures used by the parser
  ** 不太明白这个域的设计目的
  */ 
  struct Dyndata *dyd;

  TString *source;  	/* current source name */
	/* 环境名
	** 当前是： "_ENV" , 还不太明白具体的用意，等后面语法分析了再说
	*/
  TString *envn;  		/* environment variable name */
  
  char decpoint;  		/* locale decimal point 表示个位和小数位分割的".",不用的国家，符号不一样 */
} LexState;


LUAI_FUNC void luaX_init (lua_State *L);
LUAI_FUNC void luaX_setinput (lua_State *L, LexState *ls, ZIO *z,
                              TString *source, int firstchar);
LUAI_FUNC TString *luaX_newstring (LexState *ls, const char *str, size_t l);
LUAI_FUNC void luaX_next (LexState *ls);
LUAI_FUNC int luaX_lookahead (LexState *ls);
LUAI_FUNC l_noret luaX_syntaxerror (LexState *ls, const char *s);
LUAI_FUNC const char *luaX_token2str (LexState *ls, int token);


#endif
