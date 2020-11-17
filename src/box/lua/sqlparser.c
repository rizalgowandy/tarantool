#include "execute.h"
#include "lua/utils.h"
#include "sqlInt.h"
#include "../sql/vdbe.h"	// FIXME
#include "../execute.h"		// FIXME
#include "../schema.h"		// FIXME
#include "../session.h"		// FIXME
#include "box/sql_stmt_cache.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

/**
 * Parse SQL to AST, return it as cdata
 * FIXME - split to the Lua and SQL parts..
 */
static int
lbox_sqlparser_parse(struct lua_State *L)
{
	size_t length;
	int top = lua_gettop(L);

	if (top != 1 || !lua_isstring(L, 1))
		return luaL_error(L, "Usage: sqlparser.parse(sqlstring)");

	const char *sql = lua_tolstring(L, 1, &length);

	uint32_t stmt_id = sql_stmt_calculate_id(sql, length);
	struct sql_stmt *stmt = sql_stmt_cache_find(stmt_id);

	if (stmt == NULL) {
		if (sql_stmt_compile(sql, length, NULL, &stmt, NULL) != 0)
			return -1;
		if (sql_stmt_cache_insert(stmt) != 0) {
			sql_stmt_finalize(stmt);
			goto error;
		}
	} else {
		if (sql_stmt_schema_version(stmt) != box_schema_version() &&
		    !sql_stmt_busy(stmt)) {
			; //if (sql_reprepare(&stmt) != 0)
			//	goto error;
		}
	}
	assert(stmt != NULL);
	/* Add id to the list of available statements in session. */
	if (!session_check_stmt_id(current_session(), stmt_id))
		session_add_stmt_id(current_session(), stmt_id);

	return 1;
error:
	return luaT_push_nil_and_error(L);
}

static int
lbox_sqlparser_unparse(struct lua_State *L)
{
	lua_pushliteral(L, "sqlparser.unparse");
	return 1;
}

static int
lbox_sqlparser_serialize(struct lua_State *L)
{
	lua_pushliteral(L, "sqlparser.serialize");
	return 1;
}

static int
lbox_sqlparser_deserialize(struct lua_State *L)
{
	lua_pushliteral(L, "sqlparser.deserialize");
	return 1;
}

void
box_lua_sqlparser_init(struct lua_State *L)
{
	static const struct luaL_Reg meta[] = {
		{ "parse", lbox_sqlparser_parse },
		{ "unparse", lbox_sqlparser_unparse },
		{ "serialize", lbox_sqlparser_serialize },
		{ "deserialize", lbox_sqlparser_deserialize },
		{ NULL, NULL },
	};
	luaL_register_module(L, "sqlparser", meta);
	lua_pop(L, 1);

	return;
}
