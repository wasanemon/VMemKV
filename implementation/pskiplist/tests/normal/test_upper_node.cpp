#include <doctest/doctest.h>

#include <pskiplist/pskiplist.hpp>

using pskiplist::allocate_upper_node;
using pskiplist::deallocate_upper_node;
using pskiplist::UpperNode;

TEST_CASE("a freshly allocated node has null forward pointers at every level") {
  UpperNode *node = allocate_upper_node(7, 5);
  CHECK(node->durable_offset == 7);
  CHECK(node->height == 5);
  for (int level = 0; level < 5; ++level) {
    CHECK(node->forwards[level].load() == nullptr);
  }
  deallocate_upper_node(node);
}

TEST_CASE("forward pointers at each level are independently settable") {
  UpperNode *a = allocate_upper_node(1, 3);
  UpperNode *b = allocate_upper_node(2, 1);

  a->forwards[0].store(b);
  a->forwards[2].store(a);  // a level can point back to the node itself; storage doesn't care

  CHECK(a->forwards[0].load() == b);
  CHECK(a->forwards[1].load() == nullptr);
  CHECK(a->forwards[2].load() == a);

  deallocate_upper_node(a);
  deallocate_upper_node(b);
}

TEST_CASE("a height-1 node works the same as any other height") {
  UpperNode *node = allocate_upper_node(99, 1);
  CHECK(node->height == 1);
  CHECK(node->forwards[0].load() == nullptr);
  deallocate_upper_node(node);
}
