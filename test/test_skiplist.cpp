#include "../include/skiplist/skiplist.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <latch>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

// 测试基本插入、查找和删除
TEST(SkipListTest, BasicOperations) {
  SkipList skipList;

  // 测试插入和查找
  skipList.put("key1", "value1");
  EXPECT_EQ(skipList.get("key1").value(), "value1");

  // 测试更新
  skipList.put("key1", "new_value");
  EXPECT_EQ(skipList.get("key1").value(), "new_value");

  // 测试删除
  skipList.remove("key1");
  EXPECT_FALSE(skipList.get("key1").has_value());
}

// 测试迭代器
TEST(SkipListTest, Iterator) {
  SkipList skipList;
  skipList.put("key1", "value1");
  skipList.put("key2", "value2");
  skipList.put("key3", "value3");

  // 测试迭代器
  std::vector<std::pair<std::string, std::string>> result;
  for (auto it = skipList.begin(); it != skipList.end(); ++it) {
    result.push_back(*it);
  }

  EXPECT_EQ(result.size(), 3);
  EXPECT_EQ(result[0].first, "key1");
  EXPECT_EQ(result[1].first, "key2");
  EXPECT_EQ(result[2].first, "key3");
}

// 测试大量数据插入和查找
TEST(SkipListTest, LargeScaleInsertAndGet) {
  SkipList skipList;
  const int num_elements = 10000;

  // 插入大量数据
  for (int i = 0; i < num_elements; ++i) {
    std::string key = "key" + std::to_string(i);
    std::string value = "value" + std::to_string(i);
    skipList.put(key, value);
  }

  // 验证插入的数据
  for (int i = 0; i < num_elements; ++i) {
    std::string key = "key" + std::to_string(i);
    std::string expected_value = "value" + std::to_string(i);
    EXPECT_EQ(skipList.get(key).value(), expected_value);
  }
}

// 测试大量数据删除
TEST(SkipListTest, LargeScaleRemove) {
  SkipList skipList;
  const int num_elements = 10000;

  // 插入大量数据
  for (int i = 0; i < num_elements; ++i) {
    std::string key = "key" + std::to_string(i);
    std::string value = "value" + std::to_string(i);
    skipList.put(key, value);
  }

  // 删除所有数据
  for (int i = 0; i < num_elements; ++i) {
    std::string key = "key" + std::to_string(i);
    skipList.remove(key);
  }

  // 验证所有数据已被删除
  for (int i = 0; i < num_elements; ++i) {
    std::string key = "key" + std::to_string(i);
    EXPECT_FALSE(skipList.get(key).has_value());
  }
}

// 测试重复插入
TEST(SkipListTest, DuplicateInsert) {
  SkipList skipList;

  // 重复插入相同的key
  skipList.put("key1", "value1");
  skipList.put("key1", "value2");
  skipList.put("key1", "value3");

  // 验证最后一次插入的值
  EXPECT_EQ(skipList.get("key1").value(), "value3");
}

// 测试空跳表
TEST(SkipListTest, EmptySkipList) {
  SkipList skipList;

  // 验证空跳表的查找和删除
  EXPECT_FALSE(skipList.get("nonexistent_key").has_value());
  skipList.remove("nonexistent_key"); // 删除不存在的key
}

// 测试随机插入和删除
TEST(SkipListTest, RandomInsertAndRemove) {
  SkipList skipList;
  std::unordered_set<std::string> keys;
  const int num_operations = 10000;

  for (int i = 0; i < num_operations; ++i) {
    std::string key = "key" + std::to_string(rand() % 1000);
    std::string value = "value" + std::to_string(rand() % 1000);

    if (keys.find(key) == keys.end()) {
      // 插入新key
      skipList.put(key, value);
      keys.insert(key);
    } else {
      // 删除已存在的key
      skipList.remove(key);
      keys.erase(key);
    }

    // 验证当前状态
    if (keys.find(key) != keys.end()) {
      EXPECT_EQ(skipList.get(key).value(), value);
    } else {
      EXPECT_FALSE(skipList.get(key).has_value());
    }
  }
}

// 测试内存大小跟踪
TEST(SkipListTest, MemorySizeTracking) {
  SkipList skipList;

  // 插入数据
  skipList.put("key1", "value1");
  skipList.put("key2", "value2");

  // 验证内存大小
  size_t expected_size = sizeof("key1") - 1 + sizeof("value1") - 1 +
                         sizeof("key2") - 1 + sizeof("value2") - 1;
  EXPECT_EQ(skipList.get_size(), expected_size);

  // 删除数据
  skipList.remove("key1");
  expected_size -= sizeof("key1") - 1 + sizeof("value1") - 1;
  EXPECT_EQ(skipList.get_size(), expected_size);

  skipList.clear();
  EXPECT_EQ(skipList.get_size(), 0);
}

// 测试跳表的并发性能
TEST(SkipListTest, ConcurrentOperations) {
  SkipList skipList;
  const int num_readers = 4;       // 读线程数
  const int num_writers = 2;       // 写线程数
  const int num_operations = 1000; // 每个线程的操作数

  // 用于同步所有线程的开始
  std::atomic<bool> start{false};
  // 用于等待所有线程完成
  std::latch completion_latch((num_readers + num_writers));

  // 记录写入的键，用于验证
  std::vector<std::string> inserted_keys;
  std::mutex keys_mutex;

  // 写线程函数
  auto writer_func = [&](int thread_id) {
    // 等待开始信号
    while (!start) {
      std::this_thread::yield();
    }

    // 执行写操作
    for (int i = 0; i < num_operations; ++i) {
      std::string key =
          "key_" + std::to_string(thread_id) + "_" + std::to_string(i);
      std::string value =
          "value_" + std::to_string(thread_id) + "_" + std::to_string(i);

      if (i % 2 == 0) {
        // 插入操作
        skipList.put(key, value);
        {
          std::lock_guard<std::mutex> lock(keys_mutex);
          inserted_keys.push_back(key);
        }
      } else {
        // 删除操作
        skipList.remove(key);
      }

      // 随机休眠一小段时间，模拟实际工作负载
      std::this_thread::sleep_for(std::chrono::microseconds(rand() % 100));
    }

    completion_latch.count_down();
  };

  // 读线程函数
  auto reader_func = [&](int thread_id) {
    // 等待开始信号
    while (!start) {
      std::this_thread::yield();
    }

    int found_count = 0;
    // 执行读操作
    for (int i = 0; i < num_operations; ++i) {
      // 随机选择一个已插入的key进行查询
      std::string key_to_find;
      {
        std::lock_guard<std::mutex> lock(keys_mutex);
        if (!inserted_keys.empty()) {
          key_to_find = inserted_keys[rand() % inserted_keys.size()];
        }
      }

      if (!key_to_find.empty()) {
        auto result = skipList.get(key_to_find);
        if (result.has_value()) {
          found_count++;
        }
      }

      // 每隔一段时间进行一次遍历操作
      if (i % 100 == 0) {
        std::vector<std::pair<std::string, std::string>> items;
        for (auto it = skipList.begin(); it != skipList.end(); ++it) {
          items.push_back(*it);
        }
      }

      std::this_thread::sleep_for(std::chrono::microseconds(rand() % 50));
    }

    completion_latch.count_down();
  };

  // 创建并启动写线程
  std::vector<std::thread> writers;
  for (int i = 0; i < num_writers; ++i) {
    writers.emplace_back(writer_func, i);
  }

  // 创建并启动读线程
  std::vector<std::thread> readers;
  for (int i = 0; i < num_readers; ++i) {
    readers.emplace_back(reader_func, i);
  }

  // 给线程一点时间进入等待状态
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // 记录开始时间
  auto start_time = std::chrono::high_resolution_clock::now();

  // 发送开始信号
  start = true;

  // 等待所有线程完成
  completion_latch.wait();

  // 记录结束时间
  auto end_time = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      end_time - start_time);

  // 等待所有线程结束
  for (auto &w : writers) {
    w.join();
  }
  for (auto &r : readers) {
    r.join();
  }

  // 验证跳表的最终状态
  size_t final_size = 0;
  for (auto it = skipList.begin(); it != skipList.end(); ++it) {
    final_size++;
  }

  //   std::cout << "Concurrent test completed in " << duration.count()
  //             << "ms\nFinal skiplist size: " << final_size << std::endl;

  // 基本正确性检查
  EXPECT_GT(final_size, 0); // 跳表不应该为空
  EXPECT_LE(final_size,
            num_writers * num_operations); // 跳表大小不应超过最大可能值
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}