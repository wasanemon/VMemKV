#include <doctest/doctest.h>

#include <pskiplist/pskiplist.hpp>

using pskiplist::allocate_upper_node;
using pskiplist::deallocate_upper_node;
using pskiplist::marked_ptr_marked;
using pskiplist::marked_ptr_value;
using pskiplist::pack_marked_ptr;
using pskiplist::UpperNode;

TEST_CASE("a freshly allocated node has null, unmarked forward pointers at every level") {
  UpperNode *node = allocate_upper_node(7, 5);
  CHECK(node->durable_offset == 7);
  CHECK(node->height == 5);
  CHECK(node->levels_remaining.load() == 5);
  for (int level = 0; level < 5; ++level) {
    const uint64_t raw = node->forwards[level].load();
    CHECK(marked_ptr_value<UpperNode>(raw) == nullptr);
    CHECK_FALSE(marked_ptr_marked(raw));
  }
  deallocate_upper_node(node);
}

TEST_CASE("forward pointers at each level are independently settable") {
  UpperNode *a = allocate_upper_node(1, 3);
  UpperNode *b = allocate_upper_node(2, 1);

  a->forwards[0].store(pack_marked_ptr(b, false));
  a->forwards[2].store(pack_marked_ptr(a, false));  // a level can point back to the node itself

  CHECK(marked_ptr_value<UpperNode>(a->forwards[0].load()) == b);
  CHECK(marked_ptr_value<UpperNode>(a->forwards[1].load()) == nullptr);
  CHECK(marked_ptr_value<UpperNode>(a->forwards[2].load()) == a);

  deallocate_upper_node(a);
  deallocate_upper_node(b);
}

TEST_CASE("the mark bit is independent of the pointer value") {
  UpperNode *a = allocate_upper_node(1, 1);
  UpperNode *b = allocate_upper_node(2, 1);

  a->forwards[0].store(pack_marked_ptr(b, true));

  CHECK(marked_ptr_value<UpperNode>(a->forwards[0].load()) == b);
  CHECK(marked_ptr_marked(a->forwards[0].load()));

  deallocate_upper_node(a);
  deallocate_upper_node(b);
}

TEST_CASE("a height-1 node works the same as any other height") {
  UpperNode *node = allocate_upper_node(99, 1);
  CHECK(node->height == 1);
  CHECK(node->levels_remaining.load() == 1);
  CHECK(marked_ptr_value<UpperNode>(node->forwards[0].load()) == nullptr);
  deallocate_upper_node(node);
}
