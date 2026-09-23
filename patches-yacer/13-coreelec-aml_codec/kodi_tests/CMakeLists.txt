# Tests for the AML codec group.
#
# CAMLSeekPreroll is header-only and depends on nothing, so this needs no
# production translation units, no stand-ins and no seams at all.

yacer_add_test(
  NAME AMLSeekPreroll
  SOURCES TestAMLSeekPreroll.cpp
)
