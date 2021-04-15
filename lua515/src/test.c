#include "lauxlib.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>  
#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"

int loadFile(lua_State *vm, char *path) {
    int ret = luaL_loadfile(vm, path);
    if (0 == ret) {
        return 0;
    }

    char reason[1024];
    const char *rea;
    size_t len;
    rea = luaL_checklstring(vm, -1, &len);
    printf("parse file(%s) fail, reason(%s)", rea);
    return -1;
}

int t_add(lua_State *L) {
    lua_Integer sum = 0;
    sum = lua_tointeger(L, -1) + lua_tointeger(L, -2);
    lua_pushinteger(L, sum);
    printf("sum(%d)", sum);
    return 1;
}

int main(int argc, char *argv[]) {
    char buf[80];   
    getcwd(buf,sizeof(buf));   
    printf("cwd: %s\n", buf);   
 
    lua_State *vm = luaL_newstate();
    if (NULL == vm) {
        return -1;
    }
    luaL_openlibs(vm);  /* open libraries */

    lua_pushcfunction(vm, t_add);
    lua_pushinteger(vm, 1);
    lua_pushinteger(vm, 2);
    lua_call(vm, 2, 1);
    lua_Integer sum;
    sum = lua_tointeger(vm, -1);

   int ret = 0;
   ret = loadFile(vm, "../script/opcode.lua");
   if (0 != ret) {
       printf("load file fail, ret(%d)", ret);
       return -2;
   }
   ret = lua_pcall(vm, 0, LUA_MULTRET, 0);
   if (0 != ret) {
       printf("call fail, ret(%d)", ret);
       return -2;
   }
}

