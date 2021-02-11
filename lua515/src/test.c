#include "lauxlib.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>  

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

int main(int argc, char *argv[]) {
    char buf[80];   
    getcwd(buf,sizeof(buf));   
    printf("cwd: %s\n", buf);   
 
    lua_State *vm = luaL_newstate();
    if (NULL == vm) {
        return -1;
    }
    luaL_openlibs(vm);  /* open libraries */

   int ret = loadFile(vm, "../script/opcode.lua");
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

