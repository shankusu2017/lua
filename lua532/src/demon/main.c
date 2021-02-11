/*
** $Id: lua.c,v 1.226 2015/08/14 19:11:20 roberto Exp $
** Lua stand-alone interpreter
** See Copyright Notice in lua.h
*/

#include "lprefix.h"

#include <stdio.h>
#include <stdlib.h>

#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"
#include <errno.h>
#include "conversion.h"
#include "operator.h"

static int handle_script (lua_State *L, char **argv) {
  const char *fname = "demon.lua";
  int ret = luaL_loadfile(L, fname);
  if (LUA_OK != ret) {
      printf("err:\n", ret);
      perror("load lua file fail err:\n");
      return ret;
  }

  ret = lua_pcall(L, 0, LUA_MULTRET, 0);
  if (LUA_OK != ret) {
       printf("call lua fail :%s\n", lua_tostring(L, -1));
       return ret;
  }

  //luaL_dofile(L, fname);
//  lua_getglobal(L, "width");
//  lua_getglobal(L, "height");
//  if (!lua_isnumber(L, -2))
//      perror("width should be a number\n");
//  if (!lua_isnumber(L, -1))
//      perror("height should be a number\n");

  int width = lua_tonumber(L, -2);
  int height = lua_tonumber(L, -1);

  return 0;
}

/* 自己写的测试函数 */
void test(lua_State *L)
{
   lua_pushnumber(L, 12345);
   lua_setglobal(L, "global_lua_var");
}

int main (int argc, char *argv[]) {
  //ope();
  //conver();
  lua_State *L = luaL_newstate();  /* create state */
  if (!L) return EXIT_FAILURE;
  luaL_openlibs(L);  /* open standard libraries */
  //index2addr();
  int t = -1;
  unsigned t2 = t;
  int t3 = t2;
  float d4 = t2;
  handle_script(L, argv);

  printf("enter any char to exit program!\n");
  int c = getchar();
  //test(L);

  lua_close(L);
  return 0;
}

