# Copies SRC to DST when SRC exists, and succeeds silently when it does not.
#
# `cmake -E copy_if_different` treats a missing source as an error, which is wrong
# for the asset .meta sidecars: they are authored by `--index-assets` against the
# source tree, so a given asset may legitimately not have one yet on a fresh
# checkout. The build must carry them across when they are there and not fail when
# they are not.
#
# Invoked as:
#   cmake -DSRC=<path> -DDST=<path> -P copy_if_exists.cmake

if(NOT DEFINED SRC OR NOT DEFINED DST)
    message(FATAL_ERROR "copy_if_exists.cmake requires -DSRC= and -DDST=")
endif()

if(EXISTS "${SRC}")
    file(COPY_FILE "${SRC}" "${DST}" ONLY_IF_DIFFERENT)
endif()
