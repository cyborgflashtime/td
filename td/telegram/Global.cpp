//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2019
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/telegram/Global.h"

#include "td/telegram/ConfigShared.h"
#include "td/telegram/net/ConnectionCreator.h"
#include "td/telegram/net/MtprotoHeader.h"
#include "td/telegram/net/NetQueryDispatcher.h"
#include "td/telegram/net/TempAuthKeyWatchdog.h"
#include "td/telegram/StateManager.h"
#include "td/telegram/TdDb.h"

#include "td/actor/PromiseFuture.h"

#include "td/utils/format.h"
#include "td/utils/logging.h"
#include "td/utils/port/Clocks.h"
#include "td/utils/tl_helpers.h"

#include <cmath>

namespace td {

Global::Global() = default;

Global::~Global() = default;

void Global::close_all(Promise<> on_finished) {
  td_db_->close_all(std::move(on_finished));
  state_manager_.clear();
  parameters_ = TdParameters();
}
void Global::close_and_destroy_all(Promise<> on_finished) {
  td_db_->close_and_destroy_all(std::move(on_finished));
  state_manager_.clear();
  parameters_ = TdParameters();
}

ActorId<ConnectionCreator> Global::connection_creator() const {
  return connection_creator_.get();
}
void Global::set_connection_creator(ActorOwn<ConnectionCreator> connection_creator) {
  connection_creator_ = std::move(connection_creator);
}

ActorId<TempAuthKeyWatchdog> Global::temp_auth_key_watchdog() const {
  return temp_auth_key_watchdog_.get();
}
void Global::set_temp_auth_key_watchdog(ActorOwn<TempAuthKeyWatchdog> actor) {
  temp_auth_key_watchdog_ = std::move(actor);
}

MtprotoHeader &Global::mtproto_header() {
  return *mtproto_header_;
}
void Global::set_mtproto_header(unique_ptr<MtprotoHeader> mtproto_header) {
  mtproto_header_ = std::move(mtproto_header);
}

Status Global::init(const TdParameters &parameters, ActorId<Td> td, unique_ptr<TdDb> td_db_ptr) {
  parameters_ = parameters;

  gc_scheduler_id_ = min(Scheduler::instance()->sched_id() + 2, Scheduler::instance()->sched_count() - 1);
  slow_net_scheduler_id_ = min(Scheduler::instance()->sched_id() + 3, Scheduler::instance()->sched_count() - 1);

  td_ = td;
  td_db_ = std::move(td_db_ptr);

  string save_diff_str = td_db()->get_binlog_pmc()->get("server_time_difference");
  string save_system_time_str = td_db()->get_binlog_pmc()->get("system_time");
  auto system_time = Clocks::system();
  auto default_time_difference = system_time - Time::now();
  if (save_diff_str.empty()) {
    server_time_difference_ = default_time_difference;
    server_time_difference_was_updated_ = false;
  } else {
    double save_diff;
    unserialize(save_diff, save_diff_str).ensure();

    double save_system_time;
    if (save_system_time_str.empty()) {
      save_system_time = 0;
    } else {
      unserialize(save_system_time, save_system_time_str).ensure();
    }

    double diff = save_diff + default_time_difference;
    if (save_system_time > system_time) {
      double time_backwards_fix = save_system_time - system_time;
      LOG(WARNING) << "Fix system time which went backwards: " << format::as_time(time_backwards_fix) << " "
                   << tag("saved_system_time", save_system_time) << tag("system_time", system_time);
      diff += time_backwards_fix;
    }
    LOG(DEBUG) << "LOAD: " << tag("server_time_difference", diff);
    server_time_difference_ = diff;
    server_time_difference_was_updated_ = false;
  }
  dns_time_difference_ = default_time_difference;
  dns_time_difference_was_updated_ = false;

  return Status::OK();
}

void Global::update_server_time_difference(double diff) {
  if (!server_time_difference_was_updated_ || server_time_difference_ < diff) {
    server_time_difference_ = diff;
    server_time_difference_was_updated_ = true;

    // diff = server_time - Time::now
    // save_diff = server_time - Clocks::system
    double save_diff = diff + Time::now() - Clocks::system();

    td_db()->get_binlog_pmc()->set("server_time_difference", serialize(save_diff));
    save_system_time();
  }
}

void Global::save_system_time() {
  auto t = Time::now();
  if (system_time_saved_at_.load(std::memory_order_relaxed) + 10 < t) {
    system_time_saved_at_ = t;
    double save_system_time = Clocks::system();
    LOG(INFO) << "Save system time";
    td_db()->get_binlog_pmc()->set("system_time", serialize(save_system_time));
  }
}

void Global::update_dns_time_difference(double diff) {
  dns_time_difference_ = diff;
  dns_time_difference_was_updated_ = true;
}

double Global::get_dns_time_difference() const {
  // rely that was updated flag is monotonic. Currenly it is true. If it stops being monitonic at some point it won't
  // lead to problems anyway.
  bool dns_flag = dns_time_difference_was_updated_;
  double dns_diff = dns_time_difference_;
  bool server_flag = server_time_difference_was_updated_;
  double server_diff = server_time_difference_;
  if (dns_flag != server_flag) {
    return dns_flag ? dns_diff : server_diff;
  }
  if (dns_flag) {
    return max(dns_diff, server_diff);
  }
  if (td_db_) {
    return server_diff;
  }
  return Clocks::system() - Time::now();
}

DcId Global::get_webfile_dc_id() const {
  CHECK(shared_config_ != nullptr);
  int32 dc_id = shared_config_->get_option_integer("webfile_dc_id");
  if (!DcId::is_valid(dc_id)) {
    if (is_test_dc()) {
      dc_id = 2;
    } else {
      dc_id = 4;
    }

    CHECK(DcId::is_valid(dc_id));
  }

  return DcId::internal(dc_id);
}

bool Global::ignore_backgrond_updates() const {
  return !parameters_.use_file_db && !parameters_.use_secret_chats &&
         shared_config_->get_option_boolean("ignore_background_updates");
}

void Global::set_net_query_dispatcher(unique_ptr<NetQueryDispatcher> net_query_dispatcher) {
  net_query_dispatcher_ = std::move(net_query_dispatcher);
}

void Global::set_shared_config(unique_ptr<ConfigShared> shared_config) {
  shared_config_ = std::move(shared_config);
}

int64 Global::get_location_key(double latitude, double longitude) {
  const double PI = 3.14159265358979323846;
  latitude *= PI / 180;
  longitude *= PI / 180;

  int64 key = 0;
  if (latitude < 0) {
    latitude = -latitude;
    key = 65536;
  }

  double f = std::tan(PI / 4 - latitude / 2);
  key += static_cast<int64>(f * std::cos(longitude) * 128) * 256;
  key += static_cast<int64>(f * std::sin(longitude) * 128);
  return key;
}

int64 Global::get_location_access_hash(double latitude, double longitude) {
  auto it = location_access_hashes_.find(get_location_key(latitude, longitude));
  if (it == location_access_hashes_.end()) {
    return 0;
  }
  return it->second;
}

void Global::add_location_access_hash(double latitude, double longitude, int64 access_hash) {
  if (access_hash == 0) {
    return;
  }

  location_access_hashes_[get_location_key(latitude, longitude)] = access_hash;
}

}  // namespace td
