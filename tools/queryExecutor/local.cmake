# ---------------------------------------------------------------------------
# SQLPARSER
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Files
# ---------------------------------------------------------------------------

file(GLOB_RECURSE SRC_CC "tools/queryExecutor/*.cpp")

# ---------------------------------------------------------------------------
# Libraries
# ---------------------------------------------------------------------------

add_library(queryExecutor STATIC ${SRC_CC})