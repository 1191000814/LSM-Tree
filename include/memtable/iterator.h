#pragma once

#include "../skiplist/skiplist.h"
#include "memtable.h"
#include <queue>

struct SearchItem {
  std::string key;
  std::string value;
  int mem_idx;
};

bool operator<(const SearchItem &a, const SearchItem &b);
bool operator>(const SearchItem &a, const SearchItem &b);
bool operator==(const SearchItem &a, const SearchItem &b);

class MemTableIterator {
public:
  MemTableIterator();
  MemTableIterator(const MemTable &memtable);
  std::pair<std::string, std::string> operator*() const;
  MemTableIterator &operator++();
  MemTableIterator operator++(int);
  bool operator==(const MemTableIterator &other) const;
  bool operator!=(const MemTableIterator &other) const;

private:
  std::priority_queue<SearchItem, std::vector<SearchItem>,
                      std::greater<SearchItem>>
      items;
};