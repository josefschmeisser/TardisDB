# ---------------------------------------------------------------------------
# SQLPARSER
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Files
# ---------------------------------------------------------------------------

file(GLOB_RECURSE SRC_CC "tools/sqlParser/*.cpp")

# ---------------------------------------------------------------------------
# Libraries
# ---------------------------------------------------------------------------

add_library(sqlparser STATIC ${SRC_CC})