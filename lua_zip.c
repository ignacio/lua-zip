#include <lauxlib.h>
#include <lua.h>
#include <zip.h>
#include <assert.h>
#include <errno.h>

#define ARCHIVE_MT "zip{archive}"
#define ARCHIVE_FILE_MT "zip{archive.file}"
#define WEAK_MT "zip{weak}"

#define check_archive(L, narg)                                   \
    ((struct zip**)luaL_checkudata((L), (narg), ARCHIVE_MT))

#define check_archive_file(L, narg)                                   \
    ((struct zip_file**)luaL_checkudata((L), (narg), ARCHIVE_FILE_MT))

#define absindex(L,i) ((i)>0?(i):lua_gettop(L)+(i)+1)

/* If zip_error is non-zero, then push an appropriate error message
 * onto the top of the Lua stack and return zip_error.  Otherwise,
 * just return 0.
 */
static int S_push_error(lua_State* L, int zip_error, int sys_error) {
    char buff[1024];
    if ( 0 == zip_error ) return 0;

    int len = zip_error_to_str(buff, sizeof(buff), zip_error, sys_error);
    if ( len >= sizeof(buff) ) len = sizeof(buff)-1;
    lua_pushlstring(L, buff, len);

    return zip_error;
}

static int S_archive_open(lua_State* L) {
    const char*  path  = luaL_checkstring(L, 1);
    int          flags = (lua_gettop(L) < 2) ? 0 : luaL_checkint(L, 2); 
    struct zip** ar    = (struct zip**)lua_newuserdata(L, sizeof(struct zip*));
    int          err   = 0;

    *ar = zip_open(path, flags, &err);

    if ( ! *ar ) {
        assert(err);
        S_push_error(L, err, errno);
        lua_pushnil(L);
        lua_insert(L, -2);
        return 2;
    }

    luaL_getmetatable(L, ARCHIVE_MT);
    assert(!lua_isnil(L, -1)/* ARCHIVE_MT found? */);

    lua_setmetatable(L, -2);

    /* Each archive has a weak table of objects that are invalidated
     * when the archive is closed or garbage collected.
     */
    lua_newtable(L);
    luaL_getmetatable(L, WEAK_MT);
    assert(!lua_isnil(L, -1)/* WEAK_MT found? */);

    lua_setmetatable(L, -2);

    lua_setfenv(L, -2);

    return 1;
}

/* Iterate over the fenv of ar_idx and call the __gc metamethod to
 * assert that these objects are invalidated.  This functionality is
 * necessary since the lifetime of these "child" objects is dependent
 * on the archive "parent" object still existing.
 */
static void S_archive_gc_refs(lua_State* L, int ar_idx) {
    lua_getfenv(L, ar_idx);
    assert(lua_istable(L, -1) /* fenv of archive must exist! */);

    lua_pushnil(L);
    while ( lua_next(L, -2) != 0 ) {
        if ( luaL_callmeta(L, -2, "__gc") ) {
            lua_pop(L, 2);
        } else {
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1); /* Pop the fenv */
}

/* Adds a reference from the archive at ar_idx to the object at
 * obj_idx.  Adding this reference
 */
static void S_archive_add_ref(lua_State* L, int ar_idx, int obj_idx) {
    obj_idx = absindex(L, obj_idx);
    lua_getfenv(L, ar_idx);
    assert(lua_istable(L, -1) /* fenv of archive must exist! */);

    lua_pushvalue(L, obj_idx);
    lua_pushboolean(L, 1);
    lua_settable(L, -3);

    lua_pop(L, 1); /* Pop the fenv */
}

/* Explicitly close the archive, throwing an error if there are any
 * problems.
 */
static int S_archive_close(lua_State* L) {
    struct zip** ar  = check_archive(L, 1);
    int          err;

    if ( ! *ar ) return 0;

    S_archive_gc_refs(L, 1);

    err = zip_close(*ar);
    *ar = NULL;

    if ( S_push_error(L, err, errno) ) lua_error(L);


    return 0;
}

/* Try to revert all changes and close the archive since the archive
 * was not explicitly closed.
 */
static int S_archive_gc(lua_State* L) {
    struct zip** ar = check_archive(L, 1);

    if ( ! *ar ) return 0;

    S_archive_gc_refs(L, 1);

    zip_unchange_all(*ar);
    zip_close(*ar);

    *ar = NULL;

    return 0;
}

static int S_archive_get_num_files(lua_State* L) {
    struct zip** ar = check_archive(L, 1);

    if ( ! *ar ) return 0;

    lua_pushinteger(L, zip_get_num_files(*ar));

    return 1;
}

static int S_archive_name_locate(lua_State* L) {
    struct zip** ar    = check_archive(L, 1);
    const char*  fname = luaL_checkstring(L, 2);
    int          flags = (lua_gettop(L) < 3) ? 0 : luaL_checkint(L, 3);
    int          idx;

    if ( ! *ar ) return 0;

    idx = zip_name_locate(*ar, fname, flags);

    if ( idx < 0 ) {
        int zip_err, sys_err;
        zip_error_get(*ar, &zip_err, &sys_err);
        lua_pushnil(L);
        S_push_error(L, zip_err, sys_err);
        return 2;
    }

    lua_pushinteger(L, idx+1);
    return 1;
}

static int S_archive_file_open(lua_State* L) {
    struct zip** ar        = check_archive(L, 1);
    const char*  path      = (lua_isnumber(L, 2)) ? NULL : luaL_checkstring(L, 2);
    int          path_idx  = (lua_isnumber(L, 2)) ? luaL_checkint(L, 2)-1 : -1;
    int          flags     = (lua_gettop(L) < 3)  ? 0    : luaL_checkint(L, 3); 
    struct zip_file** file = (struct zip_file**)
        lua_newuserdata(L, sizeof(struct zip_file*));

    *file = NULL;

    if ( ! *ar ) return 0;

    if ( NULL == path ) {
        *file = zip_fopen_index(*ar, path_idx, flags);
    } else {
        *file = zip_fopen(*ar, path, flags);
    }

    if ( ! *file ) {
        int zip_err, sys_err;
        zip_error_get(*ar, &zip_err, &sys_err);
        lua_pushnil(L);
        S_push_error(L, zip_err, sys_err);
        return 2;
    }

    luaL_getmetatable(L, ARCHIVE_FILE_MT);
    assert(!lua_isnil(L, -1)/* ARCHIVE_FILE_MT found? */);

    lua_setmetatable(L, -2);

    S_archive_add_ref(L, 1, -1);

    return 1;
}

static int S_archive_file_close(lua_State* L) {
    struct zip_file** file = check_archive_file(L, 1);
    int err;

    if ( ! *file ) return 0;

    err = zip_fclose(*file);
    *file = NULL;

    if ( err ) {
        S_push_error(L, err, errno);
        lua_error(L);
    }

    return 0;
}

static int S_archive_file_gc(lua_State* L) {
    struct zip_file** file = check_archive_file(L, 1);

    if ( ! *file ) return 0;

    zip_fclose(*file);
    *file = NULL;

    return 0;
}

static int S_archive_file_read(lua_State* L) {
    struct zip_file** file = check_archive_file(L, 1);
    int               len  = luaL_checkint(L, 2);
    char*             buff;

    if ( len <= 0 ) luaL_argerror(L, 2, "Must be > 0");

    if ( ! *file ) return 0;

    buff = (char*)lua_newuserdata(L, len);

    len = zip_fread(*file, buff, len);

    if ( -1 == len ) {
        lua_pushnil(L);
        lua_pushstring(L, zip_file_strerror(*file));
        return 2;
    }

    lua_pushlstring(L, buff, len);
    return 1;
}

static void S_register_archive(lua_State* L) {
    luaL_newmetatable(L, ARCHIVE_MT);

    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, S_archive_gc);
    lua_setfield(L, -2, "__gc");

    lua_pushcfunction(L, S_archive_get_num_files);
    lua_setfield(L, -2, "__len");

    lua_pushcfunction(L, S_archive_close);
    lua_setfield(L, -2, "close");

    lua_pushcfunction(L, S_archive_get_num_files);
    lua_setfield(L, -2, "get_num_files");

    lua_pushcfunction(L, S_archive_name_locate);
    lua_setfield(L, -2, "name_locate");

    lua_pushcfunction(L, S_archive_file_open);
    lua_setfield(L, -2, "open");

    lua_pop(L, 1);
}

static void S_register_archive_file(lua_State* L) {
    luaL_newmetatable(L, ARCHIVE_FILE_MT);

    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, S_archive_file_close);
    lua_setfield(L, -2, "close");

    lua_pushcfunction(L, S_archive_file_gc);
    lua_setfield(L, -2, "__gc");

    lua_pushcfunction(L, S_archive_file_read);
    lua_setfield(L, -2, "read");

    lua_pop(L, 1);
}

static void S_register_weak(lua_State* L) {
    luaL_newmetatable(L, WEAK_MT);

    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");

    lua_pushliteral(L, "k");
    lua_setfield(L, -2, "__mode");

    lua_pop(L, 1);
}

static int S_OR(lua_State* L) {
    int result = 0;
    int top = lua_gettop(L);
    while ( top ) {
        result |= luaL_checkint(L, top--);
    }
    lua_pushinteger(L, result);
    return 1;
}

LUALIB_API int luaopen_zip(lua_State* L) {
    static luaL_reg fns[] = {
        { "open",     S_archive_open },
        { "OR",       S_OR },
        { NULL, NULL }
    };

    lua_newtable(L);
    luaL_register(L, NULL, fns);

#define EXPORT_CONSTANT(NAME) \
    lua_pushnumber(L, ZIP_##NAME); \
    lua_setfield(L, -2, #NAME)

    EXPORT_CONSTANT(CREATE);
    EXPORT_CONSTANT(EXCL);
    EXPORT_CONSTANT(CHECKCONS);
    EXPORT_CONSTANT(FL_NOCASE);
    EXPORT_CONSTANT(FL_NODIR);
    EXPORT_CONSTANT(FL_COMPRESSED);
    EXPORT_CONSTANT(FL_UNCHANGED);

    S_register_archive(L);
    S_register_archive_file(L);
    S_register_weak(L);

    return 1;
}
