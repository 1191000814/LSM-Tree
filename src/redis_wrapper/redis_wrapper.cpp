#include "../../include/redis_wrapper/redis_wrapper.h"
#include "../../include/consts.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// Helper functions
RedisWrapper::RedisWrapper(const std::string &db_path) {
  this->lsm = std::make_unique<LSM>(db_path);
}

std::vector<std::string>
get_fileds_from_hash_value(const std::optional<std::string> &field_list_opt) {
  std::string field_list = field_list_opt.value_or("");
  if (!field_list.empty()) {
    // 去除前缀后才是字段列表
    std::string preffix = REDIS_HASH_VALUE_PREFFIX;
    field_list =
        field_list.substr(preffix.size(), field_list.size() - preffix.size());
  }
  std::vector<std::string> fields;
  std::istringstream iss(field_list);
  std::string token;
  while (std::getline(iss, token, REDIS_FIELD_SEPARATOR)) {
    fields.push_back(token);
  }
  return fields;
}

std::string get_hash_value_from_fields(const std::vector<std::string> &fields) {
  std::ostringstream oss;
  oss << REDIS_HASH_VALUE_PREFFIX;
  for (size_t i = 0; i < fields.size(); ++i) {
    if (i > 0)
      oss << REDIS_FIELD_SEPARATOR;
    oss << fields[i];
  }
  return oss.str();
}

inline std::string get_hash_filed_key(const std::string &key,
                                      const std::string &field) {
  return REDIS_FIELD_PREFIX + key + "_" + field;
}

inline bool is_value_hash(const std::string &key) {
  return key.find(REDIS_HASH_VALUE_PREFFIX) == 0;
}

inline std::string get_explire_key(const std::string &key) {
  return REDIS_EXPIRE_HEADER + key;
}

std::string get_zset_key_socre(const std::string &key,
                               const std::string &score) {
  std::ostringstream oss;
  oss << std::setw(REDIS_SORTED_SET_SCORE_LEN) << std::setfill('0') << score;

  std::string formatted_score = oss.str();

  std::string res = REDIS_SORTED_SET_PREFIX + key + "_SCORE_" + formatted_score;
  return res;
}

inline std::string get_zset_key_elem(const std::string &key,
                                     const std::string &elem) {
  return REDIS_SORTED_SET_PREFIX + key + "_ELEM_" + elem;
}

inline std::string get_zset_key_preffix(const std::string &key) {
  return REDIS_SORTED_SET_PREFIX + key + "_";
}

inline std::string get_zset_score_preffix(const std::string &key) {
  return REDIS_SORTED_SET_PREFIX + key + "_SCORE_";
}

inline std::string get_zset_elem_preffix(const std::string &key) {
  return REDIS_SORTED_SET_PREFIX + key + "_ELEM_";
}

inline std::string get_zset_score_item(const std::string &key) {
  // 定义 _SCORE_ 的前缀
  const std::string score_prefix = "_SCORE_";

  // 找到 _SCORE_ 的位置
  size_t score_pos = key.find(score_prefix);

  // 如果找到了 _SCORE_，则返回其右边的部分；否则返回空字符串
  if (score_pos != std::string::npos) {
    return key.substr(score_pos + score_prefix.size());
  } else {
    return "";
  }
}

bool is_expired(const std::optional<std::string> &expire_str,
                std::time_t *now_time) {
  if (!expire_str.has_value()) {
    return false;
  }
  auto now = std::chrono::system_clock::now();
  auto now_time_t = std::chrono::system_clock::to_time_t(now);

  if (now_time != nullptr) {
    *now_time = now_time_t;
  }

  // 检查是否过期
  return (std::stoll(expire_str.value()) < now_time_t);
}

std::string get_expire_time(const std::string &seconds_count) {
  // 获取当前时间戳, 以秒为单位
  auto now = std::chrono::system_clock::now();
  auto now_time_t = std::chrono::system_clock::to_time_t(now);

  auto time_add = std::stoll(seconds_count);

  // 将时间戳转换为字符串
  std::string expire_time_str = std::to_string(now_time_t + time_add);
  return expire_time_str;
}

std::vector<std::string> split(const std::string &str, char delimiter) {
  std::vector<std::string> tokens;
  std::istringstream iss(str);
  std::string token;
  while (std::getline(iss, token, delimiter)) {
    tokens.push_back(token);
  }
  return tokens;
}

std::string join(const std::vector<std::string> &elements, char delimiter) {
  std::ostringstream oss;
  for (size_t i = 0; i < elements.size(); ++i) {
    if (i > 0) {
      oss << delimiter;
    }
    oss << elements[i];
  }
  return oss.str();
}

// ************************ Redis *************************
bool RedisWrapper::expire_hash_clean(
    const std::string &key, std::shared_lock<std::shared_mutex> &rlock) {
  std::string expire_key = get_explire_key(key);
  auto expire_query = this->lsm->get(expire_key);

  if (is_expired(expire_query, nullptr)) {
    // 整个哈希数据结构都过期了, 需要删除所有的字段
    // 先升级锁
    rlock.unlock();                                       // 解锁读锁
    std::unique_lock<std::shared_mutex> wlock(redis_mtx); // 写锁
    auto fileds = get_fileds_from_hash_value(lsm->get(key));
    for (const auto &field : fileds) {
      std::string field_key = get_hash_filed_key(key, field);
      lsm->remove(field_key);
    }
    lsm->remove(key);
    lsm->remove(expire_key);
    return true;
  }
  return false;
}

bool RedisWrapper::expire_list_clean(
    const std::string &key, std::shared_lock<std::shared_mutex> &rlock) {
  std::string expire_key = get_explire_key(key);
  auto expire_query = this->lsm->get(expire_key);
  if (is_expired(expire_query, nullptr)) {
    // 链表都过期了, 需要删除链表
    // 先升级锁
    rlock.unlock();                                       // 解锁读锁
    std::unique_lock<std::shared_mutex> wlock(redis_mtx); // 写锁
    lsm->remove(key);
    lsm->remove(expire_key);
    return true;
  }
  return false;
}

bool RedisWrapper::expire_zset_clean(
    const std::string &key, std::shared_lock<std::shared_mutex> &rlock) {
  std::string expire_key = get_explire_key(key);
  auto expire_query = this->lsm->get(expire_key);
  if (is_expired(expire_query, nullptr)) {
    // 都过期了, 需要删除zset
    // 先升级锁
    rlock.unlock();                                       // 解锁读锁
    std::unique_lock<std::shared_mutex> wlock(redis_mtx); // 写锁
    lsm->remove(key);
    lsm->remove(expire_key);
    auto preffix = get_zset_key_preffix(key);
    auto result_elem = this->lsm->lsm_iters_monotony_predicate(
        [&preffix](const std::string &elem) {
          return -elem.compare(0, preffix.size(), preffix);
        });
    if (result_elem.has_value()) {
      auto [elem_begin, elem_end] = result_elem.value();
      std::vector<std::string> remove_vec;
      for (; elem_begin != elem_end; ++elem_begin) {
        remove_vec.push_back(elem_begin->first);
      }
      lsm->remove_batch(remove_vec);
    }
    return true;
  }
  return false;
}

// ************************* Redis Command *************************
// 基础操作
std::string RedisWrapper::set(std::vector<std::string> &args) {
  return redis_set(args[1], args[2]);
}
std::string RedisWrapper::get(std::vector<std::string> &args) {
  return redis_get(args[1]);
}
std::string RedisWrapper::del(std::vector<std::string> &args) {
  return redis_del(args);
}
std::string RedisWrapper::incr(std::vector<std::string> &args) {
  return redis_incr(args[1]);
}

std::string RedisWrapper::decr(std::vector<std::string> &args) {
  return redis_decr(args[1]);
}

std::string RedisWrapper::expire(std::vector<std::string> &args) {
  return redis_expire(args[1], args[2]);
}

std::string RedisWrapper::ttl(std::vector<std::string> &args) {
  return redis_ttl(args[1]);
}

// 哈希操作
std::string RedisWrapper::hset(std::vector<std::string> &args) {
  return redis_hset(args[1], args[2], args[3]);
}

std::string RedisWrapper::hget(std::vector<std::string> &args) {
  return redis_hget(args[1], args[2]);
}

std::string RedisWrapper::hdel(std::vector<std::string> &args) {
  return redis_hdel(args[1], args[2]);
}

std::string RedisWrapper::hkeys(std::vector<std::string> &args) {
  return redis_hkeys(args[1]);
}

// 链表操作
std::string RedisWrapper::lpush(std::vector<std::string> &args) {
  return redis_lpush(args[1], args[2]);
}
std::string RedisWrapper::rpush(std::vector<std::string> &args) {
  return redis_rpush(args[1], args[2]);
}
std::string RedisWrapper::lpop(std::vector<std::string> &args) {
  return redis_lpop(args[1]);
}
std::string RedisWrapper::rpop(std::vector<std::string> &args) {
  return redis_rpop(args[1]);
}
std::string RedisWrapper::llen(std::vector<std::string> &args) {
  return redis_llen(args[1]);
}
std::string RedisWrapper::lrange(std::vector<std::string> &args) {
  int start = std::stoi(args[2]);
  int end = std::stoi(args[3]);

  return redis_lrange(args[1], start, end);
}

// 有序集合操作
std::string RedisWrapper::zadd(std::vector<std::string> &args) {
  return redis_zadd(args);
}

std::string RedisWrapper::zrem(std::vector<std::string> &args) {
  return redis_zrem(args);
}

std::string RedisWrapper::zrange(std::vector<std::string> &args) {
  return redis_zrange(args);
}

std::string RedisWrapper::zcard(std::vector<std::string> &args) {

  return redis_zcard(args[1]);
}

std::string RedisWrapper::zscore(std::vector<std::string> &args) {
  return redis_zscore(args[1], args[2]);
}
std::string RedisWrapper::zincrby(std::vector<std::string> &args) {
  return redis_zincrby(args[1], args[2], args[3]);
}

std::string RedisWrapper::zrank(std::vector<std::string> &args) {
  return redis_zrank(args[1], args[2]);
}

void RedisWrapper::clear() { this->lsm->clear(); }
void RedisWrapper::flushall() { this->lsm->flush(); }

// *********************** Redis ***********************
// 基础操作
std::string RedisWrapper::redis_incr(const std::string &key) {
  // 该操作需要原子性
  std::unique_lock<std::shared_mutex> lock(redis_mtx); // 写锁
  auto original_vale = this->lsm->get(key);
  if (!original_vale.has_value()) {
    this->lsm->put(key, "1");
    return "1";
  }
  auto new_value = std::to_string(std::stoll(original_vale.value()) + 1);
  this->lsm->put(key, new_value);
  return new_value;
}

std::string RedisWrapper::redis_del(std::vector<std::string> &args) {
  std::unique_lock<std::shared_mutex> lock(redis_mtx); // 写锁
  int del_count = 0;
  for (int idx = 1; idx < args.size(); idx++) {
    std::string cur_key = args[idx];
    auto cur_value = this->lsm->get(cur_key);

    if (cur_value.has_value()) {
      // 需要判断这个key的value是不是哈希类型
      if (is_value_hash(cur_value.value())) {
        // 如果是哈希类型, 需要删除所有字段
        auto field_list = get_fileds_from_hash_value(cur_value);
        for (const auto &field : field_list) {
          std::string field_key = get_hash_filed_key(cur_key, field);
          this->lsm->remove(field_key);
        }
      }
      this->lsm->remove(cur_key);
      del_count++;
    }
    std::string expire_key = get_explire_key(cur_key);
    if (this->lsm->get(expire_key).has_value()) {
      this->lsm->remove(expire_key);
    }
  }
  return ":" + std::to_string(del_count) + "\r\n";
}

std::string RedisWrapper::redis_decr(const std::string &key) {
  // 该操作需要原子性
  std::unique_lock<std::shared_mutex> lock(redis_mtx); // 写锁
  auto original_vale = this->lsm->get(key);
  if (!original_vale.has_value()) {
    this->lsm->put(key, "-1");
    return "-1";
  }
  auto new_value = std::to_string(std::stoll(original_vale.value()) - 1);
  this->lsm->put(key, new_value);
  return new_value;
}

std::string RedisWrapper::redis_expire(const std::string &key,
                                       std::string seconds_count) {
  std::unique_lock<std::shared_mutex> lock(redis_mtx); // 写锁
  std::string expire_key = get_explire_key(key);

  auto expire_time_str = get_expire_time(seconds_count);

  // 存储过期时间
  this->lsm->put(expire_key, expire_time_str);

  return ":1\r\n";
}

std::string RedisWrapper::redis_set(std::string &key, std::string &value) {
  std::unique_lock<std::shared_mutex> lock(redis_mtx); // 写锁
  this->lsm->put(key, value);
  // 同时如果设置有过期时间, 需要删除过期时间
  std::string expire_key = get_explire_key(key);
  if (this->lsm->get(expire_key).has_value()) {
    this->lsm->remove(expire_key);
  }
  return "+OK\r\n";
}

std::string RedisWrapper::redis_get(std::string &key) {
  std::shared_lock<std::shared_mutex> rlock(redis_mtx); // 读锁

  auto key_query = this->lsm->get(key);

  std::string expire_key = get_explire_key(key);
  auto expire_query = this->lsm->get(expire_key);

  if (key_query.has_value()) {
    // 还需要检测 TTL
    if (expire_query.has_value()) {

      // 检查是否过期
      if (is_expired(expire_query, nullptr)) {
        // 过期了, 先进行锁升级, 再清理数据
        rlock.unlock();                                       // 解锁读锁
        std::unique_lock<std::shared_mutex> wlock(redis_mtx); // 写锁
        this->lsm->remove(key);
        this->lsm->remove(expire_key);
        return "$-1\r\n";
      } else {
        // 没有过期
        return "$" + std::to_string(key_query.value().size()) + "\r\n" +
               key_query.value() + "\r\n";
      }
    } else {
      // 没有设置过期时间, 直接返回
      return "$" + std::to_string(key_query.value().size()) + "\r\n" +
             key_query.value() + "\r\n";
    }
  } else {
    // key 不存在, 有必要的话清理 expire_key
    if (expire_query.has_value()) {
      // 先进行锁升级, 再清理数据
      rlock.unlock();                                       // 解锁读锁
      std::unique_lock<std::shared_mutex> wlock(redis_mtx); // 写锁
      this->lsm->remove(expire_key);
    }
  }
  return "$-1\r\n"; // 表示键不存在
}

std::string RedisWrapper::redis_ttl(std::string &key) {
  std::shared_lock<std::shared_mutex> lock(redis_mtx); // 读锁

  auto key_query = this->lsm->get(key);

  std::string expire_key = get_explire_key(key);
  auto expire_query = this->lsm->get(expire_key);

  if (key_query.has_value()) {
    // key 存在, 判断是否过期
    if (expire_query.has_value()) {
      std::time_t now_time_t;
      // 检查是否过期
      if (is_expired(expire_query, &now_time_t)) {
        // 过期了, key不存在
        // 过期了也不删除, ttl这里设计为只读, 删除在之后进行
        // -2 表示 key 不存在
        return ":-2\r\n";
      } else {
        // 没有过期
        auto now = std::chrono::system_clock::now();
        return ":" +
               std::to_string(std::stoll(expire_query.value()) - now_time_t) +
               "\r\n";
      }
    } else {
      // 没有设置过期时间, 返回 -1
      return ":-1\r\n";
    }
  } else {
    // key 不存在
    return ":-1\r\n";
  }
}

// 哈希操作
std::string RedisWrapper::redis_hset(const std::string &key,
                                     const std::string &field,
                                     const std::string &value) {
  std::shared_lock<std::shared_mutex> rlock(redis_mtx); // 读锁线判断是否过期
  bool is_expired = expire_hash_clean(key, rlock);

  // 关闭读锁打开写锁
  if (!is_expired) {
    // 如果没有过期的话, expire_hash_clean 不会升级读锁, 这里需要手动解锁
    rlock.unlock();
  }
  std::unique_lock<std::shared_mutex> lock(redis_mtx); // 写锁

  // 更新字段值
  std::string field_key = get_hash_filed_key(key, field);
  lsm->put(field_key, value);

  // 更新字段列表
  auto field_list_opt = lsm->get(key);
  std::vector<std::string> field_list =
      get_fileds_from_hash_value(field_list_opt);

  if (std::find(field_list.begin(), field_list.end(), field) ==
      field_list.end()) {
    // 不存在则添加
    field_list.push_back(field);
    auto new_value = get_hash_value_from_fields(field_list);
    lsm->put(key, new_value);
  }

  return "+OK\r\n";
}

std::string RedisWrapper::redis_hget(const std::string &key,
                                     const std::string &field) {
  std::shared_lock<std::shared_mutex> rlock(redis_mtx); // 读锁
  bool is_expired = expire_hash_clean(key, rlock);

  if (is_expired) {
    return "$-1\r\n";
  }

  std::string field_key = get_hash_filed_key(key, field);
  auto value_opt = lsm->get(field_key);

  if (value_opt.has_value()) {
    return "$" + std::to_string(value_opt->size()) + "\r\n" +
           value_opt.value() + "\r\n";
  } else {
    return "$-1\r\n"; // 表示字段不存在
  }
}

std::string RedisWrapper::redis_hdel(const std::string &key,
                                     const std::string &field) {
  std::shared_lock<std::shared_mutex> rlock(redis_mtx); // 读锁线判断是否过期
  bool is_expired = expire_hash_clean(key, rlock);

  if (is_expired) {
    return ":0\r\n";
  }

  // 没有过期的话, expire_hash_clean 不会升级读锁, 这里需要手动解锁
  rlock.unlock();
  std::unique_lock<std::shared_mutex> lock(
      redis_mtx); // 写锁, 如果过期的话, 读锁在 expire_hash_clean 中已经被释放了

  int del_count = 0;
  // 删除字段值
  std::string field_key = get_hash_filed_key(key, field);
  if (this->lsm->get(field_key).has_value()) {
    del_count++;
    this->lsm->remove(field_key);
  }

  // 更新字段列表
  auto field_list_opt = lsm->get(key);
  auto field_list = get_fileds_from_hash_value(field_list_opt);
  auto find_res = std::find(field_list.begin(), field_list.end(), field);
  if (find_res != field_list.end()) {
    // 存在则删除
    field_list.erase(find_res);
    if (field_list.empty()) {
      // 如果字段列表为空, 则删除 key
      lsm->remove(key);
    } else {
      // 否则更新字段列表
      auto new_value = get_hash_value_from_fields(field_list);
      lsm->put(key, new_value);
    }
  }

  return ":" + std::to_string(del_count) + "\r\n";
}

std::string RedisWrapper::redis_hkeys(const std::string &key) {
  std::shared_lock<std::shared_mutex> rlock(redis_mtx); // 读锁
  bool is_expired = expire_hash_clean(key, rlock);

  if (is_expired) {
    return "*0\r\n";
  }

  auto field_list_opt = lsm->get(key);
  std::vector<std::string> fields;
  auto res_vec = get_fileds_from_hash_value(field_list_opt);

  std::string res_str = "*";
  res_str += std::to_string(res_vec.size()) + "\r\n";
  for (const auto &field : res_vec) {
    res_str += "$" + std::to_string(field.size()) + "\r\n" + field + "\r\n";
  }

  return res_str;
}

// 链表操作
std::string RedisWrapper::redis_lpush(const std::string &key,
                                      const std::string &value) {
  std::shared_lock<std::shared_mutex> rlock(redis_mtx); // 读锁
  bool is_expire = expire_list_clean(key, rlock);

  if (!is_expire) {
    // 如果过期了, 会执行清理操作, expire_list_clean 会升级读锁
    // 没有过期的话, 读锁仍然存在, 需要手动释放
    rlock.unlock();
  }
  std::unique_lock<std::shared_mutex> lock(redis_mtx); // 写锁

  auto list_opt = lsm->get(key);
  std::string list_value = list_opt.value_or("");
  if (!list_value.empty()) {
    list_value = value + REDIS_LIST_SEPARATOR + list_value;
  } else {
    list_value = value;
  }

  lsm->put(key, list_value);
  return ":" + std::to_string(split(list_value, REDIS_LIST_SEPARATOR).size()) +
         "\r\n";
}

std::string RedisWrapper::redis_rpush(const std::string &key,
                                      const std::string &value) {
  std::shared_lock<std::shared_mutex> rlock(redis_mtx); // 读锁
  bool is_expire = expire_list_clean(key, rlock);

  if (!is_expire) {
    // 如果过期了, 会执行清理操作, expire_list_clean 会升级读锁
    // 没有过期的话, 读锁仍然存在, 需要手动释放
    rlock.unlock();
  }

  std::unique_lock<std::shared_mutex> lock(redis_mtx); // 写锁

  auto list_opt = lsm->get(key);
  std::string list_value = list_opt.value_or("");
  if (!list_value.empty()) {
    list_value = list_value + REDIS_LIST_SEPARATOR + value;
  } else {
    list_value = value;
  }

  lsm->put(key, list_value);
  return ":" + std::to_string(split(list_value, REDIS_LIST_SEPARATOR).size()) +
         "\r\n";
}

std::string RedisWrapper::redis_lpop(const std::string &key) {
  std::shared_lock<std::shared_mutex> rlock(redis_mtx); // 读锁
  bool is_expire = expire_list_clean(key, rlock);

  if (is_expire) {
    return "$-1\r\n";
  }

  // 没过期的情况, 需要手动释放读锁
  rlock.unlock();                                      // 升级锁
  std::unique_lock<std::shared_mutex> lock(redis_mtx); // 写锁

  auto list_opt = lsm->get(key);
  if (!list_opt.has_value()) {
    return "$-1\r\n"; // 表示链表不存在
  }

  std::vector<std::string> elements =
      split(list_opt.value(), REDIS_LIST_SEPARATOR);
  if (elements.empty()) {
    return "$-1\r\n"; // 表示链表为空
  }

  std::string value = elements.front();
  elements.erase(elements.begin());

  if (elements.empty()) {
    lsm->remove(key);
  } else {
    lsm->put(key, join(elements, REDIS_LIST_SEPARATOR));
  }
  return "$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
}

std::string RedisWrapper::redis_rpop(const std::string &key) {
  std::shared_lock<std::shared_mutex> rlock(redis_mtx); // 读锁
  bool is_expire = expire_list_clean(key, rlock);

  if (is_expire) {
    return "$-1\r\n";
  }

  // 没过期的情况, 需要手动释放读锁
  rlock.unlock();                                      // 升级锁
  std::unique_lock<std::shared_mutex> lock(redis_mtx); // 写锁

  auto list_opt = lsm->get(key);
  if (!list_opt.has_value()) {
    return "$-1\r\n"; // 表示链表不存在
  }

  std::vector<std::string> elements =
      split(list_opt.value(), REDIS_LIST_SEPARATOR);
  if (elements.empty()) {
    return "$-1\r\n"; // 表示链表为空
  }

  std::string value = elements.back();
  elements.pop_back();

  if (elements.empty()) {
    lsm->remove(key);
  } else {
    lsm->put(key, join(elements, REDIS_LIST_SEPARATOR));
  }
  return "$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
}

std::string RedisWrapper::redis_llen(const std::string &key) {
  std::shared_lock<std::shared_mutex> rlock(redis_mtx); // 读锁
  bool is_expired = expire_list_clean(key, rlock);

  if (is_expired) {
    return ":0\r\n";
  }

  auto list_opt = lsm->get(key);
  if (!list_opt.has_value()) {
    return ":0\r\n"; // 表示链表不存在
  }

  std::vector<std::string> elements =
      split(list_opt.value(), REDIS_LIST_SEPARATOR);
  return ":" + std::to_string(elements.size()) + "\r\n";
}

std::string RedisWrapper::redis_lrange(const std::string &key, int start,
                                       int stop) {
  std::shared_lock<std::shared_mutex> rlock(redis_mtx); // 读锁
  bool is_expire = expire_list_clean(key, rlock);

  if (is_expire) {
    return "*0\r\n";
  }

  auto list_opt = lsm->get(key);
  if (!list_opt.has_value()) {
    return "*0\r\n"; // 表示链表不存在
  }

  std::vector<std::string> elements =
      split(list_opt.value(), REDIS_LIST_SEPARATOR);
  if (elements.empty()) {
    return "*0\r\n"; // 表示链表为空
  }

  if (start < 0)
    start += elements.size();
  if (stop < 0)
    stop += elements.size();
  if (start < 0)
    start = 0;
  if (stop >= elements.size())
    stop = elements.size() - 1;
  if (start > stop)
    return "*0\r\n";

  std::ostringstream oss;
  oss << "*" << (stop - start + 1) << "\r\n";
  for (int i = start; i <= stop; ++i) {
    oss << "$" << elements[i].size() << "\r\n" << elements[i] << "\r\n";
  }
  return oss.str();
}

std::string RedisWrapper::redis_zadd(std::vector<std::string> &args) {
  std::string key = args[1];
  std::shared_lock<std::shared_mutex> rlock(redis_mtx); // 读锁
  bool is_expired = expire_zset_clean(key, rlock);

  if (!is_expired) {
    // 如果过期了, 会执行清理操作, expire_hash_clean 会升级读锁
    // 这次操作还会继续, 因为相当于新建
    // 没有过期过期, 需要手动释放读锁
    rlock.unlock();
  }
  std::unique_lock<std::shared_mutex> lock(redis_mtx); // 写锁

  std::vector<std::pair<std::string, std::string>> put_kvs;
  std::vector<std::string> del_keys;

  auto value = get_zset_key_preffix(key); // 直接将 前缀 作为 value
  if (!lsm->get(value).has_value()) {
    // 如果不存在, 需要新建
    put_kvs.emplace_back(key, value);
  }

  std::vector<std::string> remove_keys;
  int added_count = 0;
  for (size_t i = 2; i < args.size(); i += 2) {
    std::string score = args[i];
    std::string elem = args[i + 1];
    std::string key_score = get_zset_key_socre(key, score);
    std::string key_elem = get_zset_key_elem(key, elem);

    auto query_elem = lsm->get(key_elem);

    if (query_elem.has_value()) {
      // 将以前的旧记录删除
      std::string original_score = query_elem.value();
      if (original_score == score) {
        // 不需要更新score
        continue;
      }
      // 需要移除旧 score
      std::string original_key_score = get_zset_key_socre(key, original_score);
      remove_keys.push_back(original_key_score);
    }
    put_kvs.emplace_back(key_score, elem);
    put_kvs.emplace_back(key_elem, score);
    added_count++;
  }
  lsm->remove_batch(del_keys);
  lsm->put_batch(put_kvs);

  return ":" + std::to_string(added_count) + "\r\n";
}

std::string RedisWrapper::redis_zrem(std::vector<std::string> &args) {
  if (args.size() < 3) {
    return "-ERR wrong number of arguments for 'zrem' command\r\n";
  }

  std::string key = args[1];
  std::shared_lock<std::shared_mutex> rlock(redis_mtx); // 读锁
  bool is_expired = expire_zset_clean(key, rlock);

  if (is_expired) {
    return ":0\r\n";
  }

  rlock.unlock();
  std::unique_lock<std::shared_mutex> lock(redis_mtx); // 写锁

  int removed_count = 0;
  for (size_t i = 2; i < args.size(); ++i) {
    std::string elem = args[i];
    std::string key_elem = get_zset_key_elem(key, elem);

    auto query_elem = lsm->get(key_elem);
    if (query_elem.has_value()) {
      std::string score = query_elem.value();
      std::string key_score = get_zset_key_socre(key, score);
      lsm->remove(key_elem);
      lsm->remove(key_score);
      removed_count++;
    }
  }

  return ":" + std::to_string(removed_count) + "\r\n";
}

std::string RedisWrapper::redis_zrange(std::vector<std::string> &args) {
  std::string key = args[1];
  int start = std::stoi(args[2]);
  int stop = std::stoi(args[3]);

  std::shared_lock<std::shared_mutex> rlock(redis_mtx); // 读锁
  bool is_expired = expire_zset_clean(key, rlock);

  if (is_expired) {
    return "*0\r\n";
  }

  // 范围查询: 按照 score 查询就能满足 zrange 的顺序
  std::string preffix_score = get_zset_score_preffix(key);
  auto result_elem = this->lsm->lsm_iters_monotony_predicate(
      [&preffix_score](const std::string &elem) {
        return -elem.compare(0, preffix_score.size(), preffix_score);
      });

  if (!result_elem.has_value()) {
    return "*0\r\n";
  }

  auto [elem_begin, elem_end] = result_elem.value();
  std::vector<std::pair<std::string, std::string>> elements;
  for (; elem_begin != elem_end; ++elem_begin) {
    std::string key_score = elem_begin->first;
    std::string elem = elem_begin->second;
    std::string score = get_zset_score_item(key_score);
    elements.emplace_back(score, elem);
  }

  if (start < 0)
    start += elements.size();
  if (stop < 0)
    stop += elements.size();
  if (start < 0)
    start = 0;
  if (stop >= elements.size())
    stop = elements.size() - 1;
  if (start > stop)
    return "*0\r\n";

  std::ostringstream oss;
  oss << "*" << (stop - start + 1) << "\r\n";
  for (int i = start; i <= stop; ++i) {
    oss << "$" << elements[i].second.size() << "\r\n"
        << elements[i].second << "\r\n";
  }
  return oss.str();
}

std::string RedisWrapper::redis_zcard(const std::string &key) {
  std::shared_lock<std::shared_mutex> rlock(redis_mtx); // 读锁
  bool is_expired = expire_zset_clean(key, rlock);

  if (is_expired) {
    return ":0\r\n";
  }

  // key_score 和 key_elem 是一对, 所以只需要一个即可
  std::string preffix = get_zset_score_preffix(key);
  auto result_elem = this->lsm->lsm_iters_monotony_predicate(
      [&preffix](const std::string &elem) {
        return -elem.compare(0, preffix.size(), preffix);
      });

  if (!result_elem.has_value()) {
    return ":0\r\n";
  }

  auto [elem_begin, elem_end] = result_elem.value();
  int count = 0;
  while (elem_begin != elem_end) {
    count++;
    ++elem_begin;
  }

  return ":" + std::to_string(count) + "\r\n";
}

std::string RedisWrapper::redis_zscore(const std::string &key,
                                       const std::string &elem) {
  std::shared_lock<std::shared_mutex> rlock(redis_mtx); // 读锁
  bool is_expired = expire_zset_clean(key, rlock);

  if (is_expired) {
    return "$-1\r\n";
  }

  std::string key_elem = get_zset_key_elem(key, elem);
  auto query_elem = lsm->get(key_elem);

  if (query_elem.has_value()) {
    return "$" + std::to_string(query_elem.value().size()) + "\r\n" +
           query_elem.value() + "\r\n";
  } else {
    return "$-1\r\n"; // 表示成员不存在
  }
}

std::string RedisWrapper::redis_zincrby(const std::string &key,
                                        const std::string &increment,
                                        const std::string &elem) {
  std::shared_lock<std::shared_mutex> rlock(redis_mtx); // 读锁
  bool is_expired = expire_zset_clean(key, rlock);

  if (!is_expired) {
    rlock.unlock();
  }
  std::unique_lock<std::shared_mutex> lock(redis_mtx); // 写锁

  std::string key_elem = get_zset_key_elem(key, elem);
  auto query_elem = lsm->get(key_elem);

  uint64_t new_score;
  if (query_elem.has_value()) {
    std::string original_score = query_elem.value();
    new_score = std::stol(original_score) + std::stod(increment);
    std::string original_key_score = get_zset_key_socre(key, original_score);
    lsm->remove(original_key_score);
  } else {
    // 如果查询不到, 则相当于新建
    new_score = std::stod(increment);
  }

  std::string new_score_str = std::to_string(new_score);
  std::string key_score = get_zset_key_socre(key, new_score_str);

  lsm->put(key_elem, new_score_str);
  lsm->put(key_score, elem);

  return ":" + new_score_str + "\r\n";
}

std::string RedisWrapper::redis_zrank(const std::string &key,
                                      const std::string &elem) {
  std::shared_lock<std::shared_mutex> rlock(redis_mtx); // 读锁
  bool is_expired = expire_zset_clean(key, rlock);

  if (is_expired) {
    return "$-1\r\n";
  }

  // 获取元素对应的 score
  std::string key_elem = get_zset_key_elem(key, elem);
  auto query_elem = lsm->get(key_elem);

  if (!query_elem.has_value()) {
    return "$-1\r\n"; // 表示成员不存在
  }

  std::string score = query_elem.value();
  std::string key_score = get_zset_key_socre(key, score);

  // 获取有序集合的前缀
  std::string preffix_score = get_zset_key_preffix(key);
  auto result_elem = this->lsm->lsm_iters_monotony_predicate(
      [&preffix_score](const std::string &elem) {
        return -elem.compare(0, preffix_score.size(), preffix_score);
      });

  if (!result_elem.has_value()) {
    return "$-1\r\n";
  }

  auto [elem_begin, elem_end] = result_elem.value();
  int rank = 0;
  for (; elem_begin != elem_end; ++elem_begin) {
    if (elem_begin->first == key_score) {
      return ":" + std::to_string(rank) + "\r\n";
    }
    rank++;
  }

  return "$-1\r\n"; // 表示成员不存在
}
