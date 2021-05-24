/*
** $Id: linit.c,v 1.39.1.1 2017/04/19 17:20:42 roberto Exp $
** Initialization of libraries for lua.c and other clients
** See Copyright Notice in lua.h
*/


#define linit_c
#define LUA_LIB

/*
** If you embed Lua in your program and need to open the standard
** libraries, call luaL_openlibs in your program. If you need a
** different set of libraries, copy this file to your project and edit
** it to suit your needs.
**
** You can also *preload* libraries, so that a later 'require' can
** open the library, which is already linked to the application.
** For that, do the following code:
**
**  luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
**  lua_pushcfunction(L, luaopen_modname);
**  lua_setfield(L, -2, modname);
**  lua_pop(L, 1);  // remove PRELOAD table
*/

#include "lprefix.h"


#include <stddef.h>

#include "lua.h"

#include "lualib.h"
#include "lauxlib.h"


/*
** these libs are loaded by lua.c and are readily available to any Lua
** program
*/
/* lua启动过程中会加载的所有库，这些库在所有的lua程序中都可以使用 */
static const luaL_Reg loadedlibs[] = {
  {"_G", luaopen_base},
  {LUA_LOADLIBNAME, luaopen_package},
  {LUA_COLIBNAME, luaopen_coroutine},
  {LUA_TABLIBNAME, luaopen_table},
  {LUA_IOLIBNAME, luaopen_io},
  {LUA_OSLIBNAME, luaopen_os},
  {LUA_STRLIBNAME, luaopen_string},
  {LUA_MATHLIBNAME, luaopen_math},
  {LUA_UTF8LIBNAME, luaopen_utf8},
  {LUA_DBLIBNAME, luaopen_debug},
#if defined(LUA_COMPAT_BITLIB)
  {LUA_BITLIBNAME, luaopen_bit32},
#endif
  {NULL, NULL}
};

/* luaL_openlibs()函数用于加载lua的预加载库 */
LUALIB_API void luaL_openlibs (lua_State *L) {
  const luaL_Reg *lib;
  /* "require" functions from 'loadedlibs' and set results to global table */
  for (lib = loadedlibs; lib->func; lib++) {
  	/* 
    ** luaL_requiref()函数的作用就是将模块lib->name对应的table加入到_LOADED和_G
    ** 这两个table中，成为它们的子table。模块对应的table的创建可以看具体模块的
    ** 加载函数的实现，即下面的lib->func，比如math库，可以看luaopen_math()
    */
    luaL_requiref(L, lib->name, lib->func, 1);
	/* 
	** 从luaL_requiref()的实现我们可以知道，在执行完luaL_requiref()函数后，
	** 位于堆栈顶部的元素仍然是模块modname对应的table，因为这时候的这个table
	** 已经没有啥用了，于是下面就调用lua_pop()将其弹出堆栈。
	*/
    lua_pop(L, 1);  /* remove lib */
  }
}

