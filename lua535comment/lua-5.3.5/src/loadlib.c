/*
** $Id: loadlib.c,v 1.130.1.1 2017/04/19 17:20:42 roberto Exp $
** Dynamic library loader for Lua
** See Copyright Notice in lua.h
**
** This module contains an implementation of loadlib for Unix systems
** that have dlfcn, an implementation for Windows, and a stub for other
** systems.
*/

#define loadlib_c
#define LUA_LIB

#include "lprefix.h"


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"


/*
** LUA_IGMARK is a mark to ignore all before it when building the
** luaopen_ function name.
*/
#if !defined (LUA_IGMARK)
#define LUA_IGMARK		"-"
#endif


/*
** LUA_CSUBSEP is the character that replaces dots in submodule names
** when searching for a C loader.
** LUA_LSUBSEP is the character that replaces dots in submodule names
** when searching for a Lua loader.
*/
#if !defined(LUA_CSUBSEP)
#define LUA_CSUBSEP		LUA_DIRSEP
#endif

#if !defined(LUA_LSUBSEP)
#define LUA_LSUBSEP		LUA_DIRSEP
#endif


/* prefix for open functions in C libraries */
#define LUA_POF		"luaopen_"

/* separator for open functions in C libraries */
#define LUA_OFSEP	"_"


/*
** unique key for table in the registry that keeps handles
** for all loaded C libraries
*/
static const int CLIBS = 0;

#define LIB_FAIL	"open"


#define setprogdir(L)           ((void)0)


/*
** system-dependent functions
*/

/*
** unload library 'lib'
*/
static void lsys_unloadlib (void *lib);

/*
** load C library in file 'path'. If 'seeglb', load with all names in
** the library global.
** Returns the library; in case of error, returns NULL plus an
** error string in the stack.
*/
static void *lsys_load (lua_State *L, const char *path, int seeglb);

/*
** Try to find a function named 'sym' in library 'lib'.
** Returns the function; in case of error, returns NULL plus an
** error string in the stack.
*/
static lua_CFunction lsys_sym (lua_State *L, void *lib, const char *sym);




#if defined(LUA_USE_DLOPEN)	/* { */
/*
** {========================================================================
** This is an implementation of loadlib based on the dlfcn interface.
** The dlfcn interface is available in Linux, SunOS, Solaris, IRIX, FreeBSD,
** NetBSD, AIX 4.2, HPUX 11, and  probably most other Unix flavors, at least
** as an emulation layer on top of native functions.
** =========================================================================
*/

#include <dlfcn.h>

/*
** Macro to convert pointer-to-void* to pointer-to-function. This cast
** is undefined according to ISO C, but POSIX assumes that it works.
** (The '__extension__' in gnu compilers is only to avoid warnings.)
*/
#if defined(__GNUC__)
#define cast_func(p) (__extension__ (lua_CFunction)(p))
#else
#define cast_func(p) ((lua_CFunction)(p))
#endif


static void lsys_unloadlib (void *lib) {
  dlclose(lib);
}


static void *lsys_load (lua_State *L, const char *path, int seeglb) {
  void *lib = dlopen(path, RTLD_NOW | (seeglb ? RTLD_GLOBAL : RTLD_LOCAL));
  if (lib == NULL) lua_pushstring(L, dlerror());
  return lib;
}


static lua_CFunction lsys_sym (lua_State *L, void *lib, const char *sym) {
  lua_CFunction f = cast_func(dlsym(lib, sym));
  if (f == NULL) lua_pushstring(L, dlerror());
  return f;
}

/* }====================================================== */



#elif defined(LUA_DL_DLL)	/* }{ */
/*
** {======================================================================
** This is an implementation of loadlib for Windows using native functions.
** =======================================================================
*/

#include <windows.h>


/*
** optional flags for LoadLibraryEx
*/
#if !defined(LUA_LLE_FLAGS)
#define LUA_LLE_FLAGS	0
#endif


#undef setprogdir


/*
** Replace in the path (on the top of the stack) any occurrence
** of LUA_EXEC_DIR with the executable's path.
*/
static void setprogdir (lua_State *L) {
  char buff[MAX_PATH + 1];
  char *lb;
  DWORD nsize = sizeof(buff)/sizeof(char);
  DWORD n = GetModuleFileNameA(NULL, buff, nsize);  /* get exec. name */
  if (n == 0 || n == nsize || (lb = strrchr(buff, '\\')) == NULL)
    luaL_error(L, "unable to get ModuleFileName");
  else {
    *lb = '\0';  /* cut name on the last '\\' to get the path */
    luaL_gsub(L, lua_tostring(L, -1), LUA_EXEC_DIR, buff);
    lua_remove(L, -2);  /* remove original string */
  }
}




static void pusherror (lua_State *L) {
  int error = GetLastError();
  char buffer[128];
  if (FormatMessageA(FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_SYSTEM,
      NULL, error, 0, buffer, sizeof(buffer)/sizeof(char), NULL))
    lua_pushstring(L, buffer);
  else
    lua_pushfstring(L, "system error %d\n", error);
}

static void lsys_unloadlib (void *lib) {
  FreeLibrary((HMODULE)lib);
}


static void *lsys_load (lua_State *L, const char *path, int seeglb) {
  HMODULE lib = LoadLibraryExA(path, NULL, LUA_LLE_FLAGS);
  (void)(seeglb);  /* not used: symbols are 'global' by default */
  if (lib == NULL) pusherror(L);
  return lib;
}


static lua_CFunction lsys_sym (lua_State *L, void *lib, const char *sym) {
  lua_CFunction f = (lua_CFunction)GetProcAddress((HMODULE)lib, sym);
  if (f == NULL) pusherror(L);
  return f;
}

/* }====================================================== */


#else				/* }{ */
/*
** {======================================================
** Fallback for other systems
** =======================================================
*/

#undef LIB_FAIL
#define LIB_FAIL	"absent"


#define DLMSG	"dynamic libraries not enabled; check your Lua installation"


static void lsys_unloadlib (void *lib) {
  (void)(lib);  /* not used */
}


static void *lsys_load (lua_State *L, const char *path, int seeglb) {
  (void)(path); (void)(seeglb);  /* not used */
  lua_pushliteral(L, DLMSG);
  return NULL;
}


static lua_CFunction lsys_sym (lua_State *L, void *lib, const char *sym) {
  (void)(lib); (void)(sym);  /* not used */
  lua_pushliteral(L, DLMSG);
  return NULL;
}

/* }====================================================== */
#endif				/* } */


/*
** {==================================================================
** Set Paths
** ===================================================================
*/

/*
** LUA_PATH_VAR and LUA_CPATH_VAR are the names of the environment
** variables that Lua check to set its paths.
*/
#if !defined(LUA_PATH_VAR)
#define LUA_PATH_VAR    "LUA_PATH"
#endif

#if !defined(LUA_CPATH_VAR)
#define LUA_CPATH_VAR   "LUA_CPATH"
#endif


#define AUXMARK         "\1"	/* auxiliary mark */


/*
** return registry.LUA_NOENV as a boolean
*/
static int noenv (lua_State *L) {
  int b;
  lua_getfield(L, LUA_REGISTRYINDEX, "LUA_NOENV");
  b = lua_toboolean(L, -1);
  lua_pop(L, 1);  /* remove value */
  return b;
}


/*
** Set a path
*/
/* 根据envname等信息构造相应的path信息，然后以fieldname为键值，将path信息添加到package库对应的table中 */
static void setpath (lua_State *L, const char *fieldname,
                                   const char *envname,
                                   const char *dft) {
  /* 构造版本号信息对应的环境变量的名字，并压入栈顶部 */
  const char *nver = lua_pushfstring(L, "%s%s", envname, LUA_VERSUFFIX);
  
  /* 获取版本号信息对应的环境变量的具体内容 */
  const char *path = getenv(nver);  /* use versioned name */

  /* 构造path的内容 */
  if (path == NULL)  /* no environment variable? */
    path = getenv(envname);  /* try unversioned name */
  if (path == NULL || noenv(L))  /* no environment variable? */
    lua_pushstring(L, dft);  /* use default */
  else {
    /* replace ";;" by ";AUXMARK;" and then AUXMARK by default path */
    path = luaL_gsub(L, path, LUA_PATH_SEP LUA_PATH_SEP,
                              LUA_PATH_SEP AUXMARK LUA_PATH_SEP);
    luaL_gsub(L, path, AUXMARK, dft);
    lua_remove(L, -2); /* remove result from 1st 'gsub' */
  }
  setprogdir(L);
  /* 将构造好的path路径以fieldname为键值加入到package库对应的table中。添加完后path路径会弹出栈顶部。 */
  lua_setfield(L, -3, fieldname);  /* package[fieldname] = path value */
  lua_pop(L, 1);  /* pop versioned variable name */
}

/* }================================================================== */


/*
** return registry.CLIBS[path]
*/
static void *checkclib (lua_State *L, const char *path) {
  void *plib;
  lua_rawgetp(L, LUA_REGISTRYINDEX, &CLIBS);
  lua_getfield(L, -1, path);
  plib = lua_touserdata(L, -1);  /* plib = CLIBS[path] */
  lua_pop(L, 2);  /* pop CLIBS table and 'plib' */
  return plib;
}


/*
** registry.CLIBS[path] = plib        -- for queries
** registry.CLIBS[#CLIBS + 1] = plib  -- also keep a list of all libraries
*/
static void addtoclib (lua_State *L, const char *path, void *plib) {
  lua_rawgetp(L, LUA_REGISTRYINDEX, &CLIBS);
  lua_pushlightuserdata(L, plib);
  lua_pushvalue(L, -1);
  lua_setfield(L, -3, path);  /* CLIBS[path] = plib */
  lua_rawseti(L, -2, luaL_len(L, -2) + 1);  /* CLIBS[#CLIBS + 1] = plib */
  lua_pop(L, 1);  /* pop CLIBS table */
}


/*
** __gc tag method for CLIBS table: calls 'lsys_unloadlib' for all lib
** handles in list CLIBS
*/
static int gctm (lua_State *L) {
  lua_Integer n = luaL_len(L, 1);
  for (; n >= 1; n--) {  /* for each handle, in reverse order */
    lua_rawgeti(L, 1, n);  /* get handle CLIBS[n] */
    lsys_unloadlib(lua_touserdata(L, -1));
    lua_pop(L, 1);  /* pop handle */
  }
  return 0;
}



/* error codes for 'lookforfunc' */
#define ERRLIB		1
#define ERRFUNC		2

/*
** Look for a C function named 'sym' in a dynamically loaded library
** 'path'.
** First, check whether the library is already loaded; if not, try
** to load it.
** Then, if 'sym' is '*', return true (as library has been loaded).
** Otherwise, look for symbol 'sym' in the library and push a
** C function with that symbol.
** Return 0 and 'true' or a function in the stack; in case of
** errors, return an error code and an error message in the stack.
*/
static int lookforfunc (lua_State *L, const char *path, const char *sym) {
  void *reg = checkclib(L, path);  /* check loaded C libraries */
  if (reg == NULL) {  /* must load library? */
    reg = lsys_load(L, path, *sym == '*');  /* global symbols if 'sym'=='*' */
    if (reg == NULL) return ERRLIB;  /* unable to load library */
    addtoclib(L, path, reg);
  }
  if (*sym == '*') {  /* loading only library (no function)? */
    lua_pushboolean(L, 1);  /* return 'true' */
    return 0;  /* no errors */
  }
  else {
    lua_CFunction f = lsys_sym(L, reg, sym);
    if (f == NULL)
      return ERRFUNC;  /* unable to find function */
    lua_pushcfunction(L, f);  /* else create new function */
    return 0;  /* no errors */
  }
}


static int ll_loadlib (lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  const char *init = luaL_checkstring(L, 2);
  int stat = lookforfunc(L, path, init);
  if (stat == 0)  /* no errors? */
    return 1;  /* return the loaded function */
  else {  /* error; error message is on stack top */
    lua_pushnil(L);
    lua_insert(L, -2);
    lua_pushstring(L, (stat == ERRLIB) ?  LIB_FAIL : "init");
    return 3;  /* return nil, error message, and where */
  }
}



/*
** {======================================================
** 'require' function
** =======================================================
*/


static int readable (const char *filename) {
  FILE *f = fopen(filename, "r");  /* try to open file */
  if (f == NULL) return 0;  /* open failed */
  fclose(f);
  return 1;
}


static const char *pushnexttemplate (lua_State *L, const char *path) {
  const char *l;
  while (*path == *LUA_PATH_SEP) path++;  /* skip separators */
  if (*path == '\0') return NULL;  /* no more templates */
  l = strchr(path, *LUA_PATH_SEP);  /* find next separator */
  if (l == NULL) l = path + strlen(path);
  lua_pushlstring(L, path, l - path);  /* template */
  return l;
}


static const char *searchpath (lua_State *L, const char *name,
                                             const char *path,
                                             const char *sep,
                                             const char *dirsep) {
  luaL_Buffer msg;  /* to build error message */
  luaL_buffinit(L, &msg);
  if (*sep != '\0')  /* non-empty separator? */
    name = luaL_gsub(L, name, sep, dirsep);  /* replace it by 'dirsep' */
  while ((path = pushnexttemplate(L, path)) != NULL) {
    const char *filename = luaL_gsub(L, lua_tostring(L, -1),
                                     LUA_PATH_MARK, name);
    lua_remove(L, -2);  /* remove path template */
    if (readable(filename))  /* does file exist and is readable? */
      return filename;  /* return that file name */
    lua_pushfstring(L, "\n\tno file '%s'", filename);
    lua_remove(L, -2);  /* remove file name */
    luaL_addvalue(&msg);  /* concatenate error msg. entry */
  }
  luaL_pushresult(&msg);  /* create error message */
  return NULL;  /* not found */
}


static int ll_searchpath (lua_State *L) {
  const char *f = searchpath(L, luaL_checkstring(L, 1),
                                luaL_checkstring(L, 2),
                                luaL_optstring(L, 3, "."),
                                luaL_optstring(L, 4, LUA_DIRSEP));
  if (f != NULL) return 1;
  else {  /* error message is on top of the stack */
    lua_pushnil(L);
    lua_insert(L, -2);
    return 2;  /* return nil + error message */
  }
}


static const char *findfile (lua_State *L, const char *name,
                                           const char *pname,
                                           const char *dirsep) {
  const char *path;
  lua_getfield(L, lua_upvalueindex(1), pname);
  path = lua_tostring(L, -1);
  if (path == NULL)
    luaL_error(L, "'package.%s' must be a string", pname);
  return searchpath(L, name, path, ".", dirsep);
}


static int checkload (lua_State *L, int stat, const char *filename) {
  if (stat) {  /* module loaded successfully? */
    lua_pushstring(L, filename);  /* will be 2nd argument to module */
    return 2;  /* return open function and file name */
  }
  else
    return luaL_error(L, "error loading module '%s' from file '%s':\n\t%s",
                          lua_tostring(L, 1), filename, lua_tostring(L, -1));
}


static int searcher_Lua (lua_State *L) {
  const char *filename;
  const char *name = luaL_checkstring(L, 1);
  filename = findfile(L, name, "path", LUA_LSUBSEP);
  if (filename == NULL) return 1;  /* module not found in this path */
  return checkload(L, (luaL_loadfile(L, filename) == LUA_OK), filename);
}


/*
** Try to find a load function for module 'modname' at file 'filename'.
** First, change '.' to '_' in 'modname'; then, if 'modname' has
** the form X-Y (that is, it has an "ignore mark"), build a function
** name "luaopen_X" and look for it. (For compatibility, if that
** fails, it also tries "luaopen_Y".) If there is no ignore mark,
** look for a function named "luaopen_modname".
*/
static int loadfunc (lua_State *L, const char *filename, const char *modname) {
  const char *openfunc;
  const char *mark;
  modname = luaL_gsub(L, modname, ".", LUA_OFSEP);
  mark = strchr(modname, *LUA_IGMARK);
  if (mark) {
    int stat;
    openfunc = lua_pushlstring(L, modname, mark - modname);
    openfunc = lua_pushfstring(L, LUA_POF"%s", openfunc);
    stat = lookforfunc(L, filename, openfunc);
    if (stat != ERRFUNC) return stat;
    modname = mark + 1;  /* else go ahead and try old-style name */
  }
  openfunc = lua_pushfstring(L, LUA_POF"%s", modname);
  return lookforfunc(L, filename, openfunc);
}


static int searcher_C (lua_State *L) {
  const char *name = luaL_checkstring(L, 1);
  const char *filename = findfile(L, name, "cpath", LUA_CSUBSEP);
  if (filename == NULL) return 1;  /* module not found in this path */
  return checkload(L, (loadfunc(L, filename, name) == 0), filename);
}


static int searcher_Croot (lua_State *L) {
  const char *filename;
  const char *name = luaL_checkstring(L, 1);
  const char *p = strchr(name, '.');
  int stat;
  if (p == NULL) return 0;  /* is root */
  lua_pushlstring(L, name, p - name);
  filename = findfile(L, lua_tostring(L, -1), "cpath", LUA_CSUBSEP);
  if (filename == NULL) return 1;  /* root not found */
  if ((stat = loadfunc(L, filename, name)) != 0) {
    if (stat != ERRFUNC)
      return checkload(L, 0, filename);  /* real error */
    else {  /* open function not found */
      lua_pushfstring(L, "\n\tno module '%s' in file '%s'", name, filename);
      return 1;
    }
  }
  lua_pushstring(L, filename);  /* will be 2nd argument to module */
  return 2;
}


static int searcher_preload (lua_State *L) {
  const char *name = luaL_checkstring(L, 1);
  lua_getfield(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
  if (lua_getfield(L, -1, name) == LUA_TNIL)  /* not found? */
    lua_pushfstring(L, "\n\tno field package.preload['%s']", name);
  return 1;
}

/* 从package.searchers这个table中找到库名为name对应的库加载函数，例如数学库的luaopen_math() */
static void findloader (lua_State *L, const char *name) {
  int i;
  luaL_Buffer msg;  /* to build error message */
  luaL_buffinit(L, &msg);
  /* push 'package.searchers' to index 3 in the stack */
  /* 从包package中获取searchers这个table，并将其压入栈，栈索引值为3。 */
  if (lua_getfield(L, lua_upvalueindex(1), "searchers") != LUA_TTABLE)
    luaL_error(L, "'package.searchers' must be a table");
  /*  iterate over available searchers to find a loader */
  for (i = 1; ; i++) {
    /* 以整数作为键值遍历package.searchers这个table，获取用于寻找库加载函数的搜索函数，并压入栈顶部 */
    if (lua_rawgeti(L, 3, i) == LUA_TNIL) {  /* no more searchers? */
      lua_pop(L, 1);  /* remove nil */
      luaL_pushresult(&msg);  /* create error message */
      luaL_error(L, "module '%s' not found:%s", name, lua_tostring(L, -1));
    }
	
    /* 将库名字压入堆栈，作为寻找库加载函数的搜索函数的参数 */
    lua_pushstring(L, name);

	/*
	** 调用搜索函数，1表示需要函数参数个数为1,2表示函数返回值个数。函数调用的返回值会压入栈顶部。
	** 每个返回值都会占用一个栈单元。
	*/
    lua_call(L, 1, 2);  /* call it */
	/*
	** 判断函数调用结果的第一个返回值是不是一个函数，如果是一个函数，说明找到了name对应的库加载函数。
	** 此时库加载函数位于栈次顶部位置。栈顶部是另外一个返回值
	*/
    if (lua_isfunction(L, -2))  /* did it find a loader? */
      return;  /* module loader found */
    else if (lua_isstring(L, -2)) {  /* searcher returned error message? */
      lua_pop(L, 1);  /* remove extra return */
      luaL_addvalue(&msg);  /* concatenate error message */
    }
    else
      lua_pop(L, 2);  /* remove both returns */
  }
}

/* 关键字requuire的对应的处理函数 */
static int ll_require (lua_State *L) {
  /* 
  ** 获取处于目前函数调用栈的栈索引值为1（对应L->ci->func + 1）的栈元素的内容，以字符串显示结果，
  ** 即紧跟在require关键字后面的库名，这个是require函数的参数。如require "math"，name这里的name
  ** 就是"math"。
  */
  const char *name = luaL_checkstring(L, 1);
  
  /* 重新设置栈指针L->top，设置完成之后，L->top = L->ci->func + 1 + 1，即存放库名字的栈单元的
  ** 下一个栈单元。
  */
  lua_settop(L, 1);  /* LOADED table will be at index 2 */
  
  /*
  ** 从栈索引值（伪索引）为LUA_REGISTRYINDEX的注册表中获取键值为"_LOADED"的table，并压入栈顶部，
  ** 此时"_LOADED" table在该函数调用栈中的索引值为2。索引值为1的库名字。
  */
  lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
  
  /* 
  ** 从位于栈顶部的"_LOADED" table中获取键值为name对应的元素，并将该元素压入栈顶部。如果该库之前
  ** 没有加载过，那么这个元素应该是一个nil；如果之前加载过，那么就应该是一个库级别的table，存放了
  ** 对应库的函数和常量等信息。
  */
  lua_getfield(L, 2, name);  /* LOADED[name] */

  /* 如果该库已经加载过了，就不再加载一遍了，直接使用即可。 */
  if (lua_toboolean(L, -1))  /* is it there? */
    return 1;  /* package is already loaded */
  /* else must load package */
  
  /* 程序执行到这里，说明require的库还没加载过，那么此时位于栈顶部的元素是无用的，这里将其弹出栈顶部 */
  lua_pop(L, 1);  /* remove 'getfield' result */

  /* 
  ** 找到库名name对应的库加载函数，此时库加载函数位于栈次顶部单元，而栈顶部单元是搜索库加载函数的
  ** 搜索函数的第二个返回值，第一个返回值就是库加载函数。
  */
  findloader(L, name);

  /* 将库名压入栈顶部，作为库加载函数的参数 */
  lua_pushstring(L, name);  /* pass name as argument to module loader */
  /*
  ** 交换栈顶部的库名和栈次顶部的搜索函数的第二个返回值，交换完成后，栈次顶部是库名字，而栈顶部
  ** 则是库加载函数的搜索函数的第二个返回值。这两个值都要作为库加载函数的参数，库加载函数的第一个
  ** 参数一定要是库名字。
  */
  lua_insert(L, -2);  /* name is 1st argument (before search data) */
  /* 这里会调用库加载函数，库加载的结果（一般来说一个table，包含了库函数和常量）会压入栈顶部 */
  lua_call(L, 2, 1);  /* run loader to load module */
  /*
  ** 判断库加载函数的执行结果是不是nil，如果不是nil，则将执行结果以库名name为键值添加到栈索引值为
  ** 2的"_LOADED" table中，同时将执行结果弹出栈顶部。本函数开始部分已经将"_LOADED" table压入了
  ** 栈索引值为2的地方。
  */
  if (!lua_isnil(L, -1))  /* non-nil return? */
    lua_setfield(L, 2, name);  /* LOADED[name] = returned value */

  /*
  ** 如果库加载函数返回的是nil，那么就以name为键值，将bool值true添加到"_LOADED" table中，
  ** 表示该库已经加载过了。
  ** 尝试以库名name从"_LOADED" table中获取对应的值对象（正常情况下是库name的加载函数的执行结果），
  ** 将获取到的值对象压入栈顶部。判断位于栈顶的值对象是不是nil，是nil的话，那么就以name为键值，
  ** 将bool值true添加到"_LOADED" table中，表示该库已经加载过了。如果不是nil，那么此时位于栈顶部的
  ** 就是库name加载函数的执行结果（一般来说是一个table）。
  */
  if (lua_getfield(L, 2, name) == LUA_TNIL) {   /* module set no value? */
    lua_pushboolean(L, 1);  /* use true as result */
    lua_pushvalue(L, -1);  /* extra copy to be returned */ /* 这一步的目的是什么？暂时没看明白 */
    lua_setfield(L, 2, name);  /* LOADED[name] = true */
  }

  /*
  ** 程序执行到这里，位于栈顶部的是库name加载函数的执行结果（一般来说是该库对应的table），
  ** 如果将require的结果赋值给一个本地变量，那么就是将这个table赋值给了这个变量。例如：
  ** local math = require "math"，那么就是将math库对应的table赋值给了math变量。这对于上面那个
  ** 库已经加载的情况也是一样的。
  */
  return 1;
}

/* }====================================================== */



/*
** {======================================================
** 'module' function
** =======================================================
*/
#if defined(LUA_COMPAT_MODULE)

/*
** changes the environment variable of calling function
*/
static void set_env (lua_State *L) {
  lua_Debug ar;
  if (lua_getstack(L, 1, &ar) == 0 ||
      lua_getinfo(L, "f", &ar) == 0 ||  /* get calling function */
      lua_iscfunction(L, -1))
    luaL_error(L, "'module' not called from a Lua function");
  lua_pushvalue(L, -2);  /* copy new environment table to top */
  lua_setupvalue(L, -2, 1);
  lua_pop(L, 1);  /* remove function */
}


static void dooptions (lua_State *L, int n) {
  int i;
  for (i = 2; i <= n; i++) {
    if (lua_isfunction(L, i)) {  /* avoid 'calling' extra info. */
      lua_pushvalue(L, i);  /* get option (a function) */
      lua_pushvalue(L, -2);  /* module */
      lua_call(L, 1, 0);
    }
  }
}


static void modinit (lua_State *L, const char *modname) {
  const char *dot;
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "_M");  /* module._M = module */
  lua_pushstring(L, modname);
  lua_setfield(L, -2, "_NAME");
  dot = strrchr(modname, '.');  /* look for last dot in module name */
  if (dot == NULL) dot = modname;
  else dot++;
  /* set _PACKAGE as package name (full module name minus last part) */
  lua_pushlstring(L, modname, dot - modname);
  lua_setfield(L, -2, "_PACKAGE");
}


static int ll_module (lua_State *L) {
  const char *modname = luaL_checkstring(L, 1);
  int lastarg = lua_gettop(L);  /* last parameter */
  luaL_pushmodule(L, modname, 1);  /* get/create module table */
  /* check whether table already has a _NAME field */
  if (lua_getfield(L, -1, "_NAME") != LUA_TNIL)
    lua_pop(L, 1);  /* table is an initialized module */
  else {  /* no; initialize it */
    lua_pop(L, 1);
    modinit(L, modname);
  }
  lua_pushvalue(L, -1);
  set_env(L);
  dooptions(L, lastarg);
  return 1;
}


static int ll_seeall (lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  if (!lua_getmetatable(L, 1)) {
    lua_createtable(L, 0, 1); /* create new metatable */
    lua_pushvalue(L, -1);
    lua_setmetatable(L, 1);
  }
  lua_pushglobaltable(L);
  lua_setfield(L, -2, "__index");  /* mt.__index = _G */
  return 0;
}

#endif
/* }====================================================== */



static const luaL_Reg pk_funcs[] = {
  {"loadlib", ll_loadlib},
  {"searchpath", ll_searchpath},
#if defined(LUA_COMPAT_MODULE)
  {"seeall", ll_seeall},
#endif
  /* placeholders */
  {"preload", NULL},
  {"cpath", NULL},
  {"path", NULL},
  {"searchers", NULL},
  {"loaded", NULL},
  {NULL, NULL}
};


static const luaL_Reg ll_funcs[] = {
#if defined(LUA_COMPAT_MODULE)
  {"module", ll_module},
#endif
  {"require", ll_require},
  {NULL, NULL}
};

/*
** 创建存放了搜索函数的table，并将该table以"searchers"为键值存放到package库对应的库级别table中。
** 该函数执行完之后，位于栈顶部的元素就是package库对应的库级别的table。
*/
static void createsearcherstable (lua_State *L) {
  /* 定义搜索函数 */
  static const lua_CFunction searchers[] =
    {searcher_preload, searcher_Lua, searcher_C, searcher_Croot, NULL};
  int i;
  /* create 'searchers' table */
  /*
  ** 创建一个用于存放搜索函数的table，搜索函数存放在table的数组部分，用数字索引。
  ** 该table创建完成之后会压入栈顶部。
  */
  lua_createtable(L, sizeof(searchers)/sizeof(searchers[0]) - 1, 0);
  /* fill it with predefined searchers */
  /*
  ** 将预定义的搜索函数存入到table中，注意，搜索函数会有一个内容为'package'的自由变量，因此
  ** 要将预定义的搜索函数以CClosure的方式添加到table中。除了函数自身之外，函数的自由变量也会
  ** 存放到CClosure中。
  */
  for (i=0; searchers[i] != NULL; i++) {
    lua_pushvalue(L, -2);  /* set 'package' as upvalue for all searchers */
    lua_pushcclosure(L, searchers[i], 1);
    lua_rawseti(L, -2, i+1);
  }
#if defined(LUA_COMPAT_LOADERS)
  lua_pushvalue(L, -1);  /* make a copy of 'searchers' table */
  lua_setfield(L, -3, "loaders");  /* put it in field 'loaders' */
#endif
  /*
  ** 将存放了搜索函数的table以"searchers"为键值，添加到libpackage对应的库级别table中，作为其子table。
  ** 在这里之后就可以用"searchers"去package这个包对应的table中去获取存放了搜索函数的table了。
  */
  lua_setfield(L, -2, "searchers");  /* put it in field 'searchers' */

  /* 该函数执行完之后，位于栈顶部的元素就是package库对应的库级别的table。*/
}


/*
** create table CLIBS to keep track of loaded C libraries,
** setting a finalizer to close all libraries when closing state.
*/
/* 
** 创建用于跟踪所有加载了的C库的CLIBS table，CLIBS table会以键值0存入到栈索引值为
** LUA_REGISTRYINDEX的注册表中。
*/
static void createclibstable (lua_State *L) {
  /* 
  ** 创建一个空的table，即数组部分和hash表部分大小均为0，这个table会作为CLIBS table，
  ** 执行完这条语句后，CLIBS table就位于栈顶部了。
  */
  lua_newtable(L);  /* create CLIBS table */

  /*
  ** 创建另外一个表，作为CLIBS table的元表，执行完这条语句后，CLIBS table的元表就位于栈顶部了。
  ** 而CLIBS table就位于栈次顶部。
  */
  lua_createtable(L, 0, 1);  /* create metatable for CLIBS */

  /* 将函数gctm压入栈顶部 */
  lua_pushcfunction(L, gctm);

  /*
  ** 将位于栈顶的函数gctm以键值“__gc”加入到第二条语句创建的那个表中，作为CLIBS table的finalizer。
  ** 同时会将gctm从栈顶部弹出。
  */
  lua_setfield(L, -2, "__gc");  /* set finalizer for CLIBS table */

  /* 将第二条语句中创建的table作为CLIBS table的元表，同时将这个元表从栈顶部弹出 */
  lua_setmetatable(L, -2);

  /* 程序执行到这里时，位于栈顶部的元素就是该函数一开始创建的CLIBS table */

  /*
  ** 将此时位于栈顶部的CLIBS table以0（CLIBS值为0）作为键值添加到栈索引值为LUA_REGISTRYINDEX的
  ** 注册表中，后面就可以用0去注册表中索引CLIBS table了。执行完下面的语句之后，也会将CLIBS table
  ** 从栈顶部弹出。
  */
  lua_rawsetp(L, LUA_REGISTRYINDEX, &CLIBS);  /* set CLIBS table in registry */
}

/*
** package库的加载函数，当该函数执行完毕后，位于栈顶的就是package库对应的table，里面存放了
** package库的函数和常量。
*/
LUAMOD_API int luaopen_package (lua_State *L) {
  /* 
  ** 创建用于跟踪所有加载了的C库的CLIBS table，CLIBS table会以键值0存入到栈索引值为
  ** LUA_REGISTRYINDEX的注册表中。
  */
  createclibstable(L);

  /* 
  ** 将packagelib中定义的函数及其名字存入一个table中，这个table是库级别的，
  ** 即一个库对应一个table。执行完luaL_newlib()之后，这个table就位于栈
  ** 最顶部。
  */
  luaL_newlib(L, pk_funcs);  /* create 'package' table */

  /*
  ** 创建存放了搜索函数的table，并将该table以"searchers"为键值存放到package库对应的库级别table中。
  ** 该函数执行完之后，位于栈顶部的元素就是package库对应的库级别的table。搜索函数会根据库名找到
  ** 库对应的加载函数。其实，package对应的table本身也会以"package"为键值加入到"_LOADED" table中。
  */
  createsearcherstable(L);
  /* set paths */
  /* 将path，cpath等信息添加到package库对应的table中 */
  setpath(L, "path", LUA_PATH_VAR, LUA_PATH_DEFAULT);
  setpath(L, "cpath", LUA_CPATH_VAR, LUA_CPATH_DEFAULT);
  /* store config information */
  /* 将配置信息压入栈顶部，此时位于栈次顶部的就是package库对应的table */
  lua_pushliteral(L, LUA_DIRSEP "\n" LUA_PATH_SEP "\n" LUA_PATH_MARK "\n"
                     LUA_EXEC_DIR "\n" LUA_IGMARK "\n");

  /* 将位于栈顶部的配置信息，以"config"为键值添加到package库对应的table中，并将配置信息弹出栈顶 */
  lua_setfield(L, -2, "config");
  
  /* set field 'loaded' */
  /*
  ** 从栈索引值（伪索引）为LUA_REGISTRYINDEX的注册表中获取"_LOADED" table，并压入栈顶部。然后将
  ** 此时位于栈顶部的"_LOADED" table以"loaded"为键值加入到package库的table中成为其子table，并将
  ** "_LOADED" table从栈顶弹出，经过这一系列操作后，注册表中的"_LOADED" table和package库对应的
  ** table中的"package.loaded"就是同一个表，存放的就是已经加载了的库的table。另外，package对应的
  ** table本身也会以"package"为键值加入到"_LOADED" table中。
  */
  luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
  lua_setfield(L, -2, "loaded");
  
  /* set field 'preload' */
  /*
  ** 从栈索引值（伪索引）为LUA_REGISTRYINDEX的注册表中获取"_PRELOAD" table，并压入栈顶部。然后将
  ** 此时位于栈顶部的"_PRELOAD" table以"preload"为键值加入到package库的table中成为其子table，并将
  ** "_LOADED" table从栈顶弹出，经过这一系列操作后，注册表中的"_PRELOAD" table和package库对应的
  ** table中的"package.preload"就是同一个表，存放的就是已经加载了的库的table。
  */
  luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
  lua_setfield(L, -2, "preload");
  
  /* 程序执行到这里，此时位于栈顶部的元素仍然是package库对应的table */

  /* 将_G table压入栈顶部 */
  lua_pushglobaltable(L);
  
  /*
  ** 此时位于栈次顶部的是package库对应table，这里将该table再次压入栈顶，之后位于次栈顶的是_G table。
  ** 这里之所以要将package库对应的table再次压入栈顶是为了要将该table作为ll_funcs中定义的函数的自由
  ** 变量，因此这里先将他们压入栈顶，luaL_setfuncs()负责将其设置成ll_funcs中定义函数的upvalue。
  ** 此时位于栈顶部的是package库对应的table，栈次顶部的是_G table，栈次次顶部的仍然是package库对应的table。
  ** 这里一定要分析清楚。
  */
  lua_pushvalue(L, -2);  /* set 'package' as upvalue for next lib */

  /* 
  ** 将全局变量ll_funcs中的函数注册到_G table中，键值是函数的名字，内容就是ll_funcs中定义的函数对应的
  ** CClosure对象。因为要将package库对应的库级别的table作为ll_funcs中定义的函数的自由变量，因此
  ** 要用CClosure对象。luaL_setfuncs()在结束的时候会将重复压入栈顶的package对应的table（作为自由变量）
  ** 弹出堆栈。
  */
  luaL_setfuncs(L, ll_funcs, 1);  /* open lib into global table */
  
  /* 程序执行到这里，此时位于栈顶部的是_G table，栈次顶部的是package库对应的table */
  /* 将_G table弹出栈顶 */
  lua_pop(L, 1);  /* pop global table */

  /* 程序执行到这里，位于栈顶的仍然是package库对应的table。 */
  return 1;  /* return 'package' table */
}

